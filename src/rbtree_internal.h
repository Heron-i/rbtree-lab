#ifndef RBTREE_INTERNAL_H
#define RBTREE_INTERNAL_H

/* Internal layout, shared only between src/rbtree.c and its white-box
 * tests (tests/test_rbtree.c). Not part of the graded public contract in
 * include/rbtree.h -- rbtree_t stays opaque there. */

#include "rbtree.h"

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

#endif
