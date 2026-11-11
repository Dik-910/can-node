#include "boot.h"

#include <string.h>

/* ---------------- CRC32 ---------------- */

/*
 * 位运算版 CRC-32，不占查找表空间。Bootloader 对体积敏感，能省几百字节就省。
 * 内部用反码运算（crc = ~crc 进出），这样多次 update 的结果与一次性计算一致。
 */
uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (data == 0 && len > 0)
        return crc;

    crc = ~crc;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }

    return ~crc;
}

uint32_t boot_crc32(const uint8_t *data, uint32_t len)
{
    return boot_crc32_update(0, data, len);
}

/* ---------------- 分区表序列化 ---------------- */

/*
 * 显式按字节序列化，不直接 memcpy 结构体。
 * 原因：boot_part_t 里有 uint32 和 uint8 混排，结构体会插入填充字节，
 * memcpy 的结果会随编译器变化；而分区表要跨版本、跨工具链读取，必须是固定布局。
 */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void serialize(const boot_table_t *t, uint8_t out[BOOT_TABLE_SIZE])
{
    uint32_t off = 0;

    put_u32(&out[off], BOOT_TABLE_MAGIC);
    off += 4;
    put_u32(&out[off], BOOT_TABLE_VERSION);
    off += 4;
    out[off++] = t->active;

    for (int i = 0; i < BOOT_SLOT_COUNT; i++)
    {
        put_u32(&out[off], t->parts[i].offset);
        off += 4;
        put_u32(&out[off], t->parts[i].size);
        off += 4;
        put_u32(&out[off], t->parts[i].crc32);
        off += 4;
        put_u32(&out[off], t->parts[i].fw_version);
        off += 4;
        out[off++] = t->parts[i].valid;
    }

    /* 最后 4 字节：前 43 字节的 CRC，用来发现分区表自身被写坏 */
    put_u32(&out[off], boot_crc32(out, off));
}

static int deserialize(const uint8_t *in, boot_table_t *t)
{
    uint32_t off = 0;
    uint32_t stored_crc;

    if (get_u32(&in[0]) != BOOT_TABLE_MAGIC)
        return BOOT_ERR_PARAM;
    if (get_u32(&in[4]) != BOOT_TABLE_VERSION)
        return BOOT_ERR_PARAM;

    stored_crc = get_u32(&in[BOOT_TABLE_SIZE - 4]);
    if (boot_crc32(in, BOOT_TABLE_SIZE - 4) != stored_crc)
        return BOOT_ERR_PARAM;

    off = 8;
    t->active = in[off++];

    for (int i = 0; i < BOOT_SLOT_COUNT; i++)
    {
        t->parts[i].offset = get_u32(&in[off]);
        off += 4;
        t->parts[i].size = get_u32(&in[off]);
        off += 4;
        t->parts[i].crc32 = get_u32(&in[off]);
        off += 4;
        t->parts[i].fw_version = get_u32(&in[off]);
        off += 4;
        t->parts[i].valid = in[off++];
    }

    if (t->active >= BOOT_SLOT_COUNT)
        return BOOT_ERR_PARAM;

    return BOOT_FLASH_OK;
}

int boot_table_load(boot_flash_t *f, boot_table_t *t)
{
    uint8_t buf[BOOT_TABLE_SIZE];

    if (f == 0 || t == 0)
        return BOOT_ERR_PARAM;
    if (boot_flash_read(f, BOOT_TABLE_OFFSET, buf, BOOT_TABLE_SIZE) != BOOT_FLASH_OK)
        return BOOT_ERR_IO;

    return deserialize(buf, t);
}

int boot_table_save(boot_flash_t *f, const boot_table_t *t)
{
    uint8_t buf[BOOT_TABLE_SIZE];

    if (f == 0 || t == 0)
        return BOOT_ERR_PARAM;

    serialize(t, buf);

    /*
     * 真实 Flash 要按扇区擦除，这里为了能在内存里精确模拟只擦表本身。
     * 移植到硬件时这一步应改为「擦除包含分区表的整个扇区」。
     */
    if (boot_flash_erase(f, BOOT_TABLE_OFFSET, BOOT_TABLE_SIZE) != BOOT_FLASH_OK)
        return BOOT_ERR_IO;
    if (boot_flash_write(f, BOOT_TABLE_OFFSET, buf, BOOT_TABLE_SIZE) != BOOT_FLASH_OK)
        return BOOT_ERR_IO;

    return BOOT_FLASH_OK;
}

/* ---------------- 分区选择 ---------------- */

int boot_select_slot(const boot_table_t *t, uint8_t *slot)
{
    uint8_t other;

    if (t == 0 || slot == 0)
        return BOOT_ERR_PARAM;

    if (t->active < BOOT_SLOT_COUNT && t->parts[t->active].valid)
    {
        *slot = t->active;
        return BOOT_FLASH_OK;
    }

    /* 当前分区不可用，退到另一个——这是双区存在的意义 */
    other = (uint8_t)(1 - t->active);
    if (other < BOOT_SLOT_COUNT && t->parts[other].valid)
    {
        *slot = other;
        return BOOT_FLASH_OK;
    }

    return BOOT_ERR_IO; /* 两个都坏了，只能停在 Bootloader 等诊断 */
}

/* ---------------- 跳转检查 ---------------- */

boot_check_t boot_check_vectors(const uint32_t *vectors, const boot_mem_map_t *map,
                                uint32_t image_crc, uint32_t expected_crc)
{
    uint32_t stack;
    uint32_t entry;

    if (vectors == 0 || map == 0)
        return BOOT_CHECK_BAD_ENTRY;

    stack = vectors[0];
    entry = vectors[1];

    /*
     * 初始 MSP 必须落在 SRAM 内，否则一进函数就 HardFault。
     *
     * 上界用闭区间：Cortex-M 的栈是满递减栈，第一压栈会先减 4 再写，
     * 所以 MSP 初始化为 SRAM 末尾（基址 + 容量）是**标准做法**，不是越界。
     * 早前写成半开区间会把这个正确的向量表误判为非法。
     */
    if (stack < map->sram_base || stack > map->sram_base + map->sram_size)
        return BOOT_CHECK_BAD_STACK;

    /*
     * Cortex-M 的复位向量：bit0 置 1 表示 Thumb 状态。
     * 取真实地址要先 & ~1。如果 bit0 是 0，说明这不是有效的 Cortex-M 向量，
     * 跳过去会直接锁死——而这个错误在 CRC 校验里是看不出来的。
     */
    if ((entry & 1u) == 0)
        return BOOT_CHECK_BAD_ENTRY;

    entry &= ~1u;
    if (entry < map->flash_base || entry >= map->flash_base + map->flash_size)
        return BOOT_CHECK_BAD_ENTRY;

    if (image_crc != expected_crc)
        return BOOT_CHECK_BAD_CRC;

    return BOOT_CHECK_OK;
}

/* ---------------- 升级流程 ---------------- */

int boot_update_begin(boot_update_t *u, boot_flash_t *f, boot_table_t *t)
{
    if (u == 0 || f == 0 || t == 0)
        return BOOT_ERR_PARAM;
    if (t->active >= BOOT_SLOT_COUNT)
        return BOOT_ERR_PARAM;

    memset(u, 0, sizeof(*u));
    u->flash = f;
    u->table = t;

    /*
     * 目标永远是「非运行分区」。
     * 写正在运行的那一块等于自杀——擦到一半掉电，板子就变砖了。
     */
    u->target = (uint8_t)(1 - t->active);

    if (t->parts[u->target].size == 0)
        return BOOT_ERR_PARAM;

    if (boot_flash_erase(f, t->parts[u->target].offset, t->parts[u->target].size) != BOOT_FLASH_OK)
        return BOOT_ERR_IO;

    u->state = BOOT_UPDATE_RECEIVING;
    u->written = 0;
    u->crc = 0;
    return BOOT_FLASH_OK;
}

int boot_update_write(boot_update_t *u, const uint8_t *data, uint32_t len)
{
    uint32_t max;
    int rc;

    if (u == 0 || data == 0 || len == 0)
        return BOOT_ERR_PARAM;
    if (u->state != BOOT_UPDATE_RECEIVING)
        return BOOT_ERR_PARAM;

    max = u->table->parts[u->target].size;
    if (u->written + len > max)
        return BOOT_ERR_RANGE;

    rc = boot_flash_write(u->flash, u->table->parts[u->target].offset + u->written, data, len);
    if (rc != BOOT_FLASH_OK)
        return rc;

    u->crc = boot_crc32_update(u->crc, data, len);
    u->written += len;
    return BOOT_FLASH_OK;
}

int boot_update_finish(boot_update_t *u, uint32_t fw_version)
{
    uint8_t buf[64];
    uint32_t remain;
    uint32_t off = 0;
    uint32_t verify_crc = 0;

    if (u == 0 || u->state != BOOT_UPDATE_RECEIVING)
        return BOOT_ERR_PARAM;
    if (u->written == 0)
        return BOOT_ERR_PARAM;

    /* 从 Flash 重读一遍再算 CRC：写入可能静默失败，不能只信写入时的累积值 */
    remain = u->written;
    while (remain > 0)
    {
        uint32_t chunk = (remain > sizeof(buf)) ? sizeof(buf) : remain;

        if (boot_flash_read(u->flash, u->table->parts[u->target].offset + off, buf, chunk) != BOOT_FLASH_OK)
            return BOOT_ERR_IO;

        verify_crc = boot_crc32_update(verify_crc, buf, chunk);
        off += chunk;
        remain -= chunk;
    }

    if (verify_crc != u->crc)
    {
        /* 读回来的和写进去的不一致，整包作废，不要把坏分区标记为有效 */
        boot_update_abort(u);
        return BOOT_ERR_IO;
    }

    u->table->parts[u->target].crc32 = verify_crc;
    u->table->parts[u->target].fw_version = fw_version;
    u->table->parts[u->target].valid = 1;

    /* 切换启动分区；旧分区保持 valid，留着回滚 */
    u->table->active = u->target;

    u->state = BOOT_UPDATE_IDLE;

    return boot_table_save(u->flash, u->table);
}

void boot_update_abort(boot_update_t *u)
{
    if (u == 0)
        return;

    u->state = BOOT_UPDATE_IDLE;
    u->written = 0;
    u->crc = 0;
}
