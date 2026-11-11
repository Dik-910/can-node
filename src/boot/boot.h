#ifndef __BOOT_H
#define __BOOT_H

#include <stdint.h>

#include "boot_flash.h"

/*
 * Bootloader 核心逻辑：分区表、CRC 校验、跳转条件、升级流程。
 *
 * 这一层不含任何与芯片相关的代码——真正的跳转（设 MSP、取复位向量、BX）
 * 必须用汇编且平台相关，放在平台层。这里只回答两个问题：
 *   1. 现在该跳哪个分区
 *   2. 这个分区里的映像到底能不能跳
 *
 * 分区表放在独立扇区，升级过程只改它，不碰 Bootloader 自身。
 */

/* ---------------- CRC32 ---------------- */

/*
 * 标准 CRC-32（IEEE 802.3，多项式 0xEDB88320 反射）。
 * 与 zlib / 以太网 / STM32 硬件 CRC 常见配置一致，
 * 校验值可用 "123456789" -> 0xCBF43926 验证。
 */
uint32_t boot_crc32(const uint8_t *data, uint32_t len);
uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/* ---------------- 分区表 ---------------- */

#define BOOT_TABLE_OFFSET  0x1000u    /* 分区表所在位置，紧跟在 Bootloader 之后 */
#define BOOT_TABLE_MAGIC   0x544F4F42u /* 'BOOT'，小端存储 */
#define BOOT_TABLE_VERSION 1u
#define BOOT_TABLE_SIZE    47u        /* 显式序列化后的字节数，见 boot.c */

#define BOOT_SLOT_A 0
#define BOOT_SLOT_B 1
#define BOOT_SLOT_COUNT 2

typedef struct
{
    uint32_t offset;     /* 分区起始地址（相对 Flash 基址） */
    uint32_t size;
    uint32_t crc32;      /* 整个映像的 CRC */
    uint32_t fw_version; /* 固件版本，用于判断新分区是否可用 */
    uint8_t valid;       /* 1 表示该分区内容完整可信 */
} boot_part_t;

typedef struct
{
    uint8_t active; /* 当前应启动的分区 */
    boot_part_t parts[BOOT_SLOT_COUNT];
} boot_table_t;

/* 读分区表。magic 或 CRC 不对都视为无效，返回错误让上层走「等待诊断」分支 */
int boot_table_load(boot_flash_t *f, boot_table_t *t);
int boot_table_save(boot_flash_t *f, const boot_table_t *t);

/* 选择可启动分区：优先 active，坏了再退到另一个 */
int boot_select_slot(const boot_table_t *t, uint8_t *slot);

/* ---------------- 跳转检查 ---------------- */

typedef enum
{
    BOOT_CHECK_OK = 0,
    BOOT_CHECK_BAD_STACK = 1, /* 初始栈顶不在 SRAM 范围 */
    BOOT_CHECK_BAD_ENTRY = 2, /* 复位向量越界或不是 Thumb 地址 */
    BOOT_CHECK_BAD_CRC = 3    /* 映像校验不符 */
} boot_check_t;

typedef struct
{
    uint32_t sram_base;
    uint32_t sram_size;
    uint32_t flash_base;
    uint32_t flash_size;
} boot_mem_map_t;

/*
 * 判断映像能否跳转。vectors 指向映像起始处的向量表
 * （vectors[0] = 初始 MSP，vectors[1] = Reset_Handler）。
 *
 * 三项都要过：栈顶在 SRAM 内、复位向量在 Flash 内且为奇数（Thumb 位）、CRC 匹配。
 * 只查其中一两项是最常见的翻车点——比如只查 CRC，跳过去仍然是 HardFault。
 */
boot_check_t boot_check_vectors(const uint32_t *vectors, const boot_mem_map_t *map,
                                uint32_t image_crc, uint32_t expected_crc);

/* ---------------- 升级流程 ---------------- */

#define BOOT_UPDATE_IDLE      0
#define BOOT_UPDATE_RECEIVING 1

typedef struct
{
    boot_flash_t *flash;
    boot_table_t *table;
    uint8_t target;   /* 本次写入的目标分区 */
    uint8_t state;
    uint32_t written;
    uint32_t crc;     /* 写入过程中累积的 CRC */
} boot_update_t;

/* 开始升级：选目标分区、擦除、进入接收态 */
int boot_update_begin(boot_update_t *u, boot_flash_t *f, boot_table_t *t);

/* 追加一块数据，顺序写入目标分区 */
int boot_update_write(boot_update_t *u, const uint8_t *data, uint32_t len);

/*
 * 结束升级：从 Flash 重新读回数据算一遍 CRC，与写入时累积的值比对，
 * 一致才标记有效并切换启动分区。
 *
 * 「重读校验」而不是「相信写入过程」——Flash 写入失败可能静默发生，
 * 不重读就发现不了。
 */
int boot_update_finish(boot_update_t *u, uint32_t fw_version);

void boot_update_abort(boot_update_t *u);

#endif
