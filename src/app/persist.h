/*
 * persist.h —— 节点持久化状态
 *
 * 把两样东西打包存进参数区：标定参数与故障存储器。
 *
 * 为什么不分开存：
 *   它们共享同一份「上电时该恢复成什么样」的语义，分开就要在 NVM 里
 *   管理两套页和两份 CRC。合成一个 blob 后，一次写入要么全成功要么全失败，
 *   不会出现「参数是新的、故障历史是旧的」这种半更新状态。
 *
 * 写频率控制：
 *   故障的 status 位每 10ms 就变一次（本循环测过没、本次失败没），
 *   逐字节比较触发写入的话，一天就能把 Flash 写穿。
 *   所以判断依据是「值得留存的变化」——确认/待确认状态、发生次数、
 *   操作循环号、参数值——而不是整个 blob。
 */
#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>

#include "dtc.h"
#include "nvm_store.h"

/* 参数区负载的第一字节。布局变化必须递增 */
#define PERSIST_VERSION 1u

/* 两次写入的最小间隔。真实 Flash 单页擦写寿命 1 万~10 万次，
 * 这个间隔把最坏情况下的写频率压到 0.5 次/秒 */
#define PERSIST_MIN_INTERVAL_MS 2000u

/* 参数段预留长度。加字段时只要不超过这个值，旧数据仍然可读 */
#define PERSIST_PARAM_MAX 16

/* ver(1) + param_len(2) + params + dtc_len(2) + dtc blob */
#define PERSIST_BLOB_MAX (1u + 2u + PERSIST_PARAM_MAX + 2u + DTC_BLOB_MAX)

typedef struct
{
    /*
     * DID 0x0201 的标定值。单位由 DID 定义（本节点是 0~100 的百分比），
     * 持久化层不解释语义，只负责原样存回。
     */
    uint32_t threshold;
} persist_params_t;

typedef struct
{
    nvm_store_t *nvm;
    dtc_store_t *dtcs;

    persist_params_t params;

    /* 上次成功写入的内容，用来判断「真的变了没有」 */
    uint8_t last[PERSIST_BLOB_MAX];
    uint16_t last_len;
    int last_valid;

    /* 上次落盘时的关键状态签名，见 persist.c 里为什么不能逐字节比 */
    uint32_t last_sig;

    uint32_t last_save_ms;
    uint32_t writes;   /* 累计落盘次数，供观测 */
} persist_t;

/*
 * 上电引导：挂载块设备 → 恢复状态。
 *
 * 这个序列（擦除？挂载？恢复？）在 ecu_server 和 node_main 里各写过一遍，
 * 结果两边都写错过一次：直接访问 dev->erase，而此时 dev 的函数表还没被
 * nvm_file_dev 填充，一执行就跳 NULL。抽到这里以后有单测盯着。
 *
 * reset 非 0 时先把整个参数区擦除，等价于一块新板子。
 *
 * 成功返回时**已经推进了一个操作循环**（dtc_operation_cycle）——
 * 本次上电就是一个新循环，且是在恢复之后推进的，cycle 接着上次的数。
 * 调用方不要再自己调一次。
 *
 * 返回 0 成功，负值表示设备不可用 —— 调用方应当降级运行而不是退出。
 */
int persist_boot(persist_t *p, nvm_store_t *nvm, const nvm_dev_t *dev,
                 int reset, dtc_store_t *dtcs,
                 const persist_params_t *def, uint32_t now_ms);

/*
 * 挂载。nvm 必须已经 nvm_store_mount 过（那一层会重置自身状态，
 * 两者顺序反了会把刚读到的数据清掉）。
 *
 * 没有有效副本时：params 取 *def，故障存储器保持 dtc_init 之后的空状态，
 * 并立刻写一次 —— 这样下次上电至少能读到一份结构正确的东西。
 *
 * 返回 0 表示完成（无论是否读到旧数据），负值为参数错误。
 */
int persist_mount(persist_t *p, nvm_store_t *nvm, dtc_store_t *dtcs,
                  const persist_params_t *def, uint32_t now_ms);

/*
 * 定期调用。有值得留存的变化、且距上次写入超过最小间隔时落盘。
 * 返回 1 表示本次写了，0 表示没写，负值表示写失败。
 */
int persist_flush(persist_t *p, uint32_t now_ms);

/* 强制落盘，忽略最小间隔。参数写入后用这个，别等下次定时 */
int persist_save_now(persist_t *p, uint32_t now_ms);

#endif
