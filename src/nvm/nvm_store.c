#include "nvm_store.h"

#include <string.h>

#include "crc32.h"

/* ---------------- 页头编解码 ---------------- */

/*
 * 显式按字节读写，不用 memcpy 结构体。
 * 页要在不同编译器、不同 Flash 宽度下读出同样的内容，布局必须固定。
 */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t page_base(const nvm_store_t *st, uint8_t page)
{
    return (uint32_t)page * st->page_size;
}

/* ---------------- 单页读取与校验 ---------------- */

typedef struct
{
    int ok;
    uint32_t seq;
    uint16_t len;
} page_info_t;

static page_info_t read_page(nvm_store_t *st, uint8_t page, uint8_t *payload)
{
    page_info_t info;
    uint8_t hdr[NVM_HEADER_SIZE];
    uint32_t base = page_base(st, page);

    memset(&info, 0, sizeof(info));

    if (st->dev->read(st->dev->ctx, base, hdr, sizeof(hdr)) != 0)
        return info;

    if (get_u32(&hdr[0]) != NVM_PAGE_MAGIC)
        return info;

    info.len = get_u16(&hdr[8]);

    /* 长度必须落在页内，否则页头本身就是坏的，不必再读负载 */
    if (info.len > NVM_PAYLOAD_MAX || (uint32_t)info.len > st->page_size - NVM_HEADER_SIZE)
    {
        st->rejects++;
        return info;
    }

    if (info.len > 0 &&
        st->dev->read(st->dev->ctx, base + NVM_HEADER_SIZE, payload, info.len) != 0)
        return info;

    if (crc32(payload, info.len) != get_u32(&hdr[10]))
    {
        st->rejects++;
        return info;
    }

    info.seq = get_u32(&hdr[4]);
    info.ok = 1;
    return info;
}

/* ---------------- 挂载 ---------------- */

/*
 * 序号比较用有符号差值。
 *
 * seq 会回绕（0xFFFFFFFF 之后是 0）。直接比大小的话，回绕后新页会被当成旧的，
 * 于是一直读到上一次的数据。这不是理论问题：页寿命按 10 万次算，每次加 1 要
 * 4 千万次才回绕，但代码里没有理由留这个定时炸弹。
 */
static int seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

int nvm_store_mount(nvm_store_t *st, const nvm_dev_t *dev)
{
    uint8_t payload[NVM_PAYLOAD_MAX];
    page_info_t info[2];

    if (st == 0 || dev == 0 || dev->read == 0 || dev->write == 0 || dev->erase == 0)
        return -1;

    memset(st, 0, sizeof(*st));
    st->dev = dev;
    st->active = 0xFF;
    st->page_size = dev->size / 2;

    /* 页至少要能放页头加一个字节，否则这个分区规划没有意义 */
    if (st->page_size <= NVM_HEADER_SIZE)
        return -1;

    info[0] = read_page(st, 0, payload);
    if (info[0].ok)
    {
        st->active = 0;
        st->seq = info[0].seq;
        st->len = info[0].len;
        memcpy(st->buf, payload, info[0].len);
        st->valid = 1;
    }

    info[1] = read_page(st, 1, payload);
    if (info[1].ok)
    {
        /*
         * 第二页有效。它比第一页新就替换掉，否则保留第一页。
         * 两个页都有效是正常状态 —— 上次保存成功后旧页并不会被擦掉。
         */
        if (!st->valid || seq_newer(info[1].seq, st->seq))
        {
            st->active = 1;
            st->seq = info[1].seq;
            st->len = info[1].len;
            memcpy(st->buf, payload, info[1].len);
            st->valid = 1;
        }
    }

    return 0;
}

/* ---------------- 保存 ---------------- */

int nvm_store_save(nvm_store_t *st, const uint8_t *data, uint16_t len)
{
    uint8_t hdr[NVM_HEADER_SIZE];
    uint8_t verify[NVM_PAYLOAD_MAX];
    uint8_t target;
    uint32_t base;
    uint32_t new_seq;

    if (st == 0 || st->dev == 0 || (data == 0 && len > 0))
        return -1;

    if (len > NVM_PAYLOAD_MAX)
        return -1;

    /*
     * 目标页：当前有效页的另一页。
     * 还没有有效页时（首次上电或两页都坏）从第 0 页开始。
     */
    target = (st->active == 0xFF) ? 0 : (uint8_t)(1 - st->active);
    base = page_base(st, target);
    new_seq = st->valid ? st->seq + 1 : 1;

    if (st->dev->erase(st->dev->ctx, base, st->page_size) != 0)
        return -1;

    put_u32(&hdr[0], NVM_PAGE_MAGIC);
    put_u32(&hdr[4], new_seq);
    put_u16(&hdr[8], len);
    put_u32(&hdr[10], crc32(data, len));
    hdr[14] = 0xFF;
    hdr[15] = 0xFF;

    if (st->dev->write(st->dev->ctx, base, hdr, sizeof(hdr)) != 0)
        return -1;

    if (len > 0 && st->dev->write(st->dev->ctx, base + NVM_HEADER_SIZE, data, len) != 0)
        return -1;

    /*
     * 回读校验。
     *
     * Flash 写入可能静默失败（写保护、扇区已磨损、供电跌落），
     * 只相信「我刚才写成功了」等于没有校验。读回来重算 CRC 才知道
     * 这一页到底能不能用 —— 不能的话必须保持旧页有效，不能更新状态。
     */
    if (st->dev->read(st->dev->ctx, base, hdr, sizeof(hdr)) != 0)
        return -1;

    if (get_u32(&hdr[0]) != NVM_PAGE_MAGIC || get_u16(&hdr[8]) != len ||
        get_u32(&hdr[4]) != new_seq)
        return -1;

    if (len > 0 && st->dev->read(st->dev->ctx, base + NVM_HEADER_SIZE, verify, len) != 0)
        return -1;

    if (crc32(verify, len) != get_u32(&hdr[10]))
        return -1;

    if (len > 0)
        memcpy(st->buf, data, len);

    st->len = len;
    st->seq = new_seq;
    st->active = target;
    st->valid = 1;
    st->saves++;

    return 0;
}

const uint8_t *nvm_store_data(const nvm_store_t *st, uint16_t *len)
{
    if (st == 0 || !st->valid)
        return 0;

    if (len != 0)
        *len = st->len;

    return st->buf;
}
