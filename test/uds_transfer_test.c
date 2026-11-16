#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/uds/uds.h"

/*
 * 0x34 / 0x36 / 0x37 固件传输测试。
 *
 * 存储器用内存模拟，但刻意保留了真实 Flash 的一条约束：
 * **只能把 1 写成 0，未擦除区域写入会失败**。
 * 这条约束能顺带验出两个容易写错的地方：
 *   - 0x34 有没有真的擦除
 *   - 重传的块有没有被重复写入（第二次写必然失败）
 *
 * 输出一律英文：中文常量在嵌入式工具链上容易编码错乱。
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

/* ---------------- 存储器模拟 ---------------- */

#define MEM_BASE 0x08000000u
#define MEM_SIZE 1024u

static uint8_t g_mem[MEM_SIZE];
static int g_fail_write = 0;
static int g_write_calls = 0;
static int g_erase_calls = 0;

static int mem_write(void *ctx, uint32_t addr, const uint8_t *data, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    g_write_calls++;

    if (g_fail_write)
        return -1;

    if (addr < MEM_BASE)
        return -1;

    off = addr - MEM_BASE;
    if (off + len > MEM_SIZE)
        return -1;

    /* 只能 1 -> 0：未擦除的区域写不进去 */
    for (uint16_t i = 0; i < len; i++)
    {
        if ((g_mem[off + i] & data[i]) != data[i])
            return -1;
    }

    memcpy(&g_mem[off], data, len);
    return 0;
}

static int mem_read(void *ctx, uint32_t addr, uint8_t *data, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < MEM_BASE)
        return -1;

    off = addr - MEM_BASE;
    if (off + len > MEM_SIZE)
        return -1;

    memcpy(data, &g_mem[off], len);
    return 0;
}

static int mem_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint32_t off;
    (void)ctx;

    g_erase_calls++;

    if (addr < MEM_BASE)
        return -1;

    off = addr - MEM_BASE;
    if (off + len > MEM_SIZE)
        return -1;

    memset(&g_mem[off], 0xFF, len);
    return 0;
}

static const uds_memory_t g_memif = {
    mem_write, mem_read, mem_erase, 0, MEM_BASE, MEM_SIZE
};

/* ---------------- 测试脚手架 ---------------- */

static uint32_t calc_key(uint32_t seed)
{
    return (uint32_t)(((seed ^ 0x5A5A5A5Au) + 0x12345678u) & 0xFFFFFFFFu);
}

static int do_unlock(uds_server_t *srv)
{
    uint8_t resp[64];
    uint8_t req[2] = {UDS_SID_SECURITY_ACCESS, 0x01};
    int n = uds_handle_request(srv, req, 2, resp, sizeof(resp));
    uint32_t seed, key;
    uint8_t req2[6];

    if (n != 6 || resp[0] != 0x67)
        return 0;

    seed = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) |
           ((uint32_t)resp[4] << 8) | (uint32_t)resp[5];
    key = calc_key(seed);

    req2[0] = UDS_SID_SECURITY_ACCESS;
    req2[1] = 0x02;
    req2[2] = (uint8_t)(key >> 24);
    req2[3] = (uint8_t)(key >> 16);
    req2[4] = (uint8_t)(key >> 8);
    req2[5] = (uint8_t)key;

    n = uds_handle_request(srv, req2, 6, resp, sizeof(resp));
    return (n == 2 && resp[0] == 0x67 && resp[1] == 0x02);
}

static void setup(uds_server_t *srv, uds_config_t *cfg, int with_mem)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mem = with_mem ? &g_memif : 0;

    memset(g_mem, 0xFF, sizeof(g_mem));
    g_fail_write = 0;
    g_write_calls = 0;
    g_erase_calls = 0;

    uds_init(srv, cfg);
}

/* 进扩展会话并解锁，后续所有传输测试都以此为起点 */
static void ready(uds_server_t *srv)
{
    uint8_t resp[64];
    uint8_t r[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};

    uds_handle_request(srv, r, 2, resp, sizeof(resp));
    do_unlock(srv);
}

static int req_download(uds_server_t *srv, uint32_t addr, uint32_t size,
                        uint8_t *resp, uint16_t rsize)
{
    uint8_t req[11];

    req[0] = UDS_SID_REQUEST_DOWNLOAD;
    req[1] = 0x00; /* dataFormatIdentifier：无压缩无加密 */
    req[2] = 0x44; /* 4 字节地址 + 4 字节长度 */
    req[3] = (uint8_t)(addr >> 24);
    req[4] = (uint8_t)(addr >> 16);
    req[5] = (uint8_t)(addr >> 8);
    req[6] = (uint8_t)addr;
    req[7] = (uint8_t)(size >> 24);
    req[8] = (uint8_t)(size >> 16);
    req[9] = (uint8_t)(size >> 8);
    req[10] = (uint8_t)size;

    return uds_handle_request(srv, req, sizeof(req), resp, rsize);
}

static int req_transfer(uds_server_t *srv, uint8_t bsc,
                        const uint8_t *data, uint16_t dlen,
                        uint8_t *resp, uint16_t rsize)
{
    uint8_t req[600];

    req[0] = UDS_SID_TRANSFER_DATA;
    req[1] = bsc;
    memcpy(&req[2], data, dlen);

    return uds_handle_request(srv, req, (uint16_t)(dlen + 2), resp, rsize);
}

/* ---------------- 用例 ---------------- */

/* 完整刷写：256 字节分两块，写完逐字节校验 */
static void test_full_transfer(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[300];
    uint8_t payload[256];
    int n;

    printf("Full transfer (256 bytes, 2 blocks)\n");

    setup(&srv, &cfg, 1);
    ready(&srv);

    check(uds_get_state(&srv)->security_level != 0, "unlocked");

    n = req_download(&srv, MEM_BASE + 16, 256, resp, sizeof(resp));
    check(n == 4, "0x34 returns 4 bytes");
    check(resp[0] == 0x74, "positive response id is 0x74");
    check(resp[1] == 0x20, "lengthFormatIdentifier says 2 bytes");
    check(((resp[2] << 8) | resp[3]) == 256, "maxBlockLength is 256");
    check(g_erase_calls == 1, "erase was called once");

    {
        const uds_transfer_t *t = uds_get_transfer(&srv);
        check(t->active == 1, "transfer marked active");
        check(t->total == 256, "total length recorded");
        check(t->received == 0, "nothing received yet");
        check(t->bsc_next == 1, "first expected block is 1");
    }

    for (int i = 0; i < 256; i++)
        payload[i] = (uint8_t)(i ^ 0x3C);

    /* 第一块：254 字节（max_block_len 256 - SID - BSC） */
    n = req_transfer(&srv, 1, payload, 254, resp, sizeof(resp));
    check(n == 2 && resp[0] == 0x76 && resp[1] == 0x01, "block 1 accepted, echoed");

    /* 第二块：剩余 2 字节 */
    n = req_transfer(&srv, 2, &payload[254], 2, resp, sizeof(resp));
    check(n == 2 && resp[0] == 0x76 && resp[1] == 0x02, "block 2 accepted");

    {
        const uds_transfer_t *t = uds_get_transfer(&srv);
        check(t->received == 256, "all 256 bytes received");
        check(t->blocks == 2, "2 blocks counted");
    }

    /* 退出 */
    {
        uint8_t req[1] = {UDS_SID_TRANSFER_EXIT};
        n = uds_handle_request(&srv, req, 1, resp, sizeof(resp));
        check(n == 1 && resp[0] == 0x77, "0x37 returns 0x77");
    }

    check(uds_get_transfer(&srv)->active == 0, "transfer cleared after exit");

    /* 真正的证据：写进存储器的内容逐字节正确 */
    check(memcmp(&g_mem[16], payload, 256) == 0, "memory content matches payload");
    check(g_mem[15] == 0xFF, "byte before range untouched");
    check(g_mem[16 + 256] == 0xFF, "byte after range untouched");
}

/* 前置条件：没有存储器 / 会话不对 / 没解锁 */
static void test_preconditions(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    int n;

    printf("\nPreconditions\n");

    /* 没有存储器接口 -> 服务不支持，而不是假装成功 */
    setup(&srv, &cfg, 0);
    ready(&srv);
    n = req_download(&srv, MEM_BASE, 16, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SERVICE_NOT_SUPPORTED,
          "no memory interface -> NRC 0x11");

    /* 默认会话 */
    setup(&srv, &cfg, 1);
    n = req_download(&srv, MEM_BASE, 16, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT,
          "default session -> NRC 0x22");

    /* 扩展会话但没解锁 */
    {
        uint8_t r[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};
        uds_handle_request(&srv, r, 2, resp, sizeof(resp));
    }
    n = req_download(&srv, MEM_BASE, 16, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SECURITY_DENIED,
          "extended but locked -> NRC 0x33");
}

/* 0x34 的参数格式错误 */
static void test_download_params(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t req[16];
    int n;

    printf("\nRequestDownload parameter validation\n");

    setup(&srv, &cfg, 1);
    ready(&srv);

    /* 长度不足 */
    {
        uint8_t short_req[2] = {UDS_SID_REQUEST_DOWNLOAD, 0x00};
        n = uds_handle_request(&srv, short_req, 2, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "too short -> NRC 0x13");
    }

    /* ALFID 里地址长度写 0 */
    memset(req, 0, sizeof(req));
    req[0] = UDS_SID_REQUEST_DOWNLOAD;
    req[1] = 0x00;
    req[2] = 0x40; /* 地址 0 字节，长度 4 字节 */
    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "zero address length -> NRC 0x13");

    /* ALFID 声明 4+4，但实际只给了 3 字节 */
    req[2] = 0x44;
    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH,
          "length mismatch with ALFID -> NRC 0x13");

    /* dataFormatIdentifier 非 0：不支持压缩/加密，必须明确拒绝 */
    n = req_download(&srv, MEM_BASE, 16, resp, sizeof(resp));
    check(n == 4, "baseline download ok");
    {
        uint8_t d[11];
        memcpy(d, req, 0); /* 仅为清晰，下面重新构造 */
        d[0] = UDS_SID_REQUEST_DOWNLOAD;
        d[1] = 0x10; /* 声称有压缩 */
        d[2] = 0x44;
        d[3] = 0x08; d[4] = 0x00; d[5] = 0x00; d[6] = 0x00;
        d[7] = 0x00; d[8] = 0x00; d[9] = 0x00; d[10] = 0x10;
        n = uds_handle_request(&srv, d, sizeof(d), resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
              "compressed DFI rejected with NRC 0x31");
    }

    /* 长度为 0 */
    n = req_download(&srv, MEM_BASE, 0, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE, "zero size -> NRC 0x31");

    /* 地址在可写区之前 */
    n = req_download(&srv, MEM_BASE - 1, 16, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "address below base -> NRC 0x31");

    /* 越过可写区末尾 */
    n = req_download(&srv, MEM_BASE + MEM_SIZE - 8, 16, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "range crossing the end -> NRC 0x31");

    /*
     * 地址接近 uint32 上限：如果实现用 addr + total 判越界，
     * 加法会溢出成一个很小的数，检查反而通过。
     * 正确写法是先减后比。
     */
    n = req_download(&srv, 0xFFFFFFF0u, 0x20, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "overflowing addr+size rejected (subtract-then-compare)");
}

/* 0x36 的序号与顺序 */
static void test_transfer_sequence(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int n;

    printf("\nTransferData sequence\n");

    setup(&srv, &cfg, 1);
    ready(&srv);

    /* 没有 0x34 就发 0x36 */
    n = req_transfer(&srv, 1, data, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
          "0x36 without 0x34 -> NRC 0x24");

    /* 开始传输 */
    req_download(&srv, MEM_BASE, 20, resp, sizeof(resp));

    /* 序号跳号：期望 1，发了 2 */
    n = req_transfer(&srv, 2, data, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_WRONG_BLOCK_SEQ, "skipped block -> NRC 0x73");

    /* 正确的序号 */
    n = req_transfer(&srv, 1, data, 8, resp, sizeof(resp));
    check(n == 2 && resp[0] == 0x76, "correct block accepted");
    check(uds_get_transfer(&srv)->received == 8, "8 bytes recorded");

    /*
     * 重传：同一个序号再来一次（tester 没收到响应）。
     * 必须重发正响应，且不能再写一次存储器 —— 内存模拟里
     * 重复写同一位置会失败（1 已经变成 0），正好能验出来。
     */
    {
        int calls_before = g_write_calls;
        n = req_transfer(&srv, 1, data, 8, resp, sizeof(resp));
        check(n == 2 && resp[1] == 0x01, "retransmit acknowledged");
        check(g_write_calls == calls_before, "retransmit did not write again");
        check(uds_get_transfer(&srv)->received == 8, "received count unchanged");
    }

    /* 单块超过声明的总长度 */
    n = req_transfer(&srv, 2, data, 20, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_TRANSFER_SUSPENDED,
          "block larger than remaining -> NRC 0x71");

    /* 正常收尾 */
    n = req_transfer(&srv, 2, data, 12, resp, sizeof(resp));
    check(n == 2 && resp[1] == 0x02, "final block fills the range");
    check(uds_get_transfer(&srv)->received == 20, "20 bytes total");

    /* 退出后再发 0x36 */
    {
        uint8_t r[1] = {UDS_SID_TRANSFER_EXIT};
        uds_handle_request(&srv, r, 1, resp, sizeof(resp));
    }
    n = req_transfer(&srv, 3, data, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
          "0x36 after 0x37 -> NRC 0x24");
}

/* 块序号 0xFF 之后回绕到 0x00 */
static void test_block_sequence_wrap(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t one = 0x5A;
    int n;
    int ok = 1;

    printf("\nBlock sequence wrap (0xFF -> 0x00)\n");

    setup(&srv, &cfg, 1);
    ready(&srv);

    /* 256 个单字节块：序号 1..255，然后 0，然后 1 */
    n = req_download(&srv, MEM_BASE, 257, resp, sizeof(resp));
    check(n == 4, "download of 257 bytes accepted");

    for (int i = 0; i < 255; i++)
    {
        n = req_transfer(&srv, (uint8_t)(i + 1), &one, 1, resp, sizeof(resp));
        if (n != 2)
            ok = 0;
    }
    check(ok, "blocks 1..255 accepted");

    /* 第 256 块序号是 0，不是 1 —— 写成 1 会被判成重传，数据永远少一块 */
    n = req_transfer(&srv, 0x00, &one, 1, resp, sizeof(resp));
    check(n == 2 && resp[1] == 0x00, "block 256 uses sequence 0");

    n = req_transfer(&srv, 0x01, &one, 1, resp, sizeof(resp));
    check(n == 2 && resp[1] == 0x01, "sequence restarts at 1 after wrap");

    check(uds_get_transfer(&srv)->received == 257, "all 257 bytes received");
}

/* 存储器写入失败 */
static void test_write_failure(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int n;

    printf("\nWrite failure\n");

    setup(&srv, &cfg, 1);
    ready(&srv);
    req_download(&srv, MEM_BASE, 8, resp, sizeof(resp));

    g_fail_write = 1;
    n = req_transfer(&srv, 1, data, 4, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_GENERAL_PROGRAM_FAIL,
          "write error -> NRC 0x72");

    /* 失败的块不能被计入：否则后续字节会写到错误偏移 */
    check(uds_get_transfer(&srv)->received == 0, "failed block not counted");

    g_fail_write = 0;
    n = req_transfer(&srv, 1, data, 4, resp, sizeof(resp));
    check(n == 2 && resp[1] == 0x01, "retry with same sequence succeeds");
    check(uds_get_transfer(&srv)->received == 4, "4 bytes counted");
}

int main(void)
{
    printf("=== UDS transfer test (0x34 / 0x36 / 0x37) ===\n\n");

    test_full_transfer();
    test_preconditions();
    test_download_params();
    test_transfer_sequence();
    test_block_sequence_wrap();
    test_write_failure();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
