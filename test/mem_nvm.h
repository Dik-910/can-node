/*
 * mem_nvm.h —— 测试用的内存参数区
 *
 * 除了存取，还要能模拟两件真机上才会出现的事：
 *   - 写到一半掉电（擦除或写入中途失败）
 *   - 写入静默失效（写成功了但读回来不是那个值）
 *
 * 这两种是双页方案存在的全部理由，测不到就等于没实现。
 */
#ifndef MEM_NVM_H
#define MEM_NVM_H

#include <stdint.h>

#include "nvm.h"

#define MEM_NVM_MAX 8192u

typedef struct
{
    uint8_t area[MEM_NVM_MAX];
    uint32_t size;

    /* 剩余可写字节数。-1 表示不受限；0 时任何写入都失败 */
    int budget;

    /* 非 0 时，写入的数据按位与掩码后落盘，模拟位粘连导致的静默错误 */
    uint8_t corrupt_mask;

    uint32_t operations;

    /* 设备接口放在实例里，多个参数区互不干扰 */
    nvm_dev_t dev;
} mem_nvm_t;

void mem_nvm_init(mem_nvm_t *m, uint32_t size);

/* 恢复到完全擦除态 */
void mem_nvm_erase_all(mem_nvm_t *m);

const nvm_dev_t *mem_nvm_dev(mem_nvm_t *m);

#endif
