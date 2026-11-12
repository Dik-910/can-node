#ifndef __NODE_HEALTH_H
#define __NODE_HEALTH_H

#include <stdint.h>

#include "can_driver.h"

/*
 * 总线健康管理。
 *
 * 节点遇到错误不能只是「上报一下」就完事，必须有应对策略，否则一个小问题会滚成大雪球：
 *   TEC 升高时如果还满速发报文，只会让错误计数继续涨，很快进 Bus Off
 *   Bus Off 时如果还照常发，等于往已经断掉的总线上灌数据，毫无意义还耗电
 *
 * 四种状态与对应动作：
 *
 *   NORMAL      正常，按标称周期发送
 *   DEGRADED    错误被动（TEC > 127），周期拉长一倍——降低总线压力，给恢复留机会
 *   BUSOFF      总线关闭，停止一切发送，只等硬件恢复
 *   RECOVERING  刚恢复，观察一段时间确认稳定后再回正常
 *
 * 另有长期降级：短时间内反复 Bus Off 说明是硬件/接线问题，软件救不了，
 * 只能降频维持最低功能并报故障，等人来修。
 */

#define HEALTH_NORMAL      0
#define HEALTH_DEGRADED    1
#define HEALTH_BUSOFF      2
#define HEALTH_RECOVERING  3

#define HEALTH_BUSOFF_LIMIT     3      /* 一个窗口内允许的 Bus Off 次数 */
#define HEALTH_WINDOW_MS        60000  /* 统计窗口：1 分钟 */
#define HEALTH_RECOVER_MS       2000   /* 恢复后的观察期 */
#define HEALTH_DEGRADED_TEC     128    /* 进入降级的 TEC 阈值 */

typedef struct
{
    uint8_t state;
    uint8_t busoff_in_window; /* 当前窗口内的 Bus Off 次数 */
    uint8_t long_degraded;    /* 1 表示已判定为硬件问题，长期降频 */

    uint32_t window_ms;       /* 窗口剩余时间 */
    uint32_t recover_ms;      /* 恢复观察期剩余时间 */
    uint32_t busoff_total;    /* 累计 Bus Off 次数，用于上报 */
    uint8_t last_tec;
} node_health_t;

void node_health_init(node_health_t *h);

/* 每次主循环调用，传入最新总线状态与经过的毫秒数 */
void node_health_update(node_health_t *h, const can_bus_status_t *st, uint32_t elapsed_ms);

/* 当前是否允许发送应用报文 */
int node_health_can_send(const node_health_t *h);

/*
 * 周期缩放系数：返回 1 表示按标称周期，2 表示周期加倍（频率减半），
 * 0 表示不要发送。调用方用它调整发送间隔。
 */
uint32_t node_health_period_scale(const node_health_t *h);

const char *node_health_state_name(const node_health_t *h);

#endif
