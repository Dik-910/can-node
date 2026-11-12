#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*
 * 总线时序分析：诊断多帧传输会挤压周期帧吗？
 *
 * 这个问题在真实项目里调不通时最常遇到，也是面试高频题。它本质是**可计算**的：
 * 知道波特率和每帧位数，就能算出最坏延迟，不需要示波器。
 *
 * 一个必须说清的前提：
 *   vcan 是内存回环，没有位时间、没有仲裁延迟、没有总线独占的概念，
 *   所以它**测不出**这里要分析的任何东西。这些数字是算出来的，不是测出来的。
 *   真实验证需要硬件 + CAN 分析仪（或至少 can-utils 的 canbusload）。
 *
 * 计算基于 CAN 2.0B 标准帧 11 位标识符。
 */

#define BITRATE_KBPS 500

/* 标准帧字段位数（不含位填充） */
#define BITS_SOF        1
#define BITS_ID         11
#define BITS_RTR        1
#define BITS_IDE        1
#define BITS_R0         1
#define BITS_DLC        4
#define BITS_CRC        15
#define BITS_CRC_DELIM  1
#define BITS_ACK        1
#define BITS_ACK_DELIM  1
#define BITS_EOF        7
#define BITS_IFS        3  /* 帧间隔，3 个隐性位 */

static void print_rule(void)
{
    printf("--------------------------------------------------------------\n");
}

/*
 * 一帧的总位数。
 *
 * 位填充只作用于 SOF 到 CRC 序列（共 98 位 / DLC=8），规则是连续 5 个同值位后插 1 个反向位。
 * 最坏情况下每 4 个数据位就要插 1 位，所以额外位数约为 该段位数 / 4。
 */
static int frame_bits(uint8_t dlc, int worst_stuffing)
{
    int base = BITS_SOF + BITS_ID + BITS_RTR + BITS_IDE + BITS_R0 + BITS_DLC +
               8 * dlc + BITS_CRC + BITS_CRC_DELIM + BITS_ACK + BITS_ACK_DELIM +
               BITS_EOF + BITS_IFS;

    if (!worst_stuffing)
        return base;

    /* 填充区：SOF 到 CRC，不含 CRC 界定符及之后 */
    {
        int stuffable = BITS_SOF + BITS_ID + BITS_RTR + BITS_IDE + BITS_R0 +
                        BITS_DLC + 8 * dlc + BITS_CRC;
        return base + stuffable / 4;
    }
}

static double frame_time_us(int bits)
{
    /* 1 位 = 1/500000 秒 = 2 us */
    return (double)bits * 1000.0 / (double)BITRATE_KBPS;
}

int main(void)
{
    const int bits_nominal = frame_bits(8, 0);
    const int bits_worst = frame_bits(8, 1);
    const double t_nominal = frame_time_us(bits_nominal);
    const double t_worst = frame_time_us(bits_worst);

    printf("=== Bus timing analysis @ %d kbps ===\n\n", BITRATE_KBPS);

    print_rule();
    printf("1. Single frame cost (DLC = 8)\n");
    print_rule();
    printf("   bits, no stuffing    : %d  -> %.0f us\n", bits_nominal, t_nominal);
    printf("   bits, worst stuffing : %d  -> %.0f us\n", bits_worst, t_worst);
    printf("   (worst case is what you size your deadline against)\n\n");

    /* ---------------- 周期报文负载 ---------------- */
    print_rule();
    printf("2. Periodic load\n");
    print_rule();

    struct
    {
        uint32_t id;
        int period_ms;
        const char *name;
    } periodic[] = {
        {0x180, 100, "NodeStatus"},
        {0x181, 20, "BusStatus"},
        {0x200, 50, "PackData"},
    };

    double total_load = 0.0;
    for (unsigned i = 0; i < sizeof(periodic) / sizeof(periodic[0]); i++)
    {
        /* 每周期占用 = 一帧最坏时间 / 周期 */
        double load = t_worst / (periodic[i].period_ms * 1000.0) * 100.0;
        total_load += load;
        printf("   ID 0x%03X %-11s  every %3d ms -> %5.2f%%\n",
               periodic[i].id, periodic[i].name, periodic[i].period_ms, load);
    }
    printf("   %-26s %s  %5.2f%%\n", "", "total:", total_load);
    printf("   (under 30%% is comfortable for classic CAN)\n\n");

    /* ---------------- 诊断传输占用 ---------------- */
    print_rule();
    printf("3. Diagnostic transfer of 200 bytes\n");
    print_rule();

    {
        int payload = 200;
        int ff = 1;                        /* 首帧带 6 字节 */
        int rest = payload - 6;
        int cf = (rest + 6) / 7;           /* 每个连续帧带 7 字节，向上取整 */
        int bs = 8;                        /* 每 8 个连续帧回一次流控 */
        int fc = 1 + cf / bs;              /* 首个 FC + 每满一块再来一个 */
        int total_frames = ff + cf + fc;
        double busy_ms = total_frames * t_worst / 1000.0;

        printf("   first frame      : %d\n", ff);
        printf("   consecutive frame: %d  (%d bytes after FF, 7 per frame)\n", cf, rest);
        printf("   flow control     : %d  (block size %d)\n", fc, bs);
        printf("   total frames     : %d\n", total_frames);
        printf("   bus busy time    : %.2f ms\n\n", busy_ms);

        /* ---------------- 对周期帧的影响 ---------------- */
        print_rule();
        printf("4. Impact on the 20 ms periodic frame (0x181)\n");
        print_rule();

        /*
         * CAN 是非破坏性仲裁：ID 小的赢，输的一方下一帧边界再争，报文不会丢。
         * 所以关键是「谁等谁」。
         */
        double delay_high_prio = busy_ms;        /* 诊断 ID < 0x181，周期帧一路让路 */
        double delay_low_prio = t_worst / 1000.0;/* 诊断 ID > 0x181，周期帧只需等当前一帧发完 */

        printf("   diag ID 0x100 (higher priority than 0x181)\n");
        printf("     periodic frame delayed up to : %6.2f ms  (waits for the whole burst)\n",
               delay_high_prio);
        printf("     as fraction of 20 ms period  : %6.1f%%\n",
               delay_high_prio / 20.0 * 100.0);
        printf("     jitter                       : %.2f ms\n", delay_high_prio);
        printf("     -> if the deadline is 20 ms +/- 5 ms, this MISSES it\n\n");

        printf("   diag ID 0x7E0 (lower priority than 0x181)\n");
        printf("     periodic frame delayed up to : %6.2f ms  (only waits for one frame)\n",
               delay_low_prio);
        printf("     as fraction of 20 ms period  : %6.1f%%\n",
               delay_low_prio / 20.0 * 100.0);
        printf("     jitter                       : %.2f ms\n", delay_low_prio);
        printf("     -> comfortably inside the deadline\n\n");

        printf("   ratio: %.0fx worse when diagnostics take a low ID\n",
               delay_high_prio / delay_low_prio);
        printf("\n");

        /* ---------------- 优化方案对比 ---------------- */
        print_rule();
        printf("5. What actually helps\n");
        print_rule();

        {
            int cf_no_fc = ff + cf;
            double ms_no_fc = cf_no_fc * t_worst / 1000.0;

            printf("   block size = 0 (no further flow control)\n");
            printf("     frames %d instead of %d, busy time %.2f ms  (saves %.0f%%)\n",
                   cf_no_fc, total_frames, ms_no_fc,
                   (busy_ms - ms_no_fc) / busy_ms * 100.0);

            printf("   smaller payload (chunked into 64-byte reads)\n");
            printf("     a 64-byte read costs roughly %.2f ms, well inside one 20 ms period\n",
                   (1 + (64 - 6 + 6) / 7) * t_worst / 1000.0);
        }
    }

    print_rule();
    printf("\nNOTE: numbers above are computed from bit timing, not measured.\n");
    printf("vcan has no bit timing and cannot reproduce any of this.\n");
    printf("To measure for real: use canbusload from can-utils on a physical bus,\n");
    printf("or a CAN analyzer with timestamped traces.\n");

    return 0;
}
