// Purpose: Unit tests for pool_create and pool_destroy (M5, Mutation 2),
// built as build/test_pool_create. Destroy is merged in here: its
// correctness shows up only as leaks under asan/memcheck, and every create
// test already ends in a destroy.
//   PL-01 fresh pool: (0,0,0), one rb_malloc        (moved from test_pool.c)
//   PL-02 impossible sizes rejected, no allocation  (moved)
//   PL-09 pool_create's own allocation failing      (moved)
//   PC-05 two pools are independent
//   PL-07 destroy with live objects; destroy(NULL)  (moved)
//   PD-02 destroy a fresh pool that holds no slab
//   PD-03 destroy after every object was freed
// After every step of every scenario, check_stats asserts
//     live + free_objs == slabs * objs_per_slab
#include "pool.h"
#include "pool_check.h"   /* objs_per_slab_for, check_stats, expect_stats */
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the exact expected triples assume 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "PL/PC/PD expected triples are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */

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

/* ---- PC-05: two pools never share slabs, free lists or counts; one
 * keeps working after the other is destroyed ---- */
static bool test_pc05_independent_pools(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    rb_pool_t *q = pool_create(100);
    if (!p || !q) {
        pool_destroy(p);
        pool_destroy(q);
        return false;
    }
    size_t per_p = objs_per_slab_for(NODE_SIZE);   /* 85 */
    size_t per_q = objs_per_slab_for(100);         /* 36 */
    bool ok = true;
    /* invariant: k objects allocated from each pool, alternating p then q */
    for (size_t k = 0; k < 40 && ok; k++) {
        if (!pool_alloc(p) || !pool_alloc(q)) {
            fprintf(stderr, "    PC-05 alloc %zu failed\n", k);
            ok = false;
            break;
        }
        ok = check_stats(p, NODE_SIZE, "PC-05 p", (long)k) && ok;
        ok = check_stats(q, 100, "PC-05 q", (long)k) && ok;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PC-05 p after 40", 1, 40, per_p - 40);
    ok = ok && expect_stats(q, 100, "PC-05 q after 40", 2, 40, 2 * per_q - 40);
    pool_destroy(q);   /* q's live objects go with its slabs */

    void *o = pool_alloc(p);
    ok = o && expect_stats(p, NODE_SIZE, "PC-05 p alloc after q destroyed", 1, 41, per_p - 41) && ok;
    pool_free(p, o);
    ok = expect_stats(p, NODE_SIZE, "PC-05 p free after q destroyed", 1, 40, per_p - 40) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PD-02: destroying a pool that never grew a slab frees only the
 * pool itself (leaks are judged by asan/memcheck) ---- */
static bool test_pd02_destroy_fresh(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    bool ok = expect_stats(p, NODE_SIZE, "PD-02 fresh", 0, 0, 0);
    long a0 = fault_alloc_total();
    pool_destroy(p);
    if (fault_alloc_total() != a0) {
        fprintf(stderr, "    pool_destroy made %ld rb_malloc calls\n", fault_alloc_total() - a0);
        ok = false;
    }
    return ok;
}

/* ---- PD-03: destroy after every object was freed: all slabs are still
 * held (the pool never shrinks) and all of them are released ---- */
static bool test_pd03_destroy_all_freed(void) {
    enum { N = 200 };
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);
    void *objs[N];
    bool ok = true;
    /* invariant: objs[0..k) live */
    for (size_t k = 0; k < N && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { fprintf(stderr, "    PD-03 alloc %zu failed\n", k); ok = false; break; }
        ok = check_stats(p, NODE_SIZE, "PD-03 alloc", (long)k) && ok;
    }
    /* invariant: objs[0..k) have been freed */
    for (size_t k = 0; k < N && ok; k++) {
        pool_free(p, objs[k]);
        ok = check_stats(p, NODE_SIZE, "PD-03 free", (long)k) && ok;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PD-03 all freed", 3, 0, 3 * per);
    pool_destroy(p);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"PL-01", "fresh pool: (0,0,0), one rb_malloc", test_pl01_create},
    {"PL-02", "pool_create rejects impossible sizes", test_pl02_create_rejects},
    {"PL-09", "pool_create allocation failure", test_pl09_create_fault},
    {"PC-05", "two pools are independent", test_pc05_independent_pools},
    {"PL-07", "destroy with live objects; destroy(NULL)", test_pl07_destroy_with_live},
    {"PD-02", "destroy a fresh pool with no slab", test_pd02_destroy_fresh},
    {"PD-03", "destroy after every object was freed", test_pd03_destroy_all_freed},
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
