/* pread / pwrite / ftruncate 在 c99 下需要显式打开扩展声明 */
#define _DEFAULT_SOURCE

#include "nvm_file.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 256

static int file_read(void *vctx, uint32_t off, uint8_t *buf, uint16_t len)
{
    nvm_file_ctx_t *ctx = (nvm_file_ctx_t *)vctx;
    size_t done = 0;

    if (ctx == 0 || (buf == 0 && len > 0))
        return -1;

    if (off > ctx->size || len > ctx->size - off)
        return -1;

    while (done < len)
    {
        ssize_t n = pread(ctx->fd, buf + done, len - done, (off_t)(off + done));

        if (n <= 0)
            return -1;

        done += (size_t)n;
    }

    return 0;
}

static int file_write(void *vctx, uint32_t off, const uint8_t *buf, uint16_t len)
{
    nvm_file_ctx_t *ctx = (nvm_file_ctx_t *)vctx;
    size_t done = 0;

    if (ctx == 0 || (buf == 0 && len > 0))
        return -1;

    if (off > ctx->size || len > ctx->size - off)
        return -1;

    while (done < len)
    {
        ssize_t n = pwrite(ctx->fd, buf + done, len - done, (off_t)(off + done));

        if (n <= 0)
            return -1;

        done += (size_t)n;
    }

    return 0;
}

/* 擦除 = 写 0xFF，与 Flash 擦除后的读出一致 */
static int file_erase(void *vctx, uint32_t off, uint32_t len)
{
    uint8_t blank[CHUNK];

    memset(blank, 0xFF, sizeof(blank));

    while (len > 0)
    {
        uint16_t n = (uint16_t)((len > CHUNK) ? CHUNK : len);

        if (file_write(vctx, off, blank, n) != 0)
            return -1;

        off += n;
        len -= n;
    }

    return 0;
}

int nvm_file_open(nvm_file_ctx_t *ctx, const char *path, uint32_t size)
{
    uint8_t blank[CHUNK];
    off_t cur;

    if (ctx == 0 || path == 0 || size == 0)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    ctx->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (ctx->fd < 0)
        return -1;

    ctx->size = size;

    /* 长度不足时补到 size，内容按擦除态 */
    cur = lseek(ctx->fd, 0, SEEK_END);
    if (cur < 0)
    {
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    memset(blank, 0xFF, sizeof(blank));

    if ((uint64_t)cur < size)
    {
        uint32_t off = (uint32_t)cur;

        while (off < size)
        {
            uint16_t n = (uint16_t)((size - off > CHUNK) ? CHUNK : (size - off));

            if (pwrite(ctx->fd, blank, n, (off_t)off) != (ssize_t)n)
            {
                close(ctx->fd);
                ctx->fd = -1;
                return -1;
            }

            off += n;
        }
    }

    return 0;
}

void nvm_file_close(nvm_file_ctx_t *ctx)
{
    if (ctx != 0 && ctx->fd >= 0)
    {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

const nvm_dev_t *nvm_file_dev(nvm_file_ctx_t *ctx)
{
    if (ctx == 0)
        return 0;

    ctx->dev.read = file_read;
    ctx->dev.write = file_write;
    ctx->dev.erase = file_erase;
    ctx->dev.ctx = ctx;
    ctx->dev.size = ctx->size;

    return &ctx->dev;
}
