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
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    can_frame_t out;
    const uint8_t payload[5] = {0x10, 0x03, 0x22, 0xF1, 0x90};

    printf("单帧发送（5 字节）\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    check(isotp_send(&tp, &can, payload, 5) == ISOTP_OK, "小于 7 字节一次发完");
    check(isotp_tx_busy(&tp) == 0, "不处于传输中");

    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "取到发出的帧");
    check(out.id == 0x7E0, "使用 tx_id");
    check(out.dlc == 6, "DLC = 1 + 5");
    check(out.data[0] == 0x05, "PCI 为单帧且长度 5");
    check(memcmp(&out.data[1], payload, 5) == 0, "数据区一致");
}

/* 长报文：首帧 + 连续帧，验证切分与序号 */
static void test_multi_frame_tx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    can_frame_t out;
    uint8_t payload[20];
    int rc;

    printf("多帧发送（20 字节）\n");

    for (int i = 0; i < 20; i++)
        payload[i] = (uint8_t)(0x40 + i);

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    rc = isotp_send(&tp, &can, payload, 20);
    check(rc == ISOTP_BUSY, "发起后进入等待流控");
    check(isotp_tx_busy(&tp) == 1, "处于传输中");

    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "发出首帧");
    check(out.data[0] == 0x10, "PCI 为首帧，长度高 4 位为 0");
    check(out.data[1] == 20, "长度低 8 位为 20");
    check(out.dlc == 8, "首帧满载 8 字节");
    check(memcmp(&out.data[2], payload, 6) == 0, "首帧带前 6 字节");

    /* 没有流控帧之前不应发连续帧 */
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == 0, "未收到流控前不发连续帧");

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 0, 0);

    isotp_poll(&tp, &can);
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "发出第 1 个连续帧");
    check(out.data[0] == 0x21, "序号为 1");
    check(memcmp(&out.data[1], payload + 6, 7) == 0, "携带第 7..13 字节");

    isotp_poll(&tp, &can);
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "发出第 2 个连续帧");
    check(out.data[0] == 0x22, "序号为 2");
    check(out.dlc == 8, "最后一帧剩余 7 字节，满载");
    check(memcmp(&out.data[1], payload + 13, 7) == 0, "携带第 14..20 字节");

    check(isotp_tx_busy(&tp) == 0, "20 字节发完，回到空闲");
}

/* 对端通告块大小，发满一块必须停下等新的流控帧 */
static void test_block_size(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    can_frame_t out;
    uint8_t payload[30];
    unsigned before;

    printf("流控分块（BS = 2）\n");

    for (int i = 0; i < 30; i++)
        payload[i] = (uint8_t)i;

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 30);
    can_stub_pop_tx(&stub, &out); /* 首帧 */

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 2, 0);

    isotp_poll(&tp, &can); /* 第 1 帧 */
    isotp_poll(&tp, &can); /* 第 2 帧，本块已满 */
    check(can_stub_tx_count(&stub) == 2, "块内发满 2 帧");

    before = can_stub_tx_count(&stub);
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == before, "块满后不再继续发");

    inject_fc(&stub, cfg.rx_id, ISOTP_FS_CTS, 2, 0);
    isotp_poll(&tp, &can);
    check(can_stub_tx_count(&stub) == before + 1, "收到新流控后继续");
}

/* 对端报告溢出 */
static void test_fc_overflow(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    uint8_t payload[20];
    int rc;

    printf("对端流控溢出\n");

    memset(payload, 0x11, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 20);
    inject_fc(&stub, cfg.rx_id, ISOTP_FS_OVFLW, 0, 0);
    rc = isotp_poll(&tp, &can);

    check(rc == ISOTP_ERR_FC_OVERFLOW, "返回流控溢出");
    check(isotp_tx_busy(&tp) == 0, "传输被中止");
}

/* 接收单帧 */
static void test_single_frame_rx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0};
    uint8_t msg[16];
    uint16_t len = 0;
    uint8_t d[5] = {0x03, 0x62, 0xF1, 0x90, 0xAA};

    printf("单帧接收\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    inject_frame(&stub, cfg.rx_id, d, 5);
    check(isotp_poll(&tp, &can) == ISOTP_RX_COMPLETE, "收到完整报文");
    check(isotp_recv(&tp, msg, &len) == ISOTP_OK, "取走报文");
    check(len == 3, "长度为 3");
    check(msg[0] == 0x62 && msg[2] == 0x90, "内容正确");
    check(isotp_recv(&tp, msg, &len) == ISOTP_ERR_NOMSG, "重复取返回 NOMSG");
}

/* 接收多帧：收到首帧要自动回流量控制，再拼连续帧 */
static void test_multi_frame_rx(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0};
    can_frame_t out;
    uint8_t msg[32];
    uint16_t len = 0;
    uint8_t expect[20];

    printf("多帧接收（20 字节）\n");

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

    check(isotp_poll(&tp, &can) == ISOTP_BUSY, "首帧到达，进入接收");
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "自动发出流控帧");
    check(out.id == cfg.tx_id, "流控帧用 tx_id");
    check(out.data[0] == 0x30, "流控状态为 CTS");
    check(out.data[1] == 8, "通告我方块大小");
    check(out.data[2] == 10, "通告我方 STmin");

    /* 第 1 个连续帧 */
    {
        uint8_t d[8];
        d[0] = 0x21;
        memcpy(&d[1], expect + 6, 7);
        inject_frame(&stub, cfg.rx_id, d, 8);
    }
    check(isotp_poll(&tp, &can) == ISOTP_BUSY, "收到第 1 个连续帧");

    /* 第 2 个连续帧，凑满 20 */
    {
        uint8_t d[8];
        d[0] = 0x22;
        memcpy(&d[1], expect + 13, 7);
        inject_frame(&stub, cfg.rx_id, d, 8);
    }
    check(isotp_poll(&tp, &can) == ISOTP_RX_COMPLETE, "收齐 20 字节");

    check(isotp_recv(&tp, msg, &len) == ISOTP_OK, "取走完整报文");
    check(len == 20, "长度为 20");
    check(memcmp(msg, expect, 20) == 0, "内容逐字节一致");
}

/* 序号跳变必须整包废弃 */
static void test_sequence_error(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E8, 0x7E0, 8, 10, 0};
    uint8_t d[8];
    int rc;

    printf("连续帧序号错误\n");

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

    check(rc == ISOTP_ERR_SEQ, "返回序号错误");
}

/* 长度与缓冲上限 */
static void test_limits(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    can_frame_t out;
    uint8_t d[8];
    static uint8_t huge[ISOTP_MAX_PAYLOAD + 1];
    int rc;

    printf("长度与缓冲上限\n");

    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    check(isotp_send(&tp, &can, huge, 0) == ISOTP_ERR_TOO_LARGE, "长度 0 被拒绝");
    check(isotp_send(&tp, &can, huge, ISOTP_MAX_PAYLOAD + 1) == ISOTP_ERR_TOO_LARGE,
          "超过 4095 被拒绝");

    /* 接收方向：首帧声明的长度超过缓冲，应回 OVFLW 并报错 */
    d[0] = 0x11; /* 长度高 4 位为 1 */
    d[1] = 0xFF; /* 12 位长度 = 0x1FF = 511，仍在缓冲内 */
    memset(&d[2], 0, 6);
    inject_frame(&stub, cfg.rx_id, d, 8);
    rc = isotp_poll(&tp, &can);
    check(rc == ISOTP_BUSY, "511 字节在缓冲范围内，接受");

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
    check(rc == ISOTP_ERR_OVERFLOW, "4095 字节超过接收缓冲，报溢出");
    check(can_stub_pop_tx(&stub, &out) == CAN_OK, "回了流控帧");
    check(out.data[0] == 0x32, "流控状态为 OVFLW");
}

/* 中止 */
static void test_abort(void)
{
    can_stub_t stub;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {0x7E0, 0x7E8, 0, 0, 0};
    uint8_t payload[20];

    printf("主动中止\n");

    memset(payload, 0x22, sizeof(payload));
    can_driver_init(&can, can_stub_ops(), &stub, 500000);
    isotp_init(&tp, &cfg);

    isotp_send(&tp, &can, payload, 20);
    check(isotp_tx_busy(&tp) == 1, "传输开始");

    isotp_abort(&tp);
    check(isotp_tx_busy(&tp) == 0, "中止后回到空闲");
    check(isotp_send(&tp, &can, payload, 20) == ISOTP_BUSY, "可以重新发起");
}

int main(void)
{
    printf("=== ISO-TP 测试 ===\n\n");
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

    printf("\n结果：%d 通过，%d 失败\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
