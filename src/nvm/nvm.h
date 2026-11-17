/*
 * nvm.h —— 参数区块设备接口
 *
 * 与 can_driver、uds_memory_t 同样的做法：操作表注入。
 * 上层（nvm_store）只管「怎么在一页上安全地写、怎么判断哪页有效」，
 * 具体落在 Flash 扇区还是文件由这里决定。
 *
 * 为什么不复用 uds_memory_t：
 *   那个接口描述的是刷写目标（App 分区），0x31 0xFF00 会整段擦除它。
 *   参数区按设计要在升级后保留，两者生命周期完全不同，共用接口会让
 *   「升级时参数被一起擦掉」这种错误变得不容易发现。
 */
#ifndef NVM_H
#define NVM_H

#include <stdint.h>

/* 返回 0 表示成功，负值失败 */
typedef struct
{
    int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint16_t len);
    int (*write)(void *ctx, uint32_t off, const uint8_t *buf, uint16_t len);

    /*
     * 擦除。参数区要求擦除后读出 0xFF，与 Flash 语义一致。
     * 真实 Flash 的擦除粒度是一整个扇区，实现里自行按扇区对齐。
     */
    int (*erase)(void *ctx, uint32_t off, uint32_t len);

    void *ctx;

    /* 参数区总字节数，由 nvm_store 对半分给两个页 */
    uint32_t size;
} nvm_dev_t;

#endif
