#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "dtc.h"

/*
 * 故障存储器逻辑测试。
 *
 * 这一层不碰 CAN 也不碰 UDS，所以整个生命周期可以用「喂事件」的方式
 * 在毫秒内跑完：失败 -> 待确认 -> 确认 -> 老化清除。
 *
 * 重点验的是三件容易做错的事：
 *   1. 一次失败不能直接确认（否则一个上电毛刺就变成永久故障）
 *   2. 老化必须数「真正测过且通过」的循环（没人测不能算好）
 *   3. 发生次数按沿计（按调用计会瞬间饱和）
 */

#define DTC_BUS_OFF   0xC00187u
#define DTC_PASSIVE   0xC00287u
#define DTC_STUCK     0xC00387u

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *what)
{
    if (cond)
    {
        printf("  [PASS] %s\n", what);
        passed++;
    }
    else
    {
        printf("  [FAIL] %s\n", what);
        failed++;
    }
}

static const dtc_def_t g_defs[] = {
    {DTC_BUS_OFF, "bus off"},
    {DTC_PASSIVE, "error passive"},
    {DTC_STUCK, "bus stuck dominant"},
};

static int g_snap_calls = 0;

static int snap_provider(uint32_t code, uint8_t *out, uint8_t max_len)
{
    if (max_len < 3)
        return 0;

    g_snap_calls++;
    out[0] = (uint8_t)(code >> 16);
    out[1] = 0x11;
    out[2] = 0x22;
    return 3;
}

static void setup(dtc_store_t *st, uint8_t confirm, uint8_t aging)
{
    dtc_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.defs = g_defs;
    cfg.def_count = (uint16_t)(sizeof(g_defs) / sizeof(g_defs[0]));
    cfg.confirm_cycles = confirm;
    cfg.aging_cycles = aging;

    dtc_init(st, &cfg);
    g_snap_calls = 0;
}

/* 「一个循环里测过且通过」：先报通过，再切循环 */
static void passing_cycle(dtc_store_t *st, uint32_t now)
{
    dtc_report(st, DTC_BUS_OFF, 0, now);
    dtc_report(st, DTC_PASSIVE, 0, now);
    dtc_report(st, DTC_STUCK, 0, now);
    dtc_operation_cycle(st);
}

static void test_init(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;

    printf("[init]\n");
    setup(&st, 2, 3);

    check(st.count == 3, "all defined DTCs pre-registered");
    e = dtc_find(&st, DTC_BUS_OFF);
    check(e != 0, "find by code works");
    check(e->status == (DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR |
                        DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE),
          "fresh entry is 'not tested' on both levels");
    check(e->occurrence == 0, "no occurrence before first failure");
    check(dtc_find(&st, 0x123456u) == 0, "undefined code returns NULL");
    check(dtc_get(&st, 3) == 0, "index out of range returns NULL");
}

static void test_single_failure_is_pending_only(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;

    printf("[pending vs confirmed]\n");
    setup(&st, 2, 3);

    dtc_report(&st, DTC_BUS_OFF, 1, 1000);
    e = dtc_find(&st, DTC_BUS_OFF);

    check((e->status & DTC_STATUS_TEST_FAILED) != 0, "testFailed set");
    check((e->status & DTC_STATUS_PENDING) != 0, "pendingDTC set on first failure");
    check((e->status & DTC_STATUS_CONFIRMED) == 0,
          "one failure is not enough to confirm");
    check((e->status & DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR) == 0,
          "failed entry is no longer 'not tested'");
    check(e->occurrence == 1, "occurrence counted once");
    check(e->first_ms == 1000 && e->last_ms == 1000, "timestamps recorded");

    /* 同一个循环里再怎么失败，都只算一次「循环内失败」 */
    dtc_report(&st, DTC_BUS_OFF, 1, 1100);
    dtc_report(&st, DTC_BUS_OFF, 1, 1200);
    check(e->occurrence == 1,
          "repeated failures in one cycle keep occurrence at 1 (edge triggered)");
    check((e->status & DTC_STATUS_CONFIRMED) == 0,
          "repeated failures in the same cycle do not confirm");

    /* 通过一次：本次失败的位清掉，但「本循环失败过」要留到循环结束 */
    dtc_report(&st, DTC_BUS_OFF, 0, 1300);
    check((e->status & DTC_STATUS_TEST_FAILED) == 0, "pass clears testFailed");
    check((e->status & DTC_STATUS_TEST_FAILED_THIS_CYCLE) != 0,
          "pass keeps testFailedThisOperationCycle");
    check((e->status & DTC_STATUS_PENDING) != 0, "pass alone does not clear pending");
    check((e->status & DTC_STATUS_CONFIRMED) == 0, "pass keeps it unconfirmed");

    /* 进入下一个循环后再失败 -> 这是第二个循环，升为确认 */
    dtc_operation_cycle(&st);
    dtc_report(&st, DTC_BUS_OFF, 1, 2000);

    check((e->status & DTC_STATUS_CONFIRMED) != 0,
          "failure in a second operation cycle confirms the DTC");
    check((e->status & DTC_STATUS_WARNING_INDICATOR) != 0,
          "confirmed DTC requests the warning indicator");
    check(e->occurrence == 2, "occurrence counted in the second cycle");
    check(e->first_ms == 1000 && e->last_ms == 2000, "first/last timestamps span cycles");
}

static void test_aging(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;
    uint32_t now = 10000;

    printf("[aging]\n");
    setup(&st, 2, 3);
    e = dtc_find(&st, DTC_PASSIVE);

    /* 两个循环都失败，确认下来 */
    dtc_report(&st, DTC_PASSIVE, 1, now);
    dtc_operation_cycle(&st);
    dtc_report(&st, DTC_PASSIVE, 1, now);
    check((e->status & DTC_STATUS_CONFIRMED) != 0, "precondition: DTC confirmed");

    /*
     * 确认所在的那个循环本身算「有故障」，切循环时不推进老化，
     * 所以要有一拍收尾 —— 老化是从下一个干净循环才开始数的。
     */
    passing_cycle(&st, now);
    check((e->status & DTC_STATUS_CONFIRMED) != 0,
          "cycle that confirmed the DTC does not advance aging");

    passing_cycle(&st, now);
    check((e->status & DTC_STATUS_CONFIRMED) != 0, "confirmed survives 1 clean cycle");
    passing_cycle(&st, now);
    check((e->status & DTC_STATUS_CONFIRMED) != 0, "confirmed survives 2 clean cycles");

    passing_cycle(&st, now);
    check((e->status & DTC_STATUS_CONFIRMED) == 0,
          "confirmed aged out after 3 clean cycles");
    check((e->status & DTC_STATUS_PENDING) == 0, "aging also clears pending");
    check((e->status & DTC_STATUS_WARNING_INDICATOR) == 0,
          "aging clears the warning indicator");

    /*
     * 没测过的循环不算「好」。
     *
     * 这一条是老化最容易写错的地方：如果直接把每个循环都当成通过，
     * 一个传感器掉线（再也没人调用 report）的故障会在 40 个循环后
     * 自己消失，售后永远查不到。
     */
    setup(&st, 2, 2);
    e = dtc_find(&st, DTC_STUCK);
    dtc_report(&st, DTC_STUCK, 1, now);
    dtc_operation_cycle(&st);
    dtc_report(&st, DTC_STUCK, 1, now);
    check((e->status & DTC_STATUS_CONFIRMED) != 0, "precondition: second DTC confirmed");

    for (int i = 0; i < 5; i++)
        dtc_operation_cycle(&st); /* 只切循环，从不调用 report */

    check((e->status & DTC_STATUS_CONFIRMED) != 0,
          "silent cycles (test never ran) do not age the DTC out");
}

static void test_occurrence_saturation(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;

    printf("[occurrence counter]\n");
    setup(&st, 2, 40);
    e = dtc_find(&st, DTC_BUS_OFF);

    for (int i = 0; i < 300; i++)
    {
        dtc_report(&st, DTC_BUS_OFF, 1, (uint32_t)i);
        dtc_report(&st, DTC_BUS_OFF, 0, (uint32_t)i);
    }

    check(e->occurrence == 255, "occurrence saturates at 255 instead of wrapping to 0");
}

static void test_snapshot(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;

    printf("[snapshot]\n");
    setup(&st, 2, 3);
    dtc_set_snapshot_provider(&st, snap_provider);
    e = dtc_find(&st, DTC_BUS_OFF);

    check(g_snap_calls == 0, "provider not called before any failure");

    dtc_report(&st, DTC_BUS_OFF, 1, 500);
    check(g_snap_calls == 1, "provider called on first failure");
    check(e->has_snapshot == 1, "snapshot stored");
    check(e->snapshot_len == 3 && e->snapshot[1] == 0x11 && e->snapshot[2] == 0x22,
          "snapshot content matches provider output");

    dtc_report(&st, DTC_BUS_OFF, 0, 600);
    dtc_report(&st, DTC_BUS_OFF, 1, 700);
    check(g_snap_calls == 1, "snapshot not recaptured on later failures");

    /* 清码后快照也要消失，否则下次读到的还是旧故障的工况 */
    dtc_clear(&st, DTC_GROUP_ALL);
    check(e->has_snapshot == 0 && e->snapshot_len == 0, "clear removes the snapshot");
}

static void test_clear(void)
{
    dtc_store_t st;
    const dtc_entry_t *e;

    printf("[clear]\n");
    setup(&st, 2, 3);

    dtc_report(&st, DTC_BUS_OFF, 1, 100);
    dtc_report(&st, DTC_PASSIVE, 1, 100);
    check(dtc_count_by_mask(&st, DTC_STATUS_TEST_FAILED) == 2, "two DTCs failed");

    check(dtc_clear(&st, DTC_BUS_OFF) == 1, "clearing one code returns 1");
    e = dtc_find(&st, DTC_BUS_OFF);
    check(e->status == (DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR |
                        DTC_STATUS_TEST_NOT_COMPLETED_THIS_CYCLE),
          "cleared entry goes back to 'not tested'");
    check(e->occurrence == 0 && e->first_ms == 0 && e->last_ms == 0,
          "clear resets counters and timestamps");
    check(dtc_count_by_mask(&st, DTC_STATUS_TEST_FAILED) == 1,
          "other DTC untouched when clearing one code");

    check(dtc_clear(&st, DTC_GROUP_ALL) == 3, "group clear covers every entry");
    check(dtc_count_by_mask(&st, 0x0F) == 0,
          "no failure/confirm bits left after group clear");

    /* 清完之后再失败，仍然是「首次」，不能被历史计数带偏 */
    dtc_report(&st, DTC_BUS_OFF, 1, 900);
    e = dtc_find(&st, DTC_BUS_OFF);
    check(e->occurrence == 1, "post-clear failure counts from 1 again");
    check((e->status & DTC_STATUS_CONFIRMED) == 0,
          "post-clear failure needs a second cycle again");
}

static void test_mask_count(void)
{
    dtc_store_t st;

    printf("[status mask]\n");
    setup(&st, 2, 3);

    /* 刚初始化：三条都是「未测过」，用 0x08 数不到任何一条 */
    check(dtc_count_by_mask(&st, DTC_STATUS_CONFIRMED) == 0,
          "no confirmed DTC before any failure");
    check(dtc_count_by_mask(&st, DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR) == 3,
          "all three are 'not tested since clear'");

    dtc_report(&st, DTC_BUS_OFF, 1, 10);
    dtc_operation_cycle(&st);
    dtc_report(&st, DTC_BUS_OFF, 1, 20);

    check(dtc_count_by_mask(&st, DTC_STATUS_CONFIRMED) == 1,
          "confirmed mask counts exactly the confirmed one");
    check(dtc_count_by_mask(&st, DTC_STATUS_PENDING) == 1,
          "pending mask counts it too");
    check(dtc_count_by_mask(&st, 0x00) == 0, "zero mask counts nothing");
}

static void test_unknown_code(void)
{
    dtc_store_t st;

    printf("[unknown code]\n");
    setup(&st, 2, 3);

    check(dtc_report(&st, 0xABCDEFu, 1, 0) == -1,
          "reporting an undefined DTC is rejected");
    check(st.count == 3, "store size unchanged after a rejected report");
}

int main(void)
{
    printf("=== DTC store test ===\n\n");

    test_init();
    test_single_failure_is_pending_only();
    test_aging();
    test_occurrence_saturation();
    test_snapshot();
    test_clear();
    test_mask_count();
    test_unknown_code();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
