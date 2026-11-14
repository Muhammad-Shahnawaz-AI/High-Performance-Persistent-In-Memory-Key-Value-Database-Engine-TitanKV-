#ifndef TITANKV_H
#define TITANKV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TITANKV_MAX_KEY_LEN 65535
#define TITANKV_MAX_VAL_LEN 16777216

typedef enum {
    FSYNC_ALWAYS = 0,
    FSYNC_EVERYSEC = 1,
    FSYNC_NO = 2
} fsync_policy_t;

typedef enum {
    VAL_TYPE_STRING = 1,
    VAL_TYPE_ZSET   = 2
} val_type_t;

typedef struct zset zset_t;

typedef struct kv_entry {
    char *key;
    uint16_t key_len;
    val_type_t type;
    union {
        struct {
            uint8_t *data;
            uint32_t len;
        } str;
        zset_t *zset;
    } val;
    uint64_t expire_at_ms;   /* 0 if persistent */
    int32_t  heap_idx;       /* -1 if not in TTL min-heap */
    struct kv_entry *next;   /* Hash bucket chaining pointer */
} kv_entry_t;

typedef struct {
    uint16_t port;
    const char *data_dir;
    size_t max_memory_bytes;
    fsync_policy_t fsync_policy;
    size_t wal_compact_threshold;
} server_config_t;

uint64_t now_ms(void);

#endif /* TITANKV_H */