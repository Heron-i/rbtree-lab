#ifndef POOL_H
#define POOL_H

/* Slab pool for the pooled build (Mutation 2). Internal: shared only
 * between src/pool.c, src/rbtree.c and the white-box tests. Not part of
 * the graded public contract in include/rbtree.h. */

#include <stddef.h>

/* Reminder: Pooled tree does not mean 'the tree performs no ordinary allocations.' */

typedef struct rb_pool rb_pool_t;
/* Constraint: slabs are 4096 bytes (2^12 bytes) chunks obtained w/ rb_malloc */
rb_pool_t *pool_create(size_t obj_size);
/* O(1), from a free list */
void *pool_alloc(rb_pool_t *p);
/* O(1), returns to the free list -> freed objects are chained into the intrusive free list inside the dead objects themselves */
void pool_free(rb_pool_t *p, void *obj);
void pool_stats(const rb_pool_t *p,
size_t *slabs, size_t *live, size_t *free_objs);
/* releases all slabs -> destroying a pooled tree can present issues when it comes to the key copies,
 * and (when owned) the values still ahve to be released one at a time,
 * Which means teardown MUST VISIT EVERY NODE. */
void pool_destroy(rb_pool_t *p);

/* ---- Slab geometry (Mutation-2.md, Decisions 1 and 2) ---- */
#define POOL_SLAB_SIZE ((size_t)4096)
/* every slot is aligned like malloc's result, so it fits any object type
 * and, once the object dies, the intrusive free-list pointer */
#define POOL_ALIGN alignof(max_align_t)

/* Slot i of a slab lives at (char *)slab + hdr + i * stride. */
struct pool_geom {
    size_t stride;          /* multiple of POOL_ALIGN, >= max(obj_size, sizeof(void *)) */
    size_t hdr;             /* offset of slot 0: in-band header rounded up to POOL_ALIGN */
    size_t objs_per_slab;   /* (POOL_SLAB_SIZE - hdr) / stride, always >= 1 */
    size_t tail;            /* bytes left after the last slot, always < stride */
};

/* Returns 0 and fills *g, or -1 with *g untouched if obj_size is 0,
 * overflows, or leaves no room for a single slot. */
int pool_geometry(size_t obj_size, struct pool_geom *g);

#endif
