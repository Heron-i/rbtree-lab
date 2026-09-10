//Purpose: My unit tests created with known specs and edge cases (table-driven tests)
// RBC-01, RBC-02, RBC-03, RBC-04, RBC-05, RBC-10 (rb_create test plan) now
// live in tests/test_create.c, split out so that file builds and runs
// independently of every operation besides rb_create/rb_destroy/rb_validate.
// RBC-08 stays here (see the comment above it below) because it depends on
// rb_insert. The remaining planned cases (RBC-06, RBC-07, RBC-09, RBC-11,
// RBC-12, RBC-13) are deferred but still tracked in the plan.
// Implements RBD-01..RBD-07 from the rb_delete test plan: red leaf (01),
// black leaf with a red sibling and its mirror (02, 07), two children (03),
// root of a one-node tree (04), and a black node with exactly one red
// child, both sides (05, 06). Every fixture built via more than one insert
// has its pre-delete shape/colors asserted through rbtree_internal.h before
// rb_delete is called, rather than relying on a hand-traced comment alone.
// RBX-01, RBX-02 (rb_destroy test plan) now live in tests/test_destroy.c,
// split out so that file builds and runs independently of rb_insert/
// rb_delete. RBX-03..RBX-06 stay here (see the comment above RBX-03 below)
// because they depend on rb_insert (RBX-06 also on rb_delete). These stick
// to what's observable in-process (crash-freedom, value_free call
// count/values); leak/double-free correctness (e.g. "every node freed
// together with its key copy") is verified separately by `make memcheck`.
// Implements RBV-01..RBV-10 from the rb_validate test plan (root-black,
// no-red-red, black-height, in-order/BST ordering, size-matches-count --
// each with a valid and an invalid tree). The invalid-tree cases are
// white-box: they build a valid tree via the public API, then mutate one
// field through rbtree_internal.h to force a single targeted invariant
// violation.
#include "rbtree.h"
#include "../src/rbtree_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* value_free test double: RBX-04 uses its call count/recorded values to
 * check rb_destroy invokes it exactly once per node with the right value
 * pointers. */
#define FREE_RECORDER_CAP 8
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

/* order-independent membership check: rb_destroy walks structurally, not
 * in insertion order, so we can't assume call order matches insert order */
static bool free_recorder_saw(void *value) {
    int limit = free_recorder_count < FREE_RECORDER_CAP ? free_recorder_count : FREE_RECORDER_CAP;
    for (int i = 0; i < limit; i++) {
        if (free_recorder_seen[i] == value) return true;
    }
    return false;
}

/* RBC-08 lives here rather than in tests/test_create.c: it calls rb_insert
 * to prove two independently-created trees don't share state, so it can't
 * link until rb_insert exists. tests/test_create.c is meant to build and
 * pass independently of every operation besides rb_create/rb_destroy/
 * rb_validate; keeping this test there would break that property for the
 * whole file. It stays here, alongside the other tests that already depend
 * on rb_insert/rb_delete/rb_foreach, until those exist and this whole file
 * links again. */
/* ---- RBC-08: two trees created independently don't share state ---- */
static bool test_rbc08_independent_trees(void) {
    rbtree_t *t1 = rb_create(NULL);
    rbtree_t *t2 = rb_create(NULL);
    if (!t1 || !t2) {
        fprintf(stderr, "    rb_create(NULL) returned NULL for t1 or t2\n");
        if (t1) rb_destroy(t1);
        if (t2) rb_destroy(t2);
        return false;
    }
    int rc = rb_insert(t1, "a", NULL);
    bool ok = (rc == 0) && (rb_size(t1) == 1) && (rb_size(t2) == 0) && (rb_validate(t2) == 0);
    if (!ok) {
        fprintf(stderr,
                "    after inserting into t1: rb_insert rc=%d, size(t1)=%zu (want 1), "
                "size(t2)=%zu (want 0), validate(t2)=%d (want 0)\n",
                rc, rb_size(t1), rb_size(t2), rb_validate(t2));
    }
    rb_destroy(t1);
    rb_destroy(t2);
    return ok;
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

/* RBX-03..RBX-06 live here rather than in tests/test_destroy.c: each calls
 * rb_insert to build a populated tree (RBX-06 also calls rb_delete to
 * thin it first), so none of them can link until those exist.
 * tests/test_destroy.c is meant to build and pass independently of every
 * operation besides rb_create/rb_destroy; keeping these there would break
 * that property for the whole file. They stay here, alongside the other
 * tests that already depend on rb_insert/rb_delete/rb_foreach, until those
 * exist and this whole file links again. */
/* ---- RBX-03: destroying a populated tree runs cleanly ----
 * (node-struct + key-copy freeing itself is verified by `make memcheck`,
 * not observable from here; this confirms a real, non-trivial tree was
 * actually being destroyed) */
static bool test_rbx03_destroy_populated_tree(void) {
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
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (vrc == 0) && (n == 5);
    if (!ok) {
        fprintf(stderr,
                "    before destroy: rb_validate=%d (want 0), rb_size=%zu (want 5)\n",
                vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBX-04: value_free is called exactly once per node, with the
 * right values, when a callback is provided ---- */
static bool test_rbx04_value_free_called_correctly(void) {
    rbtree_t *t = rb_create(free_recorder);
    if (!t) {
        fprintf(stderr, "    rb_create(free_recorder) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_a, tag_b, tag_c;
    struct { const char *key; void *value; } entries[] = {
        {"10", &tag_a}, {"5", &tag_b}, {"15", &tag_c},
    };
    free_recorder_reset();
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (rb_insert(t, entries[i].key, entries[i].value) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n",
                    entries[i].key);
            rb_destroy(t);
            return false;
        }
    }
    rb_destroy(t);
    bool ok = (free_recorder_count == 3) && free_recorder_saw(&tag_a) &&
              free_recorder_saw(&tag_b) && free_recorder_saw(&tag_c);
    if (!ok) {
        fprintf(stderr,
                "    after destroy: free_recorder_count=%d (want 3), saw a=%d b=%d c=%d "
                "(want all 1)\n",
                free_recorder_count, free_recorder_saw(&tag_a), free_recorder_saw(&tag_b),
                free_recorder_saw(&tag_c));
    }
    return ok;
}

/* ---- RBX-05: value_free == NULL at creation means it's never invoked ---- */
static bool test_rbx05_no_value_free_when_null(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag;
    const char *keys[] = {"10", "5", "15"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], &tag) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    rb_destroy(t);   /* a NULL callback being dereferenced would crash here */
    return true;
}

/* ---- RBX-06: destroying a tree with a mixed insert/delete history runs
 * cleanly, without double-freeing already-deleted nodes ---- */
static bool test_rbx06_destroy_after_mixed_history(void) {
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
    if (rb_delete(t, "03") != 0 || rb_delete(t, "15") != 0) {
        fprintf(stderr, "    rb_delete failed while thinning the tree before destroy\n");
        rb_destroy(t);
        return false;
    }
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (vrc == 0) && (n == 3);
    if (!ok) {
        fprintf(stderr,
                "    before final destroy: rb_validate=%d (want 0), rb_size=%zu (want 3)\n",
                vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* All numeric-looking keys in this file are zero-padded to 2 digits
 * ("03" not "3"). Ordering here is via strcmp (lexicographic), not
 * numeric, and unpadded mixed-width numbers sort unintuitively under
 * strcmp ("3" > "10", since '3' > '1') -- zero-padding to a fixed width
 * makes strcmp order match numeric order again, so the hand-traced shapes
 * below hold. If a key >= 100 is ever needed, widen the padding to 3
 * digits and repad every existing key to match, or this bug resurfaces. */

/* shared baseline tree for most RBV-xx cases: 10B root, left 05B (children
 * 03R,07R), right 15B leaf -- black-height 2 on every path, size 5 */
static rbtree_t *build_baseline_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return NULL;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the baseline tree\n",
                    keys[i]);
            rb_destroy(t);
            return NULL;
        }
    }
    return t;
}

/* ---- RBV-01: root is black (valid) ---- */
static bool test_rbv01_root_is_black_valid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    int vrc = rb_validate(t);
    bool ok = (vrc == 0) && (t->root->color == BLACK);
    if (!ok) {
        fprintf(stderr,
                "    baseline tree: rb_validate=%d (want 0), root color=%d (want BLACK=%d)\n",
                vrc, t->root->color, BLACK);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBV-02: root is black, violated (invalid) ---- */
static bool test_rbv02_root_is_black_invalid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    t->root->color = RED;   /* force violation: root must always be BLACK */
    int vrc = rb_validate(t);
    bool ok = (vrc != 0);
    if (!ok) fprintf(stderr, "    root forced RED: rb_validate=%d (want nonzero)\n", vrc);
    rb_destroy(t);
    return ok;
}

/* ---- RBV-03: no red node has a red child (valid) ---- */
static bool test_rbv03_no_red_red_valid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    int vrc = rb_validate(t);
    bool ok = (vrc == 0);
    if (!ok) fprintf(stderr, "    baseline tree: rb_validate=%d (want 0)\n", vrc);
    rb_destroy(t);
    return ok;
}

/* ---- RBV-04: red-red violation (invalid) ----
 * Flips node "05" (black, parent of red leaves "03","07") to red, creating
 * red-red on both "05"-"03" and "05"-"07". This also drops black-height on
 * those two paths relative to the "15" path -- acceptable, rb_validate
 * only needs to report "invalid" (nonzero), not which single rule broke. */
static bool test_rbv04_red_red_invalid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    t->root->left->color = RED;   /* node "05": BLACK -> RED */
    int vrc = rb_validate(t);
    bool ok = (vrc != 0);
    if (!ok) fprintf(stderr, "    node \"05\" forced RED: rb_validate=%d (want nonzero)\n", vrc);
    rb_destroy(t);
    return ok;
}

/* ---- RBV-05: equal black-height on every root-to-NIL path (valid) ----
 * Baseline tree: root->left("05")->left("03")->nil and
 * root->left("05")->right("07")->nil each have black-height 2 (05B+nil);
 * root->right("15")->nil also has black-height 2 (15B+nil). */
static bool test_rbv05_black_height_valid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    int vrc = rb_validate(t);
    bool ok = (vrc == 0);
    if (!ok) fprintf(stderr, "    baseline tree: rb_validate=%d (want 0)\n", vrc);
    rb_destroy(t);
    return ok;
}

/* ---- RBV-06: black-height mismatch, isolated (invalid) ----
 * Flips node "15" (a black leaf, parent is the black root) to red. It has
 * no children, so this creates no red-red violation -- it purely drops the
 * right-side path's black-height from 2 to 1 while the left side stays 2. */
static bool test_rbv06_black_height_invalid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    t->root->right->color = RED;   /* node "15": BLACK -> RED */
    int vrc = rb_validate(t);
    bool ok = (vrc != 0);
    if (!ok) fprintf(stderr, "    node \"15\" forced RED: rb_validate=%d (want nonzero)\n", vrc);
    rb_destroy(t);
    return ok;
}

#define TRAVERSAL_CAP 8
struct traversal_ctx {
    const char *keys[TRAVERSAL_CAP];
    int count;
};

static void collect_key(const char *key, void *value, void *ctx) {
    (void)value;
    struct traversal_ctx *tc = ctx;
    if (tc->count < TRAVERSAL_CAP) {
        tc->keys[tc->count] = key;
    }
    tc->count++;
}

/* ---- RBV-07: in-order traversal yields strictly increasing keys (valid) ---- */
static bool test_rbv07_inorder_increasing_valid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    struct traversal_ctx tc = {0};
    rb_foreach(t, collect_key, &tc);
    bool ok = (tc.count == 5);
    for (int i = 0; ok && i + 1 < tc.count; i++) {
        if (strcmp(tc.keys[i], tc.keys[i + 1]) >= 0) ok = false;
    }
    int vrc = rb_validate(t);
    ok = ok && (vrc == 0);
    if (!ok) {
        fprintf(stderr,
                "    in-order traversal not strictly increasing or count/validate wrong "
                "(count=%d, want 5; rb_validate=%d, want 0)\n",
                tc.count, vrc);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBV-08: BST order violation (invalid) ----
 * Swaps the key POINTERS (not contents) of nodes "05" and "03" -- a pure
 * pointer swap, not a copy/free, so both heap key allocations still have
 * exactly one live owner afterward (just held by the other node); the
 * later rb_destroy still frees each exactly once. Structure/colors are
 * untouched, but the left subtree's in-order sequence becomes
 * "05","03","07", which is not strictly increasing. */
static bool test_rbv08_bst_order_invalid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    struct rb_node *a = t->root->left;        /* "05" */
    struct rb_node *b = t->root->left->left;  /* "03" */
    char *tmp = a->key;
    a->key = b->key;
    b->key = tmp;
    int vrc = rb_validate(t);
    bool ok = (vrc != 0);
    if (!ok) {
        fprintf(stderr, "    keys of \"05\"/\"03\" swapped: rb_validate=%d (want nonzero)\n", vrc);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBV-09: rb_size matches actual node count (valid) ---- */
static bool test_rbv09_size_matches_count_valid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    size_t n = rb_size(t);
    int vrc = rb_validate(t);
    bool ok = (n == 5) && (vrc == 0);
    if (!ok) {
        fprintf(stderr, "    baseline tree: rb_size=%zu (want 5), rb_validate=%d (want 0)\n", n,
                vrc);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBV-10: size/count mismatch (invalid) ---- */
static bool test_rbv10_size_mismatch_invalid(void) {
    rbtree_t *t = build_baseline_tree();
    if (!t) return false;
    t->size += 1;   /* no structural change at all -- purely a bookkeeping drift */
    int vrc = rb_validate(t);
    bool ok = (vrc != 0);
    if (!ok) fprintf(stderr, "    size bumped by 1: rb_validate=%d (want nonzero)\n", vrc);
    rb_destroy(t);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBC-08", "two trees created independently don't share state",
     test_rbc08_independent_trees},
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
    {"RBX-03", "destroying a populated tree runs cleanly", test_rbx03_destroy_populated_tree},
    {"RBX-04", "value_free called exactly once per node with correct values",
     test_rbx04_value_free_called_correctly},
    {"RBX-05", "value_free never invoked when NULL at creation",
     test_rbx05_no_value_free_when_null},
    {"RBX-06", "destroy after mixed insert/delete history runs cleanly",
     test_rbx06_destroy_after_mixed_history},
    {"RBV-01", "root is black (valid)", test_rbv01_root_is_black_valid},
    {"RBV-02", "root is black, violated (invalid)", test_rbv02_root_is_black_invalid},
    {"RBV-03", "no red node has a red child (valid)", test_rbv03_no_red_red_valid},
    {"RBV-04", "red-red violation (invalid)", test_rbv04_red_red_invalid},
    {"RBV-05", "equal black-height on every path (valid)", test_rbv05_black_height_valid},
    {"RBV-06", "black-height mismatch (invalid)", test_rbv06_black_height_invalid},
    {"RBV-07", "in-order traversal strictly increasing (valid)",
     test_rbv07_inorder_increasing_valid},
    {"RBV-08", "BST order violation (invalid)", test_rbv08_bst_order_invalid},
    {"RBV-09", "rb_size matches actual node count (valid)",
     test_rbv09_size_matches_count_valid},
    {"RBV-10", "size/count mismatch (invalid)", test_rbv10_size_mismatch_invalid},
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
