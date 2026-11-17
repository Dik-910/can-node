/*
 * nvm_file.h —— 参数区的 PC 侧实现
 *
 * 用一个固定长度的文件模拟 Flash：读 0xFF 表示已擦除，写只能把 1 变成 0
 * 这件事由上层保证（跟真实 Flash 一样，本层不额外校验）。
 * 这样在 PC 上就能验证「双页交替」「掉电半写」「CRC 坏页回退」这些
 * 只有真 Flash 才暴露的行为，不用等硬件。
 */
#ifndef NVM_FILE_H
#define NVM_FILE_H

#include <stdint.h>

#include "nvm.h"

typedef struct
{
    int fd;
    uint32_t size;

    /*
     * 设备接口放在 ctx 里而不是用全局单例：
     * 测试里会同时开两个参数区（比如模拟两块板子），
     * 单例会让 dev.ctx 互相覆盖。
     */
    nvm_dev_t dev;
} nvm_file_ctx_t;

/*
 * 打开或创建一个 size 字节的参数区。
 * 文件不存在时按擦除态（全 0xFF）初始化；长度不足时补 0xFF。
 * 返回 0 成功。
 */
int nvm_file_open(nvm_file_ctx_t *ctx, const char *path, uint32_t size);

void nvm_file_close(nvm_file_ctx_t *ctx);

/* 取设备接口。ctx 必须比返回的指针活得久 */
const nvm_dev_t *nvm_file_dev(nvm_file_ctx_t *ctx);

#endif
