#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/uds/uds.h"

/*
 * 0x31 例程控制测试。
 *
 * 覆盖两类：内置例程（擦除 / CRC 校验）和自定义例程表。
 *
 * CRC 的期望值不是「用被测代码算一遍再比对」——那是自证。
 * 这里用的是 zlib.crc32 独立算出的定值。
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
static int g_fail_erase = 0;
static int g_erase_calls = 0;

static int mem_write(void *ctx, uint32_t addr, const uint8_t *d, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < MEM_BASE)
        return -1;
    off = addr - MEM_BASE;
    if (off + len > MEM_SIZE)
        return -1;

    for (uint16_t i = 0; i < len; i++)
    {
        if ((g_mem[off + i] & d[i]) != d[i])
            return -1;
    }

    memcpy(&g_mem[off], d, len);
    return 0;
}

static int mem_read(void *ctx, uint32_t addr, uint8_t *d, uint16_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < MEM_BASE)
        return -1;
    off = addr - MEM_BASE;
    if (off + len > MEM_SIZE)
        return -1;

    memcpy(d, &g_mem[off], len);
    return 0;
}

static int mem_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint32_t off;
    (void)ctx;

    g_erase_calls++;

    if (g_fail_erase)
        return -1;

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

/* ---------------- 自定义例程 ---------------- */

static int g_custom_started = 0;
static int g_custom_stopped = 0;
static int g_custom_fail = 0;
static uint16_t g_custom_opt_len = 0;

static int custom_start(void *ctx, const uint8_t *opt, uint16_t opt_len)
{
    (void)ctx;
    (void)opt;

    if (g_custom_fail)
        return -1;

    g_custom_started++;
    g_custom_opt_len = opt_len;
    return 0;
}

static int custom_stop(void *ctx)
{
    (void)ctx;
    g_custom_stopped++;
    return 0;
}

static int custom_result(void *ctx, uint8_t *out, uint16_t *out_len)
{
    (void)ctx;

    out[0] = 0x12;
    out[1] = 0x34;
    *out_len = 2;
    return 0;
}

static const uds_routine_entry_t g_routines[] = {
    {0x1234, custom_start, custom_stop, custom_result, UDS_SESSION_EXTENDED, 1, 0},
};

/* ---------------- 脚手架 ---------------- */

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

static void setup(uds_server_t *srv, uds_config_t *cfg, int with_mem, int with_routines)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mem = with_mem ? &g_memif : 0;

    if (with_routines)
    {
        cfg->routines = g_routines;
        cfg->routine_count = (uint16_t)(sizeof(g_routines) / sizeof(g_routines[0]));
    }

    memset(g_mem, 0xFF, sizeof(g_mem));
    g_fail_erase = 0;
    g_erase_calls = 0;
    g_custom_started = 0;
    g_custom_stopped = 0;
    g_custom_fail = 0;
    g_custom_opt_len = 0;

    uds_init(srv, cfg);
}

static void ready(uds_server_t *srv)
{
    uint8_t resp[64];
    uint8_t r[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};

    uds_handle_request(srv, r, 2, resp, sizeof(resp));
    do_unlock(srv);
}

/* 构造 0x31 请求：sub + rid + 可选参数 */
static int req_routine(uds_server_t *srv, uint8_t sub, uint16_t rid,
                       const uint8_t *opt, uint16_t opt_len,
                       uint8_t *resp, uint16_t rsize)
{
    uint8_t req[64];

    req[0] = UDS_SID_ROUTINE_CONTROL;
    req[1] = sub;
    req[2] = (uint8_t)(rid >> 8);
    req[3] = (uint8_t)rid;

    if (opt_len > 0)
        memcpy(&req[4], opt, opt_len);

    return uds_handle_request(srv, req, (uint16_t)(4 + opt_len), resp, rsize);
}

/* addr + size 参数，大端 */
static void put_addr_size(uint8_t *opt, uint32_t addr, uint32_t size)
{
    opt[0] = (uint8_t)(addr >> 24);
    opt[1] = (uint8_t)(addr >> 16);
    opt[2] = (uint8_t)(addr >> 8);
    opt[3] = (uint8_t)addr;
    opt[4] = (uint8_t)(size >> 24);
    opt[5] = (uint8_t)(size >> 16);
    opt[6] = (uint8_t)(size >> 8);
    opt[7] = (uint8_t)size;
}

/* ---------------- 用例 ---------------- */

static void test_erase(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    int n;

    printf("Built-in routine 0xFF00 (erase memory)\n");

    setup(&srv, &cfg, 1, 0);
    ready(&srv);

    /* 先写点东西进去，证明擦除真的生效 */
    memset(&g_mem[32], 0x5A, 64);

    put_addr_size(opt, MEM_BASE + 32, 64);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));

    check(n == 4, "0x31 start returns 4 bytes");
    check(resp[0] == 0x71, "positive response id is 0x71");
    check(resp[1] == UDS_ROUTINE_START, "sub-function echoed");
    check(resp[2] == 0xFF && resp[3] == 0x00, "routine id echoed");
    check(g_erase_calls == 1, "erase called once");

    {
        int all_ff = 1;

        for (int i = 0; i < 64; i++)
            if (g_mem[32 + i] != 0xFF)
                all_ff = 0;

        check(all_ff, "target range erased to 0xFF");
        check(g_mem[31] == 0xFF && g_mem[96] == 0xFF, "adjacent bytes untouched");
    }
}

static void test_check_memory(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    uint32_t got_crc;
    int n;

    printf("\nBuilt-in routine 0x0202 (check memory CRC)\n");

    setup(&srv, &cfg, 1, 0);
    ready(&srv);

    /* 写入一段确定的数据：与 zlib.crc32 核对过的 256 字节 */
    for (int i = 0; i < 256; i++)
        g_mem[i] = (uint8_t)((i * 37 + 11) & 0xFF);

    put_addr_size(opt, MEM_BASE, 256);

    /* start 只负责算，响应不带结果 */
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_CHECK_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 4 && resp[0] == 0x71, "0x31 start accepted");

    {
        const uds_routine_result_t *r = uds_get_routine_result(&srv);
        check(r->valid == 1, "result cached after start");
        check(r->size == 256, "size recorded");
    }

    /* requestResults 取结果 */
    n = req_routine(&srv, UDS_ROUTINE_RESULT, UDS_RID_CHECK_MEMORY, 0, 0, resp, sizeof(resp));
    check(n == 8, "result returns 8 bytes (header + crc)");
    check(resp[1] == UDS_ROUTINE_RESULT, "sub-function echoed");

    got_crc = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
              ((uint32_t)resp[6] << 8) | (uint32_t)resp[7];

    /* 0x8ED7A350 由 zlib.crc32 独立算出，不是拿被测代码自证 */
    check(got_crc == 0x8ED7A350u, "CRC32 matches zlib's value");

    /* 改一个字节，CRC 必须变 */
    g_mem[100] ^= 0x01;
    req_routine(&srv, UDS_ROUTINE_START, UDS_RID_CHECK_MEMORY, opt, 8, resp, sizeof(resp));
    req_routine(&srv, UDS_ROUTINE_RESULT, UDS_RID_CHECK_MEMORY, 0, 0, resp, sizeof(resp));

    {
        uint32_t changed = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                           ((uint32_t)resp[6] << 8) | (uint32_t)resp[7];

        check(changed != got_crc, "one flipped byte changes the CRC");
    }
}

static void test_result_before_start(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    int n;

    printf("\nResult before start\n");

    setup(&srv, &cfg, 1, 0);
    ready(&srv);

    n = req_routine(&srv, UDS_ROUTINE_RESULT, UDS_RID_CHECK_MEMORY, 0, 0, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_SEQUENCE_ERROR,
          "requestResults before start -> NRC 0x24");
}

static void test_permissions(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    int n;

    printf("\nPermissions\n");

    /* 默认会话 */
    setup(&srv, &cfg, 1, 0);
    put_addr_size(opt, MEM_BASE, 16);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT,
          "default session -> NRC 0x22");

    /* 扩展会话但没解锁 */
    {
        uint8_t r[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};
        uds_handle_request(&srv, r, 2, resp, sizeof(resp));
    }
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SECURITY_DENIED, "locked -> NRC 0x33");
}

static void test_parameter_validation(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    int n;

    printf("\nParameter validation\n");

    setup(&srv, &cfg, 1, 0);
    ready(&srv);

    /* 请求长度不足 4 */
    {
        uint8_t short_req[3] = {UDS_SID_ROUTINE_CONTROL, 0x01, 0xFF};
        n = uds_handle_request(&srv, short_req, 3, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "too short -> NRC 0x13");
    }

    /* 子功能非法 */
    n = req_routine(&srv, 0x04, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SUB_FUNCTION_INVALID,
          "bad sub-function -> NRC 0x12");

    /* 例程不存在 */
    n = req_routine(&srv, UDS_ROUTINE_START, 0x9999, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "unknown routine -> NRC 0x31");

    /* 参数长度不对（内置例程要求恰好 8 字节） */
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 4, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "short option record -> NRC 0x31");

    /* 越界 */
    put_addr_size(opt, MEM_BASE + MEM_SIZE - 4, 16);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "range crossing the end -> NRC 0x31");

    /* 地址在区之前 */
    put_addr_size(opt, MEM_BASE - 8, 8);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "address below base -> NRC 0x31");

    /* 长度为 0 */
    put_addr_size(opt, MEM_BASE, 0);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE, "zero size -> NRC 0x31");

    /* 溢出地址：先减后比才不会绕过 */
    put_addr_size(opt, 0xFFFFFFF0u, 0x20);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "overflowing addr+size rejected");
}

static void test_erase_failure(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    int n;

    printf("\nErase failure\n");

    setup(&srv, &cfg, 1, 0);
    ready(&srv);

    put_addr_size(opt, MEM_BASE, 32);
    g_fail_erase = 1;
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));

    check(n == 3 && resp[2] == UDS_NRC_GENERAL_PROGRAM_FAIL, "erase error -> NRC 0x72");
}

static void test_custom_routine(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    int n;

    printf("\nCustom routine\n");

    setup(&srv, &cfg, 0, 1); /* 不配存储器，只测自定义表 */
    ready(&srv);

    /* start，带 3 字节参数 */
    {
        uint8_t opt[3] = {0xA1, 0xA2, 0xA3};
        n = req_routine(&srv, UDS_ROUTINE_START, 0x1234, opt, 3, resp, sizeof(resp));
    }
    check(n == 4 && resp[0] == 0x71, "custom start accepted");
    check(g_custom_started == 1, "start callback ran");
    check(g_custom_opt_len == 3, "option record passed through unchanged");

    /* result */
    n = req_routine(&srv, UDS_ROUTINE_RESULT, 0x1234, 0, 0, resp, sizeof(resp));
    check(n == 6 && resp[4] == 0x12 && resp[5] == 0x34, "custom result returned");

    /* stop */
    n = req_routine(&srv, UDS_ROUTINE_STOP, 0x1234, 0, 0, resp, sizeof(resp));
    check(n == 4, "custom stop accepted");
    check(g_custom_stopped == 1, "stop callback ran");

    /* 回调失败 -> NRC 0x72 */
    g_custom_fail = 1;
    n = req_routine(&srv, UDS_ROUTINE_START, 0x1234, 0, 0, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_GENERAL_PROGRAM_FAIL,
          "callback failure -> NRC 0x72");
}

static void test_no_memory_no_builtin(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t opt[8];
    int n;

    printf("\nBuilt-in routines need a memory interface\n");

    /* 没有 mem：内置 rid 视为不存在 */
    setup(&srv, &cfg, 0, 0);
    ready(&srv);

    put_addr_size(opt, MEM_BASE, 16);
    n = req_routine(&srv, UDS_ROUTINE_START, UDS_RID_ERASE_MEMORY, opt, 8, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "no memory -> built-in routine not offered");
}

int main(void)
{
    printf("=== UDS routine control test (0x31) ===\n\n");

    test_erase();
    test_check_memory();
    test_result_before_start();
    test_permissions();
    test_parameter_validation();
    test_erase_failure();
    test_custom_routine();
    test_no_memory_no_builtin();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
