#include "hashtable.h"
#include "allocator.h"
#include <string.h>

#define REHASH_BATCH_SIZE 8

static uint64_t hash_fnv1a_64(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static bool ht_alloc_table(ht_table_t *t, size_t cap) {
    size_t bytes = cap * sizeof(kv_entry_t *);
    t->buckets = (kv_entry_t **)tkv_malloc(bytes);
    if (!t->buckets) return false;
    memset(t->buckets, 0, bytes);
    t->capacity = cap;
    t->mask = cap - 1;
    t->count = 0;
    return true;
}

static void dict_rehash_step(dict_t *d) {
    if (d->rehash_idx == -1) return;

    int migrated = 0;
    while (migrated < REHASH_BATCH_SIZE && (size_t)d->rehash_idx < d->ht[0].capacity) {
        kv_entry_t *curr = d->ht[0].buckets[d->rehash_idx];
        if (!curr) {
            d->rehash_idx++;
            continue;
        }
        while (curr) {
            kv_entry_t *next = curr->next;
            uint64_t h = hash_fnv1a_64(curr->key, curr->key_len) & d->ht[1].mask;
            curr->next = d->ht[1].buckets[h];
            d->ht[1].buckets[h] = curr;
            d->ht[0].count--;
            d->ht[1].count++;
            curr = next;
        }
        d->ht[0].buckets[d->rehash_idx] = NULL;
        d->rehash_idx++;
        migrated++;
    }

    if (d->ht[0].count == 0) {
        tkv_free(d->ht[0].buckets, d->ht[0].capacity * sizeof(kv_entry_t *));
        d->ht[0] = d->ht[1];
        memset(&d->ht[1], 0, sizeof(ht_table_t));
        d->rehash_idx = -1;
    }
}

static void dict_maybe_expand(dict_t *d) {
    if (d->rehash_idx != -1) return;
    if (d->ht[0].count * 4 >= d->ht[0].capacity * 3) { /* Load factor >= 0.75 */
        if (ht_alloc_table(&d->ht[1], d->ht[0].capacity * 2)) {
            d->rehash_idx = 0;
        }
    }
}

void dict_init(dict_t *d, size_t initial_cap) {
    memset(d, 0, sizeof(*d));
    d->rehash_idx = -1;
    ht_alloc_table(&d->ht[0], initial_cap);
}

void dict_destroy(dict_t *d, void (*free_entry_cb)(kv_entry_t *)) {
    for (int t = 0; t < 2; t++) {
        if (!d->ht[t].buckets) continue;
        for (size_t i = 0; i < d->ht[t].capacity; i++) {
            kv_entry_t *curr = d->ht[t].buckets[i];
            while (curr) {
                kv_entry_t *next = curr->next;
                if (free_entry_cb) free_entry_cb(curr);
                curr = next;
            }
        }
        tkv_free(d->ht[t].buckets, d->ht[t].capacity * sizeof(kv_entry_t *));
    }
    memset(d, 0, sizeof(*d));
    d->rehash_idx = -1;
}

kv_entry_t *dict_get(dict_t *d, const char *key, uint16_t key_len) {
    dict_rehash_step(d);
    uint64_t hash = hash_fnv1a_64(key, key_len);

    for (int t = 0; t < 2; t++) {
        if (!d->ht[t].buckets) continue;
        size_t idx = hash & d->ht[t].mask;
        kv_entry_t *curr = d->ht[t].buckets[idx];
        while (curr) {
            if (curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
                return curr;
            }
            curr = curr->next;
        }
        if (d->rehash_idx == -1) break;
    }
    return NULL;
}

bool dict_set(dict_t *d, kv_entry_t *entry) {
    dict_rehash_step(d);
    dict_maybe_expand(d);

    ht_table_t *target = (d->rehash_idx != -1) ? &d->ht[1] : &d->ht[0];
    uint64_t idx = hash_fnv1a_64(entry->key, entry->key_len) & target->mask;

    entry->next = target->buckets[idx];
    target->buckets[idx] = entry;
    target->count++;
    return true;
}

kv_entry_t *dict_unlink(dict_t *d, const char *key, uint16_t key_len) {
    dict_rehash_step(d);
    uint64_t hash = hash_fnv1a_64(key, key_len);

    for (int t = 0; t < 2; t++) {
        if (!d->ht[t].buckets) continue;
        size_t idx = hash & d->ht[t].mask;
        kv_entry_t *prev = NULL;
        kv_entry_t *curr = d->ht[t].buckets[idx];
        while (curr) {
            if (curr->key_len == key_len && memcmp(curr->key, key, key_len) == 0) {
                if (prev) prev->next = curr->next;
                else d->ht[t].buckets[idx] = curr->next;
                d->ht[t].count--;
                curr->next = NULL;
                return curr;
            }
            prev = curr;
            curr = curr->next;
        }
        if (d->rehash_idx == -1) break;
    }
    return NULL;
}

size_t dict_size(const dict_t *d) {
    return d->ht[0].count + d->ht[1].count;
}

void dict_foreach(dict_t *d, dict_iter_cb cb, void *ctx) {
    for (int t = 0; t < 2; t++) {
        if (!d->ht[t].buckets) continue;
        for (size_t i = 0; i < d->ht[t].capacity; i++) {
            kv_entry_t *curr = d->ht[t].buckets[i];
            while (curr) {
                kv_entry_t *next = curr->next;
                cb(curr, ctx);
                curr = next;
            }
        }
    }
}