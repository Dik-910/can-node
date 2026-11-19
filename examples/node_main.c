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
#include "src/nvm/nvm_file.h"
#include "src/nvm/nvm_store.h"
#include "src/app/persist.h"
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
    uint32_t tx_timeouts;   /* 诊断响应因对端无流控而超时的次数 */
    uint32_t status_sent;
    uint32_t bus_sent;

    uint64_t last_status_ms;
    uint64_t last_bus_ms;
    uint64_t start_ms;
} node_t;

/* ---------------- 故障码 ---------------- */

/*
 * 本节点的故障全部来自总线健康状态，三条都能被 node_health 真实检测到，
 * 没有一条是「占位」的。
 *
 * 码的高两位是 11（网络类），和 0xC0xxxx 这种常见的通信故障编码风格一致。
 */
#define DTC_BUS_OFF       0xC00187u /* 总线关闭 */
#define DTC_ERROR_PASSIVE 0xC00287u /* 错误被动，TEC 偏高 */
#define DTC_BUS_STUCK     0xC00387u /* 反复 Bus Off，判定为硬件问题 */

static const dtc_def_t g_dtc_defs[] = {
    {DTC_BUS_OFF, "can bus off"},
    {DTC_ERROR_PASSIVE, "can error passive"},
    {DTC_BUS_STUCK, "can bus stuck / wiring fault"},
};

static dtc_store_t g_dtcs;

/* 快照要读节点状态，回调没有 ctx 参数，所以用文件级指针挂上去 */
static const node_health_t *g_snap_health;
static const node_t *g_snap_node;

/*
 * 快照按 ISO 14229 的 DTCSnapshotRecord 组织：[DID 个数][DID(2)][数据...]，
 * 和 ecu_server 保持一致。「DID 个数」那一字节不能省 —— 通用 tester
 * 靠它判断后面有几组键值对。
 */
#define DID_FAULT_CONTEXT 0x0001u

static int snap_provider(uint32_t code, uint8_t *out, uint8_t max_len)
{
    (void)code;

    if (max_len < 10 || g_snap_health == 0 || g_snap_node == 0)
        return 0;

    out[0] = 1; /* 只有一组 DID */
    out[1] = (uint8_t)(DID_FAULT_CONTEXT >> 8);
    out[2] = (uint8_t)(DID_FAULT_CONTEXT & 0xFF);
    out[3] = g_snap_health->state;
    out[4] = g_snap_health->last_tec;
    out[5] = (uint8_t)(g_snap_health->busoff_total & 0xFF);
    out[6] = (uint8_t)((g_snap_health->busoff_total >> 8) & 0xFF);
    out[7] = g_snap_node->err_level;
    out[8] = (uint8_t)(g_snap_node->uptime_s & 0xFF);
    out[9] = (uint8_t)((g_snap_node->uptime_s >> 8) & 0xFF);
    return 10;
}

/* ---------------- 诊断用假数据 ---------------- */

static uint16_t g_voltage_mv = 3660;
static uint8_t g_threshold = 50;

/* 参数区大小，与分区表一致（末页起 2KB） */
#define PARAM_AREA_SIZE 2048u

static nvm_file_ctx_t g_nvmf;
static nvm_store_t g_nvm;
static persist_t g_persist;
static int g_persist_ready;

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

    /* 标定值写入立刻落盘：回了正响应就该是真的存下来了 */
    if (g_persist_ready)
    {
        g_persist.params.threshold = g_threshold;
        persist_save_now(&g_persist, (uint32_t)now_ms());
    }

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
    /*
     * block_size=0 / stmin=0：不分块、不限速，对端可以连续发。
     *
     * 原来写 8 / 10ms，对几十字节的普通 UDS 请求没问题，
     * 但在固件传输上会致命：一个 0x36 请求 256 字节 = 1 FF + 36 CF，
     * 36 帧 × 10ms = 360ms，而 tester 的 P2（等响应）只有 50ms ——
     * 数据还没发完，tester 已经判超时了。
     *
     * 真实项目里 STmin 要按总线负载留余量（1~2ms 常见），
     * 这里为了联调速度取 0。接收侧靠主循环一次收干净来跟上。
     */
    isotp_config_t cfg = {ID_RESPONSE, ID_REQUEST, 0, 0, 0, 0, 0};
    uds_server_t srv;
    uds_config_t ucfg;
    node_t node;
    node_health_t health;
    uint32_t last_scale = 1;
    uint8_t prev_health = HEALTH_NORMAL;
    /* 与 ECU 同理：要容得下最大的 0x36 请求，不能按 64 字节给 */
    uint8_t req[512], resp[512];
    uint16_t len = 0;
    uint64_t last_tick;
    uint64_t last_persist;
    int seconds = 10;

    const char *nvm_path = ".node-nvm.bin";
    const char *ifname = "vcan0";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--nvm") == 0 && i + 1 < argc)
            nvm_path = argv[++i];
        /*
         * 接口名可选。网关联调时节点接在 vcan1，tester 在 vcan0 ——
         * 写死 vcan0 就没法验证「上位机经由网关访问节点」这条链路。
         */
        else if (strcmp(argv[i], "--if") == 0 && i + 1 < argc)
            ifname = argv[++i];
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&sk, 0, sizeof(sk));
    strncpy(sk.ifname, ifname, sizeof(sk.ifname) - 1);
    sk.rx_filter_id = ID_REQUEST; /* 只收诊断请求，回环回来的周期帧不要 */
    sk.fd = -1;

    if (can_driver_init(&can, can_socketcan_ops(), &sk, 500000) != CAN_OK)
    {
        printf("cannot open %s\n", ifname);
        return 1;
    }

    isotp_init(&tp, &cfg);

    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.dids = g_dids;
    ucfg.did_count = (uint16_t)(sizeof(g_dids) / sizeof(g_dids[0]));

    /*
     * 故障存储器挂在 UDS 配置上，0x19 / 0x14 才有东西可读可清。
     * 不挂的话这两个服务会回 NRC 0x11 —— 那才是诚实的做法，
     * 不支持的节点不该假装支持。
     */
    {
        dtc_config_t dcfg;

        memset(&dcfg, 0, sizeof(dcfg));
        dcfg.defs = g_dtc_defs;
        dcfg.def_count = (uint16_t)(sizeof(g_dtc_defs) / sizeof(g_dtc_defs[0]));
        dtc_init(&g_dtcs, &dcfg);
        dtc_set_snapshot_provider(&g_dtcs, snap_provider);
        /* 操作循环在持久化恢复之后再推进，见下面 persist_boot 之后 */
    }
    ucfg.dtcs = &g_dtcs;

    uds_init(&srv, &ucfg);

    /*
     * 参数区。dtc_init 之后才能挂 —— 恢复故障条目时要用到定义表。
     *
     * 打开失败不阻止节点启动：参数区坏了还能跑，只是标定值回默认、
     * 故障历史不保留。对车上的节点来说，能通信比保住历史重要。
     */
    {
        persist_params_t def;

        def.threshold = g_threshold;

        if (nvm_file_open(&g_nvmf, nvm_path, PARAM_AREA_SIZE) != 0)
        {
            printf("[node] WARN: param area %s unavailable, running without persistence\n",
                   nvm_path);
        }
        else if (persist_boot(&g_persist, &g_nvm, nvm_file_dev(&g_nvmf), 0,
                              &g_dtcs, &def, (uint32_t)now_ms()) != 0)
        {
            printf("[node] WARN: param area mount failed\n");
            nvm_file_close(&g_nvmf);
        }
        else
        {
            g_persist_ready = 1;

            if (g_persist.params.threshold <= 0xFFu)
                g_threshold = (uint8_t)g_persist.params.threshold;

            printf("[node] param area: %s, threshold=%u, dtc entries=%u, cycle=%u\n",
                   g_nvm.valid ? "restored" : "blank", g_threshold,
                   (unsigned)g_dtcs.count, (unsigned)g_dtcs.cycle);
        }

    }

    memset(&node, 0, sizeof(node));
    node.node_state = 1; /* 正常 */
    node.start_ms = now_ms();
    node.last_status_ms = node.start_ms;
    node.last_bus_ms = node.start_ms;
    last_tick = node.start_ms;
    last_persist = node.start_ms;

    node_health_init(&health);
    g_snap_health = &health;
    g_snap_node = &node;

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

            /*
             * 把总线状态翻译成故障码。
             *
             * 每个循环都要调，通过也要调 —— 不调的话这一层不知道「测过了」，
             * 老化了永远不推进，故障会一直挂在那里。
             */
            {
                uint32_t now32 = (uint32_t)(now - node.start_ms);

                dtc_report(&g_dtcs, DTC_BUS_OFF,
                           health.state == HEALTH_BUSOFF, now32);
                dtc_report(&g_dtcs, DTC_ERROR_PASSIVE,
                           health.state == HEALTH_DEGRADED, now32);
                dtc_report(&g_dtcs, DTC_BUS_STUCK,
                           health.long_degraded, now32);

                /*
                 * Bus Off 恢复视为一次操作循环的边界。
                 *
                 * 依据不是「凑一个循环」，而是 Bus Off 恢复时控制器确实做了
                 * 一次重新初始化并重新参与仲裁 —— 通信栈是从零开始的。
                 * 一个上电周期里反复 Bus Off，只有按这个边界切分，
                 * 「偶发一次」和「每恢复一次就再挂」才能被区分出来。
                 */
                if (prev_health == HEALTH_BUSOFF && health.state != HEALTH_BUSOFF)
                    dtc_operation_cycle(&g_dtcs);

                prev_health = health.state;
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
        /*
         * 一次收干净，不要每 tick 只推一帧。
         * 固件传输的 0x36 请求有几百字节（1 FF + 几十个 CF），
         * 按节拍慢慢收会顶着 tester 的 P2 超时。
         */
        for (int step = 0; step < 64; step++)
        {
            if (isotp_poll_at(&tp, &can, (uint32_t)now_ms()) <= 0)
                break;
        }
        if (isotp_recv(&tp, req, sizeof(req), &len) == ISOTP_OK)
        {
            int rlen;

            node.diag_requests++;
            node.rx_frames++;

            rlen = uds_handle_request(&srv, req, len, resp, sizeof(resp));
            if (rlen > 0)
            {
                isotp_send(&tp, &can, resp, (uint16_t)rlen);

                /*
                 * 把响应推完再继续，否则下一次请求会打断传输。
                 *
                 * 超时交给 ISO-TP 自己判断（N_Bs），这里只负责给节拍。
                 * 原来的写法是 for (i < 50) 写死次数：循环退出后状态仍停在
                 * WAIT_FC，下一次 isotp_send 因 state != IDLE 而永久失败 ——
                 * 一次对端掉线就能让这个节点再也发不出响应。
                 */
                while (isotp_tx_busy(&tp))
                {
                    if (isotp_poll_at(&tp, &can, (uint32_t)now_ms()) == ISOTP_ERR_TIMEOUT)
                    {
                        node.tx_timeouts++;
                        break;
                    }
                    usleep(500);
                }
            }
        }

        if (g_persist_ready && (now_ms() - last_persist) >= 1000)
        {
            persist_flush(&g_persist, (uint32_t)now_ms());
            last_persist = now_ms();
        }

        usleep(1000); /* 1ms 节拍 */
    }

    if (g_persist_ready)
    {
        persist_save_now(&g_persist, (uint32_t)now_ms());
        nvm_file_close(&g_nvmf);
    }

    can_driver_deinit(&can);

    printf("\n[node] stopped\n");
    printf("  uptime            : %u s\n", node.uptime_s);
    printf("  tx frames         : %u (status %u, bus %u)\n",
           node.tx_frames, node.status_sent, node.bus_sent);
    printf("  diag requests     : %u\n", node.diag_requests);
    printf("  isotp tx timeouts : %u\n", node.tx_timeouts);
    printf("  expected status   : %d\n", seconds * 1000 / PERIOD_STATUS_MS);
    printf("  expected bus      : %d\n", seconds * 1000 / PERIOD_BUS_MS);
    return 0;
}
