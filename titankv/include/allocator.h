#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>

void allocator_init(size_t max_budget_bytes);
void allocator_destroy(void);
void *tkv_malloc(size_t size);
void tkv_free(void *ptr, size_t size);
void *tkv_realloc(void *ptr, size_t old_size, size_t new_size);
size_t allocator_used_bytes(void);
bool allocator_is_oom(size_t upcoming_bytes);

#endif /* ALLOCATOR_H */