# M5 Mutation 1: allocation-failure injection + fault sweep

## Context
Milestone 5, Mutation 1 requires that every allocation in `src/rbtree.c` goes through a replaceable `rb_malloc`/`rb_free` seam, and a harness that runs a **fault sweep**: for n = 1, 2, 3, … re-run a fixed scenario with the n-th allocation failing, until a run finishes without hitting the fault.

Today the wrappers are `static` inside [rbtree.c:8-17](src/rbtree.c#L8-L17) and are driven by a one-shot `rb_fail_next_alloc` flag that lives in `src/`. Three tests use that flag:
- RBC-10 in [test_create.c:90](tests/test_create.c#L90)
- RBI-06 and RBI-07 in [test_insert.c:272](tests/test_insert.c#L272) and [test_insert.c:310](tests/test_insert.c#L310)

A test build can't swap out a static function, so the seam isn't real yet.

**Success means all of the following:**
- Every n passes.
- The sweep terminates.
- The same n gives the same result on every run.
- All of it is clean under `make test`, `make asan` and `make memcheck`.

## Design principle
- **Rule:** move every allocation to the left of the commit point, so there is nothing to unwind after committing.
- **What we must prove:** every operation either completes after all of its fallible work is done, or fails before it mutates the tree and frees everything it acquired.
- **Commit point:** the first write to any memory reachable from `t` (a link, `size++`, recoloring, a rotation). Nothing to the right of it may fail.

## Allocation sites (complete list for `src/rbtree.c`)
| # | Site | Function | Size |
|---|---|---|---|
| A | [rbtree.c:20](src/rbtree.c#L20) | `rb_create` | `struct rbtree` |
| B | [rbtree.c:153](src/rbtree.c#L153) | `node_alloc` ← `rb_insert` | `struct rb_node` (insert allocation #1) |
| C | [rbtree.c:157](src/rbtree.c#L157) | `node_alloc` ← `rb_insert` | key copy, `strlen(key)+1` (insert allocation #2) |

There are no other `rb_malloc` calls. `rb_delete` ([rbtree.c:358](src/rbtree.c#L358)) only calls `rb_free` and `value_free`.

### `rb_insert`: the allocate-then-commit pattern
```
find key (no allocation, read-only)
  ├─ key found → OVERWRITE path: swap value, value_free(old), return 0   (no allocation)
  └─ not found → rb_malloc node (#1, site B)
                 → rb_malloc key copy (#2, site C)
                 ══ COMMIT POINT ══
                 → link under parent, size++, insert_fixup (recolor/rotate) → return 0
```
The current code already has this shape ([rbtree.c:151-173](src/rbtree.c#L151-L173) and [rbtree.c:227-258](src/rbtree.c#L227-L258)). Everything after `node_alloc` returns non-NULL is pointer and color writes, none of which can fail. **This mutation does not change insert logic. The sweep is the proof.**

### What each failure point has acquired, and what must be unwound
| Fails at | Acquired so far | Must unwind | Tree touched? | Returns | Caller still owns |
|---|---|---|---|---|---|
| A (`rb_create`) | nothing | nothing | no tree exists | `NULL` | `value_free` was never stored |
| B (node, #1) | nothing (only the read-only search ran) | nothing | no | `-1` | `value` and `key` |
| C (key copy, #2) | node `z` (uninitialized, never linked) | `rb_free(z)` via `node_release` | no | `-1` | `value` and `key` |
| after C | — | — | **committed, nothing can fail** | `0` | tree owns `value` and the key copy |

### Why each failure path leaves the tree exactly as it was
- **Tree unchanged:** before the commit point, `rb_insert` only reads `t` (the `strcmp` walk) and writes local variables (`parent`, `cur`). `z->parent = parent` writes into `z`, not into the tree. So at B or C, no byte reachable from `t` has changed.
- **`rb_size` unchanged:** `t->size++` happens only after the commit point.
- **The value and key still belong to the caller:**
  - `value` is stored only in `z->value`, and the `z->value = value` line comes after allocation C succeeds. `value_free` is never called on any failure path.
  - The caller's `key` is only read (`strlen`/`memcpy`). The tree never keeps the caller's pointer.
- **No leak:** at B nothing is held. At C the only thing held is `z`, and `fail_release_node → node_release(z)` frees it before `return -1`.

### Paths that never allocate
- **Overwrite (key already present):** it returns before `node_alloc` is reached, so it cannot fail and makes no allocation.
- **`rb_delete`:** it has no `rb_malloc` call, so it cannot fail because of allocation.
- **Both are enforced by the harness:** across every overwrite and delete, `fault_alloc_total()` must not change (step 3).
- **Note, out of scope:** re-inserting the *same* value pointer under an existing key makes the overwrite path free the value it has just stored. Flagged only; not changed (smallest diff).

## Standard cleanup structure for allocation failures
Every function that allocates more than once uses goto-cleanup in reverse order of acquisition. `node_alloc` is the reference:
```c
a = rb_malloc(...);  if (!a) goto fail;
b = rb_malloc(...);  if (!b) goto fail_free_a;
/* initialize a/b; no mutation of the tree */
return a;                    /* hand off to the caller, which then commits */
fail_free_a:  rb_free(a);
fail:         return NULL;   /* caller returns its documented error code */
```
Rules:
1. Each label frees exactly what was acquired above its matching `goto`, in reverse order.
2. No cleanup label ever touches the tree.
3. The caller commits only after the helper returns success.

## Decisions
- **Production forwarding lives in `tests/fault_alloc.h`** (your choice). The contract prototypes stay verbatim under `#ifdef RB_FAULT_INJECT`. The `#else` branch defines `static inline` forwarders to `malloc`/`free`.
- **The armed fault fires once, then disarms itself.**
- **`fault_alloc_total()` counts every `rb_malloc` call, including the one that fails.** So for a run that starts at total `t0` and is armed with n, the fault fired iff `total − t0 ≥ n`.
- **Harness-owned values are allocated with plain `malloc`, never `rb_malloc`**, so they don't shift what "the n-th allocation" means.

## Steps
Step 0 comes first because tasks 1 and 2 can only be checked when an operation actually fails. Your tasks 1 and 2 are swapped because the helper's last clause uses task 2's counter. Steps 0–4 and 6 are test code and are written before step 5, the implementation.

### 0. Fault-injection contract (test infrastructure)
- **`tests/fault_alloc.h`:** the contract you pasted, plus the production `#else` branch described in Decisions.
- **`tests/fault_alloc.c`:**
  - State: `static long total, countdown;` (countdown 0 = disarmed).
  - `rb_malloc` increments `total`. If the fault is armed and `--countdown == 0`, it returns NULL and disarms.
  - `rb_free` forwards to `free`.
  - `arm(n)` sets the countdown, `disarm()` clears it, and `total()` returns the count.

### 1. Counting destructor (your task 2)
This goes in `tests/fault_sweep.c`.
- **Value type:** `struct tval { uint32_t magic; int id; }`, allocated with `malloc`, with `magic = 0x7E57BEEF`.
- **`counting_free`:**
  - Asserts the magic is intact.
  - Poisons the magic.
  - Calls `free`.
  - Increments the global `g_freed`.
- **What it proves:** across a failed insert, `g_freed` must not move. The harness then frees the value itself, as the caller who owns it. If the tree had already freed it, that becomes a double free, which ASan and Valgrind report.

### 2. "Exactly as it was" helper (your task 1)
```c
struct before { size_t size; void *find_failed; long freed; };
static bool assert_unchanged(const rbtree_t *t, const struct model *m,
                             const struct before *b, const char *key,
                             const struct tval *caller_val);
```
It checks five things and prints a diagnostic for each one that fails:
1. `rb_validate(t) == 0`.
2. `rb_size(t) == b->size`.
3. `rb_find(t, key) == b->find_failed`.
4. Every key in the model is findable with its value.
5. `g_freed == b->freed`, and `caller_val->magic` is intact.

**A test of the test:** the helper is also called once with a deliberately wrong `before`, and it must return false.

### 3. Sweep harness (your task 3)
**Model and scenario.**
- **Model:** keys `k000`–`k199`, stored as an array indexed by id holding `struct tval *` (NULL = absent).
- **Scenario:** deterministic, with the order fixed by a constant stride permutation.
  - Insert all 200 keys.
  - Delete 100 of them.
  - Overwrite 50 of the remaining keys.

**Expected allocation map.** This is what makes the run at a given n map to a specific failure site.
- n = 1 is site A (`rb_create`).
- For the k-th new-key insert (k = 1..200):
  - Site B (node) is allocation 2k.
  - Site C (key copy) is allocation 2k + 1.
- N = 401. The clean run is n = 402.

**Each run (fault armed at the n-th allocation):**
1. Record `t0 = fault_alloc_total()`, then `fault_alloc_arm(n)`.
2. Call `rb_create(counting_free)`.
   - n = 1: expect `NULL`, check that `g_freed` didn't move, mark the fault as hit, and end the run.
   - Any other n: a `NULL` here is a failure.
3. For each insert of a new key:
   - Take a `before` snapshot, then call `rb_insert`.
   - If it returns −1:
     - Assert that this is the expected insert k and site (B if n is even, C if n is odd).
     - Call `assert_unchanged`.
     - `free` the value ourselves, and record that the fault was hit.
     - Leave the model unchanged and continue the scenario. The later operations exercise the tree that survived the failure.
   - Any −1 at an unexpected place is a failure.
4. For each delete and overwrite:
   - Assert that `fault_alloc_total()` did not change across the call (the no-allocation guarantee).
   - Assert that `g_freed` went up by exactly 1.
   - Assert the return value is 0.
5. After the scenario, do a full model check, call `rb_destroy`, then `fault_alloc_disarm()`.
6. If the fault did not fire (`total − t0 < n`), this is the clean run: N = `total − t0`. Stop.

**Checks on the sweep as a whole:**
- **N > 0.** Otherwise the injector isn't wired in. This is the expected red state before step 5.
- **N equals the predicted 401.**
- **Every n ≤ N saw exactly one failed operation**, at its predicted site.
- **The clean run is repeated once and must give the same N** (determinism).
- **Termination guard:** if n exceeds 100000, fail loudly.
- Each non-obvious loop gets a one-line invariant comment.

### 4. Makefile
- **New target:** a `build/fault_sweep` binary, run from `test`, and therefore also from `asan` and `memcheck`.
- **Every test binary** compiles with `-DRB_FAULT_INJECT -Itests` and links `tests/fault_alloc.c`.
- **A compile-only `build/rbtree_prod.o`** (no `RB_FAULT_INJECT`) is added to `all`, so the production branch is also built with `-Werror`.

### 5. Implementation: switch `src/rbtree.c` to the seam
- Delete [rbtree.c:8-17](src/rbtree.c#L8-L17): the flag and the static wrappers.
- Add `#include "fault_alloc.h"`.
- No call site changes and no changes to insert logic. The analysis above shows it already follows the commit-point pattern.

### 6. Port the old fault tests and add targeted cases (in `test_insert.c` / `test_create.c`)
**Port the existing tests:**
- In RBC-10, RBI-06 and RBI-07, replace `rb_fail_next_alloc = true` with `fault_alloc_arm(1)`, and add `fault_alloc_disarm()` afterwards. Their assertions stay exactly the same.
- RBI-06 and RBI-07 already cover site B. RBC-10 covers site A.

**Add two targeted cases:**
- **New RBI-15 (site C):** on a non-empty tree, call `fault_alloc_arm(2)` and then insert a new key. Expect `-1`, a tree that is byte-for-byte unchanged (reusing RBI-07's `snapshot_walk`/`snapshots_equal`), and the same size. Valgrind and ASan confirm the node was freed.
- **New RBI-16 (overwrite doesn't allocate):** an overwrite leaves `fault_alloc_total()` unchanged, even while the fault is armed with `arm(1)`.

## Status (2026-10-09) and tonight's scope: steps 5 + 6
- **Done:** steps 0–4. The suite is red only on the sweep (`N == 0, injector not wired`) under test, asan and memcheck. All existing tests and the fuzzer are green.
- **Tonight:** steps 5 and 6, landed together. Once the flag is deleted, the three old tests won't link until they're ported.

**First:** rename this plan file to `/home/lheer/.claude/plans/Mutation-1.md`.

**Step 5 edits ([src/rbtree.c](src/rbtree.c)):**
- Replace lines 8–17 (`rb_fail_next_alloc` plus the two static wrappers) with `#include "fault_alloc.h"`.
- Nothing else changes.

**Step 6 edits:**
- **[tests/test_create.c](tests/test_create.c):**
  - Replace `extern bool rb_fail_next_alloc;` and its comment with `#include "fault_alloc.h"`.
  - In RBC-10, `rb_fail_next_alloc = true` becomes `fault_alloc_arm(1)`.
  - Call `fault_alloc_disarm()` immediately after `rb_create`.
  - Update the diagnostic string to say "fault_alloc_arm(1)".
- **[tests/test_insert.c](tests/test_insert.c):**
  - Make the same include swap, and port RBI-06 and RBI-07 the same way (arm before the insert, disarm right after). The assertions stay unchanged.
  - **RBI-15 (site C, key-copy failure):** build the tree from 50, 25, 75, take a snapshot, call `fault_alloc_arm(2)`, insert "10", then disarm. Expect `rc == -1`, the same root, the same size, and `snapshots_equal`.
  - **RBI-16 (an overwrite doesn't allocate):** insert "10" with the old value, call `fault_alloc_arm(1)`, record `a0 = fault_alloc_total()`, overwrite "10" with the new value, then disarm. Expect `rc == 0`, `fault_alloc_total() == a0`, and `rb_find` returning the new value.
  - Add both new tests to the test table.
  - Update the header comment: it now implements RBI-01..RBI-16, and the "overwrite never allocates" deferral is resolved by RBI-16.

**Then:**
- Run `make CC=gcc-14 test`, `asan` and `memcheck`, and show the output. The expected result is the sweep printing N=401, clean at n=402.
- If any n fails, stop and report. Fix `src/`; never the tests.
- Once everything is green, commit with `M5: allocation-failure injection seam and fault sweep`.

## Files
- **New:** `tests/fault_alloc.h`, `tests/fault_alloc.c`, `tests/fault_sweep.c`
- **Modified:** `src/rbtree.c` (seam only), `tests/test_create.c`, `tests/test_insert.c`, `Makefile`
- **Untouched:** `include/rbtree.h` and `src/pool.c`

## Verification
- Run `make test`, `make asan` and `make memcheck`, and show the output of all three.
- The sweep prints N = 401 and reports that n = 1..401 each failed at its predicted site.
- Before step 5, the sweep must fail with "N == 0, injector not wired". That is the tests-first red state.
- Commit once everything is green: `M5: allocation-failure injection seam and fault sweep`.

---

# Part 2: M5 Mutation 1 (continued): Steps 7–11, delete/overwrite paths and the rest of the sweep

## Status (2026-10-09, end of evening 2): Steps 0–6 complete
All of [Mutation-1.md](Mutation-1.md) Steps 0–6 are done and committed in `d5b0076`, and `git diff d5b0076` is empty:
- The injector, the counting destructor, the "exactly as it was" helper and sweep scenario A are in place.
- The Makefile wiring is done, `rbtree.c` is on the seam, and the RBC-10/RBI-06/RBI-07 tests are ported.
- RBI-15 and RBI-16 were added.

`make test`, `asan` and `memcheck` are all green (with `CC=gcc-14`), and the sweep reports N=401, clean at n=402.

## Context for Steps 7–11
Today the sweep checks delete and overwrite only by count: no new allocations, and `g_freed` went up by 1. In scenario A every fault fires during the insert phase, so a delete or overwrite never runs while a fault is pending, except in the final clean run.

The spec says the honest fix may be to delete an allocation rather than handle its failure. That fix is already built into the code:
- **`rb_delete` relinks the successor node in place of the deleted one** ([rbtree.c:368-383](src/rbtree.c#L368-L383)) instead of copying the successor's key and value into it. A copy-based delete would need a new key copy after the tree was changed, an allocation that can fail too late to back out. It would also `value_free` the wrong value.
- **Overwrite reuses the existing node and key copy** ([rbtree.c:224-228](src/rbtree.c#L224-L228)).

Neither path allocates. Steps 7–11 prove that under a live fault.

The header's ownership comments expose two bugs, and you chose to fix both:
1. **Re-inserting the same value pointer frees it.** `rb_insert(t, k, v)` when `v` is already stored under `k` calls `value_free(v)`, which frees the value the tree has just taken ownership of. That leaves a dangling pointer, and `rb_destroy` later frees it again. ([rbtree.c:225-227](src/rbtree.c#L225-L227))
2. **`rb_delete` calls `value_free` on a half-updated tree.** The callback runs before `delete_fixup` and before `size--` ([rbtree.c:388-395](src/rbtree.c#L388-L395)). At that moment `rb_validate` fails, because the node count no longer equals `size`.

**Principle (the mirror of Part 1):** allocations go before the commit point, and releases (`rb_free` and the caller's `value_free`) go after the tree is fully consistent again.

## Steps (tests first; no commits, you commit yourself)

### Step 7: documentation catch-up
- Append the Status section above to `Mutation-1.md`, in both `~/.claude/plans/` and the repo copy.
- Append Steps 7–11 below to both copies too.
- Delete this temporary plan file afterwards, so `Mutation-1.md` stays the single plan.

### Step 8: sweep extensions in [tests/fault_sweep.c](tests/fault_sweep.c)
**8a, check which value was freed, not just how many.**
- `counting_free` records `g_last_freed`.
- Delete must free exactly the model's value for the deleted key. This catches a copy-based delete that frees the successor's value.
- An overwrite of a present key must free exactly the old value, and the new value's magic must stay intact.

**8b, a live fault during every delete and overwrite.**
- If the run's main fault has already fired, `fault_alloc_arm(1)` before the operation and `fault_alloc_disarm()` after it.
- If the main fault is still pending, it is already live, so leave it alone.
- Either way, assert that `fault_alloc_total()` didn't change and that the operation succeeded.

**8c, a consistency check inside the destructor.**
- `g_check_tree` is set only during a delete or overwrite call.
- While it's set, `counting_free` asserts `rb_validate(g_check_tree) == 0`.
- It's NULL during `rb_destroy`.

**8d, scenario B (interleaved).**
- 50 rounds. Each round inserts 4 new keys (stride order), deletes one key from the previous round, overwrites one present key, and re-inserts one present key with the *same* value pointer.
- A same-pointer re-insert must leave `g_freed` unchanged and `rb_find` returning the same pointer.
- Scenario A stays exactly as it is.

**8e, a general predicted-failure rule (replaces the closed-form `expected_site`).**
- Before each new-key insert, take `rel = fault_alloc_total() − t0`.
- If `rel < n ≤ rel+2`, this insert must fail: at the node allocation if `n == rel+1`, at the key copy if `n == rel+2`.
- Otherwise it must succeed.

**Shared structure:**
- Each scenario is a fixed op list `{INSERT, DELETE, OVERWRITE, REINSERT_SAME} × id`, run by the existing model-driven `run_scenario`.
- `main` sweeps A, then B. Each must give N = 401, reproducibly.
- Each non-obvious loop gets a one-line invariant comment.

### Step 9: red unit tests
- **RBI-17 ([test_insert.c](tests/test_insert.c)):** a tree created with `free_recorder`. Insert "10" → `&tag`, reset the recorder, then insert "10" → `&tag` again.
  - Expect `rc == 0`, `free_recorder_count == 0`, `rb_find == &tag`, and `rb_validate == 0`.
  - Currently fails: the count is 1.
- **RBD-19 ([test_delete.c](tests/test_delete.c)), planned as RBD-17 but renumbered because RBD-17 and RBD-18 already existed:** a static `g_t`, and a `value_free` callback that records `rb_validate(g_t)` and `rb_size(g_t)`. Build about 7 keys, then delete a black node with two children.
  - Expect the callback ran once, saw `rb_validate == 0`, and saw `rb_size == size_before − 1`.
  - Currently fails because `size` is stale when the callback runs.
- **Run all three targets and confirm the red state:**
  - RBI-17 and RBD-19 fail.
  - The sweep fails on 8c at the first delete, and on the first same-pointer re-insert in scenario B.
  - Everything else stays green.

### Step 10: fixes in [src/rbtree.c](src/rbtree.c) (the smallest diff that passes)
- **F1, overwrite:** `if (t->value_free != NULL && old_value != value) t->value_free(old_value);`. Add a one-line comment: when the pointer is the same, the tree already owns it, so there's nothing to release.
- **F2, delete:**
  - Move `rb_free(z->key)`, `value_free(z->value)` and `rb_free(z)` below `t->size--`.
  - Update the comment: z is unlinked and nothing reaches it, and releasing after the commit means the callback sees a valid tree.
  - Safe because no path from `x` leads back to z after the transplants, and `delete_fixup` never reads z.

### Step 11: comment upkeep
- In `test_insert.c` and `test_delete.c`, update the "Implements …" ranges in the header comments.
- In `fault_sweep.c`, update the file header to describe both scenarios and the general failure rule.

## Files
- **Modified:** `tests/fault_sweep.c`, `tests/test_insert.c`, `tests/test_delete.c`, `src/rbtree.c` (F1 + F2 only), and `Mutation-1.md` (both copies)
- **Untouched:** `include/rbtree.h`, `tests/fault_alloc.{h,c}`, `Makefile`, `src/pool.c`

## Verification
- Run `make CC=gcc-14 test`, then `asan`, then `memcheck`, and show the output.
- **After Step 9 (red):** only RBI-17, RBD-19 and the sweep fail, for the reasons above.
- **After Step 10 (green):**
  - Unit tests: 17/17 insert and 19/19 delete pass.
  - The sweep prints N=401 for scenario A and for scenario B, each reproduced.
  - Valgrind: 0 errors, 0 bytes in use.
- If anything else fails, stop and report. Fix `src/`; never the tests.

## Status (2026-10-10, end of day 3): Steps 7–11 complete
All of Part 2 is done; Mutation 1 is complete and waiting for your commit.
- **Step 8:** `fault_sweep.c` checks which value was freed (by serial), runs every delete, overwrite and same-pointer re-insert under a live fault, validates the tree inside `value_free`, and sweeps scenarios A and B with the general `rel < n ≤ rel+2` rule.
- **Step 9:** RBI-17 and RBD-19 were added and confirmed red: RBI-17 saw 1 `value_free` call; RBD-19 saw `rb_validate == 1` and a stale `rb_size` (5, want 4); the sweep reported an inconsistent tree in the callback and a double free in scenario B.
- **Step 10:** F1 (skip `value_free` when the overwrite stores the same pointer) and F2 (release the key copy, the value and the node only after `delete_fixup` and `size--`) in `src/rbtree.c`. No allocation was added or removed: delete and overwrite already never allocated.
- **Step 11:** header comments updated in `test_insert.c` (RBI-01..RBI-17, with the overwrite rule stated as MUST NOT ALLOCATE), `test_delete.c` (RBD-19) and `fault_sweep.c` (both scenarios and the failure rule).
- **RBI-17 is stronger than planned:** besides the Step 9 checks, the re-insert runs under `fault_alloc_arm(1)` and asserts `fault_alloc_total()` did not change, so it checks the no-allocation rule directly.

**Verification (with `CC=gcc-14`):**
- `make test` and `make asan`: every suite passes (insert 17/17, delete 19/19); fuzz passes 100,000 operations; the sweep prints N=401 for A and for B, clean at n=402, reproduced.
- `make memcheck`: 0 errors and 0 bytes in use for every binary.
- Run `make clean` before `make memcheck` if `make asan` ran last; otherwise valgrind runs the ASan-built binaries, which exit before any test runs.
