/**
 * @file ccev_dns.c
 * @brief Asynchronous DNS resolver — race mode, with cache + hosts + pending.
 *
 * @author CandyMi
 * @license MIT
 *
 * Wire-format encode/decode uses ccdns (deps/ccsocket/ccdns.h).
 * Queries for A and AAAA are sent in parallel using two ccdns_t contexts.
 * The first valid response wins.
 *
 * Concurrent requests for the same domain are coalesced: when a resolution
 * is already in-flight, new callbacks are appended as waiters instead of
 * creating duplicate UDP sockets.
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

/* ════════════════════════════════════════════════════════════════
 *  DNS config
 * ════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════
 *  DNS initialisation — parse /etc/resolv.conf for nameservers,
 *  fall back to {"1.1.1.1", 53}
 * ════════════════════════════════════════════════════════════════ */

void ccev__dns_init(ccev_loop_t *loop) {
    if (!loop) return;

    loop->dns.nservers = 0;

#if !defined(_WIN32)
    FILE *fp = fopen("/etc/resolv.conf", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp) &&
               loop->dns.nservers < CCEV_DNS_MAX_SERVERS) {
            const char *p = line;
            while (*p && (unsigned char)*p <= ' ') p++;
            if (*p == '#' || *p == '\0' || *p == '\n') continue;
            if (strncmp(p, "nameserver", 10) != 0) continue;
            p += 10;
            while (*p && (unsigned char)*p <= ' ') p++;
            char ip[64];
            int  pos = 0;
            while (*p && (unsigned char)*p > ' ' && pos < (int)sizeof(ip) - 1)
                ip[pos++] = *p++;
            ip[pos] = '\0';
            if (pos == 0) continue;
            if (ccsocket_get_version(ip) == CC_FAMILY_INVALID) continue;

            size_t len = (size_t)pos;
            char *copy = (char *)ccev__realloc_fn(NULL, len + 1);
            if (!copy) continue;
            memcpy(copy, ip, len + 1);
            loop->dns.servers[loop->dns.nservers].server = copy;
            loop->dns.servers[loop->dns.nservers].port   = 53;
            loop->dns.nservers++;
        }
        fclose(fp);
    }
#endif /* !_WIN32 */

    if (loop->dns.nservers == 0) {
        char *srv = (char *)ccev__realloc_fn(NULL, 8);
        if (srv) {
            memcpy(srv, "1.1.1.1", 8);
            loop->dns.servers[0].server = srv;
            loop->dns.servers[0].port   = 53;
            loop->dns.nservers          = 1;
        }
    }
}

int ccev_dns_set_server(ccev_loop_t *loop,
                         const ccev_dns_server_t servers[], int n) {
    if (!loop || !servers || n <= 0 || n > CCEV_DNS_MAX_SERVERS) return CCEV_ERR;

    /* Free all old server strings first */
    for (int i = 0; i < loop->dns.nservers; i++)
        ccev__free_fn((void *)(uintptr_t)loop->dns.servers[i].server);

    loop->dns.nservers = 0;  /* temporarily empty — clean OOM rollback */

    for (int i = 0; i < n; i++) {
        if (!servers[i].server) return CCEV_ERR;
        size_t len = strlen(servers[i].server);
        char *copy = (char *)ccev__realloc_fn(NULL, len + 1);
        if (!copy) {
            /* OOM: free already-copied entries, state stays clean */
            for (int j = 0; j < i; j++)
                ccev__free_fn((void *)(uintptr_t)loop->dns.servers[j].server);
            return CCEV_ERR;
        }
        memcpy(copy, servers[i].server, len + 1);
        loop->dns.servers[i].server = copy;
        loop->dns.servers[i].port   = servers[i].port > 0 ? servers[i].port : 53;
    }

    loop->dns.nservers = n;
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  Domain hash/equal helpers — FNV-1a via function pointers
 *  Both ccev_dns_cache_t and ccev_dns_pending_t embed the domain
 *  string right after cchashmap_node_t, so (n + 1) reaches it
 *  at the same offset — a single pair of functions suffices.
 * ════════════════════════════════════════════════════════════════ */

/* FNV-1a hash: (n + 1) reaches domain for both cache & pending types */
static uint64_t dns_domain_hash(const cchashmap_node_t *n, size_t seed) {
    const unsigned char *p = (const unsigned char *)(n + 1);
    uint64_t h = seed ^ 0xcbf29ce484222325ULL;
    while (*p) { h ^= *p++; h *= 0x100000001b3ULL; }
    return h;
}
static bool dns_domain_equal(const cchashmap_node_t *a, const cchashmap_node_t *b) {
    return strcmp((const char *)(a + 1), (const char *)(b + 1)) == 0;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS answer collector (stack-allocated, used in recv callback)
 * ════════════════════════════════════════════════════════════════ */

/* Collects the best A/AAAA answer from ccdns_decode results.
 * Lives on the stack in dns_recv_cb — no heap allocation. */
typedef struct dns_collect_s {
    char  best_ip[256];   /**< Best IP string (longest TTL) */
    int   best_ttl;       /**< TTL of the selected IP */
    bool  has_ip;         /**< true if any valid A/AAAA was found */
} dns_collect_t;

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
        ccev__free_fn(e);
        return NULL;
    }
    return e;
}

static void cache_insert(ccev_loop_t *loop, const char *domain,
                          const char *ip, int ttl) {
    if (!loop->dns_cache.buckets)
        cchashmap_init(&loop->dns_cache, dns_domain_hash, dns_domain_equal);
    ccev_dns_cache_t *old = cache_lookup(loop, domain);
    if (old) { cchashmap_del(&loop->dns_cache, &old->node); ccev__free_fn(old); }
    ccev_dns_cache_t *e = (ccev_dns_cache_t *)
        ccev__realloc_fn(NULL, sizeof(ccev_dns_cache_t));
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
                ccev__free_fn(e);
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
    char line[4096];
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
            if (old) { cchashmap_del(&loop->dns_cache, &old->node); ccev__free_fn(old); }
            if (!loop->dns_cache.buckets)
                cchashmap_init(&loop->dns_cache, dns_domain_hash, dns_domain_equal);
            ccev_dns_cache_t *e = (ccev_dns_cache_t *)
                ccev__realloc_fn(NULL, sizeof(ccev_dns_cache_t));
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
 *  Pending resolution helpers
 * ════════════════════════════════════════════════════════════════ */

/* Look up a pending resolution by domain (thread-safe: reactor thread only) */
static ccev_dns_pending_t *pending_lookup(ccev_loop_t *loop, const char *domain) {
    if (!loop->dns_pending.buckets) return NULL;
    ccev_dns_pending_t probe;
    memset(&probe, 0, sizeof(probe));
    size_t len = strlen(domain);
    if (len >= sizeof(probe.domain)) len = sizeof(probe.domain) - 1;
    memcpy(probe.domain, domain, len);
    probe.domain[len] = '\0';
    cchashmap_node_t *n = cchashmap_get(&loop->dns_pending, &probe.node);
    return n ? CCHASHMAP_CONTAINER(n, ccev_dns_pending_t, node) : NULL;
}

/* Register a pending resolution entry */
static ccev_dns_pending_t *pending_add(ccev_loop_t *loop, const char *domain) {
    if (!loop->dns_pending.buckets)
        cchashmap_init(&loop->dns_pending, dns_domain_hash, dns_domain_equal);
    ccev_dns_pending_t *p = (ccev_dns_pending_t *)
        ccev__realloc_fn(NULL, sizeof(ccev_dns_pending_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    size_t dlen = strlen(domain);
    if (dlen >= sizeof(p->domain)) dlen = sizeof(p->domain) - 1;
    memcpy(p->domain, domain, dlen);
    p->domain[dlen] = '\0';
    cclink_init(&p->waiters);
    if (cchashmap_set(&loop->dns_pending, &p->node, NULL))
        return p;
    ccev__free_fn(p);  /* duplicate (shouldn't happen) */
    return NULL;
}

/* Remove and free a pending entry (does NOT free waiters — caller handles) */
static void pending_remove(ccev_loop_t *loop, ccev_dns_pending_t *p) {
    cchashmap_del(&loop->dns_pending, &p->node);
    ccev__free_fn(p);
}

/* Pop-front distribution — safe for re-entrant push_back from callbacks.
 * Does NOT call pending_remove — caller handles lifecycle. */
static void pending_distribute(ccev_loop_t *loop, ccev_dns_pending_t *p,
                                const char *address, int status) {
    while (!cclink_empty(&p->waiters)) {
        cclink_node_t *wn = cclink_pop_front(&p->waiters);
        ccev_dns_waiter_t *w = CCLINK_CONTAINER(wn, ccev_dns_waiter_t, node);
        if (w->cb) w->cb(w->udata, address, status);
        ccev__free_fn(w);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  DNS internal state (per-resolution)
 * ════════════════════════════════════════════════════════════════ */

typedef struct ccev_dns_query_s {
    ccev_loop_t     *loop;
    ccev_sock_t     *sock;
    ccev_timer_t    *timer;
    ccev_dns_cb      cb;
    void            *udata;
    bool             finished;
    char             domain[256];
    ccdns_t          ctx_a;
    ccdns_t          ctx_aaaa;
    /* pending entry, if this query is the primary for a domain */
    ccev_dns_pending_t *pending;
} ccev_dns_query_t;

/* ════════════════════════════════════════════════════════════════
 *  ccdns collect callback — picks the answer with the longest TTL
 * ════════════════════════════════════════════════════════════════ */

/* Collect callback for ccdns_decode — picks the answer with the longest TTL.
 * Lives on the stack in dns_recv_cb; no heap allocation. */
static void dns_collect_cb(void *udata, const ccdns_ans_t *ans) {
    dns_collect_t *col = (dns_collect_t *)udata;
    if (!ans || !col) return;
    if (ans->type != CCDNS_A && ans->type != CCDNS_AAAA) return;
    if (!ans->ip[0]) return;
    if (!col->has_ip || ans->ttl > (uint32_t)col->best_ttl) {
        memcpy(col->best_ip, ans->ip, sizeof(col->best_ip));
        col->best_ip[sizeof(col->best_ip) - 1] = '\0';
        col->best_ttl = (int)ans->ttl;
        col->has_ip = true;
    }
}

/* ════════════════════════════════════════════════════════════════
 *  Internal: send a DNS query to all servers
 * ════════════════════════════════════════════════════════════════ */

static int ccev__dns_send_one(ccev_loop_t *loop, ccsocket_t fd,
                               ccdns_t *ctx, const char *domain,
                               ccdns_type_t qtype) {
    unsigned char buf[512];
    uint16_t qlen = ccdns_encode(ctx, buf, sizeof(buf), domain, qtype);
    if (qlen == 0) return CCEV_ERR;
    for (int i = 0; i < loop->dns.nservers; i++) {
        int sent = 0;
        ccsocket_sendto(fd, (const char*)buf, qlen,
                        loop->dns.servers[i].server,
                        loop->dns.servers[i].port, &sent);
    }
    return CCEV_OK;
}

/* ════════════════════════════════════════════════════════════════
 *  DNS recv callback — distribute result to original cb + waiters
 * ════════════════════════════════════════════════════════════════ */

static void dns_recv_cb(ccev_sock_t *sock, int events) {
    (void)events;
    ccev_dns_query_t *q = (ccev_dns_query_t *)sock->udata;
    if (!q || q->finished) return;

    unsigned char buf[4096];
    int n;
    ccsocket_stcode_t rc = ccsocket_recv(sock->fd, (char*)buf, sizeof(buf), &n);
    if (rc != CC_OPCODE_OK || n <= 0) return;

    q->finished = true;
    if (q->timer) { ccev_timer_del(q->loop, q->timer); q->timer = NULL; }

    /* Collect best IP from DNS response (stack, no allocation) */
    dns_collect_t col;
    memset(&col, 0, sizeof(col));

    int ret = ccdns_decode(&q->ctx_a, buf, (size_t)n, &col, dns_collect_cb);
    if (ret < 0)
        ccdns_decode(&q->ctx_aaaa, buf, (size_t)n, &col, dns_collect_cb);

    char addr[256] = "";
    int status = CCEV_ERR;
    if (col.has_ip) {
        memcpy(addr, col.best_ip, sizeof(col.best_ip));
        addr[sizeof(col.best_ip) - 1] = '\0';
        status = CCEV_OK;
    }

    ccev_dns_pending_t *p = q->pending;

    /* 1. Distribute waiters (pop_front — safe for re-enter push_back) */
    if (p) pending_distribute(q->loop, p, addr, status);

    /* 2. Fire original callback — re-entered resolve appends to p->waiters */
    if (q->cb) q->cb(q->udata, addr, status);

    /* 3. Harvest waiters added during q->cb re-entry */
    if (p) {
        pending_distribute(q->loop, p, addr, status);
        pending_remove(q->loop, p);
        q->pending = NULL;
    }

    /* 4. Write cache AFTER all callbacks */
    if (status == CCEV_OK)
        cache_insert(q->loop, q->domain, addr, col.best_ttl);

    ccev__sock_schedule_close(q->loop, q->sock);
    ccev__free_fn(q);
}

static void dns_timeout_cb(void *udata) {
    ccev_dns_query_t *q = (ccev_dns_query_t *)udata;
    if (!q || q->finished) return;
    q->finished = true;
    q->timer = NULL;
#if !defined(_WIN32)
    write(2, "T", 1);
    if (q->cb) write(2, "C", 1);
#endif
    ccev_dns_pending_t *p = q->pending;

    /* 1. Distribute waiters with timeout */
    if (p) pending_distribute(q->loop, p, "", CCEV_ERR);

    /* 2. Fire original callback */
    if (q->cb) q->cb(q->udata, "", CCEV_ERR);

    /* 3. Harvest re-entered waiters */
    if (p) {
        pending_distribute(q->loop, p, "", CCEV_ERR);
        pending_remove(q->loop, p);
        q->pending = NULL;
    }

    /* 4. No cache write on timeout */

    if (q->sock) ccev__sock_schedule_close(q->loop, q->sock);
    ccev__free_fn(q);
}

/* ════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════ */

int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata) {
    if (!loop || !domain || !cb) return CCEV_ERR;

    /* DNS wire format limit: 255 octets per label sequence.
     * Reject longer domains upfront to avoid silent truncation. */
    if (strlen(domain) >= 255) return CCEV_ERR;

    /* 1. IP or UDS short circuit — domain is already a direct address */
    {
        ccsocket_family_t family = ccsocket_get_version(domain);
        if (family == CC_INET4 || family == CC_INET6 || family == CC_UNIX) {
            cb(udata, domain, CCEV_OK);
            return CCEV_OK;
        }
    }

    /* 2. Check cache */
    ccev_dns_cache_t *cached = cache_lookup(loop, domain);
    if (cached) {
        char addr[256];
        memset(addr, 0, sizeof(addr));
        memcpy(addr, cached->ip, sizeof(cached->ip));
        cb(udata, addr, CCEV_OK);
        return CCEV_OK;
    }

    /* 3. Check pending — if an in-flight resolution exists, append waiter */
    ccev_dns_pending_t *pending = pending_lookup(loop, domain);
    if (pending) {
        ccev_dns_waiter_t *w = (ccev_dns_waiter_t *)
            ccev__realloc_fn(NULL, sizeof(ccev_dns_waiter_t));
        if (!w) return CCEV_ERR;
        w->cb = cb; w->udata = udata;
        cclink_push_back(&pending->waiters, &w->node);
        return CCEV_OK;
    }

    /* 4. Cache miss + no pending — start a new resolution */
    /* Register pending FIRST so racing callers find it */
    pending = pending_add(loop, domain);
    if (!pending) return CCEV_ERR;

    ccev_dns_query_t *q = (ccev_dns_query_t *)
        ccev__realloc_fn(NULL, sizeof(ccev_dns_query_t));
    if (!q) { pending_remove(loop, pending); return CCEV_ERR; }
    memset(q, 0, sizeof(*q));
    q->loop = loop; q->cb = cb; q->udata = udata;
    q->pending = pending;
    size_t dlen = strlen(domain);
    if (dlen >= sizeof(q->domain)) dlen = sizeof(q->domain) - 1;
    memcpy(q->domain, domain, dlen);
    q->domain[dlen] = '\0';

    ccdns_init(&q->ctx_a);
    ccdns_init(&q->ctx_aaaa);

    ccsocket_t fd = ccsocket(CC_INET4, CC_UDP);
    if (fd == (ccsocket_t)-1) { pending_remove(loop, pending); ccev__free_fn(q); return CCEV_ERR; }
    ccsocket_set_nonblock(fd, true);

    ccev_sock_t *sock = ccev_sock_create(loop, fd, q);
    if (!sock) { ccsocket_close(fd); pending_remove(loop, pending); ccev__free_fn(q); return CCEV_ERR; }
    q->sock = sock;
    ccev_sock_read_start(sock, dns_recv_cb);

    if (timeout_ms > 0)
        q->timer = ccev_timer_add(loop, timeout_ms, CCEV_TIMER_ONCE, dns_timeout_cb, q);

    if ((type & CCEV_DNS_A))
        ccev__dns_send_one(loop, fd, &q->ctx_a, domain, CCDNS_A);
    if ((type & CCEV_DNS_AAAA))
        ccev__dns_send_one(loop, fd, &q->ctx_aaaa, domain, CCDNS_AAAA);

    return CCEV_OK;
}


