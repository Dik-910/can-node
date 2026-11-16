#include "dtc.h"

#include <string.h>

/*
 * 故障存储器实现。
 *
 * 全部是纯逻辑：不取时间、不碰 IO、不分配内存。
 * 这样整个故障生命周期（失败 -> 待确认 -> 确认 -> 老化清除）可以在
 * 单元测试里用几行代码跑完，不用等真实的几个小时。
 */

/*
 * last_fail_cycle 的哨兵值。
 *
 * 用 0xFFFFFFFF 而不是 0，是因为操作循环号从 0 开始 ——
 * 拿 0 当「没有记录过」会和第一次失败撞车，导致 fail_cycles 少算一次，
 * 于是「两个循环都失败才确认」永远触发不了。这和之前在 ISO-TP 里
 * 用 0 当「定时器未启动」踩的是同一个坑。
 */
#define DTC_NO_CYCLE 0xFFFFFFFFu

/* 一条故障被写入/清除后的初始状态：还没测过 */
#define DTC_STATUS_FRESH (DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR | \
                          DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE)

static uint16_t effective_confirms(const dtc_store_t *st)
{
    return (st->cfg.confirm_cycles > 0) ? st->cfg.confirm_cycles
                                        : DTC_DEFAULT_CONFIRM_CYCLES;
}

static uint16_t effective_aging(const dtc_store_t *st)
{
    return (st->cfg.aging_cycles > 0) ? st->cfg.aging_cycles
                                      : DTC_DEFAULT_AGING_CYCLES;
}

static void entry_reset(dtc_entry_t *e)
{
    uint32_t code = e->code;
    memset(e, 0, sizeof(*e));
    e->code = code;
    e->status = DTC_STATUS_FRESH;
    e->last_fail_cycle = DTC_NO_CYCLE;
}

static dtc_entry_t *entry_find(dtc_store_t *st, uint32_t code)
{
    for (uint16_t i = 0; i < st->count; i++)
    {
        if (st->entries[i].code == code)
            return &st->entries[i];
    }
    return 0;
}

/* 码必须先在定义表里登记过，不允许随手上报一个没定义过的码 */
static int code_is_defined(const dtc_store_t *st, uint32_t code)
{
    if (st->cfg.def_count == 0)
        return 1; /* 没给定义表就不校验，便于单独测试 */

    for (uint16_t i = 0; i < st->cfg.def_count; i++)
    {
        if ((st->cfg.defs[i].code & 0xFFFFFFu) == code)
            return 1;
    }
    return 0;
}

/* ---------------- 对外接口 ---------------- */

int dtc_init(dtc_store_t *st, const dtc_config_t *cfg)
{
    if (st == 0 || cfg == 0)
        return -1;

    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
    st->cycle = 0;

    /*
     * 定义表里每一条都要建一条运行态，哪怕从来没故障过。
     *
     * 原因：0x19 0A（读支持的故障码）要求返回「本 ECU 支持的全部 DTC」，
     * 而不是「当前有故障的 DTC」。先建表才能把这个语义区分开。
     */
    for (uint16_t i = 0; i < cfg->def_count && st->count < DTC_MAX_ENTRIES; i++)
    {
        dtc_entry_t *e = &st->entries[st->count];
        memset(e, 0, sizeof(*e));
        e->code = cfg->defs[i].code & 0xFFFFFFu;
        e->status = DTC_STATUS_FRESH;
        e->last_fail_cycle = DTC_NO_CYCLE;
        st->count++;
    }

    return 0;
}

void dtc_set_snapshot_provider(dtc_store_t *st, dtc_snapshot_fn fn)
{
    if (st != 0)
        st->snapshot_fn = fn;
}

int dtc_report(dtc_store_t *st, uint32_t code, int failed, uint32_t now_ms)
{
    dtc_entry_t *e;

    if (st == 0)
        return -1;

    code &= 0xFFFFFFu;

    if (!code_is_defined(st, code))
        return -1;

    e = entry_find(st, code);
    if (e == 0)
    {
        /* 定义表里没有但允许上报的情形：表满则丢弃 */
        if (st->count >= DTC_MAX_ENTRIES)
            return -1;

        e = &st->entries[st->count++];
        memset(e, 0, sizeof(*e));
        e->code = code;
        e->status = DTC_STATUS_FRESH;
        e->last_fail_cycle = DTC_NO_CYCLE;
    }

    if (failed)
    {
        /*
         * 上升沿用「上一次测试是否失败」判断。
         *
         * 发生次数按沿计而不是按调用计：主循环每 10ms 报一次，
         * 如果每次调用都 +1，一个持续故障十秒钟就是 1000 次，
         * 计数器立刻饱和成 255，售后看不出「到底偶发了几次」。
         */
        int rising = (e->status & DTC_STATUS_TEST_FAILED) == 0;

        e->status |= DTC_STATUS_TEST_FAILED |
                     DTC_STATUS_TEST_FAILED_THIS_CYCLE |
                     DTC_STATUS_TEST_FAILED_SINCE_CLEAR;
        e->status &= (uint8_t)~(DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR |
                                DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE);

        if (rising)
        {
            if (e->occurrence == 0)
                e->first_ms = now_ms;
            if (e->occurrence < 0xFFu)
                e->occurrence++;
            e->last_ms = now_ms;

            /* 第一次失败才抓快照：之后的工况已经漂移了，参考价值更低 */
            if (!e->has_snapshot && st->snapshot_fn != 0)
            {
                int n = st->snapshot_fn(e->code, e->snapshot, DTC_SNAPSHOT_MAX);
                if (n > 0 && n <= DTC_SNAPSHOT_MAX)
                {
                    e->snapshot_len = (uint8_t)n;
                    e->has_snapshot = 1;
                }
            }
        }

        e->status |= DTC_STATUS_PENDING;

        /*
         * 失败循环计数。同一个循环里连续失败只算一次，
         * 跨循环才 +1 —— 这正是「偶发/持续」的区分方式：
         * 上电抖一下不复位就是 pending，反复上电都挂才确认。
         */
        if (e->last_fail_cycle != st->cycle)
        {
            e->last_fail_cycle = st->cycle;
            if (e->fail_cycles < 0xFFu)
                e->fail_cycles++;
        }

        if (e->fail_cycles >= effective_confirms(st))
            e->status |= DTC_STATUS_CONFIRMED | DTC_STATUS_WARNING_INDICATOR;
    }
    else
    {
        /* 通过：只清「本次失败」，历史痕迹（本循环失败过、清码后失败过）保留 */
        e->status &= (uint8_t)~(DTC_STATUS_TEST_FAILED |
                                DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR |
                                DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE);
    }

    return 0;
}

void dtc_operation_cycle(dtc_store_t *st)
{
    uint16_t aging_limit;

    if (st == 0)
        return;

    st->cycle++;
    aging_limit = effective_aging(st);

    for (uint16_t i = 0; i < st->count; i++)
    {
        dtc_entry_t *e = &st->entries[i];

        /* 先按「上一个循环」结算，再做循环切换，顺序反了就永远差一拍 */
        int tested = (e->status & DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE) == 0;
        int failed = (e->status & DTC_STATUS_TEST_FAILED_THIS_CYCLE) != 0;

        if (tested && !failed)
        {
            if (e->aging_cycles < 0xFFu)
                e->aging_cycles++;
        }
        else
        {
            /*
             * 循环内失败过，或者根本没人测 —— 都不能算「无故障」，
             * 计数归零。没人测就老化会让没接上的传感器把故障悄悄清掉。
             */
            e->aging_cycles = 0;
        }

        if (e->aging_cycles >= aging_limit)
        {
            e->status &= (uint8_t)~(DTC_STATUS_CONFIRMED |
                                    DTC_STATUS_PENDING |
                                    DTC_STATUS_WARNING_INDICATOR);
            e->fail_cycles = 0;
            e->last_fail_cycle = DTC_NO_CYCLE;
            e->aging_cycles = 0;
            e->has_snapshot = 0;
            e->snapshot_len = 0;
        }

        e->status &= (uint8_t)~DTC_STATUS_TEST_FAILED_THIS_CYCLE;
        e->status |= DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE;
    }
}

int dtc_clear(dtc_store_t *st, uint32_t group)
{
    int cleared = 0;

    if (st == 0)
        return -1;

    group &= 0xFFFFFFu;

    /*
     * 0xFFFFFF = 全部，0x000000 = 排放相关全部。
     * 本节点没有排放相关故障，两个都按「全部」处理并写进协议文档，
     * 免得 tester 发 000000 时收到 0 条、以为清码失败。
     */
    int all = (group == DTC_GROUP_ALL) || (group == DTC_GROUP_EMISSIONS);

    for (uint16_t i = 0; i < st->count; i++)
    {
        if (all || st->entries[i].code == group)
        {
            entry_reset(&st->entries[i]);
            cleared++;
        }
    }

    return cleared;
}

uint16_t dtc_count_by_mask(const dtc_store_t *st, uint8_t mask)
{
    uint16_t n = 0;

    if (st == 0)
        return 0;

    for (uint16_t i = 0; i < st->count; i++)
    {
        if ((st->entries[i].status & mask) != 0)
            n++;
    }

    return n;
}

const dtc_entry_t *dtc_get(const dtc_store_t *st, uint16_t index)
{
    if (st == 0 || index >= st->count)
        return 0;

    return &st->entries[index];
}

const dtc_entry_t *dtc_find(const dtc_store_t *st, uint32_t code)
{
    if (st == 0)
        return 0;

    code &= 0xFFFFFFu;

    for (uint16_t i = 0; i < st->count; i++)
    {
        if (st->entries[i].code == code)
            return &st->entries[i];
    }

    return 0;
}

int dtc_get_info(const dtc_store_t *st, uint32_t code, dtc_info_t *out)
{
    const dtc_entry_t *e = dtc_find(st, code);

    if (e == 0 || out == 0)
        return -1;

    out->code = e->code;
    out->status = e->status;
    out->occurrence = e->occurrence;
    out->aging_cycles = e->aging_cycles;
    out->first_ms = e->first_ms;
    out->last_ms = e->last_ms;
    out->snapshot_len = e->snapshot_len;
    out->has_snapshot = e->has_snapshot;
    memset(out->snapshot, 0, sizeof(out->snapshot));
    if (e->has_snapshot)
        memcpy(out->snapshot, e->snapshot, e->snapshot_len);

    return 0;
}

const char *dtc_name(const dtc_store_t *st, uint32_t code)
{
    code &= 0xFFFFFFu;

    if (st != 0)
    {
        for (uint16_t i = 0; i < st->cfg.def_count; i++)
        {
            if ((st->cfg.defs[i].code & 0xFFFFFFu) == code)
                return st->cfg.defs[i].name;
        }
    }

    return "UNKNOWN";
}
