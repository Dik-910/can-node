#include "uds.h"

#include <string.h>

/*
 * UDS 服务层实现。
 *
 * 分派方式用 switch 而不是函数指针表：服务只有十来个，加一层间接反而更难读。
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


/* ---------------- 0x31 例程控制 ---------------- */

/*
 * CRC32，反射算法（多项式 0xEDB88320），与 src/boot/boot.c 一致。
 * 校验固件完整性和 Bootloader 跳转前算的是同一个值，不能各写一套。
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }

    return ~crc;
}

/*
 * 内置例程的参数格式：addr(4 字节大端) + size(4 字节大端)。
 *
 * 为什么要求参数里带地址，而不是复用上一次 0x34 的范围：
 * 校验和擦除可能作用于不同的区（比如只校验应用区、擦除整个备份区），
 * 复用 0x34 的范围会把两件事绑死。显式传更好排障。
 */
static int parse_addr_size(const uds_memory_t *mem, const uint8_t *opt, uint16_t opt_len,
                           uint32_t *addr, uint32_t *size)
{
    uint32_t a = 0, n = 0;

    if (opt_len != 8)
        return -1;

    for (int i = 0; i < 4; i++)
        a = (a << 8) | opt[i];
    for (int i = 0; i < 4; i++)
        n = (n << 8) | opt[4 + i];

    if (n == 0)
        return -1;

    /* 与 0x34 相同的越界判断：先确认起点，再用减法算剩余空间，避免相加溢出 */
    if (a < mem->base_addr)
        return -1;

    if (a - mem->base_addr > mem->capacity ||
        n > mem->capacity - (a - mem->base_addr))
        return -1;

    *addr = a;
    *size = n;
    return 0;
}

/* 算一段存储区的 CRC32。分块读，避免在栈上开一个大缓冲 */
static int compute_crc(const uds_memory_t *mem, uint32_t addr, uint32_t size,
                       uint32_t *out_crc)
{
    uint8_t buf[64];
    uint32_t crc = 0;
    uint32_t off = 0;

    while (off < size)
    {
        uint32_t n = size - off;

        if (n > sizeof(buf))
            n = sizeof(buf);

        if (mem->read(mem->ctx, addr + off, buf, (uint16_t)n) != 0)
            return -1;

        /*
         * crc32_update 内部做反码进出，所以第一次必须从 0 开始传入，
         * 后续把上一次的结果接上即可 —— 不能自己再补 0xFFFFFFFF，
         * 那会把中间结果算坏（表现为 CRC 只在单块数据时正确）。
         */
        crc = crc32_update(crc, buf, n);
        off += n;
    }

    *out_crc = crc;
    return 0;
}

/* 在自定义表里找，内置的不在这张表里 */
static const uds_routine_entry_t *find_routine(const uds_server_t *srv, uint16_t rid)
{
    for (uint16_t i = 0; i < srv->cfg.routine_count; i++)
    {
        if (srv->cfg.routines[i].rid == rid)
            return &srv->cfg.routines[i];
    }

    return 0;
}

/* 0x31 RoutineControl */
static int handle_routine_control(uds_server_t *srv, const uint8_t *req, uint16_t len,
                                  uint8_t *resp, uint16_t size)
{
    const uds_memory_t *mem = srv->cfg.mem;
    const uds_routine_entry_t *re;
    uint8_t sub;
    uint16_t rid;
    uint8_t builtin;

    if (len < 4)
        return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL, UDS_NRC_INCORRECT_LENGTH);

    sub = req[1];
    rid = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);

    if (sub != UDS_ROUTINE_START && sub != UDS_ROUTINE_STOP && sub != UDS_ROUTINE_RESULT)
        return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL, UDS_NRC_SUB_FUNCTION_INVALID);

    /*
     * 内置例程优先。放在自定义表之前查，是为了让「配了 mem 就有擦除和校验」
     * 这件事无条件成立 —— 用户表里误写了同一个 rid 也不该把内置的顶掉。
     */
    builtin = (mem != 0 && (rid == UDS_RID_ERASE_MEMORY || rid == UDS_RID_CHECK_MEMORY)) ? 1 : 0;
    re = find_routine(srv, rid);

    if (!builtin && re == 0)
        return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL, UDS_NRC_REQUEST_OUT_OF_RANGE);

    /* 权限：内置例程一律要求扩展或编程会话 + 解锁 */
    if (builtin)
    {
        if (srv->st.session != UDS_SESSION_EXTENDED &&
            srv->st.session != UDS_SESSION_PROGRAMMING)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_CONDITIONS_NOT_CORRECT);

        if (srv->st.security_level == 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL, UDS_NRC_SECURITY_DENIED);
    }
    else
    {
        if (!session_allows(srv, re->min_session))
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_CONDITIONS_NOT_CORRECT);

        if (re->need_unlock && srv->st.security_level == 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL, UDS_NRC_SECURITY_DENIED);
    }

    if (size < 4)
        return -1;

    /* 正响应先填好头，具体内容由各分支补 */
    resp[0] = (uint8_t)(UDS_SID_ROUTINE_CONTROL + POS_OFFSET);
    resp[1] = sub;
    resp[2] = (uint8_t)(rid >> 8);
    resp[3] = (uint8_t)rid;

    if (builtin && rid == UDS_RID_ERASE_MEMORY)
    {
        uint32_t addr, n;

        if (sub != UDS_ROUTINE_START)
            return 4; /* 擦除没有中间状态，stop/result 回空状态即可 */

        if (parse_addr_size(mem, &req[4], (uint16_t)(len - 4), &addr, &n) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_REQUEST_OUT_OF_RANGE);

        if (mem->erase == 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);

        if (mem->erase(mem->ctx, addr, n) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);

        return 4;
    }

    if (builtin && rid == UDS_RID_CHECK_MEMORY)
    {
        uint32_t addr, n;

        if (sub == UDS_ROUTINE_RESULT)
        {
            /*
             * 结果在 start 时就算好了。这里只取缓存 ——
             * 现算的话 64KB 要几十毫秒，顶着 P2 超时。
             */
            if (!srv->routine_crc_valid)
                return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                     UDS_NRC_REQUEST_SEQUENCE_ERROR);

            if (size < 8)
                return -1;

            resp[4] = (uint8_t)(srv->routine_crc >> 24);
            resp[5] = (uint8_t)(srv->routine_crc >> 16);
            resp[6] = (uint8_t)(srv->routine_crc >> 8);
            resp[7] = (uint8_t)srv->routine_crc;
            return 8;
        }

        if (sub == UDS_ROUTINE_STOP)
            return 4;

        /* start：先算，结果留给 requestResults */
        if (parse_addr_size(mem, &req[4], (uint16_t)(len - 4), &addr, &n) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_REQUEST_OUT_OF_RANGE);

        if (mem->read == 0 ||
            compute_crc(mem, addr, n, &srv->routine_crc) != 0)
        {
            srv->routine_crc_valid = 0;
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);
        }

        srv->routine_addr = addr;
        srv->routine_size = n;
        srv->routine_crc_valid = 1;
        return 4;
    }

    /* 自定义例程 */
    if (sub == UDS_ROUTINE_START)
    {
        if (re->start == 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_SUB_FUNCTION_INVALID);
        if (re->start(re->ctx, &req[4], (uint16_t)(len - 4)) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);
        return 4;
    }

    if (sub == UDS_ROUTINE_STOP)
    {
        if (re->stop == 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_SUB_FUNCTION_INVALID);
        if (re->stop(re->ctx) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);
        return 4;
    }

    /* UDS_ROUTINE_RESULT */
    if (re->result == 0)
        return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                             UDS_NRC_SUB_FUNCTION_INVALID);

    {
        uint16_t out_len = 0;

        if (re->result(re->ctx, &resp[4], &out_len) != 0)
            return send_negative(resp, size, UDS_SID_ROUTINE_CONTROL,
                                 UDS_NRC_GENERAL_PROGRAM_FAIL);

        if ((uint16_t)(4 + out_len) > size)
            return -1;

        return (int)(4 + out_len);
    }
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


/* ---------------- 0x19 / 0x14 故障码 ---------------- */

/*
 * 往响应缓冲里追加，越界就整体失败。
 *
 * 0x19 02 的响应长度取决于故障条数，是唯一一个「长度不确定」的服务。
 * 每追加一段都查边界，比事先算长度更不容易算错。
 */
static int append(uint8_t *resp, uint16_t size, uint16_t *pos,
                  const uint8_t *data, uint16_t n)
{
    if ((uint32_t)*pos + n > size)
        return -1;

    if (n > 0)
        memcpy(resp + *pos, data, n);
    *pos = (uint16_t)(*pos + n);
    return 0;
}

static void put_dtc(uint8_t *out, uint32_t code)
{
    out[0] = (uint8_t)((code >> 16) & 0xFF);
    out[1] = (uint8_t)((code >> 8) & 0xFF);
    out[2] = (uint8_t)(code & 0xFF);
}

static uint32_t get_dtc(const uint8_t *in)
{
    return ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
}

static int read_dtc_number(uds_server_t *srv, uint8_t *resp, uint16_t size,
                           const uint8_t *req)
{
    uint16_t n = dtc_count_by_mask(srv->cfg.dtcs, req[2]);
    uint16_t pos = 0;
    uint8_t head[6];

    head[0] = UDS_SID_READ_DTC + POS_OFFSET;
    head[1] = UDS_DTC_REPORT_NUMBER_BY_MASK;
    head[2] = DTC_STATUS_AVAILABILITY_MASK;
    head[3] = UDS_DTC_FORMAT_ISO14229;
    head[4] = (uint8_t)(n >> 8);
    head[5] = (uint8_t)(n & 0xFF);

    if (append(resp, size, &pos, head, sizeof(head)) != 0)
        return -1;

    return (int)pos;
}

static int read_dtc_list(uds_server_t *srv, uint8_t *resp, uint16_t size,
                         const uint8_t *req, uint8_t sub)
{
    uint16_t pos = 0;
    uint8_t head[3];

    head[0] = UDS_SID_READ_DTC + POS_OFFSET;
    head[1] = sub;
    head[2] = DTC_STATUS_AVAILABILITY_MASK;

    if (append(resp, size, &pos, head, sizeof(head)) != 0)
        return -1;

    for (uint16_t i = 0; i < srv->cfg.dtcs->count; i++)
    {
        const dtc_entry_t *e = dtc_get(srv->cfg.dtcs, i);
        uint8_t item[4];

        if (e == 0)
            continue;

        /*
         * 0x0A 要的是「支持的全部码」，不看状态；
         * 0x02 只看掩码命中的。这是两个服务唯一的差别，
         * 混用会让 tester 在无故障时读到一堆 CDTC=0 的条目。
         */
        if (sub == UDS_DTC_REPORT_BY_MASK && (e->status & req[2]) == 0)
            continue;

        put_dtc(item, e->code);
        item[3] = e->status;

        if (append(resp, size, &pos, item, sizeof(item)) != 0)
            return -1;
    }

    return (int)pos;
}

static int read_dtc_snapshot(uds_server_t *srv, uint8_t *resp, uint16_t size,
                             const uint8_t *req)
{
    const dtc_entry_t *e = dtc_find(srv->cfg.dtcs, get_dtc(&req[2]));
    uint16_t pos = 0;
    uint8_t head[7];

    /*
     * 三种「没有」都返回 0x31：码不存在、记录号不对、该码没有快照。
     * 协议允许更细的 NRC，但 tester 对 0x31 的处理最一致，
     * 强行区分反而会在不同工具上出现兼容问题。
     */
    if (e == 0)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);
    if (req[5] != UDS_DTC_RECORD_ALL && req[5] != UDS_DTC_RECORD_SNAPSHOT)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);
    if (!e->has_snapshot)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);

    /* 59 04 <DTC3> <status> <recordNumber> <snapshot...> */
    head[0] = UDS_SID_READ_DTC + POS_OFFSET;
    head[1] = UDS_DTC_REPORT_SNAPSHOT;
    put_dtc(&head[2], e->code);
    head[5] = e->status;
    head[6] = UDS_DTC_RECORD_SNAPSHOT; /* 回填规范记录号，不回 0xFF */

    if (append(resp, size, &pos, head, sizeof(head)) != 0)
        return -1;
    if (append(resp, size, &pos, e->snapshot, e->snapshot_len) != 0)
        return -1;

    return (int)pos;
}

static int read_dtc_ext_data(uds_server_t *srv, uint8_t *resp, uint16_t size,
                             const uint8_t *req)
{
    const dtc_entry_t *e = dtc_find(srv->cfg.dtcs, get_dtc(&req[2]));
    uint16_t pos = 0;
    uint8_t head[7];
    uint8_t ext[10];

    if (e == 0)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);
    if (req[5] != UDS_DTC_RECORD_ALL && req[5] != UDS_DTC_RECORD_EXT)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);

    /* 59 06 <DTC3> <status> <recordNumber> <extData...> */
    head[0] = UDS_SID_READ_DTC + POS_OFFSET;
    head[1] = UDS_DTC_REPORT_EXT_DATA;
    put_dtc(&head[2], e->code);
    head[5] = e->status;
    head[6] = UDS_DTC_RECORD_EXT;

    /*
     * 扩展数据记录 0x01，厂商自定义格式，本实现固定 10 字节：
     *   发生次数(1) + 老化进度(1) + 首次发生时刻(4, 小端) + 最近发生时刻(4, 小端)
     * 时刻是调用方给的时间基准，单位毫秒，不保证是绝对时间。
     */
    ext[0] = e->occurrence;
    ext[1] = e->aging_cycles;
    ext[2] = (uint8_t)(e->first_ms & 0xFF);
    ext[3] = (uint8_t)((e->first_ms >> 8) & 0xFF);
    ext[4] = (uint8_t)((e->first_ms >> 16) & 0xFF);
    ext[5] = (uint8_t)((e->first_ms >> 24) & 0xFF);
    ext[6] = (uint8_t)(e->last_ms & 0xFF);
    ext[7] = (uint8_t)((e->last_ms >> 8) & 0xFF);
    ext[8] = (uint8_t)((e->last_ms >> 16) & 0xFF);
    ext[9] = (uint8_t)((e->last_ms >> 24) & 0xFF);

    if (append(resp, size, &pos, head, sizeof(head)) != 0)
        return -1;
    if (append(resp, size, &pos, ext, sizeof(ext)) != 0)
        return -1;

    return (int)pos;
}

static int handle_read_dtc(uds_server_t *srv, const uint8_t *req, uint16_t len,
                           uint8_t *resp, uint16_t size)
{
    uint8_t sub;

    if (srv->cfg.dtcs == 0)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_SERVICE_NOT_SUPPORTED);

    if (len < 2)
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);

    /* 0x19 的子功能不支持「抑制正响应」，最高位不是有效标志位，直接掩掉 */
    sub = (uint8_t)(req[1] & 0x7Fu);

    switch (sub)
    {
    case UDS_DTC_REPORT_NUMBER_BY_MASK:
        if (len != 3)
            return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);
        return read_dtc_number(srv, resp, size, req);

    case UDS_DTC_REPORT_BY_MASK:
        if (len != 3)
            return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);
        return read_dtc_list(srv, resp, size, req, sub);

    case UDS_DTC_REPORT_SNAPSHOT:
        if (len != 6)
            return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);
        return read_dtc_snapshot(srv, resp, size, req);

    case UDS_DTC_REPORT_EXT_DATA:
        if (len != 6)
            return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);
        return read_dtc_ext_data(srv, resp, size, req);

    case UDS_DTC_REPORT_SUPPORTED:
        if (len != 2)
            return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);
        return read_dtc_list(srv, resp, size, req, sub);

    default:
        return send_negative(resp, size, UDS_SID_READ_DTC, UDS_NRC_SUB_FUNCTION_INVALID);
    }
}

static int handle_clear_dtc(uds_server_t *srv, const uint8_t *req, uint16_t len,
                            uint8_t *resp, uint16_t size)
{
    uint32_t group;

    if (srv->cfg.dtcs == 0)
        return send_negative(resp, size, UDS_SID_CLEAR_DTC, UDS_NRC_SERVICE_NOT_SUPPORTED);

    if (len != 4)
        return send_negative(resp, size, UDS_SID_CLEAR_DTC, UDS_NRC_INCORRECT_LENGTH);

    /*
     * 要求扩展会话。
     *
     * 有人会说默认会话也该能清 —— 那样任何接上总线的设备都能把历史故障
     * 抹掉，售后就查不到真因了。宁可让 tester 多走一步 0x10 03。
     */
    if (!session_allows(srv, UDS_SESSION_EXTENDED))
        return send_negative(resp, size, UDS_SID_CLEAR_DTC, UDS_NRC_CONDITIONS_NOT_CORRECT);

    group = get_dtc(&req[1]);

    if (dtc_clear(srv->cfg.dtcs, group) < 0)
        return send_negative(resp, size, UDS_SID_CLEAR_DTC, UDS_NRC_REQUEST_OUT_OF_RANGE);

    if (size < 1)
        return -1;

    resp[0] = UDS_SID_CLEAR_DTC + POS_OFFSET;
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
    case UDS_SID_CLEAR_DTC:
        return handle_clear_dtc(srv, req, req_len, resp, resp_size);
    case UDS_SID_READ_DTC:
        return handle_read_dtc(srv, req, req_len, resp, resp_size);
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
    case UDS_SID_ROUTINE_CONTROL:
        return handle_routine_control(srv, req, req_len, resp, resp_size);
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

const uds_routine_result_t *uds_get_routine_result(const uds_server_t *srv)
{
    static uds_routine_result_t r;

    if (srv == 0)
    {
        r.valid = 0;
        return &r;
    }

    r.addr = srv->routine_addr;
    r.size = srv->routine_size;
    r.crc = srv->routine_crc;
    r.valid = srv->routine_crc_valid;
    return &r;
}
