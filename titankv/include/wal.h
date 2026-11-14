#ifndef WAL_H
#define WAL_H

#include "hashtable.h"
#include "minheap.h"
#include "titankv.h"
#include <pthread.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t crc32;
    uint64_t timestamp_ms;
    uint64_t expire_at_ms;
    uint8_t  op_type;
    uint16_t key_len;
    uint32_t val_len;
} wal_record_hdr_t;
#pragma pack(pop)

typedef struct {
    int fd;
    char dir_path[512];
    char active_path[576];
    char temp_path[576];
    fsync_policy_t policy;
    uint64_t last_fsync_ms;
    size_t current_size;
    size_t compact_threshold;
    pthread_mutex_t write_lock;
    volatile bool compacting;
} wal_t;

bool wal_init(wal_t *w, const char *data_dir, fsync_policy_t policy, size_t compact_threshold);
void wal_destroy(wal_t *w);
bool wal_append(wal_t *w, uint8_t op_type, uint64_t expire_at_ms,
                const void *key, uint16_t key_len, const void *val, uint32_t val_len);
void wal_tick_fsync(wal_t *w, uint64_t current_ms);
bool wal_recover(wal_t *w, dict_t *dict, minheap_t *heap);
void wal_maybe_compact(wal_t *w, dict_t *dict);

#endif /* WAL_H */