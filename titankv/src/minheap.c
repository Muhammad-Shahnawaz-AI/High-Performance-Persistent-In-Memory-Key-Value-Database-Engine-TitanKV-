#include "minheap.h"
#include "allocator.h"
#include <string.h>

static void heap_swap(minheap_t *h, size_t i, size_t j) {
    kv_entry_t *tmp = h->data[i];
    h->data[i] = h->data[j];
    h->data[j] = tmp;
    h->data[i]->heap_idx = (int32_t)i;
    h->data[j]->heap_idx = (int32_t)j;
}

static void sift_up(minheap_t *h, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (h->data[idx]->expire_at_ms >= h->data[parent]->expire_at_ms) break;
        heap_swap(h, idx, parent);
        idx = parent;
    }
}

static void sift_down(minheap_t *h, size_t idx) {
    while (true) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t smallest = idx;

        if (left < h->size && h->data[left]->expire_at_ms < h->data[smallest]->expire_at_ms) {
            smallest = left;
        }
        if (right < h->size && h->data[right]->expire_at_ms < h->data[smallest]->expire_at_ms) {
            smallest = right;
        }
        if (smallest == idx) break;
        heap_swap(h, idx, smallest);
        idx = smallest;
    }
}

void minheap_init(minheap_t *h, size_t initial_cap) {
    h->capacity = initial_cap;
    h->size = 0;
    h->data = (kv_entry_t **)tkv_malloc(initial_cap * sizeof(kv_entry_t *));
}

void minheap_destroy(minheap_t *h) {
    if (h->data) {
        tkv_free(h->data, h->capacity * sizeof(kv_entry_t *));
    }
    memset(h, 0, sizeof(*h));
}

bool minheap_push(minheap_t *h, kv_entry_t *entry) {
    if (h->size == h->capacity) {
        size_t new_cap = h->capacity * 2;
        kv_entry_t **new_data = (kv_entry_t **)tkv_realloc(
            h->data, h->capacity * sizeof(kv_entry_t *), new_cap * sizeof(kv_entry_t *));
        if (!new_data) return false;
        h->data = new_data;
        h->capacity = new_cap;
    }
    size_t idx = h->size++;
    h->data[idx] = entry;
    entry->heap_idx = (int32_t)idx;
    sift_up(h, idx);
    return true;
}

void minheap_remove(minheap_t *h, kv_entry_t *entry) {
    if (entry->heap_idx < 0 || (size_t)entry->heap_idx >= h->size) return;
    size_t idx = (size_t)entry->heap_idx;
    size_t last = --h->size;
    entry->heap_idx = -1;

    if (idx != last) {
        h->data[idx] = h->data[last];
        h->data[idx]->heap_idx = (int32_t)idx;
        sift_up(h, idx);
        sift_down(h, (size_t)h->data[idx]->heap_idx);
    }
}

void minheap_update(minheap_t *h, kv_entry_t *entry) {
    if (entry->heap_idx < 0) {
        minheap_push(h, entry);
        return;
    }
    size_t idx = (size_t)entry->heap_idx;
    sift_up(h, idx);
    sift_down(h, (size_t)entry->heap_idx);
}

kv_entry_t *minheap_peek(const minheap_t *h) {
    return h->size > 0 ? h->data[0] : NULL;
}