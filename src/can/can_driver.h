#ifndef __CAN_DRIVER_H
#define __CAN_DRIVER_H

#include <stdint.h>

/*
 * CAN 驱动抽象层
 *
 * 设计目的：上层（ISO-TP / UDS / 应用）只依赖本文件，不依赖任何 MCU 头文件。
 * 换平台时只替换 ops 的实现，上层代码一行不改。
 *
 * 实现约定：
 *   - 本层只处理 11 位标准帧。扩展帧在协议里没有使用，需要时再扩 flags 字段。
 *   - 返回码为负值表示失败，便于统一用 if (ret < 0) 判断。
 *   - recv 返回 CAN_ERR_NOMSG 表示"暂无数据"，不是故障，调用方不应按错误处理。
 */

#define CAN_OK            0
#define CAN_ERR_PARAM   (-1) /* 参数非法：空指针、DLC 超限、ID 越界 */
#define CAN_ERR_STATE   (-2) /* 驱动未初始化 */
#define CAN_ERR_NOMSG   (-3) /* 接收队列为空 */
#define CAN_ERR_BUSOFF  (-4) /* 总线关闭，控制器已停止收发 */
#define CAN_ERR_FULL    (-5) /* 发送队列已满 */

#define CAN_STD_ID_MAX 0x7FFU /* 11 位标准帧 ID 上限 */
#define CAN_MAX_DLC    8      /* 经典 CAN 单帧数据上限 */

/* 错误状态级别，取值与 NodeStatus 报文的 err_level 信号对齐 */
typedef enum
{
    CAN_LEVEL_ACTIVE = 0,  /* 错误主动，TEC/REC <= 127 */
    CAN_LEVEL_PASSIVE = 1, /* 错误被动，TEC/REC > 127 */
    CAN_LEVEL_BUSOFF = 2   /* 总线关闭，TEC > 255 */
} can_level_t;

typedef struct
{
    uint32_t id;              /* 标准帧 ID，不含标志位 */
    uint8_t dlc;              /* 数据长度 0..8 */
    uint8_t data[CAN_MAX_DLC];
} can_frame_t;

/* 总线状态快照，供诊断与状态上报使用 */
typedef struct
{
    uint8_t tec;           /* 发送错误计数 */
    uint8_t rec;           /* 接收错误计数 */
    can_level_t level;     /* 当前错误状态 */
    uint32_t busoff_count; /* 累计进入 Bus Off 次数 */
} can_bus_status_t;

/* 每个平台实现一份，函数指针为空表示该平台不支持 */
typedef struct
{
    int (*init)(void *ctx, uint32_t bitrate);
    int (*send)(void *ctx, const can_frame_t *frame);
    int (*recv)(void *ctx, can_frame_t *frame);
    int (*get_status)(void *ctx, can_bus_status_t *status);
    void (*deinit)(void *ctx);
} can_driver_ops_t;

typedef struct
{
    const can_driver_ops_t *ops; /* NULL 表示未初始化 */
    void *ctx;                   /* 平台私有数据 */
} can_driver_t;

/*
 * 通用包装层。
 *
 * 参数校验集中在这里，各平台实现不必重复检查——这是抽象层存在的第二个理由：
 * 校验逻辑只有一处，不会出现"stm32 版检查了 dlc，桩版没检查"这类分歧。
 */
int can_driver_init(can_driver_t *drv, const can_driver_ops_t *ops, void *ctx, uint32_t bitrate);
int can_driver_send(can_driver_t *drv, const can_frame_t *frame);
int can_driver_recv(can_driver_t *drv, can_frame_t *frame);
int can_driver_get_status(can_driver_t *drv, can_bus_status_t *status);
void can_driver_deinit(can_driver_t *drv);

#endif
