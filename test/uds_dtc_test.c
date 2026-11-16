#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "uds.h"

/*
 * 0x19 读故障码 / 0x14 清故障码 的协议层测试。
 *
 * 重点不在「能不能返回数据」，而在响应字节的排布必须与 ISO 14229-1 一致 ——
 * 顺序错了本机测试照样能过（自己读自己写的），只有第三方 tester 会崩。
 * 所以这里逐字节比对，不比较长度。
 */

#define DTC_BUS_OFF 0xC00187u
#define DTC_PASSIVE 0xC00287u

static int passed = 0;
static int failed = 0;

/* 在缓冲区里找一段字节。不用 memmem：那是 GNU 扩展，-std=c99 下不可移植 */
static int contains(const uint8_t *hay, int hay_len, const uint8_t *needle, int n_len)
{
    for (int i = 0; i + n_len <= hay_len; i++)
    {
        if (memcmp(hay + i, needle, (size_t)n_len) == 0)
            return 1;
    }
    return 0;
}

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

static const dtc_def_t g_defs[] = {
    {DTC_BUS_OFF, "bus off"},
    {DTC_PASSIVE, "error passive"},
};

static dtc_store_t g_dtcs;

static int snap_provider(uint32_t code, uint8_t *out, uint8_t max_len)
{
    (void)code;
    if (max_len < 4)
        return 0;

    /* 模拟故障瞬间的工况：电压高字节 + TEC 高字节 */
    out[0] = 0x0E; /* 3.6V */
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x80; /* TEC = 128 */
    return 4;
}

/* 把服务器切到扩展会话，很多服务的前置条件 */
static int enter_extended(uds_server_t *srv)
{
    static uint8_t r[8];
    uint8_t req[2] = {UDS_SID_SESSION_CONTROL, UDS_SESSION_EXTENDED};
    return uds_handle_request(srv, req, sizeof(req), r, sizeof(r));
}

static int with_dtcs(uds_server_t *srv)
{
    uds_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.dtcs = &g_dtcs;

    if (uds_init(srv, &cfg) != 0)
        return -1;

    enter_extended(srv);
    return 0;
}

/* 让第一条 DTC 走完「两个循环失败」从而确认 */
static void confirm_bus_off(void)
{
    dtc_clear(&g_dtcs, DTC_GROUP_ALL);
    dtc_report(&g_dtcs, DTC_BUS_OFF, 1, 1000);
    dtc_operation_cycle(&g_dtcs);
    dtc_report(&g_dtcs, DTC_BUS_OFF, 1, 2000);
}

static void test_report_number(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[3];
    int n;

    printf("[0x19 01 report number by mask]\n");
    with_dtcs(&srv);
    dtc_clear(&g_dtcs, DTC_GROUP_ALL);

    req[0] = UDS_SID_READ_DTC;
    req[1] = UDS_DTC_REPORT_NUMBER_BY_MASK;
    req[2] = DTC_STATUS_CONFIRMED;

    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 6, "response is 6 bytes");
    check(resp[0] == 0x59 && resp[1] == 0x01, "positive response 59 01");
    check(resp[2] == DTC_STATUS_AVAILABILITY_MASK, "availability mask echoed");
    check(resp[3] == UDS_DTC_FORMAT_ISO14229, "DTC format identifier is 1");
    check(resp[4] == 0 && resp[5] == 0, "count is 0 before any fault");

    confirm_bus_off();

    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 6, "response still 6 bytes");
    check(resp[4] == 0 && resp[5] == 1, "count is 1 after the DTC is confirmed");

    /* 只要 pending 的那条码用 pending 掩码也能数到 */
    req[2] = DTC_STATUS_PENDING;
    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 6 && resp[4] == 0 && resp[5] == 1, "pending mask counts the same DTC");

    /* 长度多一字节必须拒绝，不能「容错」——容错会让 tester 的错帧被静默接受 */
    n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
    check(n == 3 && resp[0] == 0x7F && resp[2] == UDS_NRC_INCORRECT_LENGTH,
          "short request rejected with 0x13");
}

static void test_report_by_mask(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[3] = {UDS_SID_READ_DTC, UDS_DTC_REPORT_BY_MASK, DTC_STATUS_CONFIRMED};
    int n;

    printf("[0x19 02 report DTC by status mask]\n");
    with_dtcs(&srv);
    confirm_bus_off();

    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 7, "one DTC -> 3 + 4 bytes");
    check(resp[0] == 0x59 && resp[1] == 0x02 && resp[2] == DTC_STATUS_AVAILABILITY_MASK,
          "header 59 02 + availability mask");
    check(resp[3] == 0xC0 && resp[4] == 0x01 && resp[5] == 0x87,
          "DTC encoded big endian in 3 bytes");
    check((resp[6] & DTC_STATUS_CONFIRMED) != 0 &&
              (resp[6] & DTC_STATUS_TEST_FAILED) != 0,
          "status byte follows the DTC");

    /* 没有故障的码不能被掩码带出来 */
    {
        const uint8_t passive[3] = {0xC0, 0x02, 0x87};
        check(!contains(resp, n, passive, 3), "healthy DTC is not reported");
    }
}

static void test_report_supported(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[2] = {UDS_SID_READ_DTC, UDS_DTC_REPORT_SUPPORTED};
    int n;

    printf("[0x19 0A report supported DTC]\n");
    with_dtcs(&srv);
    dtc_clear(&g_dtcs, DTC_GROUP_ALL);

    n = uds_handle_request(&srv, req, 2, resp, sizeof(resp));
    check(n == 11, "two supported DTCs -> 3 + 2*4 bytes");
    check(resp[3] == 0xC0 && resp[5] == 0x87, "first supported DTC is bus off");
    check(resp[7] == 0xC0 && resp[9] == 0x87, "second supported DTC is error passive");
    check((resp[6] & DTC_STATUS_CONFIRMED) == 0, "supported does not mean failed");
    check((resp[10] & DTC_STATUS_TEST_NOT_COMPLETED_SINCE_CLEAR) != 0,
          "untested DTCs carry the 'not tested' bit");

    /* 长度不对时 NRC 0x13 */
    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "0x0A rejects extra bytes");
}

static void test_snapshot(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[6];
    int n;

    printf("[0x19 04 report snapshot]\n");
    dtc_set_snapshot_provider(&g_dtcs, snap_provider);
    with_dtcs(&srv);
    confirm_bus_off();

    req[0] = UDS_SID_READ_DTC;
    req[1] = UDS_DTC_REPORT_SNAPSHOT;
    req[2] = 0xC0;
    req[3] = 0x01;
    req[4] = 0x87;
    req[5] = UDS_DTC_RECORD_SNAPSHOT;

    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 11, "response = 7 header bytes + 4 snapshot bytes");
    check(resp[0] == 0x59 && resp[1] == 0x04, "positive response 59 04");
    check(resp[2] == 0xC0 && resp[3] == 0x01 && resp[4] == 0x87, "DTC echoed");
    check(resp[5] == dtc_find(&g_dtcs, DTC_BUS_OFF)->status, "status echoed between DTC and record");
    check(resp[6] == UDS_DTC_RECORD_SNAPSHOT, "record number normalised to 0x01");
    check(resp[7] == 0x0E && resp[10] == 0x80, "snapshot payload follows the header");

    /* 0xFF 表示「全部记录」，只有一条时行为相同 */
    req[5] = UDS_DTC_RECORD_ALL;
    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 11 && resp[6] == UDS_DTC_RECORD_SNAPSHOT,
          "record 0xFF reports record 1 for a single-record DTC");

    /* 记录号 0x02 不存在 */
    req[5] = 0x02;
    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "unknown record number -> NRC 0x31");

    /* 无故障的码没有快照 */
    req[2] = 0xC0;
    req[3] = 0x02;
    req[4] = 0x87;
    req[5] = UDS_DTC_RECORD_SNAPSHOT;
    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "DTC without snapshot -> NRC 0x31");

    /* 根本不存在的码 */
    req[2] = 0xAA;
    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_REQUEST_OUT_OF_RANGE,
          "unknown DTC -> NRC 0x31");
}

static void test_ext_data(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[6] = {UDS_SID_READ_DTC, UDS_DTC_REPORT_EXT_DATA, 0xC0, 0x01, 0x87,
                      UDS_DTC_RECORD_EXT};
    int n;

    printf("[0x19 06 report extended data]\n");
    with_dtcs(&srv);
    confirm_bus_off();

    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 17, "7 header bytes + 10 extended bytes");
    check(resp[0] == 0x59 && resp[1] == 0x06, "positive response 59 06");
    check(resp[6] == UDS_DTC_RECORD_EXT, "record number is 0x01");

    /*
     * 发生次数按「失败沿」计：中间没有通过过，说明是同一次持续故障，
     * 只算 1 次。这条容易被写成「每次失败 +1」，那样一个持续故障
     * 跑几十秒就饱和到 255，售后看不出到底偶发了几回。
     */
    check(resp[7] == 1, "a continuous fault counts as a single occurrence");

    check(resp[8] == 0, "aging progress is 0 while still failing");
    /* 首次发生 1000ms = 0x000003E8，小端 */
    check(resp[9] == 0xE8 && resp[10] == 0x03 && resp[11] == 0x00 && resp[12] == 0x00,
          "first occurrence timestamp little endian");

    /* 中间通过一次、之后再失败 —— 这才是第二次发生 */
    dtc_report(&g_dtcs, DTC_BUS_OFF, 0, 3000);
    dtc_report(&g_dtcs, DTC_BUS_OFF, 1, 4000);

    n = uds_handle_request(&srv, req, 6, resp, sizeof(resp));
    check(n == 17 && resp[7] == 2, "re-failure after a pass counts as a second occurrence");
}

static void test_clear(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[4] = {UDS_SID_CLEAR_DTC, 0xFF, 0xFF, 0xFF};
    int n;

    printf("[0x14 clear diagnostic information]\n");
    with_dtcs(&srv);
    confirm_bus_off();

    /* 先在默认会话里试，必须被拒 */
    srv.st.session = UDS_SESSION_DEFAULT;
    n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
    check(n == 3 && resp[0] == 0x7F && resp[1] == UDS_SID_CLEAR_DTC &&
              resp[2] == UDS_NRC_CONDITIONS_NOT_CORRECT,
          "clear in default session -> NRC 0x22");
    check(dtc_count_by_mask(&g_dtcs, DTC_STATUS_CONFIRMED) == 1,
          "rejected clear leaves the DTC in place");

    enter_extended(&srv);
    n = uds_handle_request(&srv, req, 4, resp, sizeof(resp));
    check(n == 1 && resp[0] == 0x54, "positive response is a bare 0x54");
    check(dtc_count_by_mask(&g_dtcs, DTC_STATUS_CONFIRMED) == 0,
          "group 0xFFFFFF clears everything");

    /* 清完之后读列表应为空 */
    {
        uint8_t r2[3] = {UDS_SID_READ_DTC, UDS_DTC_REPORT_BY_MASK, 0x0F};
        n = uds_handle_request(&srv, r2, 3, resp, sizeof(resp));
        check(n == 3, "no DTC reported after clear");
    }

    /* 长度错 */
    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_INCORRECT_LENGTH, "clear rejects short request");
}

static void test_no_store(void)
{
    uds_server_t srv;
    uds_config_t cfg;
    uint8_t resp[64];
    uint8_t req19[3] = {UDS_SID_READ_DTC, UDS_DTC_REPORT_BY_MASK, 0xFF};
    uint8_t req14[4] = {UDS_SID_CLEAR_DTC, 0xFF, 0xFF, 0xFF};
    int n;

    printf("[no DTC store configured]\n");
    memset(&cfg, 0, sizeof(cfg));
    uds_init(&srv, &cfg);

    n = uds_handle_request(&srv, req19, 3, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SERVICE_NOT_SUPPORTED,
          "0x19 -> NRC 0x11 when no fault memory is wired");

    n = uds_handle_request(&srv, req14, 4, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SERVICE_NOT_SUPPORTED,
          "0x14 -> NRC 0x11 when no fault memory is wired");
}

static void test_unsupported_sub(void)
{
    uds_server_t srv;
    uint8_t resp[64];
    uint8_t req[3] = {UDS_SID_READ_DTC, 0x0B, 0xFF};
    int n;

    printf("[unsupported sub-function]\n");
    with_dtcs(&srv);

    n = uds_handle_request(&srv, req, 3, resp, sizeof(resp));
    check(n == 3 && resp[2] == UDS_NRC_SUB_FUNCTION_INVALID,
          "0x19 0B -> NRC 0x12 rather than an empty list");
}

int main(void)
{
    dtc_config_t cfg;

    printf("=== UDS DTC services test (0x19 / 0x14) ===\n\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.defs = g_defs;
    cfg.def_count = (uint16_t)(sizeof(g_defs) / sizeof(g_defs[0]));
    cfg.confirm_cycles = 2;
    cfg.aging_cycles = 40;
    dtc_init(&g_dtcs, &cfg);

    test_report_number();
    test_report_by_mask();
    test_report_supported();
    test_snapshot();
    test_ext_data();
    test_clear();
    test_no_store();
    test_unsupported_sub();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
