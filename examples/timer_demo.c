/**
 * @file timer_demo.c
 * @brief Demonstrates ccev timer functionality.
 *
 * Creates three timers:
 *   1. One-shot timer that fires after 500ms (stops the loop)
 *   2. Repeat timer that fires every 200ms (3 times)
 *   3. Timer that is reset before it expires
 */

#include "ccev.h"
#include <stdio.h>

typedef struct {
    ccev_loop_t *loop;
    int          once_count;
    int          repeat_count;
    int          reset_count;
} demo_ctx;

static void on_once(void *udata) {
    demo_ctx *ctx = (demo_ctx *)udata;
    ctx->once_count++;
    printf("  [once] timer fired! (%d)\n", ctx->once_count);
    ccev_loop_stop(ctx->loop);
}

static void on_repeat(void *udata) {
    demo_ctx *ctx = (demo_ctx *)udata;
    ctx->repeat_count++;
    printf("  [repeat] timer fired! (%d)\n", ctx->repeat_count);
    if (ctx->repeat_count >= 3)
        ccev_loop_stop(ctx->loop);
}

static void on_reset_expiry(void *udata) {
    demo_ctx *ctx = (demo_ctx *)udata;
    ctx->reset_count++;
    printf("  [reset] timer fired! (%d)\n", ctx->reset_count);
    if (ctx->reset_count >= 1)
        ccev_loop_stop(ctx->loop);
}

int main(void) {
    printf("ccev timer demo:\n");

    ccev_loop_t *loop = ccev_loop_create(64);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    demo_ctx ctx = { .loop = loop };

    /* 1. One-shot timer — fires once after 500ms */
    ccev_timer_add(loop, 500, CCEV_TIMER_ONCE, on_once, &ctx);

    /* 2. Repeat timer — fires every 200ms, 3 times then stops */
    ccev_timer_add(loop, 200, CCEV_TIMER_REPEAT, on_repeat, &ctx);

    /* 3. Timer with reset — initially 10s, reset to 100ms */
    ccev_timer_t *t = ccev_timer_add(loop, 10000, CCEV_TIMER_ONCE,
                                       on_reset_expiry, &ctx);
    ccev_timer_reset(loop, t, 100);

    printf("  running event loop...\n");
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    printf("  summary: once=%d, repeat=%d, reset=%d\n",
           ctx.once_count, ctx.repeat_count, ctx.reset_count);

    ccev_loop_destroy(loop);
    return 0;
}
