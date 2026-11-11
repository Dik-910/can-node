#ifndef __CAN_DRIVER_STUB_H
#define __CAN_DRIVER_STUB_H

#include "can_driver.h"

/*
 * PC 侧桩驱动
 *
 * 用内存队列模拟收发，用于在没有硬件的情况下跑通上层逻辑
 * （ISO-TP 分包、UDS 服务、状态上报）。
 *
 * 它刻意模拟了两条真实硬件行为，否则上层的错误处理逻辑测不出来：
 *   1. 处于 Bus Off 时发送失败，返回 CAN_ERR_BUSOFF
 *   2. 每次成功发送 TEC 递减 1（真实控制器就是这样做的）
 */

#define CAN_STUB_QUEUE_LEN 16

typedef struct
{
    can_frame_t rx[CAN_STUB_QUEUE_LEN];
    unsigned rx_head;
    unsigned rx_tail;
    unsigned rx_count;

    can_frame_t tx[CAN_STUB_QUEUE_LEN];
    unsigned tx_head;
    unsigned tx_tail;
    unsigned tx_count;

    can_bus_status_t status;
    uint32_t bitrate;
    int initialized;
} can_stub_t;

/* 取桩驱动的操作表，交给 can_driver_init 使用 */
const can_driver_ops_t *can_stub_ops(void);

/* 测试辅助 ---------------------------------------------------------- */

/* 模拟"总线上收到一帧"：放入接收队列，供上层 recv 取走 */
int can_stub_inject_rx(can_stub_t *stub, const can_frame_t *frame);

/* 取出桩"发出去"的帧，队列空返回 CAN_ERR_NOMSG */
int can_stub_pop_tx(can_stub_t *stub, can_frame_t *frame);

/* 直接设定错误状态，用来模拟进入错误被动 / Bus Off */
void can_stub_set_status(can_stub_t *stub, uint8_t tec, uint8_t rec, can_level_t level);

/* 查询已发送帧数 / 待接收帧数，便于断言 */
unsigned can_stub_tx_count(const can_stub_t *stub);
unsigned can_stub_rx_count(const can_stub_t *stub);

#endif
