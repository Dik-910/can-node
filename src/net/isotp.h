#ifndef __ISOTP_H
#define __ISOTP_H

#include <stdint.h>

#include "can_driver.h"

/*
 * ISO 15765-2（ISO-TP）网络层：把超过 8 字节的 UDS 报文切成多帧，或把多帧拼回完整报文。
 *
 * 帧类型由首字节高 4 位区分：
 *   0x0 SF 单帧      len(0..7) + 数据
 *   0x1 FF 首帧      总长度占 12 位（低 4 位 + 下一字节），带 6 字节数据
 *   0x2 CF 连续帧    低 4 位是序号 SN（1..15，0 表示第 16 帧），带 7 字节数据
 *   0x3 FC 流控帧    低 4 位是状态 FS，后跟 BS 与 STmin
 *
 * 传输方向与 ID 的关系：
 *   本节点发请求用 tx_id，对端响应用 rx_id；作为服务端时反过来。
 *   本实现不区分主从，角色由配置的两个 ID 决定。
 *
 * 关于时序：本模块**不阻塞、不内置定时器**。STmin 与超时由调用方在自己的主循环里控制，
 * 这样同一份代码可以跑在裸机、RTOS 和 PC 测试环境里。
 */

#define ISOTP_MAX_PAYLOAD 4095 /* FF 的 12 位长度字段上限 */
#define ISOTP_RX_BUF_SIZE 512  /* 接收缓冲，够 UDS 用；改大需评估 RAM */

/* 返回值：负数为错误，0 为无事件，正数为状态 */
#define ISOTP_OK             0
#define ISOTP_BUSY           1  /* 传输进行中，需要继续 poll */
#define ISOTP_RX_COMPLETE    2  /* 收到完整报文，可用 isotp_recv 取走 */

#define ISOTP_ERR_PARAM       (-1)
#define ISOTP_ERR_STATE       (-2)
#define ISOTP_ERR_TOO_LARGE   (-3) /* 超过 12 位长度字段能表达的上限 */
#define ISOTP_ERR_SEQ         (-4) /* 连续帧序号不连续 */
#define ISOTP_ERR_OVERFLOW    (-5) /* 报文长度超过接收缓冲 */
#define ISOTP_ERR_FC_OVERFLOW (-6) /* 对端流控帧报告溢出 */
#define ISOTP_ERR_NOMSG       (-7) /* 没有完整报文可取 */

/* 流控状态 FS */
#define ISOTP_FS_CTS    0 /* Continue To Send */
#define ISOTP_FS_WAIT   1 /* Wait，继续等 */
#define ISOTP_FS_OVFLW  2 /* Overflow，对端缓冲不够 */

typedef struct
{
    uint32_t tx_id;    /* 本节点发送用 ID */
    uint32_t rx_id;    /* 接收对端响应用的 ID */
    uint8_t block_size;/* 我方作为接收方时通告的 BS，0 表示不分块 */
    uint8_t stmin;     /* 我方作为接收方时通告的 STmin（毫秒） */
    uint8_t padding;   /* 帧尾填充字节，0 表示不填充（用最小 DLC） */
} isotp_config_t;

typedef enum
{
    ISOTP_TX_IDLE = 0,
    ISOTP_TX_WAIT_FC, /* 已发首帧，等对端流控 */
    ISOTP_TX_SENDING  /* 正在发连续帧 */
} isotp_tx_state_t;

typedef struct
{
    const uint8_t *data; /* 指向调用方缓冲区，本模块不拷贝也不持有生命周期 */
    uint16_t len;
    uint16_t sent;     /* 已发送字节数 */
    uint8_t sn;        /* 下一个连续帧序号 */
    uint8_t bs_count;  /* 当前块内已发送的连续帧数 */
    uint8_t peer_bs;   /* 对端在流控帧里通告的块大小，0 表示不分块 */
    uint8_t peer_stmin;/* 对端通告的最小帧间隔（毫秒），由调用方控制节奏 */
    isotp_tx_state_t state;
} isotp_tx_t;

typedef struct
{
    uint8_t data[ISOTP_RX_BUF_SIZE];
    uint16_t total;    /* 报文总长度 */
    uint16_t received; /* 已接收字节数 */
    uint8_t next_sn;   /* 期望的下一个连续帧序号 */
    uint8_t active;    /* 是否处于多帧接收过程 */
    uint8_t complete;  /* 是否已收到完整报文 */
    uint16_t complete_len;
} isotp_rx_t;

typedef struct
{
    isotp_config_t cfg;
    isotp_tx_t tx;
    isotp_rx_t rx;
    int initialized;
} isotp_t;

int isotp_init(isotp_t *tp, const isotp_config_t *cfg);

/*
 * 发起一次发送。
 *   len <= 7   走单帧，一次发完，返回 ISOTP_OK
 *   len <= 4095 走首帧 + 连续帧，返回 ISOTP_BUSY，需继续 isotp_poll
 * 返回后调用方必须保证 data 缓冲区存活到传输结束。
 */
int isotp_send(isotp_t *tp, can_driver_t *can, const uint8_t *data, uint16_t len);

/*
 * 推进状态机，每次调用最多收一帧、发一帧。
 * 主循环里反复调用即可；返回值见上方 ISOTP_* 定义。
 */
int isotp_poll(isotp_t *tp, can_driver_t *can);

/* 取走已接收的完整报文，成功返回 ISOTP_OK 并写出长度 */
int isotp_recv(isotp_t *tp, uint8_t *out, uint16_t *out_len);

/* 是否有传输在进行中 */
int isotp_tx_busy(const isotp_t *tp);

/* 中止当前发送，回到空闲态 */
void isotp_abort(isotp_t *tp);

#endif
