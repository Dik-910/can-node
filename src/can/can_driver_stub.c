#include "can_driver_stub.h"

#include <string.h>

/* ---------------- 队列辅助 ---------------- */

static int rx_push(can_stub_t *stub, const can_frame_t *frame)
{
    if (stub->rx_count >= CAN_STUB_QUEUE_LEN)
        return CAN_ERR_FULL;

    stub->rx[stub->rx_tail] = *frame;
    stub->rx_tail = (stub->rx_tail + 1) % CAN_STUB_QUEUE_LEN;
    stub->rx_count++;
    return CAN_OK;
}

static int rx_pop(can_stub_t *stub, can_frame_t *frame)
{
    if (stub->rx_count == 0)
        return CAN_ERR_NOMSG;

    *frame = stub->rx[stub->rx_head];
    stub->rx_head = (stub->rx_head + 1) % CAN_STUB_QUEUE_LEN;
    stub->rx_count--;
    return CAN_OK;
}

static int tx_push(can_stub_t *stub, const can_frame_t *frame)
{
    if (stub->tx_count >= CAN_STUB_QUEUE_LEN)
        return CAN_ERR_FULL;

    stub->tx[stub->tx_tail] = *frame;
    stub->tx_tail = (stub->tx_tail + 1) % CAN_STUB_QUEUE_LEN;
    stub->tx_count++;
    return CAN_OK;
}

/* ---------------- 驱动接口实现 ---------------- */

static int stub_init(void *ctx, uint32_t bitrate)
{
    can_stub_t *stub = (can_stub_t *)ctx;

    if (stub == 0)
        return CAN_ERR_PARAM;
    if (bitrate == 0)
        return CAN_ERR_PARAM;

    memset(stub, 0, sizeof(*stub));
    stub->bitrate = bitrate;
    stub->status.level = CAN_LEVEL_ACTIVE;
    stub->initialized = 1;
    return CAN_OK;
}

static int stub_send(void *ctx, const can_frame_t *frame)
{
    can_stub_t *stub = (can_stub_t *)ctx;
    int ret;

    if (stub == 0 || stub->initialized == 0)
        return CAN_ERR_STATE;

    /* Bus Off 状态下控制器停止收发，这是上层的保护逻辑要覆盖的分支 */
    if (stub->status.level == CAN_LEVEL_BUSOFF)
        return CAN_ERR_BUSOFF;

    ret = tx_push(stub, frame);
    if (ret != CAN_OK)
        return ret;

    /* 真实控制器：一次成功发送使 TEC 递减 1，且不会低于 0 */
    if (stub->status.tec > 0)
    {
        stub->status.tec--;
        if (stub->status.tec <= 127 && stub->status.level == CAN_LEVEL_PASSIVE)
            stub->status.level = CAN_LEVEL_ACTIVE;
    }

    return CAN_OK;
}

static int stub_recv(void *ctx, can_frame_t *frame)
{
    can_stub_t *stub = (can_stub_t *)ctx;

    if (stub == 0 || stub->initialized == 0)
        return CAN_ERR_STATE;

    return rx_pop(stub, frame);
}

static int stub_get_status(void *ctx, can_bus_status_t *status)
{
    const can_stub_t *stub = (const can_stub_t *)ctx;

    if (stub == 0 || stub->initialized == 0)
        return CAN_ERR_STATE;

    *status = stub->status;
    return CAN_OK;
}

static void stub_deinit(void *ctx)
{
    can_stub_t *stub = (can_stub_t *)ctx;

    if (stub == 0)
        return;

    stub->initialized = 0;
}

static const can_driver_ops_t stub_ops = {
    stub_init,
    stub_send,
    stub_recv,
    stub_get_status,
    stub_deinit,
};

const can_driver_ops_t *can_stub_ops(void)
{
    return &stub_ops;
}

/* ---------------- 测试辅助 ---------------- */

int can_stub_inject_rx(can_stub_t *stub, const can_frame_t *frame)
{
    if (stub == 0 || frame == 0 || stub->initialized == 0)
        return CAN_ERR_PARAM;

    return rx_push(stub, frame);
}

int can_stub_pop_tx(can_stub_t *stub, can_frame_t *frame)
{
    if (stub == 0 || frame == 0)
        return CAN_ERR_PARAM;
    if (stub->tx_count == 0)
        return CAN_ERR_NOMSG;

    *frame = stub->tx[stub->tx_head];
    stub->tx_head = (stub->tx_head + 1) % CAN_STUB_QUEUE_LEN;
    stub->tx_count--;
    return CAN_OK;
}

void can_stub_set_status(can_stub_t *stub, uint8_t tec, uint8_t rec, can_level_t level)
{
    if (stub == 0)
        return;

    stub->status.tec = tec;
    stub->status.rec = rec;
    if (level == CAN_LEVEL_BUSOFF && stub->status.level != CAN_LEVEL_BUSOFF)
        stub->status.busoff_count++;
    stub->status.level = level;
}

unsigned can_stub_tx_count(const can_stub_t *stub)
{
    return stub ? stub->tx_count : 0;
}

unsigned can_stub_rx_count(const can_stub_t *stub)
{
    return stub ? stub->rx_count : 0;
}
