// Purpose: Unit tests for pool_free and the debug poisoning of dead slots
// (M5, Mutation 2, Evening 5), built as build/test_pool_free.
//   PF-01 pool_free(p, NULL) is a no-op
//   PF-02 freeing a whole slab never shrinks the pool
//   PF-03 reverse / forward / random frees come back as exactly the freed set
//   PF-04 a freed slot holds the raw link in its first bytes, poison after
//   PF-05 a slot popped from the free list is all poison, link bytes included
//   PF-06 poisoning a slot never touches its neighbours
// After every step of every scenario, check_stats asserts
//     live + free_objs == slabs * objs_per_slab
// Poison layout (debug builds): pool_free fills the whole stride with
// POOL_POISON_BYTE, then writes the free-list link over the first
// sizeof(void *) bytes; pool_alloc re-poisons the link before handing out.
#include "pool.h"
#include "pool_check.h"
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "test_pool_free checks debug-build poisoning; build without NDEBUG"
#endif

/* the exact expected triples assume 16-byte max alignment, 8-byte pointers */
static_assert(alignof(max_align_t) == 16 && sizeof(void *) == 8,
              "PF expected triples are computed for x86-64");

#define NODE_SIZE ((size_t)48)   /* sizeof(struct rb_node) on this platform */
#define LINK sizeof(void *)      /* bytes the free-list link owns in a dead slot */

static const size_t poison_sizes[] = {1, NODE_SIZE, 100};
#define NSIZES (sizeof poison_sizes / sizeof poison_sizes[0])

static size_t stride_for(size_t obj_size) {
    struct pool_geom g;
    return pool_geometry(obj_size, &g) == 0 ? g.stride : 0;
}

/* The link stored in a dead slot, read without aliasing the slot's type. */
static void *link_of(const void *slot) {
    void *next;
    memcpy(&next, slot, sizeof next);
    return next;
}

/* Index of the first byte in [from, to) that is not poison, or `to`. */
static size_t first_unpoisoned(const void *slot, size_t from, size_t to) {
    const unsigned char *b = slot;
    size_t i = from;
    /* invariant: b[from..i) are all POOL_POISON_BYTE */
    while (i < to && b[i] == POOL_POISON_BYTE) i++;
    return i;
}

/* A per-slot fill byte that is never the poison byte. */
static int pattern(size_t k) {
    return (int)(0x10 + k % 0x80);
}

/* Index of the first byte of slot[0..n) that is not `byte`, or n. */
static size_t first_mismatch(const void *slot, int byte, size_t n) {
    const unsigned char *b = slot;
    size_t i = 0;
    /* invariant: b[0..i) all equal byte */
    while (i < n && b[i] == (unsigned char)byte) i++;
    return i;
}

/* ---- PF-01: pool_free(p, NULL) changes nothing ---- */
static bool test_pf01_free_null(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    void *a = pool_alloc(p);
    bool ok = a && expect_stats(p, NODE_SIZE, "PF-01 after alloc", 1, 1, 84);
    long a0 = fault_alloc_total();
    pool_free(p, NULL);
    ok = expect_stats(p, NODE_SIZE, "PF-01 after free(NULL)", 1, 1, 84) && ok;
    if (fault_alloc_total() != a0) {
        fprintf(stderr, "    pool_free(NULL) made %ld allocations\n", fault_alloc_total() - a0);
        ok = false;
    }
    pool_free(p, a);
    ok = expect_stats(p, NODE_SIZE, "PF-01 after free(a)", 1, 0, 85) && ok;
    pool_destroy(p);
    return ok;
}

/* ---- PF-02: emptying a slab keeps the slab; free_objs grows by one per free ---- */
static bool test_pf02_never_shrinks(void) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);
    void *objs[85];
    bool ok = true;
    /* invariant: objs[0..k) live, all from slab 1 */
    for (size_t k = 0; k < per && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { fprintf(stderr, "    alloc %zu failed\n", k); ok = false; break; }
        ok = check_stats(p, NODE_SIZE, "PF-02 alloc", (long)k) && ok;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PF-02 slab full", 1, per, 0);
    /* invariant: objs[0..k) freed; the pool still holds exactly one slab */
    for (size_t k = 0; k < per && ok; k++) {
        pool_free(p, objs[k]);
        char where[48];
        snprintf(where, sizeof where, "PF-02 after free %zu", k + 1);
        ok = expect_stats(p, NODE_SIZE, where, 1, per - k - 1, k + 1) && ok;
    }
    pool_destroy(p);
    return ok;
}

/* ---- PF-03: whatever the free order, re-allocation hands back exactly
 * the freed set, each pointer once, without rb_malloc ---- */
enum { PF03_REVERSE, PF03_FORWARD, PF03_RANDOM, PF03_NORDERS };
static const char *const pf03_order_name[] = {"reverse", "forward", "random"};

static bool pf03_one_order(int order) {
    rb_pool_t *p = pool_create(NODE_SIZE);
    if (!p) return false;
    size_t per = objs_per_slab_for(NODE_SIZE);
    enum { N = 2 * 85 + 5 };     /* crosses into a third slab */
    void *objs[N], *back[N];
    size_t idx[N];
    bool ok = true;
    /* invariant: objs[0..k) live */
    for (size_t k = 0; k < N && ok; k++) {
        objs[k] = pool_alloc(p);
        if (!objs[k]) { fprintf(stderr, "    alloc %zu failed\n", k); ok = false; break; }
        ok = check_stats(p, NODE_SIZE, "PF-03 alloc", (long)k) && ok;
    }
    /* invariant: idx[0..k) is the chosen free order so far */
    for (size_t k = 0; k < N; k++) {
        idx[k] = order == PF03_REVERSE ? N - 1 - k : k;
    }
    if (order == PF03_RANDOM) {
        unsigned seed = 2024u;
        /* invariant: idx[k+1..N) are final; idx[0..k] holds the rest, unshuffled */
        for (size_t k = N - 1; k > 0; k--) {
            seed = seed * 1103515245u + 12345u;
            size_t j = (seed >> 16) % (k + 1);
            size_t t = idx[k]; idx[k] = idx[j]; idx[j] = t;
        }
    }
    /* invariant: objs[idx[0..k)] have been freed */
    for (size_t k = 0; k < N && ok; k++) {
        pool_free(p, objs[idx[k]]);
        ok = check_stats(p, NODE_SIZE, "PF-03 free", (long)k) && ok;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PF-03 all freed", 3, 0, 3 * per);
    long a0 = fault_alloc_total();
    /* invariant: back[0..k) are distinct members of objs[] */
    for (size_t k = 0; k < N && ok; k++) {
        back[k] = pool_alloc(p);
        bool member = false, dup = false;
        /* invariant: member/dup reflect objs[0..j) and back[0..j) */
        for (size_t j = 0; j < N; j++) {
            if (objs[j] == back[k]) member = true;
            if (j < k && back[j] == back[k]) dup = true;
        }
        if (!member || dup) {
            fprintf(stderr, "    %s: re-alloc %zu returned %p (%s)\n", pf03_order_name[order],
                    k, back[k], !member ? "never freed" : "handed out twice");
            ok = false;
        }
        ok = check_stats(p, NODE_SIZE, "PF-03 re-alloc", (long)k) && ok;
    }
    if (ok && fault_alloc_total() != a0) {
        fprintf(stderr, "    %s: re-allocating the freed set made %ld rb_malloc calls, want 0\n",
                pf03_order_name[order], fault_alloc_total() - a0);
        ok = false;
    }
    ok = ok && expect_stats(p, NODE_SIZE, "PF-03 end", 3, N, 3 * per - N);
    pool_destroy(p);
    return ok;
}

static bool test_pf03_free_orders(void) {
    bool ok = true;
    for (int order = 0; order < PF03_NORDERS; order++) {
        ok = pf03_one_order(order) && ok;
    }
    return ok;
}

/* ---- PF-04: a freed slot is poison except for the raw link at the front:
 * b (freed second) links to a; a is the tail and links to NULL ---- */
static bool test_pf04_freed_slot_layout(void) {
    bool ok = true;
    for (size_t i = 0; i < NSIZES; i++) {
        size_t s = poison_sizes[i], stride = stride_for(s);
        rb_pool_t *p = pool_create(s);
        if (!p) return false;
        void *a = pool_alloc(p), *b = pool_alloc(p);
        if (!a || !b) { pool_destroy(p); return false; }
        memset(a, pattern(1), s);
        memset(b, pattern(2), s);
        ok = check_stats(p, s, "PF-04 allocs", 0) && ok;

        pool_free(p, a);
        ok = check_stats(p, s, "PF-04 free a", 1) && ok;
        pool_free(p, b);
        ok = check_stats(p, s, "PF-04 free b", 2) && ok;

        if (link_of(b) != a) {
            fprintf(stderr, "    obj_size %zu: b's link is %p, want a = %p\n", s, link_of(b), a);
            ok = false;
        }
        if (link_of(a) != NULL) {
            fprintf(stderr, "    obj_size %zu: tail a's link is %p, want NULL\n", s, link_of(a));
            ok = false;
        }
        size_t bad_a = first_unpoisoned(a, LINK, stride);
        size_t bad_b = first_unpoisoned(b, LINK, stride);
        if (bad_a != stride || bad_b != stride) {
            fprintf(stderr, "    obj_size %zu (stride %zu): freed slot not poisoned past the link "
                    "(a: first live byte %zu, b: %zu)\n", s, stride, bad_a, bad_b);
            ok = false;
        }
        pool_destroy(p);
    }
    return ok;
}

/* ---- PF-05: a slot popped from the free list comes back all poison,
 * link bytes included, so stale contents never leak to the new owner ---- */
static bool test_pf05_popped_slot_poisoned(void) {
    bool ok = true;
    for (size_t i = 0; i < NSIZES; i++) {
        size_t s = poison_sizes[i], stride = stride_for(s);
        rb_pool_t *p = pool_create(s);
        if (!p) return false;
        void *a = pool_alloc(p), *b = pool_alloc(p);
        if (!a || !b) { pool_destroy(p); return false; }
        memset(a, pattern(1), s);
        memset(b, pattern(2), s);
        pool_free(p, a);
        pool_free(p, b);
        ok = check_stats(p, s, "PF-05 freed", 0) && ok;

        void *first = pool_alloc(p);    /* LIFO: b, whose link pointed at a */
        ok = check_stats(p, s, "PF-05 pop 1", 1) && ok;
        void *second = pool_alloc(p);   /* a, the tail */
        ok = check_stats(p, s, "PF-05 pop 2", 2) && ok;
        if (first != b || second != a) {
            fprintf(stderr, "    obj_size %zu: pops returned %p, %p; want b = %p, a = %p\n",
                    s, first, second, b, a);
            ok = false;
        } else {
            size_t bad_b = first_unpoisoned(b, 0, stride);
            size_t bad_a = first_unpoisoned(a, 0, stride);
            if (bad_b != stride || bad_a != stride) {
                fprintf(stderr, "    obj_size %zu (stride %zu): popped slot not all poison "
                        "(b: first live byte %zu, a: %zu)\n", s, stride, bad_b, bad_a);
                ok = false;
            }
        }
        pool_destroy(p);
    }
    return ok;
}

/* ---- PF-06: poisoning stays inside the freed slot. ASan sees a slab as
 * one block, so an overrun into a neighbour would be invisible to it.
 * Includes the last slot of a slab that has a tail (obj_size 100). ---- */
static bool pf06_neighbours_intact(void **objs, size_t s, size_t freed, size_t lo, size_t hi) {
    bool ok = true;
    /* invariant: every neighbour in [lo, k) still holds its own pattern */
    for (size_t k = lo; k <= hi; k++) {
        if (k == freed) continue;
        size_t bad = first_mismatch(objs[k], pattern(k), s);
        if (bad != s) {
            fprintf(stderr, "    obj_size %zu: freeing slot %zu changed neighbour %zu at byte %zu\n",
                    s, freed, k, bad);
            ok = false;
        }
    }
    return ok;
}

static bool test_pf06_neighbours_untouched(void) {
    static const size_t sizes[] = {NODE_SIZE, 100};
    bool ok = true;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t s = sizes[i], stride = stride_for(s), per = objs_per_slab_for(s);
        rb_pool_t *p = pool_create(s);
        void **objs = malloc(per * sizeof *objs);
        if (!p || !objs) { pool_destroy(p); free(objs); return false; }
        /* invariant: objs[0..k) fill slab 1 in carve order, each with its own pattern */
        for (size_t k = 0; k < per && ok; k++) {
            objs[k] = pool_alloc(p);
            if (!objs[k] || (k > 0 && (char *)objs[k] != (char *)objs[k - 1] + stride)) {
                fprintf(stderr, "    obj_size %zu: slot %zu is not adjacent to slot %zu\n",
                        s, k, k - 1);
                ok = false;
                break;
            }
            memset(objs[k], pattern(k), s);
            ok = check_stats(p, s, "PF-06 alloc", (long)k) && ok;
        }
        if (ok) {
            size_t mid = per / 2, last = per - 1;
            pool_free(p, objs[mid]);
            ok = check_stats(p, s, "PF-06 free mid", 0) && ok;
            ok = pf06_neighbours_intact(objs, s, mid, mid - 1, mid + 1) && ok;
            pool_free(p, objs[last]);   /* the slab's tail bytes follow this slot */
            ok = check_stats(p, s, "PF-06 free last", 1) && ok;
            ok = pf06_neighbours_intact(objs, s, last, last - 1, last) && ok;
        }
        pool_destroy(p);
        free(objs);
    }
    return ok;
}

typedef struct {
    const char *id;
    const char *name;
    bool (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
    {"PF-01", "pool_free(NULL) is a no-op", test_pf01_free_null},
    {"PF-02", "emptying a slab never shrinks the pool", test_pf02_never_shrinks},
    {"PF-03", "any free order returns exactly the freed set", test_pf03_free_orders},
    {"PF-04", "freed slot: raw link, then poison", test_pf04_freed_slot_layout},
    {"PF-05", "popped slot is all poison", test_pf05_popped_slot_poisoned},
    {"PF-06", "poison never touches neighbouring slots", test_pf06_neighbours_untouched},
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
