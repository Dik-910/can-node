#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_stub.h"

/*
 * CAN 抽象层 + 桩驱动测试。
 *
 * 覆盖两类东西：
 *   1. 抽象层的参数校验与状态检查（不依赖任何平台）
 *   2. 桩驱动的收发与错误状态行为（模拟真实控制器的 TEC 递减、Bus Off 拒发）
 *
 * 这些用例的意义在于：等 STM32 硬件到了，只要新的 ops 实现能跑通同一套用例，
 * 就说明平台适配没引入行为差异。
 *
 * 测试输出一律英文：中文常量在不同终端 locale 与嵌入式工具链下容易编码错乱。
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

static void make_frame(can_frame_t *f, uint32_t id, uint8_t dlc)
{
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->dlc = dlc;
    for (uint8_t i = 0; i < dlc; i++)
        f->data[i] = (uint8_t)(0xA0 + i);
}

/* 抽象层：参数与状态校验 */
static void test_layer_checks(void)
{
    printf("Layer parameter checks\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f;
    make_frame(&f, 0x180, 8);

    check(can_driver_init(0, can_stub_ops(), &stub, 500000) == CAN_ERR_PARAM,
          "null driver rejected");
    check(can_driver_init(&drv, 0, &stub, 500000) == CAN_ERR_PARAM,
          "null ops rejected");

    /* 未初始化时所有操作都应返回 STATE，而不是崩溃 */
    memset(&drv, 0, sizeof(drv));
    check(can_driver_send(&drv, &f) == CAN_ERR_STATE, "send before init returns STATE");
    check(can_driver_recv(&drv, &f) == CAN_ERR_STATE, "recv before init returns STATE");
    check(can_driver_get_status(&drv, 0) == CAN_ERR_STATE, "get_status before init returns STATE");

    check(can_driver_init(&drv, can_stub_ops(), &stub, 500000) == CAN_OK, "init succeeds");

    /* 帧合法性 */
    make_frame(&f, 0x800, 8);
    check(can_driver_send(&drv, &f) == CAN_ERR_PARAM, "id 0x800 out of range rejected");

    make_frame(&f, 0x180, 9);
    check(can_driver_send(&drv, &f) == CAN_ERR_PARAM, "dlc 9 rejected");

    check(can_driver_send(&drv, 0) == CAN_ERR_PARAM, "null frame rejected");
    check(can_driver_recv(&drv, 0) == CAN_ERR_PARAM, "null rx buffer rejected");

    /* 边界值应当通过 */
    make_frame(&f, 0x7FF, 8);
    check(can_driver_send(&drv, &f) == CAN_OK, "id 0x7FF boundary accepted");
}

/* 收发往返 */
static void test_roundtrip(void)
{
    printf("Send / receive roundtrip\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f, got;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);

    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "recv on empty queue returns NOMSG");

    make_frame(&f, 0x200, 8);
    check(can_driver_send(&drv, &f) == CAN_OK, "send succeeds");
    check(can_stub_tx_count(&stub) == 1, "stub records one pending frame");

    check(can_stub_pop_tx(&stub, &got) == CAN_OK, "pop the sent frame");
    check(got.id == 0x200 && got.dlc == 8, "id and dlc match");
    check(memcmp(got.data, f.data, 8) == 0, "payload matches");

    make_frame(&f, 0x300, 4);
    check(can_stub_inject_rx(&stub, &f) == CAN_OK, "inject one rx frame");
    check(can_stub_rx_count(&stub) == 1, "rx queue holds one frame");
    check(can_driver_recv(&drv, &got) == CAN_OK, "caller receives it");
    check(got.id == 0x300 && got.dlc == 4, "injected frame matches");
    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "queue empty afterwards");
}

/* 错误状态与 TEC 行为 */
static void test_error_state(void)
{
    printf("Error state and TEC behaviour\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f, got;
    can_bus_status_t st;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);
    make_frame(&f, 0x181, 8);

    /* 模拟进入错误被动 */
    can_stub_set_status(&stub, 130, 5, CAN_LEVEL_PASSIVE);
    check(can_driver_get_status(&drv, &st) == CAN_OK, "read status succeeds");
    check(st.tec == 130 && st.rec == 5, "tec / rec match");
    check(st.level == CAN_LEVEL_PASSIVE, "level is passive");

    /* 成功发送使 TEC 递减 */
    can_driver_send(&drv, &f);
    can_driver_get_status(&drv, &st);
    check(st.tec == 129, "successful send decrements tec");

    /* TEC 回落到 127 以下应恢复主动态 */
    can_stub_set_status(&stub, 128, 0, CAN_LEVEL_PASSIVE);
    can_driver_send(&drv, &f);
    can_driver_get_status(&drv, &st);
    check(st.tec == 127, "tec drops to 127");
    check(st.level == CAN_LEVEL_ACTIVE, "tec <= 127 restores error active");

    /* 进入 Bus Off */
    can_stub_set_status(&stub, 255, 0, CAN_LEVEL_BUSOFF);
    can_driver_get_status(&drv, &st);
    check(st.level == CAN_LEVEL_BUSOFF, "level is bus off");
    check(st.busoff_count == 1, "bus-off counter incremented");

    check(can_driver_send(&drv, &f) == CAN_ERR_BUSOFF, "send rejected while bus off");

    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "no pending rx while bus off");
}

/* 队列满 */
static void test_queue_full(void)
{
    printf("Tx queue limit\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f;
    int ret = CAN_OK;
    int sent = 0;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);
    make_frame(&f, 0x200, 8);

    for (int i = 0; i < CAN_STUB_QUEUE_LEN + 2; i++)
    {
        ret = can_driver_send(&drv, &f);
        if (ret == CAN_OK)
            sent++;
    }

    check(sent == CAN_STUB_QUEUE_LEN, "queue holds at most 16 frames");
    check(ret == CAN_ERR_FULL, "returns FULL when exceeded");
}

/* 反初始化 */
static void test_deinit(void)
{
    printf("Deinit\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);
    make_frame(&f, 0x200, 8);

    can_driver_deinit(&drv);
    check(drv.ops == 0, "ops cleared after deinit");
    check(can_driver_send(&drv, &f) == CAN_ERR_STATE, "send after deinit returns STATE");
}

int main(void)
{
    printf("=== CAN abstraction layer test ===\n\n");
    test_layer_checks();
    printf("\n");
    test_roundtrip();
    printf("\n");
    test_error_state();
    printf("\n");
    test_queue_full();
    printf("\n");
    test_deinit();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
