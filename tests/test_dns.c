/**
 * @file test_dns.c
 * @brief DNS resolver tests — server config, resolve API, callback, NULL guards.
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
    const char *servers[] = {"1.1.1.1", "8.8.8.8"};
    ASSERT(ccev_dns_set_server(servers, 2) == CCEV_OK);
}

TEST(dns_set_server_invalid) {
    ASSERT(ccev_dns_set_server(NULL, 1) == CCEV_ERR);
    ASSERT(ccev_dns_set_server((const char*[]){"x"}, 0) == CCEV_ERR);
}

TEST(dns_free_null) {
    ccev_dns_free(NULL);
    ASSERT(1);
}

TEST(dns_resolve_api_smoke) {
    /* Verify the API accepts a call and returns OK */
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    int rc = ccev_dns_resolve(loop, "example.com", 3000,
                               CCEV_DNS_A, on_resolved, loop);
    ASSERT(rc == CCEV_OK);

    /* Let the loop process one iteration (may not resolve in time) */
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_domain) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    int rc = ccev_dns_resolve(loop, NULL, 100, CCEV_DNS_A, on_resolved, loop);
    ASSERT(rc == CCEV_ERR);

    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_cb) {
    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    int rc = ccev_dns_resolve(loop, "example.com", 100, CCEV_DNS_A, NULL, NULL);
    ASSERT(rc == CCEV_ERR);

    ccev_loop_destroy(loop);
}

TEST(dns_resolve_null_loop) {
    int rc = ccev_dns_resolve(NULL, "example.com", 100, CCEV_DNS_A, on_resolved, NULL);
    ASSERT(rc == CCEV_ERR);
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
    printf("\n  %d passed, %d failed\n", passed, failed); fflush(stdout);
    return failed ? 1 : 0;
}
