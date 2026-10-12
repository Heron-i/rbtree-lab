# Mutation 3 ("No stack to spare"): O(1)-space teardown and traversal

> **Status:** plan approved (2026-10-11).
> - **Increment 1:** done, in commit `cf4cf0e`.
> - **Increment 2:** done, in commit `fe457f7`.
> - **Increment 3:** green under `make test` + `make asan`, awaiting review.
> - **Increment 4:** not started.
>
> This file is a copy of the approved plan. Increment 4 turns it into the final record with the test results.

## Context
The comments in `include/rbtree.h` now require `rb_foreach` and `rb_destroy` to use **O(1) auxiliary space with no recursion**. Both are recursive today: `rb_destroy_subtree` is at [src/rbtree.c:62-69](src/rbtree.c#L62-L69) and `rb_foreach_node` at [src/rbtree.c:444-450](src/rbtree.c#L444-L450). (`rb_delete` is already iterative and is not touched.)
- **rb_destroy** will use right-spine rotation.
- **rb_foreach** will use the explicit parent-pointer walk. It is read-only. Its memory cost is shown through `pool_stats`, with a test and documentation only, and **pool.c is not changed**.
- **Misuse:** a callback that mutates the tree, or a NULL `fn`, is documented as UB. A debug-build `assert` checks that `t->size` is unchanged after each callback.

### Hard constraint: no explicit stack
We do **not** fix the recursion by pushing node pointers onto an explicit stack. That covers a stack on the heap and also a fixed-size array on the C stack. It cannot work, for three reasons:
1. **Its size is O(h), not O(1).** That still breaks the header's contract.
2. **`rb_destroy` returns `void`.** If the stack's `rb_malloc` fails, destroy cannot report the failure. It would have to leak the tree, and every failure path must unwind completely.
3. **A fixed array has a hidden depth cap.** Any array of fixed size has a depth beyond which it overflows.

The traversal state has to live in pointers the tree already has (see "Where traversal state lives").

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
**Loop invariant. It must hold at the top of every pass:**
1. **Freed nodes are fully disconnected.** No live node's `left` or `right` link points at a freed node, and `t->root` never does either. Parent links are dead during teardown and are never read.
2. **Rotations free nothing.** A rotation changes only the shape of the live part. The set of live nodes and their in-order sequence stay the same.
3. **`t->root` is always the root of the still-live part.** The live nodes are exactly the nodes reachable from `t->root` through `left`/`right`. That structure is a real tree: each live node has exactly one incoming link, and there are no cycles.
4. **Each pass does exactly one of two things:**
   - **Restructure the live part.** If the root has a left child, a right rotation moves that child up.
   - **Remove the root.** If the root has no left child, it is the leftmost live node. We release it (value if owned, then key, then the node itself when unpooled) and move `t->root` to its right subtree.
   - Removing the root is safe because that node has no left child, and its right subtree is now reachable only through `t->root`. So releasing it leaves invariants 1-3 true.
   - As a side effect, nodes are released in ascending key order. We observe this but do not promise it in the contract.

**What it maintains, and why it cannot spin forever.** Let *L* be the number of live nodes. Let *s* be the length of the right spine from `t->root`.
- **A rotation** puts `l` on the spine (*s* +1). `r` stays on the spine. `l->right`'s subtree moves from under `l` to under `r->left`, so it stays off the spine. *L* does not change.
- **A removal** takes the spine head off (*s* −1, *L* −1).
- **No step ever moves a node off the spine.** Each node enters the spine at most once, so there are at most N rotations, and there are exactly N removals.

**Measure:** Φ_D = *L* + (*L* − *s*), i.e. live nodes plus live nodes off the spine.
- Nonnegative, because *s* ≤ *L*.
- A rotation lowers Φ_D by 1: off-spine count −1.
- A removal lowers Φ_D by 1: −2 + 1.
- So **every pass lowers Φ_D by exactly 1**, from 2N − s₀ down to 0. The total is exactly 2N − s₀ ≤ 2N − 1 passes.

**Precondition:** invariant 3 holds on entry, so the tree really is a tree. A cycle, such as a thread a Morris traversal left behind, would make this loop spin or double-free. This is one reason Morris is rejected.

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

**Precondition:** parent links are correct. `rb_validate` does **not** check them, so the fuzzer gains a parent-link check (Increment 1).

### Where traversal state lives, and why it is O(1)
A recursive version keeps "where am I, and where do I go next" in one stack frame per level, which is O(h). Both new loops split that state in two:

| | Per-call state (fixed size) | "Where next" state (already in the tree) |
|---|---|---|
| `rb_destroy` | local `r`, plus `l` (rotate) or the saved `r->right` (remove). That is 2 pointers in `rb_destroy`'s one frame, or in registers. | **The live part's own `left`/`right` links.** Rotation re-points child links that would be thrown away anyway, so the right spine *is* the to-do list. Nodes waiting to be freed are found by following links, not by remembering them. `t->root` is the cursor. |
| `rb_foreach` | locals `n`, `p`, and debug-only `size_before`. That is 2 pointers + 1 `size_t` in `rb_foreach`'s one frame. | **Each node's existing `parent` field.** The way back up is read from the tree, not pushed onto a stack. Nothing is written. |

Why this is O(1):
- **There is no recursion.** `rb_foreach → fn` is one call deep. `fn`'s own frame belongs to the caller and is not counted.
- **The frame size is fixed at compile time.** It holds a fixed number of scalars, with no VLA and no array.
- **There is no allocation.** RBX-11 and RBW-12 check this with `fault_alloc_total`.
- **No field is added to any node.** The parent pointer was already paid for by the insert/delete fixups. Its cost is 0 slots in the pooled build: `struct rb_node` is 48 B with `parent` and 40 B without, and `pool_geometry` rounds both to stride 48, i.e. 85 objs/slab. So `pool_stats` is identical whether or not the field exists. Unpooled, the cost depends on malloc chunk sizes: 64 B vs 48 B with glibc x86-64.

The 64 KB-stack stress test checks this whole argument end to end.

### rb_foreach contract (goes into a comment above rb_foreach)
- Visits every entry exactly once, in strictly ascending `strcmp` order.
- **Allowed in `fn`:** read-only calls on `t` (`rb_find`, `rb_size`, `rb_validate`, a nested `rb_foreach`). `fn` may also change what `*value` points to.
- **UB in `fn`:** `rb_insert`, `rb_delete`, or `rb_destroy` on `t`. Debug builds assert that `t->size` is unchanged after each call. `fn == NULL` and `t == NULL` are preconditions.
- **If `fn` never returns:**
  - **It loops forever:** the tree is untouched, so other read-only users stay safe.
  - **It `longjmp`s or `exit`s:** the tree is still valid, usable and destroyable. Nothing is held, so nothing leaks.
  - **No resume:** there is no cursor, so a caller who wants to continue has to restart the walk.
- **Why not Morris.** Morris temporarily rewrites `right` links and casts away `const`.
  - If `fn` never returns, it leaves cycles behind. Then `rb_validate` recurses without end, and right-spine `rb_destroy` spins or double-frees.
  - A nested foreach corrupts the tree further.
  - Concurrent readers race.

---

## Increments: one function per cycle

Each increment is **one diff** that is reviewed. Every increment follows the same cycle:
1. **Write that function's tests.**
2. **Run them against today's code and show the red output.**
3. **Write the implementation.**
4. **Run the battery and show the output:** `make test` and `make asan`. `make clean && make memcheck` runs only when requested (from Increment 3 on; Increments 1-2 also ran it).
5. **Stop for review.** The commit is made only after approval.

A red test can't be committed by itself, because every commit has to be green. So each diff holds the new tests **and** the implementation together. The red output from step 2 is shown during review as evidence. Tests that already pass on today's code (guards) go into the increment for the function they protect.

**Shared chain fixture**, introduced in Increment 2 and reused in Increment 3: a hand-built degenerate chain with correct parent links, keys `"k%07d"`, and `t->size = N`.
- Node storage comes from `pool_alloc(t->pool)` if `t->pool` is set, otherwise from `rb_malloc`. Keys come from `rb_malloc`.
- It works in both builds and needs no N/A entry.
- It never calls `rb_validate`, because the chain is not an RB tree.
- Each test file is its own binary, so the fixture is a `static` helper in each file that needs it. That copies about 20 lines, to keep the files independent as they are today.

### Static core helpers in src/rbtree.c (added 2026-10-12)
The spec suggests isolating each loop in a `static` helper with one duty and its invariant comment on top. In Mutation 3 that means:
- **Teardown core (Increment 2):** `destroy_all_nodes(t)` holds the right-spine loop, with the four-part invariant and Φ_D above it. `rb_destroy` becomes a thin wrapper: NULL check, `destroy_all_nodes`, `pool_destroy`, `rb_free(t)`.
  - The teardown rotation is written inline and does **not** reuse `rotate_right`. `rotate_right` reads `x->parent` to decide whether to update `t->root`. During teardown, parent links are dead (invariant 1): once the old root is removed, the new root's `parent` still points at the freed node, so `rotate_right` would read and write freed memory.
- **Traversal core (Increment 3):**
  - `inorder_first(t)` holds the descend loop (measure: subtree height).
  - `inorder_next(t, n)` holds the descend-or-climb step (measure: depth). It is const, unlike `tree_minimum`, which stays as it is for `rb_delete`.
  - `rb_foreach` becomes one `for` loop over them, carrying Φ_F, the `fn` call and the debug size assert.
- **What is not shared:**
  - **Node release.** `rb_delete` frees key then value, while teardown frees value then key and skips the per-node free when pooled. Merging them would change behavior.
  - **`src/pool.c`.** It already follows this style (`freelist_push`/`freelist_pop`) and is not touched.
- **Testing:** all helpers are `static` and are tested only through `rb_destroy` and `rb_foreach`. The planned tests are unchanged.

### Increment 1: guard the parent links (test only, M3 prep). Done in `cf4cf0e`
- **Why first:** the foreach walk depends on correct parent links, and `rb_validate` does not check them. This pins the precondition before anything relies on it.
- [tests/fuzz.c](tests/fuzz.c): `run_validate` now also checks that `root->parent == &t->nil` and that every child's parent is its node, in both plain and pooled modes.
  - **Cadence:** it runs with the fuzzer's existing validation, every 100 ops plus once at the end, not after every op. That keeps valgrind cost flat.
- **No src change.** It was green on the existing code.
- **Shown to go red:** a throwaway mutant that corrupted only one parent pointer, leaving child links and colors correct. The fuzzer failed with `broken parent link at op 99`, while `rb_validate` alone did not notice.

### Increment 2: rb_destroy (right-spine teardown)
**Destroy comes before foreach.** The stress tests have to tear down their 10⁶-node trees. If destroy were still recursive, teardown would crash and hide what the foreach test is measuring.

Tests:
- **New [tests/test_stress.c](tests/test_stress.c)** plus a Makefile target (`-pthread`), added to `all`, `test` and `memcheck`. Each case runs inside a pthread with `pthread_attr_setstacksize(64 * 1024)`.
  - **RBM-01:** a real RB tree of N = 1,048,576 built with `rb_create_pooled` + `rb_insert`, then `rb_destroy` on the 64 KB thread. The `value_free` count must equal N. This is the stress test, and it passes today because RB height is about 40.
  - **RBM-02:** a pooled left chain of N = 1,048,576, destroyed on the 64 KB thread. **Red today.**
  - **RBM-03:** the same with a right chain. **Red today.**
- [tests/test_destroy.c](tests/test_destroy.c), guards, with the header comment updated:
  - **RBX-08 / RBX-09:** left chain / right chain of N = 4096.
  - **RBX-10:** a zigzag shape. Every value is freed exactly once.
  - **RBX-11:** `rb_destroy` makes zero `rb_malloc` calls.

Implementation in [src/rbtree.c](src/rbtree.c):
- Replace `rb_destroy_subtree` with the teardown core `destroy_all_nodes`, which runs the right-spine loop on `t->root` under its invariant comment. `rb_destroy` wraps it.
- Per-node release stays as it is: `value_free`, `rb_free(key)`, then `if (t->pool == NULL) rb_free(n)`. Move `t->root` past the node before releasing it.
- Then call `pool_destroy` and `rb_free(t)`.
- Fix the `node_release` comment that mentions `rb_destroy_subtree`.

### Increment 3: rb_foreach (parent-pointer walk)
Tests:
- [tests/test_stress.c](tests/test_stress.c): add a foreach stage to RBM-01..03, on the same 64 KB thread **before** the destroy. It checks the count and strictly ascending order. RBM-02/03 go **red today**, because recursive foreach on a 10⁶ chain overflows.
- [tests/test_foreach.c](tests/test_foreach.c), guards:
  - **RBW-11:** 4096-node hand-built shapes are visited in ascending order. As built, this covers the left chain, the right chain (one 4095-level final climb) and the zigzag, not only the left chain.
  - **RBW-12:** zero `rb_malloc` calls, the `pool_stats` triple unchanged on pooled trees, and the stride-48 parent-cost check from `pool_geometry`.
  - **RBW-13:** a callback `longjmp`s out. The tree is still valid, and a re-walk is complete.
  - **RBW-14:** a nested foreach gives N×N calls in order.
  - **The debug size assert has no committed test,** because a passing test can't survive the `abort`. It was checked once with a throwaway probe whose callback deletes a key mid-walk, and the assert fired.

Implementation in [src/rbtree.c](src/rbtree.c):
- Replace `rb_foreach_node` with the traversal core, `inorder_first` and `inorder_next`, each with its loop invariant and measure. `rb_foreach` is one `for` loop over them, with the contract comment and the `#ifndef NDEBUG` size assert (adds `<assert.h>`).

### Increment 4: finalize this file
- Turn this plan into the record of what was done: mark each increment done with its commit, note anything that changed from the plan, and add the final battery output.
- It records only work that is done. Docs only, so `make test` is re-run to confirm nothing changed.

**Across all increments:**
- No explicit stack and no allocation in either function.
- No changes to `include/rbtree.h`, `src/pool.c` or `src/pool.h`.
- No test is ever weakened.

---

## Verification (per increment)
- **Red:** the new tests run against the old code, with output shown. Increment 1 has no red step, because it is a guard.
- **Green:** `make test` and `make asan`, with the output shown. `make clean && make memcheck` runs only when requested, because valgrind on ≥10⁶-node trees is slow.
- Then the diff is reviewed and committed.
