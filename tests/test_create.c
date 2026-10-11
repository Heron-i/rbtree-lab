//Purpose: Unit tests for rb_create, decoupled from every other rbtree
// operation. This file only calls rb_create/rb_create_pooled/rb_destroy/
// rb_validate/rb_size (plus pool_stats through the internal headers), all
// already implemented, so it builds and runs independently of rb_insert,
// rb_find, rb_delete, and rb_foreach.
// Implements RBC-01, RBC-02, RBC-03, RBC-04, RBC-05, RBC-10 from the
// rb_create test plan. RBC-08 depends on rb_insert and stays in
// tests/test_rbtree.c instead (see the comment above it there). The
// remaining planned cases (RBC-06, RBC-07, RBC-09, RBC-11, RBC-12, RBC-13)
// are deferred but still tracked in the plan.
// RBC-14/RBC-15 (M5 Mutation 2) cover rb_create_pooled: two allocations,
// tree then pool, and no slab until the first insert. They read the pool
// through the internal headers.
#include "rbtree.h"
#include "../src/rbtree_internal.h"
#include "../src/pool.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* test-build fault injector behind rb_malloc (not part of rbtree.h) */
#include "fault_alloc.h"

/* ---- RBC-01: create with value_free = NULL ---- */
static bool test_rbc01_create_null_value_free(void) {
    rbtree_t *t = rb_create(NULL);
    bool ok = (t != NULL);
    if (!ok) fprintf(stderr, "    rb_create(NULL) returned NULL, expected non-NULL\n");
    if (t) rb_destroy(t);
    return ok;
}

/* ---- RBC-02: create with a real value_free callback ---- */
static void noop_value_free(void *value) { (void)value; }

static bool test_rbc02_create_with_value_free(void) {
    rbtree_t *t = rb_create(noop_value_free);
    bool ok = (t != NULL);
    if (!ok) fprintf(stderr, "    rb_create(noop_value_free) returned NULL, expected non-NULL\n");
    if (t) rb_destroy(t);
    return ok;
}

/* ---- RBC-03: fresh tree has size 0 ---- */
static bool test_rbc03_fresh_size_zero(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot check size\n");
        return false;
    }
    size_t n = rb_size(t);
    bool ok = (n == 0);
    if (!ok) fprintf(stderr, "    rb_size(fresh tree) = %zu, expected 0\n", n);
    rb_destroy(t);
    return ok;
}

/* ---- RBC-04: fresh tree passes full validation ---- */
static bool test_rbc04_fresh_validates(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot validate\n");
        return false;
    }
    int rc = rb_validate(t);
    bool ok = (rc == 0);
    if (!ok) fprintf(stderr, "    rb_validate(fresh tree) = %d, expected 0\n", rc);
    rb_destroy(t);
    return ok;
}

/* ---- RBC-05: sentinel is BLACK immediately at creation, not just
 * post-insert/post-fixup ---- */
static bool test_rbc05_sentinel_black_at_creation(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot validate\n");
        return false;
    }
    int rc = rb_validate(t);
    bool ok = (rc == 0);
    if (!ok) {
        fprintf(stderr,
                "    rb_validate(fresh tree, pre-insert) = %d, expected 0 "
                "(sentinel must be BLACK at creation)\n",
                rc);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBC-10: allocation failure during create returns NULL ---- */
static bool test_rbc10_alloc_failure_returns_null(void) {
    fault_alloc_arm(1);
    rbtree_t *t = rb_create(NULL);
    fault_alloc_disarm();
    bool ok = (t == NULL);
    if (!ok) {
        fprintf(stderr,
                "    rb_create(NULL) with fault_alloc_arm(1) returned non-NULL, "
                "expected NULL\n");
        rb_destroy(t);
    }
    return ok;
}

/* ---- RBC-14: rb_create_pooled builds an empty, valid tree whose pool
 * holds no slab yet; it costs exactly 2 rb_malloc calls (tree, then pool).
 * rb_create leaves the pool NULL. ---- */
static bool test_rbc14_create_pooled(void) {
    long a0 = fault_alloc_total();
    rbtree_t *t = rb_create_pooled(NULL);
    long allocs = fault_alloc_total() - a0;
    if (!t) {
        fprintf(stderr, "    rb_create_pooled(NULL) returned NULL, expected non-NULL\n");
        return false;
    }
    size_t slabs = 1, live = 1, free_objs = 1;
    if (t->pool != NULL) pool_stats(t->pool, &slabs, &live, &free_objs);
    bool ok = (t->pool != NULL) && (rb_size(t) == 0) && (rb_validate(t) == 0) &&
              (slabs == 0) && (live == 0) && (free_objs == 0) && (allocs == 2);
    if (!ok) {
        fprintf(stderr,
                "    rb_create_pooled: pool=%p, size=%zu, validate=%d, stats (%zu,%zu,%zu) "
                "(want (0,0,0)), allocations=%ld (want 2)\n",
                (void *)t->pool, rb_size(t), rb_validate(t), slabs, live, free_objs, allocs);
    }
    rb_destroy(t);

    rbtree_t *u = rb_create(NULL);
    if (!u || u->pool != NULL) {
        fprintf(stderr, "    rb_create(NULL): tree=%p, pool=%p (want NULL)\n",
                (void *)u, u ? (void *)u->pool : NULL);
        ok = false;
    }
    rb_destroy(u);
    return ok;
}

/* ---- RBC-15: fault sweep over rb_create_pooled. A failure at either
 * allocation (1 = tree, 2 = pool) returns NULL with nothing leaked
 * (asan/memcheck); n = 3 never fires and succeeds. ---- */
static bool test_rbc15_create_pooled_fault_sweep(void) {
    bool ok = true;
    /* invariant: every n' < n behaved as the two-allocation create predicts */
    for (long n = 1; n <= 3; n++) {
        fault_alloc_arm(n);
        rbtree_t *t = rb_create_pooled(NULL);
        fault_alloc_disarm();
        bool want_null = n <= 2;
        if ((t == NULL) != want_null) {
            fprintf(stderr, "    rb_create_pooled under fault_alloc_arm(%ld) returned %p, want %s\n",
                    n, (void *)t, want_null ? "NULL" : "a tree");
            ok = false;
        }
        rb_destroy(t);
    }
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBC-01", "create with value_free = NULL", test_rbc01_create_null_value_free},
    {"RBC-02", "create with a real value_free callback", test_rbc02_create_with_value_free},
    {"RBC-03", "fresh tree has size 0", test_rbc03_fresh_size_zero},
    {"RBC-04", "fresh tree passes full validation", test_rbc04_fresh_validates},
    {"RBC-05", "sentinel is BLACK immediately at creation", test_rbc05_sentinel_black_at_creation},
    {"RBC-10", "allocation failure during create returns NULL",
     test_rbc10_alloc_failure_returns_null},
    {"RBC-14", "rb_create_pooled: empty tree, lazy pool, 2 allocations",
     test_rbc14_create_pooled},
    {"RBC-15", "rb_create_pooled fault sweep (tree, then pool)",
     test_rbc15_create_pooled_fault_sweep},
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
