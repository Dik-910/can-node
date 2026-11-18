#ifndef __DTC_H
#define __DTC_H

#include <stdint.h>

/*
 * 故障存储器（Diagnostic Trouble Code memory）
 *
 * 这一层只负责「记住发生过什么故障」，不管怎么被读出来。
 * 0x19 读故障码、0x14 清故障码是它的两个编码入口，在 uds.c 里。
 *
 * 拆开的理由：故障判定逻辑（什么算故障、什么时候记一次）和协议编码
 * 是完全不同的两件事，混在一起会让状态机难以测试。现在这一层是纯逻辑，
 * 不需要 CAN、不需要 UDS，喂时间和事件就能跑完整个生命周期。
 *
 * ---- 状态位（ISO 14229-1 定义，位序不能改）----
 *
 *   bit0 TF       testFailed                 本次测试失败
 *   bit1 TFTOC    testFailedThisOperationCycle  本操作循环内失败过
 *   bit2 PDTC     pendingDTC                待确认
 *   bit3 CDTC     confirmedDTC              已确认，真正「存下来」的故障
 *   bit4 TNCSLC   testNotCompletedSinceLastClear 清码后还没测过
 *   bit5 TFSLC    testFailedSinceLastClear  清码后失败过
 *   bit6 TNCTOC   testNotCompletedThisOperationCycle 本循环还没测过
 *   bit7 WIR      warningIndicatorRequested 需要点亮故障灯
 *
 * ---- 判定与老化 ----
 *
 * 失败一次只是 pending（偶发故障不该立刻停车报警），
 * 在**两个不同的操作循环**里都失败才升为 confirmed。
 * 之后连续 aging_cycles 个循环都没再失败，confirmed 自动老化清除 ——
 * 否则一次静电打出来的假故障会永久留在 ECU 里，产线和售后都没法处理。
 */

#define DTC_STATUS_TEST_FAILED                    0x01u
#define DTC_STATUS_TEST_FAILED_THIS_CYCLE         0x02u
#define DTC_STATUS_PENDING                        0x04u
#define DTC_STATUS_CONFIRMED                      0x08u
#define DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR 0x10u
#define DTC_STATUS_TEST_FAILED_SINCE_CLEAR        0x20u
#define DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE  0x40u
#define DTC_STATUS_WARNING_INDICATOR              0x80u

/* 0x19 响应里回给 tester 的「支持哪些状态位」，本实现八位全用 */
#define DTC_STATUS_AVAILABILITY_MASK 0xFFu

/* 快照（freeze frame）最大长度。真实项目一般存 10~30 字节的工况数据 */
#define DTC_SNAPSHOT_MAX 24

/*
 * 快照内容的布局（对上层是约定，dtc 层只当字节数组搬运）：
 *
 *   [0]        identifier 个数 n
 *   [1..]      n 组 (DID 高字节 DID 低字节 数据...)
 *
 * 必须是这个结构，不能存裸字节：0x19 04 和 0x19 05 的响应里都要求
 * 「numberOfIdentifiers 后面跟若干组 DID+数据」，裸字节没有 DID 就编不出来，
 * tester 也没法知道每段数据是什么。
 */

/* 表容量。超出直接丢弃并返回错误，不动态分配 —— 车规代码不做堆 */
#define DTC_MAX_ENTRIES 16

/* 默认：连续 40 个操作循环无故障则老化清除（多数主机厂用 40） */
#define DTC_DEFAULT_AGING_CYCLES 40

/* 默认：两个不同循环都失败才确认 */
#define DTC_DEFAULT_CONFIRM_CYCLES 2

/* 组码，0x14 用 */
#define DTC_GROUP_ALL 0xFFFFFFu
#define DTC_GROUP_EMISSIONS 0x000000u

/*
 * 故障定义表项。
 *
 * code 是 3 字节 DTC，高两位表示故障类别：
 *   00 动力总成  01 底盘  10 车身  11 网络
 * 本节点是全网络通信类故障，所以用 0xC0xxxx。
 */
typedef struct
{
    uint32_t code;
    const char *name; /* 英文，出现在日志里；与 C 源码字符串约定一致 */
} dtc_def_t;

/* 单条故障的运行态 */
typedef struct
{
    uint32_t code;
    uint8_t status;
    uint8_t occurrence;    /* 发生次数，饱和到 255，不溢出回绕 */
    uint8_t fail_cycles;   /* 已确认失败过的不同循环数 */
    uint8_t aging_cycles;  /* 连续无故障循环数 */
    uint32_t first_ms;     /* 首次发生时刻 */
    uint32_t last_ms;      /* 最近一次发生时刻 */
    uint32_t last_fail_cycle; /* 上次失败所在的操作循环号，用来去重 */

    uint8_t snapshot[DTC_SNAPSHOT_MAX];
    uint8_t snapshot_len;
    uint8_t has_snapshot;
} dtc_entry_t;

/*
 * 快照采集回调。首次失败时调用一次，把故障当时的工况填进 out。
 * 返回写入长度，0 表示没有快照。可为空。
 *
 * 为什么用回调而不是让调用方自己塞：故障发生点往往在中断或
 * 通信层深处，那里不该知道「快照要存什么」。
 */
typedef int (*dtc_snapshot_fn)(uint32_t code, uint8_t *out, uint8_t max_len);

typedef struct
{
    uint32_t code;
    uint8_t status;
    uint8_t occurrence;
    uint8_t aging_cycles;
    uint32_t first_ms;
    uint32_t last_ms;
    uint8_t snapshot[DTC_SNAPSHOT_MAX];
    uint8_t snapshot_len;
    uint8_t has_snapshot;
} dtc_info_t;

typedef struct
{
    uint16_t def_count;
    const dtc_def_t *defs;

    uint8_t confirm_cycles; /* 多少个不同循环失败才 confirmed，0 用默认 2 */
    uint8_t aging_cycles;   /* 多少个循环无故障自动老化，0 用默认 40 */
} dtc_config_t;

typedef struct
{
    dtc_config_t cfg;
    dtc_entry_t entries[DTC_MAX_ENTRIES];
    uint16_t count;
    uint32_t cycle; /* 操作循环号，每次上电/点火循环 +1 */
    dtc_snapshot_fn snapshot_fn;
} dtc_store_t;

/* 初始化。defs 指向的表必须比 store 活得久（通常是静态常量） */
int dtc_init(dtc_store_t *st, const dtc_config_t *cfg);

void dtc_set_snapshot_provider(dtc_store_t *st, dtc_snapshot_fn fn);

/*
 * 上报一次测试结果。这是唯一的故障输入口。
 *
 * failed 非 0 表示本次测试失败，0 表示通过。
 * 必须在**每个操作循环内**至少调用一次（通过也要调），否则老化计数不推进，
 * 故障永远清不掉。没调用的后果是 bit6 一直置位，读出来能看出「这个循环没测」。
 *
 * now_ms 由调用方提供：本层不取时间，和 ISO-TP 一样，时间责任在调用方。
 *
 * 返回 0 表示记录成功，-1 表示码未在定义表里（调用方写错码时会立刻暴露，
 * 而不是往表里塞一条永远不会被读到的垃圾）。
 */
int dtc_report(dtc_store_t *st, uint32_t code, int failed, uint32_t now_ms);

/* 操作循环切换（上电、点火循环）。推进老化、清循环内状态位 */
void dtc_operation_cycle(dtc_store_t *st);

/*
 * 清故障码。group 为 DTC_GROUP_ALL 时清全部，否则只清匹配的单条。
 * 返回清掉的条数，负值表示参数非法。
 */
int dtc_clear(dtc_store_t *st, uint32_t group);

/* 按状态掩码统计条数：status & mask 非 0 即计入 */
uint16_t dtc_count_by_mask(const dtc_store_t *st, uint8_t mask);

/* 按下标取条目，越界返回 NULL。0x19 遍历用 */
const dtc_entry_t *dtc_get(const dtc_store_t *st, uint16_t index);

/* 按码取条目，没有返回 NULL */
const dtc_entry_t *dtc_find(const dtc_store_t *st, uint32_t code);

/* 填一份对外的拷贝，避免调用方拿到内部指针。成功返回 0 */
int dtc_get_info(const dtc_store_t *st, uint32_t code, dtc_info_t *out);

/* 故障名称，查不到返回 "UNKNOWN" */
const char *dtc_name(const dtc_store_t *st, uint32_t code);

/* ---------------- 跨复位保持 ---------------- */

/*
 * 故障存储要跨复位保留，否则「连续 40 个操作循环无故障才老化清除」
 * 这件事在上电周期之间根本接不上 —— 每条故障的 aging_cycles 都从 0 重新数，
 * 永远到不了 40。
 *
 * 所以序列化的内容不只是条目，还有**操作循环号 cycle** 本身：
 * 它是老化计数的分母，丢了等于把老化进度清零。
 *
 * 本层仍然不碰 IO：只负责「打包成字节 / 从字节恢复」，
 * 存到哪里由 nvm 层决定。
 */

/* 布局版本。字段有任何增减都要递增，旧数据会被 import 拒绝 */
#define DTC_BLOB_VERSION 1u

/* 单条记录的线格式字节数（显式逐字节，不含结构体填充） */
#define DTC_ENTRY_WIRE_SIZE (4 + 1 + 1 + 1 + 1 + 4 + 4 + 4 + 1 + 1 + DTC_SNAPSHOT_MAX)

/* 打包后最大长度：头 7 字节 + 满表 */
#define DTC_BLOB_MAX (7u + DTC_ENTRY_WIRE_SIZE * DTC_MAX_ENTRIES)

/*
 * 打包。返回写入字节数，负值表示缓冲区不够。
 * 空表也会写头（版本 + cycle + count=0），这样 cycle 能单独保存。
 */
int dtc_export(const dtc_store_t *st, uint8_t *buf, uint16_t size);

/*
 * 从字节恢复。返回恢复的条目数，负值表示数据不可用。
 *
 * 不可用的情形：版本不符、长度与 count 不符、count 超上限。
 * 单条码不在当前定义表里时跳过该条（固件升级后码表可能变化），
 * 不因此拒绝整块数据 —— 丢掉全部故障历史比丢一条更糟。
 *
 * 调用前需已 dtc_init（cfg 与 snapshot_fn 不会被覆盖）。
 */
int dtc_import(dtc_store_t *st, const uint8_t *buf, uint16_t len);

#endif
