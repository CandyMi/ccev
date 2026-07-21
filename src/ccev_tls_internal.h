/**
 * @file ccev_tls_internal.h
 * @brief TLS internal header — OpenSSL includes and internal declarations.
 *
 * Private header for TLS source files.  The struct layout for ccev_tls_s
 * is defined in ccev_internal.h (inside the CCEV_HAVE_TLS guard) so it
 * participates in the ccev_sock_any_t union.
 */

#ifndef CCEV_TLS_INTERNAL_H
#define CCEV_TLS_INTERNAL_H

#include "ccev_internal.h"

#ifdef CCEV_HAVE_TLS
#  include <openssl/ssl.h>
#  include <openssl/bio.h>
#  include <openssl/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  TLS context — wraps SSL_CTX*
 * ════════════════════════════════════════════════════════════════ */

struct ccev_tls_ctx_s {
    SSL_CTX *ssl_ctx;
    bool     is_server;      /**< true = server (accept), false = client (connect). */
};

/* ════════════════════════════════════════════════════════════════
 *  Internal function declarations
 * ════════════════════════════════════════════════════════════════ */

/** Single-shot OpenSSL initialisation (idempotent, called from entry points). */
void ccev__tls_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_HAVE_TLS */
#endif /* CCEV_TLS_INTERNAL_H */
