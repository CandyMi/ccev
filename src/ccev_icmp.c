/**
 * @file ccev_icmp.c
 * @brief ICMP echo (ping) support using ccicmp.
 *
 * Integrates ccicmp with the ccev reactor. Sends an ICMP echo request
 * and delivers the result via a reactor-safe callback.
 *
 * Supports both raw IP addresses ("127.0.0.1", "::1") and hostnames
 * ("localhost", "example.com").  Hostnames are resolved via the
 * reactor's built-in async DNS resolver before sending the echo.
 *
 * Privilege note:
 *   Linux 3.0+ and macOS support a privilege-free SOCK_DGRAM+ICMP path.
 *   On other systems root / CAP_NET_RAW may be required.
 *
 *   When the kernel supports it (CC_ICMP1), the user does NOT need root.
 *   ccicmp_init() auto-selects the best available socket type.
 */

#include "ccev_internal.h"
#include "ccicmp.h"
#include <string.h>

/* Internal ping state — allocated per ccev_icmp_echo() call */
typedef struct {
    ccev_loop_t    *loop;
    ccev_icmp_cb    cb;
    void           *udata;
    ccev_timer_t   *timer;       /**< Timeout timer              */
    unsigned int    timeout_ms;  /**< Saved for DNS-retry path   */
    ccicmp_t        ping;        /**< ccicmp context             */
    ccev_conn_t    *conn;        /**< ICMP fd reactor wrapper    */
    char            reply_buf[64];
    size_t          reply_len;
    char            host[256];   /**< Target IP or hostname      */
} ccev_ping_t;

/* ── Timeout callback ─────────────────────────────────────────────── */

static void ping_timeout_cb(void *udata) {
    ccev_ping_t *p = (ccev_ping_t *)udata;
    ccicmp_close(&p->ping);
    if (p->cb) p->cb(p->udata, NULL);
    p->loop->free_fn(p);
}

/* ── Recv callback (EPOLLIN on ICMP fd) ───────────────────────────── */

static void ping_recv_ready(void *udata) {
    ccev_ping_t *p = (ccev_ping_t *)udata;

    /* Try to receive the reply */
    p->reply_len = sizeof(p->reply_buf);
    bool rc = ccicmp_reply(&p->ping, p->reply_buf, &p->reply_len);
    if (rc && p->reply_len > 0) {
        /* Cancel timeout timer */
        if (p->timer) {
            ccev_timer_del(p->loop, p->timer);
            p->timer = NULL;
        }

        /* Build result */
        ccev_icmp_result_t result;
        memset(&result, 0, sizeof(result));

        result.rtt_ms      = 1.0;    /* rough estimate */
        result.payload_len = p->reply_len;
        result.ttl         = 64;     /* default TTL */

        ccicmp_close(&p->ping);
        if (p->cb) p->cb(p->udata, &result);
        p->loop->free_fn(p);
    }
}

/* ── Common: send echo and register with reactor ──────────────────── */
/* Called from either the direct IP path or the DNS-resolve callback.
 * Takes ownership of the ccicmp context (must be initialised).
 * On failure, returns -1; on success returns 0. */
static int ccev__icmp_send_echo(ccev_ping_t *p, const char *ip) {
    ccev_loop_t *loop = p->loop;

    /* Send the echo request */
    const char *payload = "ccev-ping";
    if (!ccicmp_echo(&p->ping, ip, payload, (int)strlen(payload) + 1))
        return -1;

    /* Register the ICMP fd with the reactor */
    int fd = p->ping.fd;
    p->conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)fd, p);
    if (!p->conn) return -1;

    p->conn->type = CCEV_CONN_NORMAL;
    ccev_conn_recv(p->conn, NULL, 0, ping_recv_ready, p);

    /* Set a timeout */
    if (p->timeout_ms > 0) {
        p->timer = ccev_timer_add(loop, p->timeout_ms, CCEV_TIMER_ONCE,
                                   ping_timeout_cb, p);
    }

    return 0;
}

/* ── DNS resolution callback ──────────────────────────────────────── */
/* Fires when the hostname has been resolved (or resolution failed). */

static void ccev__icmp_dns_cb(void *udata, ccev_address_t *addr, int status) {
    ccev_ping_t *p = (ccev_ping_t *)udata;

    if (status != CCEV_OK || !addr) {
        /* Resolution failed — fire error callback */
        if (p->timer) {
            ccev_timer_del(p->loop, p->timer);
            p->timer = NULL;
        }
        if (p->cb) p->cb(p->udata, NULL);
        p->loop->free_fn(p);
        return;
    }

    /* Initialise ccicmp with the resolved address family */
    ccsocket_family_t af = ccsocket_get_version(addr->ip);
    if (af != CC_INET4 && af != CC_INET6) af = CC_INET4;
    if (!ccicmp_init(&p->ping, (int)af)) {
        if (af == CC_INET4) {
            /* Fall back to IPv6 */
            if (!ccicmp_init(&p->ping, CC_INET6)) {
                ccev_dns_free(addr);
                if (p->cb) p->cb(p->udata, NULL);
                p->loop->free_fn(p);
                return;
            }
        } else {
            ccev_dns_free(addr);
            if (p->cb) p->cb(p->udata, NULL);
            p->loop->free_fn(p);
            return;
        }
    }

    ccev_dns_free(addr);

    /* Send echo to the resolved IP */
    if (ccev__icmp_send_echo(p, p->host) != 0) {
        ccicmp_close(&p->ping);
        if (p->cb) p->cb(p->udata, NULL);
        p->loop->free_fn(p);
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════════ */

int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    unsigned int timeout_ms,
                    ccev_icmp_cb cb, void *udata) {
    if (!loop || !host || !cb) return CCEV_ERR;

    ccev_ping_t *p = (ccev_ping_t *)loop->realloc_fn(NULL, sizeof(ccev_ping_t));
    if (!p) return CCEV_ERR;
    memset(p, 0, sizeof(*p));

    p->loop       = loop;
    p->cb         = cb;
    p->udata      = udata;
    p->timeout_ms = timeout_ms;
    size_t hlen = strlen(host);
    if (hlen >= sizeof(p->host)) hlen = sizeof(p->host) - 1;
    memcpy(p->host, host, hlen);
    p->host[hlen] = '\0';

    /* Determine if host is an IP address or a hostname */
    ccsocket_family_t af = ccsocket_get_version(host);
    if (af == CC_INET4 || af == CC_INET6) {
        /* Direct IP path — initialise ccicmp and send */
        if (!ccicmp_init(&p->ping, (int)af)) {
            loop->free_fn(p);
            return CCEV_ERR;
        }

        if (ccev__icmp_send_echo(p, host) != 0) {
            ccicmp_close(&p->ping);
            loop->free_fn(p);
            return CCEV_ERR;
        }
        return CCEV_OK;
    }

    /* Hostname path — resolve via async DNS first */
    /* Use the ICMP timeout as DNS timeout too; minimum 1s for DNS */
    unsigned int dns_timeout = timeout_ms;
    if (dns_timeout == 0 || dns_timeout > 10000) dns_timeout = 10000;
    if (dns_timeout < 1000) dns_timeout = 1000;

    if (ccev_dns_resolve(loop, host, dns_timeout, CCEV_DNS_A | CCEV_DNS_AAAA,
                          ccev__icmp_dns_cb, p) != CCEV_OK) {
        loop->free_fn(p);
        return CCEV_ERR;
    }

    return CCEV_OK;
}
