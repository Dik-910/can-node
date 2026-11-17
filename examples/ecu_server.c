#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "src/can/can_driver.h"
#include "src/can/can_driver_socketcan.h"
#include "src/net/isotp.h"
#include "src/uds/uds.h"
#include "src/nvm/nvm_file.h"
#include "src/nvm/nvm_store.h"
#include "src/app/persist.h"

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

static uint32_t now_ms32(void);

/* ---------------- ECU 侧的假数据 ---------------- */

static uint16_t g_voltage_mv = 3660;
static uint8_t g_threshold = 50;

/*
 * 参数区大小。与 docs/design.md 的分区表一致（末页起 2KB）。
 * 刷写流程只操作 App 分区，不会碰这里。
 */
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

    /*
     * 立刻落盘，不等主循环的定时 flush。
     *
     * 标定值写入是低频、用户主动的动作，值得马上写；
     * 而「等下一次定时保存」的窗口里如果掉电，诊断仪已经回了正响应，
     * 用户却以为参数已经生效——这种不一致比多写一次 Flash 代价大。
     */
    if (g_persist_ready)
    {
        g_persist.params.threshold = g_threshold;
        if (persist_save_now(&g_persist, now_ms32()) < 0)
            printf("[ecu] WARN: threshold saved to RAM only (nvm write failed)\n");
    }

    printf("[ecu] threshold updated to %u\n", g_threshold);
    fflush(stdout);
    return 0;
}

/* ---------------- 固件区模拟（供 0x34/0x36/0x37 刷写） ---------------- */

#define FLASH_BASE 0x08000000u
#define FLASH_SIZE (64u * 1024u)

static uint8_t g_flash[FLASH_SIZE];
static uint32_t g_flash_len;   /* 本次 0x34 声明的长度 */
static uint32_t g_flash_crc;   /* 0x37 之后算出的 CRC32 */

/* 位运算 CRC32，与 src/boot/boot.c 的算法一致（多项式 0xEDB88320） */
static uint32_t crc32_of(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }

    return ~crc;
}

static int flash_write(void *ctx, uint32_t addr, const uint8_t *d, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < FLASH_BASE)
        return -1;

    off = addr - FLASH_BASE;
    if (off + len > FLASH_SIZE)
        return -1;

    /* 只能 1 -> 0：未擦除区域写不进去，和真实 Flash 一致 */
    for (uint16_t i = 0; i < len; i++)
    {
        if ((g_flash[off + i] & d[i]) != d[i])
            return -1;
    }

    memcpy(&g_flash[off], d, len);
    return 0;
}

static int flash_read(void *ctx, uint32_t addr, uint8_t *d, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < FLASH_BASE)
        return -1;

    off = addr - FLASH_BASE;
    if (off + len > FLASH_SIZE)
        return -1;

    memcpy(d, &g_flash[off], len);
    return 0;
}

static int flash_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < FLASH_BASE)
        return -1;

    off = addr - FLASH_BASE;
    if (off + len > FLASH_SIZE)
        return -1;

    memset(&g_flash[off], 0xFF, len);
    return 0;
}

static const uds_memory_t g_memif = {
    flash_write, flash_read, flash_erase, 0, FLASH_BASE, FLASH_SIZE
};

/*
 * DID 0x0301：返回 (CRC32, 长度)，各 4 字节大端。
 *
 * 为什么要暴露这个：刷完之后总得有个办法证明「写进去的字节是对的」。
 * 用标准 0x22 读摘要，测试脚本就能拿自己算的 CRC 比对，
 * 不依赖日志解析，也不依赖内部状态。
 */
static int read_flash_digest(uint8_t *out, uint8_t *out_len)
{
    out[0] = (uint8_t)(g_flash_crc >> 24);
    out[1] = (uint8_t)(g_flash_crc >> 16);
    out[2] = (uint8_t)(g_flash_crc >> 8);
    out[3] = (uint8_t)g_flash_crc;
    out[4] = (uint8_t)(g_flash_len >> 24);
    out[5] = (uint8_t)(g_flash_len >> 16);
    out[6] = (uint8_t)(g_flash_len >> 8);
    out[7] = (uint8_t)g_flash_len;
    *out_len = 8;
    return 0;
}

/* 定义在故障码段落（要访问 g_dtcs），这里只做前置声明 */
static int read_dtc_stats(uint8_t *out, uint8_t *out_len);

static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},  /* VIN，17 字节走多帧 */
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1},
    {0x0301, 0, 0, read_flash_digest, 0, UDS_SESSION_DEFAULT, 0},  /* 刷写摘要 */
    {0x0302, 0, 0, read_dtc_stats, 0, UDS_SESSION_DEFAULT, 0},     /* 循环号与条目数 */
};

static void print_bytes(const char *tag, const uint8_t *d, int n)
{
    printf("%s", tag);
    for (int i = 0; i < n; i++)
        printf(" %02X", d[i]);
    printf("\n");
    fflush(stdout);
}

static uint32_t now_ms32(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

/* ---------------- 故障码 ---------------- */

/*
 * 本 ECU 只有一个真实可检测的故障：诊断通信超时。
 *
 * 触发路径是完全真实的 —— tester 发了首帧却不发连续帧（或者中途拔线），
 * ISO-TP 的 N_Cr 计时器到点后判定超时，这里记一条 DTC。
 * 不是插桩造出来的假故障，所以 0x19 读出来的东西有实际意义。
 */
#define DTC_DIAG_TIMEOUT 0xC10087u

static const dtc_def_t g_dtc_defs[] = {
    {DTC_DIAG_TIMEOUT, "diagnostic response timeout"},
};

static dtc_store_t g_dtcs;

/*
 * 故障存储的统计量：操作循环号 + 已记录条目数。
 *
 * 循环号存在的意义是让「老化跨复位」这件事可观测 ——
 * 0x19 只能看到故障本身，看不到它在数第几个循环。
 * 上位机或测试脚本读这个值，就能确认周期号是接着上次的，而不是每次上电从 1 开始。
 */
static int read_dtc_stats(uint8_t *out, uint8_t *out_len)
{
    uint32_t cycle = g_dtcs.cycle;

    out[0] = (uint8_t)(cycle >> 24);
    out[1] = (uint8_t)(cycle >> 16);
    out[2] = (uint8_t)(cycle >> 8);
    out[3] = (uint8_t)cycle;
    out[4] = (uint8_t)(g_dtcs.count >> 8);
    out[5] = (uint8_t)g_dtcs.count;
    *out_len = 6;
    return 0;
}

static uint32_t g_boot_ms;
static int g_last_isotp_rc;

/*
 * 快照：故障那一刻的工况。
 *
 * 内容按 ISO 14229 的 DTCSnapshotRecord 组织：
 *   [DID 个数][DID(2)][数据...]
 *
 * 这里必须带「DID 个数」这一字节 —— 通用 tester 靠它知道后面有几组
 * 键值对，少了它 udsoncan 会直接判「record 里没有 DID」并抛
 * InvalidResponseException。这个坑只有拿别人的栈来读才会暴露，
 * 自测时自己的解析器怎么写都能对上。
 *
 * 用 DID 组织而不是裸字节，是为了让工具能按 DID 各自的编码解码，
 * 否则读出来只能看到一串十六进制。
 *
 * 存运行时长而不是墙钟时间：车上的 ECU 没有 RTC，只有上电计时。
 */
#define DID_FAULT_CONTEXT 0x0001u

static int snap_provider(uint32_t code, uint8_t *out, uint8_t max_len)
{
    uint32_t up;

    (void)code;

    if (max_len < 8)
        return 0;

    up = now_ms32() - g_boot_ms;
    out[0] = 1; /* 只有一组 DID */
    out[1] = (uint8_t)(DID_FAULT_CONTEXT >> 8);
    out[2] = (uint8_t)(DID_FAULT_CONTEXT & 0xFF);
    out[3] = (uint8_t)(up & 0xFF);
    out[4] = (uint8_t)((up >> 8) & 0xFF);
    out[5] = (uint8_t)((up >> 16) & 0xFF);
    out[6] = (uint8_t)((up >> 24) & 0xFF);
    /* 负的错误码按位截断，看到 0xF8 就是 ISOTP_ERR_TIMEOUT(-8) */
    out[7] = (uint8_t)g_last_isotp_rc;
    return 8;
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
    /*
     * 请求缓冲必须容得下最大的 0x36 请求（max_block_len = 256），
     * 所以不能按「普通 UDS 请求 64 字节」来给。
     * 响应侧同理留足余量（0x22 读多帧可能几十字节）。
     */
    uint8_t req[512], resp[512];
    uint16_t len = 0;
    int seconds = 30;
    const char *ifname = "vcan0";
    const char *nvm_path = ".ecu-nvm.bin";
    int nvm_reset = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--if") == 0 && i + 1 < argc)
            ifname = argv[++i];
        else if (strcmp(argv[i], "--nvm") == 0 && i + 1 < argc)
            nvm_path = argv[++i];
        else if (strcmp(argv[i], "--nvm-reset") == 0)
            nvm_reset = 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&sk, 0, sizeof(sk));
    strncpy(sk.ifname, ifname, sizeof(sk.ifname) - 1);
    sk.rx_filter_id = ID_REQUEST;
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
    ucfg.mem = &g_memif;

    /* 诊断仪读码要有地方读，先把这个 ECU 的故障存储器立起来 */
    {
        dtc_config_t dcfg;

        memset(&dcfg, 0, sizeof(dcfg));
        dcfg.defs = g_dtc_defs;
        dcfg.def_count = (uint16_t)(sizeof(g_dtc_defs) / sizeof(g_dtc_defs[0]));
        dtc_init(&g_dtcs, &dcfg);
        dtc_set_snapshot_provider(&g_dtcs, snap_provider);
        /* 上电即进入第 1 个操作循环，老化计数从这里开始数 */
        dtc_operation_cycle(&g_dtcs);
    }

    ucfg.dtcs = &g_dtcs;
    uds_init(&srv, &ucfg);

    g_boot_ms = now_ms32();

    /*
     * 参数区。放在 dtc_init 之后、主循环之前。
     *
     * 顺序有讲究：persist_mount 会把存储里的故障条目恢复进 g_dtcs，
     * 所以 dtc 必须先 init 好 cfg（定义表）—— 没有定义表就判断不了
     * 哪些码是认识的。
     */
    {
        const nvm_dev_t *dev;
        persist_params_t def;

        def.threshold = g_threshold;

        if (nvm_file_open(&g_nvmf, nvm_path, PARAM_AREA_SIZE) != 0)
        {
            printf("cannot open param area %s\n", nvm_path);
            can_driver_deinit(&can);
            return 1;
        }

        /* nvm_file_dev 负责填函数表，persist_boot 才敢调 dev->erase */
        dev = nvm_file_dev(&g_nvmf);

        if (persist_boot(&g_persist, &g_nvm, dev, nvm_reset, &g_dtcs,
                         &def, now_ms32()) != 0)
        {
            printf("cannot mount param area\n");
            nvm_file_close(&g_nvmf);
            can_driver_deinit(&can);
            return 1;
        }

        if (nvm_reset)
            printf("[ecu] param area erased (%s)\n", nvm_path);

        g_persist_ready = 1;

        /* 恢复上一次写入的标定值 */
        if (g_persist.params.threshold <= 0xFFu)
            g_threshold = (uint8_t)g_persist.params.threshold;

        printf("[ecu] param area %s: %s, threshold=%u, dtc entries=%u, cycle=%u\n",
               nvm_path,
               g_nvm.valid ? "restored" : "blank",
               g_threshold,
               (unsigned)g_dtcs.count,
               (unsigned)g_dtcs.cycle);
    }

    printf("[ecu] listening on %s, req=0x%03X rsp=0x%03X, %d s\n",
           ifname, ID_REQUEST, ID_RESPONSE, seconds);
    fflush(stdout);

    for (int tick = 0; tick < seconds * 1000 && !g_stop; tick++)
    {
        /*
         * 一次把当前可读的帧都处理掉，而不是每 tick 只推进一帧。
         *
         * 256 字节的 0x36 请求 = 1 个 FF + 36 个 CF。每 tick 收一帧的话，
         * 收完要 37ms，而 tester 的 P2 是 50ms —— 刚好卡在边界上，
         * 调度一抖就判超时。
         *
         * isotp_poll_at 返回正数表示「本次推进了」（BUSY / RX_COMPLETE），
         * 0 表示没有更多帧，负数表示错误或超时 —— 后两种都该停下。
         */
        for (int step = 0; step < 64; step++)
        {
            g_last_isotp_rc = isotp_poll_at(&tp, &can, now_ms32());
            if (g_last_isotp_rc <= 0)
                break;
        }

        /*
         * 超时就是一次真实的通信故障。
         *
         * 只有超时才记，别的错误码（序号错、溢出）是 tester 自己发错了帧，
         * 属于对端的问题，不该记到本 ECU 的故障存储里 ——
         * 否则任何人拿 can-utils 乱发几帧就能把 ECU 刷成满屏故障。
         */
        dtc_report(&g_dtcs, DTC_DIAG_TIMEOUT, g_last_isotp_rc == ISOTP_ERR_TIMEOUT,
                   now_ms32());

        if (isotp_recv(&tp, req, sizeof(req), &len) == ISOTP_OK)
        {
            int rlen;

            print_bytes("[ecu] <-", req, len);

            rlen = uds_handle_request(&srv, req, len, resp, sizeof(resp));
            if (rlen > 0)
            {
                /*
                 * 刷写进度跟踪。
                 *
                 * 库本身不关心 CRC —— 那是流程校验，属于应用层的事。
                 * 但「写进去的字节对不对」总要有个办法证明，
                 * 所以在示例里算一次，通过 DID 0x0301 暴露出去，
                 * 让测试脚本用标准 0x22 读回来比对。
                 */
                if (req[0] == UDS_SID_REQUEST_DOWNLOAD && resp[0] == 0x74 && len >= 3)
                {
                    uint8_t alfid = req[2];
                    uint8_t addr_len = (uint8_t)(alfid & 0x0F);
                    uint8_t size_len = (uint8_t)((alfid >> 4) & 0x0F);
                    uint32_t addr = 0, sz = 0;

                    if ((uint16_t)(3 + addr_len + size_len) <= len)
                    {
                        for (uint8_t i = 0; i < addr_len; i++)
                            addr = (addr << 8) | req[3 + i];
                        for (uint8_t i = 0; i < size_len; i++)
                            sz = (sz << 8) | req[3 + addr_len + i];

                        g_flash_len = sz;
                        g_flash_crc = 0;
                        printf("[ecu] download: %u bytes -> 0x%08X\n", sz, addr);
                    }
                }
                else if (req[0] == UDS_SID_TRANSFER_EXIT && resp[0] == 0x77)
                {
                    g_flash_crc = crc32_of(g_flash, g_flash_len);
                    printf("[ecu] flash complete: %u bytes, crc32=0x%08X\n",
                           g_flash_len, g_flash_crc);
                }

                print_bytes("[ecu] ->", resp, rlen);
                isotp_send(&tp, &can, resp, (uint16_t)rlen);

                /* 把响应推完再继续，否则下一次请求可能打断传输。
                 * 超时由 ISO-TP 自己判断，见 isotp.h 里关于 N_Bs 的说明。 */
                while (isotp_tx_busy(&tp))
                {
                    int rc = isotp_poll_at(&tp, &can, now_ms32());

                    if (rc == ISOTP_ERR_TIMEOUT)
                    {
                        /*
                         * 响应发不出去同样算通信故障：tester 要了响应却不给流控，
                         * 这一条和收方向的超时是同一类问题，共用同一个码。
                         */
                        g_last_isotp_rc = rc;
                        dtc_report(&g_dtcs, DTC_DIAG_TIMEOUT, 1, now_ms32());
                        printf("[ecu] tx timeout, transfer aborted\n");
                        break;
                    }
                    usleep(500);
                }
            }
        }

        /* S3 会话计时需要推进 */
        uds_tick(&srv, 1);

        /*
         * 每秒检查一次是否需要落盘。
         * 真正的写频率由 persist 内部的关键状态签名控制 ——
         * 故障的 status 位每 10ms 就变，逐字节比较会一天写两万次。
         */
        if ((tick % 1000) == 0 && g_persist_ready)
            persist_flush(&g_persist, now_ms32());

        usleep(1000);
    }

    /* 正常退出前保存：否则这一段跑出来的故障历史就白记了 */
    if (g_persist_ready)
    {
        persist_save_now(&g_persist, now_ms32());
        printf("[ecu] persisted: threshold=%u, dtc entries=%u, cycle=%u, writes=%u\n",
               g_threshold, (unsigned)g_dtcs.count, (unsigned)g_dtcs.cycle,
               (unsigned)g_persist.writes);
    }

    nvm_file_close(&g_nvmf);
    can_driver_deinit(&can);
    printf("[ecu] exit\n");
    return 0;
}
