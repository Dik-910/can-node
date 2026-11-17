/*
 * nvm_store_test.c —— 双页参数存储
 *
 * 重点不是「存进去能读出来」，而是掉电与损坏时的行为：
 *   - 擦除后掉电
 *   - 页头写了一半掉电
 *   - 负载写完但校验失败（静默位错）
 *   - 两页都坏
 *
 * 这四种情况下设备必须还能给出「上一份好数据」，或者明确地什么都没有，
 * 不能给出半份数据 —— 参数被截断比参数丢失更难查。
 */
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "mem_nvm.h"
#include "nvm_store.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what, long got, long want)
{
    g_total++;
    if (cond)
    {
        printf("  [PASS] %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  [FAIL] %s (got %ld, want %ld)\n", what, got, want);
    }
}

#define AREA_SIZE 2048u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void fill(uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
}

/* ---- 基本存取 ---- */

static void test_blank_and_roundtrip(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t data[64], out[64];
    const uint8_t *got;
    uint16_t len = 0;

    printf("空白设备与基本往返\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 0, "空白设备不应有有效数据", st.valid, 0);
    check(nvm_store_data(&st, &len) == 0, "无数据时返回 NULL", 0, 0);

    fill(data, sizeof(data), 0x31);
    check(nvm_store_save(&st, data, sizeof(data)) == 0, "首次保存应成功", 0, 0);

    got = nvm_store_data(&st, &len);
    check(got != 0 && len == sizeof(data), "保存后长度正确", len, sizeof(data));
    check(got != 0 && memcmp(got, data, len) == 0, "保存后内容一致", 0, 0);
    check(st.active == 0, "首份数据写在第 0 页", st.active, 0);

    /* 重启 */
    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));
    got = nvm_store_data(&st, &len);

    check(st.valid == 1, "重启后应有数据", st.valid, 1);
    check(len == sizeof(data), "重启后长度一致", len, sizeof(data));
    check(got != 0 && memcmp(got, data, len) == 0, "重启后内容一致", 0, 0);

    (void)out;
}

/* ---- 交替写 ---- */

static void test_alternating_pages(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t a[32], b[32], c[32];
    uint16_t len = 0;

    printf("两页交替写入\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(a, sizeof(a), 0x01);
    fill(b, sizeof(b), 0x40);
    fill(c, sizeof(c), 0x80);

    nvm_store_save(&st, a, sizeof(a));
    check(st.active == 0, "第 1 份在第 0 页", st.active, 0);

    nvm_store_save(&st, b, sizeof(b));
    check(st.active == 1, "第 2 份在第 1 页", st.active, 1);

    nvm_store_save(&st, c, sizeof(c));
    check(st.active == 0, "第 3 份回到第 0 页", st.active, 0);

    check(st.saves == 3, "累计写入 3 次", st.saves, 3);

    /* 重启后应拿到最后一份 */
    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(nvm_store_data(&st, &len) != 0, "重启后仍有数据", 1, 1);
    check(len == sizeof(c) && memcmp(st.buf, c, len) == 0, "拿到的是最后一份", 0, 0);
}

/* ---- 掉电：擦除后就没再写 ---- */

static void test_power_loss_after_erase(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t old[64], newd[64];
    uint16_t len = 0;

    printf("擦除后掉电\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(old, sizeof(old), 0x11);
    nvm_store_save(&st, old, sizeof(old));

    /*
     * 第二次保存时只给擦除的预算：擦完就没电了，页头一个字节都没写。
     * 这是最坏的一种掉电位置。
     */
    mem.budget = (int)(AREA_SIZE / 2);
    fill(newd, sizeof(newd), 0x99);
    check(nvm_store_save(&st, newd, sizeof(newd)) != 0, "半途掉电应报失败", 1, 0);

    mem.budget = -1;

    /* 掉电重启 */
    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 1, "旧页仍应有效", st.valid, 1);
    check(nvm_store_data(&st, &len) != 0 && len == sizeof(old), "旧数据长度不变", len, sizeof(old));
    check(len == sizeof(old) && memcmp(st.buf, old, len) == 0, "读到的是旧数据", 0, 0);
}

/* ---- 掉电：页头写了一半 ---- */

static void test_power_loss_mid_header(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t old[64], newd[64];
    uint16_t len = 0;

    printf("页头写一半掉电\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(old, sizeof(old), 0x22);
    nvm_store_save(&st, old, sizeof(old));

    /* 擦除 + 页头写 8 字节就断电：magic 与 seq 落盘，len/crc 没有 */
    mem.budget = (int)(AREA_SIZE / 2) + 8;
    fill(newd, sizeof(newd), 0x33);
    nvm_store_save(&st, newd, sizeof(newd));
    mem.budget = -1;

    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 1, "旧页仍应有效", st.valid, 1);
    check(nvm_store_data(&st, &len) != 0 && memcmp(st.buf, old, len) == 0,
          "读到的是旧数据", 0, 0);
}

/* ---- 损坏：负载位翻转 ---- */

static void test_corrupt_payload_falls_back(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t a[32], b[32];
    uint16_t len = 0;
    uint32_t page1_base;

    printf("新页负载损坏时回退\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(a, sizeof(a), 0x55);
    nvm_store_save(&st, a, sizeof(a));

    fill(b, sizeof(b), 0xAA);
    nvm_store_save(&st, b, sizeof(b));
    check(st.active == 1, "第二份在第 1 页", st.active, 1);

    /* 把第 1 页的负载改坏一个字节 */
    page1_base = AREA_SIZE / 2;
    mem.area[page1_base + NVM_HEADER_SIZE + 3] ^= 0x01;

    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 1, "应回退到旧页", st.valid, 1);
    check(st.active == 0, "有效页是第 0 页", st.active, 0);
    check(nvm_store_data(&st, &len) != 0 && memcmp(st.buf, a, len) == 0,
          "读到的是第 0 页的数据", 0, 0);
}

/* ---- 两页都坏 ---- */

static void test_both_pages_bad(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t a[16], b[16];

    printf("两页都坏\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(a, sizeof(a), 0x01);
    nvm_store_save(&st, a, sizeof(a));
    fill(b, sizeof(b), 0x02);
    nvm_store_save(&st, b, sizeof(b));

    /* 两页的负载各改一个字节 */
    mem.area[NVM_HEADER_SIZE + 1] ^= 0xFF;
    mem.area[AREA_SIZE / 2 + NVM_HEADER_SIZE + 1] ^= 0xFF;

    memset(&st, 0, sizeof(st));
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 0, "两页都坏时应报告无数据", st.valid, 0);
    check(st.rejects >= 2, "两页都应被记为拒绝", st.rejects >= 2, 1);
}

/* ---- 序号回绕 ---- */

static void test_seq_wrap(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t hdr[NVM_HEADER_SIZE];
    uint8_t payload[8];

    printf("序号回绕\n");

    mem_nvm_init(&mem, AREA_SIZE);

    /*
     * 手工造两页：第 0 页 seq = 0xFFFFFFFF，第 1 页 seq = 0。
     * 按无符号比较，第 0 页更大；按回绕语义，第 1 页才是新写的。
     * 取错的话，回绕之后会一直读到回绕前那份数据。
     */
    memset(payload, 0x5A, sizeof(payload));

    memset(hdr, 0, sizeof(hdr));
    put32(&hdr[0], NVM_PAGE_MAGIC);
    put32(&hdr[4], 0xFFFFFFFFu);
    hdr[8] = (uint8_t)sizeof(payload);
    hdr[9] = 0;
    put32(&hdr[10], 0); /* CRC 稍后填 */
    memcpy(&mem.area[0], hdr, sizeof(hdr));
    memcpy(&mem.area[NVM_HEADER_SIZE], payload, sizeof(payload));

    memset(hdr, 0, sizeof(hdr));
    put32(&hdr[0], NVM_PAGE_MAGIC);
    put32(&hdr[4], 0);
    hdr[8] = (uint8_t)sizeof(payload);
    hdr[9] = 0;
    memcpy(&mem.area[AREA_SIZE / 2], hdr, sizeof(hdr));
    memcpy(&mem.area[AREA_SIZE / 2 + NVM_HEADER_SIZE], payload, sizeof(payload));

    /* 两页的 CRC 都是对同一份 payload 算的，这里直接补上正确值 */
    {
        uint32_t c = crc32(payload, sizeof(payload));

        put32(&mem.area[10], c);
        put32(&mem.area[AREA_SIZE / 2 + 10], c);
    }

    nvm_store_mount(&st, mem_nvm_dev(&mem));

    check(st.valid == 1, "应有一个有效页", st.valid, 1);
    check(st.active == 1, "回绕后应选 seq=0 的页", st.active, 1);
    check(st.seq == 0, "序号为 0", (long)st.seq, 0);
}

/* ---- 容量与参数校验 ---- */

static void test_limits(void)
{
    mem_nvm_t mem;
    nvm_store_t st;
    uint8_t big[NVM_PAYLOAD_MAX + 1];
    uint8_t small[8];

    printf("容量与参数校验\n");

    mem_nvm_init(&mem, AREA_SIZE);
    nvm_store_mount(&st, mem_nvm_dev(&mem));

    fill(big, sizeof(big), 0x00);
    check(nvm_store_save(&st, big, sizeof(big)) != 0, "超长负载应拒绝", 1, 0);

    fill(small, sizeof(small), 0x00);
    check(nvm_store_save(&st, small, sizeof(small)) == 0, "正常长度应通过", 0, 0);
    check(nvm_store_save(&st, 0, 0) == 0, "空负载应允许（清空语义）", 0, 0);

    check(nvm_store_save(0, small, sizeof(small)) != 0, "空指针应拒绝", 1, 0);
    check(nvm_store_mount(0, mem_nvm_dev(&mem)) != 0, "空 store 应拒绝", 1, 0);
}

int main(void)
{
    printf("=== nvm_store_test ===\n");

    test_blank_and_roundtrip();
    test_alternating_pages();
    test_power_loss_after_erase();
    test_power_loss_mid_header();
    test_corrupt_payload_falls_back();
    test_both_pages_bad();
    test_seq_wrap();
    test_limits();

    printf("\nResult: %d passed, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
