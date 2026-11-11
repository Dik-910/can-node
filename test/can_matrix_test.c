#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/can/generated/can_matrix.h"

/*
 * 报文编解码单元测试。
 *
 * 目的有两个：
 *   1. 证明 DBC 生成的代码能正常 pack / unpack
 *   2. 钉死字节序 —— 期望的十六进制串与 Python(cantools) 端编码结果一致，
 *      两端对不上就说明协议实现有分歧，必须在联调前解决
 *
 * 注意：生成的代码里结构体存的是**原始整数值**，不是物理值。
 * 缩放与偏移由调用方按头文件注释处理（如 pack_voltage 4875 表示 48.75 V）。
 *
 * 测试输出的字符串一律用英文：C 源码里的中文常量在不同终端 locale 和
 * 编辑器（尤其 Keil / IAR）下容易出现编码错乱，输出英文最稳。
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

static void hex_dump(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02X%s", buf[i], (i + 1 < len) ? " " : "");
    printf("\n");
}

/* 采样数据：与 Python 端期望字节串 0B 13 2E FB 57 4B 01 03 对齐 */
static void test_pack_data(void)
{
    printf("PackData 0x200 encode/decode\n");

    struct can_matrix_pack_data_t src;
    memset(&src, 0, sizeof(src));
    src.pack_voltage = 4875; /* 4875 x 0.01 = 48.75 V */
    src.pack_current = -1234; /* -1234 x 0.01 = -12.34 A */
    src.soc = 87;
    src.temp_max = 75; /* 75 + (-40) = 35 degC */
    src.prot_state = 1;
    src.fault_code = 3;

    uint8_t buf[8];
    int ret = can_matrix_pack_data_pack(buf, &src, sizeof(buf));
    printf("  encoded: ");
    hex_dump(buf, sizeof(buf));

    check(ret > 0, "pack returns byte count");
    const uint8_t expect[8] = {0x0B, 0x13, 0x2E, 0xFB, 0x57, 0x4B, 0x01, 0x03};
    check(memcmp(buf, expect, 8) == 0, "byte stream matches Python (little endian)");

    struct can_matrix_pack_data_t dst;
    memset(&dst, 0, sizeof(dst));
    ret = can_matrix_pack_data_unpack(&dst, buf, sizeof(buf));
    check(ret == 0, "unpack returns 0");
    check(dst.pack_voltage == 4875, "pack voltage restored");
    check(dst.pack_current == -1234, "pack current restored (signed)");
    check(dst.soc == 87, "soc restored");
    check(dst.temp_max == 75, "temp raw value restored");
    check(dst.prot_state == 1, "prot state restored");
    check(dst.fault_code == 3, "fault code restored");
}

/* 控制命令：4 字节，验证 DLC 与序列号 */
static void test_control_cmd(void)
{
    printf("ControlCmd 0x300 encode/decode\n");

    struct can_matrix_control_cmd_t src;
    memset(&src, 0, sizeof(src));
    src.cmd = 1; /* Start */
    src.seq = 42;
    src.param = 100;

    uint8_t buf[8];
    memset(buf, 0, sizeof(buf));
    int ret = can_matrix_control_cmd_pack(buf, &src, 4);
    printf("  encoded: ");
    hex_dump(buf, 4);

    check(ret > 0, "pack returns byte count");
    const uint8_t expect[4] = {0x01, 0x2A, 0x64, 0x00};
    check(memcmp(buf, expect, 4) == 0, "byte stream matches Python");

    struct can_matrix_control_cmd_t dst;
    memset(&dst, 0, sizeof(dst));
    can_matrix_control_cmd_unpack(&dst, buf, 4);
    check(dst.cmd == 1 && dst.seq == 42 && dst.param == 100, "all fields restored");
}

/* 节点状态：验证 32 位字段在 8 字节帧里的偏移 */
static void test_node_status(void)
{
    printf("NodeStatus 0x180 encode/decode\n");

    struct can_matrix_node_status_t src;
    memset(&src, 0, sizeof(src));
    src.node_state = 1;
    src.err_level = 0;
    src.bus_off_cnt = 3;
    src.uptime_s = 3600;

    uint8_t buf[8];
    int ret = can_matrix_node_status_pack(buf, &src, sizeof(buf));
    printf("  encoded: ");
    hex_dump(buf, sizeof(buf));

    check(ret > 0, "pack returns byte count");

    struct can_matrix_node_status_t dst;
    memset(&dst, 0, sizeof(dst));
    can_matrix_node_status_unpack(&dst, buf, sizeof(buf));
    check(dst.node_state == 1, "node state restored");
    check(dst.bus_off_cnt == 3, "bus-off count restored");
    check(dst.uptime_s == 3600, "32-bit uptime restored");
}

/* 错误参数量程：超出 DBC 定义范围应被拒绝 */
static void test_range(void)
{
    printf("Range check\n");

    struct can_matrix_pack_data_t src;
    memset(&src, 0, sizeof(src));
    src.soc = 200; /* DBC 定义 0..100 */

    uint8_t buf[8];
    int ret = can_matrix_pack_data_pack(buf, &src, sizeof(buf));
    check(ret != 0, "soc out of range rejected");
}

int main(void)
{
    printf("=== can_matrix generated code test ===\n\n");
    test_pack_data();
    printf("\n");
    test_control_cmd();
    printf("\n");
    test_node_status();
    printf("\n");
    test_range();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
