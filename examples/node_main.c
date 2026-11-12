#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_socketcan.h"
#include "src/can/generated/can_matrix.h"
#include "src/net/isotp.h"
#include "src/uds/uds.h"
#include "src/app/node_health.h"

/*
 * 完整节点主循环。
 *
 * 与 ecu_server 的区别：那个只被动等诊断请求，是「协议栈」；
 * 这个是「节点」——自己按周期发报文，同时随时能被诊断打断。
 *
 * 主循环就三件事，真实固件也是这个结构：
 *   1. 周期任务：按 deadline 发送应用报文（0x180 每 100ms、0x181 每 20ms）
 *   2. 诊断任务：处理 ISO-TP / UDS 请求
 *   3. 定时任务：推进 S3、BusOff 恢复等软件定时器
 *
 * 报文打包用的是 DBC 生成的 can_matrix_*_pack()，
 * 不是手写字节偏移——这样协议改了重新生成即可，不用改本文件。
 */

#define ID_NODE_STATUS 0x180u
#define ID_BUS_STATUS  0x181u
#define ID_REQUEST     0x7E0u
#define ID_RESPONSE    0x7E8u

#define PERIOD_STATUS_MS 100
#define PERIOD_BUS_MS    20

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---------------- 节点状态 ---------------- */

typedef struct
{
    uint8_t node_state;   /* 0 初始化 1 正常 2 降级 3 故障 */
    uint8_t err_level;    /* 0 主动 1 被动 2 BusOff */
    uint16_t busoff_cnt;
    uint32_t uptime_s;

    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t diag_requests;
    uint32_t status_sent;
    uint32_t bus_sent;

    uint64_t last_status_ms;
    uint64_t last_bus_ms;
    uint64_t start_ms;
} node_t;

/* ---------------- 诊断用假数据 ---------------- */

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
    return 0;
}

static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1},
};

/* ---------------- 周期报文发送 ---------------- */

static void send_node_status(node_t *n, can_driver_t *can)
{
    struct can_matrix_node_status_t msg;
    can_frame_t f;

    memset(&msg, 0, sizeof(msg));
    msg.node_state = n->node_state;
    msg.err_level = n->err_level;
    msg.bus_off_cnt = n->busoff_cnt;
    msg.uptime_s = n->uptime_s;

    memset(&f, 0, sizeof(f));
    f.id = ID_NODE_STATUS;
    f.dlc = 8;
    can_matrix_node_status_pack(f.data, &msg, sizeof(f.data));

    if (can_driver_send(can, &f) == CAN_OK)
    {
        n->tx_frames++;
        n->status_sent++;
    }
}

static void send_bus_status(node_t *n, can_driver_t *can)
{
    struct can_matrix_bus_status_t msg;
    can_bus_status_t st;
    can_frame_t f;

    memset(&msg, 0, sizeof(msg));
    if (can_driver_get_status(can, &st) == CAN_OK)
    {
        msg.tec = st.tec;
        msg.rec = st.rec;
        n->err_level = (uint8_t)st.level;
        n->busoff_cnt = (uint16_t)st.busoff_count;
    }
    msg.bus_load_permille = 0; /* 简化：真实实现按位时间统计 */

    memset(&f, 0, sizeof(f));
    f.id = ID_BUS_STATUS;
    f.dlc = 8;
    can_matrix_bus_status_pack(f.data, &msg, sizeof(f.data));

    if (can_driver_send(can, &f) == CAN_OK)
    {
        n->tx_frames++;
        n->bus_sent++;
    }
}

int main(int argc, char **argv)
{
    can_socketcan_t sk;
    can_driver_t can;
    isotp_t tp;
    isotp_config_t cfg = {ID_RESPONSE, ID_REQUEST, 8, 10, 0};
    uds_server_t srv;
    uds_config_t ucfg;
    node_t node;
    node_health_t health;
    uint32_t last_scale = 1;
    uint8_t req[64], resp[64];
    uint16_t len = 0;
    uint64_t last_tick;
    int seconds = 10;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&sk, 0, sizeof(sk));
    strncpy(sk.ifname, "vcan0", sizeof(sk.ifname) - 1);
    sk.rx_filter_id = ID_REQUEST; /* 只收诊断请求，回环回来的周期帧不要 */
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

    memset(&node, 0, sizeof(node));
    node.node_state = 1; /* 正常 */
    node.start_ms = now_ms();
    node.last_status_ms = node.start_ms;
    node.last_bus_ms = node.start_ms;
    last_tick = node.start_ms;

    node_health_init(&health);

    printf("[node] running %d s: 0x%03X/%dms, 0x%03X/%dms, diag 0x%03X<->0x%03X\n",
           seconds, ID_NODE_STATUS, PERIOD_STATUS_MS, ID_BUS_STATUS, PERIOD_BUS_MS,
           ID_REQUEST, ID_RESPONSE);
    fflush(stdout);

    while (!g_stop)
    {
        uint64_t now = now_ms();

        if (now - node.start_ms >= (uint64_t)seconds * 1000u)
            break;

        /* ---- 1. 定时器推进 ---- */
        uds_tick(&srv, (uint32_t)(now - last_tick));
        last_tick = now;
        node.uptime_s = (uint32_t)((now - node.start_ms) / 1000u);

        /* ---- 2. 总线健康与周期任务 ---- */
        {
            can_bus_status_t st;
            uint32_t scale;

            if (can_driver_get_status(&can, &st) == CAN_OK)
            {
                node_health_update(&health, &st, (uint32_t)(now - last_tick));
                node.err_level = (uint8_t)st.level;
                node.busoff_cnt = (uint16_t)st.busoff_count;
            }

            scale = node_health_period_scale(&health);

            /*
             * 缩放系数变化时重置基准，否则 last += 旧周期 会让基准偏移，
             * 恢复满速后会连着补发一批报文。
             */
            if (scale != last_scale)
            {
                node.last_status_ms = now;
                node.last_bus_ms = now;
                last_scale = scale;
            }

            /*
             * 用 last_xxx += period 而不是 last_xxx = now：
             * 后者会把每次的调度延迟永久累积进去，周期越跑越偏。
             * 降级时周期翻倍，减少总线竞争。
             */
            if (node_health_can_send(&health) && scale > 0)
            {
                while (now - node.last_status_ms >= (uint64_t)PERIOD_STATUS_MS * scale)
                {
                    send_node_status(&node, &can);
                    node.last_status_ms += (uint64_t)PERIOD_STATUS_MS * scale;
                }
                while (now - node.last_bus_ms >= (uint64_t)PERIOD_BUS_MS * scale)
                {
                    send_bus_status(&node, &can);
                    node.last_bus_ms += (uint64_t)PERIOD_BUS_MS * scale;
                }
            }
        }

        /* ---- 3. 诊断任务 ---- */
        isotp_poll(&tp, &can);
        if (isotp_recv(&tp, req, &len) == ISOTP_OK)
        {
            int rlen;

            node.diag_requests++;
            node.rx_frames++;

            rlen = uds_handle_request(&srv, req, len, resp, sizeof(resp));
            if (rlen > 0)
            {
                isotp_send(&tp, &can, resp, (uint16_t)rlen);

                for (int i = 0; i < 50 && isotp_tx_busy(&tp); i++)
                {
                    isotp_poll(&tp, &can);
                    usleep(500);
                }
            }
        }

        usleep(1000); /* 1ms 节拍 */
    }

    can_driver_deinit(&can);

    printf("\n[node] stopped\n");
    printf("  uptime            : %u s\n", node.uptime_s);
    printf("  tx frames         : %u (status %u, bus %u)\n",
           node.tx_frames, node.status_sent, node.bus_sent);
    printf("  diag requests     : %u\n", node.diag_requests);
    printf("  expected status   : %d\n", seconds * 1000 / PERIOD_STATUS_MS);
    printf("  expected bus      : %d\n", seconds * 1000 / PERIOD_BUS_MS);
    return 0;
}
