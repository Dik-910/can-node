/*
 * persist_test.c —— 参数与故障存储的跨复位行为
 *
 * 这是「老化 40 个循环」这条设计真正成立的地方。
 * 之前的实现里，故障存储在内存中，每次上电 aging_cycles 从 0 开始数，
 * 40 个循环永远到不了 —— 一条确认下来的故障会永久留在 ECU 里。
 *
 * 所以这里的核心用例是：
 *   每次「上电」都重新 mount 一次，反复 40 次，看故障是否自己消失。
 */
#include <stdio.h>
#include <string.h>

#include "dtc.h"
#include "mem_nvm.h"
#include "nvm_store.h"
#include "persist.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what, long got, long want)
{
    g_total++;
    if (cond)
    {
        printf("  [PASS] %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  [FAIL] %s (got %ld, want %ld)\n", what, got, want);
    }
}

#define AREA_SIZE 2048u

#define DTC_A 0xC00187u
#define DTC_B 0xC00287u

static const dtc_def_t g_defs[] =
{
    {DTC_A, "can bus off"},
    {DTC_B, "can error passive"},
};

static const persist_params_t DEF_PARAMS = {50u};

/* 一个「板子」：参数区 + 故障存储器 + 持久化状态 */
typedef struct
{
    mem_nvm_t mem;
    nvm_store_t nvm;
    dtc_store_t dtcs;
    persist_t persist;
} board_t;

/*
 * 上电。每次都从擦除态以外的 NVM 内容恢复，等价于重新加电。
 * cycle 参数决定推进多少个操作循环，用来模拟「跑了很多次上电」。
 */
static void power_on(board_t *b, uint32_t now_ms)
{
    dtc_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.defs = g_defs;
    cfg.def_count = (uint16_t)(sizeof(g_defs) / sizeof(g_defs[0]));

    memset(&b->dtcs, 0, sizeof(b->dtcs));
    dtc_init(&b->dtcs, &cfg);

    /*
     * 走 persist_boot 而不是手动拼 mount 序列 —— 与 ecu_server / node_main
     * 的实际启动路径保持一致。曾经因为「先取 dev 再挂载」写错，
     * ECU 一启动就跳 NULL，而那时没有任何测试覆盖这条序列。
     */
    memset(&b->nvm, 0, sizeof(b->nvm));
    memset(&b->persist, 0, sizeof(b->persist));
    /* persist_boot 内部会在恢复之后推进操作循环，这里不要再调一次 */
    persist_boot(&b->persist, &b->nvm, mem_nvm_dev(&b->mem), 0,
                 &b->dtcs, &DEF_PARAMS, now_ms);
}

/* ---- 首次上电 ---- */

static void test_first_boot(void)
{
    board_t b;

    printf("首次上电写默认值\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    check(b.persist.params.threshold == DEF_PARAMS.threshold,
          "参数取默认值", b.persist.params.threshold, DEF_PARAMS.threshold);
    check(b.nvm.valid == 1, "首次上电应写入一份", b.nvm.valid, 1);
    check(b.nvm.saves == 1, "写入 1 次", b.nvm.saves, 1);
}

/* ---- 参数跨复位 ---- */

static void test_param_survives(void)
{
    board_t b;

    printf("参数跨复位保持\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    b.persist.params.threshold = 61u;
    check(persist_save_now(&b.persist, 100) == 1, "强制落盘成功", 1, 1);

    /* 重新上电 */
    power_on(&b, 1000);
    check(b.persist.params.threshold == 61u, "读回写入的阈值",
          b.persist.params.threshold, 61);

    /* 再上电一次，确认不是偶然 */
    power_on(&b, 2000);
    check(b.persist.params.threshold == 61u, "二次上电仍保持",
          b.persist.params.threshold, 61);
}

/* ---- 故障跨复位 ---- */

static void test_dtc_survives(void)
{
    board_t b;

    printf("故障跨复位保持\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    /* 第 1 个循环失败 -> pending */
    dtc_report(&b.dtcs, DTC_A, 1, 10);
    persist_save_now(&b.persist, 10);

    power_on(&b, 100);
    check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_PENDING) == 1,
          "重启后 pending 仍在", dtc_count_by_mask(&b.dtcs, DTC_STATUS_PENDING), 1);

    /*
     * 第 2 个循环再失败 -> confirmed。
     * 关键在于 fail_cycles 是跨复位累加的：如果重启把它清零，
     * 这里只会停在 pending。
     */
    dtc_report(&b.dtcs, DTC_A, 1, 110);
    check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 1,
          "第二个循环后确认", dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED), 1);

    persist_save_now(&b.persist, 110);

    power_on(&b, 200);
    check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 1,
          "重启后 confirmed 仍在", dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED), 1);

    /* 发生次数与首次时间也要活下来 */
    {
        dtc_info_t info;

        check(dtc_get_info(&b.dtcs, DTC_A, &info) == 0, "能取到条目", 0, 0);
        check(info.occurrence == 2, "发生次数跨复位累计", info.occurrence, 2);
        check(info.first_ms == 10, "首次发生时刻保持", (long)info.first_ms, 10);
    }
}

/* ---- 老化跨复位（核心用例） ---- */

static void test_aging_across_reboots(void)
{
    board_t b;
    int i;
    int cleared_at = -1;

    printf("老化跨复位推进\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    /* 制造一条 confirmed。第一次失败也要落盘，否则重启后 fail_cycles 归零 */
    dtc_report(&b.dtcs, DTC_A, 1, 10);
    persist_save_now(&b.persist, 10);
    power_on(&b, 100);
    dtc_report(&b.dtcs, DTC_A, 1, 110);
    persist_save_now(&b.persist, 110);

    check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 1,
          "起点是一条 confirmed", dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED), 1);

    /*
     * 之后每次上电都正常（不报失败），看它什么时候自己被清掉。
     * 默认 aging_cycles = 40。
     */
    for (i = 0; i < 60; i++)
    {
        power_on(&b, (uint32_t)(1000 + i * 100));
        dtc_report(&b.dtcs, DTC_A, 0, (uint32_t)(1000 + i * 100));
        persist_save_now(&b.persist, (uint32_t)(1000 + i * 100));

        if (dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 0)
        {
            cleared_at = i;
            break;
        }
    }

    check(cleared_at > 0, "故障应在若干次上电后自动老化清除", cleared_at, 1);
    check(cleared_at != -1 && cleared_at <= 45,
          "老化发生在 40 个循环附近", cleared_at, 40);
}

/* ---- 静默上电不推进老化 ---- */

static void test_silent_boots_do_not_age(void)
{
    board_t b;
    int i;

    printf("没上报测试结果的上电不推进老化\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    dtc_report(&b.dtcs, DTC_A, 1, 10);
    persist_save_now(&b.persist, 10);
    power_on(&b, 100);
    dtc_report(&b.dtcs, DTC_A, 1, 110);
    persist_save_now(&b.persist, 110);

    /*
     * 之后每次上电都**不调用 dtc_report**，等价于传感器掉线。
     * 这种情况不能算「测过且通过」，否则故障会自己消失、售后查不到根因。
     */
    for (i = 0; i < 50; i++)
    {
        power_on(&b, (uint32_t)(1000 + i * 100));

        /*
         * 每次都要保存：这样存下来的是「本循环没测过」（bit6 置位）的状态，
         * 下一次上电结算时 aging 才会归零。
         * 不保存的话测的就只是「回到了同一份旧数据」，与 bit6 无关。
         */
        persist_save_now(&b.persist, (uint32_t)(1000 + i * 100));
    }

    check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 1,
          "静默上电不应清掉故障", dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED), 1);
}

/* ---- 写频率 ---- */

static void test_write_rate(void)
{
    board_t b;
    uint32_t before;
    int i;

    printf("无变化时不重复写\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    dtc_report(&b.dtcs, DTC_A, 1, 10);
    persist_flush(&b.persist, 5000);

    before = b.nvm.saves;

    /* 反复调用 flush 但状态没有实质变化，不该产生写入 */
    for (i = 0; i < 100; i++)
    {
        dtc_report(&b.dtcs, DTC_A, 1, (uint32_t)(5100 + i * 10));
        persist_flush(&b.persist, (uint32_t)(5100 + i * 10));
    }

    check(b.nvm.saves == before, "重复上报同一故障不应反复落盘",
          b.nvm.saves, before);

    /*
     * 发生次数变了（中间恢复过一次）就该写 —— 这是有信息量的变化。
     */
    dtc_report(&b.dtcs, DTC_A, 0, 20000);
    dtc_report(&b.dtcs, DTC_A, 1, 20010);
    persist_flush(&b.persist, 30000);

    check(b.nvm.saves > before, "新的一次失败应落盘", b.nvm.saves > before, 1);
}

/* ---- 掉电写入的数据不可信时回默认 ---- */

static void test_corrupt_data_uses_defaults(void)
{
    board_t b;

    printf("数据不可读时回默认值\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    b.persist.params.threshold = 61u;
    persist_save_now(&b.persist, 100);

    /* 把两页负载都改坏 */
    b.mem.area[NVM_HEADER_SIZE + 2] ^= 0xFF;
    b.mem.area[AREA_SIZE / 2 + NVM_HEADER_SIZE + 2] ^= 0xFF;

    power_on(&b, 200);

    check(b.persist.params.threshold == DEF_PARAMS.threshold,
          "坏数据应回默认值", b.persist.params.threshold, DEF_PARAMS.threshold);
    check(b.nvm.valid == 1, "回默认后应立即写一份可用数据", b.nvm.valid, 1);
}

/* ---- 版本不符 ---- */

static void test_bad_version(void)
{
    board_t b;
    uint8_t blob[8];

    printf("版本不符时回默认值\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);
    b.persist.params.threshold = 70u;
    persist_save_now(&b.persist, 100);

    /*
     * 写入一份「页校验正确、但应用层版本不认识」的数据。
     * 改字节会把页 CRC 一起弄坏，那样测到的是 nvm 层的回退，
     * 不是应用层的版本判断 —— 两条路径要分开验。
     */
    memset(blob, 0, sizeof(blob));
    blob[0] = 0x7F; /* 未来版本的标记 */
    check(nvm_store_save(&b.nvm, blob, sizeof(blob)) == 0, "写入伪版本数据", 0, 0);

    power_on(&b, 200);

    check(b.persist.params.threshold == DEF_PARAMS.threshold,
          "版本不认识则回默认", b.persist.params.threshold, DEF_PARAMS.threshold);
}

/* ---- 循环号必须跨复位递增 ---- */

/*
 * 这一条盯的是「恢复与推进的先后顺序」。
 *
 * 如果实现里先 dtc_operation_cycle 再 import（或者调用方自己又推一次），
 * import 会用存储里的旧值覆盖刚加的那一次，cycle 永远停在 1 ——
 * 而「读码能读到」之类的断言全都仍然通过。CI 里就是这么漏过去的。
 */
static void test_cycle_advances(void)
{
    board_t b;
    uint32_t c1, c2, c3;

    printf("操作循环号跨复位递增\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);
    c1 = b.dtcs.cycle;
    check(c1 == 1, "首次上电 cycle=1", (long)c1, 1);

    /* 必须落盘，否则下次读到的是空的，测的就不是「跨复位」了 */
    persist_save_now(&b.persist, 0);

    power_on(&b, 100);
    c2 = b.dtcs.cycle;
    check(c2 == c1 + 1, "重启后 cycle 递增", (long)c2, (long)(c1 + 1));
    persist_save_now(&b.persist, 100);

    power_on(&b, 200);
    c3 = b.dtcs.cycle;
    check(c3 == c2 + 1, "再次重启仍递增", (long)c3, (long)(c2 + 1));
}

/* ---- 引导序列本身 ---- */

static void test_boot_reset(void)
{
    board_t b;

    printf("reset 启动等价于新板子\n");

    mem_nvm_init(&b.mem, AREA_SIZE);
    power_on(&b, 0);

    b.persist.params.threshold = 61u;
    persist_save_now(&b.persist, 100);

    /* 不 reset 时能读到 61，说明上面确实落下去了 */
    power_on(&b, 200);
    check(b.persist.params.threshold == 61u, "reset 之前是 61",
          b.persist.params.threshold, 61);

    /* 用 reset 启动：参数区被擦掉，回到默认 */
    {
        dtc_config_t cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.defs = g_defs;
        cfg.def_count = (uint16_t)(sizeof(g_defs) / sizeof(g_defs[0]));

        memset(&b.dtcs, 0, sizeof(b.dtcs));
        dtc_init(&b.dtcs, &cfg);
        memset(&b.nvm, 0, sizeof(b.nvm));
        memset(&b.persist, 0, sizeof(b.persist));

        check(persist_boot(&b.persist, &b.nvm, mem_nvm_dev(&b.mem), 1,
                           &b.dtcs, &DEF_PARAMS, 400) == 0, "reset 启动成功", 0, 0);
        check(b.persist.params.threshold == DEF_PARAMS.threshold,
              "reset 后回到默认值", b.persist.params.threshold, DEF_PARAMS.threshold);
        /*
         * count 不会是 0：dtc_init 会为定义表里每条码建一条空记录，
         * 否则 0x19 0A（报支持的码）在无故障时返回空列表。
         * 要断言的是「没有确认的故障」。
         */
        check(dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED) == 0,
              "reset 后没有确认的故障", dtc_count_by_mask(&b.dtcs, DTC_STATUS_CONFIRMED), 0);
        check(b.dtcs.count == 2, "空记录数等于定义表条数", b.dtcs.count, 2);
    }
}

static void test_boot_rejects_bad_device(void)
{
    persist_t p;
    nvm_store_t nvm;
    dtc_store_t dtcs;
    dtc_config_t cfg;

    printf("设备接口不完整时拒绝启动\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.defs = g_defs;
    cfg.def_count = (uint16_t)(sizeof(g_defs) / sizeof(g_defs[0]));
    memset(&dtcs, 0, sizeof(dtcs));
    dtc_init(&dtcs, &cfg);

    check(persist_boot(&p, &nvm, 0, 0, &dtcs, &DEF_PARAMS, 0) != 0,
          "空设备应拒绝", 1, 0);

    {
        /* 只有 size、没有函数表的设备同样要拒绝，而不是等运行时跳 NULL */
        static nvm_dev_t hollow;

        memset(&hollow, 0, sizeof(hollow));
        hollow.size = 2048;

        check(persist_boot(&p, &nvm, &hollow, 0, &dtcs, &DEF_PARAMS, 0) != 0,
              "函数表为空应拒绝", 1, 0);
    }
}

int main(void)
{
    printf("=== persist_test ===\n");

    test_first_boot();
    test_param_survives();
    test_dtc_survives();
    test_aging_across_reboots();
    test_silent_boots_do_not_age();
    test_write_rate();
    test_corrupt_data_uses_defaults();
    test_bad_version();
    test_cycle_advances();
    test_boot_reset();
    test_boot_rejects_bad_device();

    printf("\nResult: %d passed, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
