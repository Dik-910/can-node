#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/app/node_health.h"

/*
 * 总线健康管理测试。
 *
 * 重点验证状态优先级：Bus Off 期间即使 TEC 已经回落，也不能判定为正常——
 * 总线还没恢复，这时候发报文就是往死线上灌数据。
 */

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

static can_bus_status_t mk_status(uint8_t tec, uint8_t rec, can_level_t level)
{
    can_bus_status_t s;
    memset(&s, 0, sizeof(s));
    s.tec = tec;
    s.rec = rec;
    s.level = level;
    return s;
}

/* 正常 / 降级 / 恢复的完整状态流转 */
static void test_state_flow(void)
{
    node_health_t h;
    can_bus_status_t st;

    printf("State flow\n");
    node_health_init(&h);

    check(h.state == HEALTH_NORMAL, "starts normal");
    check(node_health_can_send(&h) == 1, "can send when normal");
    check(node_health_period_scale(&h) == 1, "period scale 1 when normal");

    /* 持续正常 */
    st = mk_status(0, 0, CAN_LEVEL_ACTIVE);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_NORMAL, "stays normal under healthy bus");

    /* 错误被动 -> 降级 */
    st = mk_status(130, 20, CAN_LEVEL_PASSIVE);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_DEGRADED, "tec >= 128 enters degraded");
    check(node_health_period_scale(&h) == 2, "period doubled when degraded");
    check(node_health_can_send(&h) == 1, "still allowed to send when degraded");

    /* 回落 -> 正常 */
    st = mk_status(40, 0, CAN_LEVEL_ACTIVE);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_NORMAL, "recovers to normal when tec drops");
    check(node_health_period_scale(&h) == 1, "period back to nominal");
}

/* Bus Off 与恢复观察期 */
static void test_busoff_recover(void)
{
    node_health_t h;
    can_bus_status_t st;

    printf("\nBus off and recovery\n");
    node_health_init(&h);

    st = mk_status(255, 0, CAN_LEVEL_BUSOFF);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_BUSOFF, "bus off detected");
    check(node_health_can_send(&h) == 0, "sending blocked while bus off");
    check(node_health_period_scale(&h) == 0, "period scale 0 while bus off");
    check(h.busoff_total == 1, "bus off counted once");

    /* 同一个 Bus Off 状态持续，不应重复计数 */
    node_health_update(&h, &st, 100);
    check(h.busoff_total == 1, "still one count while staying bus off");

    /*
     * 关键用例：Bus Off 期间 TEC 已回落，但 level 仍是 BUSOFF。
     * 此时必须保持 BUSOFF，不能因为 TEC 低就判成 NORMAL。
     */
    st = mk_status(0, 0, CAN_LEVEL_BUSOFF);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_BUSOFF, "low tec does not override bus off");

    /* 总线恢复 -> 观察期 */
    st = mk_status(0, 0, CAN_LEVEL_ACTIVE);
    node_health_update(&h, &st, 100);
    check(h.state == HEALTH_RECOVERING, "enters recovering after bus off clears");
    check(node_health_can_send(&h) == 1, "sending allowed during recovery");

    /* 观察期未过，仍为 RECOVERING */
    node_health_update(&h, &st, HEALTH_RECOVER_MS - 500);
    check(h.state == HEALTH_RECOVERING, "still recovering before window ends");

    /* 观察期结束 -> 正常 */
    node_health_update(&h, &st, 600);
    check(h.state == HEALTH_NORMAL, "back to normal after recovery window");
}

/* 反复 Bus Off：判定为硬件问题，长期降频 */
static void test_repeated_busoff(void)
{
    node_health_t h;
    can_bus_status_t on;
    can_bus_status_t off;

    printf("\nRepeated bus off\n");
    node_health_init(&h);

    on = mk_status(255, 0, CAN_LEVEL_BUSOFF);
    off = mk_status(0, 0, CAN_LEVEL_ACTIVE);

    for (int i = 0; i < HEALTH_BUSOFF_LIMIT; i++)
    {
        node_health_update(&h, &on, 100);
        node_health_update(&h, &off, 100);
    }

    check(h.busoff_total == HEALTH_BUSOFF_LIMIT, "all occurrences counted");
    check(h.long_degraded == 1, "flagged as long-term degraded");
    check(node_health_period_scale(&h) == 2, "period doubled permanently");

    /* 长期降级下即使总线看起来正常，也不回满速 */
    node_health_update(&h, &off, 5000);
    check(node_health_period_scale(&h) == 2, "stays degraded while long_degraded set");
}

/* 统计窗口滚动后计数清零，不会因为历史故障永久降级 */
static void test_window_rollover(void)
{
    node_health_t h;
    can_bus_status_t on;
    can_bus_status_t off;

    printf("\nWindow rollover\n");
    node_health_init(&h);

    on = mk_status(255, 0, CAN_LEVEL_BUSOFF);
    off = mk_status(0, 0, CAN_LEVEL_ACTIVE);

    /* 两次 Bus Off，还没到上限 */
    node_health_update(&h, &on, 100);
    node_health_update(&h, &off, 100);
    node_health_update(&h, &on, 100);
    node_health_update(&h, &off, 100);
    check(h.busoff_in_window == 2, "two occurrences in window");
    check(h.long_degraded == 0, "not degraded yet");

    /* 熬过整个统计窗口，计数应清零 */
    node_health_update(&h, &off, HEALTH_WINDOW_MS + 1000);
    check(h.busoff_in_window == 0, "window counter reset after one period");

    /* 再发生一次不应触发长期降级 */
    node_health_update(&h, &on, 100);
    check(h.long_degraded == 0, "single new occurrence does not trigger degraded");
}

int main(void)
{
    printf("=== Node health test ===\n\n");
    test_state_flow();
    test_busoff_recover();
    test_repeated_busoff();
    test_window_rollover();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
