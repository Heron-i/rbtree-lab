//Purpose: Unit tests for rb_foreach, isolated from tests/test_rbtree.c's
// scenario tests (RBV-07 there stays put -- it's a legitimate insert+
// foreach+validate scenario, the same shape as RBV-01..06/08/10, not a
// foreach unit test that leaked into the wrong file; see the test plan).
// Every test here is verified primarily via its own recorded callback
// invocations (record_call/struct foreach_recorder below) -- the direct,
// first-party observation of what rb_foreach actually did. rb_validate and
// rb_size are NOT used anywhere in this file: neither has any causal
// relationship to what rb_foreach does (both are computed independently of
// traversal), so they would add no diagnostic value as secondary checks
// here, unlike in tests/test_insert.c where rb_validate genuinely could
// catch a broken rotation. rb_find appears exactly once (RBW-09), where an
// independent lookup path is actually useful corroboration.
// Fixtures are built with rb_insert, now legitimate here since it has its
// own independently-green test suite (tests/test_insert.c).
// Implements RBW-01..RBW-10 from the rb_foreach test plan.
#include "rbtree.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared double: records every (key, value, ctx) triple rb_foreach calls
 * fn with, in call order. ctx is always the recorder's own address, so a
 * test can compare each recorded .ctx against the known &rec for the
 * identity check (RBW-08) while also using .key/.value for everything
 * else. Same shape as tests/test_rbtree.c's traversal_ctx/collect_key,
 * generalized to also capture value and ctx. */
#define FOREACH_CAP 12
struct call_record {
    const char *key;
    void *value;
    void *ctx;
};

struct foreach_recorder {
    struct call_record calls[FOREACH_CAP];
    int count;
};

static void record_call(const char *key, void *value, void *ctx) {
    struct foreach_recorder *rec = ctx;
    if (rec->count < FOREACH_CAP) {
        rec->calls[rec->count] = (struct call_record){.key = key, .value = value, .ctx = ctx};
    }
    rec->count++;
}

/* ---- RBW-01: foreach on an empty tree calls fn zero times ---- */
static bool test_rbw01_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 0);
    if (!ok) {
        fprintf(stderr, "    rb_foreach(empty tree): call count=%d (want 0)\n", rec.count);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-02: foreach on a one-node tree ----
 * STANDALONE: zero calls to rb_find/rb_size/rb_validate -- meaningful even
 * if all three were simultaneously broken. */
static bool test_rbw02_single_node(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag;
    if (rb_insert(t, "10", &tag) != 0) {
        fprintf(stderr, "    rb_insert(\"10\") failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 1) && (strcmp(rec.calls[0].key, "10") == 0) &&
              (rec.calls[0].value == &tag) && (rec.calls[0].ctx == &rec);
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach(one-node tree \"10\"): count=%d (want 1), key/value/ctx "
                "mismatch\n",
                rec.count);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-03: foreach visits every node exactly once ----
 * Count only, independent of order -- isolates a skipped/double-visited
 * node from an ordering bug (RBW-04). */
static bool test_rbw03_visits_every_node_once(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 5);
    if (!ok) {
        fprintf(stderr, "    rb_foreach(5-node tree): call count=%d (want 5)\n", rec.count);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-04: in-order traversal yields strictly increasing keys ----
 * (This is RBV-07's property, independently re-derived here as
 * rb_foreach's own unit test rather than relocated from it.) */
static bool test_rbw04_strictly_increasing_order(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 5);
    for (int i = 0; ok && i + 1 < rec.count; i++) {
        if (strcmp(rec.calls[i].key, rec.calls[i + 1].key) >= 0) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach(5-node tree): keys not strictly increasing (count=%d, want 5)\n",
                rec.count);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-05: traversal order depends on keys, not insertion order/shape ----
 * Ascending- and descending-inserted trees end up differently shaped (per
 * rb_insert's rebalancing) but must traverse to the same key sequence. */
static bool test_rbw05_order_independent_of_insertion_order(void) {
    rbtree_t *t_asc = rb_create(NULL);
    rbtree_t *t_desc = rb_create(NULL);
    if (!t_asc || !t_desc) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build trees\n");
        if (t_asc) rb_destroy(t_asc);
        if (t_desc) rb_destroy(t_desc);
        return false;
    }
    const char *asc_keys[] = {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10"};
    const char *desc_keys[] = {"10", "09", "08", "07", "06", "05", "04", "03", "02", "01"};
    size_t n = sizeof(asc_keys) / sizeof(asc_keys[0]);
    for (size_t i = 0; i < n; i++) {
        if (rb_insert(t_asc, asc_keys[i], NULL) != 0 ||
            rb_insert(t_desc, desc_keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert failed while building the trees\n");
            rb_destroy(t_asc);
            rb_destroy(t_desc);
            return false;
        }
    }
    struct foreach_recorder rec_asc = {0};
    struct foreach_recorder rec_desc = {0};
    rb_foreach(t_asc, record_call, &rec_asc);
    rb_foreach(t_desc, record_call, &rec_desc);
    bool ok = (rec_asc.count == (int)n) && (rec_desc.count == (int)n);
    for (int i = 0; ok && i < (int)n; i++) {
        if (strcmp(rec_asc.calls[i].key, rec_desc.calls[i].key) != 0) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    ascending-built vs descending-built trees: recorded key sequences differ "
                "(asc count=%d, desc count=%d, want %zu each)\n",
                rec_asc.count, rec_desc.count, n);
    }
    rb_destroy(t_asc);
    rb_destroy(t_desc);
    return ok;
}

/* ---- RBW-06: recorded keys match a fully-known expected sequence,
 * position by position -- not just "increasing" ---- */
static bool test_rbw06_exact_key_sequence(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    const char *expected[] = {"03", "05", "07", "10", "15"};
    size_t n = sizeof(expected) / sizeof(expected[0]);
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == (int)n);
    for (size_t i = 0; ok && i < n; i++) {
        if (strcmp(rec.calls[i].key, expected[i]) != 0) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach(5-node tree): recorded key sequence did not match expected "
                "03,05,07,10,15 (count=%d, want %zu)\n",
                rec.count, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-07: recorded value at each position matches the value inserted
 * for that specific key ---- */
static bool test_rbw07_value_argument_correctness(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_10, tag_05, tag_15, tag_03, tag_07;
    struct {
        const char *key;
        void *value;
    } entries[] = {
        {"10", &tag_10}, {"05", &tag_05}, {"15", &tag_15}, {"03", &tag_03}, {"07", &tag_07},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (rb_insert(t, entries[i].key, entries[i].value) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n",
                    entries[i].key);
            rb_destroy(t);
            return false;
        }
    }
    /* expected sorted order: 03,05,07,10,15 */
    void *expected_values[] = {&tag_03, &tag_05, &tag_07, &tag_10, &tag_15};
    size_t n = sizeof(expected_values) / sizeof(expected_values[0]);
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == (int)n);
    for (size_t i = 0; ok && i < n; i++) {
        if (rec.calls[i].value != expected_values[i]) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach(5-node tree): recorded values did not match expected per-key "
                "values (count=%d, want %zu)\n",
                rec.count, n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-08: ctx is forwarded unchanged on every single call, not just
 * the first ---- */
static bool test_rbw08_ctx_identity_every_call(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 5);
    for (int i = 0; ok && i < rec.count; i++) {
        if (rec.calls[i].ctx != &rec) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach(5-node tree): ctx pointer did not match &rec on every call "
                "(count=%d, want 5)\n",
                rec.count);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-09: an overwritten key appears exactly once, with the new value ---- */
static bool test_rbw09_overwrite_appears_once(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    static int tag_new;
    if (rb_insert(t, "05", &tag_new) != 0) {
        fprintf(stderr, "    rb_insert(\"05\", new) failed while overwriting\n");
        rb_destroy(t);
        return false;
    }
    struct foreach_recorder rec = {0};
    rb_foreach(t, record_call, &rec);
    bool ok = (rec.count == 3);
    int matches = 0;
    for (int i = 0; ok && i < rec.count; i++) {
        if (strcmp(rec.calls[i].key, "05") == 0) {
            matches++;
            if (rec.calls[i].value != &tag_new) ok = false;
        }
    }
    ok = ok && (matches == 1);
    /* secondary: an independent lookup path corroborates the tree's actual
     * state, not just what this recorder happened to capture */
    ok = ok && (rb_find(t, "05") == &tag_new);
    if (!ok) {
        fprintf(stderr,
                "    rb_foreach after overwriting \"05\": count=%d (want 3), matches=%d (want "
                "1), rb_find(\"05\")=%p (want %p)\n",
                rec.count, matches, rb_find(t, "05"), (void *)&tag_new);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-10: calling rb_foreach twice on an unmodified tree produces
 * identical results both times -- foreach must not mutate what it walks ---- */
static bool test_rbw10_repeated_calls_are_idempotent(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "05", "15", "03", "07"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    struct foreach_recorder rec1 = {0};
    struct foreach_recorder rec2 = {0};
    rb_foreach(t, record_call, &rec1);
    rb_foreach(t, record_call, &rec2);
    bool ok = (rec1.count == rec2.count);
    for (int i = 0; ok && i < rec1.count; i++) {
        if (strcmp(rec1.calls[i].key, rec2.calls[i].key) != 0) ok = false;
        if (rec1.calls[i].value != rec2.calls[i].value) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    two rb_foreach calls on the same unmodified tree produced different "
                "results (count1=%d, count2=%d)\n",
                rec1.count, rec2.count);
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
    {"RBW-01", "foreach on an empty tree calls fn zero times", test_rbw01_empty_tree},
    {"RBW-02", "foreach on a one-node tree (standalone)", test_rbw02_single_node},
    {"RBW-03", "foreach visits every node exactly once", test_rbw03_visits_every_node_once},
    {"RBW-04", "in-order traversal yields strictly increasing keys",
     test_rbw04_strictly_increasing_order},
    {"RBW-05", "traversal order is independent of insertion order/shape",
     test_rbw05_order_independent_of_insertion_order},
    {"RBW-06", "recorded key sequence matches the exact expected order",
     test_rbw06_exact_key_sequence},
    {"RBW-07", "recorded value matches the value inserted for each key",
     test_rbw07_value_argument_correctness},
    {"RBW-08", "ctx is forwarded unchanged on every call",
     test_rbw08_ctx_identity_every_call},
    {"RBW-09", "an overwritten key appears exactly once, with the new value",
     test_rbw09_overwrite_appears_once},
    {"RBW-10", "repeated foreach calls on an unmodified tree are idempotent",
     test_rbw10_repeated_calls_are_idempotent},
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
