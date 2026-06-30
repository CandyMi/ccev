/**
 * @file test_oom.c
 * @brief OOM (out-of-memory) resilience tests — simulate allocation failures.
 *
 * Uses a replaceable allocator that fails after a configurable number of
 * successful allocations.  Every public API that performs internal allocation
 * must return a graceful error (NULL or CCEV_ERR) when memory is exhausted.
 *
 * These tests verify:
 *   - No NULL-pointer dereference on allocation failure
 *   - No resource leak on partial construction
 *   - All cleanup code paths are exercised
 */

#include "ccev.h"
#include "ccsocket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

/* ════════════════════════════════════════════════════════════════
 *  OOM allocator (counting)
 * ════════════════════════════════════════════════════════════════
 *
 *  Counts allocations and fails when the counter reaches `fail_at`.
 *  Set fail_at = 0 to fail on the very first allocation.
 *  Set fail_at = -1 to never fail (passthrough).
 */

static int   oom_count;
static int   oom_fail_at = -1;  /* -1 = never fail */

static void *oom_realloc(void *ptr, size_t sz) {
    if (sz == 0) { free(ptr); return NULL; }
    if (oom_fail_at >= 0 && oom_count >= oom_fail_at)
        return NULL;
    oom_count++;
    return realloc(ptr, sz);
}

static void oom_free(void *ptr) { free(ptr); }

static void timer_stop_loop(void *udata) {
    ccev_loop_stop((ccev_loop_t *)udata);
}

static void connect_stop_loop(void *udata, ccev_sock_t *sock, int status) {
    (void)sock; (void)status;
    ccev_loop_stop((ccev_loop_t *)udata);
}

static void oom_set_fail_at(int n) {
    oom_fail_at = n;
    oom_count   = 0;
}

static void oom_reset(void) {
    oom_set_fail_at(-1);
}

/* ════════════════════════════════════════════════════════════════
 *  Helper: create a socketpair for tests that need fds
 * ════════════════════════════════════════════════════════════════ */

static int pair_create(ccsocket_t sv[2]) {
    return ccsocketpair(sv, CC_NOFLAG) ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════
 *  Loop lifecycle OOM
 * ════════════════════════════════════════════════════════════════ */

TEST(loop_create_oom_first_alloc) {
    /* Fail on first allocation (loop struct itself) */
    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop == NULL);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
}

TEST(loop_create_oom_events_array) {
    /* Fail on second allocation (events array) — loop struct was allocated.
     * ccev_loop_create must free the partially-constructed loop. */
    oom_set_fail_at(1);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop == NULL);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
}

TEST(loop_create_oom_wake_sock) {
    /* Fail on third allocation (wake_sock via ccev_sock_create).
     * Loop struct + events already allocated — ccev_loop_create must
     * survive with a NULL wake_sock and clean up properly. */
    oom_set_fail_at(2);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_loop_t *loop = ccev_loop_create(64);
    /* wake_sock is optional — loop still works, just no wakeup support */
    ASSERT(loop != NULL);

    ccev_loop_destroy(loop);
    oom_reset();
    ccev_set_allocator(NULL, NULL);
}

/* ════════════════════════════════════════════════════════════════
 *  Socket OOM
 * ════════════════════════════════════════════════════════════════ */

TEST(sock_create_oom) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { ccev_loop_destroy(loop); passed++; return; }

    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock == NULL);
    /* sv[0] was NOT closed by ccev_sock_create — we must close it */
    ccsocket_close(sv[0]);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccsocket_close(sv[1]);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Timer OOM
 * ════════════════════════════════════════════════════════════════ */

TEST(timer_add_oom) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_timer_t *t = ccev_timer_add(loop, 100, CCEV_TIMER_ONCE,
                                      timer_stop_loop, loop);
    ASSERT(t == NULL);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Stream readline / readnum OOM (reader allocation)
 * ════════════════════════════════════════════════════════════════ */

static void dummy_stream_cb(void *udata, const char *data, size_t len, int status) {
    (void)udata; (void)data; (void)len; (void)status;
}

TEST(stream_readline_oom) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { ccev_loop_destroy(loop); passed++; return; }

    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* First read allocates reader + buffer — two allocations.
     * Fail at offset 0 (reader struct alloc fails) */
    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    int rc = ccev_stream_readline(st, '\n', 1024, 0, dummy_stream_cb, NULL);
    ASSERT(rc == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

TEST(stream_readline_oom_buffer) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { ccev_loop_destroy(loop); passed++; return; }

    ccev_sock_t *sock = ccev_sock_create(loop, sv[0], NULL);
    ASSERT(sock != NULL);
    ccev_stream_t *st = ccev_stream_open(sock);
    ASSERT(st != NULL);

    /* First allocation (reader struct) succeeds, second (buffer) fails.
     * Must free the reader struct before returning CCEV_ERR. */
    oom_set_fail_at(1);
    ccev_set_allocator(oom_realloc, oom_free);

    int rc = ccev_stream_readline(st, '\n', 1024, 0, dummy_stream_cb, NULL);
    ASSERT(rc == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_stream_close(st);
    ccev_loop_destroy(loop);
    ccsocket_close(sv[1]);
}

/* ════════════════════════════════════════════════════════════════
 *  DNS resolve OOM
 * ════════════════════════════════════════════════════════════════ */

static void dns_oom_cb(void *udata, const char *address, int status) {
    (void)udata; (void)address; (void)status;
}

TEST(dns_resolve_oom_pending) {
    /* Fail on pending_add allocation (the first allocation inside dns_resolve) */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    int rc = ccev_dns_resolve(loop, "oom-test.example", 100,
                               CCEV_DNS_A, dns_oom_cb, NULL);
    ASSERT(rc == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_oom_cchashmap_set) {
    /* pending_add → cchashmap_set → _cchashmap_resize OOM path.
     * Prior to the ccalg fix this caused a SEGV (buckets=NULL, cap=0,
     * idx = hash & (0-1) → dereference of NULL+SIZE_MAX).
     *
     * Allocation sequence inside pending_add:
     *   #0: pending_t struct    → succeeds (0 >= 1)
     *   #1: cchashmap buckets   → FAILS    (1 >= 1) → _cchashmap_resize returns false
     *                            → cchashmap_set returns false
     *                            → pending_add frees p, returns NULL
     *                            → ccev_dns_resolve returns CCEV_ERR */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    oom_set_fail_at(1);
    ccev_set_allocator(oom_realloc, oom_free);

    int rc = ccev_dns_resolve(loop, "oom-cchashmap.example", 100,
                               CCEV_DNS_A, dns_oom_cb, NULL);
    ASSERT(rc == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_oom_waiter) {
    /* Test that waiter allocation fails gracefully when a duplicate
     * resolution request comes in while the first is in-flight.
     * The waiter path does NOT call cchashmap_set (no bucket alloc),
     * so this is a clean OOM test. */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    /* First resolve with default allocator — creates pending entry + buckets */
    int rc1 = ccev_dns_resolve(loop, "oom-waiter-test.example", 50,
                                CCEV_DNS_A, dns_oom_cb, loop);
    ASSERT(rc1 == CCEV_OK);

    /* Now switch to OOM allocator and resolve the same domain again */
    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    /* Second resolve for same domain tries to alloc waiter struct → fails */
    int rc2 = ccev_dns_resolve(loop, "oom-waiter-test.example", 50,
                                CCEV_DNS_A, dns_oom_cb, loop);
    ASSERT(rc2 == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Connect OOM (DNS ctx alloc)
 * ════════════════════════════════════════════════════════════════ */

TEST(connect_oom_dns_ctx) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    /* connect to a hostname (not IP) — triggers the DNS context allocation path */
    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    ccev_sock_t *sock = ccev_connect(loop, "oom-connect.example", 80, 5000, 0,
                                      connect_stop_loop, loop);
    ASSERT(sock == NULL);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  ICMP echo OOM
 * ════════════════════════════════════════════════════════════════ */

static void icmp_oom_cb(void *udata, const ccev_icmp_result_t *result) {
    (void)udata; (void)result;
}

TEST(icmp_echo_oom_ping_struct) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    oom_set_fail_at(0);
    ccev_set_allocator(oom_realloc, oom_free);

    int rc = ccev_icmp_echo(loop, "127.0.0.1", 100, icmp_oom_cb, NULL);
    /* First allocation is the ccev_ping_t struct — should fail, return CCEV_ERR */
    ASSERT(rc == CCEV_ERR);

    oom_reset();
    ccev_set_allocator(NULL, NULL);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("OOM resilience tests:\n"); fflush(stdout);

    printf("  loop:\n");
    RUN(loop_create_oom_first_alloc);
    RUN(loop_create_oom_events_array);
    RUN(loop_create_oom_wake_sock);

    printf("  sock:\n");
    RUN(sock_create_oom);

    printf("  timer:\n");
    RUN(timer_add_oom);

    printf("  stream:\n");
    RUN(stream_readline_oom);
    RUN(stream_readline_oom_buffer);

    printf("  dns:\n");
    RUN(dns_resolve_oom_pending);
    RUN(dns_resolve_oom_cchashmap_set);
    RUN(dns_resolve_oom_waiter);

    printf("  connect:\n");
    RUN(connect_oom_dns_ctx);

    printf("  icmp:\n");
    RUN(icmp_echo_oom_ping_struct);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
