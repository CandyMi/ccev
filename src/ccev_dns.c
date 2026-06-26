/**
 * @file ccev_dns.c
 * @brief Asynchronous DNS resolver — race mode, with cache + hosts.
 *
 * Design:
 *   - DNS queries are sent via UDP to ALL configured servers simultaneously.
 *   - The first valid response wins; later responses are discarded.
 *   - Queries for A and AAAA records (if both requested) are sent in
 *     parallel on independent UDP sockets.
 *   - A single callback fires when the first response arrives or all
 *     queries have timed out.
 *
 * DNS cache (per-loop):
 *   - Domain → IP mapping stored in a cchashmap.
 *   - Pre-populated from the OS hosts file at loop initialisation.
 *   - hosts entries are marked cached=true (never expire by TTL).
 *   - DNS-resolved entries are cached with their TTL for subsequent lookups.
 *   - ccev_dns_flush() clears all entries and reloads hosts.
 */

#include "ccev_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ════════════════════════════════════════════════════════════════
 *  DNS config — stored in per-loop state, initialized in loop_create
 * ════════════════════════════════════════════════════════════════ */

#define CCEV_DNS_MAX_SERVERS 4

int ccev_dns_set_server(ccev_loop_t *loop, const char *servers[], int n, int port) {
    if (!loop || !servers || n <= 0 || n > CCEV_DNS_MAX_SERVERS) return CCEV_ERR;
    loop->dns.nservers = n;
    loop->dns.port     = port > 0 ? port : 53;
    for (int i = 0; i < n; i++)
        loop->dns.servers[i] = servers[i];
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache — hash / equal
 * ════════════════════════════════════════════════════════════════ */

static uint64_t cache_hash(const cchashmap_node_t *n, size_t seed) {
    const ccev_dns_cache_t *e = CCHASHMAP_CONTAINER(n, ccev_dns_cache_t, node);
    uint64_t h = seed;
    const char *p = e->domain;
    while (*p) {
        h = h * 31 + (unsigned char)(*p);
        p++;
    }
    return h;
}

static bool cache_equal(const cchashmap_node_t *a, const cchashmap_node_t *b) {
    const ccev_dns_cache_t *ea = CCHASHMAP_CONTAINER(a, ccev_dns_cache_t, node);
    const ccev_dns_cache_t *eb = CCHASHMAP_CONTAINER(b, ccev_dns_cache_t, node);
    return strcmp(ea->domain, eb->domain) == 0;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache — probe helper (stack-allocated probe for lookup)
 * ════════════════════════════════════════════════════════════════ */

static ccev_dns_cache_t *cache_lookup(ccev_loop_t *loop, const char *domain) {
    if (!loop->dns_cache.buckets) return NULL;

    /* Stack probe — only domain field needed for hash/equal */
    ccev_dns_cache_t probe;
    memset(&probe, 0, sizeof(probe));
    size_t len = strlen(domain);
    if (len >= sizeof(probe.domain)) len = sizeof(probe.domain) - 1;
    memcpy(probe.domain, domain, len);
    probe.domain[len] = '\0';

    cchashmap_node_t *n = cchashmap_get(&loop->dns_cache, &probe.node);
    if (!n) return NULL;

    ccev_dns_cache_t *e = CCHASHMAP_CONTAINER(n, ccev_dns_cache_t, node);

    /* hosts entries (cached=true) never expire */
    if (e->cached) return e;

    /* TTL-expired entries are removed and treated as miss */
    uint64_t now = ccev__now_ms();
    if (e->cached_at + (uint64_t)e->ttl * 1000ULL <= now) {
        cchashmap_del(&loop->dns_cache, n);
        loop->free_fn(e);
        return NULL;
    }

    return e;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache — insert after successful resolution
 * ════════════════════════════════════════════════════════════════ */

static void cache_insert(ccev_loop_t *loop, const char *domain,
                          const char *ip, int ttl) {
    /* Initialise hashmap on first use (loop_create sets buckets=NULL) */
    if (!loop->dns_cache.buckets)
        cchashmap_init(&loop->dns_cache, cache_hash, cache_equal);

    /* Remove stale entry if exists */
    ccev_dns_cache_t *old = cache_lookup(loop, domain);
    if (old) {
        cchashmap_del(&loop->dns_cache, &old->node);
        loop->free_fn(old);
    }

    ccev_dns_cache_t *e = (ccev_dns_cache_t *)
        loop->realloc_fn(NULL, sizeof(ccev_dns_cache_t));
    if (!e) return;
    memset(e, 0, sizeof(*e));

    size_t dlen = strlen(domain);
    if (dlen >= sizeof(e->domain)) dlen = sizeof(e->domain) - 1;
    memcpy(e->domain, domain, dlen);
    e->domain[dlen] = '\0';

    size_t ilen = strlen(ip);
    if (ilen >= sizeof(e->ip)) ilen = sizeof(e->ip) - 1;
    memcpy(e->ip, ip, ilen);

    e->ttl       = ttl;
    e->cached    = false;
    e->cached_at = ccev__now_ms();

    cchashmap_set(&loop->dns_cache, &e->node, NULL);
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache — flush (clear all + reload hosts file)
 * ════════════════════════════════════════════════════════════════ */

void ccev_dns_flush(ccev_loop_t *loop) {
    if (!loop) return;

    /* 1. Free all existing cache entries */
    if (loop->dns_cache.buckets) {
        /* Iterate buckets to free every entry */
        for (size_t i = 0; i < loop->dns_cache.cap; i++) {
            cchashmap_node_t *n = loop->dns_cache.buckets[i];
            while (n) {
                cchashmap_node_t *next = n->next;
                ccev_dns_cache_t *e = CCHASHMAP_CONTAINER(n, ccev_dns_cache_t, node);
                loop->free_fn(e);
                n = next;
            }
            loop->dns_cache.buckets[i] = NULL;
        }
        loop->dns_cache.size = 0;
    }

    /* 2. Reload hosts file */
#if defined(_WIN32)
    const char *hosts_path = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
    const char *hosts_path = "/etc/hosts";
#endif

    FILE *fp = fopen(hosts_path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip empty lines and comments */
        const char *p = line;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        /* Parse IP portion */
        char ip[65];
        int pos = 0;
        while (*p && (unsigned char)*p > ' ' && pos < (int)sizeof(ip) - 1)
            ip[pos++] = *p++;
        ip[pos] = '\0';
        if (pos == 0) continue;

        /* Validate IP looks plausible */
        if (ccsocket_get_version(ip) == CC_FAMILY_INVALID) continue;

        /* Parse domain names on the rest of the line */
        while (*p) {
            while (*p && (unsigned char)*p <= ' ') p++;
            if (*p == '#' || *p == '\0') break;

            char domain[256];
            int dpos = 0;
            while (*p && (unsigned char)*p > ' ' && dpos < (int)sizeof(domain) - 1)
                domain[dpos++] = *p++;
            domain[dpos] = '\0';
            if (dpos == 0) continue;

            /* Remove existing entry so later lines override earlier ones */
            ccev_dns_cache_t *old = cache_lookup(loop, domain);
            if (old) {
                cchashmap_del(&loop->dns_cache, &old->node);
                loop->free_fn(old);
            }

            if (!loop->dns_cache.buckets)
                cchashmap_init(&loop->dns_cache, cache_hash, cache_equal);

            ccev_dns_cache_t *e = (ccev_dns_cache_t *)
                loop->realloc_fn(NULL, sizeof(ccev_dns_cache_t));
            if (!e) break;
            memset(e, 0, sizeof(*e));

            memcpy(e->domain, domain, (size_t)dpos);
            memcpy(e->ip, ip, (size_t)pos);
            e->ttl    = 0;
            e->cached = true;       /* hosts entries never expire */

            cchashmap_set(&loop->dns_cache, &e->node, NULL);
        }
    }
    fclose(fp);
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
    char             domain[256];   /**< Saved for cache write        */
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
                                ccev_address_t **out, int *out_ttl) {
    *out = NULL;
    if (out_ttl) *out_ttl = 0;
    if (len < 12) return CCEV_ERR;

    /* Check response flags */
    unsigned char rcode = buf[3] & 0x0F;
    if ((buf[2] & 0x80) == 0 || rcode != 0) /* QR bit not set or error */
        return CCEV_ERR;

    int ancount = (buf[6] << 8) | buf[7];
    if (ancount <= 0) return CCEV_ERR;

    /* Skip header + question section to reach answer section */
    size_t pos = 12;
    while (pos < len) {
        if (buf[pos] == 0) { pos++; break; }
        if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
        pos += (size_t)buf[pos] + 1;
    }
    pos += 4;

    /* Parse answers, keeping the TTL from the first matching record */
    ccev_address_t *head = NULL, *tail = NULL;
    int first_ttl = 0;
    for (int i = 0; i < ancount && pos + 10 < len; i++) {
        if (pos < len && (buf[pos] & 0xC0) == 0xC0) {
            pos += 2;
        } else {
            while (pos < len && buf[pos] != 0) pos += (size_t)buf[pos] + 1;
            if (pos < len) pos++;
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
            if (first_ttl == 0) first_ttl = (int)ttl;

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

            if (tail) tail->next = addr;
            else      head = addr;
            tail = addr;
        }

        pos += rdlen;
    }

    if (out_ttl && head) *out_ttl = first_ttl;
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

    for (int i = 0; i < loop->dns.nservers; i++) {
        int sent_dns = 0;
        ccsocket_sendto(conn->fd, (const char*)buf, (size_t)qlen,
                        loop->dns.servers[i], (uint16_t)loop->dns.port, &sent_dns);
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
    int ttl = 0;
    if (dns_decode_response(buf, (size_t)n, &addr, &ttl) == CCEV_OK && addr) {
        q->finished = true;

        /* Cache the first resolved IP */
        if (addr && addr->ip[0])
            cache_insert(q->loop, q->domain, addr->ip, ttl);

        if (q->timer) {
            ccev_timer_del(q->loop, q->timer);
            q->timer = NULL;
        }

        if (q->cb)
            q->cb(q->udata, addr, CCEV_OK);

        ccev__conn_schedule_close(q->loop, q->conn);
    }
}

static void dns_timeout_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;

    q->finished = true;
    q->timer = NULL;

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

    /* Check cache before querying the network */
    ccev_dns_cache_t *cached = cache_lookup(loop, domain);
    if (cached) {
        ccev_address_t *addr = (ccev_address_t *)
            calloc(1, sizeof(ccev_address_t));
        if (!addr) return CCEV_ERR;
        memcpy(addr->ip, cached->ip, sizeof(addr->ip));
        addr->ttl = cached->ttl;
        cb(udata, addr, CCEV_OK);
        return CCEV_OK;
    }

    /* Allocate query state */
    ccev_dns_query_t *q = (ccev_dns_query_t *)
        loop->realloc_fn(NULL, sizeof(ccev_dns_query_t));
    if (!q) return CCEV_ERR;
    memset(q, 0, sizeof(*q));

    q->loop   = loop;
    q->cb     = cb;
    q->udata  = udata;
    q->qtype  = (int)type;
    size_t dlen = strlen(domain);
    if (dlen >= sizeof(q->domain)) dlen = sizeof(q->domain) - 1;
    memcpy(q->domain, domain, dlen);
    q->domain[dlen] = '\0';

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

    conn->recv_cb   = dns_recv_cb;
    conn->recv_udata = q;
    ccev__conn_mod_internal(loop, conn, EPOLLIN);

    if (timeout_ms > 0) {
        q->timer = ccev_timer_add(loop, timeout_ms, CCEV_TIMER_ONCE,
                                   dns_timeout_cb, q);
    }

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
