#include "persist.h"

#include <string.h>

#include "crc32.h"

/*
 * 参数区容量必须装得下最坏情况：满表故障 + 参数段。
 * 这个检查在编译期完成 —— 真到了板子上才发现装不下，是要改分区表的。
 */
typedef char persist_blob_fits[(PERSIST_BLOB_MAX <= NVM_PAYLOAD_MAX) ? 1 : -1];

/* ---------------- 打包 ---------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 参数段：threshold 4 字节小端。加字段往后追加，param_len 随之增长 */
static uint16_t pack_params(const persist_params_t *pp, uint8_t *out)
{
    out[0] = (uint8_t)(pp->threshold & 0xFF);
    out[1] = (uint8_t)((pp->threshold >> 8) & 0xFF);
    out[2] = (uint8_t)((pp->threshold >> 16) & 0xFF);
    out[3] = (uint8_t)((pp->threshold >> 24) & 0xFF);

    return 4;
}

/* 旧数据的参数段较短时保留默认值，只覆盖读到的部分 —— 向前兼容 */
static void unpack_params(const uint8_t *in, uint16_t len, persist_params_t *pp)
{
    if (len < 4)
        return;

    pp->threshold = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                    ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static int pack(persist_t *p, uint8_t *buf, uint16_t size)
{
    uint16_t param_len;
    int dtc_len;
    uint16_t pos = 0;

    if (size < 5)
        return -1;

    buf[0] = (uint8_t)PERSIST_VERSION;
    pos = 1;

    param_len = pack_params(&p->params, &buf[pos + 2]);
    put_u16(&buf[pos], param_len);
    pos = (uint16_t)(pos + 2 + param_len);

    if (size < (uint16_t)(pos + 2))
        return -1;

    dtc_len = dtc_export(p->dtcs, &buf[pos + 2], (uint16_t)(size - pos - 2));
    if (dtc_len < 0)
        return -1;

    put_u16(&buf[pos], (uint16_t)dtc_len);
    pos = (uint16_t)(pos + 2 + dtc_len);

    return (int)pos;
}

static int unpack(persist_t *p, const uint8_t *buf, uint16_t len)
{
    uint16_t param_len;
    uint16_t dtc_len;
    uint16_t pos;

    if (len < 5 || buf[0] != PERSIST_VERSION)
        return -1;

    param_len = get_u16(&buf[1]);

    if ((uint32_t)param_len + 5u > len)
        return -1;

    if (param_len > PERSIST_PARAM_MAX)
        return -1;

    unpack_params(&buf[3], param_len, &p->params);
    pos = (uint16_t)(3 + param_len);

    dtc_len = get_u16(&buf[pos]);
    pos = (uint16_t)(pos + 2);

    if ((uint32_t)pos + dtc_len != len)
        return -1;

    return dtc_import(p->dtcs, &buf[pos], dtc_len);
}

/* ---------------- 写频率控制 ---------------- */

/*
 * 关键状态签名。
 *
 * 不能拿整个 blob 比：dtc 的 status 里有「本循环测过没」「本次失败没」这类位，
 * 每 10ms 的 dtc_report 都会改动它们，blob 于是永远在变 —— 5 秒一写，
 * 一天两万次，Flash 撑不过一个月。
 *
 * 这里只取「值得留存」的部分：操作循环号、条数、每条的状态位里真正影响
 * 后续判定的那几位（pending / confirmed / warning）、发生次数、老化进度。
 */
static uint32_t state_signature(const persist_t *p)
{
    const dtc_store_t *st = p->dtcs;
    uint32_t h = 2166136261u;

    h = (h ^ st->cycle) * 16777619u;
    h = (h ^ st->count) * 16777619u;
    h = (h ^ p->params.threshold) * 16777619u;

    for (uint16_t i = 0; i < st->count; i++)
    {
        const dtc_entry_t *e = dtc_get(st, i);

        if (e == 0)
            continue;

        h = (h ^ e->code) * 16777619u;
        h = (h ^ (uint32_t)(e->status & (DTC_STATUS_PENDING |
                                         DTC_STATUS_CONFIRMED |
                                         DTC_STATUS_WARNING_INDICATOR))) * 16777619u;
        h = (h ^ e->occurrence) * 16777619u;
        h = (h ^ e->aging_cycles) * 16777619u;
    }

    return h;
}

static int do_save(persist_t *p, uint32_t now_ms)
{
    uint8_t buf[PERSIST_BLOB_MAX];
    int n = pack(p, buf, sizeof(buf));

    if (n < 0)
        return -1;

    if (nvm_store_save(p->nvm, buf, (uint16_t)n) != 0)
        return -1;

    memcpy(p->last, buf, (size_t)n);
    p->last_len = (uint16_t)n;
    p->last_valid = 1;
    p->last_sig = state_signature(p);
    p->last_save_ms = now_ms;
    p->writes++;

    return 1;
}

/* ---------------- 对外接口 ---------------- */

int persist_mount(persist_t *p, nvm_store_t *nvm, dtc_store_t *dtcs,
                  const persist_params_t *def, uint32_t now_ms)
{
    const uint8_t *data;
    uint16_t len = 0;
    int restored;

    if (p == 0 || nvm == 0 || dtcs == 0 || def == 0)
        return -1;

    memset(p, 0, sizeof(*p));
    p->nvm = nvm;
    p->dtcs = dtcs;
    p->params = *def;

    /*
     * nvm 必须已经 nvm_store_mount 过 —— 那一层会重置自己的状态，
     * 两者顺序反了会把刚读到的数据清掉。
     */
    data = nvm_store_data(nvm, &len);
    if (data == 0 || len == 0)
    {
        /*
         * 首次上电（或两页都坏）。
         * 立刻写一份默认值：下次上电至少是「结构正确的空状态」，
         * 而不是又一次什么都没有。
         */
        do_save(p, now_ms);
        return 0;
    }

    restored = unpack(p, data, len);
    if (restored < 0)
    {
        /*
         * 数据存在但读不懂（版本不对、结构损坏）。
         * 回默认值并重写 —— 但不丢掉 dtc 已经初始化的状态，
         * 因为这一份本来就是无法采信的。
         */
        p->params = *def;
        do_save(p, now_ms);
        return 0;
    }

    p->last_valid = 1;
    p->last_len = len;
    memcpy(p->last, data, len);
    p->last_sig = state_signature(p);
    p->last_save_ms = now_ms;

    return 0;
}

int persist_flush(persist_t *p, uint32_t now_ms)
{
    uint32_t sig;

    if (p == 0 || p->nvm == 0)
        return -1;

    sig = state_signature(p);

    if (p->last_valid && sig == p->last_sig)
        return 0;

    /* 变化确实有，但离上次写入太近，等下一轮 */
    if (p->last_valid && (uint32_t)(now_ms - p->last_save_ms) < PERSIST_MIN_INTERVAL_MS)
        return 0;

    return do_save(p, now_ms);
}

int persist_save_now(persist_t *p, uint32_t now_ms)
{
    if (p == 0 || p->nvm == 0)
        return -1;

    return do_save(p, now_ms);
}
