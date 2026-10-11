// Purpose: Unit tests for pool_alloc (M5, Mutation 2), built as
// build/test_pool_alloc. pool_alloc has the most policy in the pool: three
// sources (free list, then carve, then grow by one slab) and the pool's only
// fault point.
//   PL-03 slots aligned and non-overlapping          (moved from test_pool.c)
//   PL-05 LIFO reuse without rb_malloc               (moved)
//   PL-06 slab-growth fault leaves the pool unchanged (moved)
//   PL-08 exact stats at slab boundaries             (moved)
//   PA-05 a free-list pop cannot fail and makes no rb_malloc
//   PA-06 a freed slot in an older slab beats the newest slab's uncarved slots
//   PA-07 pool fault sweep over a fixed 3-slab script
//   PA-08 boundary triples for 255 slots/slab and 1 slot/slab
// After every step of every scenario, check_stats asserts
//     live + free_objs == slabs * objs_per_slab
#include "pool.h"
#include "pool_check.h"   /* objs_per_slab_for, check_stats, expect_stats */
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the exact expected triples assume 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "PL/PA expected triples are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */

/* ---- PL-03: every slot is POOL_ALIGN-aligned and holds obj_size bytes,
 * across several slabs (asan catches a slot that overlaps its neighbour) ---- */
static bool test_pl03_alignment(void) {
    static const size_t sizes[] = {1, 8, 24, NODE_SIZE, 100};
    bool ok = true;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t s = sizes[i];
        size_t n = 2 * objs_per_slab_for(s) + 3;
        rb_pool_t *p = pool_create(s);
        void **objs = malloc(n * sizeof *objs);
        if (!p || !objs) {
            fprintf(stderr, "    setup failed for obj_size %zu\n", s);
            pool_destroy(p);
            free(objs);
            return false;
        }
        /* invariant: objs[0..k) are live, aligned, and filled with byte k%256 */
        for (size_t k = 0; k < n && ok; k++) {
            objs[k] = pool_alloc(p);
            if (!objs[k] || (uintptr_t)objs[k] % POOL_ALIGN != 0) {
                fprintf(stderr, "    obj_size %zu, alloc %zu: %p not %zu-aligned\n",
                        s, k, objs[k], (size_t)POOL_ALIGN);
                ok = false;
                break;
            }
            memset(objs[k], (int)(k & 0xff), s);
            ok = check_stats(p, s, "PL-03 alloc", (long)k) && ok;
        }
        /* invariant: every slot still holds its own fill byte, so no two overlap */
        for (size_t k = 0; k < n && ok; k++) {
            const unsigned char *b = objs[k];
            if (b[0] != (k & 0xff) || b[s - 1] != (k & 0xff)) {
                fprintf(stderr, "    obj_size %zu: slot %zu was overwritten by a neighbour\n",
                        s, k);
                ok = false;
            }
        }
        pool_destroy(p);
        free(objs);
    }
    return ok;
}

/* ---- PL-05: LIFO reuse -- the slot freed last comes back first, with no
 * rb_malloc ---- */
static bool test_pl05_lifo_reuse(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    void *a = pool_alloc(p);
    void *b = pool_alloc(p);
    bool ok = a && b && check_stats(p, NODE_SIZE, "PL-05 after 2 allocs", 0);
    pool_free(p, a);
    ok = check_stats(p, NODE_SIZE, "PL-05 after free", 1) && ok;
    long a0 = fault_alloc_total();
    void *c = pool_alloc(p);
    if (c != a || fault_alloc_total() != a0) {
        fprintf(stderr, "    realloc returned %p (want freed %p) after %ld rb_malloc calls (want 0)\n",
                c, a, fault_alloc_total() - a0);
        ok = false;
    }
    ok = expect_stats(p, NODE_SIZE, "PL-05 end", 1, 2, 83) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PL-06: a failed slab allocation leaves the pool unchanged, and the
 * next pool_alloc succeeds ---- */
static bool test_pl06_growth_fault(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);
    bool ok = true;

    fault_alloc_arm(1);                    /* first slab fails */
    void *o = pool_alloc(p);
    fault_alloc_disarm();
    if (o != NULL) { fprintf(stderr, "    first-slab fault: pool_alloc returned non-NULL\n"); ok = false; }
    ok = expect_stats(p, NODE_SIZE, "PL-06 after first-slab fault", 0, 0, 0) && ok;

    /* invariant: k objects allocated, all from slab 1 */
    for (size_t k = 0; k < per && ok; k++) {
        if (!pool_alloc(p)) { fprintf(stderr, "    alloc %zu failed\n", k); ok = false; }
    }
    ok = expect_stats(p, NODE_SIZE, "PL-06 slab 1 full", 1, per, 0) && ok;

    fault_alloc_arm(1);                    /* growth to slab 2 fails */
    o = pool_alloc(p);
    fault_alloc_disarm();
    if (o != NULL) { fprintf(stderr, "    growth fault: pool_alloc returned non-NULL\n"); ok = false; }
    ok = expect_stats(p, NODE_SIZE, "PL-06 after growth fault", 1, per, 0) && ok;

    o = pool_alloc(p);
    if (o == NULL) { fprintf(stderr, "    pool_alloc after the fault returned NULL\n"); ok = false; }
    ok = expect_stats(p, NODE_SIZE, "PL-06 recovered", 2, per + 1, per - 1) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PL-08: exact triples at the slab boundaries ---- */
static bool test_pl08_slab_boundaries(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);   /* 85 */
    void *objs[86];
    bool ok = true;
    /* invariant: objs[0..k) live, k <= per + 1 */
    for (size_t k = 0; k < per + 1 && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { ok = false; break; }
        if (k == 0) ok = expect_stats(p, NODE_SIZE, "after alloc 1", 1, 1, per - 1) && ok;
        if (k == per - 1) ok = expect_stats(p, NODE_SIZE, "after alloc 85", 1, per, 0) && ok;
        if (k == per) ok = expect_stats(p, NODE_SIZE, "after alloc 86", 2, per + 1, per - 1) && ok;
    }
    /* invariant: objs[0..k) have been returned to the free list */
    for (size_t k = 0; k < per + 1 && ok; k++) {
        pool_free(p, objs[k]);
        ok = check_stats(p, NODE_SIZE, "PL-08 free", (long)k) && ok;
    }
    ok = expect_stats(p, NODE_SIZE, "all freed", 2, 0, 2 * per) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PA-05: an alloc served from the free list makes no rb_malloc, so it
 * succeeds under an armed fault and leaves that fault pending ---- */
static bool test_pa05_pop_cannot_fail(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    void *a = pool_alloc(p);
    bool ok = a != NULL;
    pool_free(p, a);
    ok = expect_stats(p, NODE_SIZE, "PA-05 after free", 1, 0, 85) && ok;

    long a0 = fault_alloc_total();
    fault_alloc_arm(1);
    void *c = pool_alloc(p);
    long allocs = fault_alloc_total() - a0;
    /* the fault must still be pending: the very next rb_malloc fails */
    rb_pool_t *probe = pool_create(NODE_SIZE);
    fault_alloc_disarm();
    if (c != a || allocs != 0 || probe != NULL) {
        fprintf(stderr, "    pop under arm(1): got %p (want %p), %ld rb_malloc calls (want 0), "
                "fault still pending=%d\n", c, a, allocs, probe == NULL);
        ok = false;
    }
    pool_destroy(probe);
    ok = expect_stats(p, NODE_SIZE, "PA-05 end", 1, 1, 84) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PA-06: policy order -- a slot freed in an older slab is reused
 * before the newest slab's uncarved slots, so uncarved does not move ---- */
static bool test_pa06_free_list_before_carve(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);   /* 85 */
    void *objs[86];
    bool ok = true;
    /* invariant: objs[0..k) live; objs[0..per) are in slab 1 */
    for (size_t k = 0; k < per + 1 && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { fprintf(stderr, "    PA-06 alloc %zu failed\n", k); ok = false; break; }
        ok = check_stats(p, NODE_SIZE, "PA-06 alloc", (long)k) && ok;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PA-06 slab 2 started", 2, per + 1, per - 1);
    pool_free(p, objs[0]);   /* slab 1 */
    ok = expect_stats(p, NODE_SIZE, "PA-06 freed slab-1 slot", 2, per, per) && ok;
    void *o = pool_alloc(p);
    if (o != objs[0]) {
        fprintf(stderr, "    PA-06: alloc returned %p, want the freed slab-1 slot %p\n", o, objs[0]);
        ok = false;
    }
    ok = expect_stats(p, NODE_SIZE, "PA-06 reused", 2, per + 1, per - 1) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PA-07: pool fault sweep. For n = 1, 2, ... a fixed script (300
 * allocs, a LIFO free after every 4th, peaking at 225 live = 3 slabs) runs
 * with the n-th rb_malloc failing. Allocations: pool_create, then one per
 * slab, so N = 1 + 3. A failed pool_alloc must be a growth step and leave
 * the triple unchanged; the script then carries on. ---- */
#define PA07_ALLOCS 300
#define PA07_EXPECTED_N 4L

struct pa07_run {
    bool ok;
    bool fired;
    long allocs;
    int failures;
};

static struct pa07_run pa07_run(long n) {
    struct pa07_run r = {.ok = true};
    long t0 = fault_alloc_total();
    fault_alloc_arm(n);
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (p == NULL) {
        r.failures++;
        if (n != 1) {
            fprintf(stderr, "    PA-07 n=%ld: pool_create failed, expected only at n=1\n", n);
            r.ok = false;
        }
        goto finish;
    }
    void *live[PA07_ALLOCS];
    size_t nlive = 0;
    /* invariant: live[0..nlive) are exactly the objects handed out and not freed */
    for (long i = 0; i < PA07_ALLOCS; i++) {
        size_t s0, l0, f0, s1, l1, f1;
        pool_stats(p, &s0, &l0, &f0);
        void *o = pool_alloc(p);
        if (o != NULL) {
            live[nlive++] = o;
        } else {
            r.failures++;
            pool_stats(p, &s1, &l1, &f1);
            if (f0 != 0 || s1 != s0 || l1 != l0 || f1 != f0) {
                fprintf(stderr, "    PA-07 n=%ld op %ld: failed alloc with free_objs=%zu before; "
                        "stats (%zu,%zu,%zu) -> (%zu,%zu,%zu)\n", n, i, f0, s0, l0, f0, s1, l1, f1);
                r.ok = false;
            }
        }
        if (i % 4 == 3 && nlive > 0) pool_free(p, live[--nlive]);
        r.ok = check_stats(p, NODE_SIZE, "PA-07", i) && r.ok;
    }
    pool_destroy(p);
finish:
    fault_alloc_disarm();
    r.allocs = fault_alloc_total() - t0;
    r.fired = r.allocs >= n;
    if (r.failures != (r.fired ? 1 : 0)) {
        fprintf(stderr, "    PA-07 n=%ld: %d failed allocation(s), fault fired=%d\n",
                n, r.failures, r.fired);
        r.ok = false;
    }
    return r;
}

static bool test_pa07_fault_sweep(void) {
    long n = 1;
    bool ok = true;
    struct pa07_run r;
    /* invariant: every n' < n fired its fault and (unless ok is false) left
     * the pool unchanged at the failed step */
    for (;;) {
        if (n > 100) {
            fprintf(stderr, "    PA-07 sweep did not terminate by n=100\n");
            return false;
        }
        r = pa07_run(n);
        ok = r.ok && ok;
        if (!r.fired) break;
        n++;
    }
    struct pa07_run again = pa07_run(n);
    if (r.allocs != PA07_EXPECTED_N || again.fired || again.allocs != r.allocs || !again.ok) {
        fprintf(stderr, "    PA-07: N=%ld (want %ld), clean rerun N=%ld fired=%d ok=%d\n",
                r.allocs, PA07_EXPECTED_N, again.allocs, again.fired, again.ok);
        ok = false;
    }
    return ok;
}

/* ---- PA-08: exact triples at the slab boundaries for the smallest
 * stride (255 slots per slab) and the largest (1 slot per slab) ---- */
static bool test_pa08_boundary_triples(void) {
    bool ok = true;
    rb_pool_t *p = pool_create(1);
    if (!p) return false;
    size_t per = objs_per_slab_for(1);   /* 255 */
    /* invariant: k objects allocated from the obj_size-1 pool */
    for (size_t k = 1; k <= per + 1 && ok; k++) {
        if (!pool_alloc(p)) { fprintf(stderr, "    PA-08 size 1 alloc %zu failed\n", k); ok = false; break; }
        ok = check_stats(p, 1, "PA-08 size 1", (long)k) && ok;
        if (k == 1) ok = expect_stats(p, 1, "size 1 after alloc 1", 1, 1, per - 1) && ok;
        if (k == per) ok = expect_stats(p, 1, "size 1 after alloc 255", 1, per, 0) && ok;
        if (k == per + 1) ok = expect_stats(p, 1, "size 1 after alloc 256", 2, per + 1, per - 1) && ok;
    }
    pool_destroy(p);

    p = pool_create(4080);
    if (!p) return false;
    ok = (pool_alloc(p) != NULL) && ok;
    ok = expect_stats(p, 4080, "size 4080 after alloc 1", 1, 1, 0) && ok;
    ok = (pool_alloc(p) != NULL) && ok;
    ok = expect_stats(p, 4080, "size 4080 after alloc 2", 2, 2, 0) && ok;
    pool_destroy(p);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"PL-03", "slots aligned and non-overlapping", test_pl03_alignment},
    {"PL-05", "LIFO reuse without rb_malloc", test_pl05_lifo_reuse},
    {"PL-06", "slab-growth fault leaves pool unchanged", test_pl06_growth_fault},
    {"PL-08", "exact stats at slab boundaries", test_pl08_slab_boundaries},
    {"PA-05", "free-list pop makes no rb_malloc, even armed", test_pa05_pop_cannot_fail},
    {"PA-06", "older freed slot reused before uncarved", test_pa06_free_list_before_carve},
    {"PA-07", "pool fault sweep over a 3-slab script", test_pa07_fault_sweep},
    {"PA-08", "boundary triples at 255 and 1 slot per slab", test_pa08_boundary_triples},
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
