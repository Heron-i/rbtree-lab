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
// RBW-11..14 (Mutation 3) pin the iterative walk's contract:
//   RBW-11 hand-built deep shapes (fixture_build below, via
//          rbtree_internal.h) are walked in order;
//   RBW-12 the walk allocates nothing and leaves pool_stats unchanged, and
//          the parent pointer it climbs costs no pool slot;
//   RBW-13 a callback that never returns (longjmp) leaves a valid tree;
//   RBW-14 a nested rb_foreach from inside fn is safe.
// RBW-13 is the one place rb_validate is used: a traversal that rewrote
// links in place (Morris) could leave the tree corrupt, and that is
// exactly what it checks. The 10^6-node, 64 KB-stack proof is
// tests/test_stress.c.
#include "rbtree.h"
#include "../src/rbtree_internal.h"
#include "../src/pool.h"
#include "fault_alloc.h"

#include <setjmp.h>
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

/* ---- Hand-built shapes (Mutation 3) ----
 * Far deeper than any red-black tree: a chain of n nodes has height n.
 * NOT red-black trees, so never rb_validate them. BST order ("k<idx>"
 * sorts like idx) and parent links are correct. Node storage is whatever
 * rb_destroy releases (t->pool if set, else rb_malloc), so the fixture
 * also works in the pooled build. Same helper as tests/test_destroy.c. */
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

#define SHAPE_N 4096u
/* value identity for keys k0..k(SHAPE_N-1): key k<idx> holds &marks[idx] */
static unsigned char marks[SHAPE_N];

static bool key_is(const char *key, unsigned idx) {
    char want[FIXTURE_KEY_LEN];
    snprintf(want, sizeof want, "k%07u", idx);
    return strcmp(key, want) == 0;
}

/* Callback state: the walk must visit k0..k(n-1) in order, each with &marks[idx]. */
struct walk_check {
    unsigned n;
    unsigned next;   /* index of the key the walk must visit next */
    bool ok;
};

static void walk_check_cb(const char *key, void *value, void *ctx) {
    struct walk_check *w = ctx;
    if (!w->ok) return;
    if (w->next >= w->n || !key_is(key, w->next) || value != &marks[w->next]) {
        fprintf(stderr, "    walk position %u: got key \"%s\" (want k%07u with its own value)\n",
                w->next, key, w->next);
        w->ok = false;
        return;
    }
    w->next++;
}

/* True iff rb_foreach visits exactly k0..k(n-1), in order, with their values. */
static bool walk_in_order(const rbtree_t *t, unsigned n, const char *what) {
    struct walk_check w = {.n = n, .next = 0, .ok = true};
    rb_foreach(t, walk_check_cb, &w);
    if (w.ok && w.next != n) {
        fprintf(stderr, "    %s: walk visited %u keys (want %u)\n", what, w.next, n);
        w.ok = false;
    }
    return w.ok;
}

/* A real red-black tree of keys k0..k(n-1) (n <= SHAPE_N), inserted in
 * descending order, key k<idx> holding &marks[idx]. NULL on failure. */
static rbtree_t *build_inserted(unsigned n) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return NULL;
    }
    char key[FIXTURE_KEY_LEN];
    /* invariant: keys k(i+1)..k(n-1) are in t */
    for (unsigned i = n; i-- > 0;) {
        snprintf(key, sizeof key, "k%07u", i);
        if (rb_insert(t, key, &marks[i]) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", key);
            rb_destroy(t);
            return NULL;
        }
    }
    return t;
}

/* ---- RBW-11: hand-built 4096-deep shapes are walked in order ----
 * Left chain: one 4095-deep descent, then a one-level climb per node.
 * Right chain: no climb until the end, then one 4095-level climb.
 * Zigzag: low keys are reached by short descents, high keys by climbs
 * back up through the low keys. */
static bool test_rbw11_hand_built_shapes_in_order(void) {
    const enum fixture_shape shapes[] = {FIXTURE_LEFT_CHAIN, FIXTURE_RIGHT_CHAIN, FIXTURE_ZIGZAG};
    const char *names[] = {"left chain", "right chain", "zigzag"};
    /* invariant: every shape before s was walked in order */
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        rbtree_t *t = rb_create(NULL);
        if (!t) {
            fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
            return false;
        }
        bool ok = fixture_build(t, shapes[s], SHAPE_N, marks);
        if (!ok) fprintf(stderr, "    %s: allocation failed while building\n", names[s]);
        ok = ok && walk_in_order(t, SHAPE_N, names[s]);
        rb_destroy(t);
        if (!ok) return false;
    }
    return true;
}

/* ---- RBW-12: the walk allocates nothing, leaves pool_stats unchanged,
 * and the parent pointer it climbs costs no pool slot ---- */
static bool test_rbw12_walk_allocates_nothing(void) {
    /* the node as it is, and the same node minus its parent pointer */
    struct pool_geom with_parent, without_parent;
    if (pool_geometry(sizeof(struct rb_node), &with_parent) != 0 ||
        pool_geometry(sizeof(struct rb_node) - sizeof(struct rb_node *), &without_parent) != 0) {
        fprintf(stderr, "    pool_geometry rejected the node size\n");
        return false;
    }
    if (with_parent.stride != without_parent.stride ||
        with_parent.objs_per_slab != without_parent.objs_per_slab) {
        fprintf(stderr,
                "    parent pointer costs pool space: stride %zu vs %zu, %zu vs %zu objs/slab\n",
                with_parent.stride, without_parent.stride, with_parent.objs_per_slab,
                without_parent.objs_per_slab);
        return false;
    }
    rbtree_t *t = build_inserted(SHAPE_N);
    if (!t) return false;
    size_t slabs0 = 0, live0 = 0, free0 = 0, slabs1 = 0, live1 = 0, free1 = 0;
    if (t->pool != NULL) pool_stats(t->pool, &slabs0, &live0, &free0);
    long before = fault_alloc_total();
    bool ok = walk_in_order(t, SHAPE_N, "inserted tree");
    long made = fault_alloc_total() - before;
    if (t->pool != NULL) pool_stats(t->pool, &slabs1, &live1, &free1);
    if (made != 0) {
        fprintf(stderr, "    rb_foreach made %ld rb_malloc calls (want 0)\n", made);
        ok = false;
    }
    if (slabs0 != slabs1 || live0 != live1 || free0 != free1) {
        fprintf(stderr, "    pool_stats changed across rb_foreach: (%zu,%zu,%zu) -> (%zu,%zu,%zu)\n",
                slabs0, live0, free0, slabs1, live1, free1);
        ok = false;
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBW-13: a callback that never returns (longjmp) leaves the tree
 * valid, the same size, and fully walkable again ---- */
static jmp_buf escape_point;

struct escape_ctx {
    unsigned calls;
    unsigned stop_after;
};

static void escape_cb(const char *key, void *value, void *ctx) {
    (void)key;
    (void)value;
    struct escape_ctx *e = ctx;
    if (++e->calls == e->stop_after) longjmp(escape_point, 1);
}

static bool test_rbw13_longjmp_out_leaves_valid_tree(void) {
    rbtree_t *t = build_inserted(SHAPE_N);
    if (!t) return false;
    struct escape_ctx e = {.calls = 0, .stop_after = SHAPE_N / 2};
    if (setjmp(escape_point) == 0) {
        rb_foreach(t, escape_cb, &e);
        fprintf(stderr, "    rb_foreach returned; the callback never escaped\n");
        rb_destroy(t);
        return false;
    }
    bool ok = true;
    if (rb_validate(t) != 0 || rb_size(t) != SHAPE_N) {
        fprintf(stderr, "    after escape: rb_validate=%d (want 0), rb_size=%zu (want %u)\n",
                rb_validate(t), rb_size(t), SHAPE_N);
        ok = false;
    }
    ok = ok && walk_in_order(t, SHAPE_N, "walk after escape");
    rb_destroy(t);
    return ok;
}

/* ---- RBW-14: rb_foreach from inside fn (read-only reentrancy) ---- */
#define NESTED_N 64u

struct nested_ctx {
    const rbtree_t *t;
    unsigned outer_next;   /* key the outer walk must visit next */
    unsigned inner_next;   /* key the current inner walk must visit next */
    unsigned long calls;   /* inner callbacks so far */
    bool ok;
};

static void inner_cb(const char *key, void *value, void *ctx) {
    (void)value;
    struct nested_ctx *c = ctx;
    if (c->ok && !key_is(key, c->inner_next)) {
        fprintf(stderr, "    inner walk under k%07u: got \"%s\" (want k%07u)\n", c->outer_next,
                key, c->inner_next);
        c->ok = false;
    }
    c->inner_next++;
    c->calls++;
}

static void outer_cb(const char *key, void *value, void *ctx) {
    (void)value;
    struct nested_ctx *c = ctx;
    if (c->ok && !key_is(key, c->outer_next)) {
        fprintf(stderr, "    outer walk: got \"%s\" (want k%07u)\n", key, c->outer_next);
        c->ok = false;
    }
    c->inner_next = 0;
    rb_foreach(c->t, inner_cb, c);
    if (c->ok && c->inner_next != NESTED_N) {
        fprintf(stderr, "    inner walk under k%07u visited %u keys (want %u)\n", c->outer_next,
                c->inner_next, NESTED_N);
        c->ok = false;
    }
    c->outer_next++;
}

static bool test_rbw14_nested_foreach(void) {
    rbtree_t *t = build_inserted(NESTED_N);
    if (!t) return false;
    struct nested_ctx c = {.t = t, .outer_next = 0, .inner_next = 0, .calls = 0, .ok = true};
    rb_foreach(t, outer_cb, &c);
    if (c.ok && (c.outer_next != NESTED_N || c.calls != (unsigned long)NESTED_N * NESTED_N)) {
        fprintf(stderr, "    nested walk: %u outer, %lu inner calls (want %u, %lu)\n",
                c.outer_next, c.calls, NESTED_N, (unsigned long)NESTED_N * NESTED_N);
        c.ok = false;
    }
    rb_destroy(t);
    return c.ok;
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
    {"RBW-11", "hand-built 4096-deep shapes are walked in order",
     test_rbw11_hand_built_shapes_in_order},
    {"RBW-12", "the walk allocates nothing; parent costs no pool slot",
     test_rbw12_walk_allocates_nothing},
    {"RBW-13", "a callback that longjmps out leaves a valid tree",
     test_rbw13_longjmp_out_leaves_valid_tree},
    {"RBW-14", "rb_foreach nested inside fn is safe", test_rbw14_nested_foreach},
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
