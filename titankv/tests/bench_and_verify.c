#include "protocol.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int connect_server(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    return fd;
}

static size_t encode_req(uint8_t *dst, uint8_t op, const void *k, uint16_t kl, const void *v, uint32_t vl) {
    dst[0] = PROTO_MAGIC;
    dst[1] = op;
    uint16_t nkl = htons(kl);
    uint32_t nvl = htonl(vl);
    memcpy(dst + 2, &nkl, 2);
    memcpy(dst + 4, &nvl, 4);
    if (kl > 0) memcpy(dst + 8, k, kl);
    if (vl > 0) memcpy(dst + 8 + kl, v, vl);
    return 8 + kl + vl;
}

static void read_exact(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (uint8_t *)buf + got, len - got);
        assert(n > 0);
        got += (size_t)n;
    }
}

static void run_functional_checks(uint16_t port) {
    int fd = connect_server(port);
    uint8_t tx[512], rx[512];

    /* 1. PING */
    size_t n = encode_req(tx, OP_PING, NULL, 0, NULL, 0);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 9);
    assert(rx[0] == STATUS_OK && memcmp(rx + 5, "PONG", 4) == 0);

    /* 2. SET + GET */
    n = encode_req(tx, OP_SET, "user:1", 6, "alice", 5);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 5);
    assert(rx[0] == STATUS_OK);

    n = encode_req(tx, OP_GET, "user:1", 6, NULL, 0);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 10);
    assert(rx[0] == STATUS_OK && memcmp(rx + 5, "alice", 5) == 0);

    /* 3. ZADD + ZRANGE */
    uint8_t zval[32];
    double s1 = 42.5, s2 = 10.0;
    memcpy(zval, &s1, 8);
    memcpy(zval + 8, "bob", 3);
    n = encode_req(tx, OP_ZADD, "lb", 2, zval, 11);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 5);
    assert(rx[0] == STATUS_OK);

    memcpy(zval, &s2, 8);
    memcpy(zval + 8, "ada", 3);
    n = encode_req(tx, OP_ZADD, "lb", 2, zval, 11);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 5);
    assert(rx[0] == STATUS_OK);

    uint32_t bounds[2] = {htonl(0), htonl(1)};
    n = encode_req(tx, OP_ZRANGE, "lb", 2, bounds, 8);
    assert(write(fd, tx, n) == (ssize_t)n);
    read_exact(fd, rx, 5);
    uint32_t plen;
    memcpy(&plen, rx + 1, 4);
    plen = ntohl(plen);
    read_exact(fd, rx, plen);
    /* First member should be "ada" (score 10.0 < 42.5) */
    assert(memcmp(rx + 8, "ada", 3) == 0);

    close(fd);
    printf("[Verify] Functional PING/SET/GET/ZADD/ZRANGE assertions passed.\n");
}

typedef struct {
    uint16_t port;
    int client_id;
    int total_ops;
} worker_arg_t;

static void *pipeline_worker(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    int fd = connect_server(w->port);
    const int batch = 50;
    uint8_t tx[8192];
    uint8_t rx[4096];
    char key[32];
    char val[64];
    memset(val, 'X', sizeof(val));

    int completed = 0;
    while (completed < w->total_ops) {
        size_t tx_len = 0;
        for (int i = 0; i < batch; i++) {
            int klen = snprintf(key, sizeof(key), "k:%d:%d", w->client_id, (completed + i) % 10000);
            tx_len += encode_req(tx + tx_len, OP_SET, key, (uint16_t)klen, val, sizeof(val));
        }
        ssize_t sent = write(fd, tx, tx_len);
        assert(sent == (ssize_t)tx_len);
        read_exact(fd, rx, (size_t)batch * 5);
        completed += batch;
    }

    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 6379;
    run_functional_checks(port);

    const int num_clients = 50;
    const int ops_per_client = 10000; /* 500,000 total pipelined ops */
    pthread_t threads[50];
    worker_arg_t args[50];

    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < num_clients; i++) {
        args[i] = (worker_arg_t){.port = port, .client_id = i, .total_ops = ops_per_client};
        pthread_create(&threads[i], NULL, pipeline_worker, &args[i]);
    }
    for (int i = 0; i < num_clients; i++) {
        pthread_join(threads[i], NULL);
    }
    uint64_t t1 = bench_now_ns();

    double secs = (double)(t1 - t0) / 1e9;
    double ops_sec = (double)(num_clients * ops_per_client) / secs;
    printf("[Bench] %d pipelined ops across %d clients in %.3fs -> %.0f ops/sec\n",
           num_clients * ops_per_client, num_clients, secs, ops_sec);
    return 0;
}