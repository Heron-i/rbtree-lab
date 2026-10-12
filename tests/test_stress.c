// Purpose: Mutation 3 stack-bound proof. rb_destroy runs on a thread whose
// whole stack is 64 KB, over trees of 2^20 (>= 10^6) nodes. Any recursion
// (or explicit stack) proportional to tree height overflows there on the
// hand-built chains, whose height is N; the iterative teardown needs only
// a constant-size frame. All trees are pooled, which keeps building 10^6
// nodes fast (one rb_malloc per 85 nodes instead of one per node).
//   RBM-01: a real red-black tree built with rb_insert (stress test; its
//           height is about 40, so it fits either way)
//   RBM-02: a left chain (N - 1 teardown rotations)
//   RBM-03: a right chain (no rotations)
// Each checks that every value was freed exactly once.
#include "rbtree.h"
#include "../src/rbtree_internal.h"
#include "../src/pool.h"
#include "fault_alloc.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRESS_N (1u << 20)          /* 1,048,576 >= 10^6 */
#define SMALL_STACK ((size_t)64 * 1024)

/* ---- Hand-built shapes (Mutation 3) ----
 * Far deeper than any red-black tree: a chain of n nodes has height n.
 * NOT red-black trees, so never rb_validate them. BST order ("k<idx>"
 * sorts like idx) and parent links are correct. Node storage is whatever
 * rb_destroy releases (t->pool if set, else rb_malloc). Same helper as
 * tests/test_destroy.c. */
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

/* True iff marks[0..n) are all exactly 1; reports the first that is not. */
static bool all_marked_once(const unsigned char *marks, unsigned n) {
    /* invariant: marks[0..i) are all exactly 1 */
    for (unsigned i = 0; i < n; i++) {
        if (marks[i] != 1) {
            fprintf(stderr, "    value of k%07u freed %u times (want 1)\n", i, (unsigned)marks[i]);
            return false;
        }
    }
    return true;
}

/* ---- the 64 KB thread ---- */
static void *destroy_body(void *tree) {
    rb_destroy(tree);
    return NULL;
}

/* Runs body(arg) to completion on a thread whose entire stack is
 * SMALL_STACK bytes. False only if the thread could not be run. */
static bool run_on_small_stack(void *(*body)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_t th;
    int rc = pthread_attr_init(&attr);
    if (rc == 0) rc = pthread_attr_setstacksize(&attr, SMALL_STACK);
    if (rc == 0) rc = pthread_create(&th, &attr, body, arg);
    pthread_attr_destroy(&attr);
    if (rc == 0) rc = pthread_join(th, NULL);
    if (rc != 0) {
        fprintf(stderr, "    could not run the 64 KB thread: %s\n", strerror(rc));
        return false;
    }
    return true;
}

/* Destroys t on the 64 KB thread, then checks each value was freed once.
 * Owns t and marks either way. */
static bool destroy_small_and_check(rbtree_t *t, unsigned char *marks) {
    bool ok = run_on_small_stack(destroy_body, t);
    if (!ok) rb_destroy(t);   /* thread never ran: release on this stack */
    ok = ok && all_marked_once(marks, STRESS_N);
    free(marks);
    return ok;
}

/* ---- RBM-01: 2^20-node red-black tree from rb_insert ---- */
static bool test_rbm01_rbtree_destroy_small_stack(void) {
    unsigned char *marks = calloc(STRESS_N, 1);
    rbtree_t *t = rb_create_pooled(mark_freed);
    if (!marks || !t) {
        fprintf(stderr, "    setup allocation failed\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    char key[FIXTURE_KEY_LEN];
    /* invariant: the keys of the first i scrambled indices are in t. An odd
     * multiplier permutes [0, 2^20), so every index is inserted exactly once. */
    for (unsigned i = 0; i < STRESS_N; i++) {
        unsigned idx = (i * 2654435761u) & (STRESS_N - 1);
        snprintf(key, sizeof key, "k%07u", idx);
        if (rb_insert(t, key, &marks[idx]) != 0) {
            fprintf(stderr, "    rb_insert failed at i=%u\n", i);
            rb_destroy(t);
            free(marks);
            return false;
        }
    }
    if (rb_size(t) != STRESS_N) {
        fprintf(stderr, "    rb_size=%zu after inserts (want %u)\n", rb_size(t), STRESS_N);
        rb_destroy(t);
        free(marks);
        return false;
    }
    return destroy_small_and_check(t, marks);
}

/* Builds a 2^20-node pooled chain and destroys it on the 64 KB thread. */
static bool chain_destroy_small_stack(enum fixture_shape shape) {
    unsigned char *marks = calloc(STRESS_N, 1);
    rbtree_t *t = rb_create_pooled(mark_freed);
    if (!marks || !t) {
        fprintf(stderr, "    setup allocation failed\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    if (!fixture_build(t, shape, STRESS_N, marks)) {
        fprintf(stderr, "    allocation failed while building the chain\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    return destroy_small_and_check(t, marks);
}

/* ---- RBM-02: 2^20-node left chain ---- */
static bool test_rbm02_left_chain_destroy_small_stack(void) {
    return chain_destroy_small_stack(FIXTURE_LEFT_CHAIN);
}

/* ---- RBM-03: 2^20-node right chain ---- */
static bool test_rbm03_right_chain_destroy_small_stack(void) {
    return chain_destroy_small_stack(FIXTURE_RIGHT_CHAIN);
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBM-01", "2^20-node RB tree destroyed on a 64 KB stack", test_rbm01_rbtree_destroy_small_stack},
    {"RBM-02", "2^20-node left chain destroyed on a 64 KB stack", test_rbm02_left_chain_destroy_small_stack},
    {"RBM-03", "2^20-node right chain destroyed on a 64 KB stack", test_rbm03_right_chain_destroy_small_stack},
};

int main(void) {
    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;
    for (size_t i = 0; i < total; i++) {
        bool ok = tests[i].run();
        printf("[%s] %-55s %s\n", tests[i].id, tests[i].name, ok ? "PASS" : "FAIL");
        fflush(stdout);   /* a crash in the next case must not swallow this line */
        if (ok) passed++;
    }
    printf("%zu/%zu tests passed\n", passed, total);
    return (passed == total) ? EXIT_SUCCESS : EXIT_FAILURE;
}
