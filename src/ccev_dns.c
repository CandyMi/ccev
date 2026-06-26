/**
 * @file ccev_dns.c
 * @brief Asynchronous DNS resolver — race mode, with cache + hosts.
 *
 * Wire-format encode/decode uses ccdns (deps/ccsocket/ccdns.h).
 * Queries for A and AAAA are sent in parallel using two ccdns_t contexts.
 * The first valid response wins.
 *
 * DNS cache (per-loop):
 *   - Domain → IP mapping stored in a cchashmap.
 *   - Pre-populated from the OS hosts file at loop initialisation.
 *   - hosts entries are marked cached=true (never expire by TTL).
 *   - DNS-resolved entries are cached with their TTL.
 *   - ccev_dns_flush() clears all entries and reloads hosts.
 */

#include "ccev_internal.h"
#include "ccdns.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ════════════════════════════════════════════════════════════════
 *  DNS config — stored in per-loop state
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
    while (*p) { h = h * 31 + (unsigned char)(*p); p++; }
    return h;
}

static bool cache_equal(const cchashmap_node_t *a, const cchashmap_node_t *b) {
    const ccev_dns_cache_t *ea = CCHASHMAP_CONTAINER(a, ccev_dns_cache_t, node);
    const ccev_dns_cache_t *eb = CCHASHMAP_CONTAINER(b, ccev_dns_cache_t, node);
    return strcmp(ea->domain, eb->domain) == 0;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS cache — probe / insert / flush
 * ════════════════════════════════════════════════════════════════ */

static ccev_dns_cache_t *cache_lookup(ccev_loop_t *loop, const char *domain) {
    if (!loop->dns_cache.buckets) return NULL;
    ccev_dns_cache_t probe;
    memset(&probe, 0, sizeof(probe));
    size_t len = strlen(domain);
    if (len >= sizeof(probe.domain)) len = sizeof(probe.domain) - 1;
    memcpy(probe.domain, domain, len);
    probe.domain[len] = '\0';
    cchashmap_node_t *n = cchashmap_get(&loop->dns_cache, &probe.node);
    if (!n) return NULL;
    ccev_dns_cache_t *e = CCHASHMAP_CONTAINER(n, ccev_dns_cache_t, node);
    if (e->cached) return e;
    uint64_t now = ccev__now_ms();
    if (e->cached_at + (uint64_t)e->ttl * 1000ULL <= now) {
        cchashmap_del(&loop->dns_cache, n);
        loop->free_fn(e);
        return NULL;
    }
    return e;
}

static void cache_insert(ccev_loop_t *loop, const char *domain,
                          const char *ip, int ttl) {
    if (!loop->dns_cache.buckets)
        cchashmap_init(&loop->dns_cache, cache_hash, cache_equal);
    ccev_dns_cache_t *old = cache_lookup(loop, domain);
    if (old) { cchashmap_del(&loop->dns_cache, &old->node); loop->free_fn(old); }
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
    e->ttl = ttl; e->cached = false; e->cached_at = ccev__now_ms();
    cchashmap_set(&loop->dns_cache, &e->node, NULL);
}

void ccev_dns_flush(ccev_loop_t *loop) {
    if (!loop) return;
    if (loop->dns_cache.buckets) {
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
#if defined(_WIN32)
    const char *hosts_path = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
    const char *hosts_path = "/etc/hosts";
#endif
    FILE *fp = fopen(hosts_path, "r");
    if (!fp) return;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        const char *p = line;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        char ip[65]; int pos = 0;
        while (*p && (unsigned char)*p > ' ' && pos < (int)sizeof(ip) - 1) ip[pos++] = *p++;
        ip[pos] = '\0';
        if (pos == 0 || ccsocket_get_version(ip) == CC_FAMILY_INVALID) continue;
        while (*p) {
            while (*p && (unsigned char)*p <= ' ') p++;
            if (*p == '#' || *p == '\0') break;
            char domain[256]; int dpos = 0;
            while (*p && (unsigned char)*p > ' ' && dpos < (int)sizeof(domain) - 1)
                domain[dpos++] = *p++;
            domain[dpos] = '\0';
            if (dpos == 0) continue;
            ccev_dns_cache_t *old = cache_lookup(loop, domain);
            if (old) { cchashmap_del(&loop->dns_cache, &old->node); loop->free_fn(old); }
            if (!loop->dns_cache.buckets)
                cchashmap_init(&loop->dns_cache, cache_hash, cache_equal);
            ccev_dns_cache_t *e = (ccev_dns_cache_t *)
                loop->realloc_fn(NULL, sizeof(ccev_dns_cache_t));
            if (!e) break;
            memset(e, 0, sizeof(*e));
            memcpy(e->domain, domain, (size_t)dpos);
            memcpy(e->ip, ip, (size_t)pos);
            e->ttl = 0; e->cached = true;
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
    ccev_conn_t     *conn;
    ccev_timer_t    *timer;
    ccev_dns_cb      cb;
    void            *udata;
    int              qtype;         /**< CCEV_DNS_A | CCEV_DNS_AAAA   */
    bool             finished;
    ccev_address_t  *result_head;
    ccev_address_t  *result_tail;
    int              result_ttl;
    char             domain[256];
    ccdns_t          ctx_a;         /**< ccdns context for A query     */
    ccdns_t          ctx_aaaa;      /**< ccdns context for AAAA query  */
} ccev_dns_query_t;

/* ════════════════════════════════════════════════════════════════
 *  ccdns adapter — builds ccev_address_t linked list from ccdns_ans_t
 * ════════════════════════════════════════════════════════════════ */

static void dns_adapter_cb(void *udata, const ccdns_ans_t *ans) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (q->finished) return;
    if (ans->type != CCDNS_A && ans->type != CCDNS_AAAA) return;
    if (!ans->ip[0]) return;

    ccev_address_t *addr = (ccev_address_t *)
        calloc(1, sizeof(ccev_address_t));
    if (!addr) return;
    memcpy(addr->ip, ans->ip, sizeof(addr->ip));
    addr->ttl = ans->ttl;
    addr->next = NULL;
    if (q->result_tail) q->result_tail->next = addr;
    else                q->result_head = addr;
    q->result_tail = addr;
    if (q->result_ttl == 0) q->result_ttl = ans->ttl;
}

/* ════════════════════════════════════════════════════════════════
 *  Internal: send a DNS query to all servers
 * ════════════════════════════════════════════════════════════════ */

static int ccev__dns_send_one(ccev_loop_t *loop, ccev_conn_t *conn,
                               ccdns_t *ctx, const char *domain,
                               ccdns_type_t qtype) {
    unsigned char buf[512];
    uint16_t qlen = ccdns_encode(ctx, buf, sizeof(buf), domain, qtype);
    if (qlen == 0) return CCEV_ERR;
    for (int i = 0; i < loop->dns.nservers; i++) {
        int sent = 0;
        ccsocket_sendto(conn->fd, (const char*)buf, qlen,
                        loop->dns.servers[i], (uint16_t)loop->dns.port, &sent);
    }
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS recv callback (fired on EPOLLIN)
 * ════════════════════════════════════════════════════════════════ */

static void dns_recv_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;

    unsigned char buf[1536];
    int n;
    ccsocket_stcode_t rc = ccsocket_recv(q->conn->fd, (char*)buf, sizeof(buf), &n);
    if (rc != CC_OPCODE_OK || n <= 0) return;

    /* Try decoding with A context first, then AAAA */
    int count = ccdns_decode(&q->ctx_a, buf, (size_t)n, q, dns_adapter_cb);
    if (count < 0)
        count = ccdns_decode(&q->ctx_aaaa, buf, (size_t)n, q, dns_adapter_cb);

    if (count > 0 && q->result_head) {
        q->finished = true;
        if (q->timer) { ccev_timer_del(q->loop, q->timer); q->timer = NULL; }
        if (q->result_head->ip[0])
            cache_insert(q->loop, q->domain, q->result_head->ip, q->result_ttl);
        if (q->cb) q->cb(q->udata, q->result_head, CCEV_OK);
        ccev__conn_schedule_close(q->loop, q->conn);
    }
}

static void dns_timeout_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;
    q->finished = true; q->timer = NULL;
    if (q->cb) q->cb(q->udata, NULL, CCEV_ERR);
    if (q->conn) ccev__conn_schedule_close(q->loop, q->conn);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata) {
    if (!loop || !domain || !cb) return CCEV_ERR;

    /* Check cache */
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
    q->loop = loop; q->cb = cb; q->udata = udata; q->qtype = (int)type;
    size_t dlen = strlen(domain);
    if (dlen >= sizeof(q->domain)) dlen = sizeof(q->domain) - 1;
    memcpy(q->domain, domain, dlen);
    q->domain[dlen] = '\0';

    /* Initialise ccdns contexts */
    ccdns_init(&q->ctx_a);
    ccdns_init(&q->ctx_aaaa);

    /* Create UDP socket */
    ccsocket_t fd = ccsocket(CC_INET4, CC_UDP);
    if (fd == (ccsocket_t)-1) { loop->free_fn(q); return CCEV_ERR; }
    ccsocket_set_nonblock(fd, true);

    ccev_conn_t *conn = ccev_conn_create(loop, fd, q);
    if (!conn) { ccsocket_close(fd); loop->free_fn(q); return CCEV_ERR; }
    conn->type = CCEV_CONN_DNS; q->conn = conn;
    conn->recv_cb = dns_recv_cb; conn->recv_udata = q;
    ccev__conn_mod_internal(loop, conn, EPOLLIN);

    if (timeout_ms > 0)
        q->timer = ccev_timer_add(loop, timeout_ms, CCEV_TIMER_ONCE, dns_timeout_cb, q);

    /* Send queries — A and/or AAAA */
    if ((type & CCEV_DNS_A) && ccev__dns_send_one(loop, conn, &q->ctx_a, domain, CCDNS_A) != CCEV_OK) {
        /* Send failure — one less outstanding query; still try AAAA */
    }
    if ((type & CCEV_DNS_AAAA) && ccev__dns_send_one(loop, conn, &q->ctx_aaaa, domain, CCDNS_AAAA) != CCEV_OK) {
    }

    return CCEV_OK;
}

void ccev_dns_free(ccev_address_t *addr) {
    while (addr) {
        ccev_address_t *next = addr->next;
        free(addr);
        addr = next;
    }
}
