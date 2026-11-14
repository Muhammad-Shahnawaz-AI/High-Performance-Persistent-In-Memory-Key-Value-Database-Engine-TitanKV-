#include "eventloop.h"
#include "allocator.h"
#include "avl.h"
#include "hashtable.h"
#include "minheap.h"
#include "protocol.h"
#include "wal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define USE_KQUEUE 1
#else
#error "Unsupported POSIX platform: requires epoll or kqueue"
#endif

#define MAX_EVENTS 1024
#define INIT_BUF_CAP 8192
#define ACTIVE_EXPIRE_BATCH 16

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct client_conn {
    int fd;
    uint8_t *rbuf;
    size_t rcap;
    size_t rlen;

    uint8_t *wbuf;
    size_t wcap;
    size_t wlen;
    size_t wsent;
} client_conn_t;

typedef struct {
    dict_t dict;
    minheap_t ttl_heap;
    wal_t wal;
    int loop_fd;
    int listen_fd;
} server_state_t;

static void free_entry(kv_entry_t *e) {
    if (!e) return;
    tkv_free(e->key, e->key_len);
    if (e->type == VAL_TYPE_STRING) {
        tkv_free(e->val.str.data, e->val.str.len);
    } else if (e->type == VAL_TYPE_ZSET) {
        zset_destroy(e->val.zset);
    }
    tkv_free(e, sizeof(kv_entry_t));
}

static void conn_append_write(client_conn_t *c, const void *data, size_t len) {
    if (c->wlen + len > c->wcap) {
        size_t new_cap = c->wcap * 2;
        while (new_cap < c->wlen + len) new_cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(c->wbuf, new_cap);
        if (!nb) return;
        c->wbuf = nb;
        c->wcap = new_cap;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
}

static void conn_send_resp(client_conn_t *c, uint8_t status, const void *payload, uint32_t len) {
    uint8_t hdr[PROTO_RESP_HEADER_LEN];
    proto_encode_response_header(hdr, status, len);
    conn_append_write(c, hdr, sizeof(hdr));
    if (len > 0 && payload) {
        conn_append_write(c, payload, len);
    }
}

static bool check_and_evict_if_expired(server_state_t *srv, kv_entry_t *e, uint64_t current_ms) {
    if (e->expire_at_ms > 0 && e->expire_at_ms <= current_ms) {
        minheap_remove(&srv->ttl_heap, e);
        wal_append(&srv->wal, OP_DEL, 0, e->key, e->key_len, NULL, 0);
        dict_unlink(&srv->dict, e->key, e->key_len);
        free_entry(e);
        return true;
    }
    return false;
}

static void active_expire_tick(server_state_t *srv, uint64_t current_ms) {
    int evicted = 0;
    while (evicted < ACTIVE_EXPIRE_BATCH) {
        kv_entry_t *top = minheap_peek(&srv->ttl_heap);
        if (!top || top->expire_at_ms > current_ms) break;
        check_and_evict_if_expired(srv, top, current_ms);
        evicted++;
    }
}

/* ZRANGE response serializer */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint32_t count;
} zrange_buf_t;

static void zrange_collect_cb(const char *member, uint32_t len, double score, void *ctx) {
    zrange_buf_t *zb = (zrange_buf_t *)ctx;
    size_t needed = sizeof(uint32_t) + len + sizeof(double);
    if (zb->len + needed > zb->cap) {
        zb->cap = (zb->cap + needed) * 2;
        zb->buf = (uint8_t *)realloc(zb->buf, zb->cap);
    }
    uint32_t net_mlen = htonl(len);
    memcpy(zb->buf + zb->len, &net_mlen, sizeof(uint32_t));
    zb->len += sizeof(uint32_t);
    memcpy(zb->buf + zb->len, member, len);
    zb->len += len;
    memcpy(zb->buf + zb->len, &score, sizeof(double));
    zb->len += sizeof(double);
    zb->count++;
}

static void execute_command(server_state_t *srv, client_conn_t *c, const proto_request_t *req) {
    uint64_t current_ms = now_ms();

    switch (req->opcode) {
        case OP_PING: {
            conn_send_resp(c, STATUS_OK, "PONG", 4);
            break;
        }
        case OP_GET: {
            kv_entry_t *e = dict_get(&srv->dict, (const char *)req->key, req->key_len);
            if (!e || check_and_evict_if_expired(srv, e, current_ms)) {
                conn_send_resp(c, STATUS_NOT_FOUND, NULL, 0);
                break;
            }
            if (e->type != VAL_TYPE_STRING) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }
            conn_send_resp(c, STATUS_OK, e->val.str.data, e->val.str.len);
            break;
        }
        case OP_SET: {
            size_t est_bytes = sizeof(kv_entry_t) + req->key_len + req->val_len;
            if (allocator_is_oom(est_bytes)) {
                conn_send_resp(c, STATUS_ERR_OOM, NULL, 0);
                break;
            }
            if (!wal_append(&srv->wal, OP_SET, 0, req->key, req->key_len, req->val, req->val_len)) {
                conn_send_resp(c, STATUS_ERR_INTERNAL, NULL, 0);
                break;
            }

            kv_entry_t *old = dict_unlink(&srv->dict, (const char *)req->key, req->key_len);
            if (old) {
                minheap_remove(&srv->ttl_heap, old);
                free_entry(old);
            }

            kv_entry_t *e = (kv_entry_t *)tkv_malloc(sizeof(kv_entry_t));
            e->key = (char *)tkv_malloc(req->key_len);
            memcpy(e->key, req->key, req->key_len);
            e->key_len = req->key_len;
            e->type = VAL_TYPE_STRING;
            e->val.str.data = (uint8_t *)tkv_malloc(req->val_len > 0 ? req->val_len : 1);
            if (req->val_len > 0) memcpy(e->val.str.data, req->val, req->val_len);
            e->val.str.len = req->val_len;
            e->expire_at_ms = 0;
            e->heap_idx = -1;

            dict_set(&srv->dict, e);
            wal_maybe_compact(&srv->wal, &srv->dict);
            conn_send_resp(c, STATUS_OK, NULL, 0);
            break;
        }
        case OP_DEL: {
            kv_entry_t *e = dict_unlink(&srv->dict, (const char *)req->key, req->key_len);
            if (!e) {
                conn_send_resp(c, STATUS_NOT_FOUND, NULL, 0);
                break;
            }
            minheap_remove(&srv->ttl_heap, e);
            wal_append(&srv->wal, OP_DEL, 0, req->key, req->key_len, NULL, 0);
            free_entry(e);
            wal_maybe_compact(&srv->wal, &srv->dict);
            conn_send_resp(c, STATUS_OK, NULL, 0);
            break;
        }
        case OP_EXPIRE: {
            if (req->val_len != sizeof(uint64_t)) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }
            kv_entry_t *e = dict_get(&srv->dict, (const char *)req->key, req->key_len);
            if (!e || check_and_evict_if_expired(srv, e, current_ms)) {
                conn_send_resp(c, STATUS_NOT_FOUND, NULL, 0);
                break;
            }
            uint64_t net_secs;
            memcpy(&net_secs, req->val, sizeof(uint64_t));
            /* Convert big-endian 64-bit seconds to host */
            uint64_t secs = ((uint64_t)ntohl((uint32_t)(net_secs >> 32))) |
                            (((uint64_t)ntohl((uint32_t)(net_secs & 0xFFFFFFFFULL))) << 32);
            e->expire_at_ms = current_ms + secs * 1000ULL;
            minheap_update(&srv->ttl_heap, e);

            if (e->type == VAL_TYPE_STRING) {
                wal_append(&srv->wal, OP_SET, e->expire_at_ms, e->key, e->key_len, e->val.str.data, e->val.str.len);
            }
            conn_send_resp(c, STATUS_OK, NULL, 0);
            break;
        }
        case OP_ZADD: {
            /* Payload layout: [8B IEEE-754 double score][member bytes] */
            if (req->val_len <= sizeof(double)) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }
            if (allocator_is_oom(sizeof(avl_node_t) + req->val_len)) {
                conn_send_resp(c, STATUS_ERR_OOM, NULL, 0);
                break;
            }

            kv_entry_t *e = dict_get(&srv->dict, (const char *)req->key, req->key_len);
            if (e && check_and_evict_if_expired(srv, e, current_ms)) e = NULL;

            if (!e) {
                e = (kv_entry_t *)tkv_malloc(sizeof(kv_entry_t));
                e->key = (char *)tkv_malloc(req->key_len);
                memcpy(e->key, req->key, req->key_len);
                e->key_len = req->key_len;
                e->type = VAL_TYPE_ZSET;
                e->val.zset = zset_create();
                e->expire_at_ms = 0;
                e->heap_idx = -1;
                dict_set(&srv->dict, e);
            } else if (e->type != VAL_TYPE_ZSET) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }

            double score;
            memcpy(&score, req->val, sizeof(double));
            const char *member = (const char *)(req->val + sizeof(double));
            uint32_t mlen = req->val_len - (uint32_t)sizeof(double);

            wal_append(&srv->wal, OP_ZADD, e->expire_at_ms, req->key, req->key_len, req->val, req->val_len);
            zset_add(e->val.zset, score, member, mlen);
            wal_maybe_compact(&srv->wal, &srv->dict);
            conn_send_resp(c, STATUS_OK, NULL, 0);
            break;
        }
        case OP_ZRANGE: {
            /* Payload layout: [4B big-endian int32 start][4B big-endian int32 stop] */
            if (req->val_len != 8) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }
            kv_entry_t *e = dict_get(&srv->dict, (const char *)req->key, req->key_len);
            if (!e || check_and_evict_if_expired(srv, e, current_ms)) {
                conn_send_resp(c, STATUS_NOT_FOUND, NULL, 0);
                break;
            }
            if (e->type != VAL_TYPE_ZSET) {
                conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
                break;
            }

            uint32_t nstart, nstop;
            memcpy(&nstart, req->val, 4);
            memcpy(&nstop, req->val + 4, 4);
            int32_t start = (int32_t)ntohl(nstart);
            int32_t stop = (int32_t)ntohl(nstop);

            zrange_buf_t zb = {.buf = (uint8_t *)malloc(256), .len = 4, .cap = 256, .count = 0};
            zset_range(e->val.zset, start, stop, zrange_collect_cb, &zb);
            uint32_t net_cnt = htonl(zb.count);
            memcpy(zb.buf, &net_cnt, 4);

            conn_send_resp(c, STATUS_OK, zb.buf, (uint32_t)zb.len);
            free(zb.buf);
            break;
        }
        default:
            conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
            break;
    }
}

static void close_client(server_state_t *srv, client_conn_t *c) {
#if defined(USE_EPOLL)
    epoll_ctl(srv->loop_fd, EPOLL_CTL_DEL, c->fd, NULL);
#elif defined(USE_KQUEUE)
    struct kevent ev[2];
    EV_SET(&ev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(srv->loop_fd, ev, 2, NULL, 0, NULL);
#endif
    close(c->fd);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

static void update_client_events(server_state_t *srv, client_conn_t *c) {
    bool want_write = (c->wsent < c->wlen);
#if defined(USE_EPOLL)
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | (want_write ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(srv->loop_fd, EPOLL_CTL_MOD, c->fd, &ev);
#elif defined(USE_KQUEUE)
    struct kevent ev[2];
    EV_SET(&ev[0], c->fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, c);
    EV_SET(&ev[1], c->fd, EVFILT_WRITE, want_write ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, c);
    kevent(srv->loop_fd, ev, 2, NULL, 0, NULL);
#endif
}

static void handle_client_read(server_state_t *srv, client_conn_t *c) {
    while (true) {
        if (c->rlen == c->rcap) {
            size_t new_cap = c->rcap * 2;
            if (new_cap > TITANKV_MAX_VAL_LEN + 65536) {
                close_client(srv, c);
                return;
            }
            uint8_t *nb = (uint8_t *)realloc(c->rbuf, new_cap);
            if (!nb) {
                close_client(srv, c);
                return;
            }
            c->rbuf = nb;
            c->rcap = new_cap;
        }

        ssize_t n = read(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
        if (n > 0) {
            c->rlen += (size_t)n;
        } else if (n == 0) {
            close_client(srv, c);
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(srv, c);
            return;
        }
    }

    /* Process all pipelined TLV frames in rbuf */
    size_t offset = 0;
    while (offset < c->rlen) {
        proto_request_t req;
        int consumed = proto_parse_frame(c->rbuf + offset, c->rlen - offset, &req);
        if (consumed == 0) break; /* Need more bytes */
        if (consumed < 0) {
            conn_send_resp(c, STATUS_ERR_SYNTAX, NULL, 0);
            update_client_events(srv, c);
            close_client(srv, c);
            return;
        }
        execute_command(srv, c, &req);
        offset += (size_t)consumed;
    }

    if (offset > 0) {
        if (offset < c->rlen) {
            memmove(c->rbuf, c->rbuf + offset, c->rlen - offset);
        }
        c->rlen -= offset;
    }

    update_client_events(srv, c);
}

static void handle_client_write(server_state_t *srv, client_conn_t *c) {
    while (c->wsent < c->wlen) {
        ssize_t n = write(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent);
        if (n > 0) {
            c->wsent += (size_t)n;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(srv, c);
            return;
        }
    }

    if (c->wsent == c->wlen) {
        c->wlen = 0;
        c->wsent = 0;
        update_client_events(srv, c);
    }
}

static void accept_clients(server_state_t *srv) {
    while (true) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int cfd = accept(srv->listen_fd, (struct sockaddr *)&addr, &alen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        set_nonblocking(cfd);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client_conn_t *c = (client_conn_t *)calloc(1, sizeof(client_conn_t));
        c->fd = cfd;
        c->rcap = INIT_BUF_CAP;
        c->rbuf = (uint8_t *)malloc(c->rcap);
        c->wcap = INIT_BUF_CAP;
        c->wbuf = (uint8_t *)malloc(c->wcap);

#if defined(USE_EPOLL)
        struct epoll_event ev = {0};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        epoll_ctl(srv->loop_fd, EPOLL_CTL_ADD, cfd, &ev);
#elif defined(USE_KQUEUE)
        struct kevent ev;
        EV_SET(&ev, cfd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, c);
        kevent(srv->loop_fd, &ev, 1, NULL, 0, NULL);
#endif
    }
}

int run_server(const server_config_t *cfg) {
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    allocator_init(cfg->max_memory_bytes);

    server_state_t srv = {0};
    dict_init(&srv.dict, 1024);
    minheap_init(&srv.ttl_heap, 256);

    if (!wal_init(&srv.wal, cfg->data_dir, cfg->fsync_policy, cfg->wal_compact_threshold)) {
        fprintf(stderr, "Failed to initialize WAL in %s\n", cfg->data_dir);
        return 1;
    }

    wal_recover(&srv.wal, &srv.dict, &srv.ttl_heap);
    printf("[TitanKV] Recovered %zu keys from WAL.\n", dict_size(&srv.dict));

    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(srv.listen_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cfg->port);

    if (bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv.listen_fd, 1024) < 0) {
        perror("listen");
        return 1;
    }

#if defined(USE_EPOLL)
    srv.loop_fd = epoll_create1(0);
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* NULL signifies listen_fd */
    epoll_ctl(srv.loop_fd, EPOLL_CTL_ADD, srv.listen_fd, &ev);
    struct epoll_event events[MAX_EVENTS];
#elif defined(USE_KQUEUE)
    srv.loop_fd = kqueue();
    struct kevent ev;
    EV_SET(&ev, srv.listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(srv.loop_fd, &ev, 1, NULL, 0, NULL);
    struct kevent events[MAX_EVENTS];
#endif

    printf("[TitanKV] Listening on 0.0.0.0:%u (policy=%d, max_mem=%zuMB)\n",
           cfg->port, cfg->fsync_policy, cfg->max_memory_bytes / (1024 * 1024));

    while (g_running) {
#if defined(USE_EPOLL)
        int n = epoll_wait(srv.loop_fd, events, MAX_EVENTS, 10); /* 10ms timer tick */
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                accept_clients(&srv);
            } else {
                client_conn_t *c = (client_conn_t *)events[i].data.ptr;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    close_client(&srv, c);
                } else {
                    if (events[i].events & EPOLLIN) handle_client_read(&srv, c);
                    if (events[i].events & EPOLLOUT) handle_client_write(&srv, c);
                }
            }
        }
#elif defined(USE_KQUEUE)
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L}; /* 10ms */
        int n = kevent(srv.loop_fd, NULL, 0, events, MAX_EVENTS, &ts);
        for (int i = 0; i < n; i++) {
            if (events[i].udata == NULL) {
                accept_clients(&srv);
            } else {
                client_conn_t *c = (client_conn_t *)events[i].udata;
                if (events[i].flags & EV_EOF) {
                    close_client(&srv, c);
                } else if (events[i].filter == EVFILT_READ) {
                    handle_client_read(&srv, c);
                } else if (events[i].filter == EVFILT_WRITE) {
                    handle_client_write(&srv, c);
                }
            }
        }
#endif
        uint64_t t = now_ms();
        active_expire_tick(&srv, t);
        wal_tick_fsync(&srv.wal, t);
    }

    printf("\n[TitanKV] Shutting down gracefully...\n");
    while (srv.wal.compacting) {
        usleep(1000);
    }
    close(srv.listen_fd);
    close(srv.loop_fd);
    wal_destroy(&srv.wal);
    minheap_destroy(&srv.ttl_heap);
    dict_destroy(&srv.dict, free_entry);
    allocator_destroy();
    return 0;
}