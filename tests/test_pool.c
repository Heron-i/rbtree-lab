// Purpose: The pool_stats oracle and the conservation invariant, end to end
// (M5, Mutation 2), built as build/test_pool. Per-function tests live in
// test_pool_geom.c, test_pool_create.c (create + destroy),
// test_pool_alloc.c and test_pool_free.c.
//   PL-04 random alloc/free ops keep the invariant  (unchanged)
//   PS-02 every NULL combination of pool_stats outputs
//   PS-03 the invariant across one script driving three pools at once
// After every step of every scenario, check_stats takes the numbers
// pool_stats reports and asserts
//     live + free_objs == slabs * objs_per_slab
// with objs_per_slab taken from pool_geometry, not from pool internals.
// The pool also asserts the same invariant internally in debug builds
// (POOL_CHECK in src/pool.c); this file checks it from the outside.
#include "pool.h"
#include "pool_check.h"   /* objs_per_slab_for, check_stats, expect_stats */
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the exact expected triples assume 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "PL/PS expected triples are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */

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

/* ---- PS-02: pool_stats fills exactly the outputs it is given, for all
 * 8 NULL/non-NULL combinations ---- */
static bool test_ps02_null_outputs(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);
    void *objs[86] = {0};
    bool ok = true;
    /* invariant: objs[0..k) live */
    for (size_t k = 0; k < per + 1 && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { ok = false; break; }
    }
    pool_free(p, objs[0]);
    /* known state: 2 slabs, 85 live, 85 free */
    ok = ok && expect_stats(p, NODE_SIZE, "PS-02 known state", 2, per, per);
    const size_t want[3] = {2, per, per};
    /* invariant: every mask < m filled exactly its non-NULL outputs */
    for (unsigned m = 0; m < 8 && ok; m++) {
        size_t got[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
        pool_stats(p, (m & 1) ? &got[0] : NULL, (m & 2) ? &got[1] : NULL,
                   (m & 4) ? &got[2] : NULL);
        /* invariant: outputs < i match want if requested, else stay untouched */
        for (unsigned i = 0; i < 3; i++) {
            size_t expect = (m & (1u << i)) ? want[i] : SIZE_MAX;
            if (got[i] != expect) {
                fprintf(stderr, "    mask %u output %u: got %zu, want %zu\n", m, i, got[i], expect);
                ok = false;
            }
        }
    }
    ok = check_stats(p, NODE_SIZE, "PS-02 end", 0) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PS-03: one deterministic script drives three pools of different
 * geometry; the invariant holds on every pool after every op ---- */
static bool test_ps03_three_pools(void) {
    enum { NOPS = 3000, CAP = 400, NPOOLS = 3 };
    static const size_t sizes[NPOOLS] = {1, NODE_SIZE, 100};
    rb_pool_t *pools[NPOOLS] = {0};
    static void *live[NPOOLS][CAP];
    size_t nlive[NPOOLS] = {0};
    bool ok = true;
    /* invariant: pools[0..i) were created */
    for (int i = 0; i < NPOOLS; i++) {
        pools[i] = pool_create(sizes[i]);
        if (!pools[i]) ok = false;
    }
    unsigned seed = 777u;
    /* invariant: live[i][0..nlive[i]) are exactly pool i's outstanding objects */
    for (long op = 0; op < NOPS && ok; op++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 16) % NPOOLS);
        bool do_alloc = nlive[i] == 0 || (nlive[i] < CAP && (seed >> 8) % 3 != 0);
        if (do_alloc) {
            void *o = pool_alloc(pools[i]);
            if (!o) { fprintf(stderr, "    PS-03 op %ld: pool_alloc returned NULL\n", op); ok = false; break; }
            live[i][nlive[i]++] = o;
        } else {
            size_t victim = (seed >> 4) % nlive[i];
            pool_free(pools[i], live[i][victim]);
            live[i][victim] = live[i][--nlive[i]];
        }
        /* invariant: pools [0..j) satisfy the invariant and match the model */
        for (int j = 0; j < NPOOLS; j++) {
            size_t plive;
            pool_stats(pools[j], NULL, &plive, NULL);
            if (plive != nlive[j]) {
                fprintf(stderr, "    PS-03 op %ld pool %d: live=%zu, model %zu\n", op, j, plive, nlive[j]);
                ok = false;
            }
            ok = check_stats(pools[j], sizes[j], "PS-03", op) && ok;
        }
    }
    /* invariant: pools[0..i) destroyed */
    for (int i = 0; i < NPOOLS; i++) pool_destroy(pools[i]);
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"PL-04", "random ops keep the invariant", test_pl04_random_ops},
    {"PS-02", "pool_stats with every NULL output combination", test_ps02_null_outputs},
    {"PS-03", "invariant across three pools at once", test_ps03_three_pools},
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
