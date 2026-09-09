//Purpose: Unit tests for rb_find, decoupled from rb_insert. The populated-
// tree fixture is built by hand through rbtree_internal.h rather than via
// rb_insert (which does not exist yet): rb_find only walks BST structure
// and compares keys, so it doesn't care about color or balance, and a
// hand-wired single node is a faithful precondition for its contract. This
// is NOT the same move as faking a realistic multi-node RB tree for
// rb_validate/rb_destroy -- those need rb_insert's actual rebalancing
// output, so their tests use build_baseline_tree() (see
// tests/test_rbtree.c) once rb_insert exists instead.
// Implements RBF-01..RBF-03 from the rb_find test plan. Multi-node
// left/right-branch traversal and case-sensitivity cases are deferred
// until rb_insert exists, so larger fixtures don't also have to be
// hand-wired.
#include "rbtree.h"
#include "../src/rbtree_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds a one-node tree wired directly through rbtree_internal.h. The key
 * is heap-copied (the same ownership rb_insert would establish) so
 * rb_destroy can free it normally. */
static rbtree_t *build_single_node_tree(const char *key, void *value) {
    rbtree_t *t = rb_create(NULL);
    if (!t) return NULL;

    struct rb_node *n = malloc(sizeof *n);
    if (!n) { rb_destroy(t); return NULL; }

    char *key_copy = malloc(strlen(key) + 1);
    if (!key_copy) { free(n); rb_destroy(t); return NULL; }
    strcpy(key_copy, key);

    n->key = key_copy;
    n->value = value;
    n->left = &t->nil;
    n->right = &t->nil;
    n->parent = &t->nil;
    n->color = BLACK;

    t->root = n;
    t->size = 1;
    return t;
}

/* ---- RBF-01: find on an empty tree returns NULL ---- */
static bool test_rbf01_find_empty_returns_null(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    void *v = rb_find(t, "anything");
    bool ok = (v == NULL);
    if (!ok) fprintf(stderr, "    rb_find(empty tree, \"anything\") = %p, expected NULL\n", v);
    rb_destroy(t);
    return ok;
}

/* ---- RBF-02: find returns the value for a present key ---- */
static bool test_rbf02_find_present_key_returns_value(void) {
    static int tag = 42;
    rbtree_t *t = build_single_node_tree("10", &tag);
    if (!t) {
        fprintf(stderr, "    build_single_node_tree failed\n");
        return false;
    }
    void *v = rb_find(t, "10");
    bool ok = (v == &tag);
    if (!ok) fprintf(stderr, "    rb_find(\"10\") = %p, expected %p\n", v, (void *)&tag);
    rb_destroy(t);
    return ok;
}

/* ---- RBF-03: find returns NULL for an absent key in a non-empty tree ---- */
static bool test_rbf03_find_absent_key_returns_null(void) {
    static int tag = 7;
    rbtree_t *t = build_single_node_tree("10", &tag);
    if (!t) {
        fprintf(stderr, "    build_single_node_tree failed\n");
        return false;
    }
    void *v = rb_find(t, "99");
    bool ok = (v == NULL);
    if (!ok) fprintf(stderr, "    rb_find(\"99\") = %p, expected NULL\n", v);
    rb_destroy(t);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBF-01", "find on an empty tree returns NULL", test_rbf01_find_empty_returns_null},
    {"RBF-02", "find returns the value for a present key",
     test_rbf02_find_present_key_returns_value},
    {"RBF-03", "find returns NULL for an absent key in a non-empty tree",
     test_rbf03_find_absent_key_returns_null},
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
