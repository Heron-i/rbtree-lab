//Purpose: My unit tests created with known specs and edge cases (table-driven tests)

#include "rbtree.h"
#include <stdbool.h>
#include <stdio.h>

extern bool rb_fail_next_alloc;   /* test-only hook defined in src/rbtree.c */

typedef struct {
    const char *name;
    bool (*run)(const char **where, const char **input);
} test_case_t;

static int free_call_count;

static void dummy_free(void *value) {
    (void)value;
    free_call_count++;
}

static bool create_returns_non_null_no_free_fn(const char **where, const char **input) {
    *input = "value_free=NULL";
    rbtree_t *t = rb_create(NULL);
    if (t == NULL) {
        *where = "rb_create returned NULL for a normal allocation";
        return false;
    }
    rb_destroy(t);
    return true;
}

static bool create_returns_non_null_with_free_fn(const char **where, const char **input) {
    *input = "value_free=dummy_free";
    rbtree_t *t = rb_create(dummy_free);
    if (t == NULL) {
        *where = "rb_create returned NULL when given a non-NULL value_free callback";
        return false;
    }
    rb_destroy(t);
    return true;
}

static bool create_returns_independent_instances(const char **where, const char **input) {
    *input = "two sequential rb_create(NULL) calls";
    rbtree_t *a = rb_create(NULL);
    rbtree_t *b = rb_create(NULL);
    bool ok = true;
    if (a == NULL) { *where = "first rb_create(NULL) call returned NULL"; ok = false; }
    else if (b == NULL) { *where = "second rb_create(NULL) call returned NULL"; ok = false; }
    else if (a == b) { *where = "two independent rb_create(NULL) calls returned the same pointer"; ok = false; }
    rb_destroy(a);
    rb_destroy(b);
    return ok;
}

static bool create_oom_returns_null(const char **where, const char **input) {
    *input = "rb_fail_next_alloc=true before rb_create(NULL) (simulated OOM on the tree struct's malloc)";
    rb_fail_next_alloc = true;
    rbtree_t *t = rb_create(NULL);
    if (t != NULL) {
        *where = "rb_create did not return NULL when its allocation was forced to fail";
        rb_destroy(t);
        return false;
    }
    return true;
}

static bool create_oom_then_recovers(const char **where, const char **input) {
    *input = "rb_fail_next_alloc=true for the first call only, then a second rb_create(NULL)";
    rb_fail_next_alloc = true;
    rbtree_t *failed = rb_create(NULL);
    rbtree_t *ok_tree = rb_create(NULL);
    bool ok = true;
    if (failed != NULL) { *where = "the forced-failure call unexpectedly returned non-NULL"; ok = false; }
    else if (ok_tree == NULL) { *where = "the following normal rb_create(NULL) call also returned NULL"; ok = false; }
    rb_destroy(failed);
    rb_destroy(ok_tree);
    return ok;
}

static bool destroy_null_is_safe(const char **where, const char **input) {
    (void)where;   /* cannot fail without crashing; nothing to describe */
    *input = "t=NULL";
    rb_destroy(NULL);
    return true;   /* success == did not crash */
}

static bool destroy_freshly_created_tree(const char **where, const char **input) {
    *input = "t=rb_create(NULL) result";
    rbtree_t *t = rb_create(NULL);
    if (t == NULL) {
        *where = "rb_create(NULL) returned NULL; could not exercise rb_destroy on a real tree";
        return false;
    }
    rb_destroy(t);
    return true;
}

static bool destroy_empty_tree_never_calls_value_free(const char **where, const char **input) {
    static char detail[96];
    *input = "value_free=dummy_free, no keys inserted";
    free_call_count = 0;
    rbtree_t *t = rb_create(dummy_free);
    if (t == NULL) {
        *where = "rb_create(dummy_free) returned NULL; could not exercise rb_destroy on a real tree";
        return false;
    }
    rb_destroy(t);
    if (free_call_count != 0) {
        snprintf(detail, sizeof detail,
                 "value_free was called %d time(s) on an empty tree, expected 0",
                 free_call_count);
        *where = detail;
        return false;
    }
    return true;
}

static const test_case_t tests[] = {
    { "create_returns_non_null_no_free_fn", create_returns_non_null_no_free_fn },
    { "create_returns_non_null_with_free_fn", create_returns_non_null_with_free_fn },
    { "create_returns_independent_instances", create_returns_independent_instances },
    { "create_oom_returns_null", create_oom_returns_null },
    { "create_oom_then_recovers", create_oom_then_recovers },
    { "destroy_null_is_safe", destroy_null_is_safe },
    { "destroy_freshly_created_tree", destroy_freshly_created_tree },
    { "destroy_empty_tree_never_calls_value_free", destroy_empty_tree_never_calls_value_free },
};

int main(void) {
    size_t failures = 0;
    size_t n = sizeof tests / sizeof tests[0];
    /* invariant: every case in tests[0..i) has been run, reported, and tallied */
    for (size_t i = 0; i < n; i++) {
        const char *where = "(unspecified check)";
        const char *input = "(unspecified input)";
        bool passed = tests[i].run(&where, &input);
        if (passed) {
            printf("[PASS] %s\n", tests[i].name);
        } else {
            printf("[FAIL] Test %s failed at %s for input %s.\n",
                   tests[i].name, where, input);
            failures++;
        }
    }
    printf("%zu/%zu tests passed\n", n - failures, n);
    return failures == 0 ? 0 : 1;
}
