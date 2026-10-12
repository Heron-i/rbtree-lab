// Purpose: Mutation 3 stack-bound proof. rb_foreach and then rb_destroy run
// on a thread whose whole stack is 64 KB, over trees of N nodes. Any
// recursion (or explicit stack) proportional to tree height overflows
// there on the hand-built chains, whose height is N; the iterative walk
// and teardown need only constant-size frames. All trees are pooled, which
// keeps building them fast (one rb_malloc per 85 nodes instead of one per
// node).
// Size: N = 2^14 by default, so make memcheck stays quick; --big runs the
// 2^20 (>= 10^6) case, which make test and make asan always pass. 2^14
// still proves the bound: the recursive code overflowed this stack after
// about 1,270 (foreach) and 1,910 (destroy) frames.
//   RBM-01: a real red-black tree built with rb_insert (stress test; its
//           height is about 40, so it fits either way)
//   RBM-02: a left chain (walk descends N deep; N - 1 teardown rotations)
//   RBM-03: a right chain (walk ends with one N - 1 level climb; no rotations)
// Each checks the walk visited every key once, in order, with its own
// value, and that every value was then freed exactly once.
#include "rbtree.h"
#include "../src/rbtree_internal.h"
#include "../src/pool.h"
#include "fault_alloc.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_N (1u << 14)           /* default: 16,384, quick under valgrind */
#define BIG_N (1u << 20)             /* --big: 1,048,576 >= 10^6 */
#define SMALL_STACK ((size_t)64 * 1024)

/* nodes per case; a power of two, so RBM-01's index mask permutes [0, n) */
static unsigned stress_n = SMALL_N;

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

/* rb_foreach callback state: the walk must visit k0, k1, ... in order,
 * each with its own value &marks[idx]. */
struct walk_check {
    const unsigned char *marks;
    unsigned next;   /* index of the key the walk must visit next */
    bool ok;
};

static void walk_check_cb(const char *key, void *value, void *ctx) {
    struct walk_check *w = ctx;
    char want[FIXTURE_KEY_LEN];
    if (!w->ok) return;
    snprintf(want, sizeof want, "k%07u", w->next);
    if (w->next >= stress_n || strcmp(key, want) != 0 || value != &w->marks[w->next]) {
        fprintf(stderr, "    walk position %u: got key \"%s\" (want \"%s\" with its own value)\n",
                w->next, key, want);
        w->ok = false;
        return;
    }
    w->next++;
}

/* ---- the 64 KB thread: walk the whole tree, then tear it down ---- */
struct stress_job {
    rbtree_t *t;
    struct walk_check walk;
};

static void *walk_then_destroy_body(void *arg) {
    struct stress_job *job = arg;
    rb_foreach(job->t, walk_check_cb, &job->walk);
    rb_destroy(job->t);
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

/* On the 64 KB thread, walks t (all stress_n keys in order) and then
 * destroys it; afterwards checks each value was freed once. Owns t and
 * marks either way. */
static bool destroy_small_and_check(rbtree_t *t, unsigned char *marks) {
    struct stress_job job = {.t = t, .walk = {.marks = marks, .next = 0, .ok = true}};
    bool ok = run_on_small_stack(walk_then_destroy_body, &job);
    if (!ok) rb_destroy(t);   /* thread never ran: release on this stack */
    if (ok && job.walk.ok && job.walk.next != stress_n) {
        fprintf(stderr, "    walk visited %u keys (want %u)\n", job.walk.next, stress_n);
        job.walk.ok = false;
    }
    ok = ok && job.walk.ok && all_marked_once(marks, stress_n);
    free(marks);
    return ok;
}

/* ---- RBM-01: N-node red-black tree from rb_insert ---- */
static bool test_rbm01_rbtree_destroy_small_stack(void) {
    unsigned char *marks = calloc(stress_n, 1);
    rbtree_t *t = rb_create_pooled(mark_freed);
    if (!marks || !t) {
        fprintf(stderr, "    setup allocation failed\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    char key[FIXTURE_KEY_LEN];
    /* invariant: the keys of the first i scrambled indices are in t. An odd
     * multiplier permutes [0, N) for N a power of two, so every index is
     * inserted exactly once. */
    for (unsigned i = 0; i < stress_n; i++) {
        unsigned idx = (i * 2654435761u) & (stress_n - 1);
        snprintf(key, sizeof key, "k%07u", idx);
        if (rb_insert(t, key, &marks[idx]) != 0) {
            fprintf(stderr, "    rb_insert failed at i=%u\n", i);
            rb_destroy(t);
            free(marks);
            return false;
        }
    }
    if (rb_size(t) != stress_n) {
        fprintf(stderr, "    rb_size=%zu after inserts (want %u)\n", rb_size(t), stress_n);
        rb_destroy(t);
        free(marks);
        return false;
    }
    return destroy_small_and_check(t, marks);
}

/* Builds an N-node pooled chain and destroys it on the 64 KB thread. */
static bool chain_destroy_small_stack(enum fixture_shape shape) {
    unsigned char *marks = calloc(stress_n, 1);
    rbtree_t *t = rb_create_pooled(mark_freed);
    if (!marks || !t) {
        fprintf(stderr, "    setup allocation failed\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    if (!fixture_build(t, shape, stress_n, marks)) {
        fprintf(stderr, "    allocation failed while building the chain\n");
        rb_destroy(t);
        free(marks);
        return false;
    }
    return destroy_small_and_check(t, marks);
}

/* ---- RBM-02: N-node left chain ---- */
static bool test_rbm02_left_chain_destroy_small_stack(void) {
    return chain_destroy_small_stack(FIXTURE_LEFT_CHAIN);
}

/* ---- RBM-03: N-node right chain ---- */
static bool test_rbm03_right_chain_destroy_small_stack(void) {
    return chain_destroy_small_stack(FIXTURE_RIGHT_CHAIN);
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBM-01", "RB tree walked + destroyed on a 64 KB stack", test_rbm01_rbtree_destroy_small_stack},
    {"RBM-02", "left chain walked + destroyed on a 64 KB stack", test_rbm02_left_chain_destroy_small_stack},
    {"RBM-03", "right chain walked + destroyed on a 64 KB stack", test_rbm03_right_chain_destroy_small_stack},
};

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--big") == 0) {
        stress_n = BIG_N;
    } else if (argc != 1) {
        /* a typo must not silently fall back to the small case */
        fprintf(stderr, "usage: %s [--big]\n", argv[0]);
        return EXIT_FAILURE;
    }
    printf("test_stress: N = %u per case (2^14 default | 2^20 with --big)\n", stress_n);
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
