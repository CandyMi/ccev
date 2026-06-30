/**
 * @file ccev_icmp.c
 * @brief ICMP echo (ping) support using ccicmp.
 *
 * @author CandyMi
 * @license MIT
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

/* Internal ping state — allocated per ccev_icmp_echo() call */
typedef struct {
    ccev_loop_t    *loop;
    ccev_icmp_cb    cb;
    void           *udata;
    ccev_timer_t   *timer;       /**< Timeout timer              */
    unsigned int    timeout_ms;  /**< Saved for DNS-retry path   */
    ccicmp_t        ping;        /**< ccicmp context             */
    ccev_sock_t    *sock;        /**< ICMP fd reactor wrapper    */
    uint64_t        send_time;   /**< Monotonic ms before send   */
    char            reply_buf[64];
    size_t          reply_len;
    char            host[256];   /**< Target IP or hostname      */
} ccev_ping_t;

/* ── Sock close callback — closes the ICMP fd and frees state ──── */

static void ping_close_cb(void *udata) {
    ccev_ping_t *p = (ccev_ping_t *)udata;
    ccicmp_close(&p->ping);
    if (p->sock) p->sock->fd = (ccsocket_t)-1;  /* prevent double-close */
    ccev__free_fn(p);
}

/* ── Timeout callback ─────────────────────────────────────────────── */

static void ping_timeout_cb(void *udata) {
    ccev_ping_t *p = (ccev_ping_t *)udata;

    if (p->cb) p->cb(p->udata, NULL);

    if (p->sock) {
        ccev__sock_schedule_close(p->loop, p->sock);
        /* ccicmp_close + free p handled by ping_close_cb */
    } else {
        ccev__free_fn(p);
    }
}

/* ── Recv callback (EPOLLIN on ICMP fd) ───────────────────────────── */

static void ping_recv_ready(ccev_sock_t *sock, int events) {
    (void)events;
    ccev_ping_t *p = (ccev_ping_t *)sock->udata;

    /* Cancel the timeout timer — the ICMP socket is readable, so the
     * echo exchange is complete (reply, error, or spurious wakeup). */
    if (p->timer) {
        ccev_timer_del(p->loop, p->timer);
        p->timer = NULL;
    }

    /* Try to receive the reply */
    p->reply_len = sizeof(p->reply_buf);
    bool rc = ccicmp_reply(&p->ping, p->reply_buf, &p->reply_len);

    if (rc && p->reply_len > 0) {
        ccev_icmp_result_t result;
        memset(&result, 0, sizeof(result));
        result.rtt_ms      = (double)(ccev__now_ms() - p->send_time);
        result.payload_len = p->reply_len;
        result.ttl         = (p->ping.ttl >= 0) ? p->ping.ttl : 0;

        if (p->cb) p->cb(p->udata, &result);
    } else {
        /* No valid reply — fire error callback so the caller is
         * notified even on spurious socket events. */
        if (p->cb) p->cb(p->udata, NULL);
    }

    if (p->sock)
        ccev__sock_schedule_close(p->loop, p->sock);
    else
        ccev__free_fn(p);
    /* ccicmp_close + free p handled by ping_close_cb on sock close */
}

/* ── Common: send echo and register with reactor ──────────────────── */
/* Called from either the direct IP path or the DNS-resolve callback.
 * Takes ownership of the ccicmp context (must be initialised).
 * On failure, returns -1; on success returns 0. */
static int ccev__icmp_send_echo(ccev_ping_t *p, const char *ip) {
    ccev_loop_t *loop = p->loop;

    /* Record send time (monotonic ms) before transmit */
    p->send_time = ccev__now_ms();

    /* Send the echo request */
    const char *payload = "ccev-ping";
    if (!ccicmp_echo(&p->ping, ip, payload, (int)strlen(payload) + 1))
        return -1;

    /* Register the ICMP fd with the reactor */
    int fd = p->ping.fd;
    ccev_sock_t *s = ccev_sock_create(loop, (ccsocket_t)(intptr_t)fd, p);
    if (!s) return -1;
    p->sock = s;
    ccev_sock_set_close_cb(s, ping_close_cb, p);
    ccev_sock_read_start(s, ping_recv_ready);

    /* Set a timeout */
    if (p->timeout_ms > 0) {
        p->timer = ccev_timer_add(loop, p->timeout_ms, CCEV_TIMER_ONCE,
                                   ping_timeout_cb, p);
        if (!p->timer) {
            /* OOM: no timeout — fire error callback so the caller
             * does not hang waiting for a result.  close_cb will
             * handle ccicmp_close + free p when the sock closes. */
            if (p->cb) p->cb(p->udata, NULL);
            ccev__sock_schedule_close(loop, s);
            return -1;
        }
    }

    return 0;
}

/* ── DNS resolution callback ──────────────────────────────────────── */
/* Fires when the hostname has been resolved (or resolution failed). */

static void ccev__icmp_dns_cb(void *udata, const char *address, int status) {
    ccev_ping_t *p = (ccev_ping_t *)udata;

    if (status != CCEV_OK || !address || address[0] == '\0') {
        /* Resolution failed — fire error callback */
        if (p->timer) {
            ccev_timer_del(p->loop, p->timer);
            p->timer = NULL;
        }
        if (p->cb) p->cb(p->udata, NULL);
        /* No conn was created yet; free ping state directly. */
        ccev__free_fn(p);
        return;
    }

    /* Initialise ccicmp with the resolved address family */
    ccsocket_family_t af = ccsocket_get_version(address);
    if (af != CC_INET4 && af != CC_INET6) af = CC_INET4;
    if (!ccicmp_init(&p->ping, (int)af)) {
        if (af == CC_INET4) {
            /* Fall back to IPv6 */
            if (!ccicmp_init(&p->ping, CC_INET6)) {
                if (p->cb) p->cb(p->udata, NULL);
                /* No conn yet; free directly. */
                ccev__free_fn(p);
                return;
            }
        } else {
            if (p->cb) p->cb(p->udata, NULL);
            ccev__free_fn(p);
            return;
        }
    }

    /* Send echo to the resolved IP address (not the original hostname) */
    if (ccev__icmp_send_echo(p, address) != 0) {
        if (!p->sock) {
            /* sock was never created — cleanup directly */
            ccicmp_close(&p->ping);
            if (p->cb) p->cb(p->udata, NULL);
            ccev__free_fn(p);
        }
        /* sock was created but setup failed (OOM) — cleanup deferred
         * to ping_close_cb; user callback already fired by OOM path */
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════════ */

int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    unsigned int timeout_ms,
                    ccev_icmp_cb cb, void *udata) {
    if (!loop || !host || !cb) return CCEV_ERR;

    ccev_ping_t *p = (ccev_ping_t *)ccev__realloc_fn(NULL, sizeof(ccev_ping_t));
    if (!p) return CCEV_ERR;
    memset(p, 0, sizeof(*p));

    p->loop       = loop;
    p->cb         = cb;
    p->udata      = udata;
    p->timeout_ms = timeout_ms;
    size_t hlen = strnlen(host, sizeof(p->host));
    if (hlen >= sizeof(p->host)) hlen = sizeof(p->host) - 1;
    memcpy(p->host, host, hlen);
    p->host[hlen] = '\0';

    /* Determine if host is an IP address or a hostname */
    ccsocket_family_t af = ccsocket_get_version(host);
    if (af == CC_INET4 || af == CC_INET6) {
        /* Direct IP path — initialise ccicmp and send */
        if (!ccicmp_init(&p->ping, (int)af)) {
            ccev__free_fn(p);
            return CCEV_ERR;
        }

        if (ccev__icmp_send_echo(p, host) != 0) {
            if (!p->sock) {
                ccicmp_close(&p->ping);
                ccev__free_fn(p);
            }
            return CCEV_ERR;
        }
        return CCEV_OK;
    }

    /* Hostname path — resolve via async DNS first */
    /* Use the ICMP timeout as DNS timeout too.
     * When timeout_ms is 0 (no timeout), pass 0 through to DNS.
     * Otherwise clamp to [1000, 10000] for a reasonable DNS window. */
    unsigned int dns_timeout = timeout_ms;
    if (dns_timeout > 0) {
        if (dns_timeout > 10000) dns_timeout = 10000;
        if (dns_timeout < 1000)  dns_timeout = 1000;
    }

    if (ccev_dns_resolve(loop, host, dns_timeout, CCEV_DNS_A | CCEV_DNS_AAAA,
                          ccev__icmp_dns_cb, p) != CCEV_OK) {
        ccev__free_fn(p);
        return CCEV_ERR;
    }

    return CCEV_OK;
}
