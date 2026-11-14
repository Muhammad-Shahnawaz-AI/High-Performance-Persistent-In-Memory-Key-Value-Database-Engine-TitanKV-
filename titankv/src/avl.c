#include "avl.h"
#include "allocator.h"
#include <string.h>

#define ZSET_INIT_BUCKETS 64

static uint64_t zhash(const char *s, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int node_height(const avl_node_t *n) { return n ? n->height : 0; }
static size_t node_size(const avl_node_t *n) { return n ? n->subtree_size : 0; }

static void node_update(avl_node_t *n) {
    int lh = node_height(n->left);
    int rh = node_height(n->right);
    n->height = (lh > rh ? lh : rh) + 1;
    n->subtree_size = node_size(n->left) + node_size(n->right) + 1;
}

static int node_cmp(double s1, const char *m1, uint32_t l1,
                    double s2, const char *m2, uint32_t l2) {
    if (s1 < s2) return -1;
    if (s1 > s2) return 1;
    uint32_t min_l = l1 < l2 ? l1 : l2;
    int c = memcmp(m1, m2, min_l);
    if (c != 0) return c;
    if (l1 < l2) return -1;
    if (l1 > l2) return 1;
    return 0;
}

static avl_node_t *rot_right(avl_node_t *y) {
    avl_node_t *x = y->left;
    avl_node_t *t2 = x->right;
    x->right = y;
    y->left = t2;
    node_update(y);
    node_update(x);
    return x;
}

static avl_node_t *rot_left(avl_node_t *x) {
    avl_node_t *y = x->right;
    avl_node_t *t2 = y->left;
    y->left = x;
    x->right = t2;
    node_update(x);
    node_update(y);
    return y;
}

static avl_node_t *rebalance(avl_node_t *n) {
    node_update(n);
    int bal = node_height(n->left) - node_height(n->right);
    if (bal > 1) {
        if (node_height(n->left->left) < node_height(n->left->right)) {
            n->left = rot_left(n->left);
        }
        return rot_right(n);
    }
    if (bal < -1) {
        if (node_height(n->right->right) < node_height(n->right->left)) {
            n->right = rot_right(n->right);
        }
        return rot_left(n);
    }
    return n;
}

static avl_node_t *avl_insert(avl_node_t *root, avl_node_t *new_node) {
    if (!root) return new_node;
    int cmp = node_cmp(new_node->score, new_node->member, new_node->member_len,
                       root->score, root->member, root->member_len);
    if (cmp < 0) root->left = avl_insert(root->left, new_node);
    else root->right = avl_insert(root->right, new_node);
    return rebalance(root);
}

static avl_node_t *avl_min_node(avl_node_t *n) {
    while (n->left) n = n->left;
    return n;
}

static avl_node_t *avl_remove(avl_node_t *root, double score, const char *m, uint32_t len, avl_node_t **removed) {
    if (!root) return NULL;
    int cmp = node_cmp(score, m, len, root->score, root->member, root->member_len);
    if (cmp < 0) {
        root->left = avl_remove(root->left, score, m, len, removed);
    } else if (cmp > 0) {
        root->right = avl_remove(root->right, score, m, len, removed);
    } else {
        *removed = root;
        if (!root->left || !root->right) {
            return root->left ? root->left : root->right;
        }
        avl_node_t *succ = avl_min_node(root->right);
        avl_node_t *dummy = NULL;
        root->right = avl_remove(root->right, succ->score, succ->member, succ->member_len, &dummy);
        succ->left = root->left;
        succ->right = root->right;
        return rebalance(succ);
    }
    return rebalance(root);
}

static void avl_free_tree(avl_node_t *n) {
    if (!n) return;
    avl_free_tree(n->left);
    avl_free_tree(n->right);
    tkv_free(n->member, n->member_len);
    tkv_free(n, sizeof(avl_node_t));
}

zset_t *zset_create(void) {
    zset_t *zs = (zset_t *)tkv_malloc(sizeof(zset_t));
    if (!zs) return NULL;
    memset(zs, 0, sizeof(*zs));
    zs->bucket_cap = ZSET_INIT_BUCKETS;
    zs->buckets = (zset_map_node_t **)tkv_malloc(zs->bucket_cap * sizeof(zset_map_node_t *));
    if (!zs->buckets) {
        tkv_free(zs, sizeof(zset_t));
        return NULL;
    }
    memset(zs->buckets, 0, zs->bucket_cap * sizeof(zset_map_node_t *));
    return zs;
}

void zset_destroy(zset_t *zs) {
    if (!zs) return;
    avl_free_tree(zs->root);
    for (size_t i = 0; i < zs->bucket_cap; i++) {
        zset_map_node_t *curr = zs->buckets[i];
        while (curr) {
            zset_map_node_t *next = curr->next;
            tkv_free(curr->member, curr->member_len);
            tkv_free(curr, sizeof(zset_map_node_t));
            curr = next;
        }
    }
    tkv_free(zs->buckets, zs->bucket_cap * sizeof(zset_map_node_t *));
    tkv_free(zs, sizeof(zset_t));
}

bool zset_add(zset_t *zs, double score, const char *member, uint32_t member_len) {
    size_t idx = zhash(member, member_len) & (zs->bucket_cap - 1);
    zset_map_node_t *mnode = zs->buckets[idx];
    while (mnode) {
        if (mnode->member_len == member_len && memcmp(mnode->member, member, member_len) == 0) {
            if (mnode->score == score) return true;
            avl_node_t *detached = NULL;
            zs->root = avl_remove(zs->root, mnode->score, member, member_len, &detached);
            mnode->score = score;
            if (detached) {
                detached->score = score;
                detached->left = detached->right = NULL;
                detached->height = 1;
                detached->subtree_size = 1;
                zs->root = avl_insert(zs->root, detached);
            }
            return true;
        }
        mnode = mnode->next;
    }

    /* New member insertion */
    avl_node_t *anode = (avl_node_t *)tkv_malloc(sizeof(avl_node_t));
    char *acopy = (char *)tkv_malloc(member_len);
    zset_map_node_t *new_mnode = (zset_map_node_t *)tkv_malloc(sizeof(zset_map_node_t));
    char *mcopy = (char *)tkv_malloc(member_len);
    if (!anode || !acopy || !new_mnode || !mcopy) {
        tkv_free(anode, sizeof(avl_node_t));
        tkv_free(acopy, member_len);
        tkv_free(new_mnode, sizeof(zset_map_node_t));
        tkv_free(mcopy, member_len);
        return false;
    }

    memcpy(acopy, member, member_len);
    anode->score = score;
    anode->member = acopy;
    anode->member_len = member_len;
    anode->height = 1;
    anode->subtree_size = 1;
    anode->left = anode->right = NULL;

    memcpy(mcopy, member, member_len);
    new_mnode->member = mcopy;
    new_mnode->member_len = member_len;
    new_mnode->score = score;
    new_mnode->next = zs->buckets[idx];
    zs->buckets[idx] = new_mnode;

    zs->root = avl_insert(zs->root, anode);
    zs->count++;
    return true;
}

static void avl_range_walk(const avl_node_t *n, size_t offset, size_t start, size_t stop,
                           void (*emit_cb)(const char *, uint32_t, double, void *), void *ctx) {
    if (!n) return;
    size_t left_sz = node_size(n->left);
    size_t curr_rank = offset + left_sz;

    if (start < curr_rank) {
        avl_range_walk(n->left, offset, start, stop, emit_cb, ctx);
    }
    if (curr_rank >= start && curr_rank <= stop) {
        emit_cb(n->member, n->member_len, n->score, ctx);
    }
    if (stop > curr_rank) {
        avl_range_walk(n->right, curr_rank + 1, start, stop, emit_cb, ctx);
    }
}

void zset_range(const zset_t *zs, int64_t start, int64_t stop,
                void (*emit_cb)(const char *, uint32_t, double, void *), void *ctx) {
    if (!zs || zs->count == 0) return;
    int64_t n = (int64_t)zs->count;
    if (start < 0) start += n;
    if (stop < 0) stop += n;
    if (start < 0) start = 0;
    if (stop >= n) stop = n - 1;
    if (start > stop || start >= n) return;

    avl_range_walk(zs->root, 0, (size_t)start, (size_t)stop, emit_cb, ctx);
}