//Purpose: My unit tests created with known specs and edge cases (table-driven tests)
// Implements RBC-01, RBC-02, RBC-03, RBC-04, RBC-05, RBC-08, RBC-10 from the
// rb_create test plan. The remaining planned cases (RBC-06, RBC-07, RBC-09,
// RBC-11, RBC-12, RBC-13) are deferred but still tracked in the plan.
// Implements RBD-01, RBD-03, RBD-04 from the rb_delete test plan. RBD-02
// (black leaf with a red sibling) is written below but not yet wired into
// `tests[]`: its insert sequence needs to be confirmed against the real
// rb_insert implementation first (see the function's comment) since node
// color isn't observable through the public API.
// Implements RBX-01..RBX-06 from the rb_destroy test plan. These stick to
// what's observable in-process (crash-freedom, value_free call
// count/values); leak/double-free correctness (e.g. "every node freed
// together with its key copy") is verified separately by `make memcheck`.
#include "rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* test-only fault-injection hook defined in rbtree.c (external linkage,
 * intentionally not part of the public header contract) */
extern bool rb_fail_next_alloc;

/* value_free test double. For RBC-02 it just needs to be a valid callback;
 * RBX-04 also uses its call count/recorded values to check rb_destroy
 * invokes it exactly once per node with the right value pointers. */
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

/* ---- RBC-01: create with value_free = NULL ---- */
static bool test_rbc01_create_null_value_free(void) {
    rbtree_t *t = rb_create(NULL);
    bool ok = (t != NULL);
    if (!ok) fprintf(stderr, "    rb_create(NULL) returned NULL, expected non-NULL\n");
    if (t) rb_destroy(t);
    return ok;
}

/* ---- RBC-02: create with a real value_free callback ---- */
static bool test_rbc02_create_with_value_free(void) {
    rbtree_t *t = rb_create(free_recorder);
    bool ok = (t != NULL);
    if (!ok) fprintf(stderr, "    rb_create(free_recorder) returned NULL, expected non-NULL\n");
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

/* ---- RBD-01: delete a red leaf ---- */
static bool test_rbd01_delete_red_leaf(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "5", "15", "3"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    /* "3" is a red leaf after inserting 10,5,15,3 (standard RB insert-fixup) */
    int rc = rb_delete(t, "3");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 3);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"3\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 3)\n",
                rc, vrc, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBD-02: delete a black leaf whose sibling is red ----
 * TENTATIVE / NOT YET WIRED IN (see tests[] below). By black-height
 * arithmetic the only valid shape here is: parent P black, one child X
 * black-and-a-leaf (deleted below), the other child W red with W's own two
 * children both black leaves. I was not able to hand-verify that the
 * sequence below actually produces that exact shape under standard
 * insert-fixup, and node color isn't observable through the public API to
 * confirm it independently. Before enabling this case: build this sequence
 * against the real rb_insert, confirm via reasoning (or a temporary,
 * non-shipped debug traversal) that "30" really is a black leaf with a red
 * sibling, and adjust the key sequence if it isn't. */
[[maybe_unused]] static bool test_rbd02_delete_black_leaf_red_sibling(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "30", "70", "60", "80"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    int rc = rb_delete(t, "30");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 4);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"30\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
                "rb_size=%zu (want 4)\n",
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
    const char *keys[] = {"10", "5", "15", "3", "7"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    /* "5" has two real children ("3" and "7") after this sequence */
    int rc = rb_delete(t, "5");
    int vrc = rb_validate(t);
    size_t n = rb_size(t);
    bool ok = (rc == 0) && (vrc == 0) && (n == 4);
    if (!ok) {
        fprintf(stderr,
                "    delete(\"5\"): rb_delete rc=%d (want 0), rb_validate=%d (want 0), "
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
    const char *keys[] = {"10", "5", "15", "3", "7"};
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
    const char *keys[] = {"10", "5", "15", "3", "7"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    if (rb_delete(t, "3") != 0 || rb_delete(t, "15") != 0) {
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
    {"RBC-08", "two trees created independently don't share state",
     test_rbc08_independent_trees},
    {"RBC-10", "allocation failure during create returns NULL",
     test_rbc10_alloc_failure_returns_null},
    {"RBD-01", "delete a red leaf", test_rbd01_delete_red_leaf},
    {"RBD-03", "delete a node with two children", test_rbd03_delete_two_children},
    {"RBD-04", "delete the root of a one-node tree", test_rbd04_delete_root},
    {"RBX-01", "rb_destroy(NULL) is safe", test_rbx01_destroy_null_is_safe},
    {"RBX-02", "destroying a freshly-created tree runs cleanly", test_rbx02_destroy_empty_tree},
    {"RBX-03", "destroying a populated tree runs cleanly", test_rbx03_destroy_populated_tree},
    {"RBX-04", "value_free called exactly once per node with correct values",
     test_rbx04_value_free_called_correctly},
    {"RBX-05", "value_free never invoked when NULL at creation",
     test_rbx05_no_value_free_when_null},
    {"RBX-06", "destroy after mixed insert/delete history runs cleanly",
     test_rbx06_destroy_after_mixed_history},
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
