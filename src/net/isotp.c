#include "isotp.h"

#include <string.h>

/*
 * ISO-TP 实现。
 *
 * 采用「一次 poll 处理一帧」的模型，而不是内部阻塞等待：
 *   - 裸机上没有线程，阻塞等流控帧会卡死主循环
 *   - PC 测试里可以精确控制每一帧的推进，便于断言中间状态
 *
 * 发送方遵守对端在流控帧里通告的 BS 与 STmin（存在 tx.peer_bs / peer_stmin），
 * 而不是自己的 cfg —— cfg 里的 BS/STmin 只用于我方作为接收方时通告给别人。
 * 这个区分搞错是 ISO-TP 实现里最常见的 bug。
 */

#define PCI_TYPE_SF 0x0
#define PCI_TYPE_FF 0x1
#define PCI_TYPE_CF 0x2
#define PCI_TYPE_FC 0x3

#define SF_MAX_DATA 7 /* 单帧最多 7 字节（1 字节 PCI + 7 数据） */
#define FF_DATA    6 /* 首帧带 6 字节 */
#define CF_DATA    7 /* 连续帧带 7 字节 */

static uint8_t pci_type(const can_frame_t *f)
{
    return (uint8_t)((f->data[0] >> 4) & 0x0F);
}

/* 按配置决定 DLC 与填充。padding 为 0 时用最小 DLC */
static void frame_prepare(const isotp_t *tp, can_frame_t *f, uint32_t id, uint8_t used)
{
    memset(f->data, tp->cfg.padding, sizeof(f->data));
    f->id = id;
    f->dlc = (tp->cfg.padding != 0) ? CAN_MAX_DLC : used;
}

static int send_sf(isotp_t *tp, can_driver_t *can, const uint8_t *data, uint16_t len)
{
    can_frame_t f;

    frame_prepare(tp, &f, tp->cfg.tx_id, (uint8_t)(1 + len));
    f.data[0] = (uint8_t)((PCI_TYPE_SF << 4) | (len & 0x0F));
    memcpy(&f.data[1], data, len);

    return can_driver_send(can, &f);
}

static int send_ff(isotp_t *tp, can_driver_t *can)
{
    can_frame_t f;
    uint16_t len = tp->tx.len;

    frame_prepare(tp, &f, tp->cfg.tx_id, CAN_MAX_DLC);
    /* 12 位长度：高 4 位在 PCI 低半字节，低 8 位在下一字节 */
    f.data[0] = (uint8_t)((PCI_TYPE_FF << 4) | ((len >> 8) & 0x0F));
    f.data[1] = (uint8_t)(len & 0xFF);
    memcpy(&f.data[2], tp->tx.data, FF_DATA);

    tp->tx.sent = FF_DATA;
    return can_driver_send(can, &f);
}

static int send_cf(isotp_t *tp, can_driver_t *can)
{
    can_frame_t f;
    uint16_t remain = (uint16_t)(tp->tx.len - tp->tx.sent);
    uint8_t chunk = (remain > CF_DATA) ? CF_DATA : (uint8_t)remain;

    frame_prepare(tp, &f, tp->cfg.tx_id, (uint8_t)(1 + chunk));
    f.data[0] = (uint8_t)((PCI_TYPE_CF << 4) | (tp->tx.sn & 0x0F));
    memcpy(&f.data[1], tp->tx.data + tp->tx.sent, chunk);

    /* 序号 1..15 循环，0 表示第 16 帧 */
    tp->tx.sn = (uint8_t)((tp->tx.sn + 1) & 0x0F);
    tp->tx.sent = (uint16_t)(tp->tx.sent + chunk);
    tp->tx.bs_count++;

    return can_driver_send(can, &f);
}

/* 我方作为接收方时，向对端通告接收能力 */
static int send_fc(isotp_t *tp, can_driver_t *can, uint8_t fs)
{
    can_frame_t f;

    frame_prepare(tp, &f, tp->cfg.tx_id, 3);
    f.data[0] = (uint8_t)((PCI_TYPE_FC << 4) | (fs & 0x0F));
    f.data[1] = tp->cfg.block_size;
    f.data[2] = tp->cfg.stmin;

    return can_driver_send(can, &f);
}

static int handle_flow_control(isotp_t *tp, const can_frame_t *f)
{
    uint8_t fs;

    /* 只有等流控的阶段才处理 FC，否则属于迟到的帧，直接忽略 */
    if (tp->tx.state != ISOTP_TX_WAIT_FC)
        return ISOTP_OK;

    fs = (uint8_t)(f->data[0] & 0x0F);

    if (fs == ISOTP_FS_CTS)
    {
        /* 对端通告的块大小与最小间隔，从这里开始生效 */
        tp->tx.peer_bs = f->data[1];
        tp->tx.peer_stmin = f->data[2];
        tp->tx.bs_count = 0;
        tp->tx.state = ISOTP_TX_SENDING;
        return ISOTP_OK;
    }

    if (fs == ISOTP_FS_OVFLW)
    {
        tp->tx.state = ISOTP_TX_IDLE;
        return ISOTP_ERR_FC_OVERFLOW;
    }

    /* FS_WAIT：保持等待，继续等下一个 FC */
    return ISOTP_BUSY;
}

static int handle_single_frame(isotp_t *tp, const can_frame_t *f)
{
    uint8_t len = (uint8_t)(f->data[0] & 0x0F);

    if (len > SF_MAX_DATA || (uint8_t)(len + 1) > f->dlc)
        return ISOTP_ERR_PARAM;

    /* 新报文到来，丢弃可能残留的半截接收 */
    tp->rx.active = 0;

    memcpy(tp->rx.data, &f->data[1], len);
    tp->rx.complete_len = len;
    tp->rx.complete = 1;

    return ISOTP_RX_COMPLETE;
}

static int handle_first_frame(isotp_t *tp, can_driver_t *can, const can_frame_t *f)
{
    uint16_t total = (uint16_t)((((uint16_t)f->data[0] & 0x0F) << 8) | f->data[1]);

    /* 长度小于 8 的报文应该用单帧，用首帧发说明对端实现有问题 */
    if (total <= SF_MAX_DATA)
        return ISOTP_ERR_PARAM;

    tp->rx.active = 0;
    tp->rx.complete = 0;

    if (total > ISOTP_RX_BUF_SIZE)
    {
        /* 缓冲装不下，用流控帧明确拒绝，避免对端白等超时 */
        send_fc(tp, can, ISOTP_FS_OVFLW);
        return ISOTP_ERR_OVERFLOW;
    }

    tp->rx.total = total;
    memcpy(tp->rx.data, &f->data[2], FF_DATA);
    tp->rx.received = FF_DATA;
    tp->rx.next_sn = 1;
    tp->rx.active = 1;

    /* 告诉对端可以继续发连续帧 */
    send_fc(tp, can, ISOTP_FS_CTS);

    return ISOTP_BUSY;
}

static int handle_consecutive_frame(isotp_t *tp, const can_frame_t *f)
{
    uint8_t sn;
    uint16_t remain;
    uint8_t chunk;

    /* 没在接收过程中收到的连续帧属于孤儿帧，忽略而不是报错 */
    if (!tp->rx.active)
        return ISOTP_OK;

    sn = (uint8_t)(f->data[0] & 0x0F);
    if (sn != tp->rx.next_sn)
    {
        /* 丢帧会导致拼接出错，必须整包废弃而不是跳过继续 */
        tp->rx.active = 0;
        return ISOTP_ERR_SEQ;
    }

    remain = (uint16_t)(tp->rx.total - tp->rx.received);
    chunk = (remain > CF_DATA) ? CF_DATA : (uint8_t)remain;

    memcpy(tp->rx.data + tp->rx.received, &f->data[1], chunk);
    tp->rx.received = (uint16_t)(tp->rx.received + chunk);
    tp->rx.next_sn = (uint8_t)((tp->rx.next_sn + 1) & 0x0F);

    if (tp->rx.received >= tp->rx.total)
    {
        tp->rx.complete = 1;
        tp->rx.complete_len = tp->rx.total;
        tp->rx.active = 0;
        return ISOTP_RX_COMPLETE;
    }

    return ISOTP_BUSY;
}

static int handle_frame(isotp_t *tp, can_driver_t *can, const can_frame_t *f)
{
    switch (pci_type(f))
    {
    case PCI_TYPE_FC:
        return handle_flow_control(tp, f);
    case PCI_TYPE_SF:
        return handle_single_frame(tp, f);
    case PCI_TYPE_FF:
        return handle_first_frame(tp, can, f);
    case PCI_TYPE_CF:
        return handle_consecutive_frame(tp, f);
    default:
        return ISOTP_OK;
    }
}

int isotp_init(isotp_t *tp, const isotp_config_t *cfg)
{
    if (tp == 0 || cfg == 0)
        return ISOTP_ERR_PARAM;
    if (cfg->tx_id > CAN_STD_ID_MAX || cfg->rx_id > CAN_STD_ID_MAX)
        return ISOTP_ERR_PARAM;

    memset(tp, 0, sizeof(*tp));
    tp->cfg = *cfg;
    tp->initialized = 1;

    /* 0 表示用默认值；显式给值则尊重调用方 */
    tp->n_bs_ms = (cfg->n_bs_ms != 0) ? cfg->n_bs_ms : ISOTP_DEFAULT_N_BS_MS;
    tp->n_cr_ms = (cfg->n_cr_ms != 0) ? cfg->n_cr_ms : ISOTP_DEFAULT_N_CR_MS;

    return ISOTP_OK;
}

int isotp_send(isotp_t *tp, can_driver_t *can, const uint8_t *data, uint16_t len)
{
    if (tp == 0 || can == 0 || data == 0 || tp->initialized == 0)
        return ISOTP_ERR_PARAM;
    if (len == 0 || len > ISOTP_MAX_PAYLOAD)
        return ISOTP_ERR_TOO_LARGE;
    if (tp->tx.state != ISOTP_TX_IDLE)
        return ISOTP_ERR_STATE; /* 上一次还没发完 */

    if (len <= SF_MAX_DATA)
        return (send_sf(tp, can, data, len) == CAN_OK) ? ISOTP_OK : ISOTP_ERR_STATE;

    tp->tx.data = data;
    tp->tx.len = len;
    tp->tx.sent = 0;
    tp->tx.sn = 1;
    tp->tx.bs_count = 0;
    tp->tx.peer_bs = 0;
    tp->tx.peer_stmin = 0;

    if (send_ff(tp, can) != CAN_OK)
    {
        tp->tx.state = ISOTP_TX_IDLE;
        return ISOTP_ERR_STATE;
    }

    tp->tx.state = ISOTP_TX_WAIT_FC;
    return ISOTP_BUSY;
}

/*
 * 状态机本体：收一帧、发一帧。不含超时判断。
 * 拆出来是因为它有多处 return，超时和计时器刷新需要在外层统一处理。
 */
static int poll_step(isotp_t *tp, can_driver_t *can)
{
    can_frame_t f;
    int rc;

    /* 先收：一张网卡上收发都走这里，不区分方向 */
    if (can_driver_recv(can, &f) == CAN_OK)
    {
        rc = handle_frame(tp, can, &f);
        if (rc < 0)
            return rc;

        /*
         * 接收进行中（BUSY）或已完成（RX_COMPLETE）都要立刻返回状态。
         * 早前版本在这里只判断了 RX_COMPLETE，导致收到首帧、连续帧时
         * 返回值被吞掉、最后返回 OK，调用方无法区分"还在收"和"没事件"。
         */
        if (rc != ISOTP_OK)
            return rc;
    }

    /* 再推进发送，每次调用最多发一帧，把节奏交回给调用方 */
    if (tp->tx.state == ISOTP_TX_SENDING)
    {
        if (tp->tx.sent >= tp->tx.len)
        {
            tp->tx.state = ISOTP_TX_IDLE;
            return ISOTP_OK;
        }

        if (send_cf(tp, can) != CAN_OK)
        {
            tp->tx.state = ISOTP_TX_IDLE;
            return ISOTP_ERR_STATE;
        }

        if (tp->tx.sent >= tp->tx.len)
        {
            tp->tx.state = ISOTP_TX_IDLE;
            return ISOTP_OK;
        }

        /* 按对端通告的块大小分批，发满一块就回去等新的流控帧 */
        if (tp->tx.peer_bs != 0 && tp->tx.bs_count >= tp->tx.peer_bs)
        {
            tp->tx.state = ISOTP_TX_WAIT_FC;
            tp->tx.bs_count = 0;
        }

        return ISOTP_BUSY;
    }

    if (tp->tx.state == ISOTP_TX_WAIT_FC)
        return ISOTP_BUSY;

    return ISOTP_OK;
}

int isotp_poll_at(isotp_t *tp, can_driver_t *can, uint32_t now_ms)
{
    isotp_tx_state_t prev_state;
    uint16_t prev_sent, prev_recvd;
    uint8_t prev_active;
    int rc;

    if (tp == 0 || can == 0 || tp->initialized == 0)
        return ISOTP_ERR_STATE;

    /*
     * 超时检查必须放在状态机之前。
     *
     * 如果放在「收到帧之后」，对端掉线时永远没有帧进来，
     * 超时判断就永远执行不到 —— 这正是原来那个 bug 的形态：
     * 状态卡在 WAIT_FC 却没有任何路径能让它退出。
     */
    if (tp->tx.state != ISOTP_TX_IDLE || tp->rx.active)
    {
        if (!tp->timer_armed)
        {
            /* 本轮传输刚开始，现在起算 */
            tp->last_activity_ms = now_ms;
            tp->timer_armed = 1;
        }
        else
        {
            uint32_t elapsed = now_ms - tp->last_activity_ms;

            /*
             * N_Bs：发出 FF 或一块 CF 之后，等对端流控帧。
             * 只有 WAIT_FC 受它约束；SENDING 状态每 poll 都会发一帧，
             * 卡不住（发送失败会直接回 IDLE）。
             */
            if (tp->tx.state == ISOTP_TX_WAIT_FC &&
                elapsed >= (uint32_t)tp->n_bs_ms)
            {
                isotp_abort(tp);
                return ISOTP_ERR_TIMEOUT;
            }

            /*
             * N_Cr：收到 FF/CF 之后，等对端下一个连续帧。
             * 超时要把接收状态一起清掉，否则半个报文会一直占着
             * rx.active，下一条新报文的首帧会被当成序号错乱而丢掉。
             */
            if (tp->rx.active && elapsed >= (uint32_t)tp->n_cr_ms)
            {
                tp->rx.active = 0;
                tp->rx.total = 0;
                tp->rx.received = 0;
                tp->rx.complete = 0;
                tp->rx.complete_len = 0;
                tp->last_activity_ms = 0;
                tp->timer_armed = 0;
                return ISOTP_ERR_TIMEOUT;
            }
        }
    }
    else
    {
        tp->last_activity_ms = 0;
        tp->timer_armed = 0;
    }

    /* 记基线，用于判断这一轮有没有实际推进 */
    prev_state = tp->tx.state;
    prev_sent = tp->tx.sent;
    prev_recvd = tp->rx.received;
    prev_active = tp->rx.active;

    rc = poll_step(tp, can);

    /*
     * 有进度就重置计时器。
     *
     * 判据是「有进度」而不是「收到帧」：总线上可能有本模块不关心的帧，
     * 按收帧刷新会让一个无关 ID 的持续流量把超时无限推迟。
     * 只有状态迁移或字节数增加才算真推进。
     */
    if (tp->tx.state != prev_state || tp->tx.sent != prev_sent ||
        tp->rx.received != prev_recvd || tp->rx.active != prev_active)
    {
        if (tp->tx.state != ISOTP_TX_IDLE || tp->rx.active)
        {
            tp->last_activity_ms = now_ms;
            tp->timer_armed = 1;
        }
        else
        {
            tp->last_activity_ms = 0;
            tp->timer_armed = 0;
        }
    }

    return rc;
}

int isotp_poll(isotp_t *tp, can_driver_t *can)
{
    /*
     * 不推进时间 = 不做超时检查。
     *
     * 传 tp->last_activity_ms 而不是 0：若正在等待，now 就是基线本身，
     * 算出的 elapsed 恒为 0，永远不会触发超时。
     * 生产代码请用 isotp_poll_at —— 用这个版本意味着对端掉线后会卡死。
     */
    if (tp == 0 || can == 0)
        return ISOTP_ERR_STATE;

    return isotp_poll_at(tp, can, tp->last_activity_ms);
}


int isotp_recv(isotp_t *tp, uint8_t *out, uint16_t *out_len)
{
    if (tp == 0 || out == 0 || out_len == 0)
        return ISOTP_ERR_PARAM;
    if (!tp->rx.complete)
        return ISOTP_ERR_NOMSG;

    memcpy(out, tp->rx.data, tp->rx.complete_len);
    *out_len = tp->rx.complete_len;
    tp->rx.complete = 0;

    return ISOTP_OK;
}

int isotp_tx_busy(const isotp_t *tp)
{
    return (tp != 0 && tp->tx.state != ISOTP_TX_IDLE) ? 1 : 0;
}

void isotp_abort(isotp_t *tp)
{
    if (tp == 0)
        return;

    tp->tx.state = ISOTP_TX_IDLE;
    tp->tx.len = 0;
    tp->tx.sent = 0;
    tp->tx.bs_count = 0;
    tp->last_activity_ms = 0;
    tp->timer_armed = 0; /* 中止后计时器失效，下次传输重新起算 */
}
