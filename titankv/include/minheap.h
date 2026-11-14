#ifndef MINHEAP_H
#define MINHEAP_H

#include "titankv.h"

typedef struct {
    kv_entry_t **data;
    size_t size;
    size_t capacity;
} minheap_t;

void minheap_init(minheap_t *h, size_t initial_cap);
void minheap_destroy(minheap_t *h);
bool minheap_push(minheap_t *h, kv_entry_t *entry);
void minheap_remove(minheap_t *h, kv_entry_t *entry);
void minheap_update(minheap_t *h, kv_entry_t *entry);
kv_entry_t *minheap_peek(const minheap_t *h);

#endif /* MINHEAP_H */