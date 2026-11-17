/*
 * crc32.h —— 反射 CRC-32（多项式 0xEDB88320）
 *
 * 三个地方要用同一个算法：
 *   - Bootloader 跳转前校验固件映像
 *   - 0x31 0x0202 例程，让诊断仪刷完自己校验
 *   - 参数区的页校验
 *
 * 前两处必须算出一模一样的值，否则会出现「诊断说成功、Bootloader 说映像损坏」。
 * 所以实现只留这一份。
 */
#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

/* 增量更新。多次 update 与一次性计算等价，crc 初值传 0 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/* 一次性计算 */
uint32_t crc32(const uint8_t *data, uint32_t len);

#endif
