/**
 * @file ccev_tls.h
 * @brief TLS support for ccev — OpenSSL Memory BIO integration.
 *
 * @author CandyMi
 * @license MIT
 *
 * ccev_tls wraps OpenSSL via Memory BIOs, embedding ccev_stream_t to
 * reuse its write buffering and write-completion callbacks.
 *
 * Architecture:
 *   ccev_tls_t → ccev_stream_t → ccev_sock_t
 *   (encryption)  (buffered I/O)  (reactor primitive)
 *
 * ccev_tls_t is an independently allocated structure (not part of the
 * ccev_sock_any_t union).  ccev_tls_open() upgrades a ccev_sock_t to
 * TLS by allocating a new ccev_tls_t and taking over the sock's event
 * callbacks.
 *
 * Usage:
 *   ccev_tls_ctx_t *ctx = ccev_tls_ctx_client();
 *   ccev_tls_t *tls = ccev_tls_wrap_stream(sock, ctx, CCEV_TLS_CLIENT,
 *                                            "example.com", 5000,
 *                                            handshake_cb, my_data);
 *   // handshake_cb fires with CCEV_TLS_OK when ready
 *   ccev_tls_write(tls, data, len, NULL, NULL);
 *
 * Requires OpenSSL 1.1+ (recommended) with compatibility for 1.0.2.
 * Include this header AFTER ccev.h (or include ccev.h first).
 */

#ifndef CCEV_TLS_H
#define CCEV_TLS_H

#include "ccev.h"

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint64_t */
#include <stdbool.h>  /* bool */

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════
 *  Opaque handle types
 * ════════════════════════════════════════════════════════════════ */

/** Opaque TLS context (wraps SSL_CTX). Created by ccev_tls_ctx_*(). */
typedef struct ccev_tls_ctx_s ccev_tls_ctx_t;

/** TLS connection handle. Created by ccev_tls_open() / ccev_tls_wrap_stream(). */
typedef struct ccev_tls_s     ccev_tls_t;

/* ════════════════════════════════════════════════════════════════
 *  Enumerations
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    CCEV_TLS_CLIENT = 0,  /**< Client-side TLS (connects to server). */
    CCEV_TLS_SERVER,       /**< Server-side TLS (accepts connections). */
} ccev_tls_mode_t;

typedef enum {
    CCEV_TLS_VERIFY_NONE = 0, /**< Do not verify peer certificate. */
    CCEV_TLS_VERIFY_PEER,      /**< Verify peer certificate (default). */
} ccev_tls_verify_t;

/* ════════════════════════════════════════════════════════════════
 *  TLS error codes
 * ════════════════════════════════════════════════════════════════ */

#define CCEV_TLS_OK         0    /**< TLS handshake succeeded.              */
#define CCEV_TLS_ERR_IO    -1    /**< Network error / timeout / closed.     */
#define CCEV_TLS_ERR_PROTO -2    /**< Protocol error (version mismatch).    */
#define CCEV_TLS_ERR_CERT  -3    /**< Certificate verification failed.      */
#define CCEV_TLS_ERR_SYS   -4    /**< Resource error (OOM, invalid CTX).    */

/* ════════════════════════════════════════════════════════════════
 *  Callback types
 * ════════════════════════════════════════════════════════════════ */

/** @brief TLS handshake completion callback.
 *  @param udata  User-provided context pointer.
 *  @param tls    The TLS connection handle.
 *  @param status CCEV_TLS_OK on success, or CCEV_TLS_ERR_* on failure.
 *                On failure the socket is already scheduled for close. */
typedef void (*ccev_tls_handshake_cb)(void *udata,
                                       struct ccev_tls_s *tls,
                                       int status);

/* ════════════════════════════════════════════════════════════════
 *  TLS context management (ccev_tls_ctx_t)
 * ════════════════════════════════════════════════════════════════ */

/** @brief Create a server TLS context.
 *
 *  Loads the specified certificate and private key files.
 *  The context is configured for CCEV_TLS_SERVER mode.
 *
 *  @note Peer certificate verification is DISABLED by default
 *  (CCEV_TLS_VERIFY_NONE).  Most servers do not require client
 *  certificates, so this is the safe default.
 *
 *  @section mtls   Mutual TLS (mTLS) setup
 *
 *  To require and verify client certificates on the server side:
 *
 *    ccev_tls_ctx_t *ctx = ccev_tls_ctx_server("cert.pem", "key.pem");
 *
 *    // Step 1 — require a client certificate
 *    ccev_tls_ctx_set_verify(ctx, CCEV_TLS_VERIFY_PEER);
 *
 *    // Step 2 — specify which CAs to trust for client certs.
 *    // replace=true means ONLY this file (ignore any default CAs).
 *    ccev_tls_ctx_set_ca_file(ctx, "client-ca.pem", true);
 *
 *  @param cert_file  Path to the PEM certificate file.
 *  @param key_file   Path to the PEM private key file.
 *  @return TLS context, or NULL on failure. */
ccev_tls_ctx_t *ccev_tls_ctx_server(const char *cert_file,
                                     const char *key_file);

/** @brief Create a client TLS context.
 *
 *  Loads the system default CA certificates.
 *  The context is configured for CCEV_TLS_CLIENT mode.
 *
 *  Peer certificate verification is enabled by default
 *  (CCEV_TLS_VERIFY_PEER).  Call ccev_tls_ctx_set_verify() with
 *  CCEV_TLS_VERIFY_NONE to disable.
 *
 *  @note                        Windows users
 *  Windows has no standard system CA path.  This function
 *  returns NULL on Windows because the system trust store
 *  cannot be loaded.  You MUST call ccev_tls_ctx_set_ca_file()
 *  with a CA bundle (e.g. a curl-style ca-bundle.crt) before
 *  using the context for connections.
 *
 *  @note                        POSIX systems
 *  Return NULL if the system CA path (/etc/ssl/certs/ etc.)
 *  is missing or broken — resolve the system certificate
 *  configuration and retry.
 *
 *  @return TLS context, or NULL on failure
 *          (system CA store unavailable or OOM). */
ccev_tls_ctx_t *ccev_tls_ctx_client(void);

/** @brief Set CA file(s) for certificate verification.
 *
 *  @param ctx     TLS context.
 *  @param ca_file Path to CA PEM file (or NULL to clear).
 *  @param replace If true, use ONLY the specified file (ignore system CA).
 *                 If false, append to the system default CA store.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_ctx_set_ca_file(ccev_tls_ctx_t *ctx,
                              const char *ca_file,
                              bool replace);

/** @brief Set the peer verification mode.
 *  @param ctx  TLS context.
 *  @param mode CCEV_TLS_VERIFY_NONE or CCEV_TLS_VERIFY_PEER.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_ctx_set_verify(ccev_tls_ctx_t *ctx, ccev_tls_verify_t mode);

/** @brief Set ALPN protocol list (e.g. "h2,http/1.1").
 *  @param ctx    TLS context.
 *  @param protos Comma-separated protocol list, or NULL to clear.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_ctx_set_alpn(ccev_tls_ctx_t *ctx, const char *protos);

/** @brief Set the cipher list.
 *  @param ctx         TLS context.
 *  @param cipher_list OpenSSL cipher string, or NULL for defaults.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_ctx_set_ciphers(ccev_tls_ctx_t *ctx, const char *cipher_list);

/** @brief Load a certificate and private key onto an existing context.
 *
 *  Can be called on both client and server contexts.  The private key
 *  is verified against the certificate.  Useful for client certificate
 *  authentication (mutual TLS).
 *
 *  @param ctx       TLS context.
 *  @param cert_file Path to the PEM certificate file.
 *  @param key_file  Path to the PEM private key file.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_ctx_use_certificate(ccev_tls_ctx_t *ctx,
                                  const char *cert_file,
                                  const char *key_file);

/** @brief Free a TLS context.
 *
 *  Safe to call while TLS connections using this context are still alive
 *  (OpenSSL uses reference counting internally).
 *
 *  @warning After this call the @p ctx pointer becomes invalid and MUST
 *  NOT be passed to any other ccev_tls_ctx_*() function or to
 *  ccev_tls_open().  The context is freed immediately; OpenSSL's
 *  reference counting only protects SSL objects already created from
 *  it.
 *
 *  @param ctx TLS context (NULL-safe). */
void ccev_tls_ctx_free(ccev_tls_ctx_t *ctx);

/* ════════════════════════════════════════════════════════════════
 *  TLS connection — three-step lifecycle
 * ════════════════════════════════════════════════════════════════ */

/** @brief Step 1: Create a TLS connection object.
 *
 *  Allocates a new ccev_tls_t, initialises SSL/BIO, and takes over
 *  the sock's event callbacks (rcb → TLS read handler).
 *  Does NOT start the handshake — call ccev_tls_handshake() next.
 *
 *  @param sock  A live socket (from ccev_connect/CONNECT or ccev_listen/ACCEPT).
 *               The sock must be in INIT mode (connected, not listening).
 *  @param ctx   TLS context (may be NULL — set via ccev_tls_set_* before handshake).
 *  @param mode  CCEV_TLS_CLIENT or CCEV_TLS_SERVER.
 *  @return TLS handle, or NULL on failure (OOM / invalid sock). */
ccev_tls_t *ccev_tls_open(ccev_sock_t *sock,
                            ccev_tls_ctx_t *ctx,
                            ccev_tls_mode_t mode);

/** @brief Step 2 (optional): Set SNI servername (must be before handshake).
 *  @param tls      TLS handle.
 *  @param hostname Server name for SNI, or NULL to clear.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_set_servername(ccev_tls_t *tls, const char *hostname);

/** @brief Step 2 (optional): Set ALPN protocols (must be before handshake).
 *  @param tls    TLS handle.
 *  @param protos Comma-separated protocol list, or NULL to clear.
 *  @return CCEV_TLS_OK or CCEV_TLS_ERR_SYS. */
int ccev_tls_set_alpn(ccev_tls_t *tls, const char *protos);

/** @brief Step 3: Start TLS handshake.
 *
 *  Takes over the socket's read callback to drive the handshake.
 *  The completion callback fires asynchronously when the handshake
 *  finishes or fails.
 *
 *  @param tls         TLS handle.
 *  @param timeout_ms  Handshake timeout in ms (0 = no timeout).
 *  @param cb          Completion callback.
 *  @param udata       User pointer for @p cb.
 *  @return CCEV_TLS_OK (sync complete, rare),
 *          CCEV_ERR (immediate failure, OOM),
 *          or 1 (async — handshake in progress, callback will fire). */
int ccev_tls_handshake(ccev_tls_t *tls,
                        int timeout_ms,
                        ccev_tls_handshake_cb cb,
                        void *udata);

/** @brief Convenience: open + set_servername + handshake (one call).
 *
 *  Covers ~80% of TLS usage.  Use the three-step API for custom
 *  ALPN / cipher configuration.
 *
 *  @param sock        A live socket.
 *  @param ctx         TLS context.
 *  @param mode        CCEV_TLS_CLIENT or CCEV_TLS_SERVER.
 *  @param servername  SNI hostname (NULL = no SNI).
 *  @param timeout_ms  Handshake timeout in ms.
 *  @param cb          Handshake completion callback.
 *  @param udata       User pointer for @p cb.
 *  @return TLS handle, or NULL on failure. */
ccev_tls_t *ccev_tls_wrap_stream(ccev_sock_t *sock,
                                   ccev_tls_ctx_t *ctx,
                                   ccev_tls_mode_t mode,
                                   const char *servername,
                                   int timeout_ms,
                                   ccev_tls_handshake_cb cb,
                                   void *udata);

/* ════════════════════════════════════════════════════════════════
 *  TLS I/O operations
 * ════════════════════════════════════════════════════════════════ */

/** @brief Write data via TLS (encrypt + send).
 *
 *  Data is encrypted via SSL_write, then the ciphertext is written
 *  to the internal stream write buffer.  The per-buffer callback
 *  fires when the ciphertext has been flushed to the kernel.
 *
 *  @param tls   TLS handle.
 *  @param data  Plaintext data to write.
 *  @param len   Data length.
 *  @param cb    Per-buffer write-complete callback, or NULL.
 *  @param udata User pointer for @p cb.
 *  @return Number of plaintext bytes accepted, or CCEV_ERR on failure. */
int ccev_tls_write(ccev_tls_t *tls, const void *data, size_t len,
                    ccev_send_cb cb, void *udata);

/** @brief Batch write with explicit flush control.
 *
 *  @param tls   TLS handle.
 *  @param data  Data to write (NULL when done=true for flush-only).
 *  @param len   Data length.
 *  @param done  false = buffer only; true = buffer then trigger send.
 *  @param cb    Per-buffer write-complete callback, or NULL.
 *  @param udata User pointer for @p cb.
 *  @return Bytes accepted, or CCEV_ERR. */
int ccev_tls_write_batch(ccev_tls_t *tls, const void *data, size_t len,
                          bool done, ccev_send_cb cb, void *udata);

/** @brief Flush the TLS write buffer.
 *  @param tls TLS handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_tls_flush(ccev_tls_t *tls);

/* ── Read operations ── */

/** @brief Read continuously — dispatch decrypted data as it arrives.
 *
 *  Unlike readline/readnum (one-shot), this keeps the reader active
 *  after each dispatch until ccev_tls_read_stop() or error.
 *
 *  @param tls        TLS handle.
 *  @param cb         Dispatch callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_tls_read(ccev_tls_t *tls,
                   ccev_stream_cb cb, void *udata);

/** @brief Read until delimiter (delimiter inclusive).
 *  @param tls        TLS handle.
 *  @param delim      Delimiter byte.
 *  @param maxlen     Maximum bytes before CCEV_ERR.
 *  @param timeout_ms Read timeout in ms (0 = no timeout).
 *  @param cb         Completion callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_tls_readline(ccev_tls_t *tls, char delim, size_t maxlen,
                       int timeout_ms, ccev_stream_cb cb, void *udata);

/** @brief Read exactly N bytes.
 *  @param tls        TLS handle.
 *  @param n          Number of bytes (> 0).
 *  @param timeout_ms Read timeout in ms (0 = no timeout).
 *  @param cb         Completion callback.
 *  @param udata      User pointer for @p cb.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_tls_readnum(ccev_tls_t *tls, size_t n,
                      int timeout_ms, ccev_stream_cb cb, void *udata);

/* ════════════════════════════════════════════════════════════════
 *  TLS lifecycle
 * ════════════════════════════════════════════════════════════════ */

/** @brief Close a TLS connection.
 *
 *  Performs a two-phase shutdown:
 *    1) SSL_shutdown → sends close_notify
 *    2) Waits for peer close_notify (or timeout / EPIPE)
 *    3) Frees SSL, releases reader buffer, closes the stream
 *
 *  @param tls TLS handle.
 *  @return CCEV_OK or CCEV_ERR. */
int ccev_tls_close(ccev_tls_t *tls);

/** @brief Set the global write-drain callback.
 *  Fires when the entire write buffer has been flushed to the kernel.
 *  @param tls   TLS handle.
 *  @param cb    Callback (NULL to clear).
 *  @param udata User pointer. */
void ccev_tls_set_send_cb(ccev_tls_t *tls, ccev_send_cb cb, void *udata);

/** @brief Set the close callback.
 *  @param tls   TLS handle.
 *  @param cb    Close callback.
 *  @param udata User pointer. */
void ccev_tls_set_close_cb(ccev_tls_t *tls, ccev_close_cb cb, void *udata);

/** @brief Get the number of pending bytes in the write buffer.
 *  @param tls TLS handle.
 *  @return Pending write bytes. */
size_t ccev_tls_wbuf_len(const ccev_tls_t *tls);

#ifdef __cplusplus
}
#endif

#endif /* CCEV_TLS_H */
