// Purpose: Unit tests for the slab pool (M5, Mutation 2). Not built yet:
// joins the Makefile when pool_create/alloc/free/stats/destroy land. The
// geometry tests (GEO-01..GEO-03) live in tests/test_pool_geom.c.
// PL-01..PL-09 drive the pool itself; after every step of every scenario,
// check_stats takes the numbers pool_stats reports and asserts
//     live + free_objs == slabs * objs_per_slab
// with objs_per_slab taken from pool_geometry, not from pool internals.
// The pool also asserts the same invariant internally in debug builds
// (POOL_CHECK in src/pool.c); this file checks it from the outside.
#include "pool.h"
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the exact expected triples assume 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "PL expected triples are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */

static size_t objs_per_slab_for(size_t obj_size) {
    struct pool_geom g;
    return pool_geometry(obj_size, &g) == 0 ? g.objs_per_slab : 0;
}

/* Reads the pool's own numbers and checks the conservation invariant. */
static bool check_stats(const rb_pool_t *p, size_t obj_size, const char *where, long step) {
    size_t slabs, live, free_objs;
    pool_stats(p, &slabs, &live, &free_objs);
    size_t per = objs_per_slab_for(obj_size);
    if (live + free_objs != slabs * per) {
        fprintf(stderr, "    %s step %ld: live=%zu + free_objs=%zu != slabs=%zu * %zu\n",
                where, step, live, free_objs, slabs, per);
        return false;
    }
    return true;
}

/* check_stats plus an exact expected triple. */
static bool expect_stats(const rb_pool_t *p, size_t obj_size, const char *where,
                         size_t want_slabs, size_t want_live, size_t want_free) {
    size_t slabs, live, free_objs;
    pool_stats(p, &slabs, &live, &free_objs);
    bool ok = check_stats(p, obj_size, where, -1);
    if (slabs != want_slabs || live != want_live || free_objs != want_free) {
        fprintf(stderr, "    %s: stats (%zu, %zu, %zu), want (%zu, %zu, %zu)\n",
                where, slabs, live, free_objs, want_slabs, want_live, want_free);
        ok = false;
    }
    return ok;
}

/* ---- PL-01: a fresh pool holds no slab and costs exactly one rb_malloc ---- */
static bool test_pl01_create(void) {
    long a0 = fault_alloc_total();
    rb_pool_t *p = pool_create(NODE_SIZE);
    long allocs = fault_alloc_total() - a0;
    if (!p) {
        fprintf(stderr, "    pool_create(%zu) returned NULL\n", NODE_SIZE);
        return false;
    }
    bool ok = expect_stats(p, NODE_SIZE, "fresh pool", 0, 0, 0);
    if (allocs != 1) {
        fprintf(stderr, "    pool_create made %ld rb_malloc calls, want 1 (lazy first slab)\n",
                allocs);
        ok = false;
    }
    pool_destroy(p);
    return ok;
}

/* ---- PL-02: impossible sizes are rejected without allocating ---- */
static bool test_pl02_create_rejects(void) {
    static const size_t bad[] = {0, POOL_SLAB_SIZE - 15, SIZE_MAX};
    bool ok = true;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        long a0 = fault_alloc_total();
        rb_pool_t *p = pool_create(bad[i]);
        if (p != NULL || fault_alloc_total() != a0) {
            fprintf(stderr, "    pool_create(%zu): returned %p after %ld allocations, "
                    "want NULL after 0\n", bad[i], (void *)p, fault_alloc_total() - a0);
            pool_destroy(p);
            ok = false;
        }
    }
    return ok;
}

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

/* ---- PL-04: 3000 deterministic random alloc/free ops across several
 * slabs; the invariant holds after every op and live matches the model ---- */
static bool test_pl04_random_ops(void) {
    enum { NOPS = 3000, CAP = 400 };
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    void *live[CAP];
    size_t nlive = 0;
    unsigned seed = 12345u;
    bool ok = true;
    /* invariant: live[0..nlive) are exactly the objects handed out and not freed */
    for (long op = 0; op < NOPS && ok; op++) {
        seed = seed * 1103515245u + 12345u;
        bool do_alloc = nlive == 0 || (nlive < CAP && (seed >> 16) % 3 != 0);
        if (do_alloc) {
            void *o = pool_alloc(p);
            if (!o) {
                fprintf(stderr, "    op %ld: pool_alloc returned NULL\n", op);
                ok = false;
                break;
            }
            live[nlive++] = o;
        } else {
            size_t victim = (seed >> 8) % nlive;
            pool_free(p, live[victim]);
            live[victim] = live[--nlive];
        }
        size_t slabs, plive, free_objs;
        pool_stats(p, &slabs, &plive, &free_objs);
        if (plive != nlive) {
            fprintf(stderr, "    op %ld: pool reports live=%zu, model has %zu\n", op, plive, nlive);
            ok = false;
        }
        ok = check_stats(p, NODE_SIZE, "PL-04", op) && ok;
    }
    size_t slabs;
    pool_stats(p, &slabs, NULL, NULL);
    if (ok && slabs < 3) {
        fprintf(stderr, "    PL-04 only reached %zu slabs; scenario must cross several\n", slabs);
        ok = false;
    }
    pool_destroy(p);
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

/* ---- PL-07: pool_destroy releases every slab even with objects live
 * (leaks are caught by asan/memcheck); pool_destroy(NULL) is a no-op ---- */
static bool test_pl07_destroy_with_live(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    bool ok = true;
    void *keep[200];
    /* invariant: keep[0..k) live; every third one has been freed again */
    for (long k = 0; k < 200 && ok; k++) {
        keep[k] = pool_alloc(p);
        if (!keep[k]) { ok = false; break; }
        if (k % 3 == 0) pool_free(p, keep[k]);
        ok = check_stats(p, NODE_SIZE, "PL-07", k) && ok;
    }
    pool_destroy(p);
    pool_destroy(NULL);
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

/* ---- PL-09: pool_create's own allocation failing returns NULL ---- */
static bool test_pl09_create_fault(void) {
    fault_alloc_arm(1);
    rb_pool_t *p = pool_create(NODE_SIZE);
    fault_alloc_disarm();
    if (p != NULL) {
        fprintf(stderr, "    pool_create under fault_alloc_arm(1) returned non-NULL\n");
        pool_destroy(p);
        return false;
    }
    return true;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"PL-01", "fresh pool: (0,0,0), one rb_malloc", test_pl01_create},
    {"PL-02", "pool_create rejects impossible sizes", test_pl02_create_rejects},
    {"PL-03", "slots aligned and non-overlapping", test_pl03_alignment},
    {"PL-04", "random ops keep the invariant", test_pl04_random_ops},
    {"PL-05", "LIFO reuse without rb_malloc", test_pl05_lifo_reuse},
    {"PL-06", "slab-growth fault leaves pool unchanged", test_pl06_growth_fault},
    {"PL-07", "destroy with live objects; destroy(NULL)", test_pl07_destroy_with_live},
    {"PL-08", "exact stats at slab boundaries", test_pl08_slab_boundaries},
    {"PL-09", "pool_create allocation failure", test_pl09_create_fault},
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
