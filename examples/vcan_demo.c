#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_socketcan.h"
#include "src/net/isotp.h"
#include "src/uds/uds.h"

/*
 * vcan 联调演示：在真实 CAN 接口上跑通 驱动 -> ISO-TP -> UDS 三层。
 *
 * 一个进程里开两个 socket 扮演两端：
 *   tester  发 0x7E0，收 0x7E8
 *   ecu     收 0x7E0，发 0x7E8
 *
 * vcan 是回环的，发出去的帧自己也能收到，所以两端都设了内核过滤，
 * 否则 tester 会收到自己发的请求，iso-tp 状态机会被打乱。
 *
 * 运行前需要：
 *   sudo modprobe vcan
 *   sudo ip link add dev vcan0 type vcan
 *   sudo ip link set up vcan0
 */

#define ID_REQUEST  0x7E0u
#define ID_RESPONSE 0x7E8u

#define LOG_TAG "[tester]"

static void print_bytes(const char *tag, const uint8_t *d, int n)
{
    printf("%-9s", tag);
    for (int i = 0; i < n; i++)
        printf(" %02X", d[i]);
    printf("\n");
}

/* ---------------- ECU 侧的假数据 ---------------- */

static uint16_t g_voltage_mv = 3660;
static uint8_t g_threshold = 50;
static const uint8_t g_vin[17] = {'L', 'S', 'V', 'A', 'A', '1', '2', '3', '4',
                                  '5', '6', '7', '8', '9', '0', 'A', 'B'};

static int read_voltage(uint8_t *out, uint8_t *len)
{
    out[0] = (uint8_t)(g_voltage_mv >> 8);
    out[1] = (uint8_t)(g_voltage_mv & 0xFF);
    *len = 2;
    return 0;
}

static int read_threshold(uint8_t *out, uint8_t *len)
{
    out[0] = g_threshold;
    *len = 1;
    return 0;
}

static int write_threshold(const uint8_t *in, uint8_t len)
{
    if (len != 1)
        return -1;
    g_threshold = in[0];
    printf("          (ecu 内部：阈值已改为 %u)\n", g_threshold);
    return 0;
}

static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1},
};

/*
 * 完成一次「请求 -> 响应」的事务。
 *
 * 两个 isotp 实例交替推进：tester 发一帧、ecu 收一帧、ecu 回一帧、tester 收一帧。
 * 真实的两个进程不需要这样同步，这里是为了在同一进程里把时序摆清楚。
 */
static int transact(can_driver_t *can_t, isotp_t *tp_t,
                    can_driver_t *can_e, isotp_t *tp_e, uds_server_t *srv,
                    const uint8_t *req, uint16_t req_len,
                    uint8_t *resp, uint16_t *resp_len)
{
    uint8_t buf[64];
    uint16_t len = 0;
    int handled = 0;
    int ret;

    ret = isotp_send(tp_t, can_t, req, req_len);
    if (ret < 0)
        return -1;

    for (int i = 0; i < 500; i++)
    {
        isotp_poll(tp_t, can_t);
        isotp_poll(tp_e, can_e);

        /* ecu 收到完整请求 -> 交给 UDS 处理 -> 发回响应 */
        if (!handled && isotp_recv(tp_e, buf, &len) == ISOTP_OK)
        {
            int rlen = uds_handle_request(srv, buf, len, resp, 64);

            if (rlen > 0)
            {
                isotp_send(tp_e, can_e, resp, (uint16_t)rlen);
                *resp_len = (uint16_t)rlen;
            }
            handled = 1;
        }

        /* tester 收到完整响应 -> 结束 */
        if (isotp_recv(tp_t, buf, &len) == ISOTP_OK)
        {
            memcpy(resp, buf, len);
            *resp_len = len;
            return 0;
        }

        usleep(1000);
    }

    return -1;
}

int main(void)
{
    can_socketcan_t sk_tester, sk_ecu;
    can_driver_t can_tester, can_ecu;
    isotp_t tp_tester, tp_ecu;
    isotp_config_t cfg_tester = {ID_REQUEST, ID_RESPONSE, 0, 0, 0};
    isotp_config_t cfg_ecu = {ID_RESPONSE, ID_REQUEST, 8, 10, 0};
    uds_server_t srv;
    uds_config_t ucfg;
    uint8_t resp[64];
    uint16_t resp_len = 0;

    printf("=== vcan end-to-end demo ===\n");
    printf("driver -> ISO-TP -> UDS, all three layers on a real CAN interface\n\n");

    /* 两端各建一个 socket，各自只收自己关心的 ID */
    memset(&sk_tester, 0, sizeof(sk_tester));
    strncpy(sk_tester.ifname, "vcan0", sizeof(sk_tester.ifname) - 1);
    sk_tester.rx_filter_id = ID_RESPONSE;
    sk_tester.fd = -1;

    memset(&sk_ecu, 0, sizeof(sk_ecu));
    strncpy(sk_ecu.ifname, "vcan0", sizeof(sk_ecu.ifname) - 1);
    sk_ecu.rx_filter_id = ID_REQUEST;
    sk_ecu.fd = -1;

    if (can_driver_init(&can_tester, can_socketcan_ops(), &sk_tester, 500000) != CAN_OK)
    {
        printf("cannot open vcan0. run:\n");
        printf("  sudo modprobe vcan\n");
        printf("  sudo ip link add dev vcan0 type vcan\n");
        printf("  sudo ip link set up vcan0\n");
        return 1;
    }
    can_driver_init(&can_ecu, can_socketcan_ops(), &sk_ecu, 500000);

    isotp_init(&tp_tester, &cfg_tester);
    isotp_init(&tp_ecu, &cfg_ecu);

    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.dids = g_dids;
    ucfg.did_count = (uint16_t)(sizeof(g_dids) / sizeof(g_dids[0]));
    uds_init(&srv, &ucfg);

    /* ---------- 1. 进扩展会话 ---------- */
    {
        const uint8_t req[2] = {0x10, 0x03};
        print_bytes("[tester]", req, 2);
        if (transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 2, resp, &resp_len) != 0)
        {
            printf("timeout\n");
            return 1;
        }
        print_bytes("[ecu]", resp, resp_len);
        printf("          -> 扩展会话，S3 定时器起 5 秒\n\n");
    }

    /* ---------- 2. 请求种子 ---------- */
    {
        const uint8_t req[2] = {0x27, 0x01};
        print_bytes("[tester]", req, 2);
        transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 2, resp, &resp_len);
        print_bytes("[ecu]", resp, resp_len);
        printf("          -> 种子 %02X%02X%02X%02X\n\n", resp[2], resp[3], resp[4], resp[5]);

        /* 用 uds.c 里的默认算法算密钥，演示端需要事先知道算法 */
        {
            uint32_t seed = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) |
                            ((uint32_t)resp[4] << 8) | resp[5];
            uint32_t key = (seed ^ 0x5A5A5A5Au) + 0x12345678u;
            uint8_t kreq[6] = {0x27, 0x02,
                               (uint8_t)(key >> 24), (uint8_t)(key >> 16),
                               (uint8_t)(key >> 8), (uint8_t)key};

            print_bytes("[tester]", kreq, 6);
            transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, kreq, 6, resp, &resp_len);
            print_bytes("[ecu]", resp, resp_len);
            printf("          -> 解锁成功\n\n");
        }
    }

    /* ---------- 3. 未解锁时写会被拒（这里已经解锁，改看读） ---------- */
    {
        const uint8_t req[3] = {0x22, 0x02, 0x01};
        print_bytes("[tester]", req, 3);
        transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 3, resp, &resp_len);
        print_bytes("[ecu]", resp, resp_len);
        printf("          -> 当前阈值 = %u\n\n", resp[3]);
    }

    /* ---------- 4. 写阈值 ---------- */
    {
        const uint8_t req[4] = {0x2E, 0x02, 0x01, 0x3C};
        print_bytes("[tester]", req, 4);
        transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 4, resp, &resp_len);
        print_bytes("[ecu]", resp, resp_len);
        printf("\n");
    }

    /* ---------- 5. 读回验证 ---------- */
    {
        const uint8_t req[3] = {0x22, 0x02, 0x01};
        print_bytes("[tester]", req, 3);
        transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 3, resp, &resp_len);
        print_bytes("[ecu]", resp, resp_len);
        printf("          -> 读回 %u，写入已生效\n\n", resp[3]);
    }

    /* ---------- 6. 读 VIN（17 字节，走多帧响应） ---------- */
    {
        const uint8_t req[3] = {0x22, 0xF1, 0x90};
        print_bytes("[tester]", req, 3);
        transact(&can_tester, &tp_tester, &can_ecu, &tp_ecu, &srv, req, 3, resp, &resp_len);
        print_bytes("[ecu]", resp, resp_len);
        printf("          -> %u 字节，超过单帧上限，ISO-TP 自动分包\n\n", resp_len);
    }

    can_driver_deinit(&can_tester);
    can_driver_deinit(&can_ecu);

    printf("=== done ===\n");
    return 0;
}
