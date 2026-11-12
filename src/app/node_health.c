#include "node_health.h"

#include <string.h>

void node_health_init(node_health_t *h)
{
    if (h == 0)
        return;

    memset(h, 0, sizeof(*h));
    h->state = HEALTH_NORMAL;
    h->window_ms = HEALTH_WINDOW_MS;
}

/*
 * 状态推进。
 *
 * 优先级从高到低：Bus Off > 恢复观察 > 降级 > 正常。
 * 顺序不能反——比如 Bus Off 期间 TEC 可能已经回落，但总线还没恢复，
 * 这时候如果按 TEC 判定成 NORMAL，就会往死掉的总线上发报文。
 */
void node_health_update(node_health_t *h, const can_bus_status_t *st, uint32_t elapsed_ms)
{
    int was_busoff;

    if (h == 0 || st == 0)
        return;

    /* 统计窗口滚动 */
    if (h->window_ms > elapsed_ms)
    {
        h->window_ms -= elapsed_ms;
    }
    else
    {
        h->window_ms = HEALTH_WINDOW_MS;
        h->busoff_in_window = 0;
    }

    h->last_tec = st->tec;
    was_busoff = (h->state == HEALTH_BUSOFF);

    if (st->level == CAN_LEVEL_BUSOFF)
    {
        if (!was_busoff)
        {
            h->busoff_total++;
            if (h->busoff_in_window < 255)
                h->busoff_in_window++;

            /* 短时间内反复 Bus Off，多半是接线或终端电阻问题，软件救不回来 */
            if (h->busoff_in_window >= HEALTH_BUSOFF_LIMIT)
                h->long_degraded = 1;
        }

        h->state = HEALTH_BUSOFF;
        h->recover_ms = HEALTH_RECOVER_MS;
        return;
    }

    /* 刚从 Bus Off 出来，进观察期 */
    if (was_busoff)
    {
        h->state = HEALTH_RECOVERING;
        h->recover_ms = HEALTH_RECOVER_MS;
    }

    if (h->state == HEALTH_RECOVERING)
    {
        if (h->recover_ms > elapsed_ms)
        {
            h->recover_ms -= elapsed_ms;
            return;
        }
        h->recover_ms = 0;
        h->state = HEALTH_NORMAL;
    }

    /* 错误被动：还没到 Bus Off，但已经在恶化，此时该主动降速 */
    if (st->tec >= HEALTH_DEGRADED_TEC || st->level == CAN_LEVEL_PASSIVE)
    {
        h->state = HEALTH_DEGRADED;
        return;
    }

    if (h->state != HEALTH_RECOVERING)
        h->state = HEALTH_NORMAL;
}

int node_health_can_send(const node_health_t *h)
{
    if (h == 0)
        return 0;

    /* Bus Off 时发送没有意义：控制器已经脱离总线，写进去也是被丢弃 */
    return (h->state == HEALTH_BUSOFF) ? 0 : 1;
}

uint32_t node_health_period_scale(const node_health_t *h)
{
    if (h == 0)
        return 1;

    if (h->state == HEALTH_BUSOFF)
        return 0;

    /* 降级或长期降级都把周期拉长一倍，减少总线竞争与错误累积 */
    if (h->state == HEALTH_DEGRADED || h->long_degraded)
        return 2;

    return 1;
}

const char *node_health_state_name(const node_health_t *h)
{
    if (h == 0)
        return "?";

    switch (h->state)
    {
    case HEALTH_NORMAL:
        return "normal";
    case HEALTH_DEGRADED:
        return "degraded";
    case HEALTH_BUSOFF:
        return "busoff";
    case HEALTH_RECOVERING:
        return "recovering";
    default:
        return "?";
    }
}
