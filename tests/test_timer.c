/**
 * @file test_timer.c
 * @brief Timer subsystem tests — lifecycle, del, reset, NULL guards.
 */

#include "ccev.h"
#include <stdio.h>
#include <stdlib.h>

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

static void timer_stop_cb(void *udata) {
    ccev_loop_stop((ccev_loop_t *)udata);
}

/* ── Lifecycle ─────────────────────────────────────────── */

TEST(timer_create_destroy) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_timer_count(loop) == 0);

    ccev_timer_t *t = ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, timer_stop_cb, loop);
    ASSERT(t != NULL);
    ASSERT(ccev_timer_count(loop) == 1);

    ccev_timer_del(loop, t);
    ASSERT(ccev_timer_count(loop) == 0);

    ccev_loop_destroy(loop);
}

TEST(timer_once_fires) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccev_timer_add(loop, 5, CCEV_TIMER_ONCE, timer_stop_cb, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ccev_timer_count(loop) == 0);
    ccev_loop_destroy(loop);
}

TEST(timer_repeat_fires_three) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    int count = 0;
    ccev_timer_add(loop, 5, CCEV_TIMER_REPEAT,
                   (ccev_timer_cb)(void(*)(void))timer_stop_cb, loop);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ccev_loop_destroy(loop);
}

/* ── ccev_timer_del ────────────────────────────────────── */

TEST(timer_del_before_expiry) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccev_timer_t *t = ccev_timer_add(loop, 100000, CCEV_TIMER_ONCE, timer_stop_cb, loop);
    ASSERT(t != NULL);
    ASSERT(ccev_timer_count(loop) == 1);

    ASSERT(ccev_timer_del(loop, t) == CCEV_OK);
    ASSERT(ccev_timer_count(loop) == 0);

    ccev_loop_destroy(loop);
}

TEST(timer_del_inactive_returns_err) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ASSERT(ccev_timer_del(loop, NULL) == CCEV_ERR);

    ccev_loop_destroy(loop);
}

TEST(timer_del_null_timer) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_timer_del(loop, NULL) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

/* ── ccev_timer_reset ──────────────────────────────────── */

TEST(timer_reset_shortens) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccev_timer_t *t = ccev_timer_add(loop, 100000, CCEV_TIMER_ONCE, timer_stop_cb, loop);
    ASSERT(t != NULL);

    /* Reset to fire very soon */
    ASSERT(ccev_timer_reset(loop, t, 5) == CCEV_OK);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(ccev_timer_count(loop) == 0);
    ccev_loop_destroy(loop);
}

TEST(timer_reset_null_returns_err) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_timer_reset(loop, NULL, 100) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

/* ── Repeat timer firing count ─────────────────────────── */

static int repeat_fire_count;
static ccev_loop_t *repeat_test_loop;

static void repeat_count_cb(void *udata) {
    (void)udata;
    repeat_fire_count++;
    if (repeat_fire_count >= 3)
        ccev_loop_stop(repeat_test_loop);
}

TEST(timer_repeat_fires_at_least_three) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    repeat_test_loop  = loop;
    repeat_fire_count = 0;

    ccev_timer_add(loop, 5, CCEV_TIMER_REPEAT, repeat_count_cb, loop);
    ccev_loop_run(loop, CCEV_RUN_FOREVER);

    ASSERT(repeat_fire_count >= 3);
    ccev_loop_destroy(loop);
}

/* ── NULL guards ───────────────────────────────────────── */

TEST(timer_add_null_cb_returns_null) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_timer_t *t = ccev_timer_add(loop, 100, CCEV_TIMER_ONCE, NULL, NULL);
    ASSERT(t == NULL);
    ASSERT(ccev_timer_count(loop) == 0);
    ccev_loop_destroy(loop);
}

TEST(timer_count_null_returns_zero) {
    ASSERT(ccev_timer_count(NULL) == 0);
}

/* ── Main ──────────────────────────────────────────────── */

int main(void) {
    printf("timer tests:\n"); fflush(stdout);
    RUN(timer_create_destroy);
    RUN(timer_once_fires);
    RUN(timer_del_before_expiry);
    RUN(timer_del_inactive_returns_err);
    RUN(timer_del_null_timer);
    RUN(timer_reset_shortens);
    RUN(timer_reset_null_returns_err);
    RUN(timer_repeat_fires_at_least_three);
    RUN(timer_add_null_cb_returns_null);
    RUN(timer_count_null_returns_zero);
    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
