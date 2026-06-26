/**
 * @file ccev_timer.c
 * @brief Timer subsystem — ccheap (4-ary) with heap_index for O(log n) update.
 */

#include "ccev_internal.h"

#if defined(__APPLE__)
#  include <mach/mach_time.h>
#endif

uint64_t ccev__now_ms(void) {
#if defined(_WIN32)
    /* QueryPerformanceCounter — microsecond resolution vs GetTickCount64 (16ms) */
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER count;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000LL) / freq.QuadPart);
#elif defined(__APPLE__)
    /* macOS: mach_absolute_time() is monotonic */
    uint64_t ns = mach_absolute_time();
    static mach_timebase_info_data_t info = {0};
    if (info.denom == 0) mach_timebase_info(&info);
    return (ns * info.numer) / (info.denom * 1000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

static int64_t timer_cmp(const ccheap_node_t *a, const ccheap_node_t *b) {
    const ccev_timer_t *ta = CCHEAP_CONTAINER(a, ccev_timer_t, node);
    const ccev_timer_t *tb = CCHEAP_CONTAINER(b, ccev_timer_t, node);
    if (ta->expiry < tb->expiry) return  1;
    if (ta->expiry > tb->expiry) return -1;
    return 0;
}

void ccev__timer_process(ccev_loop_t *loop, uint64_t now_ms) {
    while (1) {
        ccheap_node_t *min = ccheap_peek(&loop->timers);
        if (!min) break;

        ccev_timer_t *timer = CCHEAP_CONTAINER(min, ccev_timer_t, node);
        if (timer->expiry > now_ms) break;

        ccheap_pop(&loop->timers);

        if (!timer->active) {
            /* Already decremented by ccev_timer_del */
            loop->free_fn(timer);
            continue;
        }

        timer->cb(timer->udata);

        if (timer->mode == CCEV_TIMER_REPEAT) {
            timer->expiry = ccev__now_ms() + timer->interval;
            ccheap_insert(&loop->timers, &timer->node);
        } else {
            loop->free_fn(timer);
            loop->timer_count--;
        }
    }
}

ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata) {
    if (!loop || !cb) return NULL;

    ccev_timer_t *timer = (ccev_timer_t *)loop->realloc_fn(NULL, sizeof(ccev_timer_t));
    if (!timer) return NULL;

    timer->loop       = loop;
    timer->expiry     = ccev__now_ms() + delay_ms;
    timer->interval   = (mode == CCEV_TIMER_REPEAT) ? delay_ms : 0;
    timer->mode       = mode;
    timer->cb         = cb;
    timer->udata      = udata;
    timer->active     = true;

    /* Initialize the heap on first use (loop_create sets data=NULL) */
    if (!loop->timers.data)
        ccheap_init(&loop->timers, timer_cmp);
    ccheap_insert(&loop->timers, &timer->node);
    loop->timer_count++;

    /* Linux: update timerfd */
#if defined(__linux__) || defined(__ANDROID__)
    if (loop->timerfd >= 0) {
        ccheap_node_t *earliest = ccheap_peek(&loop->timers);
        if (earliest) {
            ccev_timer_t *t = CCHEAP_CONTAINER(earliest, ccev_timer_t, node);
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            uint64_t rem = (t->expiry > ccev__now_ms()) ? t->expiry - ccev__now_ms() : 0;
            its.it_value.tv_sec  = (time_t)(rem / 1000ULL);
            its.it_value.tv_nsec = (long)((rem % 1000ULL) * 1000000L);
            timerfd_settime(loop->timerfd, 0, &its, NULL);
        }
    }
#endif
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
        timer->expiry    = new_expiry;
        ccheap_insert(&loop->timers, &timer->node);
        loop->timer_count++;
    } else {
        timer->expiry = new_expiry;
        ccheap_update(&loop->timers, &timer->node);
    }
    return CCEV_OK;
}

int ccev_timer_count(ccev_loop_t *loop) {
    return loop ? loop->timer_count : 0;
}
