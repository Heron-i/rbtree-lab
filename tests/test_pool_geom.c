// Purpose: Unit tests for the slab pool's geometry (M5, Mutation 2,
// evening 4). GEO-01..GEO-03 check pool_geometry's arithmetic directly: a
// hand-computed table for this platform, properties for every obj_size
// that fits, and rejection of impossible sizes. The pool's stats-invariant
// tests (PL-01..PL-09) live in tests/test_pool.c and join the build when
// the rest of the pool is implemented.
#include "pool.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the hand-computed GEO-01 table assumes 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "GEO-01 expected values are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */

/* ---- GEO-01: hand-computed geometry for representative sizes ---- */
static bool test_geo01_table(void) {
    static const struct {
        size_t obj_size;
        struct pool_geom want;
    } rows[] = {
        {1,         {16,   16, 255, 0}},
        {8,         {16,   16, 255, 0}},
        {24,        {32,   16, 127, 16}},
        {NODE_SIZE, {48,   16, 85,  0}},
        {100,       {112,  16, 36,  48}},
        {4080,      {4080, 16, 1,   0}},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        struct pool_geom g = {0};
        const struct pool_geom *w = &rows[i].want;
        int rc = pool_geometry(rows[i].obj_size, &g);
        if (rc != 0 || g.stride != w->stride || g.hdr != w->hdr ||
            g.objs_per_slab != w->objs_per_slab || g.tail != w->tail) {
            fprintf(stderr, "    pool_geometry(%zu): rc=%d stride=%zu hdr=%zu objs=%zu tail=%zu, "
                    "want rc=0 stride=%zu hdr=%zu objs=%zu tail=%zu\n",
                    rows[i].obj_size, rc, g.stride, g.hdr, g.objs_per_slab, g.tail,
                    w->stride, w->hdr, w->objs_per_slab, w->tail);
            ok = false;
        }
    }
    return ok;
}

/* ---- GEO-02: properties for every obj_size that fits in one slab ---- */
static bool test_geo02_properties(void) {
    bool ok = true;
    /* invariant: every obj_size < s has already passed every property */
    for (size_t s = 1; s <= POOL_SLAB_SIZE - 16; s++) {
        struct pool_geom g;
        if (pool_geometry(s, &g) != 0) {
            fprintf(stderr, "    pool_geometry(%zu) rejected a size that fits\n", s);
            ok = false;
            continue;
        }
        bool good = g.stride % POOL_ALIGN == 0 && g.stride >= s &&
                    g.stride >= sizeof(void *) && g.hdr % POOL_ALIGN == 0 &&
                    g.hdr >= sizeof(void *) && g.objs_per_slab >= 1 &&
                    g.hdr + g.objs_per_slab * g.stride + g.tail == POOL_SLAB_SIZE &&
                    g.tail < g.stride;
        if (!good) {
            fprintf(stderr, "    pool_geometry(%zu): stride=%zu hdr=%zu objs=%zu tail=%zu "
                    "violates a property\n", s, g.stride, g.hdr, g.objs_per_slab, g.tail);
            ok = false;
        }
    }
    return ok;
}

/* ---- GEO-03: impossible sizes are rejected and *g is left untouched ---- */
static bool test_geo03_rejects(void) {
    static const size_t bad[] = {0, POOL_SLAB_SIZE - 15, POOL_SLAB_SIZE, SIZE_MAX,
                                 SIZE_MAX - 7};
    bool ok = true;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct pool_geom g = {7, 7, 7, 7};
        int rc = pool_geometry(bad[i], &g);
        if (rc != -1 || g.stride != 7 || g.hdr != 7 || g.objs_per_slab != 7 || g.tail != 7) {
            fprintf(stderr, "    pool_geometry(%zu): rc=%d (want -1) or *g was modified\n",
                    bad[i], rc);
            ok = false;
        }
    }
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"GEO-01", "geometry matches hand-computed table", test_geo01_table},
    {"GEO-02", "geometry properties for every size 1..4080", test_geo02_properties},
    {"GEO-03", "impossible sizes rejected, output untouched", test_geo03_rejects},
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
