// Purpose: Slab pool for the pooled build (M5, Mutation 2). The slab is
// storage, the free list is policy. Each slab is one 4096-byte rb_malloc
// chunk with an in-band header; freed slots are chained through an
// intrusive free list stored in their own first bytes. Design:
// Mutation-2.md, Decisions 1-3. Evening 4 implements the geometry only.
#include "pool.h"

#include <stdint.h>

/* In-band header at the start of every slab; slots follow at geom.hdr. */
struct slab {
    struct slab *next;
};

/* What a dead slot holds: the free-list link, in its first bytes. */
struct free_slot {
    struct free_slot *next;
};

int pool_geometry(size_t obj_size, struct pool_geom *g) {
    const size_t align = POOL_ALIGN;
    const size_t hdr = (sizeof(struct slab) + align - 1) & ~(align - 1);
    if (obj_size == 0) return -1;
    /* a slot must also hold the free-list link once its object dies */
    size_t need = obj_size < sizeof(struct free_slot) ? sizeof(struct free_slot) : obj_size;
    if (need > SIZE_MAX - (align - 1)) return -1;
    size_t stride = (need + align - 1) & ~(align - 1);
    if (stride > POOL_SLAB_SIZE - hdr) return -1;

    g->stride = stride;
    g->hdr = hdr;
    g->objs_per_slab = (POOL_SLAB_SIZE - hdr) / stride;
    g->tail = POOL_SLAB_SIZE - hdr - g->objs_per_slab * stride;
    return 0;
}
