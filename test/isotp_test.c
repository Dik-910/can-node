#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_stub.h"
#include "src/net/isotp.h"

/*
 * ISO-TP 测试。
 *
 * 用桩驱动扮演「对端」：桩的 tx 队列是我们发出去的帧，rx 队列是对端发来的帧。
 * 因此每个场景都能逐帧断言中间状态，而不是只看最终结果。
 *
 * 测试输出一律英文：中文常量在不同终端 locale 与嵌入式工具链下容易编码错乱。
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

/* 造一帧对端发来的报文并注入桩的接收队列 */
static void inject_frame(can_stub_t *stub, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    can_frame_t f;

    memset(&f, 0, sizeof(f));
    f.id = id;
    f.dlc = dlc;
    memcpy(f.data, data, dlc);
    can_stub_inject_rx(stub, &f);
}

/* 造一个流控帧注入 */
static void inject_fc(can_stub_t *stub, uint32_t id, uint8_t fs, uint8_t bs, uint8_t stmin)
{
    uint8_t d[3];

    d[0] = (uint8_t)(0x30 | (fs & 0x0F));
    d[1] = bs;
    d[2] = stmin;
    inject_frame(stub, id, d, 3);
}

/* 短报文：走单帧，一次发完 */
static void test_single_frame_tx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    can_frame_t out;
    const uint8_t payload[5] = {0x10, 0x03, 0x22, 0xF1, 0x90};

    printf("Single frame tx (5 bytes)\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    check(isotp_send(&tp, &can, payload, 5) == ISOTP_OK, "payload <= 7 bytes sent at once");
    check(isotp_tx_busy(&tp) == 0, "not busy");

    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "frame available");
    check(out.id == 0x7E0, "uses tx_id");
    check(out.dlc == 6, "dlc = 1 + 5");
    check(out.data[0] == 0x05, "pci is single frame with length 5");
    check(memcmp(&out.data[1], payload, 5) == 0, "payload matches");
}

/* 长报文：首帧 + 连续帧，验证切分与序号 */
static void test_multi_frame_tx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    can_frame_t out;
    uint8_t payload[20];
    int rc;

    printf("Multi frame tx (20 bytes)\n");

    for (int i = 0; i < 20; i++)
        payload[i] = (uint8_t)(0x40 + i);

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    rc = isotp_send(&tp, &can, payload, 20);
    check(rc == ISOTP_BUSY, "enters wait-for-flow-control");
    check(isotp_tx_busy(&tp) == 1, "busy while transferring");

    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "first frame sent");
    check(out.data[0] == 0x10, "pci is first frame, high nibble 0");
    check(out.data[1] == 20, "length low byte is 20");
    check(out.dlc == 8, "first frame is full 8 bytes");
    check(memcmp(&out.data[2], payload, 6) == 0, "first frame carries 6 bytes");

    /* 没有流控帧之前不应发连续帧 */
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == 0, "no consecutive frame before flow control");

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 0, 0);

    isotp_poll(&tp, &can);
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "consecutive frame 1 sent");
    check(out.data[0] == 0x21, "sequence number 1");
    check(memcmp(&out.data[1], payload + 6, 7) == 0, "carries bytes 7..13");

    isotp_poll(&tp, &can);
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "consecutive frame 2 sent");
    check(out.data[0] == 0x22, "sequence number 2");
    check(out.dlc == 8, "last frame carries remaining 7 bytes");
    check(memcmp(&out.data[1], payload + 13, 7) == 0, "carries bytes 14..20");

    check(isotp_tx_busy(&tp) == 0, "transfer finished after 20 bytes");
}

/* 对端通告块大小，发满一块必须停下等新的流控帧 */
static void test_block_size(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    can_frame_t out;
    uint8_t payload[30];
    unsigned before;

    printf("Flow control block size (BS = 2)\n");

    for (int i = 0; i < 30; i++)
        payload[i] = (uint8_t)i;

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 30);
    can_stub_pop_tx(&stub, &out); /* 首帧 */

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 2, 0);

    isotp_poll(&tp, &can); /* 第 1 帧 */
    isotp_poll(&tp, &can); /* 第 2 帧，本块已满 */
    check(can_stub_tx_count(&stub) == 2, "stops after block size reached");

    before = can_stub_tx_count(&stub);
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == before, "no frame while waiting for next fc");

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 2, 0);
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == before + 1, "resumes after new flow control");
}

/* 对端报告溢出 */
static void test_fc_overflow(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    uint8_t payload[20];
    int rc;

    printf("Peer flow control overflow\n");

    memset(payload, 0x11, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 20);
    inject_fc(&stub, cfg.rx_id, ISOTP_FS_OVFLW, 0, 0);
    rc = isotp_poll(&tp, &can);

    check(rc == ISOTP_ERR_FC_OVERFLOW, "returns flow control overflow");
    check(isotp_tx_busy(&tp) == 0, "transfer aborted");
}

/* 接收单帧 */
static void test_single_frame_rx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0, 0, 0};
    uint8_t msg[16];
    uint16_t len = 0;
    uint8_t d[5] = {0x03, 0x62, 0xF1, 0x90, 0xAA};

    printf("Single frame rx\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    inject_frame(&stub, cfg.rx_id, d, 5);
    check(isotp_poll(&tp, &can) == ISOTP_RX_COMPLETE, "complete message received");
    check(isotp_recv(&tp, msg, sizeof(msg), &len) == ISOTP_OK, "message popped");
    check(len == 3, "length is 3");
    check(msg[0] == 0x62 && msg[2] == 0x90, "content matches");
    check(isotp_recv(&tp, msg, sizeof(msg), &len) == ISOTP_ERR_NOMSG, "second pop returns NOMSG");
}

/* 接收多帧：收到首帧要自动回流量控制，再拼连续帧 */
static void test_multi_frame_rx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0, 0, 0};
    can_frame_t out;
    uint8_t msg[32];
    uint16_t len = 0;
    uint8_t expect[20];

    printf("Multi frame rx (20 bytes)\n");

    for (int i = 0; i < 20; i++)
        expect[i] = (uint8_t)(0x80 + i);

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    /* 首帧：0x10 + 长度 20 + 前 6 字节 */
    {
        uint8_t d[8];
        d[0] = 0x10;
        d[1] = 20;
        memcpy(&d[2], expect, 6);
        inject_frame(&stub, cfg.rx_id, d, 8);
    }

    check(isotp_poll(&tp, &can) == ISOTP_BUSY, "first frame accepted, receiving");
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "flow control sent automatically");
    check(out.id == cfg.tx_id, "flow control uses tx_id");
    check(out.data[0] == 0x30, "flow status is CTS");
    check(out.data[1] == 8, "advertises local block size");
    check(out.data[2] == 10, "advertises local stmin");

    /* 第 1 个连续帧 */
    {
        uint8_t d[8];
        d[0] = 0x21;
        memcpy(&d[1], expect + 6, 7);
        inject_frame(&stub, cfg.rx_id, d, 8);
    }
    check(isotp_poll(&tp, &can) == ISOTP_BUSY, "consecutive frame 1 accepted");

    /* 第 2 个连续帧，凑满 20 */
    {
        uint8_t d[8];
        d[0] = 0x22;
        memcpy(&d[1], expect + 13, 7);
        inject_frame(&stub, cfg.rx_id, d, 8);
    }
    check(isotp_poll(&tp, &can) == ISOTP_RX_COMPLETE, "20 bytes assembled");

    check(isotp_recv(&tp, msg, sizeof(msg), &len) == ISOTP_OK, "message popped");
    check(len == 20, "length is 20");
    check(memcmp(msg, expect, 20) == 0, "content byte-exact");
}

/* 序号跳变必须整包废弃 */
static void test_sequence_error(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0, 0, 0};
    uint8_t d[8];
    int rc;

    printf("Consecutive frame sequence error\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    d[0] = 0x10;
    d[1] = 20;
    memset(&d[2], 0x55, 6);
    inject_frame(&stub, cfg.rx_id, d, 8);
    isotp_poll(&tp, &can);

    /* 期望序号 1，实际给 3 */
    d[0] = 0x23;
    memset(&d[1], 0x66, 7);
    inject_frame(&stub, cfg.rx_id, d, 8);
    rc = isotp_poll(&tp, &can);

    check(rc == ISOTP_ERR_SEQ, "returns sequence error");
}

/* 长度与缓冲上限 */
static void test_limits(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    can_frame_t out;
    uint8_t d[8];
    static uint8_t huge[ISOTP_MAX_PAYLOAD + 1];
    int rc;

    printf("Length and buffer limits\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    check(isotp_send(&tp, &can, huge, 0) == ISOTP_ERR_TOO_LARGE, "length 0 rejected");
    check(isotp_send(&tp, &can, huge, ISOTP_MAX_PAYLOAD + 1) == ISOTP_ERR_TOO_LARGE,
          "length above 4095 rejected");

    /* 接收方向：首帧声明的长度超过缓冲，应回 OVFLW 并报错 */
    d[0] = 0x11; /* 长度高 4 位为 1 */
    d[1] = 0xFF; /* 12 位长度 = 0x1FF = 511，仍在缓冲内 */
    memset(&d[2], 0, 6);
    inject_frame(&stub, cfg.rx_id, d, 8);
    rc = isotp_poll(&tp, &can);
    check(rc == ISOTP_BUSY, "511 bytes accepted, within buffer");

    /*
     * 测真正的溢出（0xFFF = 4095 > 512）。
     * 重新初始化桩以清空队列，否则会取到上一轮留下的流控帧。
     */
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);
    d[0] = 0x1F;
    d[1] = 0xFF;
    inject_frame(&stub, cfg.rx_id, d, 8);
    rc = isotp_poll(&tp, &can);
    check(rc == ISOTP_ERR_OVERFLOW, "4095 bytes exceeds rx buffer");
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "flow control replied");
    check(out.data[0] == 0x32, "flow status is OVFLW");
}

/* 中止 */
static void test_abort(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    uint8_t payload[20];

    printf("Abort\n");

    memset(payload, 0x22, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 20);
    check(isotp_tx_busy(&tp) == 1, "transfer started");

    isotp_abort(&tp);
    check(isotp_tx_busy(&tp) == 0, "idle after abort");
    check(isotp_send(&tp, &can, payload, 20) == ISOTP_BUSY, "can start again");
}

/*
 * 超时：对端不发流控帧。
 *
 * 这一组测的是原来那个 bug 的根因：状态卡在 WAIT_FC 时没有任何退出路径，
 * 于是「对端掉线一次」= 「本节点永久失去发送能力]。
 */
static void test_timeout_n_bs(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    /* N_Bs 显式给 300ms，让测试不用跨越大段时间 */
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 300, 0};
    uint8_t payload[20];

    printf("N_Bs timeout (peer never sends flow control)\n");

    memset(payload, 0x55, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    check(isotp_send(&tp, &can, payload, 20) == ISOTP_BUSY, "multi-frame started");

    /* now=0 是合法时间戳：计时器靠 timer_armed 起算，不能拿 0 当哨兵 */
    check(isotp_poll_at(&tp, &can, 0) == ISOTP_BUSY, "t=0 accepted, timer armed");
    check(isotp_poll_at(&tp, &can, 299) == ISOTP_BUSY, "299ms still within N_Bs");
    check(isotp_poll_at(&tp, &can, 301) == ISOTP_ERR_TIMEOUT, "301ms -> timeout");

    check(isotp_tx_busy(&tp) == 0, "tx state reset after timeout");

    /*
     * 核心断言。修复前这里会是 ISOTP_ERR_STATE：
     * 状态卡在 WAIT_FC、调用方放弃循环、后续所有发送全部失败。
     */
    check(isotp_send(&tp, &can, payload, 20) == ISOTP_BUSY,
          "can start a new transfer right after timeout");
}

/* 超时：收到首帧后对端不再发连续帧 */
static void test_timeout_n_cr(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 300};
    can_frame_t out;
    uint8_t ff[8] = {0x10, 20, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};

    printf("N_Cr timeout (peer stops sending consecutive frames)\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    inject_frame(&stub, cfg.rx_id, ff, 8);
    check(isotp_poll_at(&tp, &can, 0) == ISOTP_BUSY, "first frame accepted");
    check(tp.rx.active == 1, "rx marked active");

    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "flow control returned");
    check(out.data[0] == 0x30, "FC status is CTS");

    /*
     * 注意：没有新帧进来时 poll 返回 ISOTP_OK（"本次调用无事件"），
     * 不是 BUSY。BUSY 表示"本次调用推进了传输"。
     * 这里真正关心的是「没超时」，不是某个特定返回值。
     */
    check(isotp_poll_at(&tp, &can, 299) != ISOTP_ERR_TIMEOUT,
          "299ms still within N_Cr");
    check(isotp_poll_at(&tp, &can, 301) == ISOTP_ERR_TIMEOUT, "301ms -> timeout");
    check(tp.rx.active == 0, "rx state cleared, half message not left behind");

    /*
     * 半个旧报文必须彻底清掉，否则新报文的首帧会被当成
     * 「序号错乱」或「长度不符」而丢弃 —— 表现为对端恢复后仍然收不到数据。
     */
    inject_frame(&stub, cfg.rx_id, ff, 8);
    check(isotp_poll_at(&tp, &can, 302) == ISOTP_BUSY,
          "new first frame accepted after timeout");
}

/* 超时后完整恢复：不只状态字段对了，而是真的能发完一包 */
static void test_recovery_after_timeout(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 300, 0};
    uint8_t payload[20];

    printf("Recovery: full transfer works after a timeout\n");

    memset(payload, 0x77, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    /* 第一轮：对端沉默，超时 */
    isotp_send(&tp, &can, payload, 20);
    isotp_poll_at(&tp, &can, 0);
    check(isotp_poll_at(&tp, &can, 500) == ISOTP_ERR_TIMEOUT, "first attempt timed out");

    /*
     * 先清掉第一轮残留的 FF，再从干净队列开始第二轮。
     * 顺序不能反：反过来会把第二轮刚发出的 FF 一起清掉。
     */
    {
        can_frame_t junk;

        while (can_stub_pop_tx(&stub, &junk) == CAN_OK)
            ;
    }

    /* 对端恢复，第二轮必须完整跑完：FF -> FC -> CF -> CF */
    check(isotp_send(&tp, &can, payload, 20) == ISOTP_BUSY, "second attempt starts");

    isotp_poll_at(&tp, &can, 600);
    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 0, 0);

    isotp_poll_at(&tp, &can, 610);
    isotp_poll_at(&tp, &can, 620);
    isotp_poll_at(&tp, &can, 630);

    {
        can_frame_t out;
        uint8_t pci[4];
        int n = 0;

        while (n < 4 && can_stub_pop_tx(&stub, &out) == CAN_OK)
            pci[n++] = out.data[0];

        check(n == 3, "exactly FF + 2 CF on the bus");
        check(n >= 1 && pci[0] == 0x10, "first frame is FF(len=20)");
        check(n >= 2 && pci[1] == 0x21, "first CF has sequence 1");
        check(n >= 3 && pci[2] == 0x22, "second CF has sequence 2");
    }

    check(isotp_tx_busy(&tp) == 0, "transfer completed after recovery");
}

/* 时间回绕：uint32 毫秒约 49.7 天绕一次，间隔判断必须仍然正确 */
static void test_timeout_wraparound(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 300, 0};
    uint8_t payload[20];
    uint32_t t0 = 0xFFFFFF00u; /* 距上限 256ms */

    printf("Timeout across uint32 millisecond wraparound\n");

    memset(payload, 0x33, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);
    isotp_send(&tp, &can, payload, 20);

    check(isotp_poll_at(&tp, &can, t0) == ISOTP_BUSY, "armed before wraparound");
    /* t0+200 还没绕；此时间隔 200ms < 300ms */
    check(isotp_poll_at(&tp, &can, t0 + 200u) == ISOTP_BUSY, "200ms, no wrap yet");
    /*
     * t0+400 已经绕过 0：算出来是 144，远小于 t0。
     * 无符号减法 (144 - 0xFFFFFF00) 仍得 400，所以超时判断正确。
     * 如果实现里写成有符号比较或直接比大小，这里会判成「还没超时」。
     */
    check(isotp_poll_at(&tp, &can, t0 + 400u) == ISOTP_ERR_TIMEOUT,
          "400ms across wrap -> timeout");
}

/*
 * 取报文时缓冲区不够。
 *
 * 早期实现没有容量检查，直接 memcpy —— 刷写时 0x36 请求有几百字节，
 * 而调用方往往只给了 64 字节，于是写穿栈、静默退出。
 * 这里钉住两点：必须报错，且必须丢弃报文（否则调用方永远取不完）。
 */
static void test_recv_buffer_too_small(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0, 0, 0};
    uint8_t ff[8] = {0x10, 20, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t cf1[8] = {0x21, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t cf2[8] = {0x22, 0xEE, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t small[8];
    uint8_t big[64];
    uint16_t len = 0;

    printf("Recv buffer too small\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    /* 收齐一个 20 字节报文 */
    inject_frame(&stub, cfg.rx_id, ff, 8);
    isotp_poll_at(&tp, &can, 0);
    inject_frame(&stub, cfg.rx_id, cf1, 8);
    isotp_poll_at(&tp, &can, 1);
    inject_frame(&stub, cfg.rx_id, cf2, 8);
    check(isotp_poll_at(&tp, &can, 2) == ISOTP_RX_COMPLETE, "20-byte message complete");

    /* 8 字节缓冲装不下 20 字节 */
    check(isotp_recv(&tp, small, sizeof(small), &len) == ISOTP_ERR_OVERFLOW,
          "too-small buffer -> OVERFLOW");
    check(len == 0, "length not reported on overflow");

    /*
     * 报文必须已经被丢弃。
     * 如果实现只是返回错误但没清 complete，调用方下次还会取到同一条，
     * 换个大缓冲也救不回来 —— 表现为「永远在读同一条报文」。
     */
    check(isotp_recv(&tp, big, sizeof(big), &len) == ISOTP_ERR_NOMSG,
          "message discarded after overflow");

    /* 缓冲区够大时正常取走 */
    inject_frame(&stub, cfg.rx_id, ff, 8);
    isotp_poll_at(&tp, &can, 3);
    inject_frame(&stub, cfg.rx_id, cf1, 8);
    isotp_poll_at(&tp, &can, 4);
    inject_frame(&stub, cfg.rx_id, cf2, 8);
    isotp_poll_at(&tp, &can, 5);
    check(isotp_recv(&tp, big, sizeof(big), &len) == ISOTP_OK, "large buffer works");
    /* FF 带 6 字节（0x11..0x66），CF1 带 7（0x77..0xDD），所以 0xEE 在索引 13 */
    check(len == 20 && big[0] == 0x11 && big[13] == 0xEE && big[14] == 0xFF,
          "content intact");
}

int main(void)
{
    printf("=== ISO-TP test ===\n\n");
    test_single_frame_tx();
    printf("\n");
    test_multi_frame_tx();
    printf("\n");
    test_block_size();
    printf("\n");
    test_fc_overflow();
    printf("\n");
    test_single_frame_rx();
    printf("\n");
    test_multi_frame_rx();
    printf("\n");
    test_sequence_error();
    printf("\n");
    test_limits();
    printf("\n");
    test_abort();
    printf("\n");
    test_timeout_n_bs();
    printf("\n");
    test_timeout_n_cr();
    printf("\n");
    test_recovery_after_timeout();
    printf("\n");
    test_timeout_wraparound();
    printf("\n");
    test_recv_buffer_too_small();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
