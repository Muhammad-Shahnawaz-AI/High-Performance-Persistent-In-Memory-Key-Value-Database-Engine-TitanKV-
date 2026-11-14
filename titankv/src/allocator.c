#include "allocator.h"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define SLAB_ARENA_SIZE (256 * 1024) /* 256 KB chunks per mmap arena */
#define NUM_SLAB_CLASSES 7
#define PAGE_SIZE 4096

static const size_t SLAB_SIZES[NUM_SLAB_CLASSES] = {32, 64, 128, 256, 512, 1024, 4096};

typedef struct free_node {
    struct free_node *next;
} free_node_t;

typedef struct arena_block {
    void *base;
    size_t size;
    struct arena_block *next;
} arena_block_t;

static struct {
    free_node_t *free_lists[NUM_SLAB_CLASSES];
    arena_block_t *arenas;
    size_t used_bytes;
    size_t max_budget;
} g_alloc;

static int size_to_class(size_t size) {
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (size <= SLAB_SIZES[i]) return i;
    }
    return -1;
}

static size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static bool refill_slab(int cls) {
    size_t slot_size = SLAB_SIZES[cls];
    void *mem = mmap(NULL, SLAB_ARENA_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return false;

    /* Track arena block for clean shutdown */
    arena_block_t *blk = (arena_block_t *)mem;
    blk->base = mem;
    blk->size = SLAB_ARENA_SIZE;
    blk->next = g_alloc.arenas;
    g_alloc.arenas = blk;

    size_t header_offset = align_up(sizeof(arena_block_t), slot_size);
    uint8_t *cursor = (uint8_t *)mem + header_offset;
    uint8_t *end = (uint8_t *)mem + SLAB_ARENA_SIZE;

    while (cursor + slot_size <= end) {
        free_node_t *node = (free_node_t *)cursor;
        node->next = g_alloc.free_lists[cls];
        g_alloc.free_lists[cls] = node;
        cursor += slot_size;
    }
    return true;
}

void allocator_init(size_t max_budget_bytes) {
    memset(&g_alloc, 0, sizeof(g_alloc));
    g_alloc.max_budget = max_budget_bytes;
}

void allocator_destroy(void) {
    arena_block_t *curr = g_alloc.arenas;
    while (curr) {
        arena_block_t *next = curr->next;
        munmap(curr->base, curr->size);
        curr = next;
    }
    memset(&g_alloc, 0, sizeof(g_alloc));
}

size_t allocator_used_bytes(void) {
    return g_alloc.used_bytes;
}

bool allocator_is_oom(size_t upcoming_bytes) {
    return (g_alloc.used_bytes + upcoming_bytes) > g_alloc.max_budget;
}

void *tkv_malloc(size_t size) {
    if (size == 0) size = 1;
    int cls = size_to_class(size);

    if (cls >= 0) {
        size_t actual = SLAB_SIZES[cls];
        if (allocator_is_oom(actual)) return NULL;
        if (!g_alloc.free_lists[cls] && !refill_slab(cls)) return NULL;

        free_node_t *node = g_alloc.free_lists[cls];
        g_alloc.free_lists[cls] = node->next;
        g_alloc.used_bytes += actual;
        return (void *)node;
    }

    /* Large-object direct mmap path (> 4096B) */
    size_t mapped_len = align_up(size, PAGE_SIZE);
    if (allocator_is_oom(mapped_len)) return NULL;

    void *mem = mmap(NULL, mapped_len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    g_alloc.used_bytes += mapped_len;
    return mem;
}

void tkv_free(void *ptr, size_t size) {
    if (!ptr) return;
    if (size == 0) size = 1;
    int cls = size_to_class(size);

    if (cls >= 0) {
        size_t actual = SLAB_SIZES[cls];
        free_node_t *node = (free_node_t *)ptr;
        node->next = g_alloc.free_lists[cls];
        g_alloc.free_lists[cls] = node;
        g_alloc.used_bytes -= actual;
        return;
    }

    size_t mapped_len = align_up(size, PAGE_SIZE);
    munmap(ptr, mapped_len);
    g_alloc.used_bytes -= mapped_len;
}

void *tkv_realloc(void *ptr, size_t old_size, size_t new_size) {
    if (!ptr) return tkv_malloc(new_size);
    if (new_size == 0) {
        tkv_free(ptr, old_size);
        return NULL;
    }
    int old_cls = size_to_class(old_size);
    int new_cls = size_to_class(new_size);
    if (old_cls >= 0 && old_cls == new_cls) return ptr;

    void *new_ptr = tkv_malloc(new_size);
    if (!new_ptr) return NULL;
    size_t copy_len = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_len);
    tkv_free(ptr, old_size);
    return new_ptr;
}