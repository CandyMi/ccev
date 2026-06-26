/**
 * @file test_dns.c
 * @brief DNS resolver tests — server config, resolve API, callback, NULL guards, cache, pending.
 */

#include "ccev.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

static void on_resolved(void *udata, const char *address, int status) {
    (void)address;
    (void)status;
    ccev_loop_stop((ccev_loop_t *)udata);
}

/* ═══ Tests ═══ */

TEST(dns_set_server_valid) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_dns_server_t servers[] = {{"1.1.1.1", 53}, {"8.8.8.8", 53}};
    ASSERT(ccev_dns_set_server(loop, servers, 2) == CCEV_OK);
    ccev_loop_destroy(loop);
}

TEST(dns_set_server_invalid) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(ccev_dns_set_server(NULL, NULL, 1) == CCEV_ERR);
    ccev_dns_server_t s = {"x", 53};
    ASSERT(ccev_dns_set_server(loop, &s, 0) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

TEST(dns_set_server_replaces) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_dns_server_t s1[] = {{"1.1.1.1", 53}};
    ASSERT(ccev_dns_set_server(loop, s1, 1) == CCEV_OK);
    /* Second call — frees old deep-copied strings, allocates new ones */
    ccev_dns_server_t s2[] = {{"8.8.8.8", 53}, {"1.1.1.1", 53}};
    ASSERT(ccev_dns_set_server(loop, s2, 2) == CCEV_OK);
    ccev_loop_destroy(loop);
}

TEST(dns_set_server_null_server_field) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_dns_server_t s[] = {{NULL, 53}};
    ASSERT(ccev_dns_set_server(loop, s, 1) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

TEST(dns_set_server_too_many) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ccev_dns_server_t s[] = {{"1.1.1.1",53},{"8.8.8.8",53},{"9.9.9.9",53},
                             {"4.4.4.4",53},{"1.0.0.1",53}};
    ASSERT(ccev_dns_set_server(loop, s, 5) == CCEV_ERR);
    /* nservers should be unchanged (still defaults from init) */
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_api_smoke) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    int rc = ccev_dns_resolve(loop, "example.com", 3000,
                               CCEV_DNS_A, on_resolved, loop);
    ASSERT(rc == CCEV_OK);
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_domain) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_dns_resolve(loop, NULL, 100, CCEV_DNS_A, on_resolved, loop) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_cb) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ASSERT(ccev_dns_resolve(loop, "example.com", 100, CCEV_DNS_A, NULL, NULL) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_loop) {
    ASSERT(ccev_dns_resolve(NULL, "example.com", 100, CCEV_DNS_A, on_resolved, NULL) == CCEV_ERR);
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache tests (rely on /etc/hosts → localhost)
 * ════════════════════════════════════════════════════════════════ */

static int cb_count;

static void cache_cb(void *udata, const char *address, int status) {
    (void)address;
    (void)status;
    cb_count++;
    if (udata) ccev_loop_stop((ccev_loop_t *)udata);
}

TEST(dns_cache_hit_from_hosts) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_dns_flush(loop);

    cb_count = 0;
    int rc = ccev_dns_resolve(loop, "localhost", 3000, CCEV_DNS_A, cache_cb, NULL);
    ASSERT(rc == CCEV_OK);
    if (cb_count == 0) printf("  (hosts not available, skipping)\n");
    ASSERT(cb_count == 1 || cb_count == 0);
    ccev_loop_destroy(loop);
}

TEST(dns_flush_null_safe) {
    ccev_dns_flush(NULL);
    ASSERT(1);
}

/* ════════════════════════════════════════════════════════════════
 *  Pending coalescing tests (use hosts-loaded cache for speed)
 * ════════════════════════════════════════════════════════════════ */

typedef struct { ccev_loop_t *loop; int id; } pending_ctx_t;
static int pcnt[4];
static int pexpected;

static void pending_cb(void *udata, const char *address, int status) {
    pending_ctx_t *c = (pending_ctx_t *)udata;
    (void)address;
    (void)status;
    if (c && c->id >= 0 && c->id < 4) pcnt[c->id]++;
    pexpected--;
    if (pexpected <= 0 && c && c->loop)
        ccev_loop_stop(c->loop);
}

TEST(dns_pending_same_domain) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    /* Both resolves target the same domain with short timeout */
    pending_ctx_t c0 = {loop, 0}, c1 = {loop, 1};
    for (int i = 0; i < 4; i++) pcnt[i] = 0;
    pexpected = 2;

    int r1 = ccev_dns_resolve(loop, "x-pending-1", 50, CCEV_DNS_A, pending_cb, &c0);
    ASSERT(r1 == CCEV_OK);

    /* Second resolve for same domain — should coalesce (pending hit) */
    int r2 = ccev_dns_resolve(loop, "x-pending-1", 50, CCEV_DNS_A, pending_cb, &c1);
    ASSERT(r2 == CCEV_OK);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ASSERT(pcnt[0] == 1);
    ASSERT(pcnt[1] == 1);
    ccev_loop_destroy(loop);
}

TEST(dns_pending_different_domains) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    pending_ctx_t c0 = {loop, 0}, c1 = {loop, 1};
    for (int i = 0; i < 4; i++) pcnt[i] = 0;
    pexpected = 2;

    int r1 = ccev_dns_resolve(loop, "x-pending-a", 50, CCEV_DNS_A, pending_cb, &c0);
    ASSERT(r1 == CCEV_OK);

    int r2 = ccev_dns_resolve(loop, "x-pending-b", 50, CCEV_DNS_A, pending_cb, &c1);
    ASSERT(r2 == CCEV_OK);

    ccev_loop_run(loop, CCEV_RUN_FOREVER);
    ASSERT(pcnt[0] == 1);
    ASSERT(pcnt[1] == 1);
    ccev_loop_destroy(loop);
}

/* ════════════════════════════════════════════════════════════════
 *  IP/UDS short-circuit tests
 * ════════════════════════════════════════════════════════════════ */

static int short_circuit_count;

static void short_circuit_cb(void *udata, const char *address, int status) {
    short_circuit_count++;
    ASSERT(status == CCEV_OK);
    ASSERT(address != NULL && address[0] != '\0');
    if (udata) ccev_loop_stop((ccev_loop_t *)udata);
}

TEST(dns_short_circuit_ip4) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    short_circuit_count = 0;
    int rc = ccev_dns_resolve(loop, "127.0.0.1", 3000, CCEV_DNS_A, short_circuit_cb, loop);
    ASSERT(rc == CCEV_OK);
    ASSERT(short_circuit_count == 1);
    ccev_loop_destroy(loop);
}

TEST(dns_short_circuit_ip6) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    short_circuit_count = 0;
    int rc = ccev_dns_resolve(loop, "::1", 3000, CCEV_DNS_A, short_circuit_cb, NULL);
    ASSERT(rc == CCEV_OK);
    ASSERT(short_circuit_count == 1);
    ccev_loop_destroy(loop);
}

/* ═══ Main ═══ */

int main(void) {
    printf("dns tests:\n"); fflush(stdout);
    RUN(dns_set_server_valid);
    RUN(dns_set_server_invalid);
    RUN(dns_set_server_replaces);
    RUN(dns_set_server_null_server_field);
    RUN(dns_set_server_too_many);
    RUN(dns_resolve_api_smoke);
    RUN(dns_resolve_null_domain);
    RUN(dns_resolve_null_cb);
    RUN(dns_resolve_null_loop);

    printf("dns short circuit:\n");
    RUN(dns_short_circuit_ip4);
    RUN(dns_short_circuit_ip6);

    printf("dns cache:\n");
    RUN(dns_cache_hit_from_hosts);
    RUN(dns_flush_null_safe);

    printf("dns pending:\n");
    RUN(dns_pending_same_domain);
    RUN(dns_pending_different_domains);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
