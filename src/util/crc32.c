#include "crc32.h"

/*
 * 位运算版，不用查找表。
 *
 * 查找表能快 8 倍，但要 1KB 常量空间。这里三处调用点都不在热路径上：
 * Bootloader 一次跳转前校验一次、0x31 一次请求算一遍、参数区一次上电读一次。
 * 省下的 1KB 留给 Bootloader 更值。
 *
 * 进出取反（crc = ~crc）是为了让增量更新与一次性计算结果一致 ——
 * 反射算法的标准做法。
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (data == 0 && len > 0)
        return crc;

    crc = ~crc;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }

    return ~crc;
}

uint32_t crc32(const uint8_t *data, uint32_t len)
{
    return crc32_update(0, data, len);
}
