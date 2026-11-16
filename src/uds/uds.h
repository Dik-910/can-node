#ifndef __UDS_H
#define __UDS_H

#include <stdint.h>

/*
 * UDS 服务层（ISO 14229-1 子集）
 *
 * 本层只处理「报文内容」，不关心怎么收发——它把一帧完整的 UDS 请求
 * 转成一帧完整的 UDS 响应。收发由 ISO-TP 负责，所以这层可以直接用
 * 纯逻辑测试，不依赖 CAN 环境。
 *
 * 已实现的服务：
 *   0x10 会话控制     默认 / 编程 / 扩展
 *   0x11 ECU 复位     需要调用方提供复位动作
 *   0x22 读数据       按 DID 读，支持一次请求多个 DID
 *   0x27 安全访问     种子-密钥，带失败次数限制与锁定
 *   0x2E 写数据       按 DID 写，需扩展会话 + 已解锁
 *   0x3E 会话保持     重置 S3 定时器
 *
 * 两条贯穿全部服务的规则：
 *   1. 正响应 = 服务号 + 0x40，负响应固定 7F + 服务号 + NRC
 *   2. 会话决定权限，安全访问决定写权限
 */

/* 服务号 */
#define UDS_SID_SESSION_CONTROL 0x10
#define UDS_SID_ECU_RESET       0x11
#define UDS_SID_READ_DATA       0x22
#define UDS_SID_SECURITY_ACCESS 0x27
#define UDS_SID_WRITE_DATA      0x2E
#define UDS_SID_REQUEST_DOWNLOAD 0x34
#define UDS_SID_TRANSFER_DATA   0x36
#define UDS_SID_TRANSFER_EXIT   0x37
#define UDS_SID_TESTER_PRESENT  0x3E

/* 会话类型，取值与协议一致 */
#define UDS_SESSION_DEFAULT     0x01
#define UDS_SESSION_PROGRAMMING 0x02
#define UDS_SESSION_EXTENDED    0x03

/* 负响应码（NRC），只列本实现会用到的 */
#define UDS_NRC_SERVICE_NOT_SUPPORTED  0x11 /* 服务不支持 */
#define UDS_NRC_SUB_FUNCTION_INVALID   0x12 /* 子功能不支持 */
#define UDS_NRC_INCORRECT_LENGTH       0x13 /* 长度或格式错误 */
#define UDS_NRC_CONDITIONS_NOT_CORRECT 0x22 /* 条件不满足（多为会话不对） */
#define UDS_NRC_REQUEST_OUT_OF_RANGE   0x31 /* DID 不存在或参数越界 */
#define UDS_NRC_REQUEST_SEQUENCE_ERROR 0x24 /* 没有先 0x34 就发 0x36 */
#define UDS_NRC_TRANSFER_SUSPENDED     0x71 /* 传输被中止 */
#define UDS_NRC_GENERAL_PROGRAM_FAIL   0x72 /* 写存储器失败 */
#define UDS_NRC_WRONG_BLOCK_SEQ        0x73 /* 块序号不符合期望 */
#define UDS_NRC_SECURITY_DENIED        0x33 /* 未解锁 */
#define UDS_NRC_INVALID_KEY            0x35 /* 密钥错误 */
#define UDS_NRC_EXCEED_ATTEMPTS        0x36 /* 尝试次数超限 */
#define UDS_NRC_REQUIRED_TIME_DELAY    0x37 /* 还在锁定冷却期 */
#define UDS_NRC_RESPONSE_PENDING       0x78 /* 处理中，请稍等 */

/* 时间参数（毫秒） */
#define UDS_S3_TIMEOUT_MS 5000  /* 非默认会话的心跳超时 */
#define UDS_P2_MS         50    /* 收到请求到开始响应的最大时间 */
#define UDS_P2_STAR_MS    5000  /* 发 NRC 0x78 后允许的延长 */
#define UDS_MAX_ATTEMPTS  3     /* 安全访问允许的连续失败次数 */
#define UDS_LOCK_MS       10000 /* 超限后的冷却时间 */

#define UDS_MAX_DID_DATA  32    /* 单个 DID 的最大数据长度 */

/*
 * DID 表项
 *
 * 读取走两条路：
 *   data 非空 -> 直接拷贝静态数据（版本号、序列号这类不变的）
 *   read 非空 -> 调用回调现算（电压、SOC 这类会变的）
 * 两者都为空表示该 DID 只写不读。
 */
typedef struct
{
    uint16_t did;
    uint8_t len;              /* 静态数据的长度，回调方式可填 0 */
    const uint8_t *data;      /* 静态数据指针 */
    int (*read)(uint8_t *out, uint8_t *out_len);                  /* 读回调 */
    int (*write)(const uint8_t *in, uint8_t len);                 /* 写回调 */
    uint8_t min_session;      /* 最低会话要求：0x01 默认即可，0x03 需扩展 */
    uint8_t need_unlock;      /* 1 表示必须通过 0x27 解锁 */
} uds_did_entry_t;

/* 安全访问算法，可替换。默认实现见 uds.c */
typedef struct
{
    uint32_t (*seed_gen)(void *ctx);              /* 产生随机种子 */
    uint32_t (*key_calc)(uint32_t seed);          /* 由种子算出密钥 */
    void *ctx;
} uds_security_t;

typedef struct
{
    uint8_t session;
    uint8_t security_level; /* 0 未解锁，非 0 为已解锁的等级 */
    uint32_t s3_remain_ms;  /* S3 剩余时间 */
    uint8_t fail_count;     /* 连续失败次数 */
    uint32_t lock_remain_ms;/* 锁定剩余时间 */
} uds_state_t;

/*
 * 存储器接口，供 0x34/0x36/0x37 写入固件。
 *
 * 和 can_driver 一样用操作表注入：传输服务的逻辑（块序号、长度校验、
 * 越界检查）写成纯逻辑，PC 侧用内存模拟，硬件到了只实现这三个函数。
 *
 * 三条约定与真实 Flash 对齐：
 *   1. write 只能把 1 变 0，写之前必须已擦除，否则返回负值
 *   2. erase 以扇区为单位，内部自行向上取整，调用方只管给区间
 *   3. 地址是绝对地址（如 0x08008000），不是相对偏移
 */
typedef struct
{
    /* 成功返回 0，失败返回负值 */
    int (*write)(void *ctx, uint32_t addr, const uint8_t *data, uint16_t len);
    int (*read)(void *ctx, uint32_t addr, uint8_t *data, uint16_t len);

    /* 可空。为空时 0x34 不主动擦除，由写入方保证已擦除 */
    int (*erase)(void *ctx, uint32_t addr, uint32_t len);

    void *ctx;

    uint32_t base_addr; /* 可写区起始绝对地址 */
    uint32_t capacity;  /* 可写区长度，用来做越界检查 */
} uds_memory_t;

/* 一次传输的进度 */
typedef struct
{
    uint8_t active;        /* 已收到 0x34，正在等 0x36 */
    uint32_t addr;         /* 起始地址 */
    uint32_t total;        /* 总长度 */
    uint32_t received;     /* 已写入字节数 */
    uint8_t bsc_next;      /* 期望的下一个块序号 */
    uint8_t bsc_last;      /* 上一次成功写入的块序号，用来识别重传 */
    uint16_t blocks;       /* 已接受的块数 */
} uds_transfer_t;

typedef struct
{
    const uds_did_entry_t *dids;
    uint16_t did_count;
    uds_security_t sec;
    void (*on_reset)(void *ctx); /* 收到 0x11 时调用，可为空 */
    void *reset_ctx;

    /*
     * 存储器接口。为 NULL 时 0x34/0x36/0x37 返回 NRC 0x11（服务不支持）——
     * 不实现传输的节点不该假装支持，让 tester 明确知道走不通。
     */
    const uds_memory_t *mem;

    /*
     * 单块最大长度，含 SID(1) + BSC(1)，即实际数据 = max_block_len - 2。
     *
     * 这个值是 0x34 正响应里回给 tester 的，tester 据此决定每包发多少。
     * 0 表示用默认 256（实际数据 254 字节）。
     * 上限受接收缓冲约束：ISO-TP 的 ISOTP_RX_BUF_SIZE 是 512，
     * 所以不要超过 510 左右，否则 0x36 请求本身会被传输层拒掉。
     */
    uint16_t max_block_len;
} uds_config_t;

typedef struct
{
    uds_config_t cfg;
    uds_state_t st;
    uint32_t seed_state; /* 默认种子生成器的内部状态 */
    uint32_t seed;       /* 最近一次下发的种子，用来校验密钥 */
    uds_transfer_t xfer; /* 0x34/0x36/0x37 的进度 */
    int initialized;
} uds_server_t;

int uds_init(uds_server_t *srv, const uds_config_t *cfg);

/*
 * 处理一帧完整的 UDS 请求，成功返回响应长度（>0），失败返回负值。
 * resp 缓冲区由调用方提供；负响应也会写入 resp，长度通常为 3。
 *
 * 注意：0x11 ECU 复位会先返回正响应，再调用 on_reset —— 顺序不能反，
 * 否则复位动作可能导致响应还没发出去就掉电重启。
 */
int uds_handle_request(uds_server_t *srv, const uint8_t *req, uint16_t req_len,
                       uint8_t *resp, uint16_t resp_size);

/* 推进内部定时器，主循环里周期调用（建议每 10ms 一次） */
void uds_tick(uds_server_t *srv, uint32_t elapsed_ms);

/* 查询当前状态，主要用于测试和上层展示 */
const uds_state_t *uds_get_state(const uds_server_t *srv);

/* 查询传输进度。没有进行中的传输时 active 为 0 */
const uds_transfer_t *uds_get_transfer(const uds_server_t *srv);

/* 主动中止传输（例如上位机断开、或收到 0x11 复位前）。可重复调用 */
void uds_abort_transfer(uds_server_t *srv);

#endif
