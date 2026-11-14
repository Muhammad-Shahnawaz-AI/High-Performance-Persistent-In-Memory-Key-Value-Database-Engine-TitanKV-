#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "titankv.h"

typedef struct {
    kv_entry_t **buckets;
    size_t capacity;
    size_t mask;
    size_t count;
} ht_table_t;

typedef struct {
    ht_table_t ht[2];
    int rehash_idx; /* -1 when not rehashing */
} dict_t;

void dict_init(dict_t *d, size_t initial_cap);
void dict_destroy(dict_t *d, void (*free_entry_cb)(kv_entry_t *));
kv_entry_t *dict_get(dict_t *d, const char *key, uint16_t key_len);
bool dict_set(dict_t *d, kv_entry_t *entry);
kv_entry_t *dict_unlink(dict_t *d, const char *key, uint16_t key_len);
size_t dict_size(const dict_t *d);

typedef void (*dict_iter_cb)(kv_entry_t *entry, void *ctx);
void dict_foreach(dict_t *d, dict_iter_cb cb, void *ctx);

#endif /* HASHTABLE_H */