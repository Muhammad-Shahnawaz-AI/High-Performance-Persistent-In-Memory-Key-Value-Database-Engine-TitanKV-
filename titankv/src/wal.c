#include "wal.h"
#include "allocator.h"
#include "avl.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static uint32_t crc32_table[256];
static bool crc32_ready = false;

static void init_crc32(void) {
    if (crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static bool write_all_iov(int fd, struct iovec *iov, int iovcnt) {
    while (iovcnt > 0) {
        ssize_t n = writev(fd, iov, iovcnt);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        size_t written = (size_t)n;
        while (iovcnt > 0 && written >= iov[0].iov_len) {
            written -= iov[0].iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0 && written > 0) {
            iov[0].iov_base = (uint8_t *)iov[0].iov_base + written;
            iov[0].iov_len -= written;
        }
    }
    return true;
}

bool wal_init(wal_t *w, const char *data_dir, fsync_policy_t policy, size_t compact_threshold) {
    init_crc32();
    memset(w, 0, sizeof(*w));
    mkdir(data_dir, 0755);

    snprintf(w->dir_path, sizeof(w->dir_path), "%s", data_dir);
    snprintf(w->active_path, sizeof(w->active_path), "%s/titankv.wal", data_dir);
    snprintf(w->temp_path, sizeof(w->temp_path), "%s/titankv.wal.tmp", data_dir);

    w->policy = policy;
    w->compact_threshold = compact_threshold;
    w->last_fsync_ms = now_ms();
    pthread_mutex_init(&w->write_lock, NULL);

    w->fd = open(w->active_path, O_CREAT | O_RDWR | O_APPEND, 0644);
    if (w->fd < 0) return false;

    struct stat st;
    if (fstat(w->fd, &st) == 0) {
        w->current_size = (size_t)st.st_size;
    }
    return true;
}

void wal_destroy(wal_t *w) {
    if (w->fd >= 0) {
        fsync(w->fd);
        close(w->fd);
        w->fd = -1;
    }
    pthread_mutex_destroy(&w->write_lock);
}

bool wal_append(wal_t *w, uint8_t op_type, uint64_t expire_at_ms,
                const void *key, uint16_t key_len, const void *val, uint32_t val_len) {
    wal_record_hdr_t hdr;
    hdr.timestamp_ms = now_ms();
    hdr.expire_at_ms = expire_at_ms;
    hdr.op_type = op_type;
    hdr.key_len = key_len;
    hdr.val_len = val_len;

    uint32_t crc = 0;
    crc = crc32_update(crc, (uint8_t *)&hdr + sizeof(uint32_t), sizeof(hdr) - sizeof(uint32_t));
    if (key_len > 0) crc = crc32_update(crc, key, key_len);
    if (val_len > 0) crc = crc32_update(crc, val, val_len);
    hdr.crc32 = crc;

    struct iovec iov[3];
    int iovcnt = 0;
    iov[iovcnt].iov_base = &hdr;
    iov[iovcnt++].iov_len = sizeof(hdr);
    if (key_len > 0) {
        iov[iovcnt].iov_base = (void *)(uintptr_t)key;
        iov[iovcnt++].iov_len = key_len;
    }
    if (val_len > 0) {
        iov[iovcnt].iov_base = (void *)(uintptr_t)val;
        iov[iovcnt++].iov_len = val_len;
    }

    pthread_mutex_lock(&w->write_lock);
    bool ok = write_all_iov(w->fd, iov, iovcnt);
    if (ok) {
        w->current_size += sizeof(hdr) + key_len + val_len;
        if (w->policy == FSYNC_ALWAYS) {
            fsync(w->fd);
            w->last_fsync_ms = hdr.timestamp_ms;
        }
    }
    pthread_mutex_unlock(&w->write_lock);
    return ok;
}

void wal_tick_fsync(wal_t *w, uint64_t current_ms) {
    if (w->policy != FSYNC_EVERYSEC) return;
    if (current_ms - w->last_fsync_ms >= 1000) {
        pthread_mutex_lock(&w->write_lock);
        fsync(w->fd);
        w->last_fsync_ms = current_ms;
        pthread_mutex_unlock(&w->write_lock);
    }
}

/* Helper to free entry during recovery/eviction */
static void free_kv_entry(kv_entry_t *e) {
    if (!e) return;
    tkv_free(e->key, e->key_len);
    if (e->type == VAL_TYPE_STRING) {
        tkv_free(e->val.str.data, e->val.str.len);
    } else if (e->type == VAL_TYPE_ZSET) {
        zset_destroy(e->val.zset);
    }
    tkv_free(e, sizeof(kv_entry_t));
}

bool wal_recover(wal_t *w, dict_t *dict, minheap_t *heap) {
    int rfd = open(w->active_path, O_RDONLY);
    if (rfd < 0) return true;

    off_t valid_offset = 0;
    uint64_t current_time = now_ms();

    while (true) {
        wal_record_hdr_t hdr;
        ssize_t n = read(rfd, &hdr, sizeof(hdr));
        if (n == 0) break; /* Clean EOF */
        if (n < (ssize_t)sizeof(hdr)) {
            fprintf(stderr, "[WAL] Truncated header at offset %lld; truncating.\n", (long long)valid_offset);
            break;
        }
        if (hdr.key_len == 0 || hdr.val_len > TITANKV_MAX_VAL_LEN) {
            fprintf(stderr, "[WAL] Corrupt record lengths at offset %lld; truncating.\n", (long long)valid_offset);
            break;
        }

        size_t payload_bytes = (size_t)hdr.key_len + (size_t)hdr.val_len;
        uint8_t *buf = (uint8_t *)malloc(payload_bytes);
        if (!buf) break;

        ssize_t pn = read(rfd, buf, payload_bytes);
        if (pn < (ssize_t)payload_bytes) {
            fprintf(stderr, "[WAL] Truncated payload at offset %lld; truncating.\n", (long long)valid_offset);
            free(buf);
            break;
        }

        uint32_t crc = 0;
        crc = crc32_update(crc, (uint8_t *)&hdr + sizeof(uint32_t), sizeof(hdr) - sizeof(uint32_t));
        crc = crc32_update(crc, buf, payload_bytes);
        if (crc != hdr.crc32) {
            fprintf(stderr, "[WAL] CRC32 mismatch at offset %lld (expected %08x, got %08x); truncating.\n",
                    (long long)valid_offset, hdr.crc32, crc);
            free(buf);
            break;
        }

        const char *key = (const char *)buf;
        const uint8_t *val = buf + hdr.key_len;

        if (hdr.op_type == OP_DEL || (hdr.expire_at_ms > 0 && hdr.expire_at_ms <= current_time)) {
            kv_entry_t *old = dict_unlink(dict, key, hdr.key_len);
            if (old) {
                minheap_remove(heap, old);
                free_kv_entry(old);
            }
        } else if (hdr.op_type == OP_SET) {
            kv_entry_t *old = dict_unlink(dict, key, hdr.key_len);
            if (old) {
                minheap_remove(heap, old);
                free_kv_entry(old);
            }
            kv_entry_t *e = (kv_entry_t *)tkv_malloc(sizeof(kv_entry_t));
            e->key = (char *)tkv_malloc(hdr.key_len);
            memcpy(e->key, key, hdr.key_len);
            e->key_len = hdr.key_len;
            e->type = VAL_TYPE_STRING;
            e->val.str.data = (uint8_t *)tkv_malloc(hdr.val_len > 0 ? hdr.val_len : 1);
            if (hdr.val_len > 0) memcpy(e->val.str.data, val, hdr.val_len);
            e->val.str.len = hdr.val_len;
            e->expire_at_ms = hdr.expire_at_ms;
            e->heap_idx = -1;
            dict_set(dict, e);
            if (e->expire_at_ms > 0) minheap_push(heap, e);
        } else if (hdr.op_type == OP_ZADD && hdr.val_len >= sizeof(double)) {
            double score;
            memcpy(&score, val, sizeof(double));
            const char *member = (const char *)(val + sizeof(double));
            uint32_t mlen = hdr.val_len - (uint32_t)sizeof(double);

            kv_entry_t *e = dict_get(dict, key, hdr.key_len);
            if (!e) {
                e = (kv_entry_t *)tkv_malloc(sizeof(kv_entry_t));
                e->key = (char *)tkv_malloc(hdr.key_len);
                memcpy(e->key, key, hdr.key_len);
                e->key_len = hdr.key_len;
                e->type = VAL_TYPE_ZSET;
                e->val.zset = zset_create();
                e->expire_at_ms = hdr.expire_at_ms;
                e->heap_idx = -1;
                dict_set(dict, e);
            }
            if (e->type == VAL_TYPE_ZSET) {
                zset_add(e->val.zset, score, member, mlen);
            }
        }

        free(buf);
        valid_offset += (off_t)(sizeof(hdr) + payload_bytes);
    }

    close(rfd);

    /* Truncate corrupted tail and reopen active WAL */
    pthread_mutex_lock(&w->write_lock);
    close(w->fd);
    if (truncate(w->active_path, valid_offset) != 0) {
        perror("[WAL] truncate");
    }
    w->fd = open(w->active_path, O_CREAT | O_RDWR | O_APPEND, 0644);
    w->current_size = (size_t)valid_offset;
    pthread_mutex_unlock(&w->write_lock);
    return true;
}

/* Snapshot structures for non-blocking background compaction */
typedef struct {
    char *key;
    uint16_t key_len;
    val_type_t type;
    uint64_t expire_at_ms;
    uint8_t *str_data;
    uint32_t str_len;
    struct {
        double score;
        char *member;
        uint32_t mlen;
    } *zitems;
    size_t zcount;
} snap_item_t;

typedef struct {
    snap_item_t *items;
    size_t count;
    size_t cap;
    uint64_t now;
} snap_ctx_t;

typedef struct {
    snap_item_t *item;
    size_t idx;
} zsnap_ctx_t;

static void zsnap_emit(const char *m, uint32_t len, double score, void *ctx) {
    zsnap_ctx_t *zc = (zsnap_ctx_t *)ctx;
    zc->item->zitems[zc->idx].score = score;
    zc->item->zitems[zc->idx].mlen = len;
    zc->item->zitems[zc->idx].member = (char *)malloc(len);
    memcpy(zc->item->zitems[zc->idx].member, m, len);
    zc->idx++;
}

static void collect_snapshot_cb(kv_entry_t *e, void *ctx) {
    snap_ctx_t *sc = (snap_ctx_t *)ctx;
    if (e->expire_at_ms > 0 && e->expire_at_ms <= sc->now) return;

    snap_item_t *it = &sc->items[sc->count++];
    memset(it, 0, sizeof(*it));
    it->key_len = e->key_len;
    it->key = (char *)malloc(e->key_len);
    memcpy(it->key, e->key, e->key_len);
    it->type = e->type;
    it->expire_at_ms = e->expire_at_ms;

    if (e->type == VAL_TYPE_STRING) {
        it->str_len = e->val.str.len;
        it->str_data = (uint8_t *)malloc(it->str_len > 0 ? it->str_len : 1);
        if (it->str_len > 0) memcpy(it->str_data, e->val.str.data, it->str_len);
    } else if (e->type == VAL_TYPE_ZSET && e->val.zset) {
        it->zcount = e->val.zset->count;
        it->zitems = calloc(it->zcount, sizeof(*it->zitems));
        zsnap_ctx_t zc = {.item = it, .idx = 0};
        zset_range(e->val.zset, 0, -1, zsnap_emit, &zc);
    }
}

typedef struct {
    wal_t *wal;
    snap_ctx_t snap;
} compact_job_t;

static void write_compacted_rec(int fd, uint8_t op, uint64_t exp, const void *k, uint16_t kl, const void *v, uint32_t vl) {
    wal_record_hdr_t hdr = {
        .timestamp_ms = now_ms(),
        .expire_at_ms = exp,
        .op_type = op,
        .key_len = kl,
        .val_len = vl
    };
    uint32_t crc = 0;
    crc = crc32_update(crc, (uint8_t *)&hdr + sizeof(uint32_t), sizeof(hdr) - sizeof(uint32_t));
    if (kl > 0) crc = crc32_update(crc, k, kl);
    if (vl > 0) crc = crc32_update(crc, v, vl);
    hdr.crc32 = crc;

    struct iovec iov[3] = {
        {.iov_base = &hdr, .iov_len = sizeof(hdr)},
        {.iov_base = (void *)(uintptr_t)k, .iov_len = kl},
        {.iov_base = (void *)(uintptr_t)v, .iov_len = vl}
    };
    write_all_iov(fd, iov, vl > 0 ? 3 : 2);
}

static void *compaction_worker(void *arg) {
    compact_job_t *job = (compact_job_t *)arg;
    wal_t *w = job->wal;

    int tfd = open(w->temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (tfd >= 0) {
        for (size_t i = 0; i < job->snap.count; i++) {
            snap_item_t *it = &job->snap.items[i];
            if (it->type == VAL_TYPE_STRING) {
                write_compacted_rec(tfd, OP_SET, it->expire_at_ms, it->key, it->key_len, it->str_data, it->str_len);
                free(it->str_data);
            } else if (it->type == VAL_TYPE_ZSET) {
                for (size_t j = 0; j < it->zcount; j++) {
                    uint32_t vlen = (uint32_t)sizeof(double) + it->zitems[j].mlen;
                    uint8_t *vbuf = (uint8_t *)malloc(vlen);
                    memcpy(vbuf, &it->zitems[j].score, sizeof(double));
                    memcpy(vbuf + sizeof(double), it->zitems[j].member, it->zitems[j].mlen);
                    write_compacted_rec(tfd, OP_ZADD, it->expire_at_ms, it->key, it->key_len, vbuf, vlen);
                    free(vbuf);
                    free(it->zitems[j].member);
                }
                free(it->zitems);
            }
            free(it->key);
        }
        fsync(tfd);
        close(tfd);

        /* Atomic swap under brief mutex lock */
        pthread_mutex_lock(&w->write_lock);
        close(w->fd);
        rename(w->temp_path, w->active_path);
        w->fd = open(w->active_path, O_CREAT | O_RDWR | O_APPEND, 0644);
        struct stat st;
        w->current_size = (fstat(w->fd, &st) == 0) ? (size_t)st.st_size : 0;
        pthread_mutex_unlock(&w->write_lock);
    }

    free(job->snap.items);
    w->compacting = false;
    free(job);
    return NULL;
}

void wal_maybe_compact(wal_t *w, dict_t *dict) {
    if (w->compacting || w->current_size < w->compact_threshold) return;
    w->compacting = true;

    compact_job_t *job = (compact_job_t *)calloc(1, sizeof(compact_job_t));
    if (!job) {
        w->compacting = false;
        return;
    }
    job->wal = w;
    job->snap.cap = dict_size(dict) + 1;
    job->snap.items = (snap_item_t *)malloc(job->snap.cap * sizeof(snap_item_t));
    job->snap.now = now_ms();

    dict_foreach(dict, collect_snapshot_cb, &job->snap);

    pthread_t tid;
    if (pthread_create(&tid, NULL, compaction_worker, job) == 0) {
        pthread_detach(tid);
    } else {
        w->compacting = false;
        free(job->snap.items);
        free(job);
    }
}