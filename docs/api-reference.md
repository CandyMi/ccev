# ccev API Reference

## Macros

| Macro | Value | Description |
|---|---|---|
| `CCEV_OK` | `0` | Operation completed successfully. |
| `CCEV_ERR` | `-1` | Operation failed / fd is closed. |

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
    CCEV_REUSEADDR   = 1u << 8,   /* SO_REUSEADDR */
    CCEV_REUSEPORT   = 1u << 9,   /* SO_REUSEPORT */
    CCEV_TCP_NODELAY = 1u << 10,  /* TCP_NODELAY  */
    CCEV_UDP         = 1u << 12,  /* Use UDP (datagram) in ccev_connect() */
};
```

## Types

```c
typedef struct ccev_loop_s  ccev_loop_t;   /* Event-loop handle. */
typedef struct ccev_conn_s  ccev_conn_t;   /* Connection / I/O handle. */
typedef struct ccev_timer_s ccev_timer_t;  /* Timer handle. */

typedef struct ccev_address_s {
    char                    ip[64];        /* Human-readable IP. */
    int                     ttl;           /* DNS TTL in seconds. */
    struct ccev_address_s  *next;          /* Next address (linked list). */
} ccev_address_t;
```

## Callbacks

```c
typedef void (*ccev_recv_cb)(void *udata);    /* Readable notification. */
typedef void (*ccev_send_cb)(void *udata);    /* Write-buffer flushed. */
typedef void (*ccev_close_cb)(void *udata);   /* Connection closed/errored. */
typedef void (*ccev_timer_cb)(void *udata);   /* Timer expired. */
typedef void (*ccev_accept_cb)(void *udata, ccev_conn_t *conn,
                                const char *ip, int port);
typedef void (*ccev_connect_cb)(void *udata, ccev_conn_t *conn, int status);
typedef void (*ccev_dns_cb)(void *udata, ccev_address_t *addr, int status);
```

## Function Reference

### Memory Allocator

#### `ccev_set_allocator`

```c
void ccev_set_allocator(void *(*realloc_fn)(void*, size_t),
                        void  (*free_fn)(void*));
```

Replace the internal memory allocator. Must be called before `ccev_loop_create()`. Defaults to libc `realloc`/`free`.

### Event Loop

#### `ccev_loop_create`

```c
ccev_loop_t *ccev_loop_create(int max_events);
```

Create a new event-loop instance. `max_events` limits the number of events returned per `epoll_wait()` iteration. Returns `NULL` on failure.

#### `ccev_loop_destroy`

```c
void ccev_loop_destroy(ccev_loop_t *loop);
```

Destroy the loop, closing all connections, timers, and freeing resources.

#### `ccev_loop_stop`

```c
void ccev_loop_stop(ccev_loop_t *loop);
```

Request the loop to stop. Thread-safe. The loop exits after the current iteration.

#### `ccev_loop_run`

```c
int ccev_loop_run(ccev_loop_t *loop, ccev_run_mode_t mode);
```

Run the event loop. In `CCEV_RUN_FOREVER` mode, does not return until `ccev_loop_stop()` is called. In `CCEV_RUN_ONCE` mode, returns the number of events processed.

### TCP Listener

#### `ccev_listen`

```c
ccev_conn_t *ccev_listen(ccev_loop_t *loop, const char *addr, uint16_t port,
                          int backlog, ccev_flag_t flags,
                          ccev_accept_cb on_accept, void *udata);
```

Start listening for connections. Supports TCP, UDP (`CCEV_UDP` flag), and Unix domain sockets (`port=0`). Returns a listener handle that can be closed with `ccev_conn_close()`. Returns `NULL` on failure.

### Async Connection

#### `ccev_connect`

```c
int ccev_connect(ccev_loop_t *loop, const char *addr, uint16_t port,
                 unsigned int timeout_ms, ccev_flag_t flags,
                 ccev_connect_cb on_connect, void *udata);
```

Initiate an asynchronous TCP or UDP connection. The `on_connect` callback fires on completion. Returns `CCEV_OK` on successful initiation, `CCEV_ERR` on error.

### External fd

#### `ccev_conn_create`

```c
ccev_conn_t *ccev_conn_create(ccev_loop_t *loop, ccsocket_t fd, void *udata);
```

Register an external file descriptor with the reactor. The caller is responsible for creating and configuring the fd.

### Connection I/O

#### `ccev_conn_recv`

```c
int ccev_conn_recv(ccev_conn_t *conn, void *buf, size_t len,
                    ccev_recv_cb cb, void *udata);
```

Read data or register a readable callback.

**Behaviours:**
- `(buf!=NULL, len>0, cb!=NULL)`: Attempts a speculative non-blocking read. If data is available, `cb` is called synchronously. Returns bytes read, or `1` on EAGAIN (epoll re-armed, retry later).
- `(buf!=NULL, len>0, cb=NULL)`: Pure synchronous read. Returns bytes read (>0), `0` on EOF, `1` on EAGAIN, or `CCEV_ERR(-1)` on error.
- `(buf=NULL, len=0, cb!=NULL)`: Register callback and arm read events.
- `(cb=NULL, udata=NULL)`: Clear callback and disarm.

#### `ccev_conn_send`

```c
int ccev_conn_send(ccev_conn_t *conn, const void *data, size_t len,
                    ccev_send_cb cb, void *udata);
```

Send data asynchronously. On EAGAIN the data is buffered internally and `EPOLLOUT` is armed. The `cb` fires when the buffer is fully flushed. On immediate success, `cb` fires synchronously.

#### `ccev_conn_sendfile`

```c
int ccev_conn_sendfile(ccev_conn_t *conn, int fd, ccev_send_cb cb, void *udata);
```

Send an open file descriptor via kernel sendfile (zero-copy on Linux/macOS/FreeBSD). Falls back to read+send on other platforms.

#### `ccev_conn_sendall`

```c
int ccev_conn_sendall(ccev_conn_t *conn, const void *data, size_t len,
                       bool done, ccev_send_cb cb, void *udata);
```

Send with batching control. When `done=false`, data is buffered without an immediate flush attempt. When `done=true`, the accumulated buffer is flushed. Only the last non-NULL `cb` across a batch fires.

#### `ccev_conn_close`

```c
int ccev_conn_close(ccev_conn_t *conn);
```

Shut down and close a connection. Triggers the close callback. After close, all I/O operations return `CCEV_ERR`.

#### `ccev_conn_set_close_cb`

```c
void ccev_conn_set_close_cb(ccev_conn_t *conn, ccev_close_cb cb, void *udata);
```

Set the close/error callback. Fired when the peer closes or an I/O error occurs.

### Connection Metadata

```c
void  *ccev_conn_get_udata(ccev_conn_t *conn);
void   ccev_conn_set_udata(ccev_conn_t *conn, void *udata);
ccsocket_t ccev_conn_fd(ccev_conn_t *conn);
```

### Timer

#### `ccev_timer_add`

```c
ccev_timer_t *ccev_timer_add(ccev_loop_t *loop, uint64_t delay_ms,
                               ccev_timer_mode_t mode,
                               ccev_timer_cb cb, void *udata);
```

Add a timer. Returns a handle for later modification.

#### `ccev_timer_del`

```c
int ccev_timer_del(ccev_loop_t *loop, ccev_timer_t *timer);
```

Delete a timer (lazy deletion). Safe to call from inside the timer's own callback.

#### `ccev_timer_reset`

```c
int ccev_timer_reset(ccev_loop_t *loop, ccev_timer_t *timer, uint64_t delay_ms);
```

Re-schedule a timer. Uses `ccheap_update()` with embedded index for O(log n) repositioning.

### DNS

#### `ccev_dns_set_server`

```c
int ccev_dns_set_server(ccev_loop_t *loop, const char *servers[], int n, int port);
```

Set DNS server addresses and port. Default servers: `{"1.1.1.1"}`, port: `53`. Max 4 servers. @p port is ignored when <= 0.

#### `ccev_dns_resolve`

```c
int ccev_dns_resolve(ccev_loop_t *loop, const char *domain,
                      unsigned int timeout_ms, ccev_dns_type_t type,
                      ccev_dns_cb cb, void *udata);
```

Resolve a domain name asynchronously. Sends UDP queries to all configured servers simultaneously (race mode — first response wins).

#### `ccev_dns_free`

```c
void ccev_dns_free(ccev_address_t *addr);
```

Free a DNS address list. NULL-safe.

### Utilities

```c
int ccev_wakeup(ccev_loop_t *loop);      /* Wake the loop from another thread. */
int ccev_conn_count(ccev_loop_t *loop);   /* Number of active connections. */
int ccev_timer_count(ccev_loop_t *loop);  /* Number of active timers. */
```

### ICMP Echo

#### `ccev_icmp_result_t` / `ccev_icmp_cb`

```c
typedef struct ccev_icmp_result_s {
    char     ip[64];         /* Target IP. */
    double   rtt_ms;         /* Round-trip time in ms. */
    size_t   payload_len;    /* Payload bytes echoed back. */
    int      ttl;            /* TTL from response. */
} ccev_icmp_result_t;

typedef void (*ccev_icmp_cb)(void *udata, ccev_icmp_result_t *result);
```

#### `ccev_icmp_echo`

```c
int ccev_icmp_echo(ccev_loop_t *loop, const char *host,
                    ccev_icmp_cb cb, void *udata);
```

Send an ICMP echo request (ping). Uses ccicmp internally — tries privilege-free SOCK_DGRAM+ICMP on Linux 3.0+ and macOS, falls back to SOCK_RAW (requires root).

### Signal Handling

#### `ccev_default_loop`

```c
ccev_loop_t *ccev_default_loop(void);
```

Get the default event-loop singleton. Only this loop may register signal handlers. Do NOT call `ccev_loop_destroy()` on the returned pointer.

#### `ccev_signal_cb`

```c
typedef void (*ccev_signal_cb)(void *udata, int signum);
```

Signal delivery callback. `signum` is the raw OS signal number (e.g. `SIGINT`, `SIGTERM`).

#### `ccev_signal_handle`

```c
int ccev_signal_handle(int signum, ccev_signal_cb cb, void *udata);
```

Register a signal handler on the default loop. The handler fires inside `ccev_loop_run()` via the self-pipe trick. Only one handler per signum — a second call overwrites the previous one. Returns `CCEV_ERR` if `signum` is out of range.

#### `ccev_signal_ignore`

```c
int ccev_signal_ignore(int signum);
```

Restore a signal to its default disposition (`SIG_DFL`).
