#ifndef POOLED_BUILD_H
#define POOLED_BUILD_H

/* Force-included (-include) into the TEST translation unit of each
 * build/pooled/ binary, never into src/: the whole Assignment 1 +
 * Mutation 1 battery then runs, source unchanged, on pooled trees. */
#define RB_POOLED_BUILD 1
#define rb_create(vf) rb_create_pooled(vf)   /* every battery tree is pooled */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Declared N/A list: unpooled tests whose assertion or fixture depends on
 * how rb_create allocates nodes. They do not test pooling, and they remain
 * correct for the plain tree; this build swaps the allocator underneath
 * them, so here their result would say nothing about the tree. Each was run
 * unchanged on the pooled build and diagnosed before being listed
 * (Mutation-2.md, Part 5). The unpooled build still runs every one of them. */
struct pooled_na {
    const char *file;         /* __FILE__ of the test TU, as the Makefile passes it */
    const char *id;
    const char *reason;
    const char *covered_by;   /* where the same concern is tested on pooled trees */
};

static const struct pooled_na pooled_na_list[] = {
    {"tests/test_create.c", "RBC-14",
     "checks rb_create's own result (no pool); this build maps rb_create to rb_create_pooled",
     "RBC-14 in the unpooled build, which checks both rb_create and rb_create_pooled"},
    {"tests/test_insert.c", "RBI-15",
     "assumes rb_create's node-first allocation order; a pooled insert with free slots makes 1 allocation",
     "RBI-15P (key copy succeeds, slab fails)"},
    {"tests/test_delete.c", "RBD-10",
     "its fixture builds nodes with plain malloc, which a pooled tree never frees",
     "its unpooled run: delete_fixup is the same code in both builds"},
    {"tests/test_find.c", "RBF-02",
     "its fixture builds the node with plain malloc, which a pooled tree never frees",
     "RBI-13/14, the pooled sweep's model_matches, pooled fuzz finds"},
    {"tests/test_find.c", "RBF-03",
     "its fixture builds the node with plain malloc, which a pooled tree never frees",
     "RBI-13/14, the pooled sweep's model_matches, pooled fuzz finds"},
    {"tests/test_size.c", "RBS-01",
     "its fixture builds the node with plain malloc, which a pooled tree never frees",
     "RBI-17P, the pooled fuzz size check after every op"},
};

#define POOLED_NA_COUNT (sizeof pooled_na_list / sizeof pooled_na_list[0])

/* If (file, id) is declared N/A, prints why and returns true. */
static inline bool pooled_na_report(const char *file, const char *id) {
    /* invariant: no entry before k matches (file, id) */
    for (size_t k = 0; k < POOLED_NA_COUNT; k++) {
        const struct pooled_na *e = &pooled_na_list[k];
        if (strcmp(e->file, file) == 0 && strcmp(e->id, id) == 0) {
            printf("[%s] N/A on pooled build: %s (covered by %s)\n", e->id, e->reason,
                   e->covered_by);
            return true;
        }
    }
    return false;
}

/* Every entry for `file` must name an ID in the file's own test table, so a
 * stale or misspelled entry fails the run instead of hiding nothing. The
 * table is walked generically: n entries of `stride` bytes, each holding a
 * const char *id at `id_offset`. */
static inline bool pooled_na_verify(const char *file, const void *table, size_t n,
                                    size_t stride, size_t id_offset) {
    bool ok = true;
    /* invariant: every matching entry before k was found in the table */
    for (size_t k = 0; k < POOLED_NA_COUNT; k++) {
        if (strcmp(pooled_na_list[k].file, file) != 0) continue;
        bool found = false;
        /* invariant: no table row before i has this entry's id */
        for (size_t i = 0; i < n && !found; i++) {
            const char *id;
            memcpy(&id, (const char *)table + i * stride + id_offset, sizeof id);
            found = strcmp(id, pooled_na_list[k].id) == 0;
        }
        if (!found) {
            fprintf(stderr, "pooled N/A list names %s, which %s does not define\n",
                    pooled_na_list[k].id, file);
            ok = false;
        }
    }
    return ok;
}

#define POOLED_NA_VERIFY(tests, total) \
    pooled_na_verify(__FILE__, (tests), (total), sizeof (tests)[0], offsetof(test_case_t, id))

#endif
