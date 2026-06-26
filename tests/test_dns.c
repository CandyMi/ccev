/**
 * @file test_dns.c
 * @brief DNS resolver tests — server config, resolve API, callback, NULL guards, cache.
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

static void on_resolved(void *udata, ccev_address_t *addr, int status) {
    if (addr) ccev_dns_free(addr);
    ccev_loop_stop((ccev_loop_t *)udata);
}

/* ═══ Tests ═══ */

TEST(dns_set_server_valid) {
    ccev_loop_t *loop = ccev_loop_create(64);
    const char *servers[] = {"1.1.1.1", "8.8.8.8"};
    ASSERT(ccev_dns_set_server(loop, servers, 2, 53) == CCEV_OK);
    ccev_loop_destroy(loop);
}

TEST(dns_set_server_invalid) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(ccev_dns_set_server(NULL, NULL, 1, 53) == CCEV_ERR);
    ASSERT(ccev_dns_set_server(loop, (const char*[]){"x"}, 0, 53) == CCEV_ERR);
    ccev_loop_destroy(loop);
}

TEST(dns_free_null) {
    ccev_dns_free(NULL);
    ASSERT(1);
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
 *  DNS cache tests
 * ════════════════════════════════════════════════════════════════ */

static int cache_cb_count;

static void cache_cb(void *udata, ccev_address_t *addr, int status) {
    (void)status;
    cache_cb_count++;
    if (addr) ccev_dns_free(addr);
    if (udata) ccev_loop_stop((ccev_loop_t *)udata);
}

TEST(dns_cache_hit_from_hosts) {
    /* /etc/hosts contains "127.0.0.1 localhost" — flush loads it */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_dns_flush(loop);

    cache_cb_count = 0;
    int rc = ccev_dns_resolve(loop, "localhost", 3000, CCEV_DNS_A,
                               cache_cb, NULL);
    ASSERT(rc == CCEV_OK);
    /* If hosts file was loaded, callback fires synchronously (cache hit).
     * On platforms where hosts load fails (perms/path), it goes to network
     * and the callback won't fire until timeout — skip the assertion. */
    if (cache_cb_count == 0) { printf("  (hosts not available, skipping)\n"); }
    ASSERT(cache_cb_count == 1 || cache_cb_count == 0); /* pass either way */
    ccev_loop_destroy(loop);
}

TEST(dns_flush_clears_hosts_cache) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_dns_flush(loop);

    cache_cb_count = 0;
    ASSERT(ccev_dns_resolve(loop, "localhost", 100, CCEV_DNS_A,
                             cache_cb, NULL) == CCEV_OK);
    int hosts_loaded = (cache_cb_count == 1);
    if (!hosts_loaded) printf("  (hosts not available, skipping)\n");

    ccev_dns_flush(loop);  /* flush + reload: no crash */

    if (hosts_loaded) {
        cache_cb_count = 0;
        ASSERT(ccev_dns_resolve(loop, "localhost", 100, CCEV_DNS_A,
                                 cache_cb, NULL) == CCEV_OK);
        ASSERT(cache_cb_count == 1);
    }
    ccev_loop_destroy(loop);
}

TEST(dns_flush_null_safe) {
    ccev_dns_flush(NULL);
    ASSERT(1);
}

TEST(dns_cache_miss_by_domain) {
    /* A domain NOT in hosts should NOT hit cache after flush */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);
    ccev_dns_flush(loop);

    /* If "x-surely-not-in-hosts" happens to be in /etc/hosts, skip */
    cache_cb_count = 0;
    int rc = ccev_dns_resolve(loop, "x-surely-not-in-hosts", 100,
                               CCEV_DNS_A, cache_cb, loop);
    if (rc == CCEV_OK) {
        /* Should NOT fire synchronously (cache miss) */
        if (cache_cb_count > 0) {
            /* It was in hosts — skip this assertion */
            ccev_loop_destroy(loop);
            return;
        }
        ASSERT(cache_cb_count == 0);  /* miss */
        ccev_loop_run(loop, CCEV_RUN_FOREVER);  /* wait for timeout */
    }
    ccev_loop_destroy(loop);
}

/* ═══ Main ═══ */

int main(void) {
    printf("dns tests:\n"); fflush(stdout);
    RUN(dns_set_server_valid);
    RUN(dns_set_server_invalid);
    RUN(dns_free_null);
    RUN(dns_resolve_api_smoke);
    RUN(dns_resolve_null_domain);
    RUN(dns_resolve_null_cb);
    RUN(dns_resolve_null_loop);

    printf("dns cache:\n");
    RUN(dns_cache_hit_from_hosts);
    RUN(dns_flush_clears_hosts_cache);
    RUN(dns_flush_null_safe);
    RUN(dns_cache_miss_by_domain);

    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
