// Purpose: Randomized stress driver for discovering potential unknown flaws.
// Differentially fuzzes rb_insert/rb_find/rb_delete against an independent
// sorted-array reference model (bounded key universe), validating full
// tree invariants at least every 100 operations and cross-checking a full
// rb_foreach traversal on a coarser cadence. Allocation-failure injection
// (rb_fail_next_alloc) is intentionally out of scope for this pass and is
// left for a later, more extensive iteration of this fuzzer.
#include "rbtree.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEY_LEN 16
#define POOL_SIZE 2000u      /* insertable keys */
#define OOP_EXTRA 200u       /* extra keys used only for finds, never inserted */
#define VALIDATE_EVERY 100UL
#define FOREACH_EVERY 5000UL
#define MIN_OPS 100000UL     /* hard floor: never run fewer than this, regardless of argv[1] */

/* ---- PRNG: xorshift64star, seeded independently of libc rand() state ---- */
static uint64_t rng_next(uint64_t *state) {
    /* xorshift64star never maps a nonzero state to zero, so as long as the
     * caller seeds with a nonzero value this needs no per-call zero-guard */
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- reference model: sorted, growable array of fixed-size-key entries ---- */
typedef struct {
    char key[MAX_KEY_LEN];
    void *value;
} model_entry_t;

typedef struct {
    model_entry_t *entries;
    size_t count;
    size_t capacity;
} model_t;

static void model_init(model_t *m) {
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

static void model_free(model_t *m) {
    free(m->entries);
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

/* Returns the index of key if present (found=true), else its sorted
 * insertion point (found=false). */
static size_t model_lower_bound(const model_t *m, const char *key, bool *found) {
    size_t lo = 0, hi = m->count;
    *found = false;
    /* invariant: if key is present, its index lies in [lo, hi) */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(m->entries[mid].key, key);
        if (cmp == 0) {
            *found = true;
            return mid;
        } else if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void *model_get(const model_t *m, const char *key, bool *out_found) {
    bool found;
    size_t idx = model_lower_bound(m, key, &found);
    if (out_found) *out_found = found;
    return found ? m->entries[idx].value : NULL;
}

static void model_grow(model_t *m) {
    size_t new_cap = m->capacity == 0 ? 64 : m->capacity * 2;
    model_entry_t *new_entries = realloc(m->entries, new_cap * sizeof *new_entries);
    if (!new_entries) {
        fprintf(stderr, "fuzz: reference model realloc failed (out of memory)\n");
        exit(EXIT_FAILURE);
    }
    m->entries = new_entries;
    m->capacity = new_cap;
}

/* Inserts a new key or overwrites the value of an existing one. Does not
 * free anything -- freeing an old overwritten value is the tree's job via
 * value_free, and duplicating that here would double-free. */
static void model_upsert(model_t *m, const char *key, void *value) {
    bool found;
    size_t idx = model_lower_bound(m, key, &found);
    if (found) {
        m->entries[idx].value = value;
        return;
    }
    if (m->count == m->capacity) model_grow(m);
    /* shift entries[idx..count) right by one to open a slot at idx */
    memmove(&m->entries[idx + 1], &m->entries[idx], (m->count - idx) * sizeof *m->entries);
    snprintf(m->entries[idx].key, MAX_KEY_LEN, "%s", key);
    m->entries[idx].value = value;
    m->count++;
}

/* Removes key from the model. Caller confirms presence first via
 * model_get, since rb_delete's rc is the source of truth for whether the
 * key existed. Never touches .value's pointee -- rb_delete already freed
 * it through the tree's value_free callback. */
static void model_remove(model_t *m, const char *key) {
    bool found;
    size_t idx = model_lower_bound(m, key, &found);
    if (!found) return; /* defensive; call sites already checked */
    memmove(&m->entries[idx], &m->entries[idx + 1],
            (m->count - idx - 1) * sizeof *m->entries);
    m->count--;
}

/* ---- test values: content-stamped so the oracle can catch corruption,
 * not just stale/wrong pointers ---- */
typedef struct {
    int seq;
    char echoed_key[MAX_KEY_LEN];
} value_t;

static void value_free_fn(void *v) {
    free(v);
}

typedef enum { OP_INSERT, OP_FIND, OP_DELETE } op_kind_t;

static void make_key(char *buf, size_t buflen, unsigned idx) {
    snprintf(buf, buflen, "k%05u", idx);
}

/* ---- periodic full-traversal cross-check against the model ---- */
typedef struct {
    const model_t *model;
    size_t idx;
    bool ok;
    unsigned long op_index;
} foreach_ctx_t;

static void foreach_check_cb(const char *key, void *value, void *ctx_) {
    foreach_ctx_t *ctx = ctx_;
    if (!ctx->ok) return;
    if (ctx->idx >= ctx->model->count) {
        fprintf(stderr,
                "fuzz: foreach mismatch at op %lu: tree has more entries than model (extra key \"%s\")\n",
                ctx->op_index, key);
        ctx->ok = false;
        return;
    }
    const model_entry_t *expected = &ctx->model->entries[ctx->idx];
    if (strcmp(expected->key, key) != 0 || expected->value != value) {
        fprintf(stderr,
                "fuzz: foreach mismatch at op %lu, position %zu: tree(key=\"%s\", value=%p) model(key=\"%s\", value=%p)\n",
                ctx->op_index, ctx->idx, key, value, expected->key, expected->value);
        ctx->ok = false;
        return;
    }
    ctx->idx++;
}

static bool run_validate(rbtree_t *t, unsigned long op_index) {
    if (rb_validate(t) != 0) {
        fprintf(stderr, "fuzz: rb_validate failed at op %lu\n", op_index);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    unsigned long requested = MIN_OPS;
    if (argc > 1) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (end != argv[1] && *end == '\0' && parsed > 0) requested = parsed;
    }
    /* no invocation is ever allowed to run fewer than MIN_OPS operations */
    unsigned long total_ops = requested > MIN_OPS ? requested : MIN_OPS;

    uint64_t seed = 0xC0FFEEULL;
    if (argc > 2) {
        char *end = NULL;
        unsigned long long parsed = strtoull(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0') seed = (uint64_t)parsed;
    }
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;

    fprintf(stderr, "fuzz: running %lu operations (seed=%llu)\n",
            total_ops, (unsigned long long)seed);

    int exit_code = EXIT_SUCCESS;
    bool ok = true;
    int seq = 0;
    char keybuf[MAX_KEY_LEN];

    model_t model;
    model_init(&model);

    rbtree_t *tree = rb_create(value_free_fn);
    if (!tree) {
        fprintf(stderr, "fuzz: rb_create failed\n");
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    /* invariant: ok stays true only while every check so far has matched the model */
    for (unsigned long op = 0; ok && op < total_ops; op++) {
        unsigned roll = (unsigned)(rng_next(&seed) % 100);
        op_kind_t op_kind = (roll < 50) ? OP_INSERT : (roll < 80) ? OP_FIND : OP_DELETE;

        if (op_kind == OP_INSERT) {
            unsigned idx = (unsigned)(rng_next(&seed) % POOL_SIZE);
            make_key(keybuf, sizeof keybuf, idx);

            value_t *val = malloc(sizeof *val);
            if (!val) {
                fprintf(stderr, "fuzz: harness malloc failed building test value at op %lu\n", op);
                ok = false;
                break;
            }
            val->seq = seq++;
            snprintf(val->echoed_key, sizeof val->echoed_key, "%s", keybuf);

            int rc = rb_insert(tree, keybuf, val);
            if (rc == 0) {
                model_upsert(&model, keybuf, val);
            } else {
                /* per rbtree.h: on failure the tree is unchanged and the
                 * value is not consumed, so it's still ours to free */
                free(val);
            }
        } else if (op_kind == OP_FIND) {
            unsigned range = POOL_SIZE + OOP_EXTRA;
            unsigned idx = (unsigned)(rng_next(&seed) % range);
            make_key(keybuf, sizeof keybuf, idx);

            bool found;
            void *expected = model_get(&model, keybuf, &found);
            void *actual = rb_find(tree, keybuf);
            if (actual != expected) {
                fprintf(stderr,
                        "fuzz: find mismatch at op %lu: key=\"%s\" expected=%p actual=%p (model says %s)\n",
                        op, keybuf, expected, actual, found ? "present" : "absent");
                ok = false;
            }
        } else {
            unsigned range = POOL_SIZE + OOP_EXTRA;
            unsigned idx = (unsigned)(rng_next(&seed) % range);
            make_key(keybuf, sizeof keybuf, idx);

            bool found;
            model_get(&model, keybuf, &found);
            int expected_rc = found ? 0 : -1;
            int rc = rb_delete(tree, keybuf);
            if (rc != expected_rc) {
                fprintf(stderr,
                        "fuzz: delete mismatch at op %lu: key=\"%s\" expected_rc=%d actual_rc=%d (model says %s)\n",
                        op, keybuf, expected_rc, rc, found ? "present" : "absent");
                ok = false;
            } else if (found) {
                model_remove(&model, keybuf);
            }
        }

        if (ok) {
            size_t tree_size = rb_size(tree);
            if (tree_size != model.count) {
                fprintf(stderr, "fuzz: size mismatch at op %lu: tree=%zu model=%zu\n",
                        op, tree_size, model.count);
                ok = false;
            }
        }

        if (ok && (op % VALIDATE_EVERY) == (VALIDATE_EVERY - 1)) {
            ok = run_validate(tree, op);
        }

        if (ok && (op % FOREACH_EVERY) == (FOREACH_EVERY - 1)) {
            foreach_ctx_t ctx = { .model = &model, .idx = 0, .ok = true, .op_index = op };
            rb_foreach(tree, foreach_check_cb, &ctx);
            if (ctx.ok && ctx.idx != model.count) {
                fprintf(stderr,
                        "fuzz: foreach mismatch at op %lu: tree has fewer entries than model (%zu of %zu)\n",
                        op, ctx.idx, model.count);
                ctx.ok = false;
            }
            ok = ctx.ok;
        }
    }

    if (ok) {
        ok = run_validate(tree, total_ops);
    }
    if (!ok) {
        exit_code = EXIT_FAILURE;
    }

cleanup:
    rb_destroy(tree);
    model_free(&model);
    if (exit_code == EXIT_SUCCESS) {
        fprintf(stderr, "fuzz: PASSED (%lu operations)\n", total_ops);
    } else {
        fprintf(stderr, "fuzz: FAILED\n");
    }
    return exit_code;
}
