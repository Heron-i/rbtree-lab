#ifndef POOL_CHECK_H
#define POOL_CHECK_H

/* Shared oracle for the pool test files: pool_stats is checked against
 *     live + free_objs == slabs * objs_per_slab
 * with objs_per_slab taken from pool_geometry, not from pool internals. */

#include "pool.h"

#include <stdbool.h>
#include <stdio.h>

static inline size_t objs_per_slab_for(size_t obj_size) {
    struct pool_geom g;
    return pool_geometry(obj_size, &g) == 0 ? g.objs_per_slab : 0;
}

/* Reads the pool's own numbers and checks the conservation invariant. */
static inline bool check_stats(const rb_pool_t *p, size_t obj_size, const char *where, long step) {
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
static inline bool expect_stats(const rb_pool_t *p, size_t obj_size, const char *where,
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

#endif
