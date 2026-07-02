/**
 * @file ccev_timer.c
 * @brief Timer subsystem — ccheap (4-ary) with heap_index for O(log n) update.
 *
 * @author CandyMi
 * @license MIT
 */

#include "ccev_internal.h"

int ccev__timer_process(ccev_loop_t *loop, uint64_t now_ms) {
    /* Phase 1: extract all expired timers from heap into local list.
     *          Pure heap ops — no callbacks, no heap mutation after this. */
    cclink_t expired;
    cclink_init(&expired);

    while (1) {
        ccheap_node_t *min = ccheap_peek(&loop->timers);
        if (!min) break;

        ccev_timer_t *timer = CCHEAP_CONTAINER(min, ccev_timer_t, node);
        if (ccheap_node_get(&timer->node, timeout) > now_ms) break;

        ccheap_pop(&loop->timers);

        if (!timer->active) {
            ccev__free_fn(timer);
            continue;
        }

        cclink_push_back(&expired, &timer->tlist);
    }

    /* Phase 2: fire all expired callbacks (timer priority > I/O). */
    while (!cclink_empty(&expired)) {
        cclink_node_t *n = cclink_pop_front(&expired);
        ccev_timer_t *timer = CCLINK_CONTAINER(n, ccev_timer_t, tlist);

        timer->cb(timer->udata);

        if (timer->active && timer->mode == CCEV_TIMER_REPEAT) {
            ccheap_node_set(&timer->node, timeout, ccev__now_ms() + timer->interval);
            ccheap_insert(&loop->timers, &timer->node);
        } else if (timer->active) {
            ccev__free_fn(timer);
            loop->timer_count--;
        } else {
            ccev__free_fn(timer);
        }
    }

    /* Phase 3: return ms until next future timer (-1 if none). */
    ccheap_node_t *earliest = ccheap_peek(&loop->timers);
    if (!earliest) return -1;
    uint64_t t = ccheap_node_get(earliest, timeout);
    uint64_t now = ccev__now_ms();
    if (t <= now) return 0;
    uint64_t diff = t - now;
    return (diff > (uint64_t)INT_MAX) ? -1 : (int)diff;
}

ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata) {
    if (!loop || !cb) return NULL;

    ccev_timer_t *timer = (ccev_timer_t *)ccev__realloc_fn(NULL, sizeof(ccev_timer_t));
    if (!timer) return NULL;

    timer->loop       = loop;
    ccheap_node_set(&timer->node, timeout, ccev__now_ms() + delay_ms);
    timer->interval   = (mode == CCEV_TIMER_REPEAT) ? delay_ms : 0;
    timer->mode       = mode;
    timer->cb         = cb;
    timer->udata      = udata;
    timer->active     = true;

    ccheap_insert(&loop->timers, &timer->node);
    loop->timer_count++;
    return timer;
}

int ccev_timer_del(ccev_loop_t *loop, ccev_timer_t *timer) {
    if (!loop || !timer || !timer->active) return CCEV_ERR;
    timer->active = false;
    loop->timer_count--;
    return CCEV_OK;
}

int ccev_timer_reset(ccev_loop_t *loop, ccev_timer_t *timer,
                      uint64_t delay_ms) {
    if (!loop || !timer) return CCEV_ERR;

    uint64_t new_expiry = ccev__now_ms() + delay_ms;

    if (!timer->active) {
        timer->active    = true;
        ccheap_node_set(&timer->node, timeout, new_expiry);
        ccheap_insert(&loop->timers, &timer->node);
        loop->timer_count++;
    } else {
        ccheap_node_set(&timer->node, timeout, new_expiry);
        ccheap_update(&loop->timers, &timer->node);
    }
    return CCEV_OK;
}

int ccev_timer_count(ccev_loop_t *loop) {
    return loop ? loop->timer_count : 0;
}
