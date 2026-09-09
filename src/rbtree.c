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

static void rotate_left(struct rbtree *t, struct rb_node *x) {
    struct rb_node *y = x->right;
    x->right = y->left;
    if (y->left != &t->nil) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == &t->nil) {
        t->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

static void rotate_right(struct rbtree *t, struct rb_node *x) {
    struct rb_node *y = x->left;
    x->left = y->right;
    if (y->right != &t->nil) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == &t->nil) {
        t->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

/* Frees a node allocated by node_alloc that never made it into the tree
 * (its key copy allocation failed). Not for use on a linked-in node --
 * rb_destroy_subtree already owns that path. */
static void node_release(struct rb_node *z) {
    rb_free(z);
}

/* Allocates and fully initializes a new RED leaf node (key copied, value
 * stored, left/right pointed at the sentinel, parent set to the given
 * would-be parent). Returns NULL, with everything already cleaned up, on
 * either allocation failure -- the caller does not need to free anything. */
static struct rb_node *node_alloc(struct rbtree *t, const char *key, void *value,
                                   struct rb_node *parent) {
    struct rb_node *z = rb_malloc(sizeof *z);
    if (z == NULL) goto fail;

    size_t key_len = strlen(key) + 1;
    char *key_copy = rb_malloc(key_len);
    if (key_copy == NULL) goto fail_release_node;
    memcpy(key_copy, key, key_len);

    z->key = key_copy;
    z->value = value;
    z->left = &t->nil;
    z->right = &t->nil;
    z->parent = parent;
    z->color = RED;
    return z;

fail_release_node:
    node_release(z);
fail:
    return NULL;
}

/* Standard CLRS RB-INSERT-FIXUP. z is always RED; the loop's only
 * invariant violation to fix is a possible red-red edge between z and
 * z->parent. Each side (parent is a left child vs. a right child of the
 * grandparent) is a mirror image of the other: same uncle-red recolor
 * case, same black-uncle triangle-then-line rotation, just left/right and
 * rotate_left/rotate_right swapped. */
static void insert_fixup(struct rbtree *t, struct rb_node *z) {
    /* invariant: z is RED; loop continues only while it has a RED parent
     * (a genuine violation), and terminates on its own once z reaches a
     * BLACK parent or the root (whose parent is the BLACK sentinel) */
    while (z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            struct rb_node *uncle = z->parent->parent->right;
            if (uncle->color == RED) {
                /* case: red uncle -- recolor and push the violation up to
                 * the grandparent */
                z->parent->color = BLACK;
                uncle->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    /* triangle: straighten into a line first */
                    z = z->parent;
                    rotate_left(t, z);
                }
                /* line: single rotation at the grandparent + recolor */
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_right(t, z->parent->parent);
            }
        } else {
            struct rb_node *uncle = z->parent->parent->left;
            if (uncle->color == RED) {
                z->parent->color = BLACK;
                uncle->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(t, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_left(t, z->parent->parent);
            }
        }
    }
    t->root->color = BLACK;
}

int rb_insert(rbtree_t *t, const char *key, void *value) {
    struct rb_node *parent = &t->nil;
    struct rb_node *cur = t->root;
    /* invariant: if key is already present, it lies within the subtree
     * rooted at cur; parent trails cur as its would-be parent */
    while (cur != &t->nil) {
        int cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            void *old_value = cur->value;
            cur->value = value;
            if (t->value_free != NULL) t->value_free(old_value);
            return 0;
        }
        parent = cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }

    struct rb_node *z = node_alloc(t, key, value, parent);
    if (z == NULL) return -1;

    if (parent == &t->nil) {
        t->root = z;
    } else if (strcmp(key, parent->key) < 0) {
        parent->left = z;
    } else {
        parent->right = z;
    }

    t->size++;
    insert_fixup(t, z);
    return 0;
}


