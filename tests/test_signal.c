/**
 * @file test_signal.c
 * @brief Signal handling tests — register, fire, ignore, NULL guards.
 *
 * Uses SIGTERM (available on all POSIX + Windows) for cross-platform
 * coverage.  Signal delivery happens via self-pipe trick: the OS
 * signal handler writes one byte to a pipe; the event loop reads it
 * and fires the registered callback.
 */

#include "ccev.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

/* ════════════════════════════════════════════════════════════════
 *  Signal fired flag
 * ════════════════════════════════════════════════════════════════ */

static volatile int sig_fired;

static void on_signal(void *udata, int signum) {
    (void)signum;
    sig_fired = 1;
    ccev_loop_stop((ccev_loop_t *)udata);
}

/* ════════════════════════════════════════════════════════════════
 *  Tests
 * ════════════════════════════════════════════════════════════════ */

TEST(signal_register_and_fire) {
    ccev_loop_t *loop = ccev_default_loop();
    ASSERT(loop != NULL);

    sig_fired = 0;
    ASSERT(ccev_signal_handle(SIGTERM, on_signal, loop) == CCEV_OK);

    /* Self-deliver SIGTERM — handler writes to self-pipe */
    raise(SIGTERM);

    /* Run the loop — callback should fire and stop the loop */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(sig_fired == 1);

    /* Restore default disposition so subsequent tests are clean */
    ccev_signal_ignore(SIGTERM);
}

TEST(signal_ignore_api) {
    ccev_loop_t *loop = ccev_default_loop();
    ASSERT(loop != NULL);

    ASSERT(ccev_signal_handle(SIGTERM, on_signal, loop) == CCEV_OK);
    /* After ignore, the signal has SIG_DFL — don't raise, just verify API */
    ASSERT(ccev_signal_ignore(SIGTERM) == CCEV_OK);
}

TEST(signal_invalid_signum) {
    /* signum 0 and >63 should be rejected */
    ccev_loop_t *loop = ccev_default_loop();
    ASSERT(loop != NULL);

    ASSERT(ccev_signal_handle(0, on_signal, loop) == CCEV_ERR);
    ASSERT(ccev_signal_handle(64, on_signal, loop) == CCEV_ERR);
    ASSERT(ccev_signal_ignore(0) == CCEV_ERR);

    ccev_signal_ignore(SIGTERM);
}

TEST(signal_null_cb) {
    ccev_loop_t *loop = ccev_default_loop();
    ASSERT(loop != NULL);
    ASSERT(ccev_signal_handle(SIGTERM, NULL, loop) == CCEV_ERR);
    ccev_signal_ignore(SIGTERM);
}

/* File-scope state for reregister test */
static int rereg_first_fired;
static int rereg_second_fired;

static void rereg_first_cb(void *u, int s) { (void)u; (void)s; rereg_first_fired = 1; }
static void rereg_second_cb(void *u, int s) {
    (void)s; rereg_second_fired = 1;
    ccev_loop_stop((ccev_loop_t *)u);
}

TEST(signal_reregister_overwrites) {
    ccev_loop_t *loop = ccev_default_loop();
    ASSERT(loop != NULL);

    rereg_first_fired = 0;
    rereg_second_fired = 0;

    ASSERT(ccev_signal_handle(SIGTERM, rereg_first_cb, loop) == CCEV_OK);
    ASSERT(ccev_signal_handle(SIGTERM, rereg_second_cb, loop) == CCEV_OK);

    raise(SIGTERM);
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    ASSERT(rereg_first_fired == 0);   /* overwritten */
    ASSERT(rereg_second_fired == 1);  /* only the latest fires */

    ccev_signal_ignore(SIGTERM);
}

/* ════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("signal:\n"); fflush(stdout);

    RUN(signal_register_and_fire);
    RUN(signal_ignore_api);
    RUN(signal_invalid_signum);
    RUN(signal_null_cb);
    RUN(signal_reregister_overwrites);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
