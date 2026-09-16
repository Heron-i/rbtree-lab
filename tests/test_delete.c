//Purpose: Unit tests for rb_delete, including every delete-fixup case.
// RBD-01..RBD-07 moved here verbatim from tests/test_rbtree.c, now that
// rb_delete has its own dedicated test file -- matching how
// tests/test_insert.c already exists standalone for rb_insert. They cover:
// a red leaf (01), a black leaf with a red sibling and its mirror (02, 07),
// a node with two children (03), the root of a one-node tree (04), and a
// black node with exactly one red child, both sides (05, 06). Every
// fixture built via more than one insert has its pre-delete shape/colors
// asserted through rbtree_internal.h before rb_delete is called, rather
// than relying on a hand-traced comment alone -- the same technique
// tests/test_insert.c's RBI-08..12 use for insert_fixup.
// RBD-08..RBD-16 are new. RBD-08 checks the documented "-1, absent key"
// contract path (previously untested). RBD-09/10 isolate delete-fixup case
// 2 -- a single fire, then two-level propagation all the way to the root
// -- from any case-1 chaining. RBD-11..14 isolate case-3-then-4 and
// case-4-direct, both mirrors. RBD-15 mirrors RBX-04's value_free-call-
// count technique for rb_delete itself. RBD-16 is a broad sequential
// delete-to-empty regression net, validated after every step. Every new
// fixture's pre-delete shape, resulting fixup-case path, and post-delete
// shape were verified against a scratch reference re-implementation of the
// planned CLRS algorithm before being written down here -- see the
// rb_delete design plan.
// RBD-10 is the one exception to "derive the fixture from a real rb_insert
// sequence": no insertion order (exhaustively checked up to 7 keys, and
// randomly sampled up to 20-node trees) produces the two-level, all-black
// sibling structure needed to see delete-fixup's case 2 fire twice in a
// row and terminate at the true root -- insert_fixup tends to leave red
// nodes at the fringes. That one fixture is instead built by direct field
// construction through rbtree_internal.h, the same technique
// tests/test_rbtree.c's RBV-02/04/06/08/10 already use to force a specific
// precondition; rb_validate() is asserted on it before rb_delete is ever
// called, exactly like those tests verify their starting point. Its
// allocation-failure cleanup frees each already-allocated node's key copy
// *and* the node struct itself -- neither leaks, since rb_destroy(t) only
// owns nodes that were actually linked into t (they aren't yet, at that
// point).
#include "rbtree.h"
#include "../src/rbtree_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* value_free test double for RBD-15: records what it was called with so we
 * can confirm exactly the deleted node's value (and nothing else) was
 * freed. Independent copy, not shared with tests/test_rbtree.c's. */
#define FREE_RECORDER_CAP 4
static void *free_recorder_seen[FREE_RECORDER_CAP];
static int free_recorder_count;

static void free_recorder(void *value) {
    if (free_recorder_count < FREE_RECORDER_CAP) {
        free_recorder_seen[free_recorder_count] = value;
    }
    free_recorder_count++;
}

static void free_recorder_reset(void) {
    free_recorder_count = 0;
}

static bool free_recorder_saw(void *value) {
    int limit = free_recorder_count < FREE_RECORDER_CAP ? free_recorder_count : FREE_RECORDER_CAP;
    for (int i = 0; i < limit; i++) {
        if (free_recorder_seen[i] == value) return true;
    }
    return false;
}

/* ---- RBD-01: delete a red leaf ---- */
static bool test_rbd01_delete_red_leaf(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    /* "03" is a red leaf after inserting 10,05,15,03 (standard RB
     * insert-fixup; keys are zero-padded to 2 digits so strcmp order
     * matches numeric order -- see the note above build_baseline_tree) */
    int rc = rb_delete(t, "03");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 3);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"03\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 3)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-02: delete a black leaf whose sibling is red ----
 * Hand-traced against the standard CLRS insert-fixup algorithm (see the
 * rb_delete test plan): inserting 50,30,70,60,80,55 yields
 * 50B(30B(leaf), 70R(60B(55R,nil), 80B(leaf))). Structure/colors are
 * asserted below via rbtree_internal.h before the delete, so this doesn't
 * rely on a hand-traced comment alone. */
static bool test_rbd02_delete_black_leaf_red_sibling(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "30", "70", "60", "80", "55"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    /* confirm the fixture actually has the claimed shape before deleting */
    bool shape = (strcmp(t->root->key, "50") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "30") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
                 (strcmp(t->root->right->key, "70") == 0) && (t->root->right->color == RED) &&
                 (strcmp(t->root->right->left->key, "60") == 0) &&
                 (t->root->right->left->color == BLACK) &&
                 (strcmp(t->root->right->left->left->key, "55") == 0) &&
                 (t->root->right->left->left->color == RED) &&
                 (t->root->right->left->right == &t->nil) &&
                 (strcmp(t->root->right->right->key, "80") == 0) &&
                 (t->root->right->right->color == BLACK) &&
                 (t->root->right->right->left == &t->nil) &&
                 (t->root->right->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 50,30,70,60,80,55 did not match the expected shape "
                "50B(30B(leaf),70R(60B(55R,nil),80B(leaf))); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "30");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 5);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"30\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 5)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-03: delete a node with two children ---- */
static bool test_rbd03_delete_two_children(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    /* "05" has two real children ("03" and "07") after this sequence */
    int rc = rb_delete(t, "05");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 4);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"05\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 4)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-04: delete the root of a one-node tree ---- */
static bool test_rbd04_delete_root(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    if (rb_insert(t, "10", NULL) != 0) {
        fprintf(stderr, "    rb_insert(\"10\") failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "10");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 0);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"10\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 0)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-05: delete a black node with exactly one red child (left) ----
 * Hand-traced: inserting 50,30,70,60 yields 50B(30B(leaf),70B(60R(leaf),nil))
 * -- "70" is black with a single red child "60" on the left. */
static bool test_rbd05_delete_black_one_red_child_left(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "30", "70", "60"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "50") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "30") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
                 (strcmp(t->root->right->key, "70") == 0) && (t->root->right->color == BLACK) &&
                 (strcmp(t->root->right->left->key, "60") == 0) &&
                 (t->root->right->left->color == RED) && (t->root->right->left->left == &t->nil) &&
                 (t->root->right->left->right == &t->nil) && (t->root->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 50,30,70,60 did not match the expected shape "
                "50B(30B(leaf),70B(60R(leaf),nil)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "70");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 3);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"70\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 3)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-06: delete a black node with exactly one red child (right) ----
 * Mirror of RBD-05. Hand-traced: inserting 50,70,30,40 yields
 * 50B(30B(nil,40R),70B(leaf)) -- "30" is black with a single red child "40"
 * on the right. */
static bool test_rbd06_delete_black_one_red_child_right(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "70", "30", "40"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "50") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "30") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) &&
                 (strcmp(t->root->left->right->key, "40") == 0) &&
                 (t->root->left->right->color == RED) && (t->root->left->right->left == &t->nil) &&
                 (t->root->left->right->right == &t->nil) &&
                 (strcmp(t->root->right->key, "70") == 0) && (t->root->right->color == BLACK) &&
                 (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 50,70,30,40 did not match the expected shape "
                "50B(30B(nil,40R),70B(leaf)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "30");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 3);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"30\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 3)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-07: delete a black leaf whose sibling is red (mirror of RBD-02) ----
 * Hand-traced: inserting 50,70,30,40,20,45 yields
 * 50B(30R(20B,40B(nil,45R)), 70B(leaf)). */
static bool test_rbd07_delete_black_leaf_red_sibling_mirror(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "70", "30", "40", "20", "45"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "50") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->right->key, "70") == 0) && (t->root->right->color == BLACK) &&
                 (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
                 (strcmp(t->root->left->key, "30") == 0) && (t->root->left->color == RED) &&
                 (strcmp(t->root->left->left->key, "20") == 0) &&
                 (t->root->left->left->color == BLACK) && (t->root->left->left->left == &t->nil) &&
                 (t->root->left->left->right == &t->nil) &&
                 (strcmp(t->root->left->right->key, "40") == 0) &&
                 (t->root->left->right->color == BLACK) && (t->root->left->right->left == &t->nil) &&
                 (strcmp(t->root->left->right->right->key, "45") == 0) &&
                 (t->root->left->right->right->color == RED) &&
                 (t->root->left->right->right->left == &t->nil) &&
                 (t->root->left->right->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 50,70,30,40,20,45 did not match the expected shape "
                "50B(30R(20B,40B(nil,45R)),70B(leaf)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "70");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 5);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"70\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 5)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-08: delete a key not present ---- */
static bool test_rbd08_delete_missing_key(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_a, tag_b, tag_c;
    struct {
        const char *key;
        void *value;
    } entries[] = {
        {"10", &tag_a}, {"05", &tag_b}, {"15", &tag_c},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (rb_insert(t, entries[i].key, entries[i].value) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n",
                    entries[i].key);
            rb_destroy(t);
            return false;
        }
    }
    int rc = rb_delete(t, "99");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool untouched = (rb_find(t, "10") == &tag_a) && (rb_find(t, "05") == &tag_b) &&
                      (rb_find(t, "15") == &tag_c);
    bool ok = (rc == -1) && (vrc == 0) && (n == 3) && untouched;
    if (!ok) {
        fprintf(stderr,
                "    delete(\"99\") absent key: rb_delete rc=%d (want -1), rb_validate=%d "
                "(want 0), rb_size=%zu (want 3), existing keys still findable=%d\n",
                rc, vrc, n, untouched);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-09: delete-fixup case 2 in isolation -- black leaf, black
 * sibling, both nephews black; the fixup loop fires once and stops because
 * the extra black lands on a red parent ----
 * Hand-traced against the planned CLRS RB-delete-fixup algorithm (see the
 * design plan): inserting 10,20,30,40,50,60,70,80 yields
 * 40B(20R(10B,30B),60R(50B,70B(nil,80R))). Deleting "10" isolates case 2
 * from any case-1 chaining -- RBD-02/07 only ever reach later cases
 * *through* case 1. */
static bool test_rbd09_case2_isolated(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "20", "30", "40", "50", "60", "70", "80"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "40") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "20") == 0) && (t->root->left->color == RED) &&
                 (strcmp(t->root->left->left->key, "10") == 0) &&
                 (t->root->left->left->color == BLACK) && (t->root->left->left->left == &t->nil) &&
                 (t->root->left->left->right == &t->nil) &&
                 (strcmp(t->root->left->right->key, "30") == 0) &&
                 (t->root->left->right->color == BLACK) && (t->root->left->right->left == &t->nil) &&
                 (t->root->left->right->right == &t->nil) &&
                 (strcmp(t->root->right->key, "60") == 0) && (t->root->right->color == RED) &&
                 (strcmp(t->root->right->left->key, "50") == 0) &&
                 (t->root->right->left->color == BLACK) &&
                 (t->root->right->left->left == &t->nil) && (t->root->right->left->right == &t->nil) &&
                 (strcmp(t->root->right->right->key, "70") == 0) &&
                 (t->root->right->right->color == BLACK) && (t->root->right->right->left == &t->nil) &&
                 (strcmp(t->root->right->right->right->key, "80") == 0) &&
                 (t->root->right->right->right->color == RED) &&
                 (t->root->right->right->right->left == &t->nil) &&
                 (t->root->right->right->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 10,20,30,40,50,60,70,80 did not match the expected shape "
                "40B(20R(10B,30B),60R(50B,70B(nil,80R))); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "10");
    bool ok = (rc == 0) && (strcmp(t->root->key, "40") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "20") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (strcmp(t->root->left->right->key, "30") == 0) &&
              (t->root->left->right->color == RED) && (t->root->left->right->left == &t->nil) &&
              (t->root->left->right->right == &t->nil) &&
              (strcmp(t->root->right->key, "60") == 0) && (t->root->right->color == RED) &&
              (strcmp(t->root->right->left->key, "50") == 0) &&
              (t->root->right->left->color == BLACK) &&
              (strcmp(t->root->right->right->key, "70") == 0) &&
              (t->root->right->right->color == BLACK) && (t->root->right->right->left == &t->nil) &&
              (strcmp(t->root->right->right->right->key, "80") == 0) &&
              (t->root->right->right->right->color == RED);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 7);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"10\"): tree did not match the expected post-delete shape "
                "40B(20B(nil,30R),60R(50B,70B(nil,80R))); rb_delete rc=%d (want 0), "
                "rb_validate=%d, rb_size=%zu (want 7)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* Hand-builds one node of RBD-10's fixture: a heap key copy plus the
 * fields node_alloc would set, except color and linkage, which the caller
 * fills in -- mirrors src/rbtree.c's node_alloc layout so rb_destroy can
 * free it the normal way once linked in. Returns NULL (nothing to clean
 * up) on malloc failure. */
static struct rb_node *rbd10_make_node(const char *key, rb_color_t color, struct rb_node *nil) {
    struct rb_node *n = malloc(sizeof *n);
    if (n == NULL) return NULL;
    size_t len = strlen(key) + 1;
    char *key_copy = malloc(len);
    if (key_copy == NULL) {
        free(n);
        return NULL;
    }
    memcpy(key_copy, key, len);
    n->key = key_copy;
    n->value = NULL;
    n->left = nil;
    n->right = nil;
    n->parent = nil;
    n->color = color;
    return n;
}

/* Frees one RBD-10 node allocated by rbd10_make_node that never got linked
 * into a tree (so rb_destroy will never reach it) -- frees the key copy
 * *and* the node struct, and is a no-op on NULL. Only for the
 * allocation-failure cleanup path below; a node that made it into `t` is
 * rb_destroy's responsibility instead. */
static void rbd10_free_unlinked_node(struct rb_node *n) {
    if (n == NULL) return;
    free(n->key);
    free(n);
}

/* ---- RBD-10: delete-fixup case 2 propagating across two levels,
 * terminating exactly at the root ----
 * This is the one fixture that can't be reached through rb_insert: an
 * exhaustive search over every relative key ordering up to 7 keys, plus a
 * wide randomized search up to 20-node trees, found no natural insertion
 * sequence producing two consecutive case-2 firings -- the structural
 * precondition (a black node whose whole sibling subtree is also all-black,
 * two levels running) essentially never arises from insert_fixup, which
 * tends to leave red nodes at the fringes. Built by direct field
 * construction instead, matching how tests/test_rbtree.c's
 * RBV-02/04/06/08/10 already force preconditions; rb_validate() is
 * asserted on it below before rb_delete is ever called, exactly like those
 * tests verify their starting point. */
static bool test_rbd10_case2_propagates_to_root(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    struct rb_node *n10 = rbd10_make_node("10", BLACK, &t->nil);
    struct rb_node *n20 = rbd10_make_node("20", BLACK, &t->nil);
    struct rb_node *n15 = rbd10_make_node("15", BLACK, &t->nil);
    struct rb_node *n40 = rbd10_make_node("40", BLACK, &t->nil);
    struct rb_node *n60 = rbd10_make_node("60", BLACK, &t->nil);
    struct rb_node *n50 = rbd10_make_node("50", BLACK, &t->nil);
    struct rb_node *n30 = rbd10_make_node("30", BLACK, &t->nil);
    if (!n10 || !n20 || !n15 || !n40 || !n60 || !n50 || !n30) {
        fprintf(stderr, "    malloc failed while hand-building the RBD-10 fixture\n");
        /* None of these are linked into `t` yet -- rb_destroy(t) below only
         * owns whatever `t` already has (nothing), so each already-
         * allocated node's key copy and struct must both be freed here by
         * hand, or they leak. */
        rbd10_free_unlinked_node(n10);
        rbd10_free_unlinked_node(n20);
        rbd10_free_unlinked_node(n15);
        rbd10_free_unlinked_node(n40);
        rbd10_free_unlinked_node(n60);
        rbd10_free_unlinked_node(n50);
        rbd10_free_unlinked_node(n30);
        rb_destroy(t);
        return false;
    }
    /* 30B(15B(10B,20B),50B(40B,60B)) -- a fully valid, all-black 7-node
     * tree, linked by hand */
    n15->left = n10;
    n15->right = n20;
    n10->parent = n15;
    n20->parent = n15;
    n50->left = n40;
    n50->right = n60;
    n40->parent = n50;
    n60->parent = n50;
    n30->left = n15;
    n30->right = n50;
    n15->parent = n30;
    n50->parent = n30;
    n30->parent = &t->nil;
    t->root = n30;
    t->size = 7;

    int pre_vrc = rb_validate(t);
    bool shape = (pre_vrc == 0) && (strcmp(t->root->key, "30") == 0) &&
                 (t->root->color == BLACK) && (strcmp(t->root->left->key, "15") == 0) &&
                 (t->root->left->color == BLACK) &&
                 (strcmp(t->root->left->left->key, "10") == 0) &&
                 (t->root->left->left->color == BLACK) &&
                 (strcmp(t->root->left->right->key, "20") == 0) &&
                 (t->root->left->right->color == BLACK) &&
                 (strcmp(t->root->right->key, "50") == 0) && (t->root->right->color == BLACK) &&
                 (strcmp(t->root->right->left->key, "40") == 0) &&
                 (t->root->right->left->color == BLACK) &&
                 (strcmp(t->root->right->right->key, "60") == 0) &&
                 (t->root->right->right->color == BLACK);
    if (!shape) {
        fprintf(stderr,
                "    hand-built fixture did not match the expected all-black shape "
                "30B(15B(10B,20B),50B(40B,60B)); rb_validate=%d (want 0); rb_delete not "
                "exercised\n",
                pre_vrc);
        rb_destroy(t);   /* all 7 nodes are linked into t->root now -- t owns them */
        return false;
    }

    int rc = rb_delete(t, "10");
    bool ok = (rc == 0) && (strcmp(t->root->key, "30") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "15") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (strcmp(t->root->left->right->key, "20") == 0) &&
              (t->root->left->right->color == RED) && (strcmp(t->root->right->key, "50") == 0) &&
              (t->root->right->color == RED) && (strcmp(t->root->right->left->key, "40") == 0) &&
              (t->root->right->left->color == BLACK) &&
              (strcmp(t->root->right->right->key, "60") == 0) &&
              (t->root->right->right->color == BLACK);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 6);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"10\"): tree did not match the expected post-delete shape "
                "30B(15B(nil,20R),50R(40B,60B)); rb_delete rc=%d (want 0), rb_validate=%d, "
                "rb_size=%zu (want 6)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-11: delete-fixup case 3 then case 4, left side ----
 * Hand-traced against the planned CLRS RB-delete-fixup algorithm (see the
 * design plan): inserting 10,20,40,30 yields
 * 20B(10B(nil,nil),40B(30R(nil,nil),nil)). Deleting "10" hits case 3 (near
 * nephew "30" red, far nephew nil/black) which rotates at the sibling to
 * convert into case 4. */
static bool test_rbd11_case3_then_case4_left(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "20", "40", "30"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "20") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
                 (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
                 (strcmp(t->root->right->left->key, "30") == 0) &&
                 (t->root->right->left->color == RED) && (t->root->right->left->left == &t->nil) &&
                 (t->root->right->left->right == &t->nil) && (t->root->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 10,20,40,30 did not match the expected shape "
                "20B(10B(nil,nil),40B(30R(nil,nil),nil)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "10");
    bool ok = (rc == 0) && (strcmp(t->root->key, "30") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "20") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 3);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"10\"): tree did not match the expected post-delete shape "
                "30B(20B(nil,nil),40B(nil,nil)); rb_delete rc=%d (want 0), rb_validate=%d, "
                "rb_size=%zu (want 3)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-12: delete-fixup case 3 then case 4, right side (mirror of
 * RBD-11) ----
 * Inserting 10,30,40,20 yields 30B(10B(nil,20R(nil,nil)),40B(nil,nil)).
 * Deleting "30" -- the root, with two children -- routes through the
 * two-children/successor-splice path (successor "40" is z's direct right
 * child, the y->parent == z branch) at the same time as case 3 (near
 * nephew "20" red, far nephew nil/black) rotating into case 4. */
static bool test_rbd12_case3_then_case4_right(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "30", "40", "20"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "30") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) && (strcmp(t->root->left->right->key, "20") == 0) &&
                 (t->root->left->right->color == RED) && (t->root->left->right->left == &t->nil) &&
                 (t->root->left->right->right == &t->nil) &&
                 (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
                 (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 10,30,40,20 did not match the expected shape "
                "30B(10B(nil,20R(nil,nil)),40B(nil,nil)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "30");
    bool ok = (rc == 0) && (strcmp(t->root->key, "20") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 3);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"30\"): tree did not match the expected post-delete shape "
                "20B(10B(nil,nil),40B(nil,nil)); rb_delete rc=%d (want 0), rb_validate=%d, "
                "rb_size=%zu (want 3)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-13: delete-fixup case 4 directly (far nephew already red), left
 * side ----
 * Inserting 10,20,30,40 yields 20B(10B(nil,nil),30B(nil,40R(nil,nil))).
 * Deleting "10": sibling "30"'s far nephew "40" is already red, so case 4
 * fires directly, without first passing through case 3. */
static bool test_rbd13_case4_direct_left(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "20", "30", "40"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "20") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == BLACK) &&
                 (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
                 (strcmp(t->root->right->key, "30") == 0) && (t->root->right->color == BLACK) &&
                 (t->root->right->left == &t->nil) &&
                 (strcmp(t->root->right->right->key, "40") == 0) &&
                 (t->root->right->right->color == RED) && (t->root->right->right->left == &t->nil) &&
                 (t->root->right->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 10,20,30,40 did not match the expected shape "
                "20B(10B(nil,nil),30B(nil,40R(nil,nil))); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "10");
    bool ok = (rc == 0) && (strcmp(t->root->key, "30") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "20") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 3);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"10\"): tree did not match the expected post-delete shape "
                "30B(20B(nil,nil),40B(nil,nil)); rb_delete rc=%d (want 0), rb_validate=%d, "
                "rb_size=%zu (want 3)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-14: delete-fixup case 4 directly, right side (mirror of
 * RBD-13) ----
 * Inserting 20,30,40,10 yields 30B(20B(10R(nil,nil),nil),40B(nil,nil)).
 * Deleting "30" -- again the root, two children, successor "40" is z's
 * direct right child (y->parent == z branch) -- sibling "20"'s far nephew
 * "10" is already red, so case 4 fires directly. */
static bool test_rbd14_case4_direct_right(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"20", "30", "40", "10"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool shape = (strcmp(t->root->key, "30") == 0) && (t->root->color == BLACK) &&
                 (strcmp(t->root->left->key, "20") == 0) && (t->root->left->color == BLACK) &&
                 (strcmp(t->root->left->left->key, "10") == 0) &&
                 (t->root->left->left->color == RED) && (t->root->left->left->left == &t->nil) &&
                 (t->root->left->left->right == &t->nil) && (t->root->left->right == &t->nil) &&
                 (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
                 (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    if (!shape) {
        fprintf(stderr,
                "    fixture 20,30,40,10 did not match the expected shape "
                "30B(20B(10R(nil,nil),nil),40B(nil,nil)); rb_delete not exercised\n");
        rb_destroy(t);
        return false;
    }
    int rc = rb_delete(t, "30");
    bool ok = (rc == 0) && (strcmp(t->root->key, "20") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == BLACK) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "40") == 0) && (t->root->right->color == BLACK) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 3);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"30\"): tree did not match the expected post-delete shape "
                "20B(10B(nil,nil),40B(nil,nil)); rb_delete rc=%d (want 0), rb_validate=%d, "
                "rb_size=%zu (want 3)\n",
                rc, rb_validate(t), rb_size(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-15: value_free called exactly once, with the correct pointer,
 * on delete ----
 * Pass condition explicitly requires free_recorder_count == 1 (not just
 * "saw the deleted value"): seeing the right pointer while the call count
 * is wrong -- e.g. a double free_recorder call for the same value, or an
 * extra spurious call for another node -- must still fail this test. */
static bool test_rbd15_value_free_called_on_delete(void) {
    rbtree_t *t = rb_create(free_recorder);
    if (!t) {
        fprintf(stderr, "    rb_create(free_recorder) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_a, tag_b, tag_c;
    struct {
        const char *key;
        void *value;
    } entries[] = {
        {"10", &tag_a}, {"05", &tag_b}, {"15", &tag_c},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (rb_insert(t, entries[i].key, entries[i].value) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n",
                    entries[i].key);
            rb_destroy(t);
            return false;
        }
    }
    free_recorder_reset();
    int rc = rb_delete(t, "05");
    bool ok = (rc == 0) && (free_recorder_count == 1) && free_recorder_saw(&tag_b) &&
              !free_recorder_saw(&tag_a) && !free_recorder_saw(&tag_c);
    ok = ok && (rb_validate(t) == 0) && (rb_size(t) == 2);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    delete(\"05\"): rb_delete rc=%d (want 0), free_recorder_count=%d (want 1), "
                "saw deleted value=%d (want 1), saw untouched values=%d/%d (want 0/0)\n",
                rc, free_recorder_count, free_recorder_saw(&tag_b), free_recorder_saw(&tag_a),
                free_recorder_saw(&tag_c));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-16: sequential delete-to-empty -- broad regression net,
 * rb_validate and rb_size asserted after every deletion, not just at the
 * end ---- */
static bool test_rbd16_sequential_delete_to_empty(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *insert_keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(insert_keys) / sizeof(insert_keys[0]); i++) {
        if (rb_insert(t, insert_keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n",
                    insert_keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    const char *delete_keys[] = {"03", "15", "07", "10", "05"};
    size_t n = sizeof(delete_keys) / sizeof(delete_keys[0]);
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        int rc = rb_delete(t, delete_keys[i]);
        int vrc = rb_validate(t);
        size_t remaining = rb_size(t);
        size_t want = n - (i + 1);
        ok = (rc == 0) && (vrc == 0) && (remaining == want);
        if (!ok) {
            fprintf(stderr,
                    "    step %zu, delete(\"%s\"): rb_delete rc=%d (want 0), rb_validate=%d "
                    "(want 0), rb_size=%zu (want %zu)\n",
                    i, delete_keys[i], rc, vrc, remaining, want);
        }
    }
    rb_destroy(t);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBD-01", "delete a red leaf", test_rbd01_delete_red_leaf},
    {"RBD-02", "delete a black leaf whose sibling is red",
     test_rbd02_delete_black_leaf_red_sibling},
    {"RBD-03", "delete a node with two children", test_rbd03_delete_two_children},
    {"RBD-04", "delete the root of a one-node tree", test_rbd04_delete_root},
    {"RBD-05", "delete a black node with exactly one red child (left)",
     test_rbd05_delete_black_one_red_child_left},
    {"RBD-06", "delete a black node with exactly one red child (right)",
     test_rbd06_delete_black_one_red_child_right},
    {"RBD-07", "delete a black leaf whose sibling is red (mirror)",
     test_rbd07_delete_black_leaf_red_sibling_mirror},
    {"RBD-08", "delete a key not present", test_rbd08_delete_missing_key},
    {"RBD-09", "delete-fixup case 2 in isolation, single pass", test_rbd09_case2_isolated},
    {"RBD-10", "delete-fixup case 2 propagating to the root",
     test_rbd10_case2_propagates_to_root},
    {"RBD-11", "delete-fixup case 3 then case 4, left side",
     test_rbd11_case3_then_case4_left},
    {"RBD-12", "delete-fixup case 3 then case 4, right side (mirror)",
     test_rbd12_case3_then_case4_right},
    {"RBD-13", "delete-fixup case 4 directly, left side", test_rbd13_case4_direct_left},
    {"RBD-14", "delete-fixup case 4 directly, right side (mirror)",
     test_rbd14_case4_direct_right},
    {"RBD-15", "value_free called exactly once on delete",
     test_rbd15_value_free_called_on_delete},
    {"RBD-16", "sequential delete-to-empty", test_rbd16_sequential_delete_to_empty},
};

int main(void) {
    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;
    for (size_t i = 0; i < total; i++) {
        bool ok = tests[i].run();
        printf("[%s] %-55s %s\n", tests[i].id, tests[i].name, ok ? "PASS" : "FAIL");
        if (ok) passed++;
    }
    printf("%zu/%zu tests passed\n", passed, total);
    return (passed == total) ? EXIT_SUCCESS : EXIT_FAILURE;
}
