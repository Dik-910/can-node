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

/* ---------------- 0x34 / 0x36 / 0x37 固件传输 ---------------- */

/*
 * 传输服务的共同前提。
 *
 * 为什么要求「扩展会话或编程会话」而不是只认编程会话：
 * ISO 14229 里 0x34 通常在编程会话下发，但很多 ECU 允许在扩展会话里
 * 刷写应用区（尤其是只更新标定数据时）。这里放宽到两者之一，
 * 但解锁是硬要求 —— 会话只表示「我要动配置了」，解锁才表示「我被授权了」。
 */
static int transfer_precheck(uds_server_t *srv, uint8_t sid,
                             uint8_t *resp, uint16_t size)
{
    if (srv->cfg.mem == 0)
        return send_negative(resp, size, sid, UDS_NRC_SERVICE_NOT_SUPPORTED);

    if (srv->st.session != UDS_SESSION_EXTENDED &&
        srv->st.session != UDS_SESSION_PROGRAMMING)
        return send_negative(resp, size, sid, UDS_NRC_CONDITIONS_NOT_CORRECT);

    if (srv->st.security_level == 0)
        return send_negative(resp, size, sid, UDS_NRC_SECURITY_DENIED);

    return 0;
}

/* 0x34 RequestDownload：告诉 ECU 要写哪、写多长，ECU 回「每包最多多少字节」 */
static int handle_request_download(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                   uint8_t *resp, uint16_t size)
{
    const uds_memory_t *mem = srv->cfg.mem;
    uint8_t dfi, alfid, addr_len, size_len;
    uint32_t addr = 0, total = 0;
    uint32_t end;
    uint16_t max_block;
    int rc;

    if ((rc = transfer_precheck(srv, UDS_SID_REQUEST_DOWNLOAD, resp, size)) != 0)
        return rc;

    if (len < 3)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INCORRECT_LENGTH);

    dfi = req[1];
    alfid = req[2];
    addr_len = (uint8_t)(alfid & 0x0F);
    size_len = (uint8_t)((alfid >> 4) & 0x0F);

    /*
     * 长度字节数为 0 或超过 4 都不合法。
     * ISO 允许 1..4 字节的地址/长度，实际常见 2 或 4。
     */
    if (addr_len == 0 || addr_len > 4 || size_len == 0 || size_len > 4)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INCORRECT_LENGTH);

    if (len != (uint16_t)(3 + addr_len + size_len))
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INCORRECT_LENGTH);

    /*
     * dataFormatIdentifier 高 4 位是压缩方法，低 4 位是加密方法。
     * 只支持 0x00（都不做）—— 声称支持却实际没解压，会导致写进去的是
     * 压缩后的垃圾，而且 CRC 还是对的，故障极难定位。
     */
    if (dfi != 0x00)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);

    /* 大端解析 */
    for (uint8_t i = 0; i < addr_len; i++)
        addr = (addr << 8) | req[3 + i];
    for (uint8_t i = 0; i < size_len; i++)
        total = (total << 8) | req[3 + addr_len + i];

    if (total == 0)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);

    /*
     * 越界检查先减后比，避免 addr + total 在 uint32 上溢出。
     * 溢出后是个很小的数，反而能通过检查 —— 这种写法比加法安全。
     */
    if (addr < mem->base_addr)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);

    if (addr - mem->base_addr > mem->capacity ||
        total > mem->capacity - (addr - mem->base_addr))
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);

    end = addr + total; /* 到这里已经确认不溢出 */

    /*
     * 先擦后写。擦除失败就直接拒绝 —— 继续下去会在未擦除区域写，
     * 得到一堆「本该是 1 却是 0」的位，体现为固件校验不过。
     */
    if (mem->erase != 0 && mem->erase(mem->ctx, addr, total) != 0)
        return send_negative(resp, size, UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_GENERAL_PROGRAM_FAIL);

    srv->xfer.active = 1;
    srv->xfer.addr = addr;
    srv->xfer.total = total;
    srv->xfer.received = 0;
    srv->xfer.bsc_next = 1; /* 第一块固定是 1 */
    srv->xfer.bsc_last = 0;
    srv->xfer.blocks = 0;

    max_block = (srv->cfg.max_block_len != 0) ? srv->cfg.max_block_len : 256;

    /*
     * 响应：0x74 + lengthFormatIdentifier + maxNumberOfBlockLength
     *
     * 关键：这个长度**包含 SID 和 BSC 两个字节**，不是纯数据长度。
     * 报文里的值写成纯数据长度，tester 会多发 2 字节，每块都超界。
     */
    if (size < 4)
        return -1;

    resp[0] = (uint8_t)(UDS_SID_REQUEST_DOWNLOAD + POS_OFFSET);
    resp[1] = 0x20; /* 低 4 位 = 2，表示后面 2 字节 */
    resp[2] = (uint8_t)(max_block >> 8);
    resp[3] = (uint8_t)(max_block & 0xFF);

    (void)end;
    return 4;
}

/* 0x36 TransferData：收一块数据并写入存储器 */
static int handle_transfer_data(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                uint8_t *resp, uint16_t size)
{
    const uds_memory_t *mem = srv->cfg.mem;
    uint8_t bsc;
    uint16_t data_len;
    uint32_t remain;
    int rc;

    if ((rc = transfer_precheck(srv, UDS_SID_TRANSFER_DATA, resp, size)) != 0)
        return rc;

    if (len < 2)
        return send_negative(resp, size, UDS_SID_TRANSFER_DATA, UDS_NRC_INCORRECT_LENGTH);

    if (!srv->xfer.active)
        return send_negative(resp, size, UDS_SID_TRANSFER_DATA, UDS_NRC_REQUEST_SEQUENCE_ERROR);

    bsc = req[1];
    data_len = (uint16_t)(len - 2);

    if (size < 2)
        return -1;

    /*
     * 重传：同一个块序号再次到达，说明 tester 没收到我们上次的响应。
     *
     * 必须识别出来而不是当成序号错误：真实链路上丢一个响应很常见，
     * 直接报 0x73 会让 tester 中止整个刷写。这里不重复写入，
     * 只重发正响应。
     */
    if (srv->xfer.blocks > 0 && bsc == srv->xfer.bsc_last)
    {
        resp[0] = (uint8_t)(UDS_SID_TRANSFER_DATA + POS_OFFSET);
        resp[1] = bsc;
        return 2;
    }

    if (bsc != srv->xfer.bsc_next)
        return send_negative(resp, size, UDS_SID_TRANSFER_DATA, UDS_NRC_WRONG_BLOCK_SEQ);

    remain = srv->xfer.total - srv->xfer.received;

    if (data_len > remain)
        return send_negative(resp, size, UDS_SID_TRANSFER_DATA, UDS_NRC_TRANSFER_SUSPENDED);

    if (mem->write(mem->ctx, srv->xfer.addr + srv->xfer.received, &req[2], data_len) != 0)
        return send_negative(resp, size, UDS_SID_TRANSFER_DATA, UDS_NRC_GENERAL_PROGRAM_FAIL);

    srv->xfer.received += data_len;
    srv->xfer.blocks++;
    srv->xfer.bsc_last = bsc;

    /*
     * 序号推进：0xFF 之后是 0x00，不是 0x01。
     * uint8 的自然回绕正好是这个语义（0 表示第 256 块），
     * 所以不要写 if (n > 0xFF) n = 1 —— 那会让第 257 块被判成序号错误。
     */
    srv->xfer.bsc_next = (uint8_t)(bsc + 1);

    resp[0] = (uint8_t)(UDS_SID_TRANSFER_DATA + POS_OFFSET);
    resp[1] = bsc;
    return 2;
}

/* 0x37 RequestTransferExit：结束传输 */
static int handle_transfer_exit(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                uint8_t *resp, uint16_t size)
{
    int rc;

    (void)req;

    if ((rc = transfer_precheck(srv, UDS_SID_TRANSFER_EXIT, resp, size)) != 0)
        return rc;

    if (len < 1)
        return send_negative(resp, size, UDS_SID_TRANSFER_EXIT, UDS_NRC_INCORRECT_LENGTH);

    if (!srv->xfer.active)
        return send_negative(resp, size, UDS_SID_TRANSFER_EXIT, UDS_NRC_REQUEST_SEQUENCE_ERROR);

    /*
     * 允许提前退出：tester 可能在收完之前就因为别的原因放弃。
     * 这里不报错，但把 received/total 留在日志里由上层判断 ——
     * 「传输提前结束」和「传输完成」是两件事，不能悄悄混为一谈。
     */
    uds_abort_transfer(srv);

    if (size < 1)
        return -1;

    resp[0] = (uint8_t)(UDS_SID_TRANSFER_EXIT + POS_OFFSET);
    return 1;
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
    case UDS_SID_REQUEST_DOWNLOAD:
        return handle_request_download(srv, req, req_len, resp, resp_size);
    case UDS_SID_TRANSFER_DATA:
        return handle_transfer_data(srv, req, req_len, resp, resp_size);
    case UDS_SID_TRANSFER_EXIT:
        return handle_transfer_exit(srv, req, req_len, resp, resp_size);
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

const uds_transfer_t *uds_get_transfer(const uds_server_t *srv)
{
    static const uds_transfer_t empty;

    if (srv == 0)
        return &empty;

    return &srv->xfer;
}

void uds_abort_transfer(uds_server_t *srv)
{
    if (srv == 0)
        return;

    memset(&srv->xfer, 0, sizeof(srv->xfer));
}
