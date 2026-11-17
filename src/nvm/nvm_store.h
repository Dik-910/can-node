/*
 * nvm_store.h —— 双页交替的参数存储
 *
 * 参数区被切成两页，写新数据永远写「非当前有效」的那一页。
 * 这样掉电发生在任何时刻，至少有一页是完整的：
 *
 *   写之前：  [A: seq=7 有效] [B: seq=6 旧]
 *   擦 B、写 B（掉电在这里）→ A 仍有效，B 是垃圾
 *   写完校验通过 → B 成为有效页，seq=8
 *
 * 单页原地覆盖的问题在于「擦除」本身就是不可中断的破坏动作：
 * 擦到一半掉电，参数和 CRC 一起没了，设备只能回默认值。对 ECU 来说
 * 这等于标定值丢失、故障历史清零。
 *
 * 代价是可用容量减半，以及每次保存要擦一整页 —— 所以上层要控制写频率
 * （见 persist.c：变化才写，且有最小间隔）。
 */
#ifndef NVM_STORE_H
#define NVM_STORE_H

#include <stdint.h>

#include "nvm.h"

/* 页头：magic(4) seq(4) len(2) crc(4) 保留(2) */
#define NVM_HEADER_SIZE  16

/* 单页可承载的最大负载。2KB 参数区对半 = 1KB 页，减去页头 */
#define NVM_PAYLOAD_MAX  1008

/* 页标记 "NVMS" 的小端表示 */
#define NVM_PAGE_MAGIC   0x534D564Eu

typedef struct
{
    const nvm_dev_t *dev;
    uint32_t page_size;

    uint8_t active;   /* 当前有效页号 0/1；0xFF 表示还没有有效页 */
    uint32_t seq;     /* 有效页的序号，单调递增 */
    uint16_t len;     /* 负载长度 */
    uint8_t buf[NVM_PAYLOAD_MAX];

    int valid;        /* 是否挂载到了有效数据 */
    uint32_t saves;   /* 累计成功写入次数，供观测与测试 */
    uint32_t rejects; /* 因校验失败被跳过的页数 */
} nvm_store_t;

/*
 * 挂载：读两页页头，校验 CRC，取序号较大的有效页。
 * 两页都没有有效数据时 valid = 0，buf 内容不定义 —— 上层用默认值。
 * 返回 0 表示挂载完成（无论是否读到数据），负值为设备错误。
 */
int nvm_store_mount(nvm_store_t *st, const nvm_dev_t *dev);

/*
 * 保存。擦除非当前页 → 写入 → 回读校验。
 * 校验失败返回负值，此时**旧页仍然有效**，st 的状态不变。
 *
 * len 超过 NVM_PAYLOAD_MAX 或未挂载时返回 -1。
 */
int nvm_store_save(nvm_store_t *st, const uint8_t *data, uint16_t len);

/* 取当前有效数据。返回 NULL 表示没有有效副本 */
const uint8_t *nvm_store_data(const nvm_store_t *st, uint16_t *len);

#endif
