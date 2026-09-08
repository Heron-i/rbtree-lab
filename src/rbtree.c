//Purpose: Implement a full-working, self-sufficient red-black tree
#include "rbtree.h"
#include "rbtree_internal.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

bool rb_fail_next_alloc = false;   /* test-only hook, external linkage */

static void *rb_malloc(size_t size) {
    if (rb_fail_next_alloc) { rb_fail_next_alloc = false; return NULL; }
    return malloc(size);
}

static void rb_free(void *ptr) {
    free(ptr);
}

rbtree_t *rb_create(rb_value_free_fn value_free) {
    struct rbtree *t = rb_malloc(sizeof *t);
    if (t == NULL) return NULL;

    t->nil.color = BLACK;
    t->nil.left = t->nil.right = t->nil.parent = &t->nil;
    t->nil.key = NULL;
    t->nil.value = NULL;

    t->root = &t->nil;
    t->size = 0;
    t->value_free = value_free;

    /* self-check: a just-built tree must already satisfy RB invariants
     * before it's handed to the caller */
    if (rb_validate(t) != 0) {
        rb_free(t);
        return NULL;
    }

    return t;
}

static void rb_destroy_subtree(struct rbtree *t, struct rb_node *n) {
    if (n == &t->nil) return;          /* base case: sentinel marks "no node" */
    rb_destroy_subtree(t, n->left);
    rb_destroy_subtree(t, n->right);
    if (t->value_free != NULL) t->value_free(n->value);
    rb_free(n->key);
    rb_free(n);
}

void rb_destroy(rbtree_t *t) {
    if (t == NULL) return;             /* NULL-safe per header contract */
    rb_destroy_subtree(t, t->root);
    rb_free(t);
}

static size_t rb_count_nodes(const struct rbtree *t, const struct rb_node *n) {
    if (n == &t->nil) return 0;
    return 1 + rb_count_nodes(t, n->left) + rb_count_nodes(t, n->right);
}

/* Checks BST order (strcmp against the open interval (lo, hi)), the
 * no-red-red rule, and black-height equality in one pass; *black_height is
 * only meaningful when this returns true. lo/hi are NULL for "no bound". */
static bool rb_validate_node(const struct rbtree *t, const struct rb_node *n,
                              const char *lo, const char *hi, int *black_height) {
    if (n == &t->nil) {
        *black_height = 1;
        return true;
    }
    if (lo != NULL && strcmp(lo, n->key) >= 0) return false;
    if (hi != NULL && strcmp(n->key, hi) >= 0) return false;
    if (n->color == RED) {
        if (n->left != &t->nil && n->left->color == RED) return false;
        if (n->right != &t->nil && n->right->color == RED) return false;
    }
    int bh_left, bh_right;
    if (!rb_validate_node(t, n->left, lo, n->key, &bh_left)) return false;
    if (!rb_validate_node(t, n->right, n->key, hi, &bh_right)) return false;
    if (bh_left != bh_right) return false;
    *black_height = bh_left + (n->color == BLACK ? 1 : 0);
    return true;
}

int rb_validate(const rbtree_t *t) {
    if (t->root->color != BLACK) return 1;
    int black_height;
    if (!rb_validate_node(t, t->root, NULL, NULL, &black_height)) return 1;
    if (rb_count_nodes(t, t->root) != t->size) return 1;
    return 0;
}

void *rb_find(const rbtree_t *t, const char *key) {
    const struct rb_node *n = t->root;
    /* invariant: if key is present, it lies within the subtree rooted at n */
    while (n != &t->nil) {
        int cmp = strcmp(key, n->key);
        if (cmp == 0) return n->value;
        n = (cmp < 0) ? n->left : n->right;
    }
    return NULL;
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
}


