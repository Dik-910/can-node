/*
 * 必须在所有 include 之前定义：-std=c99 是严格 ANSI 模式，
 * 不会暴露 struct ifreq / IFNAMSIZ 这些 POSIX/GNU 扩展。
 * 放在文件第一行而不是编译选项里，避免依赖 Makefile 的写法。
 */
#define _GNU_SOURCE

#include "can_driver_socketcan.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

/*
 * SocketCAN 实现。
 *
 * 与内核交互只有三步：socket/bind 拿到 fd，write 发帧，read 收帧。
 * 非阻塞模式下 read 返回 EAGAIN，映射成 CAN_ERR_NOMSG——注意这不是错误，
 * 上层轮询时会大量遇到，按错误处理会刷满日志。
 */

static int sk_send(void *ctx, const can_frame_t *frame)
{
    can_socketcan_t *sk = (can_socketcan_t *)ctx;
    struct can_frame cf;
    ssize_t n;

    if (sk == 0 || sk->initialized == 0)
        return CAN_ERR_STATE;

    memset(&cf, 0, sizeof(cf));
    cf.can_id = frame->id & CAN_SFF_MASK;
    cf.can_dlc = frame->dlc;
    memcpy(cf.data, frame->data, frame->dlc);

    n = write(sk->fd, &cf, sizeof(cf));
    if (n != (ssize_t)sizeof(cf))
        return CAN_ERR_STATE;

    return CAN_OK;
}

static int sk_recv(void *ctx, can_frame_t *frame)
{
    can_socketcan_t *sk = (can_socketcan_t *)ctx;
    struct can_frame cf;
    ssize_t n;

    if (sk == 0 || sk->initialized == 0)
        return CAN_ERR_STATE;

    n = read(sk->fd, &cf, sizeof(cf));
    if (n < 0)
    {
        /* 没有数据不是故障，上层靠这个返回值继续轮询 */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return CAN_ERR_NOMSG;
        return CAN_ERR_STATE;
    }
    if (n != (ssize_t)sizeof(cf))
        return CAN_ERR_NOMSG;

    frame->id = cf.can_id & CAN_SFF_MASK;
    frame->dlc = cf.can_dlc;
    memset(frame->data, 0, sizeof(frame->data));
    memcpy(frame->data, cf.data, cf.can_dlc);

    return CAN_OK;
}

/*
 * 错误计数。
 *
 * vcan 是纯软件回环，没有真实收发器，内核不上报 TEC/REC。
 * 真实硬件上这里应改为：
 *   - 订阅 CAN_RAW_ERR_FILTER，从错误帧里解析 CAN_ERR_CNT 的 data[6]/data[7]
 *   - 或直接读控制器寄存器
 * 当前返回缓存值，Bus Off 状态由上层通过错误帧更新。
 */
static int sk_get_status(void *ctx, can_bus_status_t *status)
{
    can_socketcan_t *sk = (can_socketcan_t *)ctx;

    if (sk == 0 || status == 0 || sk->initialized == 0)
        return CAN_ERR_STATE;

    *status = sk->status;
    return CAN_OK;
}

static int sk_init(void *ctx, uint32_t bitrate)
{
    can_socketcan_t *sk = (can_socketcan_t *)ctx;
    struct sockaddr_can addr;
    struct ifreq ifr;
    int fd;

    (void)bitrate; /* vcan 不需要波特率；真实硬件由 ip link 配置 */

    if (sk == 0 || sk->ifname[0] == '\0')
        return CAN_ERR_PARAM;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
        return CAN_ERR_STATE;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, sk->ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        close(fd);
        return CAN_ERR_STATE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return CAN_ERR_STATE;
    }

    /* 需要过滤时交给内核，避免把回环回来的自己发的帧也读进来 */
    if (sk->rx_filter_id != CAN_SOCKETCAN_NO_FILTER)
    {
        struct can_filter filter;
        filter.can_id = sk->rx_filter_id;
        filter.can_mask = CAN_SFF_MASK;
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter));
    }

    /* 非阻塞：上层是轮询模型，阻塞读会卡死主循环 */
    {
        int flags = 1;
        ioctl(fd, FIONBIO, &flags);
    }

    sk->fd = fd;
    sk->initialized = 1;
    sk->status.level = CAN_LEVEL_ACTIVE;
    return CAN_OK;
}

static void sk_deinit(void *ctx)
{
    can_socketcan_t *sk = (can_socketcan_t *)ctx;

    if (sk == 0)
        return;

    if (sk->initialized && sk->fd >= 0)
        close(sk->fd);

    sk->fd = -1;
    sk->initialized = 0;
}

static const can_driver_ops_t g_socketcan_ops = {
    sk_init,
    sk_send,
    sk_recv,
    sk_get_status,
    sk_deinit,
};

const can_driver_ops_t *can_socketcan_ops(void)
{
    return &g_socketcan_ops;
}
