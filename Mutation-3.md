# Mutation 3 ("No stack to spare"): O(1)-space teardown and traversal

> **Status:** code complete. Gap closure is in progress (Increments 5-7). The plan was approved on 2026-10-11 and the code was written in three increments, one function per cycle:
> - **Increment 1:** the parent-link guard, commit `cf4cf0e`.
> - **Increment 2:** `rb_destroy`, commit `fe457f7`.
> - **Increment 3:** `rb_foreach`, commit `339b296`.
> - **Increment 4:** this file finalized as the record, plus the final battery (docs only).
>
> A check against the spec on 2026-10-12 found three gaps, which are being closed as small diffs:
> - **Increment 5:** the big stress case behind `--big`. Green, awaiting review.
> - **Increment 6:** the `const` wording in the `rb_foreach` contract. Not started.
> - **Increment 7:** final status update. Not started.
> - The spec's "account for the cost in your reflection" item is covered in `REFLECTION.md`, which you write, so it isn't an increment here.
>
> Anything that changed from the plan is noted where it happened. The final battery is under "Final verification".

## Context
The comments in `include/rbtree.h` require `rb_foreach` and `rb_destroy` to use **O(1) auxiliary space with no recursion**.
- **Before Mutation 3,** both were recursive: `rb_destroy_subtree` recursed into both children, and `rb_foreach_node` did a recursive in-order walk. `rb_delete` was already iterative and was not touched.
- **`rb_destroy`** now uses right-spine rotation: the teardown core `destroy_all_nodes`, [src/rbtree.c:71-97](src/rbtree.c#L71-L97).
- **`rb_foreach`** now uses the explicit parent-pointer walk: the traversal core `inorder_first`/`inorder_next`, [src/rbtree.c:468-520](src/rbtree.c#L468-L520). It is read-only. Its memory cost is shown through `pool_stats` by a test (RBW-12) and documentation only, so **`src/pool.c` was not changed**.
- **Misuse:** a callback that mutates the tree, or a NULL `fn`, is documented as UB. A debug-build `assert` checks that `t->size` is unchanged after each callback.

### Hard constraint: no explicit stack
The recursion was **not** replaced by pushing node pointers onto an explicit stack, whether on the heap or as a fixed-size array on the C stack. That cannot work, for three reasons:
1. **Its size is O(h), not O(1).** That still breaks the header's contract.
2. **`rb_destroy` returns `void`.** If the stack's `rb_malloc` failed, destroy could not report the failure. It would have to leak the tree, and every failure path must unwind completely.
3. **A fixed array has a hidden depth cap.** Any array of fixed size has a depth beyond which it overflows.

The traversal state lives in pointers the tree already has (see "Where traversal state lives").

Out of scope: `rb_validate` and `rb_count_nodes` stay recursive. The header's O(1) requirement covers only foreach and destroy, and RB height bounds their depth at about 2·log₂(n+1).

---

## Theory

### Right-spine teardown
The loop uses `t->root` itself as the cursor, so "root" always means the root of the still-live part of the tree.
```
while t->root != nil:
    r = t->root
    if r->left != nil:  l = r->left; r->left = l->right; l->right = r; t->root = l   // rotate right; frees nothing
    else:               t->root = r->right; release(r)                              // r is the leftmost live node
```
**Loop invariant. It holds at the top of every pass:**
1. **Freed nodes are fully disconnected.** No live node's `left` or `right` link points at a freed node, and `t->root` never does either. Parent links are dead during teardown and are never read.
2. **Rotations free nothing.** A rotation changes only the shape of the live part. The set of live nodes and their in-order sequence stay the same.
3. **`t->root` is always the root of the still-live part.** The live nodes are exactly the nodes reachable from `t->root` through `left`/`right`. That structure is a real tree: each live node has exactly one incoming link, and there are no cycles.
4. **Each pass does exactly one of two things:**
   - **Restructure the live part.** If the root has a left child, a right rotation moves that child up.
   - **Remove the root.** If the root has no left child, it is the leftmost live node. It is released (value if owned, then key, then the node itself when unpooled), and `t->root` moves to its right subtree.
   - Removing the root is safe because that node has no left child, and its right subtree is now reachable only through `t->root`. So releasing it leaves invariants 1-3 true.
   - As a side effect, nodes are released in ascending key order. This is observed but not promised in the contract.

**What it maintains, and why it cannot spin forever.** Let *L* be the number of live nodes. Let *s* be the length of the right spine from `t->root`.
- **A rotation** puts `l` on the spine (*s* +1). `r` stays on the spine. `l->right`'s subtree moves from under `l` to under `r->left`, so it stays off the spine. *L* does not change.
- **A removal** takes the spine head off (*s* −1, *L* −1).
- **No step ever moves a node off the spine.** Each node enters the spine at most once, so there are at most N rotations, and there are exactly N removals.

**Measure:** Φ_D = *L* + (*L* − *s*), i.e. live nodes plus live nodes off the spine.
- Nonnegative, because *s* ≤ *L*.
- A rotation lowers Φ_D by 1: off-spine count −1.
- A removal lowers Φ_D by 1: −2 + 1.
- So **every pass lowers Φ_D by exactly 1**, from 2N − s₀ down to 0. The total is exactly 2N − s₀ ≤ 2N − 1 passes.

**Precondition:** invariant 3 holds on entry, so the tree really is a tree. A cycle, such as a thread a Morris traversal left behind, would make this loop spin or double-free. This is one reason Morris was rejected.

### Parent-pointer in-order walk
```
n = min(root)
while n != nil:
    fn(n)
    if n->right != nil:  n = min(n->right)
    else:                climb while n is its parent's right child; n = parent
```
**Invariant:** every key smaller than `n->key` has been passed to `fn` exactly once, and no key ≥ `n->key` has been passed yet.

**Measures, all strictly decreasing and ≥ 0:**
- Outer loop: Φ_F = nodes not yet passed to `fn`. Drops by exactly 1 per pass, because each pass moves to the in-order successor.
- Descend loop: height of `n`'s subtree.
- Climb loop: depth of `n`. It stops at the root because `root->parent == &t->nil`.
- Whole run: (edges not yet descended) + (edges not yet ascended) ≤ 2(N−1). That is O(N) time.

**Precondition:** parent links are correct. `rb_validate` does **not** check them, so the fuzzer checks them (Increment 1).

### Where traversal state lives, and why it is O(1)
A recursive version keeps "where am I, and where do I go next" in one stack frame per level, which is O(h). Both loops split that state in two:

| | Per-call state (fixed size) | "Where next" state (already in the tree) |
|---|---|---|
| `rb_destroy` | local `r`, plus `l` (rotate) or the saved `r->right` (remove). That is 2 pointers in `destroy_all_nodes`'s one frame, or in registers. | **The live part's own `left`/`right` links.** Rotation re-points child links that would be thrown away anyway, so the right spine *is* the to-do list. Nodes waiting to be freed are found by following links, not by remembering them. `t->root` is the cursor. |
| `rb_foreach` | `n` in `rb_foreach`, `n` and `p` in `inorder_next`, and debug-only `size_before`. That is a fixed handful of pointers + 1 `size_t`, with `inorder_next` one call deep. | **Each node's existing `parent` field.** The way back up is read from the tree, not pushed onto a stack. Nothing is written. |

Why this is O(1):
- **There is no recursion.** `rb_foreach → fn` and `rb_foreach → inorder_next` are each one call deep. `fn`'s own frame belongs to the caller and is not counted.
- **Every frame size is fixed at compile time.** Each holds a fixed number of scalars, with no VLA and no array.
- **There is no allocation.** RBX-11 and RBW-12 check this with `fault_alloc_total`.
- **No field is added to any node.** The parent pointer was already paid for by the insert/delete fixups. Its cost is 0 slots in the pooled build: `struct rb_node` is 48 B with `parent` and 40 B without, and `pool_geometry` rounds both to stride 48, i.e. 85 objs/slab. So `pool_stats` is identical whether or not the field exists (RBW-12 checks this). Unpooled, the cost depends on malloc chunk sizes: 64 B vs 48 B with glibc x86-64.

The 64 KB-stack stress test (RBM-01..03) checks this whole argument end to end.

### rb_foreach contract (the comment above rb_foreach)
- Visits every entry exactly once, in strictly ascending `strcmp` order.
- **Allowed in `fn`:** read-only calls on `t` (`rb_find`, `rb_size`, `rb_validate`, a nested `rb_foreach`). `fn` may also change what `*value` points to.
- **UB in `fn`:** `rb_insert`, `rb_delete`, or `rb_destroy` on `t`. Debug builds assert that `t->size` is unchanged after each call. `fn == NULL` and `t == NULL` are preconditions.
- **If `fn` never returns:**
  - **It loops forever:** the tree is untouched, so other read-only users stay safe.
  - **It `longjmp`s or `exit`s:** the tree is still valid, usable and destroyable (RBW-13). Nothing is held, so nothing leaks.
  - **No resume:** there is no cursor, so a caller who wants to continue has to restart the walk.
- **Why not Morris.** Morris temporarily rewrites `right` links and casts away `const`.
  - If `fn` never returns, it leaves cycles behind. Then `rb_validate` recurses without end, and right-spine `rb_destroy` spins or double-frees.
  - A nested foreach corrupts the tree further.
  - Concurrent readers race.

---

## How the work was done: one function per cycle

Each increment was **one diff**, reviewed and committed only after approval. Every increment followed the same cycle:
1. **Write that function's tests.**
2. **Run them against the existing code and show the red output.**
3. **Write the implementation.**
4. **Run the battery.**
   - Increments 1 and 2 ran `make test`, `make asan` and `make clean && make memcheck`.
   - From Increment 3 on, routine runs used `make test` + `make asan` only, with memcheck run on request because valgrind on ≥10⁶-node trees is slow.
5. **Stop for review.**

A red test can't be committed by itself, because every commit has to be green. So each diff held the new tests **and** the implementation together, and the red output was shown during review. Tests that already passed on the old code (guards) went into the increment for the function they protect.

**Chain fixture** (`fixture_add` / `fixture_build`), used by Increments 2 and 3:
- It hand-builds degenerate shapes (left chain, right chain, zigzag) with correct parent links, keys `"k%07u"`, and `t->size = N`.
- Node storage comes from `pool_alloc(t->pool)` if `t->pool` is set, otherwise from `rb_malloc`. Keys come from `rb_malloc`. So it works in both builds and needs no N/A entry.
- It never calls `rb_validate`, because the shapes are not RB trees.
- Each test file is its own binary, so the fixture is a `static` copy in `test_destroy.c`, `test_foreach.c` and `test_stress.c`, as planned. A shared `tests/chain_fixture.h` was considered and not adopted.

### Static core helpers in src/rbtree.c (added 2026-10-12)
The spec suggests isolating each loop in a `static` helper with one duty and its invariant comment on top. In Mutation 3 that became:
- **Teardown core (Increment 2):** `destroy_all_nodes(t)` holds the right-spine loop, with the invariant and Φ_D above it. `rb_destroy` is a thin wrapper: NULL check, `destroy_all_nodes`, `pool_destroy`, `rb_free(t)`.
  - The teardown rotation is written inline and does **not** reuse `rotate_right`. `rotate_right` reads `x->parent` to decide whether to update `t->root`. During teardown, parent links are dead (invariant 1): once the old root is removed, the new root's `parent` still points at the freed node, so `rotate_right` would read and write freed memory.
- **Traversal core (Increment 3):**
  - `inorder_first(t)` holds the descend loop (measure: subtree height). It checks for an empty tree first, because the sentinel's `left` points back at itself and the loop would otherwise never end.
  - `inorder_next(t, n)` holds the descend-or-climb step (measure: depth). It is const, unlike `tree_minimum`, which stays as it is for `rb_delete`.
  - `rb_foreach` is one `for` loop over them, carrying Φ_F, the `fn` call and the debug size assert.
- **What is not shared:**
  - **Node release.** `rb_delete` frees key then value, while teardown frees value then key and skips the per-node free when pooled. Merging them would change behavior.
  - **`src/pool.c`.** It already follows this style (`freelist_push`/`freelist_pop`) and was not touched.
- **Testing:** all helpers are `static` and are tested only through `rb_destroy` and `rb_foreach`.

### Increment 1: guard the parent links (test only). Commit `cf4cf0e`
- **Why first:** the foreach walk depends on correct parent links, and `rb_validate` does not check them. This pinned the precondition before anything relied on it.
- [tests/fuzz.c](tests/fuzz.c): `run_validate` also checks that `root->parent == &t->nil` and that every child's parent is its node (`parent_links_ok`), in both plain and pooled modes.
  - **Changed from the plan:** the plan said "after every op". The check runs with the fuzzer's existing validation instead, every 100 ops plus once at the end, which keeps valgrind cost flat.
- **No src change.** It was green on the existing code.
- **Shown to go red:** a throwaway mutant that corrupted only one parent pointer, leaving child links and colors correct. The fuzzer failed with `broken parent link at op 99`, while `rb_validate` alone did not notice.

### Increment 2: rb_destroy (right-spine teardown). Commit `fe457f7`
**Destroy came before foreach.** The stress tests have to tear down their 10⁶-node trees. If destroy were still recursive, teardown would crash and hide what the foreach test measures.

Tests:
- **New [tests/test_stress.c](tests/test_stress.c)** plus a Makefile target (`-pthread`) in `all`, `test` and `memcheck`. Each case runs inside a pthread with `pthread_attr_setstacksize(64 * 1024)`, on N = 2²⁰ = 1,048,576 pooled nodes.
  - **RBM-01:** a real RB tree built with `rb_insert`. This is the stress test; it passes even with recursion, because RB height is about 40.
  - **RBM-02:** a left chain.
  - **RBM-03:** a right chain.
  - Each checks that every value is freed exactly once.
- [tests/test_destroy.c](tests/test_destroy.c), guards:
  - **RBX-08 / 09 / 10:** left chain / right chain / zigzag of 4096 nodes, each value freed exactly once.
  - **RBX-11:** `rb_destroy` makes zero `rb_malloc` calls.

**Red run, on the recursive `rb_destroy_subtree`:**
- RBX-08..11 passed in both builds, and RBM-01 passed.
- RBM-02 crashed with SIGSEGV. gdb put the crash in `rb_destroy_subtree`, about 1,910 frames deep on the 64 KB thread (64 KB ÷ about 32 B per frame ≈ 2,000), so it was stack overflow and not a fixture bug.

Implementation in [src/rbtree.c](src/rbtree.c):
- `rb_destroy_subtree` was replaced by `destroy_all_nodes`.
- Per-node release is unchanged in order: `value_free`, `rb_free(key)`, then `if (t->pool == NULL) rb_free(r)`. `t->root` moves past the node before it is released.
- The `node_release` comment now names `destroy_all_nodes`.

**Green:** `make test`, `make asan`, and `make clean && make memcheck` all exited 0. All 25 valgrind runs reported 0 errors, with all heap blocks freed.

### Increment 3: rb_foreach (parent-pointer walk). Commit `339b296`
Tests:
- [tests/test_stress.c](tests/test_stress.c): RBM-01..03 now run `rb_foreach` on the same 64 KB thread **before** the destroy. The walk must visit k0..k(N−1) in order, each with its own value.
- [tests/test_foreach.c](tests/test_foreach.c), guards:
  - **RBW-11:** 4096-node hand-built shapes are visited in ascending order.
    - **Changed from the plan:** it covers the left chain, the right chain (one 4095-level final climb) and the zigzag, not only the left chain.
  - **RBW-12:** zero `rb_malloc` calls, the `pool_stats` triple unchanged on pooled trees, and the same `pool_geometry` stride and objs/slab with and without the parent pointer.
  - **RBW-13:** a callback `longjmp`s out halfway. Afterwards `rb_validate == 0`, the size is unchanged, and a fresh walk is complete. This is the one place `test_foreach.c` uses `rb_validate`.
  - **RBW-14:** a nested `rb_foreach` gives 64×64 calls, in order at both levels.
- **The debug size assert has no committed test,** because a passing test can't survive the `abort`. It was checked once with a throwaway probe whose callback deletes a key mid-walk, and the assert fired: `Assertion 't->size == size_before && "rb_foreach callback inserted or deleted a key"' failed`.

**Red run, on the recursive `rb_foreach_node`:**
- RBW-11..14 passed in both builds, and RBM-01 passed.
- RBM-02 crashed with SIGSEGV. gdb put the crash in `rb_foreach_node`, about 1,270 frames deep on the 64 KB thread.

Implementation in [src/rbtree.c](src/rbtree.c):
- `rb_foreach_node` was replaced by `inorder_first` and `inorder_next`. `rb_foreach` is one `for` loop over them, with the contract comment and the `#ifndef NDEBUG` size assert (adds `<assert.h>`).
- An `-O2 -DNDEBUG` build compiles warning-free, with the assert compiled out.

**Green:** `make test` and `make asan` exited 0. Memcheck was not run per increment from here on; see "Final verification".

### Increment 4: finalize this file (docs only)
- **Rewritten from plan to record:**
  - The status block lists each increment's commit.
  - Context is in the past tense and links to the new code.
  - Each increment section gives its tests, red-run evidence, implementation and green result.
- **Changes from the plan,** each recorded in the increment where it happened:
  - the fuzz check's every-100-ops cadence
  - RBW-11 covering three shapes
  - the debug assert checked only by a throwaway probe
  - the shared fixture header that was considered and not adopted
- **One theory correction:** `rb_foreach`'s per-call state includes `inorder_next`'s locals, one call deep. It is still a fixed size.
- **No code changed.** The full battery (`make test`, `make asan`, `make clean && make memcheck`) was run on the code at `339b296`. The results are below.

### Gap closure (added 2026-10-12)
Checking the finished work against the spec's wording found three items that were only partly met:
1. **The big case was not behind a flag.** `test_stress` always ran 2²⁰ nodes, including under valgrind.
2. **The `rb_foreach` contract comment in the code didn't name the `const` wrinkle.** It was covered only in this file.
3. **This file's status said "complete".**

Each is closed by one small reviewed diff, using the same cycle as above.

### Increment 5: big stress case behind `--big`. Green, awaiting review
- **You decided:**
  - `test_stress` runs N = 2¹⁴ = 16,384 by default, and `--big` runs the 2²⁰ (≥10⁶) case.
  - `make test` and `make asan` always pass `--big`.
  - `make memcheck` runs the small default, and `make memcheck STRESS_FLAGS=--big` opts the big case in.
- **Why 2¹⁴ still proves the bound:** the recursive code overflowed the 64 KB stack after about 1,270 frames (foreach) and 1,910 frames (destroy). A 16,384-deep chain is 8-13× past that.
- [tests/test_stress.c](tests/test_stress.c):
  - The `STRESS_N` macro became `SMALL_N`/`BIG_N` plus a runtime `stress_n`. Both are powers of two, so RBM-01's index mask still permutes [0, N).
  - `main` accepts no argument or `--big`. Anything else prints usage and exits nonzero, so a typo can't silently run the small case.
  - It prints the N in use before the cases.
- [Makefile](Makefile): adds `STRESS_FLAGS ?=`. The `test` recipe passes `--big`, and `memcheck` passes `$(STRESS_FLAGS)`.
- **Shown to still go red:** the new `test_stress.c`, built in the scratchpad against the pre-Mutation-3 `src/rbtree.c` (`63a9bb9`), crashed with SIGSEGV in RBM-02 at **both** sizes, while RBM-01 passed.
- **Flags:**
  - With no argument it reports N = 16384 and runs in 0.03 s.
  - With `--big` it reports N = 1048576 and runs in 1.3 s.
  - With `--bogus` or `--big extra` it prints usage and exits 1.
- **Battery:**
  - `make test`: exit 0, 204 PASS, 0 FAIL, with `test_stress` at N = 1048576.
  - `make asan`: exit 0, the same 204 PASS, 0 FAIL, and no sanitizer reports, also at N = 1048576.
  - `make clean && make memcheck`: exit 0. All 25 valgrind runs report 0 errors with all heap blocks freed, and `test_stress` ran at N = 16384. The total time was **53 s**, down from 1 min 20 s.

### Increment 6: `const` wording in the `rb_foreach` contract. Not started
- **Plan:** add one sentence to the contract comment above `rb_foreach` in [src/rbtree.c](src/rbtree.c). It will say that the walk honors the header's `const rbtree_t *` because it only reads links, and that Morris traversal would have to cast `const` away to rewrite `right` links.
- **Scope:** a comment only, so no behavior changes and no new tests. `make test` and `make asan` confirm the build stays clean.

### Increment 7: final status update. Not started
- **Plan:** once Increments 5 and 6 are committed, mark them done with their commits in the status block and here.
- Update "Final verification" for the `--big` split.
- Set the status to complete.

**Across all increments:**
- No explicit stack and no allocation in either function.
- No changes to `include/rbtree.h`, `src/pool.c` or `src/pool.h`.
- No test was weakened, skipped or removed.

---

## Final verification
Run 2026-10-12 on the code at commit `339b296`. Increment 4 changes only this file.

| Command | Exit | Result |
|---|---|---|
| `make test` | 0 | 204 test lines PASS, 0 FAIL, across the unpooled and pooled batteries. Both fuzz modes pass 100,000 ops (the parent-link check runs every 100 ops). All 4 fault sweeps fail at every predicted site and leave the tree unchanged. RBM-01..03 pass on the 64 KB stack. |
| `make asan` | 0 | The same 204 PASS, 0 FAIL, with no AddressSanitizer or UBSan reports. The 64 KB thread is enough under ASan too. |
| `make clean && make memcheck` | 0 | All 25 valgrind runs report `ERROR SUMMARY: 0 errors`, and all 25 report "All heap blocks were freed -- no leaks are possible". That includes `test_stress` (3 × 2²⁰ nodes walked and destroyed). The run took 1 min 20 s. |
