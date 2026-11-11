#include "boot_flash.h"

#include <string.h>

/*
 * PC 侧 Flash 模拟。
 *
 * 刻意模拟真实 NOR Flash 的两条物理特性，否则测不出 Bootloader 的经典错误：
 *   1. 写入是 AND 运算，只能把 1 变 0
 *   2. 想在没有擦除的区域把 0 变回 1 会失败，而不是静默忽略
 */

static int check_range(const boot_ram_flash_t *ram, uint32_t offset, uint32_t len)
{
    if (ram == 0 || len == 0)
        return BOOT_ERR_PARAM;
    if (offset > ram->size || len > ram->size - offset)
        return BOOT_ERR_RANGE;
    return BOOT_FLASH_OK;
}

static int ram_erase(void *ctx, uint32_t offset, uint32_t size)
{
    boot_ram_flash_t *ram = (boot_ram_flash_t *)ctx;
    int rc = check_range(ram, offset, size);

    if (rc != BOOT_FLASH_OK)
        return rc;

    memset(&ram->mem[offset], 0xFF, size);
    return BOOT_FLASH_OK;
}

static int ram_write(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
    boot_ram_flash_t *ram = (boot_ram_flash_t *)ctx;
    int rc = check_range(ram, offset, len);

    if (rc != BOOT_FLASH_OK)
        return rc;
    if (data == 0)
        return BOOT_ERR_PARAM;

    /*
     * 先整体检查，再统一写入。
     * 边写边检查会留下「写了一半才发现失败」的残局，比直接拒绝更难排查。
     */
    for (uint32_t i = 0; i < len; i++)
    {
        /* 目标位是 0 而数据要求置 1 —— 硬件做不到，必须先擦除 */
        if ((data[i] & (uint8_t)~ram->mem[offset + i]) != 0)
            return BOOT_ERR_NOT_ERASED;
    }

    for (uint32_t i = 0; i < len; i++)
        ram->mem[offset + i] &= data[i];

    return BOOT_FLASH_OK;
}

static int ram_read(void *ctx, uint32_t offset, uint8_t *data, uint32_t len)
{
    boot_ram_flash_t *ram = (boot_ram_flash_t *)ctx;
    int rc = check_range(ram, offset, len);

    if (rc != BOOT_FLASH_OK)
        return rc;
    if (data == 0)
        return BOOT_ERR_PARAM;

    memcpy(data, &ram->mem[offset], len);
    return BOOT_FLASH_OK;
}

static const boot_flash_ops_t g_ram_ops = {
    0, /* size 由实例决定，运行时填 */
    ram_erase,
    ram_write,
    ram_read,
};

const boot_flash_ops_t *boot_ram_ops(void)
{
    static boot_flash_ops_t ops;

    ops = g_ram_ops;
    ops.size = BOOT_RAM_FLASH_MAX;
    return &ops;
}

int boot_ram_init(boot_ram_flash_t *ram, uint32_t size)
{
    if (ram == 0 || size == 0 || size > BOOT_RAM_FLASH_MAX)
        return BOOT_ERR_PARAM;

    /* 上电状态：整片擦除后的 0xFF */
    memset(ram->mem, 0xFF, sizeof(ram->mem));
    ram->size = size;
    return BOOT_FLASH_OK;
}

/* ---------------- 包装层：统一参数校验 ---------------- */

int boot_flash_read(boot_flash_t *f, uint32_t offset, uint8_t *data, uint32_t len)
{
    if (f == 0 || f->ops == 0 || f->ops->read == 0)
        return BOOT_ERR_PARAM;
    return f->ops->read(f->ctx, offset, data, len);
}

int boot_flash_write(boot_flash_t *f, uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (f == 0 || f->ops == 0 || f->ops->write == 0)
        return BOOT_ERR_PARAM;
    return f->ops->write(f->ctx, offset, data, len);
}

int boot_flash_erase(boot_flash_t *f, uint32_t offset, uint32_t len)
{
    if (f == 0 || f->ops == 0 || f->ops->erase == 0)
        return BOOT_ERR_PARAM;
    return f->ops->erase(f->ctx, offset, len);
}
