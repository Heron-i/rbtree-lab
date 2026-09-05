# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This is a CS370 lab assignment scaffold for implementing a red-black tree in C. As of now the
repository contains only stub files — the Makefile, header, and all source/test files are empty
except for a one-line purpose comment at the top of each. There is no build system, no test
runner, and no git repository initialized yet. Treat any "commands" below as things to build,
not things that already work.

## Intended structure

- `include/rbtree.h` — public API/interface for the red-black tree (types, function
  declarations). Other files should include this rather than reaching into `src/` directly.
- `src/rbtree.c` — "Implement a full-working, self-sufficient red-black tree": the actual
  insert/delete/search/rotation/rebalancing logic and any internal (static) helpers.
- `tests/test_rbtree.c` — "My unit tests created with known specs and edge cases
  (table-driven tests)": deterministic, table-driven correctness tests (empty tree, single node,
  rotations, recoloring, deletion cases, etc.).
- `tests/fuzz.c` — "Randomized stress driver for discovering potential unknown flaws": a
  randomized/property-based driver (e.g. random insert/delete sequences checked against RB
  invariants — no red-red violations, equal black-height on all paths, valid BST ordering).
- `Makefile` — needs to be written; expect targets to build the library/tests and run both the
  table-driven tests and the fuzz driver.

# rbtree-lab: project rules
  ## Commands
  - Build & unit tests: ‘make test‘
  - Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
  - A change is DONE only when all three pass. Always run them; show output.

  ## Hard constraints
  - NEVER modify include/rbtree.h. It is the graded contract.
  - Check every allocation. malloc can return NULL; a NULL return must
  leave the tree unchanged and return the documented error code.
  - NEVER weaken, skip, or delete a test to make the suite pass. If a test
  looks wrong, stop and explain why instead.

  ## Style
  - C23. -Wall -Wextra -Werror must stay clean. No VLAs.
  - Error handling: goto-cleanup pattern for multi-allocation functions.
  - Prefer the smallest diff that passes. Do not refactor unrelated code.
  - Every non-obvious loop gets a one-line invariant comment.

  ## Workflow
  - For any multi-file or algorithmic change: propose a plan and wait for
  approval before editing.
  - Commit only from a green state; message format "M<n>: <what>".

## Working in this repo

- Keep `tests/test_rbtree.c` table-driven and deterministic; keep `tests/fuzz.c` randomized and
  invariant-checking (BST property, no red-red edges, equal black-heights) rather than
  comparing against fixed expected output.
- After adding a Makefile, verify the standard C red-black tree invariants are exercised by both
  test files before considering a change complete: BST ordering, root is black, no red node has
  a red child, every root-to-leaf path has the same black-height.
