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
 * 关于时序：本模块**不阻塞、不内置定时器**，但需要调用方在每次 poll 时喂当前时间。
 * STmin 的节拍仍由调用方控制（本模块只记录时间，不 sleep），
 * 而超时必须由本模块自己判断 —— 因为它掌握状态机，只有它知道「等了多久还没等到」。
 *
 * 为什么超时不能放在调用方：
 *   早期版本让调用方写 for (i<50 && busy) { poll; usleep(500); }，
 *   超时后循环退出了，但 tx.state 仍停在 WAIT_FC。
 *   下一次 isotp_send 因为 state != IDLE 直接返回 ERR_STATE，
 *   于是**对端掉线一次，本节点就永久发不出响应**。
 *   状态机的退出必须由状态机的拥有者负责，调用方无从下手。
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
#define ISOTP_ERR_TIMEOUT     (-8) /* 等流控或等连续帧超时，状态已自动复位 */

/* 超时默认值，取自 ISO 15765-2 的推荐量级。
 * 真实项目里 N_Bs 要按对端的最坏响应时间调，这里给的是通用值。 */
#define ISOTP_DEFAULT_N_BS_MS 1000 /* 发完 FF/CF 后等对端流控帧 */
#define ISOTP_DEFAULT_N_CR_MS 1000 /* 收到 FF/CF 后等对端下一个连续帧 */

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

    /*
     * 超时（毫秒），0 表示用默认值 ISOTP_DEFAULT_N_*_MS。
     * 设为 0xFFFF 可以近似关掉检查 —— 但真要关掉，
     * 意味着对端掉线后本节点会永久卡死，只在台架调试时考虑。
     */
    uint16_t n_bs_ms;
    uint16_t n_cr_ms;
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

    /*
     * 超时状态。last_activity_ms 在「发出 FF/CF」和「收到任意帧」时刷新，
     * 于是两种等待自然共用同一个计时器：
     *   tx 在 WAIT_FC/SENDING 且长时间没动静 → N_Bs 超时
     *   rx.active 且长时间没动静            → N_Cr 超时
     *
     * 时间用 uint32_t 毫秒、靠无符号减法判断间隔，49.7 天回绕一次也不会错。
     * 前提是调用方喂的是单调递增时间（裸机用 SysTick，PC 用 CLOCK_MONOTONIC）；
     * 喂墙钟时间会在系统对时后出现负间隔，被当成巨大正数而误判超时。
     */
    uint32_t last_activity_ms;
    uint8_t timer_armed; /* 计时器是否已起算。不能拿 last_activity_ms==0 判断：
                          * 调用方的时间可能就从 0 开始（PC 测试、刚上电的 SysTick），
                          * 那样 0 是合法时间戳，计时器永远起不来，超时也就永远不触发。 */
    uint16_t n_bs_ms;
    uint16_t n_cr_ms;
} isotp_t;

int isotp_init(isotp_t *tp, const isotp_config_t *cfg);

/*
 * 发起一次发送。
 *   len <= 7   走单帧，一次发完，返回 ISOTP_OK
 *   len <= 4095 走首帧 + 连续帧，返回 ISOTP_BUSY，需继续 isotp_poll
 * 返回后调用方必须保证 data 缓冲区存活到传输结束。
 *
 * 若上一次传输因超时被中止，状态已自动复位，这里可以正常发起新的发送。
 */
int isotp_send(isotp_t *tp, can_driver_t *can, const uint8_t *data, uint16_t len);

/*
 * 推进状态机，每次调用最多收一帧、发一帧。
 * now_ms 必须是单调递增的毫秒计数，用于判断 N_Bs / N_Cr 超时。
 *
 * 返回值：
 *   ISOTP_OK / ISOTP_BUSY / ISOTP_RX_COMPLETE  正常推进
 *   ISOTP_ERR_TIMEOUT                          超时，状态已复位，可重新 send
 *   其他负数                                    协议错误
 */
int isotp_poll_at(isotp_t *tp, can_driver_t *can, uint32_t now_ms);

/*
 * 不喂时间的版本，等价于「本次调用不做超时检查」。
 *
 * 只建议在单元测试里用（测试自己控制时序、要精确断言中间状态）。
 * 生产代码请用 isotp_poll_at —— 用这个版本意味着对端掉线后本节点会卡死。
 */
int isotp_poll(isotp_t *tp, can_driver_t *can);

/* 取走已接收的完整报文，成功返回 ISOTP_OK 并写出长度 */
int isotp_recv(isotp_t *tp, uint8_t *out, uint16_t *out_len);

/* 是否有传输在进行中 */
int isotp_tx_busy(const isotp_t *tp);

/* 中止当前发送，回到空闲态 */
void isotp_abort(isotp_t *tp);

#endif
