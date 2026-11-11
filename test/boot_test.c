#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/boot/boot.h"

/*
 * Bootloader 逻辑测试。
 *
 * 全部在内存模拟的 Flash 上跑，不需要硬件。覆盖：
 *   1. CRC32 正确性（用标准校验值钉死）
 *   2. 分区表序列化往返与损坏检测
 *   3. 跳转前的三项检查
 *   4. 双区升级的完整流程与回滚前提
 */

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *what)
{
    if (cond)
    {
        printf("  [PASS] %s\n", what);
        passed++;
    }
    else
    {
        printf("  [FAIL] %s\n", what);
        failed++;
    }
}

/* 256KB 的结构体放栈上太险，用静态区 */
static boot_ram_flash_t g_ram;

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE (64u * 1024u)
#define SLOT_A_OFF 0x8000u
#define SLOT_A_LEN 0x4000u
#define SLOT_B_OFF 0xC000u
#define SLOT_B_LEN 0x4000u

#define SRAM_BASE 0x20000000u
#define SRAM_SIZE (128u * 1024u)

static void setup(boot_flash_t *f, boot_table_t *t)
{
    boot_ram_init(&g_ram, FLASH_SIZE);

    f->ops = boot_ram_ops();
    f->ctx = &g_ram;

    memset(t, 0, sizeof(*t));
    t->active = BOOT_SLOT_A;
    t->parts[BOOT_SLOT_A].offset = SLOT_A_OFF;
    t->parts[BOOT_SLOT_A].size = SLOT_A_LEN;
    t->parts[BOOT_SLOT_A].fw_version = 1;
    t->parts[BOOT_SLOT_A].valid = 1;
    t->parts[BOOT_SLOT_B].offset = SLOT_B_OFF;
    t->parts[BOOT_SLOT_B].size = SLOT_B_LEN;
}

/* ---------------- CRC32 ---------------- */

static void test_crc32(void)
{
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t c;

    printf("CRC32\n");

    c = boot_crc32(s, sizeof(s));
    printf("  crc32(\"123456789\") = 0x%08X\n", c);
    check(c == 0xCBF43926u, "standard check value 0xCBF43926");
    check(boot_crc32(s, 0) == 0, "empty input yields 0");

    /* 增量计算必须与一次性计算一致，否则升级过程中累积的 CRC 就会算错 */
    {
        uint32_t a = boot_crc32(s, 4);
        uint32_t b = boot_crc32_update(a, s + 4, 5);
        check(b == c, "incremental equals one-shot");
    }
}

/* ---------------- 分区表 ---------------- */

static void test_table(void)
{
    boot_flash_t f;
    boot_table_t t, back;
    uint8_t raw[BOOT_TABLE_SIZE];

    printf("Partition table\n");
    setup(&f, &t);

    check(boot_table_save(&f, &t) == BOOT_FLASH_OK, "save succeeds");
    check(boot_table_load(&f, &back) == BOOT_FLASH_OK, "load succeeds");
    check(back.active == t.active, "active round-trips");
    check(back.parts[0].offset == SLOT_A_OFF && back.parts[0].size == SLOT_A_LEN,
          "slot A geometry round-trips");
    check(back.parts[0].valid == 1, "valid flag round-trips");

    /* 篡改 magic */
    boot_flash_read(&f, BOOT_TABLE_OFFSET, raw, BOOT_TABLE_SIZE);
    raw[0] ^= 0xFF;
    boot_flash_erase(&f, BOOT_TABLE_OFFSET, BOOT_TABLE_SIZE);
    boot_flash_write(&f, BOOT_TABLE_OFFSET, raw, BOOT_TABLE_SIZE);
    check(boot_table_load(&f, &back) != BOOT_FLASH_OK, "bad magic rejected");

    /* 篡改内容但不改 CRC —— 应被表自身的 CRC 发现 */
    boot_flash_read(&f, BOOT_TABLE_OFFSET, raw, BOOT_TABLE_SIZE);
    raw[9] ^= 0x01; /* part0.offset 的最低字节 */
    boot_flash_erase(&f, BOOT_TABLE_OFFSET, BOOT_TABLE_SIZE);
    boot_flash_write(&f, BOOT_TABLE_OFFSET, raw, BOOT_TABLE_SIZE);
    check(boot_table_load(&f, &back) != BOOT_FLASH_OK, "corrupted payload detected by table crc");
}

/* ---------------- 跳转检查 ---------------- */

static void test_jump_check(void)
{
    boot_mem_map_t map;
    uint32_t vectors[2];

    printf("Jump conditions\n");

    map.sram_base = SRAM_BASE;
    map.sram_size = SRAM_SIZE;
    map.flash_base = FLASH_BASE;
    map.flash_size = FLASH_SIZE;

    /* 正常映像：栈顶在 SRAM 顶端，复位向量在 Flash 内且 bit0 = 1 */
    vectors[0] = SRAM_BASE + SRAM_SIZE;
    vectors[1] = FLASH_BASE + SLOT_A_OFF + 0x101;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_OK, "valid image passes");

    /* 栈顶落在 Flash 区域 */
    vectors[0] = FLASH_BASE + 0x100;
    vectors[1] = FLASH_BASE + SLOT_A_OFF + 0x101;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_BAD_STACK,
          "stack outside sram rejected");

    /* 栈顶刚好越过 SRAM 末尾（边界要能挡住） */
    vectors[0] = SRAM_BASE + SRAM_SIZE;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_OK,
          "stack at exact end still valid");

    vectors[0] = SRAM_BASE + SRAM_SIZE + 4;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_BAD_STACK,
          "stack one word past end rejected");

    /* 复位向量 bit0 = 0，不是 Thumb 地址 */
    vectors[0] = SRAM_BASE + 0x1000;
    vectors[1] = FLASH_BASE + SLOT_A_OFF + 0x100;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_BAD_ENTRY,
          "even reset vector rejected (not thumb)");

    /* 复位向量越界 */
    vectors[1] = FLASH_BASE + FLASH_SIZE + 0x101;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x1234) == BOOT_CHECK_BAD_ENTRY,
          "reset vector outside flash rejected");

    /* CRC 不符 */
    vectors[1] = FLASH_BASE + SLOT_A_OFF + 0x101;
    check(boot_check_vectors(vectors, &map, 0x1234, 0x5678) == BOOT_CHECK_BAD_CRC,
          "crc mismatch rejected");
}

/* ---------------- 分区选择 ---------------- */

static void test_select(void)
{
    boot_table_t t;
    boot_flash_t f;
    uint8_t slot;

    printf("Slot selection\n");
    setup(&f, &t);

    check(boot_select_slot(&t, &slot) == BOOT_FLASH_OK && slot == BOOT_SLOT_A,
          "active slot selected");

    /* 当前分区损坏，退到另一个 */
    t.parts[BOOT_SLOT_A].valid = 0;
    t.parts[BOOT_SLOT_B].valid = 1;
    check(boot_select_slot(&t, &slot) == BOOT_FLASH_OK && slot == BOOT_SLOT_B,
          "falls back to backup slot");

    /* 两个都不可用 */
    t.parts[BOOT_SLOT_B].valid = 0;
    check(boot_select_slot(&t, &slot) != BOOT_FLASH_OK, "no slot usable returns error");
}

/* ---------------- 升级流程 ---------------- */

static void test_update(void)
{
    boot_flash_t f;
    boot_table_t t, loaded;
    boot_update_t u;
    uint8_t fw[300];

    printf("Firmware update flow\n");
    setup(&f, &t);
    boot_table_save(&f, &t);

    for (int i = 0; i < 300; i++)
        fw[i] = (uint8_t)(i * 7);

    check(t.active == BOOT_SLOT_A, "initially running slot A");
    check(boot_update_begin(&u, &f, &t) == BOOT_FLASH_OK, "update begins");
    check(u.target == BOOT_SLOT_B, "target is the non-running slot");

    check(boot_update_write(&u, fw, 128) == BOOT_FLASH_OK, "first chunk written");
    check(boot_update_write(&u, fw + 128, 172) == BOOT_FLASH_OK, "second chunk written");
    check(u.written == 300, "byte counter tracks writes");

    check(boot_update_finish(&u, 2) == BOOT_FLASH_OK, "update finishes");
    check(t.active == BOOT_SLOT_B, "active slot switched to B");
    check(t.parts[BOOT_SLOT_B].valid == 1, "new slot marked valid");
    check(t.parts[BOOT_SLOT_B].fw_version == 2, "version recorded");
    check(t.parts[BOOT_SLOT_A].valid == 1, "old slot kept valid for rollback");

    /* 重新加载，确认表真的落盘了 */
    check(boot_table_load(&f, &loaded) == BOOT_FLASH_OK, "table reloads from flash");
    check(loaded.active == BOOT_SLOT_B, "reloaded active slot is B");

    /* 比对 Flash 内容 */
    {
        uint8_t readback[300];
        boot_flash_read(&f, SLOT_B_OFF, readback, 300);
        check(memcmp(readback, fw, 300) == 0, "firmware bytes match");
    }
}

/* 异常路径 */
static void test_update_errors(void)
{
    boot_flash_t f;
    boot_table_t t;
    boot_update_t u;
    uint8_t buf[16];

    printf("Update error paths\n");
    setup(&f, &t);

    memset(buf, 0xAA, sizeof(buf));

    /* 超出分区容量 */
    boot_update_begin(&u, &f, &t);
    check(boot_update_write(&u, buf, sizeof(buf)) == BOOT_FLASH_OK, "small write ok");
    check(boot_update_write(&u, buf, SLOT_B_LEN) == BOOT_ERR_RANGE, "oversized write rejected");
    boot_update_abort(&u);

    /* 没 begin 就写 */
    check(boot_update_write(&u, buf, sizeof(buf)) == BOOT_ERR_PARAM, "write without begin rejected");

    /* 没写数据就 finish */
    boot_update_begin(&u, &f, &t);
    check(boot_update_finish(&u, 3) == BOOT_ERR_PARAM, "finish without data rejected");

    /* 未擦除区域重复写入应报错，而不是静默出错 */
    {
        uint8_t a[4] = {0x00, 0x00, 0x00, 0x00};
        uint8_t b[4] = {0xFF, 0x00, 0x00, 0x00};

        boot_flash_erase(&f, 0x2000, 16);
        check(boot_flash_write(&f, 0x2000, a, 4) == BOOT_FLASH_OK, "write zeros ok");
        check(boot_flash_write(&f, 0x2000, b, 4) == BOOT_ERR_NOT_ERASED,
              "cannot set bits back without erase");
    }
}

int main(void)
{
    printf("=== Bootloader logic test ===\n\n");
    test_crc32();
    printf("\n");
    test_table();
    printf("\n");
    test_jump_check();
    printf("\n");
    test_select();
    printf("\n");
    test_update();
    printf("\n");
    test_update_errors();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
