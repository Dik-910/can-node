/*
 * nvm_file_test.c —— 参数区的文件实现
 *
 * nvm_store 的逻辑在 nvm_store_test 里用内存设备测过了，
 * 这里只验证文件这一层：新建时是否为擦除态、写入是否落盘、
 * 关掉重开能不能读回来。
 *
 * 单独测它的理由：ecu-server 用的是文件实现，而它不在单元测试的覆盖里。
 * 文件层的 off_t / pread / 长度补齐一旦写错，单元测试全绿也说明不了问题。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nvm_file.h"
#include "nvm_store.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what, long got, long want)
{
    g_total++;
    if (cond)
    {
        printf("  [PASS] %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  [FAIL] %s (got %ld, want %ld)\n", what, got, want);
    }
}

#define TEST_FILE ".nvm_file_test.bin"
#define AREA_SIZE 2048u

static void test_create_and_restore(void)
{
    nvm_file_ctx_t f;
    nvm_store_t st;
    uint8_t data[48], out[48];
    uint16_t len = 0;

    printf("文件参数区的创建与重开\n");

    unlink(TEST_FILE);

    check(nvm_file_open(&f, TEST_FILE, AREA_SIZE) == 0, "创建文件", 0, 0);
    check(nvm_store_mount(&st, nvm_file_dev(&f)) == 0, "挂载", 0, 0);
    check(st.valid == 0, "新文件应是擦除态", st.valid, 0);

    for (int i = 0; i < (int)sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + 1);

    check(nvm_store_save(&st, data, sizeof(data)) == 0, "写入", 0, 0);
    nvm_file_close(&f);

    /* 重新打开，等价于断电重启 */
    check(nvm_file_open(&f, TEST_FILE, AREA_SIZE) == 0, "重新打开", 0, 0);
    check(nvm_store_mount(&st, nvm_file_dev(&f)) == 0, "重新挂载", 0, 0);

    check(nvm_store_data(&st, &len) != 0, "重开后能读到", 1, 1);
    check(len == sizeof(data), "长度一致", len, sizeof(data));
    check(len == sizeof(data) && memcmp(st.buf, data, len) == 0, "内容一致", 0, 0);

    nvm_file_close(&f);
    unlink(TEST_FILE);
    (void)out;
}

static void test_short_file_grows(void)
{
    nvm_file_ctx_t f;
    nvm_store_t st;
    FILE *fp;

    printf("长度不足的文件补齐到擦除态\n");

    unlink(TEST_FILE);

    /* 先造一个只有 100 字节的文件 */
    fp = fopen(TEST_FILE, "wb");
    check(fp != 0, "创建短文件", 1, 1);
    if (fp != 0)
    {
        uint8_t junk[100];

        memset(junk, 0x00, sizeof(junk));
        fwrite(junk, 1, sizeof(junk), fp);
        fclose(fp);
    }

    check(nvm_file_open(&f, TEST_FILE, AREA_SIZE) == 0, "打开并补齐", 0, 0);
    check(nvm_store_mount(&st, nvm_file_dev(&f)) == 0, "挂载", 0, 0);

    /*
     * 前 100 字节是零，不是擦除态，所以页头 magic 对不上 ——
     * 结果是「没有有效数据」，而不是「读到一堆垃圾」。
     */
    check(st.valid == 0, "残缺内容不应被当作有效数据", st.valid, 0);

    nvm_file_close(&f);
    unlink(TEST_FILE);
}

static void test_write_persists_after_second_save(void)
{
    nvm_file_ctx_t f;
    nvm_store_t st;
    uint8_t a[16], b[16];
    uint16_t len = 0;

    printf("两次写入后重开取最后一份\n");

    unlink(TEST_FILE);

    nvm_file_open(&f, TEST_FILE, AREA_SIZE);
    nvm_store_mount(&st, nvm_file_dev(&f));

    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));

    nvm_store_save(&st, a, sizeof(a));
    nvm_store_save(&st, b, sizeof(b));
    nvm_file_close(&f);

    nvm_file_open(&f, TEST_FILE, AREA_SIZE);
    nvm_store_mount(&st, nvm_file_dev(&f));

    check(nvm_store_data(&st, &len) != 0 && len == sizeof(b), "长度是最后一份", len, sizeof(b));
    check(len == sizeof(b) && st.buf[0] == 0x22, "内容是最后一份", st.buf[0], 0x22);

    nvm_file_close(&f);
    unlink(TEST_FILE);
}

int main(void)
{
    printf("=== nvm_file_test ===\n");

    test_create_and_restore();
    test_short_file_grows();
    test_write_persists_after_second_save();

    printf("\nResult: %d passed, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
