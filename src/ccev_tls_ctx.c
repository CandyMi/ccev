/**
 * @file ccev_tls_ctx.c
 * @brief SSL_CTX wrapper — context creation, configuration, and destruction.
 *
 * @author CandyMi
 * @license MIT
 *
 * Handles OpenSSL initialisation (single-shot, idempotent), allocator
 * synchronisation (CRYPTO_set_mem_functions), and all ccev_tls_ctx_*
 * public API functions.
 *
 * OpenSSL references: Each SSL_new(ctx) increments the SSL_CTX refcount.
 * ccev_tls_ctx_free() decrements it but the CTX stays alive until all
 * SSL instances using it are freed.  This is transparent to the user.
 */
#define CCEV_HAVE_TLS 1
#include "ccev_tls_internal.h"

/* ════════════════════════════════════════════════════════════════
 *  OpenSSL initialisation (single-shot, idempotent)
 * ════════════════════════════════════════════════════════════════ */

/* Guard: 0 = uninitialized, 1 = initialized.  Written via CAS on the
 * first thread; read non-atomically on the fast path because stale
 * (seeing 0 when already 1) is harmless — the CAS serialises. */
static int ccev_tls__initialized = 0;

/* ── Allocator wrappers for CRYPTO_set_mem_functions ──
 * OpenSSL 3.x added const char *file, int line params.  We provide
 * both signatures via OPENSSL_VERSION_NUMBER, avoiding UB casts. */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static void *_tls_ossl_malloc(size_t sz, const char *file, int line) {
    (void)file; (void)line;
    return ccev__realloc_fn(NULL, sz);
}
static void *_tls_ossl_realloc(void *p, size_t sz, const char *file, int line) {
    (void)file; (void)line;
    return ccev__realloc_fn(p, sz);
}
static void _tls_ossl_free(void *p, const char *file, int line) {
    (void)file; (void)line;
    ccev__free_fn(p);
}
#else
static void *_tls_ossl_malloc(size_t sz) {
    return ccev__realloc_fn(NULL, sz);
}
static void *_tls_ossl_realloc(void *p, size_t sz) {
    return ccev__realloc_fn(p, sz);
}
static void _tls_ossl_free(void *p) {
    ccev__free_fn(p);
}
#endif

static void _tls_do_init(void) {
    /* CRYPTO_set_mem_functions MUST be called before any OpenSSL
     * initialisation (OpenSSL docs).  This is the only place where
     * allocator syncing happens — no thread races here because the
     * CAS gate (ccev__tls_init) ensures only one thread enters. */
    if (ccev__realloc_fn && ccev__free_fn) {
        CRYPTO_set_mem_functions(
            _tls_ossl_malloc,
            _tls_ossl_realloc,
            _tls_ossl_free
        );
    }

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#else
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif
}

void ccev__tls_init(void) {
    /* Fast path: single non-atomic read — stale 0 only enters the
     * CAS slow path, which serialises correctly. */
    if (ccev_tls__initialized) return;

    /* CAS serialises: only the first thread sets 0→1 and runs init. */
#if defined(_MSC_VER)
    if (_InterlockedCompareExchange((long volatile *)&ccev_tls__initialized, 1, 0) != 0)
        return;
#else
    if (!__sync_bool_compare_and_swap(&ccev_tls__initialized, 0, 1))
        return;
#endif

    _tls_do_init();
}

/* ════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ════════════════════════════════════════════════════════════════ */

/* Create a base SSL_CTX with common settings for both client and server. */
static SSL_CTX *_ctx_create_base(void) {
    ccev__tls_init();

    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return NULL;

    /* Auto-select the highest mutually-supported version. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                              SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
    /* Enable modern safety features. */
    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE |
                              SSL_OP_SINGLE_ECDH_USE |
                              SSL_OP_NO_COMPRESSION);

    return ctx;
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

int ccev_tls_ctx_use_certificate(ccev_tls_ctx_t *ctx,
                                  const char *cert_file,
                                  const char *key_file) {
    if (!ctx || !ctx->ssl_ctx) return CCEV_TLS_ERR_SYS;
    if (!cert_file || !key_file) return CCEV_TLS_ERR_SYS;

    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_file,
                                      SSL_FILETYPE_PEM) <= 0)
        return CCEV_TLS_ERR_SYS;

    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_file,
                                     SSL_FILETYPE_PEM) <= 0)
        return CCEV_TLS_ERR_SYS;

    if (!SSL_CTX_check_private_key(ctx->ssl_ctx))
        return CCEV_TLS_ERR_SYS;

    return CCEV_TLS_OK;
}

ccev_tls_ctx_t *ccev_tls_ctx_server(const char *cert_file,
                                     const char *key_file) {
    if (!cert_file || !key_file) return NULL;

    SSL_CTX *ssl_ctx = _ctx_create_base();
    if (!ssl_ctx) return NULL;

    ccev_tls_ctx_t *tls_ctx = (ccev_tls_ctx_t *)ccev__realloc_fn(
        NULL, sizeof(ccev_tls_ctx_t));
    if (!tls_ctx) {
        SSL_CTX_free(ssl_ctx);
        return NULL;
    }
    tls_ctx->ssl_ctx = ssl_ctx;

    if (ccev_tls_ctx_use_certificate(tls_ctx, cert_file, key_file)
        != CCEV_TLS_OK) {
        ccev_tls_ctx_free(tls_ctx);
        return NULL;
    }

    return tls_ctx;
}

ccev_tls_ctx_t *ccev_tls_ctx_client(void) {
    SSL_CTX *ctx = _ctx_create_base();
    if (!ctx) return NULL;

    /* Load system default CA certificates.
     *
     * On POSIX systems this reads the system trust store
     * (/etc/ssl/certs/ etc.).  On Windows there is no standard
     * system CA path — SSL_CTX_set_default_verify_paths will
     * fail, which is expected.  In either case, failure means
     * we return NULL: an empty trust anchor is a security hole
     * (every certificate would pass verification silently).
     *
     * Windows users MUST call ccev_tls_ctx_set_ca_file()
     * with a CA bundle before using the context. */
    if (!SSL_CTX_set_default_verify_paths(ctx)) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* Enable peer certificate verification by default.
     * Without this, all connections accept any certificate. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    ccev_tls_ctx_t *tls_ctx = (ccev_tls_ctx_t *)ccev__realloc_fn(
        NULL, sizeof(ccev_tls_ctx_t));
    if (!tls_ctx) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    tls_ctx->ssl_ctx = ctx;
    return tls_ctx;
}

int ccev_tls_ctx_set_ca_file(ccev_tls_ctx_t *ctx,
                              const char *ca_file,
                              bool replace) {
    if (!ctx || !ctx->ssl_ctx) return CCEV_TLS_ERR_SYS;
    if (!ca_file) return CCEV_TLS_OK;

    if (replace) {
        /* Clear existing CA store and load only the specified file. */
        X509_STORE *store = X509_STORE_new();
        if (!store) return CCEV_TLS_ERR_SYS;
        SSL_CTX_set_cert_store(ctx->ssl_ctx, store);
    }

    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) <= 0)
        return CCEV_TLS_ERR_SYS;

    return CCEV_TLS_OK;
}

int ccev_tls_ctx_set_verify(ccev_tls_ctx_t *ctx, ccev_tls_verify_t mode) {
    if (!ctx || !ctx->ssl_ctx) return CCEV_TLS_ERR_SYS;

    if (mode == CCEV_TLS_VERIFY_NONE) {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    }
    return CCEV_TLS_OK;
}

int ccev_tls_ctx_set_alpn(ccev_tls_ctx_t *ctx, const char *protos) {
    if (!ctx || !ctx->ssl_ctx) return CCEV_TLS_ERR_SYS;
    if (!protos) return CCEV_TLS_OK;

    /* Convert comma-separated ALPN list to wire format.
     * Input: "h2,http/1.1" → wire: "\x02h2\x08http/1.1" */
    size_t protos_len = strlen(protos);
    size_t wire_len = protos_len + 1; /* worst-case: all single-char protocols */
    unsigned char *wire = (unsigned char *)ccev__realloc_fn(NULL, wire_len + 2);
    if (!wire) return CCEV_TLS_ERR_SYS;

    size_t out = 0;
    const char *p = protos;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t plen = comma ? (size_t)(comma - p) : strlen(p);
        if (plen > 255) { ccev__free_fn(wire); return CCEV_TLS_ERR_SYS; }
        wire[out++] = (unsigned char)plen;
        memcpy(wire + out, p, plen);
        out += plen;
        p = comma ? comma + 1 : p + plen;
    }

    int ret = SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)out);
    ccev__free_fn(wire);
    return (ret == 0) ? CCEV_TLS_OK : CCEV_TLS_ERR_SYS;
}

int ccev_tls_ctx_set_ciphers(ccev_tls_ctx_t *ctx, const char *cipher_list) {
    if (!ctx || !ctx->ssl_ctx) return CCEV_TLS_ERR_SYS;

    if (cipher_list) {
        /* TLS 1.2 (and below) cipher configuration. */
        if (!SSL_CTX_set_cipher_list(ctx->ssl_ctx, cipher_list))
            return CCEV_TLS_ERR_SYS;

        /* TLS 1.3 uses a separate cipher suite API.  Same string
         * format (e.g. "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384").
         * OpenSSL accepts an empty/unknown cipher list gracefully —
         * it keeps the default.  Safe to always call. */
        if (!SSL_CTX_set_ciphersuites(ctx->ssl_ctx, cipher_list))
            return CCEV_TLS_ERR_SYS;
    }
    return CCEV_TLS_OK;
}

void ccev_tls_ctx_free(ccev_tls_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->ssl_ctx)
        SSL_CTX_free(ctx->ssl_ctx);
    ccev__free_fn(ctx);
}
