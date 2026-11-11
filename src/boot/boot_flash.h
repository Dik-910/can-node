#ifndef __BOOT_FLASH_H
#define __BOOT_FLASH_H

#include <stdint.h>

/*
 * Flash 存储抽象
 *
 * 和 can_driver 一样用操作表注入：Bootloader 的决策逻辑（分区选择、CRC 校验、
 * 跳转条件）写成纯逻辑，PC 侧用内存模拟，硬件到了只实现这三个函数。
 *
 * 三条与真实 Flash 对齐的约定：
 *   1. 擦除必须以扇区为单位，且只能把 1 变 0 再整体恢复为 1
 *   2. 写入只能把 1 变 0，不能把已写的 0 改回 1（必须擦除）
 *   3. 写入前未擦除会失败——很多 Bootloader 的坑就在这里
 */

#define BOOT_FLASH_OK          0
#define BOOT_ERR_PARAM       (-1)
#define BOOT_ERR_RANGE       (-2) /* 越界 */
#define BOOT_ERR_NOT_ERASED  (-3) /* 目标区未擦除，写入会失败 */
#define BOOT_ERR_IO          (-4)

typedef struct
{
    uint32_t size; /* 模拟的 Flash 总容量（字节） */

    int (*erase)(void *ctx, uint32_t offset, uint32_t size);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);
    int (*read)(void *ctx, uint32_t offset, uint8_t *data, uint32_t len);
} boot_flash_ops_t;

typedef struct
{
    const boot_flash_ops_t *ops;
    void *ctx;
} boot_flash_t;

int boot_flash_read(boot_flash_t *f, uint32_t offset, uint8_t *data, uint32_t len);
int boot_flash_write(boot_flash_t *f, uint32_t offset, const uint8_t *data, uint32_t len);
int boot_flash_erase(boot_flash_t *f, uint32_t offset, uint32_t len);

/* PC 侧内存模拟：容量由调用方提供的缓冲区决定 */
#define BOOT_RAM_FLASH_MAX (256u * 1024u)

typedef struct
{
    uint8_t mem[BOOT_RAM_FLASH_MAX];
    uint32_t size;
} boot_ram_flash_t;

const boot_flash_ops_t *boot_ram_ops(void);
int boot_ram_init(boot_ram_flash_t *ram, uint32_t size);

#endif
