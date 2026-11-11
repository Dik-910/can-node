#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/uds/uds.h"

/*
 * UDS 服务层测试。
 *
 * 覆盖四类东西：
 *   1. 服务分派与长度校验
 *   2. 会话权限：默认会话能做什么、扩展会话能做什么
 *   3. 安全访问：正确密钥、错误密钥、连续失败锁定、冷却
 *   4. S3 会话超时与权限回收
 *
 * 输出字符串一律英文（见 MEMORY 里的编码约定），注释保留中文。
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

static void hex_dump(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02X%s", b[i], (i + 1 < n) ? " " : "");
    printf("\n");
}

/* ---------------- 被测对象用到的假数据 ---------------- */

static uint16_t g_voltage_mv = 3660;  /* 动态 DID：电压 */
static uint8_t g_threshold = 50;      /* 可写 DID：保护阈值 */
static const uint8_t g_vin[17] = {'L', 'S', 'V', 'A', 'A', '1', '2', '3', '4',
                                  '5', '6', '7', '8', '9', '0', 'A', 'B'};

static int read_voltage(uint8_t *out, uint8_t *out_len)
{
    out[0] = (uint8_t)(g_voltage_mv >> 8);
    out[1] = (uint8_t)(g_voltage_mv & 0xFF);
    *out_len = 2;
    return 0;
}

static int read_threshold(uint8_t *out, uint8_t *out_len)
{
    out[0] = g_threshold;
    *out_len = 1;
    return 0;
}

static int write_threshold(const uint8_t *in, uint8_t len)
{
    if (len != 1)
        return -1;
    g_threshold = in[0];
    return 0;
}

/* DID 表：覆盖静态、动态、可写、需解锁四种情况 */
static const uds_did_entry_t g_dids[] = {
    {0xF190, 17, g_vin, 0, 0, UDS_SESSION_DEFAULT, 0},      /* VIN，默认会话可读 */
    {0x0101, 0, 0, read_voltage, 0, UDS_SESSION_DEFAULT, 0},/* 电压，动态 */
    {0x0201, 0, 0, read_threshold, write_threshold, UDS_SESSION_EXTENDED, 1}, /* 阈值，需扩展+解锁 */
    {0x0301, 0, 0, read_voltage, 0, UDS_SESSION_EXTENDED, 1},/* 需解锁才能读 */
};

#define DID_COUNT (sizeof(g_dids) / sizeof(g_dids[0]))

static void setup(uds_server_t *srv)
{
    uds_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.dids = g_dids;
    cfg.did_count = (uint16_t)DID_COUNT;
    uds_init(srv, &cfg);

    g_voltage_mv = 3660;
    g_threshold = 50;
}

/* 按默认算法算密钥：与 uds.c 里的 default_key 保持一致 */
static uint32_t calc_key(uint32_t seed)
{
    return (uint32_t)(((seed ^ 0x5A5A5A5Au) + 0x12345678u) & 0xFFFFFFFFu);
}

/* 走完「请求种子 -> 算密钥 -> 解锁」，返回 1 表示成功 */
static int do_unlock(uds_server_t *srv, uint8_t sub)
{
    uint8_t resp[64];
    uint8_t req[2] = {UDS_SID_SECURITY_ACCESS, sub};
    int n = uds_handle_request(srv, req, 2, resp, sizeof(resp));

    if (n != 6 || resp[0] != 0x67)
        return 0;

    uint32_t seed = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) |
                    ((uint32_t)resp[4] << 8) | (uint32_t)resp[5];
    uint32_t key = calc_key(seed);

    req[0] = UDS_SID_SECURITY_ACCESS;
    req[1] = (uint8_t)(sub + 1);
    uint8_t req2[6] = {UDS_SID_SECURITY_ACCESS, (uint8_t)(sub + 1),
                       (uint8_t)(key >> 24), (uint8_t)(key >> 16),
                       (uint8_t)(key >> 8), (uint8_t)key};
    n = uds_handle_request(srv, req2, 6, resp, sizeof(resp));

    return (n == 2 && resp[0] == 0x67 && resp[1] == (uint8_t)(sub + 1));
}

static int go_extended(uds_server_t *srv)
{
    uint8_t resp[64];
    uint8_t req[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};
    int n = uds_handle_request(srv, req, 2, resp, sizeof(resp));
    return (n == 6 && resp[0] == 0x50 && resp[1] == UDS_SESSION_EXTENDED);
}

/* ---------------- 分派与长度 ---------------- */

static void test_dispatch(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Service dispatch and length checks\n");
    setup(&srv);

    {
        uint8_t req[1] = {0x99};
        n = uds_handle_request(&srv, req, 1, resp, sizeof(resp));
        check(n == 3 && resp[0] == 0x7F && resp[1] == 0x99 && resp[2] == UDS_NRC_SERVICE_NOT_SUPPORTED,
              "unknown service returns NRC 0x11");
    }

    {
        uint8_t req[1] = {UDS_SID_SESSION_CONTROL};
        n = uds_handle_request(&srv, req, 1, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "short request returns NRC 0x13");
    }

    {
        uint8_t req[2] = {UDS_SID_SESSION_CONTROL, 0x7A};
        n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_SUB_FUNCTION_INVALID, "bad sub-function returns NRC 0x12");
    }
}

/* ---------------- 会话控制 ---------------- */

static void test_session(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Session control\n");
    setup(&srv);

    {
        uint8_t req[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};
        n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
        printf("  response: ");
        hex_dump(resp, n);
        check(n == 6, "extended session returns 6 bytes");
        check(resp[0] == 0x50 && resp[1] == UDS_SESSION_EXTENDED, "positive response 0x50 03");
        check(uds_get_state(&srv)->session == UDS_SESSION_EXTENDED, "state switched");
        check(uds_get_state(&srv)->s3_remain_ms == UDS_S3_TIMEOUT_MS, "s3 timer armed");
    }

    {
        uint8_t req[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_PROGRAMMING};
        n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_SECURITY_DENIED,
              "programming session denied without unlock");
    }

    {
        uint8_t req[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_DEFAULT};
        n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
        check(n == 6 && resp[1] == UDS_SESSION_DEFAULT, "back to default session");
    }
}

/* ---------------- 读数据 ---------------- */

static void test_read_data(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Read data by identifier\n");
    setup(&srv);

    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0xF1, 0x90};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 3 + 17, "static DID returns 3 + 17 bytes");
        check(resp[0] == 0x62 && resp[1] == 0xF1 && resp[2] == 0x90, "positive response 0x62");
        check(memcmp(&resp[3], g_vin, 17) == 0, "VIN payload matches");
    }

    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x01, 0x01};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 5 && resp[3] == 0x0E && resp[4] == 0x4C, "dynamic DID reads 3660");
    }

    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x09, 0x99};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE, "missing DID returns NRC 0x31");
    }

    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x03, 0x01};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT,
              "extended-only DID rejected in default session (NRC 0x22)");
    }

    /* 一次读两个 DID，响应应拼接 */
    {
        uint8_t req[5] = {UDS_SID_READ_DATA, 0x01, 0x01, 0xF1, 0x90};
        n = uds_handle_request(&srv, req, 5, resp, sizeof(resp));
        check(n == 1 + 2 + 2 + 2 + 17, "two DIDs concatenated");
        check(resp[3] == 0x0E && resp[4] == 0x4C, "first DID payload correct");
        check(memcmp(&resp[7], g_vin, 17) == 0, "second DID payload correct");
    }

    /* 会话对了但没解锁，此时才是安全拒绝 */
    check(go_extended(&srv) == 1, "enter extended session");
    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x03, 0x01};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_SECURITY_DENIED,
              "extended DID requires unlock (NRC 0x33)");
    }
    check(do_unlock(&srv, 0x01) == 1, "unlock for the next step");
    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x03, 0x01};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 5 && resp[0] == 0x62, "readable after unlock");
    }
}

/* ---------------- 安全访问 ---------------- */

static void test_security(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Security access\n");
    setup(&srv);

    check(do_unlock(&srv, 0x01) == 1, "correct key unlocks");
    check(uds_get_state(&srv)->security_level == 1, "security level is 1");

    /* 错误密钥 */
    setup(&srv);
    {
        uint8_t req[2] = {UDS_SID_SECURITY_ACCESS, 0x01};
        uds_handle_request(&srv, req, 2, resp, sizeof(resp));

        uint8_t wrong[6] = {UDS_SID_SECURITY_ACCESS, 0x02, 0xDE, 0xAD, 0xBE, 0xEF};
        n = uds_handle_request(&srv, wrong, 6, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_INVALID_KEY, "wrong key returns NRC 0x35");
        check(uds_get_state(&srv)->security_level == 0, "still locked");
    }

    /* 连续三次失败 -> 锁定 */
    setup(&srv);
    {
        uint8_t resp2[64];
        for (int i = 0; i < UDS_MAX_ATTEMPTS; i++)
        {
            uint8_t seed_req[2] = {UDS_SID_SECURITY_ACCESS, 0x01};
            uds_handle_request(&srv, seed_req, 2, resp2, sizeof(resp2));
            uint8_t wrong[6] = {UDS_SID_SECURITY_ACCESS, 0x02, 0x11, 0x22, 0x33, 0x44};
            n = uds_handle_request(&srv, wrong, 6, resp2, sizeof(resp2));
        }
        check(n == 3 && resp2[2] == UDS_NRC_EXCEED_ATTEMPTS, "third failure returns NRC 0x36");
        check(uds_get_state(&srv)->lock_remain_ms == UDS_LOCK_MS, "locked for cooling period");
    }

    /* 锁定期间连种子都不给 */
    {
        uint8_t seed_req[2] = {UDS_SID_SECURITY_ACCESS, 0x01};
        n = uds_handle_request(&srv, seed_req, 2, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_REQUIRED_TIME_DELAY,
              "locked out returns NRC 0x37");
    }

    /* 冷却结束后恢复 */
    uds_tick(&srv, UDS_LOCK_MS);
    check(uds_get_state(&srv)->lock_remain_ms == 0, "lock released after timeout");
    check(do_unlock(&srv, 0x01) == 1, "unlock works again after cooldown");
}

/* ---------------- 写数据 ---------------- */

static void test_write_data(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Write data by identifier\n");
    setup(&srv);

    /* 默认会话写 -> 条件不满足 */
    {
        uint8_t req[4] = {UDS_SID_WRITE_DATA, 0x02, 0x01, 0x3C};
        n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT,
              "write in default session returns NRC 0x22");
    }

    /* 扩展会话但未解锁 -> 安全拒绝 */
    check(go_extended(&srv) == 1, "enter extended session");
    {
        uint8_t req[4] = {UDS_SID_WRITE_DATA, 0x02, 0x01, 0x3C};
        n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_SECURITY_DENIED, "write without unlock returns NRC 0x33");
    }

    /* 解锁后写入成功 */
    check(do_unlock(&srv, 0x01) == 1, "unlock succeeds");
    {
        uint8_t req[4] = {UDS_SID_WRITE_DATA, 0x02, 0x01, 0x3C};
        n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
        check(n == 3 && resp[0] == 0x6E, "write returns positive 0x6E");
        check(g_threshold == 0x3C, "threshold actually updated to 60");
    }

    /* 读回验证 */
    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0x02, 0x01};
        n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
        check(n == 4 && resp[3] == 0x3C, "read back shows new value");
    }

    /* 写只读 DID */
    {
        uint8_t req[4] = {UDS_SID_WRITE_DATA, 0x01, 0x01, 0x00};
        n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE, "write to read-only DID rejected");
    }
}

/* ---------------- S3 超时 ---------------- */

static void test_s3_timeout(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("S3 session timeout\n");
    setup(&srv);

    check(go_extended(&srv) == 1, "enter extended session");
    check(do_unlock(&srv, 0x01) == 1, "unlock");

    /* 未超时前保持扩展会话 */
    uds_tick(&srv, UDS_S3_TIMEOUT_MS - 1000);
    check(uds_get_state(&srv)->session == UDS_SESSION_EXTENDED, "still extended before timeout");

    /* 任意请求都算心跳，不只是 0x3E */
    {
        uint8_t req[3] = {UDS_SID_READ_DATA, 0xF1, 0x90};
        uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    }
    uds_tick(&srv, UDS_S3_TIMEOUT_MS - 1000);
    check(uds_get_state(&srv)->session == UDS_SESSION_EXTENDED,
          "read request also refreshes s3");

    /* 超时后掉回默认会话，并收回安全权限 */
    uds_tick(&srv, UDS_S3_TIMEOUT_MS);
    check(uds_get_state(&srv)->session == UDS_SESSION_DEFAULT, "dropped to default after timeout");
    check(uds_get_state(&srv)->security_level == 0, "security level revoked");

    /* 掉回后再写应被拒 */
    {
        uint8_t req[4] = {UDS_SID_WRITE_DATA, 0x02, 0x01, 0x30};
        n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
        check(n == 3 && resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT, "write blocked after timeout");
    }
}

/* ---------------- 会话保持 ---------------- */

static void test_tester_present(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    int n;

    printf("Tester present\n");
    setup(&srv);

    go_extended(&srv);
    uds_tick(&srv, UDS_S3_TIMEOUT_MS - 500);

    {
        uint8_t req[2] = {UDS_SID_TESTER_PRESENT, 0x00};
        n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
        check(n == 2 && resp[0] == 0x7E && resp[1] == 0x00, "returns 0x7E 00");
    }

    uds_tick(&srv, UDS_S3_TIMEOUT_MS - 500);
    check(uds_get_state(&srv)->session == UDS_SESSION_EXTENDED, "session kept alive");
}

int main(void)
{
    printf("=== UDS server test ===\n\n");
    test_dispatch();
    printf("\n");
    test_session();
    printf("\n");
    test_read_data();
    printf("\n");
    test_security();
    printf("\n");
    test_write_data();
    printf("\n");
    test_s3_timeout();
    printf("\n");
    test_tester_present();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
