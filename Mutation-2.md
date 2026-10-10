# M5 Mutation 2: the slab is storage, the free list is policy

> The design (Decisions 1-3, the answers and steps 0-5) is **approved**. On exiting plan mode this file is saved as `Mutation-2.md` in the repo root. Tonight's implementation scope is the "Evening 4 scope" section near the end.

## Context
`include/rbtree.h` declares `rb_create_pooled`, but nothing implements it yet. Today [src/pool.c](src/pool.c) holds only the API: `pool_create / pool_alloc / pool_free / pool_stats / pool_destroy`. Its comments add these constraints:
- Slabs are **4096-byte chunks obtained with `rb_malloc`**.
- `pool_alloc` and `pool_free` are **O(1)**.
- Freed objects are chained through an **intrusive free list stored inside the dead objects themselves**.
- `pool_destroy` releases all slabs. Teardown must still **visit every node**, because key copies and owned values are ordinary allocations.
- "Pooled" does **not** mean the tree stops making ordinary allocations. `struct rbtree`, `rb_pool_t` and every key copy still go through `rb_malloc`.

The API leaves three things open: alignment, where the slab header lives, and exactly what `pool_stats` counts. This plan settles those first. Everything else follows from them.

Facts the decisions rely on (x86-64, gcc-14):
- `struct rb_node` is 5 pointers + an enum = 44 bytes, padded to **48**. Its `alignof` is 8.
- `rb_malloc` is plain `malloc` in both builds ([tests/fault_alloc.c:13-17](tests/fault_alloc.c#L13-L17) and the inline forwarder in [tests/fault_alloc.h](tests/fault_alloc.h)). So every slab base is aligned to `alignof(max_align_t)` = **16**.

---

## Decision 1: Alignment

`pool_create(size_t obj_size)` gets a size but no alignment. Each slot has to suit two tenants, one after the other: the live object, and then the free-list pointer once the object is freed.

| Option | Guarantee | Stride | Verdict |
|---|---|---|---|
| A. Pointer alignment | `alignof(void *)` = 8 | `round_up(max(obj_size, sizeof(void*)), 8)` | Wrong for any object containing `long double`, `max_align_t` or `_Alignas(16)` members |
| B. Natural from size | `max(8, min(lowbit(obj_size), 16))` | `round_up(max(obj_size, 8), align)` | Correct, because `sizeof(T)` is always a multiple of `alignof(T)`. More cleverness than we need |
| **C. malloc's promise (recommended)** | `alignof(max_align_t)` = 16 | `round_up(max(obj_size, sizeof(void*)), alignof(max_align_t))` | Same contract as `malloc`. Correct for any `T` without asking for its alignment |

**Chosen: C.** The stride enforces it through three conditions, and all three are required:
1. The slab base is 16-aligned, because `malloc` guarantees it.
2. The slab header is padded up to a multiple of 16, so slot 0 starts 16-aligned.
3. The stride is a multiple of 16, so slot *i* sits at `base + hdr + i*stride` and stays 16-aligned.

`max(obj_size, sizeof(void*))` makes even a 1-byte object's slot large enough to hold the free-list pointer.

For `struct rb_node`: stride = `round_up(48, 16)` = **48**, so there is no padding beyond what the struct already has.

`pool_create` rejects requests it cannot meet and returns `NULL` without allocating:
- `obj_size == 0`
- `stride > 4096 - hdr`, which would mean `objs_per_slab == 0`
- overflow in `round_up`

---

## Decision 2: Where the slab header lives

**In-band (recommended):** each 4096-byte slab starts with `struct slab { struct slab *next; }`, padded to 16 bytes. The slots come after it.
- For our node: `(4096 - 16) / 48` = **85 slots**, and `16 + 85*48 = 4096` exactly. The header costs **zero slots**.
- The pool is a singly linked chain of slabs, so growing it never reallocates anything.

**Out-of-band:** the pool keeps the slab pointers somewhere else. That means either a growable `struct slab **` array, or a separately allocated descriptor per slab.
- All 4096 bytes become slots. For our node that is still 85 slots, so it buys nothing here.
- The seam has no `rb_realloc`, so growing the array means malloc-copy-free.

### What each choice costs in failure paths
| | In-band | Out-of-band |
|---|---|---|
| `rb_malloc` calls when `pool_alloc` grows the pool | **1** (the slab) | **2** (slab + array growth or descriptor) |
| Fault points per growth | 1 | 2, and the order matters |
| Unwind if the 2nd allocation fails | nothing to unwind | `goto fail_free_slab`, or a grown array left behind that must not be counted |
| State to roll back | none. The slab is linked in only after `rb_malloc` succeeds | array length and capacity, plus the slab |
| `pool_destroy` | walk `next`, `rb_free` each slab | walk the array, free each slab, free the array |

**Chosen: in-band.** The API defines a slab as "a 4096-byte chunk from `rb_malloc`". In-band keeps that to exactly one allocation, so `pool_alloc` has a single fault point and no unwinding at all. That is the Mutation-1 ideal: nothing fallible after the first mutation.

The one real advantage of out-of-band is that it keeps metadata away from buffer overruns in neighbouring objects. That doesn't justify a second fault point. ASan and valgrind already cover overruns.

---

## Decision 3: What `pool_stats` counts

```
struct rb_pool {
    size_t stride, objs_per_slab;
    struct slab *slabs;        /* in-band chain, newest first */
    void *free_list;           /* intrusive: next pointer in slot's first 8 bytes */
    char *bump, *bump_end;     /* uncarved region of the newest slab */
    size_t nslabs, nlive, nfree_list;
};
```

- **`slabs`** = number of 4096-byte chunks the pool currently holds (`nslabs`).
- **`live`** = number of slots handed out by `pool_alloc` and not yet returned by `pool_free` (`nlive`).
- **`free_objs`** = `nfree_list + (bump_end - bump) / stride`. That is the slots on the free list plus the slots in the newest slab that have never been carved. Only the newest slab can still have uncarved slots, because a new slab is added only when the free list is empty **and** the bump region is used up.
- **Invariant:** `live + free_objs == slabs * objs_per_slab`. Every slot of every slab is in exactly one of three states: uncarved, on the free list, or live. Each transition moves one slot between states:

| Event | Effect on the counts |
|---|---|
| new slab | `slabs` +1, uncarved +`objs_per_slab` |
| carve | uncarved −1, `live` +1 |
| `pool_free` | `live` −1, free list +1 |
| pop free list | free list −1, `live` +1 |

**Lazy carving, not eager threading.** Eager threading would link all 85 slots onto the free list when the slab arrives. Then `free_objs` would just be the free-list length, but slab creation would cost O(objs_per_slab). Lazy carving keeps `pool_alloc` O(1) in every case, which matches the API comment.

`pool_alloc` order: pop the free list → carve from bump → grow by one slab, then carve.

`pool_stats` is O(1) and never walks the free list. It accepts `NULL` for any output it should skip. Before the first `pool_alloc`, all three counts are 0 because `pool_create` allocates no slab.

---

## Answers to the four questions

### Where does the free-list pointer live inside a dead object, and what guarantees its alignment?
- **Where:** in the **first `sizeof(void*)` bytes of the slot**, written through `struct free_slot { struct free_slot *next; }`. It overwrites whatever the object kept there. For `rb_node` that is the `key` field, which is dead because `rb_delete` freed the key before calling `pool_free`.
- **Why that store is legal:** slab memory comes from `malloc` and has no declared type, so the store simply changes the slot's effective type. No strict-aliasing violation.
- **What guarantees the alignment:** the three conditions from Decision 1, which make every slot address ≡ 0 mod 16. That is more than enough for `alignof(void*)`.
- **What guarantees it fits:** `stride ≥ sizeof(void*)`.

### What do in-band and out-of-band headers cost in failure paths, and which suits the current API?
See the table under Decision 2.
- In-band costs one fault point per growth and no unwinding.
- Out-of-band costs two fault points, a goto-cleanup chain, and array state to roll back.

**In-band is preferable** for an API whose only primitive is "4096 bytes from `rb_malloc`".

There is a matching tree-side rule: **allocate the key copy before the pooled node.**
1. If the key copy fails, the pool is never touched.
2. If `pool_alloc` fails, free the key copy and return −1. A failed `pool_alloc` leaves the pool exactly as it was.
3. Once `pool_alloc` succeeds, nothing after it can fail.

So a failed insert never leaves the pool with an extra slab. The `pool_stats` triple is identical before and after every failed `rb_insert`, and the sweep can assert exactly that.

### What exactly will `free_objs` count?
Slots `pool_alloc` could return **without calling `rb_malloc`**: `nfree_list` plus `(bump_end - bump) / stride`. It excludes:
- live slots
- the header bytes
- the tail bytes after the last whole slot (0 bytes for stride 48)

It always satisfies `live + free_objs == slabs * objs_per_slab`.

### How does `pool_destroy` free slabs while objects are still live?
- `pool_destroy` is **type-blind and wholesale**. It walks the `slabs` chain, calls `rb_free` on each 4096-byte chunk, then frees `rb_pool_t`. It never looks at slot contents, so live objects are released along with their slab. It is NULL-safe.
- **Running destructors is the tree's job, and it happens first.** `rb_destroy` on a pooled tree:
  1. Visits every node, calling `rb_free(n->key)` and `value_free(n->value)` if values are owned. Nodes are **not** passed to `pool_free`, because that would be wasted writes on memory about to vanish.
  2. Calls `pool_destroy(t->pool)`.
  3. Calls `rb_free(t)`.
- After step 2 every node pointer dangles. The rule is: **no node access after `pool_destroy`**.
- The traversal stays recursive for now. Mutation 3 replaces it.

---

## Steps (tests first; you commit yourself)

**0. Internal header `src/pool.h`** (new). Move the five prototypes and the `rb_pool_t` typedef out of `pool.c` so both `rbtree.c` and the tests can include them. `pool.c` keeps only definitions. `include/rbtree.h` is untouched.

**1. Red tests: `tests/test_pool.c`** (new). These test the pool directly against `rb_malloc`.
- **PL-01** `pool_create` on a valid size: `stats == (0,0,0)`, and the create made exactly 1 `rb_malloc` call.
- **PL-02** `pool_create(0)` and an oversize request return NULL with no allocation.
- **PL-03** every pointer from `pool_alloc` is aligned to `alignof(max_align_t)`, for `obj_size` ∈ {1, 8, 24, 48, 100}.
- **PL-04** the invariant holds after each of 300 random alloc/free ops (crossing several slabs). The first alloc moves `slabs` from 0 to 1. Alloc number `objs_per_slab + 1` adds a second slab.
- **PL-05** LIFO reuse: free `p`, then the next alloc returns `p` with no `rb_malloc`.
- **PL-06** a fault on slab growth: `pool_alloc` returns NULL, the stats triple is unchanged, and the next alloc succeeds.
- **PL-07** `pool_destroy` with live objects leaves 0 bytes in use under valgrind. `pool_destroy(NULL)` is a no-op.

**2. Red tests: the pooled tree**
- `test_create.c` **RBC-xx**: `rb_create_pooled`, plus a fault sweep over its allocations (tree, then pool).
- `test_insert.c` / `test_delete.c`: one pooled twin per existing failure test.
- [tests/fault_sweep.c](tests/fault_sweep.c): run scenarios A and B a second time on a pooled tree. Through the internal header, also assert the stats triple is unchanged across each failed op and that the invariant holds after every op.
- [tests/fuzz.c](tests/fuzz.c): add a pooled mode that checks the invariant every step.

**3. Implement [src/pool.c](src/pool.c)** following Decisions 1-3. Use goto-cleanup only in `pool_create`, which makes one allocation, so in practice that is a plain early return.

**4. Wire it into [src/rbtree.c](src/rbtree.c) and [src/rbtree_internal.h](src/rbtree_internal.h)** with the smallest diff:
- Add `rb_pool_t *pool` to `struct rbtree`. It is NULL for unpooled trees.
- `rb_create_pooled` makes two allocations with goto-cleanup: tree, then `pool_create(sizeof(struct rb_node))`.
- Add two helpers, `node_mem_alloc` and `node_mem_free`, that dispatch on `t->pool`. Use them in `node_alloc`, `node_release`, `rb_delete` and `rb_destroy_subtree`. For pooled trees, `rb_destroy` skips freeing each node and calls `pool_destroy` instead.
- In `node_alloc`, allocate the key copy **first on the pooled path only**. Unpooled trees keep node-first. See "Open item: resolved" below for why.

**5. Makefile.** Add `src/pool.c` to `SRC`, add a `test_pool` binary to `all`, `test` and `memcheck`, and compile `pool.c` in the production `-c` check as well.

## Open item: resolved (evening 4). Unpooled `node_alloc` stays node-first
I checked every test that arms a fault:
- **RBI-06 and RBI-07** ([test_insert.c:277](tests/test_insert.c#L277), [:316](tests/test_insert.c#L316)) use `arm(1)` and check only `-1` plus "unchanged". They don't depend on the order.
- **RBC-10** ([test_create.c:89](tests/test_create.c#L89)) covers `rb_create`, not inserts.
- **RBI-16 and RBI-17** test overwrites, which make no allocation.
- **RBI-15** ([test_insert.c:575-601](tests/test_insert.c#L575-L601)) is the one that matters. Its stated purpose is: "the key-copy allocation (insert's 2nd `rb_malloc`) fails **after the node allocation succeeded** — the node must be released". It does `arm(2)` with the comment `/* #1 = node succeeds, #2 = key copy fails */`.
- **The sweep** ([fault_sweep.c:10-13](tests/fault_sweep.c#L10-L13), [:279](tests/fault_sweep.c#L279)) states its failure rule as "node, then key copy" and labels the failing site in its diagnostics.

Under key-first, every assertion would still pass. But RBI-15 would no longer exercise the path it names, the `node_release` cleanup after a successful node allocation. It would test key-copy cleanup instead. The sweep's comments and labels would also become false. That counts as weakening a test without editing it.

**Decision:** keep node-first for unpooled trees, which also means no change to `src/rbtree.c`. Use key-first only on the pooled path, where it buys the "pool stats unchanged after a failed insert" guarantee. The pooled twins in step 2 will state their own order.

---

## Evening 4 scope: the geometry, plus the invariant (approved focus)
Tonight covers only the following. The tree wiring (step 4), pooled tree tests (step 2) and pooled fuzz/sweep wait for a later evening. `src/rbtree.c` and `include/rbtree.h` are not touched.

### E4-0. `src/pool.h` (new internal header)
- Move the `rb_pool_t` typedef, the five prototypes and their comments here from `pool.c`.
- Add the geometry API:
```c
#define POOL_SLAB_SIZE 4096u
#define POOL_ALIGN     alignof(max_align_t)
struct pool_geom {
    size_t stride;          /* bytes between slot starts; multiple of POOL_ALIGN, >= max(obj_size, sizeof(void*)) */
    size_t hdr;             /* offset of slot 0 = round_up(sizeof(struct slab hdr), POOL_ALIGN) */
    size_t objs_per_slab;   /* floor((POOL_SLAB_SIZE - hdr) / stride), >= 1 */
    size_t tail;            /* unused bytes after the last slot */
};
/* 0 on success; -1 (g untouched) if obj_size == 0, overflows, or no slot fits. */
int pool_geometry(size_t obj_size, struct pool_geom *g);
```
- Slot *i* lives at `slab + g.hdr + i * g.stride`. This one function is the only place the arithmetic happens. `pool_create` stores its result, and the carve path uses `stride` / `hdr`.

### E4-1. Red test `tests/test_pool.c` (new, written before any implementation)
**Geometry, checked against hand-computed numbers.** The hardcoded table is guarded by `static_assert(alignof(max_align_t) == 16 && sizeof(void*) == 8)`, so the expected values are honest for this platform.

| obj_size | stride | hdr | objs_per_slab | tail |
|---|---|---|---|---|
| 1 | 16 | 16 | 255 | 0 |
| 8 | 16 | 16 | 255 | 0 |
| 24 | 32 | 16 | 127 | 16 |
| 48 (`rb_node`) | 48 | 16 | 85 | 0 |
| 100 | 112 | 16 | 36 | 48 |
| 4080 | 4080 | 16 | 1 | 0 |
| 0, 4081, `SIZE_MAX` | → `-1` | | | |

**Geometry, checked as properties** for every `obj_size` from 1 to 4080:
- `stride % 16 == 0`
- `stride >= obj_size`
- `stride >= sizeof(void*)`
- `hdr % 16 == 0`
- `hdr + objs*stride + tail == 4096`
- `tail < stride`, which means no extra slot would fit

**Invariant, from the pool's own reported numbers.** One helper, `check_stats(p, objs_per_slab, where)`, calls `pool_stats` and asserts `live + free_objs == slabs * objs_per_slab`. It gets `objs_per_slab` from `pool_geometry(obj_size)`, independently of the pool's internals. It runs **after every step of every scenario**. The scenarios are PL-01..PL-07 from step 1 above, plus exact expected triples at the slab boundaries:
- after the 1st alloc: `(1, 1, 84)`
- after the 85th alloc: `(1, 85, 0)`
- after the 86th alloc: `(2, 86, 84)`

### E4-2. `src/pool.c` implementation
- Implement `pool_geometry`, then the pool, following Decisions 1-3: in-band header, lazy bump carving, intrusive LIFO free list, and a lazy first slab.
- **Debug invariant:** add a `static void pool_check(const rb_pool_t *p)` compiled under `#ifndef NDEBUG`, behind a `POOL_CHECK(p)` macro that expands to nothing when `NDEBUG` is defined. It:
  - `assert`s `nlive + nfree_list + (bump_end - bump) / stride == nslabs * objs_per_slab`
  - walks the free list and asserts its length equals `nfree_list`
  - asserts `bump` is slot-aligned within the newest slab
- `POOL_CHECK` runs at the exit of `pool_create`, `pool_alloc` (success **and** failure exits) and `pool_free`. Test builds don't define `NDEBUG`, so it is active under `make test`, `asan` and `memcheck`.

### E4-3. Makefile (smallest diff)
- Add a separate `POOLBIN := build/test_pool`, built from `src/pool.c tests/fault_alloc.c tests/test_pool.c`. The test binaries that link only `rbtree.c` don't change.
- Add it to `all`, `test` and `memcheck`.
- Add a compile-only production object for `pool.c` with `BASE_CFLAGS`, next to `$(PRODOBJ)`.

### E4 Verification
- `make test`: `test_pool` is red before E4-2 and green after. All existing suites stay green, including the sweep at N=401.
- `make asan`: clean.
- `make clean && make memcheck`: 0 errors and 0 bytes in use, including PL-07 (destroy with live objects).
- Stop at green and report. You commit yourself (suggested message: `M2: pool geometry + stats invariant`).

## Files
- New: `src/pool.h`, `tests/test_pool.c`
- Modified: `src/pool.c`, `src/rbtree.c`, `src/rbtree_internal.h`, `Makefile`, `tests/fault_sweep.c`, `tests/fuzz.c`, `tests/test_create.c`, `tests/test_insert.c`, `tests/test_delete.c`
- Untouched: `include/rbtree.h`

## Verification
- `make test`: every suite passes, including `test_pool`. The sweep reaches a clean n for pooled scenarios A and B, and the run reproduces.
- `make asan`: clean.
- `make clean && make memcheck`: 0 errors and 0 bytes in use for every binary, including PL-07 and pooled `rb_destroy` with live nodes.

## Status (2026-10-10, end of evening 4): geometry complete
At your direction, scope narrowed to `pool_geometry` only. The rest of the pool waits until after the next commit.
- **Open item resolved:** unpooled `node_alloc` stays node-first. RBI-15 and the sweep's labels encode that order (see "Open item: resolved").
- **Done:**
  - `src/pool.h` holds the API (moved out of `pool.c`) plus `POOL_SLAB_SIZE`, `POOL_ALIGN`, `struct pool_geom` and `pool_geometry`.
  - `src/pool.c` holds `pool_geometry` and the two structs that determine it: `struct slab`, which sets `hdr`, and `struct free_slot`, which sets the minimum stride.
- **Tests:** `tests/test_pool_geom.c` covers GEO-01 (hand-computed table), GEO-02 (properties for every size 1..4080) and GEO-03 (rejections leave `*g` untouched). It is built as `build/test_pool_geom` and runs in `test`, `asan` and `memcheck`. `build/pool_prod.o` compile-checks `pool.c` against the production seam.
- **Written but not built yet:** `tests/test_pool.c` holds PL-01..PL-09, the stats-invariant test. `check_stats` runs after every step of every scenario. Add it to the Makefile (replacing or alongside `test_pool_geom`) once `pool_create/alloc/free/stats/destroy` land.
- **Deferred:** the debug `POOL_CHECK` invariant assertion in `pool.c`. It needs the pool's counters, so it lands together with E4-2's remaining functions.

**Verification (gcc-14):** `make test` and `make asan` both pass, with every existing suite green, the sweep at N=401 for A and B (reproduced) and GEO 3/3. `make clean && make memcheck` shows 0 errors and 0 bytes in use for all 11 binaries.
