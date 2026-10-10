// Purpose: Slab pool for the pooled build (M5, Mutation 2). The slab is
// storage, the free list is policy. Each slab is one 4096-byte rb_malloc
// chunk with an in-band header; freed slots are chained through an
// intrusive free list stored in their own first bytes; slots are carved
// lazily from the newest slab. Design: Mutation-2.md, Decisions 1-3.
#include "pool.h"
#include "fault_alloc.h"   /* rb_malloc/rb_free: the only allocation seam */

#include <stdint.h>

/* In-band header at the start of every slab; slots follow at geom.hdr. */
struct slab {
    struct slab *next;
};

/* What a dead slot holds: the free-list link, in its first bytes. */
struct free_slot {
    struct free_slot *next;
};

struct rb_pool {
    struct pool_geom geom;
    struct slab *slabs;           /* chain of every slab, newest first */
    struct free_slot *free_list;  /* LIFO of freed slots */
    char *bump;                   /* next uncarved slot in the newest slab */
    size_t uncarved;              /* slots left at bump, in the newest slab */
    size_t nslabs, nlive, nfree_list;
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

#ifndef NDEBUG
#include <assert.h>
/* Debug-build check of the pool's bookkeeping, run after every operation:
 * every slot of every slab is exactly one of live, free-listed, uncarved. */
static void pool_check(const rb_pool_t *p) {
    const struct pool_geom *g = &p->geom;
    assert(p->nlive + p->nfree_list + p->uncarved == p->nslabs * g->objs_per_slab);
    size_t n = 0;
    /* invariant: n == free-list links followed so far, never more than recorded */
    for (const struct free_slot *f = p->free_list; f != NULL; f = f->next) {
        n++;
        assert(n <= p->nfree_list);
    }
    assert(n == p->nfree_list);
    if (p->uncarved > 0) {
        assert(p->slabs != NULL);
        size_t off = (size_t)(p->bump - (char *)p->slabs);
        assert(off >= g->hdr && (off - g->hdr) % g->stride == 0);
        assert((off - g->hdr) / g->stride + p->uncarved == g->objs_per_slab);
    }
}
#define POOL_CHECK(p) pool_check(p)
#else
#define POOL_CHECK(p) ((void)0)
#endif

rb_pool_t *pool_create(size_t obj_size) {
    struct pool_geom g;
    if (pool_geometry(obj_size, &g) != 0) return NULL;
    rb_pool_t *p = rb_malloc(sizeof *p);
    if (p == NULL) return NULL;

    /* lazy: the first slab is allocated by the first pool_alloc */
    p->geom = g;
    p->slabs = NULL;
    p->free_list = NULL;
    p->bump = NULL;
    p->uncarved = 0;
    p->nslabs = p->nlive = p->nfree_list = 0;
    POOL_CHECK(p);
    return p;
}

/* Free list first, then carve, then grow by one slab. Growth is the only
 * fallible step, and the pool is touched only after it succeeds. */
void *pool_alloc(rb_pool_t *p) {
    void *obj = NULL;
    if (p->free_list != NULL) {
        struct free_slot *f = p->free_list;
        p->free_list = f->next;
        p->nfree_list--;
        obj = f;
    } else {
        if (p->uncarved == 0) {
            struct slab *s = rb_malloc(POOL_SLAB_SIZE);
            if (s == NULL) goto out;
            s->next = p->slabs;
            p->slabs = s;
            p->nslabs++;
            p->bump = (char *)s + p->geom.hdr;
            p->uncarved = p->geom.objs_per_slab;
        }
        obj = p->bump;
        p->bump += p->geom.stride;
        p->uncarved--;
    }
    p->nlive++;
out:
    POOL_CHECK(p);
    return obj;
}

void pool_free(rb_pool_t *p, void *obj) {
    if (obj == NULL) return;
    struct free_slot *f = obj;
    f->next = p->free_list;
    p->free_list = f;
    p->nlive--;
    p->nfree_list++;
    POOL_CHECK(p);
}

void pool_stats(const rb_pool_t *p,
                size_t *slabs, size_t *live, size_t *free_objs) {
    if (slabs != NULL) *slabs = p->nslabs;
    if (live != NULL) *live = p->nlive;
    if (free_objs != NULL) *free_objs = p->nfree_list + p->uncarved;
}

/* Type-blind and wholesale: live objects go with their slab. The caller
 * must already have released whatever those objects own. */
void pool_destroy(rb_pool_t *p) {
    if (p == NULL) return;
    struct slab *s = p->slabs;
    /* invariant: every slab before s in the chain has been freed */
    while (s != NULL) {
        struct slab *next = s->next;
        rb_free(s);
        s = next;
    }
    rb_free(p);
}
