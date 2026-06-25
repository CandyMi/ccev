/**
 * @file ccev_dns.c
 * @brief Asynchronous DNS resolver — race mode, no UDP connect.
 *
 * Design:
 *   - DNS queries are sent via UDP to ALL configured servers simultaneously.
 *   - The first valid response wins; later responses are discarded.
 *   - Queries for A and AAAA records (if both requested) are sent in
 *     parallel on independent UDP sockets.
 *   - A single callback fires when the first response arrives or all
 *     queries have timed out.
 *
 * Integration with the reactor:
 *   Each DNS query creates a UDP socket and a ccev_conn_t.
 *   On EPOLLIN, the response is decoded. On timer expiry, the
 *   query is aborted.
 */

#include "ccev_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════
 *  Global DNS config (per-process)
 * ════════════════════════════════════════════════════════════════ */

#define CCEV_DNS_MAX_SERVERS 4

static const char *g_dns_servers[CCEV_DNS_MAX_SERVERS] = { "1.1.1.1" };
static int         g_dns_nservers = 1;
static int         g_dns_port     = 53;

int ccev_dns_set_server(const char *servers[], int n, int port) {
    if (!servers || n <= 0 || n > CCEV_DNS_MAX_SERVERS) return CCEV_ERR;
    g_dns_nservers = n;
    g_dns_port     = port > 0 ? port : 53;
    for (int i = 0; i < n; i++)
        g_dns_servers[i] = servers[i];
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS internal state (per-resolution)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_dns_query_s {
    ccev_loop_t     *loop;
    ccev_conn_t     *conn;          /**< UDP socket wrapper            */
    ccev_timer_t    *timer;         /**< Per-query timeout timer       */
    ccev_dns_cb      cb;            /**< User callback                 */
    void            *udata;         /**< User data                     */
    int              qtype;         /**< CCEV_DNS_A | CCEV_DNS_AAAA   */
    bool             finished;      /**< true after callback fired     */
    ccev_address_t  *result;        /**< Accumulated results           */
    int              active;        /**< Number of outstanding queries */
} ccev_dns_query_t;

/* ════════════════════════════════════════════════════════════════
 *  DNS wire-format helpers (minimal — A / AAAA only)
 * ════════════════════════════════════════════════════════════════ */

static uint16_t dns_id_counter = 1;

static int dns_encode_query(unsigned char *buf, size_t buflen,
                             const char *domain, int qtype, uint16_t id) {
    size_t pos = 0;

    /* DNS header: 12 bytes */
    if (buflen < 12) return -1;
    buf[pos++] = (id >> 8) & 0xFF;       /* ID high */
    buf[pos++] = id & 0xFF;              /* ID low */
    buf[pos++] = 0x01;                    /* flags: recursion desired */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;  buf[pos++] = 0x01; /* QDCOUNT = 1 */
    buf[pos++] = 0x00;  buf[pos++] = 0x00; /* ANCOUNT = 0 */
    buf[pos++] = 0x00;  buf[pos++] = 0x00; /* NSCOUNT = 0 */
    buf[pos++] = 0x00;  buf[pos++] = 0x00; /* ARCOUNT = 0 */

    /* QNAME — domain name encoded as length-prefixed labels */
    const char *p = domain;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        if (label_len > 63 || pos + label_len + 1 > buflen) return -1;
        buf[pos++] = (unsigned char)label_len;
        memcpy(buf + pos, p, label_len);
        pos += label_len;
        p = dot ? dot + 1 : p + label_len;
    }
    buf[pos++] = 0x00;   /* root label */

    /* QTYPE */
    uint16_t wire_type;
    if (qtype == CCEV_DNS_A) wire_type = 1;      /* A */
    else if (qtype == CCEV_DNS_AAAA) wire_type = 28; /* AAAA */
    else return -1;
    buf[pos++] = (wire_type >> 8) & 0xFF;
    buf[pos++] = wire_type & 0xFF;

    /* QCLASS = IN */
    buf[pos++] = 0x00; buf[pos++] = 0x01;

    return (int)pos;
}

static int dns_decode_response(const unsigned char *buf, size_t len,
                                ccev_address_t **out) {
    *out = NULL;
    if (len < 12) return CCEV_ERR;

    /* Check response flags */
    unsigned char rcode = buf[3] & 0x0F;
    if ((buf[2] & 0x80) == 0 || rcode != 0) /* QR bit not set or error */
        return CCEV_ERR;

    int ancount = (buf[6] << 8) | buf[7];
    if (ancount <= 0) return CCEV_ERR;

    /* Skip header + question section to reach answer section */
    size_t pos = 12;
    /* Parse question (skip QNAME + QTYPE + QCLASS) */
    while (pos < len) {
        if (buf[pos] == 0) { pos++; break; }
        if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
        pos += (size_t)buf[pos] + 1;
    }
    pos += 4;  /* skip QTYPE + QCLASS */

    /* Parse answers */
    ccev_address_t *head = NULL, *tail = NULL;
    for (int i = 0; i < ancount && pos + 10 < len; i++) {
        /* NAME (compression pointer or label) */
        if (pos < len && (buf[pos] & 0xC0) == 0xC0) {
            pos += 2;  /* compression pointer */
        } else {
            while (pos < len && buf[pos] != 0) pos += (size_t)buf[pos] + 1;
            if (pos < len) pos++;  /* root */
        }

        if (pos + 10 > len) break;
        uint16_t rtype  = (buf[pos] << 8) | buf[pos+1];
        uint16_t rclass = (buf[pos+2] << 8) | buf[pos+3];
        uint32_t ttl    = ((uint32_t)buf[pos+4] << 24) |
                          ((uint32_t)buf[pos+5] << 16) |
                          ((uint32_t)buf[pos+6] << 8)  |
                          (uint32_t)buf[pos+7];
        uint16_t rdlen  = (buf[pos+8] << 8) | buf[pos+9];
        pos += 10;

        if (pos + rdlen > len) break;

        if ((rtype == 1 || rtype == 28) && rclass == 1) {
            ccev_address_t *addr = (ccev_address_t *)
                calloc(1, sizeof(ccev_address_t));
            if (!addr) break;

            addr->ttl = (int)ttl;

            if (rtype == 1 && rdlen >= 4) {
                snprintf(addr->ip, sizeof(addr->ip),
                         "%u.%u.%u.%u",
                         buf[pos], buf[pos+1], buf[pos+2], buf[pos+3]);
            } else if (rtype == 28 && rdlen >= 16) {
                snprintf(addr->ip, sizeof(addr->ip),
                         "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                         "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                         buf[pos], buf[pos+1], buf[pos+2], buf[pos+3],
                         buf[pos+4], buf[pos+5], buf[pos+6], buf[pos+7],
                         buf[pos+8], buf[pos+9], buf[pos+10], buf[pos+11],
                         buf[pos+12], buf[pos+13], buf[pos+14], buf[pos+15]);
            } else {
                free(addr);
                pos += rdlen;
                continue;
            }

            /* Append to list */
            if (tail) tail->next = addr;
            else      head = addr;
            tail = addr;
        }

        pos += rdlen;
    }

    *out = head;
    return head ? CCEV_OK : CCEV_ERR;
}

/* ════════════════════════════════════════════════════════════════
 *  Internal: send a DNS query to all servers
 * ════════════════════════════════════════════════════════════════ */

static int ccev__dns_send_to_all(ccev_loop_t *loop, ccev_conn_t *conn,
                                  const char *domain, int qtype,
                                  uint16_t dns_id) {
    unsigned char buf[512];
    int qlen = dns_encode_query(buf, sizeof(buf), domain, qtype, dns_id);
    if (qlen < 0) return CCEV_ERR;

    /* Send to all configured DNS servers */
    for (int i = 0; i < g_dns_nservers; i++) {
        int sent_dns = 0;
    ccsocket_sendto(conn->fd, (const char*)buf, (size_t)qlen,
                     g_dns_servers[i], (uint16_t)g_dns_port, &sent_dns);
    }
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS recv callback (fired on EPOLLIN from the DNS socket)
 * ════════════════════════════════════════════════════════════════ */

static void dns_recv_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;

    unsigned char buf[1536];
    int n;
    ccsocket_stcode_t rc = ccsocket_recv(q->conn->fd, (char*)buf, sizeof(buf), &n);
    if (rc != CC_OPCODE_OK || n <= 0) return;

    ccev_address_t *addr = NULL;
    if (dns_decode_response(buf, (size_t)n, &addr) == CCEV_OK && addr) {
        q->finished = true;

        /* Cancel timeout timers */
        if (q->timer) {
            ccev_timer_del(q->loop, q->timer);
            q->timer = NULL;
        }

        /* Fire callback */
        if (q->cb)
            q->cb(q->udata, addr, CCEV_OK);

        /* Clean up */
        ccev__conn_schedule_close(q->loop, q->conn);
    }
}

/* The connection timeout timer fires if no response is received */
static void dns_timeout_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;

    q->finished = true;
    q->timer = NULL;  /* timer will be freed by timer subsystem */

    if (q->cb)
        q->cb(q->udata, NULL, CCEV_ERR);

    if (q->conn)
        ccev__conn_schedule_close(q->loop, q->conn);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata) {
    if (!loop || !domain || !cb) return CCEV_ERR;

    /* Allocate query state */
    ccev_dns_query_t *q = (ccev_dns_query_t *)
        loop->realloc_fn(NULL, sizeof(ccev_dns_query_t));
    if (!q) return CCEV_ERR;
    memset(q, 0, sizeof(*q));

    q->loop   = loop;
    q->cb     = cb;
    q->udata  = udata;
    q->qtype  = (int)type;

    /* Create UDP socket (no connect — sendto to all servers) */
    ccsocket_t fd = ccsocket(CC_INET4, CC_UDP);
    if (fd == (ccsocket_t)-1) {
        loop->free_fn(q);
        return CCEV_ERR;
    }
    ccsocket_set_nonblock(fd, true);

    /* Wrap in conn */
    ccev_conn_t *conn = ccev_conn_create(loop, fd, q);
    if (!conn) {
        ccsocket_close(fd);
        loop->free_fn(q);
        return CCEV_ERR;
    }
    conn->type = CCEV_CONN_DNS;
    q->conn    = conn;

    /* Set recv callback and arm read */
    conn->recv_cb   = dns_recv_cb;
    conn->recv_udata = q;
    ccev__conn_mod_internal(loop, conn, EPOLLIN);

    /* Set timeout timer */
    if (timeout_ms > 0) {
        q->timer = ccev_timer_add(loop, timeout_ms, CCEV_TIMER_ONCE,
                                   dns_timeout_cb, q);
    }

    /* Send queries — A and/or AAAA */
    uint16_t base_id = dns_id_counter;
    dns_id_counter += 2;

    if (type & CCEV_DNS_A)
        ccev__dns_send_to_all(loop, conn, domain, CCEV_DNS_A, base_id);
    if (type & CCEV_DNS_AAAA)
        ccev__dns_send_to_all(loop, conn, domain, CCEV_DNS_AAAA, base_id + 1);

    return CCEV_OK;
}

void ccev_dns_free(ccev_address_t *addr) {
    while (addr) {
        ccev_address_t *next = addr->next;
        free(addr);
        addr = next;
    }
}
