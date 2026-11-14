#ifndef AVL_H
#define AVL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct avl_node {
    double score;
    char *member;
    uint32_t member_len;
    int height;
    size_t subtree_size; /* Augmented order-statistic rank tracking */
    struct avl_node *left;
    struct avl_node *right;
} avl_node_t;

typedef struct zset_map_node {
    char *member;
    uint32_t member_len;
    double score;
    struct zset_map_node *next;
} zset_map_node_t;

typedef struct zset {
    avl_node_t *root;
    zset_map_node_t **buckets;
    size_t bucket_cap;
    size_t count;
} zset_t;

zset_t *zset_create(void);
void zset_destroy(zset_t *zs);
bool zset_add(zset_t *zs, double score, const char *member, uint32_t member_len);
void zset_range(const zset_t *zs, int64_t start, int64_t stop,
                void (*emit_cb)(const char *member, uint32_t len, double score, void *ctx),
                void *ctx);

#endif /* AVL_H */