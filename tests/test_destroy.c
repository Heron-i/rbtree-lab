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
#include "rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBX-01", "rb_destroy(NULL) is safe", test_rbx01_destroy_null_is_safe},
    {"RBX-02", "destroying a freshly-created tree runs cleanly", test_rbx02_destroy_empty_tree},
    {"RBX-07", "destroying a fresh pooled tree runs cleanly", test_rbx07_destroy_empty_pooled_tree},
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
