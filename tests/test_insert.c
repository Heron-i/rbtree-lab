//Purpose: Unit tests for rb_insert, including rebalancing. Every test that
// inserts into a non-empty/rebalancing scenario is verified primarily via
// direct struct access through rbtree_internal.h (root/child/parent
// pointers, color, key content, size as a field) -- the same white-box
// technique RBV-02/RBV-04/etc. in tests/test_rbtree.c already use.
// rb_find/rb_size/rb_validate calls are layered on AFTER those structural
// checks as secondary/confirmatory cross-checks, never as the primary
// assertion: a latent bug in any of those other functions must not be able
// to mask or be mistaken for an rb_insert bug. RBI-01 uses none of them at
// all -- it's the fully standalone case, meaningful even if rb_find/
// rb_size/rb_validate were all simultaneously broken.
// RBI-13/RBI-14 are the deliberate exception: they lean on rb_find/rb_size/
// rb_validate as their entire verification mechanism over a longer
// insertion run, explicitly as a secondary, broad-coverage safety net on
// top of RBI-01..RBI-12's isolated evidence, not a replacement for it.
// Implements RBI-01..RBI-14 from the rb_insert test plan. Deferred: a
// mirrored recolor case, an "overwrite never allocates" case (would assert
// an implementation detail rbtree.h doesn't explicitly promise), and a
// fixed shuffled-order stress case.
#include "rbtree.h"
#include "../src/rbtree_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test-only fault-injection hook defined in rbtree.c (external linkage,
 * intentionally not part of the public header contract) */
extern bool rb_fail_next_alloc;

/* value_free test double for RBI-04: records what it was called with so we
 * can confirm the OLD value (not the new one) was freed exactly once. */
#define FREE_RECORDER_CAP 4
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

static bool free_recorder_saw(void *value) {
    int limit = free_recorder_count < FREE_RECORDER_CAP ? free_recorder_count : FREE_RECORDER_CAP;
    for (int i = 0; i < limit; i++) {
        if (free_recorder_seen[i] == value) return true;
    }
    return false;
}

/* Structural snapshot for RBI-07: records full node identity (not just
 * keys) so a failed insert can be proven byte-for-byte unchanged, not just
 * "still validates" -- see the plan's rationale (a partially-applied
 * fixup could still produce a different, still-valid tree). */
#define SNAPSHOT_CAP 8
struct node_snapshot {
    const struct rb_node *self;
    const struct rb_node *left;
    const struct rb_node *right;
    const struct rb_node *parent;
    rb_color_t color;
    const char *key;
    void *value;
};

struct snapshot_ctx {
    struct node_snapshot nodes[SNAPSHOT_CAP];
    int count;
};

/* pre-order walk: order doesn't matter, as long as both snapshots use the
 * same one */
static void snapshot_walk(const struct rbtree *t, const struct rb_node *n,
                           struct snapshot_ctx *ctx) {
    if (n == &t->nil) return;
    if (ctx->count < SNAPSHOT_CAP) {
        ctx->nodes[ctx->count] = (struct node_snapshot){
            .self = n, .left = n->left, .right = n->right, .parent = n->parent,
            .color = n->color, .key = n->key, .value = n->value,
        };
    }
    ctx->count++;
    snapshot_walk(t, n->left, ctx);
    snapshot_walk(t, n->right, ctx);
}

static bool snapshots_equal(const struct snapshot_ctx *a, const struct snapshot_ctx *b) {
    if (a->count != b->count) return false;
    int limit = a->count < SNAPSHOT_CAP ? a->count : SNAPSHOT_CAP;
    for (int i = 0; i < limit; i++) {
        const struct node_snapshot *na = &a->nodes[i];
        const struct node_snapshot *nb = &b->nodes[i];
        if (na->self != nb->self || na->left != nb->left || na->right != nb->right ||
            na->parent != nb->parent || na->color != nb->color || na->key != nb->key ||
            na->value != nb->value) {
            return false;
        }
    }
    return true;
}

/* ---- RBI-01: insert into an empty tree ----
 * STANDALONE: zero calls to rb_find/rb_size/rb_validate. Verified purely
 * via direct struct access, so this test means something even in a
 * hypothetical world where every other operation in the header is broken. */
static bool test_rbi01_insert_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag;
    int rc = rb_insert(t, "10", &tag);
    bool ok = (rc == 0) && (t->root != &t->nil) && (strcmp(t->root->key, "10") == 0) &&
              (t->root->value == &tag) && (t->root->color == BLACK) &&
              (t->root->left == &t->nil) && (t->root->right == &t->nil) &&
              (t->root->parent == &t->nil) && (t->size == 1);
    if (!ok) {
        fprintf(stderr,
                "    rb_insert(\"10\") into empty tree: rc=%d (want 0), root=%p (want "
                "non-nil), size=%zu (want 1)\n",
                rc, (void *)t->root, t->size);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-02: insert a smaller second key -- parent is black, no fixup
 * needed ---- */
static bool test_rbi02_insert_smaller_no_fixup(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_root, tag_left;
    if (rb_insert(t, "10", &tag_root) != 0) {
        fprintf(stderr, "    rb_insert(\"10\") failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    struct rb_node *root = t->root;
    int rc = rb_insert(t, "05", &tag_left);
    /* primary: structural */
    bool ok = (rc == 0) && (t->root == root) && (strcmp(t->root->key, "10") == 0) &&
              (t->root->left != &t->nil) && (strcmp(t->root->left->key, "05") == 0) &&
              (t->root->left->value == &tag_left) && (t->root->left->color == RED) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (t->root->left->parent == t->root) && (t->root->right == &t->nil) &&
              (t->size == 2);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    rb_insert(\"05\") after \"10\": rc=%d (want 0), size=%zu (want 2), "
                "rb_validate=%d (want 0)\n",
                rc, t->size, rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-03: insert a larger second key -- mirror of RBI-02, catches a
 * left/right-swap bug even in the trivial no-rotation path ---- */
static bool test_rbi03_insert_larger_no_fixup(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_root, tag_right;
    if (rb_insert(t, "10", &tag_root) != 0) {
        fprintf(stderr, "    rb_insert(\"10\") failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    struct rb_node *root = t->root;
    int rc = rb_insert(t, "15", &tag_right);
    bool ok = (rc == 0) && (t->root == root) && (strcmp(t->root->key, "10") == 0) &&
              (t->root->right != &t->nil) && (strcmp(t->root->right->key, "15") == 0) &&
              (t->root->right->value == &tag_right) && (t->root->right->color == RED) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->root->right->parent == t->root) && (t->root->left == &t->nil) &&
              (t->size == 2);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    rb_insert(\"15\") after \"10\": rc=%d (want 0), size=%zu (want 2), "
                "rb_validate=%d (want 0)\n",
                rc, t->size, rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-04: insert an already-present key -- overwrites the value and
 * frees the old one via value_free exactly once ---- */
static bool test_rbi04_overwrite_existing_key(void) {
    rbtree_t *t = rb_create(free_recorder);
    if (!t) {
        fprintf(stderr, "    rb_create(free_recorder) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_old, tag_new;
    if (rb_insert(t, "10", &tag_old) != 0) {
        fprintf(stderr, "    rb_insert(\"10\", old) failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    struct rb_node *node = t->root;
    free_recorder_reset();
    int rc = rb_insert(t, "10", &tag_new);
    /* primary: structural -- same node, new value, size unchanged, old
     * value freed exactly once */
    bool ok = (rc == 0) && (t->root == node) && (t->root->value == &tag_new) && (t->size == 1) &&
              (free_recorder_count == 1) && free_recorder_saw(&tag_old);
    /* secondary: consistency check */
    ok = ok && (rb_find(t, "10") == &tag_new) && (rb_size(t) == 1) && (rb_validate(t) == 0);
    if (!ok) {
        fprintf(stderr,
                "    overwrite \"10\": rc=%d (want 0), same node=%d, value=%p (want %p), "
                "size=%zu (want 1), free_recorder_count=%d (want 1), saw old=%d\n",
                rc, t->root == node, t->root->value, (void *)&tag_new, t->size,
                free_recorder_count, free_recorder_saw(&tag_old));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-05: overwrite with value_free == NULL -- no crash, value still
 * updates (mirrors RBX-05's intent for the overwrite path specifically) ---- */
static bool test_rbi05_overwrite_no_value_free(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_old, tag_new;
    if (rb_insert(t, "10", &tag_old) != 0) {
        fprintf(stderr, "    rb_insert(\"10\", old) failed while building the tree\n");
        rb_destroy(t);
        return false;
    }
    struct rb_node *node = t->root;
    int rc = rb_insert(t, "10", &tag_new);   /* a NULL callback dereferenced would crash here */
    bool ok = (rc == 0) && (t->root == node) && (t->root->value == &tag_new) && (t->size == 1);
    ok = ok && (rb_find(t, "10") == &tag_new) && (rb_validate(t) == 0);   /* secondary */
    if (!ok) {
        fprintf(stderr,
                "    overwrite \"10\" (no value_free): rc=%d (want 0), value=%p (want %p), "
                "size=%zu (want 1)\n",
                rc, t->root->value, (void *)&tag_new, t->size);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-06: allocation failure inserting into an empty tree -- tree
 * stays empty ---- */
static bool test_rbi06_alloc_failure_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag;
    rb_fail_next_alloc = true;
    int rc = rb_insert(t, "10", &tag);
    bool ok = (rc == -1) && (t->root == &t->nil) && (t->size == 0);
    if (!ok) {
        fprintf(stderr,
                "    rb_insert(\"10\") with rb_fail_next_alloc=true on empty tree: rc=%d "
                "(want -1), root=%p (want nil=%p), size=%zu (want 0)\n",
                rc, (void *)t->root, (void *)&t->nil, t->size);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-07: allocation failure inserting a new key into a non-empty
 * tree -- tree must be byte-for-byte unchanged (see the plan: "still
 * valid" is not enough, a partially-applied fixup could still validate) ---- */
static bool test_rbi07_alloc_failure_nonempty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    static int tag_a, tag_b, tag_c;
    const char *keys[] = {"50", "25", "75"};
    void *values[] = {&tag_a, &tag_b, &tag_c};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], values[i]) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    struct rb_node *root_before = t->root;
    size_t size_before = t->size;
    struct snapshot_ctx before = {0};
    snapshot_walk(t, t->root, &before);

    static int tag_new;
    rb_fail_next_alloc = true;
    int rc = rb_insert(t, "10", &tag_new);

    struct snapshot_ctx after = {0};
    snapshot_walk(t, t->root, &after);
    bool ok = (rc == -1) && (t->root == root_before) && (t->size == size_before) &&
              snapshots_equal(&before, &after);
    if (!ok) {
        fprintf(stderr,
                "    rb_insert(\"10\") with rb_fail_next_alloc=true on non-empty tree: rc=%d "
                "(want -1), root unchanged=%d, size=%zu (want %zu), snapshot unchanged=%d\n",
                rc, t->root == root_before, t->size, size_before,
                snapshots_equal(&before, &after));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-08: uncle-red recolor case ----
 * Sequence 50,25,75,10: "10" is inserted as a left-left grandchild with
 * parent(25) RED and uncle(75) RED. Standard fixup: recolor parent and
 * uncle BLACK, grandparent(50) RED, then force root BLACK on exit.
 * Expected: 50B(25B(10R,nil), 75B(nil,nil)). Hand-traced against the
 * standard CLRS insert-fixup algorithm (see the test plan). */
static bool test_rbi08_recolor_uncle_red(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "25", "75", "10"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (strcmp(t->root->key, "50") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "25") == 0) && (t->root->left->color == BLACK) &&
              (strcmp(t->root->left->left->key, "10") == 0) &&
              (t->root->left->left->color == RED) && (t->root->left->left->left == &t->nil) &&
              (t->root->left->left->right == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "75") == 0) && (t->root->right->color == BLACK) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->size == 4);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    recolor case (50,25,75,10): tree did not match the expected shape "
                "50B(25B(10R,nil),75B(nil,nil)); rb_validate=%d\n",
                rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-09: black-uncle line case, left-left ----
 * Sequence 50,25,10: single right-rotation at grandparent(50) + recolor.
 * Expected: 25B(10R,50R). Hand-traced against the standard CLRS
 * insert-fixup algorithm (see the test plan). */
static bool test_rbi09_line_case_left_left(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "25", "10"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (strcmp(t->root->key, "25") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "10") == 0) && (t->root->left->color == RED) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "50") == 0) && (t->root->right->color == RED) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->size == 3);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    line case LL (50,25,10): tree did not match the expected shape "
                "25B(10R,50R); rb_validate=%d\n",
                rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-10: black-uncle triangle case, left-right ----
 * Sequence 50,25,40: left-rotate at the parent(25) first, which turns it
 * into RBI-09's line case, then the same rotate+recolor applies. Expected:
 * 40B(25R,50R). Hand-traced against the standard CLRS insert-fixup
 * algorithm (see the test plan). */
static bool test_rbi10_triangle_case_left_right(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "25", "40"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (strcmp(t->root->key, "40") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "25") == 0) && (t->root->left->color == RED) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "50") == 0) && (t->root->right->color == RED) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->size == 3);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    triangle case LR (50,25,40): tree did not match the expected shape "
                "40B(25R,50R); rb_validate=%d\n",
                rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-11: black-uncle line case, right-right (mirror of RBI-09) ----
 * Sequence 50,75,90. Expected: 75B(50R,90R). */
static bool test_rbi11_line_case_right_right(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "75", "90"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (strcmp(t->root->key, "75") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "50") == 0) && (t->root->left->color == RED) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "90") == 0) && (t->root->right->color == RED) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->size == 3);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    line case RR (50,75,90): tree did not match the expected shape "
                "75B(50R,90R); rb_validate=%d\n",
                rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-12: black-uncle triangle case, right-left (mirror of RBI-10) ----
 * Sequence 50,75,60. Expected: 60B(50R,75R). */
static bool test_rbi12_triangle_case_right_left(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"50", "75", "60"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (rb_insert(t, keys[i], NULL) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed while building the tree\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (strcmp(t->root->key, "60") == 0) && (t->root->color == BLACK) &&
              (strcmp(t->root->left->key, "50") == 0) && (t->root->left->color == RED) &&
              (t->root->left->left == &t->nil) && (t->root->left->right == &t->nil) &&
              (strcmp(t->root->right->key, "75") == 0) && (t->root->right->color == RED) &&
              (t->root->right->left == &t->nil) && (t->root->right->right == &t->nil) &&
              (t->size == 3);
    ok = ok && (rb_validate(t) == 0);   /* secondary consistency check */
    if (!ok) {
        fprintf(stderr,
                "    triangle case RL (50,75,60): tree did not match the expected shape "
                "60B(50R,75R); rb_validate=%d\n",
                rb_validate(t));
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-13: ascending run ----
 * Secondary consistency check, NOT an isolated rb_insert diagnosis (see
 * the file header comment): a failure here means something among
 * insert/find/size/validate together is wrong. RBI-01..RBI-12 are the
 * authoritative isolated evidence; this is a broad-coverage safety net on
 * top, stressing repeated right-heavy rotations across a longer run. */
static bool test_rbi13_ascending_run(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10"};
    size_t n = sizeof(keys) / sizeof(keys[0]);
    for (size_t i = 0; i < n; i++) {
        if (rb_insert(t, keys[i], (void *)keys[i]) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed during ascending run\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (rb_validate(t) == 0) && (rb_size(t) == n);
    for (size_t i = 0; ok && i < n; i++) {
        if (rb_find(t, keys[i]) != (void *)keys[i]) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    ascending run (01..10): rb_validate=%d (want 0), rb_size=%zu (want %zu), "
                "or a key was not found with the right value\n",
                rb_validate(t), rb_size(t), n);
    }
    rb_destroy(t);
    return ok;
}

/* ---- RBI-14: descending run -- mirror of RBI-13, left-heavy stress ---- */
static bool test_rbi14_descending_run(void) {
    rbtree_t *t = rb_create(NULL);
    if (!t) {
        fprintf(stderr, "    rb_create(NULL) returned NULL, cannot build tree\n");
        return false;
    }
    const char *keys[] = {"10", "09", "08", "07", "06", "05", "04", "03", "02", "01"};
    size_t n = sizeof(keys) / sizeof(keys[0]);
    for (size_t i = 0; i < n; i++) {
        if (rb_insert(t, keys[i], (void *)keys[i]) != 0) {
            fprintf(stderr, "    rb_insert(\"%s\") failed during descending run\n", keys[i]);
            rb_destroy(t);
            return false;
        }
    }
    bool ok = (rb_validate(t) == 0) && (rb_size(t) == n);
    for (size_t i = 0; ok && i < n; i++) {
        if (rb_find(t, keys[i]) != (void *)keys[i]) ok = false;
    }
    if (!ok) {
        fprintf(stderr,
                "    descending run (10..01): rb_validate=%d (want 0), rb_size=%zu (want %zu), "
                "or a key was not found with the right value\n",
                rb_validate(t), rb_size(t), n);
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
    {"RBI-01", "insert into an empty tree (standalone)", test_rbi01_insert_empty_tree},
    {"RBI-02", "insert a smaller second key, no fixup", test_rbi02_insert_smaller_no_fixup},
    {"RBI-03", "insert a larger second key, no fixup", test_rbi03_insert_larger_no_fixup},
    {"RBI-04", "overwrite an existing key frees the old value",
     test_rbi04_overwrite_existing_key},
    {"RBI-05", "overwrite with value_free == NULL", test_rbi05_overwrite_no_value_free},
    {"RBI-06", "allocation failure inserting into an empty tree",
     test_rbi06_alloc_failure_empty_tree},
    {"RBI-07", "allocation failure leaves a non-empty tree byte-for-byte unchanged",
     test_rbi07_alloc_failure_nonempty_tree},
    {"RBI-08", "recolor case, uncle red", test_rbi08_recolor_uncle_red},
    {"RBI-09", "line case, black uncle, left-left", test_rbi09_line_case_left_left},
    {"RBI-10", "triangle case, black uncle, left-right", test_rbi10_triangle_case_left_right},
    {"RBI-11", "line case, black uncle, right-right", test_rbi11_line_case_right_right},
    {"RBI-12", "triangle case, black uncle, right-left", test_rbi12_triangle_case_right_left},
    {"RBI-13", "ascending run (secondary consistency check)", test_rbi13_ascending_run},
    {"RBI-14", "descending run (secondary consistency check)", test_rbi14_descending_run},
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
