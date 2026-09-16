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

/* Replaces the subtree rooted at u with the subtree rooted at v, rewiring
 * u->parent's child pointer accordingly. v may be &t->nil. */
static void transplant(struct rbtree *t, struct rb_node *u, struct rb_node *v) {
    if (u->parent == &t->nil) {
        t->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    /* set even when v is the sentinel: delete_fixup walks x->parent when x
     * has no children, and that read only ever happens when rb_delete's
     * y_original_color was BLACK -- the only case delete_fixup runs at
     * all. Nothing else reads t->nil.parent. */
    v->parent = u->parent;
}

/* Leftmost node of n's subtree (the in-order successor's starting point).
 * invariant: if n's subtree is nonempty, the minimum lies within the
 * subtree rooted at n */
static struct rb_node *tree_minimum(struct rbtree *t, struct rb_node *n) {
    while (n->left != &t->nil) n = n->left;
    return n;
}

/* Standard CLRS RB-DELETE-FIXUP. x carries an "extra black" that must be
 * absorbed somewhere up the tree before the loop can terminate. Each side
 * (x is a left child vs. a right child of its parent) is a mirror image of
 * the other: same red-sibling rotate, same both-nephews-black recolor,
 * same near-red/far-black rotate-then-fall-through, same far-red terminal
 * rotate -- just left/right and rotate_left/rotate_right swapped, matching
 * insert_fixup's style. */
static void delete_fixup(struct rbtree *t, struct rb_node *x) {
    /* invariant: x carries the extra black only while it's BLACK and not
     * yet the root; the loop terminates on its own once x reaches a RED
     * node (recolored BLACK below, absorbing the extra black) or the root */
    while (x != t->root && x->color == BLACK) {
        if (x == x->parent->left) {
            struct rb_node *w = x->parent->right;
            if (w->color == RED) {
                /* case 1: red sibling -- recolor + rotate so the new
                 * sibling is guaranteed BLACK, converting into case 2/3/4 */
                w->color = BLACK;
                x->parent->color = RED;
                rotate_left(t, x->parent);
                w = x->parent->right;
            }
            if (w->left->color == BLACK && w->right->color == BLACK) {
                /* case 2: both nephews BLACK -- absorb the extra black by
                 * recoloring the sibling RED and pushing it up a level */
                w->color = RED;
                x = x->parent;
            } else {
                if (w->right->color == BLACK) {
                    /* case 3: near nephew RED, far nephew BLACK -- rotate
                     * at the sibling to convert into case 4 */
                    w->left->color = BLACK;
                    w->color = RED;
                    rotate_right(t, w);
                    w = x->parent->right;
                }
                /* case 4: far nephew RED -- recolor + rotate at the
                 * parent, fully restoring black-height; terminates */
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->right->color = BLACK;
                rotate_left(t, x->parent);
                x = t->root;
            }
        } else {
            struct rb_node *w = x->parent->left;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                rotate_right(t, x->parent);
                w = x->parent->left;
            }
            if (w->right->color == BLACK && w->left->color == BLACK) {
                w->color = RED;
                x = x->parent;
            } else {
                if (w->left->color == BLACK) {
                    w->right->color = BLACK;
                    w->color = RED;
                    rotate_left(t, w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->left->color = BLACK;
                rotate_right(t, x->parent);
                x = t->root;
            }
        }
    }
    x->color = BLACK;
}

int rb_delete(rbtree_t *t, const char *key) {
    struct rb_node *z = t->root;
    /* invariant: if key is present, it lies within the subtree rooted at z */
    while (z != &t->nil) {
        int cmp = strcmp(key, z->key);
        if (cmp == 0) break;
        z = (cmp < 0) ? z->left : z->right;
    }
    if (z == &t->nil) return -1;

    struct rb_node *y = z;
    rb_color_t y_original_color = y->color;
    struct rb_node *x;

    if (z->left == &t->nil) {
        x = z->right;
        transplant(t, z, x);
    } else if (z->right == &t->nil) {
        x = z->left;
        transplant(t, z, x);
    } else {
        y = tree_minimum(t, z->right);
        y_original_color = y->color;
        x = y->right;
        if (y->parent == z) {
            x->parent = y;
        } else {
            transplant(t, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(t, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    /* z is fully unlinked here -- every incoming pointer was already
     * rewritten by the transplant(s) above -- so it's freed now, strictly
     * before delete_fixup runs; nothing past this point may dereference z */
    rb_free(z->key);
    if (t->value_free != NULL) t->value_free(z->value);
    rb_free(z);

    if (y_original_color == BLACK) {
        delete_fixup(t, x);
    }
    t->size--;
    return 0;
}

static void rb_foreach_node(const struct rbtree *t, const struct rb_node *n,
                             void (*fn)(const char *key, void *value, void *ctx), void *ctx) {
    if (n == &t->nil) return;          /* base case: sentinel marks "no node" */
    rb_foreach_node(t, n->left, fn, ctx);
    fn(n->key, n->value, ctx);
    rb_foreach_node(t, n->right, fn, ctx);
}

void rb_foreach(const rbtree_t *t, void (*fn)(const char *key, void *value, void *ctx),
                 void *ctx) {
    rb_foreach_node(t, t->root, fn, ctx);
}


