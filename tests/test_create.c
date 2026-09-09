//Purpose: Unit tests for rb_create, decoupled from every other rbtree
// operation. This file only calls rb_create/rb_destroy/rb_validate/rb_size,
// all already implemented, so it builds and runs independently of
// rb_insert, rb_find, rb_delete, and rb_foreach.
// Implements RBC-01, RBC-02, RBC-03, RBC-04, RBC-05, RBC-10 from the
// rb_create test plan. RBC-08 depends on rb_insert and stays in
// tests/test_rbtree.c instead (see the comment above it there). The
// remaining planned cases (RBC-06, RBC-07, RBC-09, RBC-11, RBC-12, RBC-13)
// are deferred but still tracked in the plan.
#include "rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* test-only fault-injection hook defined in rbtree.c (external linkage,
 * intentionally not part of the public header contract) */
extern bool rb_fail_next_alloc;

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
    rb_fail_next_alloc = true;
    rbtree_t *t = rb_create(NULL);
    bool ok = (t == NULL);
    if (!ok) {
        fprintf(stderr,
                "    rb_create(NULL) with rb_fail_next_alloc=true returned non-NULL, "
                "expected NULL\n");
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
