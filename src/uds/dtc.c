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

/* ---------------- 跨复位保持：序列化 ---------------- */

/*
 * 逐字节显式编码，理由和分区表、参数区一样：
 * 结构体有对齐填充，sizeof 随编译器变化；这份数据要能被
 * 下一次上电（可能是重新编译过的固件）读回来，布局必须固定。
 */
static void wire_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t wire_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *wire_put_entry(uint8_t *p, const dtc_entry_t *e)
{
    wire_put_u32(p, e->code);
    p[4] = e->status;
    p[5] = e->occurrence;
    p[6] = e->fail_cycles;
    p[7] = e->aging_cycles;
    wire_put_u32(p + 8, e->first_ms);
    wire_put_u32(p + 12, e->last_ms);
    wire_put_u32(p + 16, e->last_fail_cycle);
    p[20] = e->snapshot_len;
    p[21] = e->has_snapshot;
    memcpy(p + 22, e->snapshot, DTC_SNAPSHOT_MAX);

    return p + DTC_ENTRY_WIRE_SIZE;
}

static const uint8_t *wire_get_entry(const uint8_t *p, dtc_entry_t *e)
{
    memset(e, 0, sizeof(*e));

    e->code = wire_get_u32(p) & 0xFFFFFFu;
    e->status = p[4];
    e->occurrence = p[5];
    e->fail_cycles = p[6];
    e->aging_cycles = p[7];
    e->first_ms = wire_get_u32(p + 8);
    e->last_ms = wire_get_u32(p + 12);
    e->last_fail_cycle = wire_get_u32(p + 16);
    e->snapshot_len = p[20];
    e->has_snapshot = (uint8_t)(p[21] != 0);

    /* 长度越界的快照不当真：宁可当作没有，也不要按越界长度去读 */
    if (e->snapshot_len > DTC_SNAPSHOT_MAX)
    {
        e->snapshot_len = 0;
        e->has_snapshot = 0;
    }

    if (e->has_snapshot)
        memcpy(e->snapshot, p + 22, DTC_SNAPSHOT_MAX);

    return p + DTC_ENTRY_WIRE_SIZE;
}

int dtc_export(const dtc_store_t *st, uint8_t *buf, uint16_t size)
{
    uint16_t need;

    if (st == 0 || buf == 0)
        return -1;

    need = (uint16_t)(7u + DTC_ENTRY_WIRE_SIZE * st->count);

    if (size < need)
        return -1;

    buf[0] = (uint8_t)DTC_BLOB_VERSION;
    wire_put_u32(&buf[1], st->cycle);
    buf[5] = (uint8_t)(st->count & 0xFF);
    buf[6] = (uint8_t)(st->count >> 8);

    {
        uint8_t *p = &buf[7];

        for (uint16_t i = 0; i < st->count; i++)
            p = wire_put_entry(p, &st->entries[i]);
    }

    return (int)need;
}

int dtc_import(dtc_store_t *st, const uint8_t *buf, uint16_t len)
{
    uint16_t count;
    uint16_t stored = 0;
    const uint8_t *p;

    if (st == 0 || buf == 0 || len < 7)
        return -1;

    if (buf[0] != DTC_BLOB_VERSION)
        return -1;

    count = (uint16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8));

    if (count > DTC_MAX_ENTRIES)
        return -1;

    /* 长度必须与 count 严格对上，宁可拒绝也不要读越界 */
    if (len != (uint16_t)(7u + DTC_ENTRY_WIRE_SIZE * count))
        return -1;

    /*
     * cycle 直接覆盖，不从 1 重新开始。
     * 这一步不能省：老化判据是「连续 N 个循环」，跨复位时计数轴必须是同一根，
     * 每次上电都归零等于老化永远不生效。
     */
    st->cycle = wire_get_u32(&buf[1]);

    p = &buf[7];

    for (uint16_t i = 0; i < count; i++)
    {
        dtc_entry_t e;

        p = wire_get_entry(p, &e);

        /*
         * 码不在当前定义表里就跳过。
         * 固件升级后码表可能变化，为一条不认识的码拒绝整块数据，
         * 等于把全部故障历史和 cycle 一起丢掉。
         */
        if (!code_is_defined(st, e.code))
            continue;

        if (stored >= DTC_MAX_ENTRIES)
            break;

        /*
         * 跨复位时只清这两个「描述刚才那次测试」的位，其余原样保留。
         *
         *   bit0 testFailed                 最近一次测试结果
         *   bit1 testFailedThisOperationCycle  本循环失败过
         *
         * 留着 bit0，上电后第一次失败不会被判为上升沿，occurrence 少计一次。
         * 留着 bit1，本轮老化结算会认为「这个循环失败过」，把 aging 清零。
         *
         * bit6（本循环没测）**不动**：它保存的是上次运行结束时的状态，
         * 正是判断「上一个循环到底测没测过」的依据。
         * 这里若强行置位或清零，等于替上一个循环编造结论 ——
         * 传感器掉线的那些循环会被当成「测过且通过」，故障就会自己消失。
         */
        e.status &= (uint8_t)~(DTC_STATUS_TEST_FAILED |
                               DTC_STATUS_TEST_FAILED_THIS_CYCLE);

        st->entries[stored++] = e;
    }

    st->count = stored;

    return (int)stored;
}
