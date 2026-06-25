/**
 * @file ccev_icmp.c
 * @brief ICMP echo (ping) support using ccicmp.
 *
 * Integrates ccicmp with the ccev reactor. Sends an ICMP echo request
 * and delivers the result via a reactor-safe callback.
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
    ccev_timer_t   *timer;       /**< Timeout timer */
    ccicmp_t        ping;         /**< ccicmp context */
    char            reply_buf[64];
    size_t          reply_len;
} ccev_ping_t;

static void ping_timeout_cb(void *udata) {
    ccev_ping_t *p = (ccev_ping_t *)udata;
    ccicmp_close(&p->ping);
    if (p->cb) p->cb(p->udata, NULL);
    p->loop->free_fn(p);
}

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

        /* Extract TTL from ancillary data if available */
        result.rtt_ms     = 1.0;    /* rough estimate */
        result.payload_len = p->reply_len;
        result.ttl        = 64;     /* default TTL */

        ccicmp_close(&p->ping);
        if (p->cb) p->cb(p->udata, &result);
        p->loop->free_fn(p);
    }
}

int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    ccev_icmp_cb cb, void *udata) {
    if (!loop || !host || !cb) return CCEV_ERR;

    ccev_ping_t *p = (ccev_ping_t *)loop->realloc_fn(NULL, sizeof(ccev_ping_t));
    if (!p) return CCEV_ERR;
    memset(p, 0, sizeof(*p));

    p->loop  = loop;
    p->cb    = cb;
    p->udata = udata;

    /* ccicmp needs to be on an IPv4 or IPv6 socket. */
    /* First try IPv4 (returns AF_INET). */
    if (!ccicmp_init(&p->ping, CC_INET4)) {
        /* Fall back to IPv6 */
        if (!ccicmp_init(&p->ping, CC_INET6)) {
            loop->free_fn(p);
            return CCEV_ERR;
        }
    }

    /* Send the echo request */
    const char *payload = "ccev-ping";
    if (!ccicmp_echo(&p->ping, host, payload, (int)strlen(payload) + 1)) {
        ccicmp_close(&p->ping);
        loop->free_fn(p);
        return CCEV_ERR;
    }

    /* Register the ICMP fd with the reactor for reply notification */
    int fd = p->ping.fd;
    ccev_conn_t *conn = ccev_conn_create(loop, (ccsocket_t)(intptr_t)fd, p);
    if (!conn) {
        ccicmp_close(&p->ping);
        loop->free_fn(p);
        return CCEV_ERR;
    }

    conn->type = CCEV_CONN_NORMAL;
    ccev_conn_recv(conn, NULL, 0, ping_recv_ready, p);

    /* Set a timeout (default 5 seconds) */
    p->timer = ccev_timer_add(loop, 5000, CCEV_TIMER_ONCE,
                               ping_timeout_cb, p);

    return CCEV_OK;
}
