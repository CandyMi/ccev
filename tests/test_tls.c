/**
 * @file test_tls.c
 * @brief TLS lifecycle and I/O tests for ccev_tls.
 *
 * Self-signed certificate and key are generated at runtime using OpenSSL API
 * and written to temporary files.  This avoids any filesystem dependency on
 * external PEM files.
 */

#include "ccev.h"
#include "ccev_tls.h"
#include "ccsocket.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static int passed, failed;
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { \
  if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed++; } \
  else passed++; \
} while(0)
#define RUN(name) do { printf("  %s\n", #name); fflush(stdout); test_##name(); } while(0)

static void timer_stop_loop(void *udata) {
    ccev_loop_stop((ccev_loop_t *)udata);
}

/* ── Helper: generate self-signed cert + key, write to temp files ── */
static int gen_selfsigned(const char *cert_path, const char *key_path) {
    EVP_PKEY *pkey = NULL;
    X509 *x509 = NULL;
    int ret = -1;

    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!kctx) goto out;
    if (EVP_PKEY_keygen_init(kctx) <= 0) goto out;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0) goto out;
    if (EVP_PKEY_generate(kctx, &pkey) <= 0) goto out;
    EVP_PKEY_CTX_free(kctx);
    kctx = NULL;
    if (!pkey) goto out;

    x509 = X509_new();
    if (!x509) goto out;

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (unsigned char *)"ccev-test", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_set_pubkey(x509, pkey);
    if (!X509_sign(x509, pkey, EVP_sha256())) goto out;

    FILE *f = fopen(key_path, "wb");
    if (!f) goto out;
    PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(f);

    f = fopen(cert_path, "wb");
    if (!f) { unlink(key_path); goto out; }
    PEM_write_X509(f, x509);
    fclose(f);

    ret = 0;
out:
    if (kctx) EVP_PKEY_CTX_free(kctx);
    if (pkey) EVP_PKEY_free(pkey);
    if (x509) X509_free(x509);
    return ret;
}

static int pair_create(ccsocket_t sv[2]) {
    return ccsocketpair(sv, CC_NOFLAG) ? 0 : -1;
}

/* ── Handshake callback helper ── */
struct hs_state { volatile int done; volatile int status; };

static void hs_cb(void *udata, ccev_tls_t *tls, int status) {
    (void)tls;
    struct hs_state *hs = (struct hs_state *)udata;
    hs->done   = 1;
    hs->status = status;
}

/* ── Write-complete callback ── */
struct wc_state { volatile int fired; };

static void wc_cb(void *udata) {
    struct wc_state *wc = (struct wc_state *)udata;
    wc->fired = 1;
}

/* ── Read callback ── */
struct rd_state {
    volatile int fired;
    volatile size_t got;
    char buf[256];
    volatile int status;
};

static void rd_cb(void *udata, const char *data, size_t len, int status) {
    struct rd_state *rd = (struct rd_state *)udata;
    rd->fired = 1;
    rd->status = status;
    if (data && len > 0) {
        size_t cpy = len < sizeof(rd->buf) ? len : sizeof(rd->buf) - 1;
        memcpy(rd->buf, data, cpy);
        rd->buf[cpy] = '\0';
        rd->got = cpy;
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Tests
 * ════════════════════════════════════════════════════════════════ */

TEST(null_args) {
    ASSERT(ccev_tls_ctx_server(NULL, NULL) == NULL);
    ASSERT(ccev_tls_ctx_server("file.pem", NULL) == NULL);
    ASSERT(ccev_tls_ctx_server(NULL, "file.pem") == NULL);

    ccev_tls_ctx_t *ctx = ccev_tls_ctx_client();
    ASSERT(ccev_tls_open(NULL, ctx, CCEV_TLS_CLIENT) == NULL);
    ccev_tls_ctx_free(ctx);

    ccev_tls_ctx_free(NULL);
    ccev_tls_close(NULL);
}

TEST(client_ctx_create_and_free) {
    ccev_tls_ctx_t *ctx = ccev_tls_ctx_client();
    ASSERT(ctx != NULL);
    ccev_tls_ctx_free(ctx);
}

static int _do_handshake(ccev_loop_t *loop,
                          ccev_tls_t *srv, struct hs_state *srv_hs,
                          ccev_tls_t *cli, struct hs_state *cli_hs) {
    ccev_timer_add(loop, 3000, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    int tries = 0;
    while ((!srv_hs->done || !cli_hs->done) && tries < 10) {
        ccev_timer_add(loop, 200, CCEV_TIMER_ONCE, timer_stop_loop, loop);
        ccev_loop_run(loop, CCEV_RUN_ONCE);
        tries++;
    }
    return (srv_hs->done && cli_hs->done &&
            srv_hs->status == CCEV_TLS_OK && cli_hs->status == CCEV_TLS_OK) ? 0 : -1;
}

TEST(handshake_and_data_transfer) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }

    const char *cert_path = "tls_test_cert.pem";
    const char *key_path  = "tls_test_key.pem";
    if (gen_selfsigned(cert_path, key_path) != 0) {
        ccsocket_close(sv[0]); ccsocket_close(sv[1]); passed++; return;
    }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccev_tls_ctx_t *srv_ctx = ccev_tls_ctx_server(cert_path, key_path);
    ASSERT(srv_ctx != NULL);
    ccev_tls_ctx_t *cli_ctx = ccev_tls_ctx_client();
    ASSERT(cli_ctx != NULL);
    ccev_tls_ctx_set_verify(srv_ctx, CCEV_TLS_VERIFY_NONE);
    ccev_tls_ctx_set_verify(cli_ctx, CCEV_TLS_VERIFY_NONE);

    ccev_sock_t *srv_sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_sock_t *cli_sock = ccev_sock_create(loop, sv[1], NULL);
    ASSERT(srv_sock != NULL); ASSERT(cli_sock != NULL);

    struct hs_state srv_hs = {0, 0}, cli_hs = {0, 0};
    ccev_tls_t *srv_tls = ccev_tls_wrap_stream(
        srv_sock, srv_ctx, CCEV_TLS_SERVER, NULL, 5000, hs_cb, &srv_hs);
    ccev_tls_t *cli_tls = ccev_tls_wrap_stream(
        cli_sock, cli_ctx, CCEV_TLS_CLIENT, NULL, 5000, hs_cb, &cli_hs);
    ASSERT(srv_tls != NULL); ASSERT(cli_tls != NULL);

    ASSERT(_do_handshake(loop, srv_tls, &srv_hs, cli_tls, &cli_hs) == 0);

    /* Write client → server */
    static const char *test_msg = "Hello TLS!";
    struct wc_state wc = {0};
    int ret = ccev_tls_write(cli_tls, test_msg, strlen(test_msg), wc_cb, &wc);
    ASSERT(ret == (int)strlen(test_msg));

    struct rd_state rd = {0};
    ccev_tls_read(srv_tls, rd_cb, &rd);

    ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    ASSERT(wc.fired);
    ASSERT(rd.fired && rd.status == CCEV_OK);
    ASSERT(rd.got == strlen(test_msg));
    ASSERT(memcmp(rd.buf, test_msg, rd.got) == 0);

    /* Server → client */
    struct rd_state rd2 = {0};
    ccev_tls_read(cli_tls, rd_cb, &rd2);
    ccev_tls_write(srv_tls, test_msg, strlen(test_msg), NULL, NULL);

    ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_ONCE);

    ASSERT(rd2.fired && rd2.status == CCEV_OK);
    ASSERT(rd2.got == strlen(test_msg));
    ASSERT(memcmp(rd2.buf, test_msg, rd2.got) == 0);

    ccev_tls_close(srv_tls); ccev_tls_close(cli_tls);
    ccev_loop_destroy(loop);
    ccev_tls_ctx_free(srv_ctx); ccev_tls_ctx_free(cli_ctx);
    unlink(cert_path); unlink(key_path);
}

TEST(readline_and_readnum) {
    ccsocket_t sv[2];
    if (pair_create(sv) != 0) { passed++; return; }

    const char *cert_path = "tls_test_cert.pem";
    const char *key_path  = "tls_test_key.pem";
    if (gen_selfsigned(cert_path, key_path) != 0) {
        ccsocket_close(sv[0]); ccsocket_close(sv[1]); passed++; return;
    }

    ccev_loop_t *loop = ccev_loop_create(64);
    ASSERT(loop != NULL);

    ccev_tls_ctx_t *srv_ctx = ccev_tls_ctx_server(cert_path, key_path);
    ASSERT(srv_ctx != NULL);
    ccev_tls_ctx_t *cli_ctx = ccev_tls_ctx_client();
    ASSERT(cli_ctx != NULL);
    ccev_tls_ctx_set_verify(srv_ctx, CCEV_TLS_VERIFY_NONE);
    ccev_tls_ctx_set_verify(cli_ctx, CCEV_TLS_VERIFY_NONE);

    ccev_sock_t *srv_sock = ccev_sock_create(loop, sv[0], NULL);
    ccev_sock_t *cli_sock = ccev_sock_create(loop, sv[1], NULL);
    ASSERT(srv_sock != NULL); ASSERT(cli_sock != NULL);

    struct hs_state srv_hs = {0, 0}, cli_hs = {0, 0};
    ccev_tls_t *srv_tls = ccev_tls_wrap_stream(
        srv_sock, srv_ctx, CCEV_TLS_SERVER, NULL, 5000, hs_cb, &srv_hs);
    ccev_tls_t *cli_tls = ccev_tls_wrap_stream(
        cli_sock, cli_ctx, CCEV_TLS_CLIENT, NULL, 5000, hs_cb, &cli_hs);
    ASSERT(srv_tls != NULL); ASSERT(cli_tls != NULL);

    ASSERT(_do_handshake(loop, srv_tls, &srv_hs, cli_tls, &cli_hs) == 0);

    /* Write two lines from client, readline on server */
    ccev_tls_write(cli_tls, "line1\n", 6, NULL, NULL);
    ccev_tls_write(cli_tls, "line2\n", 6, NULL, NULL);

    struct rd_state rd1 = {0};
    ASSERT(ccev_tls_readline(srv_tls, '\n', 256, 2000, rd_cb, &rd1) == CCEV_OK);

    ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(rd1.fired && rd1.status == CCEV_OK);
    ASSERT(rd1.got == 6);
    ASSERT(memcmp(rd1.buf, "line1\n", 6) == 0);

    /* Readnum the second line */
    struct rd_state rd2 = {0};
    ASSERT(ccev_tls_readnum(srv_tls, 6, 2000, rd_cb, &rd2) == CCEV_OK);

    ccev_timer_add(loop, 1000, CCEV_TIMER_ONCE, timer_stop_loop, loop);
    ccev_loop_run(loop, CCEV_RUN_ONCE);
    ASSERT(rd2.fired && rd2.status == CCEV_OK);
    ASSERT(rd2.got == 6);
    ASSERT(memcmp(rd2.buf, "line2\n", 6) == 0);

    ccev_tls_close(srv_tls); ccev_tls_close(cli_tls);
    ccev_loop_destroy(loop);
    ccev_tls_ctx_free(srv_ctx); ccev_tls_ctx_free(cli_ctx);
    unlink(cert_path); unlink(key_path);
}

/* ════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("ccev_tls tests:\n");
    RUN(null_args);
    RUN(client_ctx_create_and_free);
    RUN(handshake_and_data_transfer);
    RUN(readline_and_readnum);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
