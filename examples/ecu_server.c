#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_socketcan.h"
#include "src/net/isotp.h"
#include "src/uds/uds.h"

/*
 * 常驻 ECU：在 vcan0 上等诊断请求，处理完回响应。
 *
 * 用途是让**第三方实现**来测我们——python-can + udsoncan 作为 tester 发请求，
 * 我们的 C 实现作为 ECU 应答。自己测自己时，格式错了两边一起错也能"通过"；
 * 用别人的栈来拷问，才能说明真的符合协议。
 *
 * 用法：
 *   ./ecu-server &                          # 默认 30 秒后自动退出
 *   ./ecu-server --seconds 120              # 指定运行时长
 */

#define ID_REQUEST  0x7E0u
#define ID_RESPONSE 0x7E8u

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
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
    printf("[ecu] threshold updated to %u\n", g_threshold);
    fflush(stdout);
    return 0;
}

static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},  /* VIN，17 字节走多帧 */
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1},
};

static void print_bytes(const char *tag, const uint8_t *d, int n)
{
    printf("%s", tag);
    for (int i = 0; i < n; i++)
        printf(" %02X", d[i]);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    can_socketcan_t sk;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {ID_RESPONSE, ID_REQUEST, 8, 10, 0};
    uds_server_t srv;
    uds_config_t ucfg;
    uint8_t req[64], resp[64];
    uint16_t len = 0;
    int seconds = 30;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&sk, 0, sizeof(sk));
    strncpy(sk.ifname, "vcan0", sizeof(sk.ifname) - 1);
    sk.rx_filter_id = ID_REQUEST;
    sk.fd = -1;

    if (can_driver_init(&can, can_socketcan_ops(), &sk, 500000) != CAN_OK)
    {
        printf("cannot open vcan0\n");
        return 1;
    }

    isotp_init(&tp, &cfg);

    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.dids = g_dids;
    ucfg.did_count = (uint16_t)(sizeof(g_dids) / sizeof(g_dids[0]));
    uds_init(&srv, &ucfg);

    printf("[ecu] listening on vcan0, req=0x%03X rsp=0x%03X, %d s\n",
           ID_REQUEST, ID_RESPONSE, seconds);
    fflush(stdout);

    for (int tick = 0; tick < seconds * 1000 && !g_stop; tick++)
    {
        isotp_poll(&tp, &can);

        if (isotp_recv(&tp, req, &len) == ISOTP_OK)
        {
            int rlen;

            print_bytes("[ecu] <-", req, len);

            rlen = uds_handle_request(&srv, req, len, resp, sizeof(resp));
            if (rlen > 0)
            {
                print_bytes("[ecu] ->", resp, rlen);
                isotp_send(&tp, &can, resp, (uint16_t)rlen);

                /* 把响应推完再继续，否则下一次请求可能打断传输 */
                for (int i = 0; i < 50 && isotp_tx_busy(&tp); i++)
                {
                    isotp_poll(&tp, &can);
                    usleep(500);
                }
            }
        }

        /* S3 会话计时需要推进 */
        uds_tick(&srv, 1);
        usleep(1000);
    }

    can_driver_deinit(&can);
    printf("[ecu] exit\n");
    return 0;
}
