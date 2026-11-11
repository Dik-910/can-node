#include "can_driver.h"

/*
 * 通用包装层实现。
 *
 * 这里只做三件事：判空、校验收敛、转发。任何"平台相关"的东西都不该出现在本文件里，
 * 否则抽象层就退化成第二个平台实现了。
 */

/* 帧合法性：ID 在 11 位范围内，DLC 不超过 8 */
static int frame_is_valid(const can_frame_t *frame)
{
    if (frame == 0)
        return 0;
    if (frame->id > CAN_STD_ID_MAX)
        return 0;
    if (frame->dlc > CAN_MAX_DLC)
        return 0;
    return 1;
}

/* 操作表至少要提供 init / send / recv / get_status，deinit 可选 */
static int ops_is_valid(const can_driver_ops_t *ops)
{
    if (ops == 0)
        return 0;
    if (ops->init == 0 || ops->send == 0 || ops->recv == 0 || ops->get_status == 0)
        return 0;
    return 1;
}

int can_driver_init(can_driver_t *drv, const can_driver_ops_t *ops, void *ctx, uint32_t bitrate)
{
    int ret;

    if (drv == 0 || ops_is_valid(ops) == 0)
        return CAN_ERR_PARAM;

    /* 先标记为未初始化，避免 init 失败后残留半初始化状态 */
    drv->ops = 0;
    drv->ctx = 0;

    ret = ops->init(ctx, bitrate);
    if (ret != CAN_OK)
        return ret;

    drv->ops = ops;
    drv->ctx = ctx;
    return CAN_OK;
}

int can_driver_send(can_driver_t *drv, const can_frame_t *frame)
{
    if (drv == 0 || drv->ops == 0)
        return CAN_ERR_STATE;
    if (frame_is_valid(frame) == 0)
        return CAN_ERR_PARAM;

    return drv->ops->send(drv->ctx, frame);
}

int can_driver_recv(can_driver_t *drv, can_frame_t *frame)
{
    if (drv == 0 || drv->ops == 0)
        return CAN_ERR_STATE;
    if (frame == 0)
        return CAN_ERR_PARAM;

    return drv->ops->recv(drv->ctx, frame);
}

int can_driver_get_status(can_driver_t *drv, can_bus_status_t *status)
{
    if (drv == 0 || drv->ops == 0)
        return CAN_ERR_STATE;
    if (status == 0)
        return CAN_ERR_PARAM;

    return drv->ops->get_status(drv->ctx, status);
}

void can_driver_deinit(can_driver_t *drv)
{
    if (drv == 0 || drv->ops == 0)
        return;

    if (drv->ops->deinit != 0)
        drv->ops->deinit(drv->ctx);

    drv->ops = 0;
    drv->ctx = 0;
}
