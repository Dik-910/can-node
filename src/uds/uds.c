#include "uds.h"

#include <string.h>

/*
 * UDS 服务层实现。
 *
 * 分派方式用 switch 而不是函数指针表：服务只有六个，加一层间接反而更难读。
 * 参考实现（driftregion/iso14229）走的是「单入口事件回调」，适合库作者；
 * 我们是自己写业务，switch 更直接。
 *
 * 每个 handler 的检查顺序固定为：
 *   长度 -> 子功能/DID 合法性 -> 会话 -> 安全权限 -> 业务动作
 * 顺序不能乱：先查权限再查长度，会让攻击者靠 NRC 差异探测内部结构。
 */

#define POS_OFFSET 0x40 /* 正响应 = 服务号 + 0x40 */

static int send_negative(uint8_t *resp, uint16_t size, uint8_t sid, uint8_t nrc)
{
    if (size < 3)
        return -1;

    resp[0] = 0x7F;
    resp[1] = sid;
    resp[2] = nrc;
    return 3;
}

/* 当前会话是否满足该服务的最低要求 */
static int session_allows(const uds_server_t *srv, uint8_t min_session)
{
    if (min_session <= UDS_SESSION_DEFAULT)
        return 1;

    if (min_session == UDS_SESSION_EXTENDED)
        return srv->st.session == UDS_SESSION_EXTENDED ||
               srv->st.session == UDS_SESSION_PROGRAMMING;

    if (min_session == UDS_SESSION_PROGRAMMING)
        return srv->st.session == UDS_SESSION_PROGRAMMING;

    return 0;
}

static const uds_did_entry_t *find_did(const uds_server_t *srv, uint16_t did)
{
    for (uint16_t i = 0; i < srv->cfg.did_count; i++)
    {
        if (srv->cfg.dids[i].did == did)
            return &srv->cfg.dids[i];
    }
    return 0;
}

/* 默认种子生成器：线性同余，够演示用 */
static uint32_t default_seed(uds_server_t *srv)
{
    srv->seed_state = srv->seed_state * 1103515245u + 12345u;
    return srv->seed_state;
}

/*
 * 默认密钥算法。这只是让流程能闭环，不是安全实现——
 * 生产环境要换成真实 ECU 的挑战响应算法（通常由主机厂定义）。
 */
static uint32_t default_key(uint32_t seed)
{
    return (uint32_t)(((seed ^ 0x5A5A5A5Au) + 0x12345678u) & 0xFFFFFFFFu);
}

/* ---------------- 0x10 会话控制 ---------------- */

static int handle_session_control(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                  uint8_t *resp, uint16_t size)
{
    uint8_t type;

    if (len != 2)
        return send_negative(resp, size, UDS_SID_SESSION_CONTROL, UDS_NRC_INCORRECT_LENGTH);

    /* 最高位是 suppressPositiveResponse，这里不实现抑制，直接剥掉 */
    type = (uint8_t)(req[1] & 0x7F);

    if (type != UDS_SESSION_DEFAULT && type != UDS_SESSION_PROGRAMMING &&
        type != UDS_SESSION_EXTENDED)
        return send_negative(resp, size, UDS_SID_SESSION_CONTROL, UDS_NRC_SUB_FUNCTION_INVALID);

    /* 进编程会话前必须先解锁，否则任何人都能进刷写模式 */
    if (type == UDS_SESSION_PROGRAMMING && srv->st.security_level == 0)
        return send_negative(resp, size, UDS_SID_SESSION_CONTROL, UDS_NRC_SECURITY_DENIED);

    srv->st.session = type;
    srv->st.s3_remain_ms = UDS_S3_TIMEOUT_MS;

    /* 离开扩展/编程会话时清掉安全等级，防止权限泄漏到默认会话 */
    if (type == UDS_SESSION_DEFAULT)
        srv->st.security_level = 0;

    if (size < 6)
        return -1;

    resp[0] = (uint8_t)(UDS_SID_SESSION_CONTROL + POS_OFFSET);
    resp[1] = type;
    resp[2] = (uint8_t)(UDS_P2_MS >> 8);
    resp[3] = (uint8_t)(UDS_P2_MS & 0xFF);
    resp[4] = (uint8_t)(UDS_P2_STAR_MS >> 8);
    resp[5] = (uint8_t)(UDS_P2_STAR_MS & 0xFF);
    return 6;
}

/* ---------------- 0x11 ECU 复位 ---------------- */

static int handle_ecu_reset(uds_server_t *srv, const uint8_t *req, uint16_t len,
                            uint8_t *resp, uint16_t size)
{
    uint8_t type;

    if (len != 2)
        return send_negative(resp, size, UDS_SID_ECU_RESET, UDS_NRC_INCORRECT_LENGTH);

    type = (uint8_t)(req[1] & 0x7F);
    if (type < 1 || type > 3)
        return send_negative(resp, size, UDS_SID_ECU_RESET, UDS_NRC_SUB_FUNCTION_INVALID);

    if (size < 2)
        return -1;

    resp[0] = (uint8_t)(UDS_SID_ECU_RESET + POS_OFFSET);
    resp[1] = type;

    /*
     * 复位动作放在响应写好之后调用。调用方拿到的 resp 是完整响应，
     * 由它负责发出去；如果先复位，芯片可能在响应发出前就重启了。
     */
    if (srv->cfg.on_reset != 0)
        srv->cfg.on_reset(srv->cfg.reset_ctx);

    return 2;
}

/* ---------------- 0x3E 会话保持 ---------------- */

static int handle_tester_present(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                 uint8_t *resp, uint16_t size)
{
    (void)srv;

    if (len != 2)
        return send_negative(resp, size, UDS_SID_TESTER_PRESENT, UDS_NRC_INCORRECT_LENGTH);

    if (size < 2)
        return -1;

    /* 心跳本身不改状态，S3 定时器在调用 uds_handle_request 前已重置 */
    resp[0] = (uint8_t)(UDS_SID_TESTER_PRESENT + POS_OFFSET);
    resp[1] = (uint8_t)(req[1] & 0x7F);
    return 2;
}

/* ---------------- 0x27 安全访问 ---------------- */

static int handle_security_access(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                  uint8_t *resp, uint16_t size)
{
    uint8_t sub;
    uint32_t key;
    uint32_t expect;

    if (len < 2)
        return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_LENGTH);

    sub = req[1];

    /* 冷却期内连种子都不给，否则攻击者能持续试探 */
    if (srv->st.lock_remain_ms > 0)
        return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUIRED_TIME_DELAY);

    if (sub & 0x01)
    {
        /* 奇数为请求种子 */
        if (len != 2)
            return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_LENGTH);
        if (size < 6)
            return -1;

        srv->seed = (srv->cfg.sec.seed_gen != 0) ? srv->cfg.sec.seed_gen(srv->cfg.sec.ctx)
                                                 : default_seed(srv);

        resp[0] = (uint8_t)(UDS_SID_SECURITY_ACCESS + POS_OFFSET);
        resp[1] = sub;
        resp[2] = (uint8_t)(srv->seed >> 24);
        resp[3] = (uint8_t)(srv->seed >> 16);
        resp[4] = (uint8_t)(srv->seed >> 8);
        resp[5] = (uint8_t)(srv->seed);
        return 6;
    }

    /* 偶数为发送密钥 */
    if (len != 6)
        return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_LENGTH);

    key = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
          ((uint32_t)req[4] << 8) | (uint32_t)req[5];
    expect = (srv->cfg.sec.key_calc != 0) ? srv->cfg.sec.key_calc(srv->seed)
                                          : default_key(srv->seed);

    if (key != expect)
    {
        srv->st.fail_count++;

        /* 达到上限就锁定，防止离线穷举种子密钥 */
        if (srv->st.fail_count >= UDS_MAX_ATTEMPTS)
        {
            srv->st.fail_count = 0;
            srv->st.lock_remain_ms = UDS_LOCK_MS;
            return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEED_ATTEMPTS);
        }

        return send_negative(resp, size, UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY);
    }

    if (size < 2)
        return -1;

    srv->st.fail_count = 0;
    srv->st.security_level = (uint8_t)(sub >> 1); /* 02 -> 等级 1，04 -> 等级 2 */

    resp[0] = (uint8_t)(UDS_SID_SECURITY_ACCESS + POS_OFFSET);
    resp[1] = sub;
    return 2;
}

/* ---------------- 0x22 读数据 ---------------- */

static int handle_read_data(uds_server_t *srv, const uint8_t *req, uint16_t len,
                            uint8_t *resp, uint16_t size)
{
    uint16_t out = 0;
    uint16_t i;

    if (len < 3 || ((len - 1) % 2) != 0)
        return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_INCORRECT_LENGTH);

    if (size < 1)
        return -1;

    out = 0;
    resp[out++] = (uint8_t)(UDS_SID_READ_DATA + POS_OFFSET);

    for (i = 1; i + 1 < len; i = (uint16_t)(i + 2))
    {
        uint16_t did = (uint16_t)(((uint16_t)req[i] << 8) | req[i + 1]);
        const uds_did_entry_t *e = find_did(srv, did);
        uint8_t dlen = 0;
        uint8_t tmp[UDS_MAX_DID_DATA];

        if (e == 0)
            return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);

        if (!session_allows(srv, e->min_session))
            return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_CONDITIONS_NOT_CORRECT);

        if (e->need_unlock && srv->st.security_level == 0)
            return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_SECURITY_DENIED);

        if (e->data != 0)
        {
            dlen = e->len;
            memcpy(tmp, e->data, dlen);
        }
        else if (e->read != 0)
        {
            if (e->read(tmp, &dlen) != 0)
                return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);
        }
        else
        {
            /* 只写不读的 DID，读请求应被拒绝 */
            return send_negative(resp, size, UDS_SID_READ_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);
        }

        if (dlen > UDS_MAX_DID_DATA || (uint16_t)(out + 2 + dlen) > size)
            return -1;

        resp[out++] = req[i];
        resp[out++] = req[i + 1];
        memcpy(&resp[out], tmp, dlen);
        out = (uint16_t)(out + dlen);
    }

    return out;
}

/* ---------------- 0x2E 写数据 ---------------- */

static int handle_write_data(uds_server_t *srv, const uint8_t *req, uint16_t len,
                             uint8_t *resp, uint16_t size)
{
    uint16_t did;
    const uds_did_entry_t *e;

    if (len < 4)
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_INCORRECT_LENGTH);

    did = (uint16_t)(((uint16_t)req[1] << 8) | req[2]);
    e = find_did(srv, did);

    if (e == 0)
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);

    if (!session_allows(srv, e->min_session))
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_CONDITIONS_NOT_CORRECT);

    if (e->need_unlock && srv->st.security_level == 0)
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_SECURITY_DENIED);

    if (e->write == 0)
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);

    if (e->write(&req[3], (uint8_t)(len - 3)) != 0)
        return send_negative(resp, size, UDS_SID_WRITE_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);

    if (size < 3)
        return -1;

    resp[0] = (uint8_t)(UDS_SID_WRITE_DATA + POS_OFFSET);
    resp[1] = req[1];
    resp[2] = req[2];
    return 3;
}

/* ---------------- 对外接口 ---------------- */

int uds_init(uds_server_t *srv, const uds_config_t *cfg)
{
    if (srv == 0 || cfg == 0)
        return -1;

    memset(srv, 0, sizeof(*srv));
    srv->cfg = *cfg;
    srv->st.session = UDS_SESSION_DEFAULT;
    srv->seed_state = 0x01ABCDEFu;
    srv->initialized = 1;
    return 0;
}

int uds_handle_request(uds_server_t *srv, const uint8_t *req, uint16_t req_len,
                       uint8_t *resp, uint16_t resp_size)
{
    uint8_t sid;

    if (srv == 0 || req == 0 || resp == 0 || srv->initialized == 0)
        return -1;
    if (req_len < 1)
        return send_negative(resp, resp_size, 0x00, UDS_NRC_INCORRECT_LENGTH);

    /*
     * 任何请求都算一次心跳：只要在扩展会话里收到报文，S3 就应重置。
     * 这条容易漏——只认 0x3E 会导致「发了别的请求反而掉线」。
     */
    if (srv->st.session != UDS_SESSION_DEFAULT)
        srv->st.s3_remain_ms = UDS_S3_TIMEOUT_MS;

    sid = req[0];

    switch (sid)
    {
    case UDS_SID_SESSION_CONTROL:
        return handle_session_control(srv, req, req_len, resp, resp_size);
    case UDS_SID_ECU_RESET:
        return handle_ecu_reset(srv, req, req_len, resp, resp_size);
    case UDS_SID_READ_DATA:
        return handle_read_data(srv, req, req_len, resp, resp_size);
    case UDS_SID_SECURITY_ACCESS:
        return handle_security_access(srv, req, req_len, resp, resp_size);
    case UDS_SID_WRITE_DATA:
        return handle_write_data(srv, req, req_len, resp, resp_size);
    case UDS_SID_TESTER_PRESENT:
        return handle_tester_present(srv, req, req_len, resp, resp_size);
    default:
        return send_negative(resp, resp_size, sid, UDS_NRC_SERVICE_NOT_SUPPORTED);
    }
}

void uds_tick(uds_server_t *srv, uint32_t elapsed_ms)
{
    if (srv == 0 || srv->initialized == 0)
        return;

    if (srv->st.s3_remain_ms > 0)
    {
        if (srv->st.s3_remain_ms > elapsed_ms)
        {
            srv->st.s3_remain_ms -= elapsed_ms;
        }
        else
        {
            /* S3 超时：掉回默认会话，同时收回安全权限 */
            srv->st.s3_remain_ms = 0;
            srv->st.session = UDS_SESSION_DEFAULT;
            srv->st.security_level = 0;
        }
    }

    if (srv->st.lock_remain_ms > 0)
    {
        if (srv->st.lock_remain_ms > elapsed_ms)
            srv->st.lock_remain_ms -= elapsed_ms;
        else
            srv->st.lock_remain_ms = 0;
    }
}

const uds_state_t *uds_get_state(const uds_server_t *srv)
{
    return (srv != 0) ? &srv->st : 0;
}
