#include "mem_nvm.h"

#include <string.h>

static int mem_read(void *vctx, uint32_t off, uint8_t *buf, uint16_t len)
{
    mem_nvm_t *m = (mem_nvm_t *)vctx;

    if (m == 0 || buf == 0 || off > m->size || len > m->size - off)
        return -1;

    memcpy(buf, &m->area[off], len);
    return 0;
}

/*
 * 写入失败时**先落一部分**再返回错误，这是掉电的真实形态：
 * 不是「什么都没写」，而是「写了一截」。
 * 双页方案要能扛住这个中间态，测试就必须造出它。
 */
static int mem_write(void *vctx, uint32_t off, const uint8_t *buf, uint16_t len)
{
    mem_nvm_t *m = (mem_nvm_t *)vctx;
    uint16_t n = len;

    if (m == 0 || buf == 0 || off > m->size || len > m->size - off)
        return -1;

    m->operations++;

    if (m->budget >= 0)
    {
        if (m->budget == 0)
            return -1;

        if (n > (uint16_t)m->budget)
            n = (uint16_t)m->budget;
    }

    for (uint16_t i = 0; i < n; i++)
        m->area[off + i] = (uint8_t)(buf[i] & (m->corrupt_mask ? m->corrupt_mask : 0xFF));

    if (m->budget >= 0)
        m->budget -= (int)n;

    /* 没写完就是失败 —— 调用方据此判断这一页不可信 */
    return (n == len) ? 0 : -1;
}

static int mem_erase(void *vctx, uint32_t off, uint32_t len)
{
    mem_nvm_t *m = (mem_nvm_t *)vctx;
    uint32_t n = len;

    if (m == 0 || off > m->size || len > m->size - off)
        return -1;

    m->operations++;

    if (m->budget >= 0)
    {
        if (m->budget == 0)
            return -1;

        if (n > (uint32_t)m->budget)
            n = (uint32_t)m->budget;
    }

    memset(&m->area[off], 0xFF, n);

    if (m->budget >= 0)
        m->budget -= (int)n;

    return (n == len) ? 0 : -1;
}

void mem_nvm_init(mem_nvm_t *m, uint32_t size)
{
    if (m == 0)
        return;

    memset(m, 0, sizeof(*m));

    if (size > MEM_NVM_MAX)
        size = MEM_NVM_MAX;

    m->size = size;
    m->budget = -1;
    mem_nvm_erase_all(m);
}

void mem_nvm_erase_all(mem_nvm_t *m)
{
    if (m == 0)
        return;

    memset(m->area, 0xFF, sizeof(m->area));
}

const nvm_dev_t *mem_nvm_dev(mem_nvm_t *m)
{
    if (m == 0)
        return 0;

    m->dev.read = mem_read;
    m->dev.write = mem_write;
    m->dev.erase = mem_erase;
    m->dev.ctx = m;
    m->dev.size = m->size;

    return &m->dev;
}
