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
    printf("抽象层参数校验\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f;
    make_frame(&f, 0x180, 8);

    check(can_driver_init(0, can_stub_ops(), &stub, 500000) == CAN_ERR_PARAM,
          "drv 为空被拒绝");
    check(can_driver_init(&drv, 0, &stub, 500000) == CAN_ERR_PARAM,
          "ops 为空被拒绝");

    /* 未初始化时所有操作都应返回 STATE，而不是崩溃 */
    memset(&drv, 0, sizeof(drv));
    check(can_driver_send(&drv, &f) == CAN_ERR_STATE, "未初始化 send 返回 STATE");
    check(can_driver_recv(&drv, &f) == CAN_ERR_STATE, "未初始化 recv 返回 STATE");
    check(can_driver_get_status(&drv, 0) == CAN_ERR_STATE, "未初始化 get_status 返回 STATE");

    check(can_driver_init(&drv, can_stub_ops(), &stub, 500000) == CAN_OK, "初始化成功");

    /* 帧合法性 */
    make_frame(&f, 0x800, 8);
    check(can_driver_send(&drv, &f) == CAN_ERR_PARAM, "ID 0x800 越界被拒绝");

    make_frame(&f, 0x180, 9);
    check(can_driver_send(&drv, &f) == CAN_ERR_PARAM, "DLC 9 超限被拒绝");

    check(can_driver_send(&drv, 0) == CAN_ERR_PARAM, "空帧指针被拒绝");
    check(can_driver_recv(&drv, 0) == CAN_ERR_PARAM, "空接收缓冲被拒绝");

    /* 边界值应当通过 */
    make_frame(&f, 0x7FF, 8);
    check(can_driver_send(&drv, &f) == CAN_OK, "ID 0x7FF 边界值通过");
}

/* 收发往返 */
static void test_roundtrip(void)
{
    printf("收发往返\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f, got;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);

    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "空队列 recv 返回 NOMSG");

    make_frame(&f, 0x200, 8);
    check(can_driver_send(&drv, &f) == CAN_OK, "发送成功");
    check(can_stub_tx_count(&stub) == 1, "桩记录了 1 帧待发");

    check(can_stub_pop_tx(&stub, &got) == CAN_OK, "取出已发送帧");
    check(got.id == 0x200 && got.dlc == 8, "ID 与 DLC 一致");
    check(memcmp(got.data, f.data, 8) == 0, "数据场一致");

    make_frame(&f, 0x300, 4);
    check(can_stub_inject_rx(&stub, &f) == CAN_OK, "注入一帧到接收队列");
    check(can_stub_rx_count(&stub) == 1, "接收队列有 1 帧");
    check(can_driver_recv(&drv, &got) == CAN_OK, "上层收到该帧");
    check(got.id == 0x300 && got.dlc == 4, "注入帧内容一致");
    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "取完后队列为空");
}

/* 错误状态与 TEC 行为 */
static void test_error_state(void)
{
    printf("错误状态与 TEC\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f, got;
    can_bus_status_t st;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);
    make_frame(&f, 0x181, 8);

    /* 模拟进入错误被动 */
    can_stub_set_status(&stub, 130, 5, CAN_LEVEL_PASSIVE);
    check(can_driver_get_status(&drv, &st) == CAN_OK, "读取状态成功");
    check(st.tec == 130 && st.rec == 5, "TEC/REC 正确");
    check(st.level == CAN_LEVEL_PASSIVE, "级别为被动");

    /* 成功发送使 TEC 递减 */
    can_driver_send(&drv, &f);
    can_driver_get_status(&drv, &st);
    check(st.tec == 129, "成功发送后 TEC 递减 1");

    /* TEC 回落到 127 以下应恢复主动态 */
    can_stub_set_status(&stub, 128, 0, CAN_LEVEL_PASSIVE);
    can_driver_send(&drv, &f);
    can_driver_get_status(&drv, &st);
    check(st.tec == 127, "TEC 降到 127");
    check(st.level == CAN_LEVEL_ACTIVE, "TEC <= 127 恢复错误主动");

    /* 进入 Bus Off */
    can_stub_set_status(&stub, 255, 0, CAN_LEVEL_BUSOFF);
    can_driver_get_status(&drv, &st);
    check(st.level == CAN_LEVEL_BUSOFF, "级别为 Bus Off");
    check(st.busoff_count == 1, "Bus Off 计数加 1");

    check(can_driver_send(&drv, &f) == CAN_ERR_BUSOFF, "Bus Off 时发送被拒绝");

    /* Bus Off 下仍可收帧？真实控制器不能，桩保持一致的保守行为 */
    check(can_driver_recv(&drv, &got) == CAN_ERR_NOMSG, "Bus Off 下无待收帧");
}

/* 队列满 */
static void test_queue_full(void)
{
    printf("发送队列上限\n");

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

    check(sent == CAN_STUB_QUEUE_LEN, "最多缓存 16 帧");
    check(ret == CAN_ERR_FULL, "超出后返回 FULL");
}

/* 反初始化 */
static void test_deinit(void)
{
    printf("反初始化\n");

    can_stub_t stub;
    can_driver_t drv;
    can_frame_t f;

    can_driver_init(&drv, can_stub_ops(), &stub, 500000);
    make_frame(&f, 0x200, 8);

    can_driver_deinit(&drv);
    check(drv.ops == 0, "反初始化后 ops 清空");
    check(can_driver_send(&drv, &f) == CAN_ERR_STATE, "反初始化后 send 返回 STATE");
}

int main(void)
{
    printf("=== CAN 抽象层测试 ===\n\n");
    test_layer_checks();
    printf("\n");
    test_roundtrip();
    printf("\n");
    test_error_state();
    printf("\n");
    test_queue_full();
    printf("\n");
    test_deinit();

    printf("\n结果：%d 通过，%d 失败\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
