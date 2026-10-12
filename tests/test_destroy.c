//Purpose: Unit tests for rb_destroy, decoupled from every other rbtree
// operation. This file only calls rb_create/rb_create_pooled/rb_destroy,
// all already implemented, so it builds and runs independently of
// rb_insert, rb_find, rb_delete, and rb_foreach.
// Implements RBX-01, RBX-02 from the rb_destroy test plan. RBX-03..RBX-06
// depend on rb_insert (RBX-06 also on rb_delete) to build a populated or
// mixed-history tree and stay in tests/test_rbtree.c instead (see the
// comment above RBX-03 there).
// RBX-07 (M5 Mutation 2) destroys a fresh pooled tree, whose pool holds no
// slab yet. Populated pooled destroys are exercised by the pooled fault
// sweep and the pooled fuzz mode, which count value_free calls.
// RBX-08..11 (Mutation 3) still call no other tree operation: they build
// degenerate shapes by hand through rbtree_internal.h (fixture_build below)
// and check that the iterative teardown frees each value exactly once and
// allocates nothing. The 10^6-node, 64 KB-stack proof is tests/test_stress.c.
#include "rbtree.h"
#include "../src/rbtree_internal.h"
#include "../src/pool.h"
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Hand-built shapes (Mutation 3) ----
 * Far deeper than any red-black tree: a chain of n nodes has height n.
 * NOT red-black trees, so never rb_validate them. BST order ("k<idx>"
 * sorts like idx) and parent links are correct. Node storage is whatever
 * rb_destroy releases (t->pool if set, else rb_malloc), so the fixture
 * also works in the pooled build. Same helper as tests/test_stress.c. */
#define FIXTURE_KEY_LEN 16

enum fixture_shape { FIXTURE_LEFT_CHAIN, FIXTURE_RIGHT_CHAIN, FIXTURE_ZIGZAG };

/* Links a new node (key "k<idx>", value) under parent as its left or right
 * child; parent == &t->nil makes it the root. Returns the node, or NULL
 * with nothing linked and nothing leaked if an allocation failed. */
static struct rb_node *fixture_add(rbtree_t *t, struct rb_node *parent, bool as_left,
                                   unsigned idx, void *value) {
    char buf[FIXTURE_KEY_LEN];
    snprintf(buf, sizeof buf, "k%07u", idx);
    size_t len = strlen(buf) + 1;
    char *key = rb_malloc(len);
    if (key == NULL) return NULL;
    struct rb_node *n = t->pool != NULL ? pool_alloc(t->pool) : rb_malloc(sizeof *n);
    if (n == NULL) {
        rb_free(key);
        return NULL;
    }
    memcpy(key, buf, len);
    n->key = key;
    n->value = value;
    n->left = n->right = &t->nil;
    n->parent = parent;
    n->color = BLACK;
    if (parent == &t->nil) t->root = n;
    else if (as_left) parent->left = n;
    else parent->right = n;
    t->size++;
    return n;
}

/* Builds a one-path shape of n >= 1 nodes, keys k0..k(n-1); node k<idx>
 * gets value &marks[idx] (NULL if marks is NULL). Returns false if an
 * allocation failed; nodes linked so far stay in t for rb_destroy.
 *   LEFT_CHAIN:  every node is its parent's left child; root holds k(n-1).
 *   RIGHT_CHAIN: every node is its parent's right child; root holds k0.
 *   ZIGZAG:      root holds the largest key, its left child the smallest,
 *                that node's right child the next-largest, and so on. */
static bool fixture_build(rbtree_t *t, enum fixture_shape shape, unsigned n,
                          unsigned char *marks) {
    unsigned lo = 0, hi = n - 1;
    bool take_hi = shape != FIXTURE_RIGHT_CHAIN;
    bool as_left = shape != FIXTURE_RIGHT_CHAIN;
    struct rb_node *tail = &t->nil;
    /* invariant: keys lo..hi are unbuilt, and all belong in tail's next child slot */
    for (unsigned d = 0; d < n; d++) {
        unsigned idx = take_hi ? hi-- : lo++;
        tail = fixture_add(t, tail, as_left, idx, marks != NULL ? &marks[idx] : NULL);
        if (tail == NULL) return false;
        if (shape == FIXTURE_ZIGZAG) {
            as_left = take_hi;   /* below a high key go left, below a low key go right */
            take_hi = !take_hi;
        }
    }
    return true;
}

/* value_free double: counts frees per value through the marks array. */
static void mark_freed(void *value) {
    (*(unsigned char *)value)++;
}

#define SHAPE_N 4096u
static unsigned char marks[SHAPE_N];

/* ---- RBX-01: rb_destroy(NULL) is safe ---- */
static bool test_rbx01_destroy_null_is_safe(void) {
    rb_destroy(NULL);
    return true;   /* success == did not crash */
}

/* ---- RBX-02: destroying a freshly-created (empty) tree runs cleanly ---- */
static bool test_rbx02_destroy_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    rb_destroy(t);
    return true;
}

/* ---- RBX-07: destroying a freshly-created pooled tree (pool holds no
 * slab) runs cleanly; leaks are judged by asan/memcheck ---- */
static bool test_rbx07_destroy_empty_pooled_tree(void) {
    rbtree_t *t = rb_create_pooled(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create_pooled(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    rb_destroy(t);
    return true;
}

/* Builds `shape` with SHAPE_N nodes on a tree whose value_free marks each
 * value, destroys it, and checks every value was freed exactly once. */
static bool destroy_shape_frees_each_value_once(enum fixture_shape shape) {
    memset(marks, 0, sizeof marks);
    rbtree_t *t = rb_create(mark_freed);
    if (!t) {
        fprintf(stderr, "    rb_create returned NULL, cannot build tree\n");
        return false;
    }
    if (!fixture_build(t, shape, SHAPE_N, marks)) {
        fprintf(stderr, "    allocation failed while building the shape\n");
        rb_destroy(t);
        return false;
    }
    rb_destroy(t);
    /* invariant: marks[0..i) are all exactly 1 */
    for (unsigned i = 0; i < SHAPE_N; i++) {
        if (marks[i] != 1) {
            fprintf(stderr, "    value of k%07u freed %u times (want 1)\n", i, (unsigned)marks[i]);
            return false;
        }
    }
    return true;
}

/* ---- RBX-08: a left chain (every pass but the frees rotates) ---- */
static bool test_rbx08_destroy_left_chain(void) {
    return destroy_shape_frees_each_value_once(FIXTURE_LEFT_CHAIN);
}

/* ---- RBX-09: a right chain (no rotations at all) ---- */
static bool test_rbx09_destroy_right_chain(void) {
    return destroy_shape_frees_each_value_once(FIXTURE_RIGHT_CHAIN);
}

/* ---- RBX-10: a zigzag, so each teardown rotation moves a nonempty
 * l->right subtree under the old root ---- */
static bool test_rbx10_destroy_zigzag(void) {
    return destroy_shape_frees_each_value_once(FIXTURE_ZIGZAG);
}

/* ---- RBX-11: rb_destroy allocates nothing (no explicit stack) ---- */
static bool test_rbx11_destroy_allocates_nothing(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create returned NULL, cannot build tree\n");
        return false;
    }
    if (!fixture_build(t, FIXTURE_ZIGZAG, SHAPE_N, NULL)) {
        fprintf(stderr, "    allocation failed while building the shape\n");
        rb_destroy(t);
        return false;
    }
    long before = fault_alloc_total();
    rb_destroy(t);
    long made = fault_alloc_total() - before;
    if (made != 0) {
        fprintf(stderr, "    rb_destroy made %ld rb_malloc calls (want 0)\n", made);
        return false;
    }
    return true;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBX-01", "rb_destroy(NULL) is safe", test_rbx01_destroy_null_is_safe},
    {"RBX-02", "destroying a freshly-created tree runs cleanly", test_rbx02_destroy_empty_tree},
    {"RBX-07", "destroying a fresh pooled tree runs cleanly", test_rbx07_destroy_empty_pooled_tree},
    {"RBX-08", "destroying a 4096-node left chain frees each value once", test_rbx08_destroy_left_chain},
    {"RBX-09", "destroying a 4096-node right chain frees each value once", test_rbx09_destroy_right_chain},
    {"RBX-10", "destroying a 4096-node zigzag frees each value once", test_rbx10_destroy_zigzag},
    {"RBX-11", "rb_destroy makes no allocation", test_rbx11_destroy_allocates_nothing},
};

int main(void) {
    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;
#ifdef RB_POOLED_BUILD
    size_t na = 0;   /* declared N/A on the pooled build (tests/pooled_build.h) */
    if (!POOLED_NA_VERIFY(tests, total)) return EXIT_FAILURE;
#endif
    for (size_t i = 0; i < total; i++) {
#ifdef RB_POOLED_BUILD
        if (pooled_na_report(__FILE__, tests[i].id)) {
            na++;
            continue;
        }
#endif
        bool ok = tests[i].run();
        printf("[%s] %-55s %s\n", tests[i].id, tests[i].name, ok ? "PASS" : "FAIL");
        if (ok) passed++;
    }
#ifdef RB_POOLED_BUILD
    total -= na;
    printf("%zu declared N/A on the pooled build\n", na);
#endif
    printf("%zu/%zu tests passed\n", passed, total);
    return (passed == total) ? EXIT_SUCCESS : EXIT_FAILURE;
}
