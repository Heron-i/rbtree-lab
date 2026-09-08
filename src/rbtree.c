//Purpose: Implement a full-working, self-sufficient red-black tree
#include "rbtree.h"
#include <stdbool.h>
#include <stdlib.h>

typedef enum { RED, BLACK } rb_color_t;

struct rb_node {
    char *key;              /* heap copy; the tree owns it */
    void *value;             /* ownership per header contract */
    struct rb_node *left;
    struct rb_node *right;
    struct rb_node *parent;
    rb_color_t color;
};

struct rbtree {
    struct rb_node *root;
    struct rb_node nil;       /* embedded shared sentinel, always BLACK */
    size_t size;
    rb_value_free_fn value_free;
};

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


