# ccev API Reference

## Macros

| Macro | Value | Description |
|---|---|---|
| `CCEV_OK` | `0` | Operation completed successfully. |
| `CCEV_ERR` | `-1` | Operation failed / fd is closed / error. |
| `CCEV_DOMAIN_MAXLEN` | `256` | Maximum host string length (IPv4/IPv6/UDS/FQDN). |

## Event Flags

```c
enum {
    CCEV_EVENT_NONE  = 0,          /* No event. */
    CCEV_EVENT_READ  = 1 << 0,     /* Data available to read. */
    CCEV_EVENT_WRITE = 1 << 1,     /* Socket ready for writing. */
    CCEV_EVENT_HUP   = 1 << 2,     /* Peer closed (fast-path, not
                                    * guaranteed on select backend). */
};
```

Passed to `ccev_event_cb`. `CCEV_EVENT_HUP` is an optimization — on the
select backend, peer close is detected via `CCEV_EVENT_READ` + `recv()==0`.

## Enumerations

```c
typedef enum {
    CCEV_RUN_ONCE,      /* Process ready events once and return. */
    CCEV_RUN_FOREVER,   /* Loop until ccev_loop_stop() is called. */
} ccev_run_mode_t;

typedef enum {
    CCEV_TIMER_ONCE,    /* Fire once then auto-remove. */
    CCEV_TIMER_REPEAT,  /* Fire repeatedly at the given interval. */
} ccev_timer_mode_t;

typedef enum {
    CCEV_DNS_A    = 1,  /* A record (IPv4). */
    CCEV_DNS_AAAA = 2,  /* AAAA record (IPv6). */
} ccev_dns_type_t;
```

## Flag Constants

```c
typedef unsigned int ccev_flag_t;
enum {
    CCEV_REUSEADDR    = 1u << 8,   /* SO_REUSEADDR */
    CCEV_REUSEPORT    = 1u << 9,   /* SO_REUSEPORT */
    CCEV_TCP_NODELAY  = 1u << 10,  /* TCP_NODELAY */
    CCEV_ACCEPT_DEFER = 1u << 11,  /* TCP_DEFER_ACCEPT / SO_ACCEPTFILTER */
};
```

Note: `CCEV_UDP` was removed in v0.2.0. For UDP, create a socket with
`ccsocket()` and register it via `ccev_sock_create()`.

## Types

```c
typedef struct ccev_loop_s    ccev_loop_t;    /* Event-loop handle.    */
typedef struct ccev_sock_s    ccev_sock_t;    /* Low-level socket.     */
typedef struct ccev_stream_s  ccev_stream_t;  /* High-level stream.    */
typedef struct ccev_timer_s   ccev_timer_t;   /* Timer handle.         */
```

`ccev_stream_t` embeds a `ccev_sock_t` as its first field — the address
of a stream is the same as the address of its embedded sock.  Create a
stream by upgrading a sock: `ccev_stream_open(sock)`.

## Callbacks

```c
/* Low-level event callback (ccev_sock_t) */
typedef void (*ccev_event_cb)(ccev_sock_t *sock, int events);

/* Write-complete (per-buffer or global) */
typedef void (*ccev_send_cb)(void *udata);

/* Peer closed / error */
typedef void (*ccev_close_cb)(void *udata);

/* Accept callback */
typedef void (*ccev_listen_cb)(void *udata, ccev_sock_t *client,
                                const char *ip, int port);

/* Connect completion callback */
typedef void (*ccev_connect_cb)(void *udata, ccev_sock_t *sock, int status);

/* Stream read completion */
typedef void (*ccev_stream_cb)(void *udata, const char *data,
                                size_t len, int status);

/* Timer expiry */
typedef void (*ccev_timer_cb)(void *udata);

/* DNS resolution */
typedef void (*ccev_dns_cb)(void *udata, const char *address, int status);

/* ICMP echo result */
typedef void (*ccev_icmp_cb)(void *udata, const ccev_icmp_result_t *result);

/* Signal handler */
typedef void (*ccev_signal_cb)(void *udata, int signum);
```

## Function Reference

### Memory Allocator

#### `ccev_set_allocator`

```c
void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*));
```

Replace the internal memory allocator. Must be called before
`ccev_loop_create()`. Defaults to libc `realloc`/`free`.

### Event Loop

#### `ccev_loop_create`

```c
ccev_loop_t *ccev_loop_create(int max_events);
```

Create a new event-loop instance. `max_events` limits the number of events
returned per `epoll_wait()` iteration. Returns `NULL` on failure.

#### `ccev_default_loop`

```c
ccev_loop_t *ccev_default_loop(void);
```

Get the default event-loop singleton (created on first call). Only this
loop may register signal handlers. Do NOT call `ccev_loop_destroy()` on
the returned pointer.

#### `ccev_loop_destroy`

```c
void ccev_loop_destroy(ccev_loop_t *loop);
```

Destroy the loop: closes all sockets, timers, DNS cache, and frees
resources. NULL-safe.

#### `ccev_loop_stop`

```c
void ccev_loop_stop(ccev_loop_t *loop);
```

Request the loop to stop. Thread-safe. Wakes the loop via internal pipe.
The loop exits after the current iteration completes.

#### `ccev_loop_run`

```c
int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode);
```

Run the event loop. In `CCEV_RUN_FOREVER` mode, does not return until
`ccev_loop_stop()` is called. In `CCEV_RUN_ONCE` mode, returns the number
of events processed.

**Dispatch order per iteration:**
1. `ccev__now_ms()` — capture current time
2. `ccev__timer_process()` — extract all expired timers from heap into
   local list, fire callbacks (never interleaved with heap ops), return
   ms until next future timer (-1 if none)
3. Compute epoll timeout from `next_ms`
4. `epoll_wait()`
5. Dispatch events — listeners (batch accept up to 128), connecting
   (`ccsocket_is_connected`), HUP (mode-based: connect_cb/rcb+close),
   EPOLLIN/EPOLLOUT (fire callbacks)
6. Re-arm wake pipe
7. Process closing queue (fire close_cb, free sockets)

### Low-level Socket API (ccev_sock_t)

#### `ccev_sock_create`

```c
ccev_sock_t *ccev_sock_create(ccev_loop_t *loop, ccsocket_t fd, void *udata);
```

Register an external file descriptor with the reactor. The fd is set to
non-blocking and close-on-exec. Returns `NULL` on failure.

#### `ccev_listen`

```c
ccev_sock_t *ccev_listen(ccev_loop_t *loop, const char *addr, uint16_t port,
                           int backlog, ccev_flag_t flags,
                           ccev_listen_cb cb, void *udata);
```

Start listening for TCP connections (or Unix domain sockets with
`port=0`). Returns a listener socket that can be closed with
`ccev_sock_close()` to stop listening. Returns `NULL` on failure.

**Batch accept:** When EPOLLIN fires, up to 128 connections are accepted
in a single dispatch before re-arming, reducing `epoll_ctl` overhead.

#### `ccev_connect`

```c
ccev_sock_t *ccev_connect(ccev_loop_t *loop, const char *host, uint16_t port,
                            unsigned int timeout_ms, ccev_flag_t flags,
                            ccev_connect_cb cb, void *udata);
```

Initiate an asynchronous TCP connection (or UDS with `port=0`).

For hostnames, DNS is resolved internally — the total timeout spans both
DNS and TCP connect phases. The returned socket has `fd == -1` until
DNS+connect completes; do NOT attempt I/O on it before the callback fires.

Returns `NULL` on immediate failure (OOM). On success, the `cb` fires
later with status `CCEV_OK` or `CCEV_ERR`.

#### `ccev_sock_read_start`

```c
int ccev_sock_read_start(ccev_sock_t *sock, ccev_event_cb cb);
```

Arm read events. If `cb` is non-NULL, it replaces the current read
callback. Returns `CCEV_OK` or `CCEV_ERR` (closed).

#### `ccev_sock_read_stop`

```c
int ccev_sock_read_stop(ccev_sock_t *sock);
```

Disarm read events. Sets the read callback to NULL. Returns `CCEV_OK` or
`CCEV_ERR`.

#### `ccev_sock_write_start`

```c
int ccev_sock_write_start(ccev_sock_t *sock, ccev_event_cb cb);
```

Arm write events. Most users should use `ccev_stream_write()` instead
— this low-level API is for custom protocol implementations.

#### `ccev_sock_write_stop`

```c
int ccev_sock_write_stop(ccev_sock_t *sock);
```

Disarm write events.

#### `ccev_sock_set_close_cb`

```c
void ccev_sock_set_close_cb(ccev_sock_t *sock, ccev_close_cb cb, void *udata);
```

Set the close/error callback. Fires when the peer closes, an I/O error
occurs, or `ccev_sock_close()` is called.

#### `ccev_sock_close`

```c
int ccev_sock_close(ccev_sock_t *sock);
```

Close and release a socket. Removes from epoll, schedules deferred
close. The close_cb (if set) fires before memory is freed. Returns
`CCEV_OK` or `CCEV_ERR` (already closed / NULL).

#### `ccev_sock_get_fd`

```c
ccsocket_t ccev_sock_get_fd(const ccev_sock_t *sock);
```

Get the underlying file descriptor. Returns `(ccsocket_t)-1` for NULL
or connecting sockets (DNS in progress).

#### `ccev_sock_get_udata` / `ccev_sock_set_udata`

```c
void  *ccev_sock_get_udata(const ccev_sock_t *sock);
void   ccev_sock_set_udata(ccev_sock_t *sock, void *udata);
```

Get/set the user-data pointer.

#### `ccev_sock_count`

```c
int ccev_sock_count(ccev_loop_t *loop);
```

Get the number of active sockets (including the internal wake socket).
Returns 0 for NULL loop.

### High-level Stream API (ccev_stream_t)

#### `ccev_stream_open`

```c
ccev_stream_t *ccev_stream_open(ccev_sock_t *sock);
```

Upgrade a `ccev_sock_t` to a `ccev_stream_t`. Internally this is a
`realloc` — the returned stream address may differ from the sock address;
the library updates epoll's `data.ptr` automatically.

After this call, use the stream pointer; the original sock pointer is
still valid (same address if realloc didn't move it) but should be
treated as a stream. The stream takes over the sock's event callbacks
to drive buffered I/O and the stream reader.

Returns `NULL` on failure (OOM / closed sock).

#### `ccev_stream_close`

```c
int ccev_stream_close(ccev_stream_t *st);
```

Close a stream. Flushes pending write data, frees write buffers and
the stream reader, then closes the underlying socket. Returns `CCEV_OK`
or `CCEV_ERR` (already closed).

### Stream Write Operations

#### `ccev_stream_write`

```c
int ccev_stream_write(ccev_stream_t *st, const void *data, size_t len,
                       ccev_send_cb cb, void *udata);
```

Write data asynchronously. Data is appended to an internal write buffer
and sent non-blocking. If the kernel buffer is full, the remainder is
queued and EPOLLOUT is armed automatically.

Each write's per-buffer `cb` fires independently when that specific
buffer chunk has been flushed. Returns `len` (always accepted) or
`CCEV_ERR` (OOM / closed).

#### `ccev_stream_write_batch`

```c
int ccev_stream_write_batch(ccev_stream_t *st, const void *data, size_t len,
                             bool done, ccev_send_cb cb, void *udata);
```

Batch write with explicit flush control:

- `done=false`: buffer only (accumulate multiple chunks)
- `done=true`: buffer then trigger send

Typical usage:
```c
ccev_stream_write_batch(st, header, hlen, false, NULL, NULL);
ccev_stream_write_batch(st, body,   blen, false, NULL, NULL);
ccev_stream_write_batch(st, NULL,      0, true,  NULL, NULL);  /* flush */
```

#### `ccev_stream_flush`

```c
int ccev_stream_flush(ccev_stream_t *st);
```

Flush the write buffer — send all queued data. Equivalent to
`ccev_stream_write_batch(st, NULL, 0, true, NULL, NULL)`.

#### `ccev_stream_sendfile`

```c
int ccev_stream_sendfile(ccev_stream_t *st, const char *path,
                          ccev_send_cb cb, void *udata);
```

Send a file using kernel sendfile (zero-copy on Linux/macOS/FreeBSD).
Opens the file, sends its entire content, then closes it. Falls back
to read+send on platforms without kernel sendfile.

#### `ccev_stream_set_send_cb`

```c
void ccev_stream_set_send_cb(ccev_stream_t *st, ccev_send_cb cb, void *udata);
```

Set the global write-drain callback. Fires once when the entire write
buffer has been flushed (wbuf_len reaches 0). Unlike per-buffer
callbacks (`ccev_stream_write`'s `cb` parameter), this is a single
notification independent of how many writes were batched.

#### `ccev_stream_wbuf_len`

```c
size_t ccev_stream_wbuf_len(const ccev_stream_t *st);
```

Get the number of bytes pending in the write buffer. Returns 0 for
NULL stream.

### Stream Read Operations

#### `ccev_stream_cb`

```c
typedef void (*ccev_stream_cb)(void *udata, const char *data,
                                size_t len, int status);
```

Completion callback for `ccev_stream_readline()` and
`ccev_stream_readnum()`. `data` is valid only during the callback
— do NOT free or store the pointer.

| `status` | Meaning |
|---|---|
| `CCEV_OK` | Delimiter found (readline) or N bytes collected (readnum). |
| `CCEV_ERR` | `maxlen` exceeded (readline) or connection closed / timeout. |

#### `ccev_stream_readline`

```c
int ccev_stream_readline(ccev_stream_t *st, char delim, size_t maxlen,
                          int timeout_ms, ccev_stream_cb cb, void *udata);
```

Read until `delim` is found (delimiter inclusive). Fires exactly once,
then deactivates — call again for subsequent reads.

If `timeout_ms > 0`, the callback fires with `CCEV_ERR` if no data
arrives within the deadline. Only one stream reader may be active at
a time — calling this while another reader is active cancels the
previous one.

#### `ccev_stream_readnum`

```c
int ccev_stream_readnum(ccev_stream_t *st, size_t n,
                          int timeout_ms, ccev_stream_cb cb, void *udata);
```

Read exactly `n` bytes. Same semantics as `ccev_stream_readline()`.

#### `ccev_stream_read_stop`

```c
void ccev_stream_read_stop(ccev_stream_t *st);
```

Cancel the active stream reader, if any. The user callback will NOT
be fired. Safe to call when no reader is active.

#### `ccev_stream_set_close_cb`

```c
void ccev_stream_set_close_cb(ccev_stream_t *st, ccev_close_cb cb, void *udata);
```

Set the close callback on a stream. Delegates to
`ccev_sock_set_close_cb()` internally.

### Timer

#### `ccev_timer_add`

```c
ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata);
```

Add a timer. Internal D-ary heap (4-ary, O(log n)). REPEAT timers are
automatically re-inserted after firing. Returns NULL on failure.

#### `ccev_timer_del`

```c
int ccev_timer_del(ccev_loop_t *loop, ccev_timer_t *timer);
```

Delete a timer (lazy — marked inactive, freed when the heap naturally
reaches it). Safe to call from inside the timer's own callback.

#### `ccev_timer_reset`

```c
int ccev_timer_reset(ccev_loop_t *loop, ccev_timer_t *timer,
                      uint64_t delay_ms);
```

Re-schedule a timer to now + delay_ms. Uses `ccheap_update()` with
embedded index for O(log n) repositioning.

#### `ccev_timer_count`

```c
int ccev_timer_count(ccev_loop_t *loop);
```

Get the number of active timers.

### DNS

#### `ccev_dns_set_server`

```c
int ccev_dns_set_server(ccev_loop_t *loop, const char *address, uint16_t port);
```

Set the DNS server address. The server IP string must be a valid IPv4
or IPv6 address (hostnames are not accepted).  IPv6 addresses are
verified by creating a test socket — if the system does not support
IPv6 UDP sockets the call returns `CCEV_ERR`.  Port 0 defaults to 53.

Default: reads the first reachable nameserver from `/etc/resolv.conf`
(POSIX), falls back to `"1.1.1.1"`, port 53.

#### `ccev_dns_resolve`

```c
int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata);
```

Resolve a domain asynchronously. If `domain` is already an IP or UDS
path, the callback fires immediately. Cache is checked before issuing
network queries.

#### `ccev_dns_flush`

```c
void ccev_dns_flush(ccev_loop_t *loop);
```

Flush DNS cache and reload from the OS hosts file. Automatically
called during `ccev_default_loop()` initialization.

### Utilities

```c
int ccev_wakeup(ccev_loop_t *loop);       /* Wake the loop from another thread. */
```

### Socket count (declared in ccev_sock.c)

```c
int ccev_sock_count(ccev_loop_t *loop);   /* Active sockets (incl. wake sock). */
```

### Timer count (declared in ccev_timer.c)

```c
int ccev_timer_count(ccev_loop_t *loop);  /* Active timers. */
```

### ICMP Echo

#### `ccev_icmp_result_t`

```c
struct ccev_icmp_result_s {
    char     ip[64];         /* Target IP. */
    double   rtt_ms;         /* Round-trip time in ms. */
    size_t   payload_len;    /* Payload bytes echoed back. */
    int      ttl;            /* TTL from response. */
};
```

#### `ccev_icmp_echo`

```c
int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    unsigned int timeout_ms,
                    ccev_icmp_cb cb, void *udata);
```

Send an ICMP echo request (ping). Hostnames are resolved via the
internal async DNS resolver. Requires `CAP_NET_RAW` / root on most
systems; some kernels (Linux 3.0+, macOS) support a privilege-free
`SOCK_DGRAM+ICMP` path.

### Signal Handling

#### `ccev_signal_handle`

```c
int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata);
```

Register a signal handler on the default loop. The handler fires inside
`ccev_loop_run()` via the self-pipe trick. Only one handler per signum
— a second call overwrites the previous one. Returns `CCEV_ERR` if
`signum` is out of range.

#### `ccev_signal_ignore`

```c
int ccev_signal_ignore(int signum);
```

Restore a signal to its default disposition (`SIG_DFL`).
