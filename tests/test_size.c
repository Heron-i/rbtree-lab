//Purpose: Unit tests for rb_size, decoupled from rb_insert. Uses the same
// hand-built single-node fixture technique as tests/test_find.c (see that
// file's header comment for why this is safe for rb_find/rb_size but not
// for rb_validate/rb_destroy's realistic-tree cases, which need
// rb_insert's actual rebalancing output).
// Implements RBS-01 from the rb_size test plan. The empty-tree case
// (rb_size == 0) is already covered by RBC-03 in tests/test_create.c;
// insert/delete transition cases (increments on new key, holds on
// overwrite, decrements on delete) are deferred until those exist.
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

/* ---- RBS-01: rb_size reflects the actual node count of a non-empty tree ---- */
static bool test_rbs01_size_matches_populated_tree(void) {
    static int tag = 1;
    rbtree_t *t = build_single_node_tree("10", &tag);
    if (!t) {
        fprintf(stderr, "    build_single_node_tree failed\n");
        return false;
    }
    size_t n = rb_size(t);
    bool ok = (n == 1);
    if (!ok) fprintf(stderr, "    rb_size(one-node tree) = %zu, expected 1\n", n);
    rb_destroy(t);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"RBS-01", "rb_size matches actual node count of a non-empty tree",
     test_rbs01_size_matches_populated_tree},
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
