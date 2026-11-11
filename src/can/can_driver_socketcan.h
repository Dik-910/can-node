#ifndef __CAN_DRIVER_SOCKETCAN_H
#define __CAN_DRIVER_SOCKETCAN_H

#include "can_driver.h"

/*
 * Linux SocketCAN 驱动实现。
 *
 * 这是 can_driver_ops_t 的第三个实现（前两个是 PC 桩和待补的 STM32）。
 * 它证明抽象层成立：上层 ISO-TP 和 UDS 一行没改，直接跑在真实内核 CAN 子系统上。
 *
 * 只支持 Linux。STM32 端不要 include 这个头文件。
 */

#define CAN_SOCKETCAN_IFNAME_MAX 16
#define CAN_SOCKETCAN_NO_FILTER  0xFFFFFFFFu

typedef struct
{
    int fd;
    char ifname[CAN_SOCKETCAN_IFNAME_MAX];

    /*
     * 接收过滤，init 之前由调用方设置。
     * vcan 是回环的：谁发的帧自己也会收到，不过滤会把发出去的帧又收回来。
     * 填 CAN_SOCKETCAN_NO_FILTER 表示不过滤。
     */
    uint32_t rx_filter_id;

    can_bus_status_t status;
    int initialized;
} can_socketcan_t;

const can_driver_ops_t *can_socketcan_ops(void);

#endif
