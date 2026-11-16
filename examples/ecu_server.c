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

static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},  /* VIN，17 字节走多帧 */
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1},
    {0x0301, 0, 0, read_flash_digest, 0, UDS_SESSION_DEFAULT, 0},  /* 刷写摘要 */
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

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--if") == 0 && i + 1 < argc)
            ifname = argv[++i];
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
    uds_init(&srv, &ucfg);

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
            if (isotp_poll_at(&tp, &can, now_ms32()) <= 0)
                break;
        }

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
                    if (isotp_poll_at(&tp, &can, now_ms32()) == ISOTP_ERR_TIMEOUT)
                    {
                        printf("[ecu] tx timeout, transfer aborted\n");
                        break;
                    }
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
