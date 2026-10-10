# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.


# rbtree-lab: project rules
  ## Commands
  - Build & unit tests: ‘make test‘
  - Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
  - A change is DONE only when all three pass. Always run them; show output.
  - Run 'make clean' before 'make memcheck' if you just ran 'make asan'; otherwise memcheck reuses the sanitizer binaries.
  - Valgrind is slow (the fuzzer runs many random ops; that is expected). Run 'make memcheck' only at the end of each session.

  ## Hard constraints
  - NEVER modify include/rbtree.h. It is the graded contract.
  - All heap allocation in src/ goes through rb_malloc/rb_free (tests/fault_alloc.h). Direct malloc/free in src/ is a defect.
  - Any allocation may fail. Every failure path must unwind completely and leave the tree unchanged and return the documented error code.
  - NEVER weaken, skip, or delete a test to make the suite pass. If a test looks wrong, stop and explain why instead.
  - Do not implement or modify `src/pool.c` unless the task is specifically concerned with the pooled build.

  ## Style
  - C23. -Wall -Wextra -Werror must stay clean. No VLAs.
  - Error handling: goto-cleanup pattern for multi-allocation functions.
  - Prefer the smallest diff that passes. Do not refactor unrelated code.
  - Every non-obvious loop gets a one-line invariant comment.

  ## Workflow
  - For any multi-file or algorithmic change: propose a plan and wait for
  approval before editing.
  - build tests first always, then implementation.
  - Commit only from a green state; message format "M<n>: <what>".

  ## Code map (→ = depends on / calls)
- include/rbtree.h ← src/rbtree.c ← tests/*.c
- src/rbtree.c → rb_malloc/rb_free (the allocation seam), supplied by:
  - tests/fault_alloc.c: fault injector, used by test builds
  - src/pool.c: slab pool, used by the pooled build. Touch only for pooled-build tasks.
- tests/fuzz.c: randomized ops checked against a reference model