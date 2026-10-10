// Purpose: Allocation-failure fault sweep (M5, Mutation 1). For n = 1, 2,
// 3, ... re-runs a fixed, deterministic scenario with the n-th rb_malloc
// failing, until a run completes without the fault ever firing. Two
// scenarios are swept:
//   A: insert 200 keys, then delete 100, then overwrite 50 (the spec's).
//   B: 50 interleaved rounds of {insert 4 new keys, delete one, overwrite
//      one, re-insert one with its SAME value pointer}, so faults land on
//      trees reshaped by delete_fixup and every delete/overwrite also runs
//      on trees that survived a failed insert.
// Failure rule (both scenarios): only a new-key insert allocates, exactly
// twice (node, then key copy). With rel = allocations so far this run, the
// insert must fail iff rel < n <= rel+2 -- at the node if n == rel+1, at
// the key copy if n == rel+2 -- and must leave the tree exactly as it was
// (assert_unchanged) without consuming the caller's value (counting_free).
// Delete, overwrite and same-pointer re-insert must never allocate: each
// runs under a live fault, must free exactly the right value (by serial),
// and every value_free callback must see a fully consistent tree.
#include "rbtree.h"
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NKEYS 200
#define NDELETE 100
#define NOVERWRITE 50
#define NROUNDS 50
#define PER_ROUND 4          /* NROUNDS * PER_ROUND == NKEYS */
#define MAX_OPS 512
#define STRIDE 77            /* coprime to NKEYS, so perm() is a permutation */
#define MAX_N 100000L        /* termination guard */
#define EXPECTED_N (1L + 2L * NKEYS)

#define TVAL_MAGIC 0x7E57BEEFu
#define TVAL_POISON 0xDEADDEADu

/* ---- task 2: counting destructor ---- */
struct tval {
    uint32_t magic;
    int id;
    long serial;   /* unique per allocation, so "which value was freed" is checkable */
};

static long g_freed;             /* values the tree has released through value_free */
static long g_last_freed_serial; /* serial of the most recently released value */
static long g_bad_callbacks;     /* value_free calls that saw an inconsistent tree */
static const rbtree_t *g_check_tree;  /* set only around delete/overwrite calls */
static long g_next_serial;
static bool g_quiet;             /* silences assert_unchanged during its self-test */

static struct tval *tval_new(int id) {
    struct tval *v = malloc(sizeof *v);   /* plain malloc: must not shift n */
    if (v == NULL) {
        fprintf(stderr, "    test harness out of memory\n");
        exit(EXIT_FAILURE);
    }
    v->magic = TVAL_MAGIC;
    v->id = id;
    v->serial = g_next_serial++;
    return v;
}

static void tval_release(struct tval *v) {
    v->magic = TVAL_POISON;
    free(v);
}

static void counting_free(void *p) {
    struct tval *v = p;
    if (v == NULL || v->magic != TVAL_MAGIC) {
        fprintf(stderr, "    value_free got a value with bad magic (double free?)\n");
        abort();
    }
    /* the tree must already be consistent when it hands a value back */
    if (g_check_tree != NULL && rb_validate(g_check_tree) != 0) g_bad_callbacks++;
    g_last_freed_serial = v->serial;
    tval_release(v);
    g_freed++;
}

/* ---- reference model over the fixed key universe ---- */
struct model {
    struct tval *val[NKEYS];   /* NULL = key absent */
    size_t count;
};

static void key_of(int id, char buf[static 8]) {
    snprintf(buf, 8, "k%03d", id);
}

static int perm(int i) {
    return (int)(((long)i * STRIDE) % NKEYS);
}

static bool model_matches(const rbtree_t *t, const struct model *m) {
    bool ok = true;
    /* invariant: every id < i present in the model was found with its value */
    for (int i = 0; i < NKEYS; i++) {
        char key[8];
        key_of(i, key);
        void *got = rb_find(t, key);
        if (got != m->val[i]) {
            if (!g_quiet) {
                fprintf(stderr, "    rb_find(\"%s\") = %p, model has %p\n",
                        key, got, (void *)m->val[i]);
            }
            ok = false;
        }
    }
    return ok;
}

/* ---- task 1: "exactly as it was" after a failed operation ---- */
struct before {
    size_t size;
    void *find_failed;
    long freed;
};

static bool assert_unchanged(const rbtree_t *t, const struct model *m,
                             const struct before *b, const char *key,
                             const struct tval *caller_val) {
    bool ok = true;
    if (rb_validate(t) != 0) {
        if (!g_quiet) fprintf(stderr, "    rb_validate failed after failed op\n");
        ok = false;
    }
    if (rb_size(t) != b->size) {
        if (!g_quiet) fprintf(stderr, "    rb_size = %zu, was %zu\n", rb_size(t), b->size);
        ok = false;
    }
    void *found = rb_find(t, key);
    if (found != b->find_failed) {
        if (!g_quiet) {
            fprintf(stderr, "    rb_find(\"%s\") = %p, was %p\n",
                    key, found, b->find_failed);
        }
        ok = false;
    }
    if (!model_matches(t, m)) ok = false;
    if (g_freed != b->freed) {
        if (!g_quiet) {
            fprintf(stderr, "    value_free ran %ld time(s) during failed op\n",
                    g_freed - b->freed);
        }
        ok = false;
    }
    if (caller_val->magic != TVAL_MAGIC) {
        if (!g_quiet) fprintf(stderr, "    caller's value was released by the tree\n");
        ok = false;
    }
    return ok;
}

static struct before take_before(const rbtree_t *t, const char *key) {
    return (struct before){
        .size = rb_size(t), .find_failed = rb_find(t, key), .freed = g_freed,
    };
}

/* The helper must be able to fail: feed it a deliberately wrong snapshot. */
static bool self_test_assert_unchanged(void) {
    struct model m = {0};
    rbtree_t *t = rb_create(counting_free);
    if (t == NULL) {
        fprintf(stderr, "    self-test: rb_create failed\n");
        return false;
    }
    char key[8];
    key_of(0, key);
    m.val[0] = tval_new(0);
    m.count = 1;
    if (rb_insert(t, key, m.val[0]) != 0) {
        fprintf(stderr, "    self-test: rb_insert failed\n");
        rb_destroy(t);
        return false;
    }
    struct tval *probe = tval_new(-1);
    struct before right = take_before(t, key);
    struct before wrong = right;
    wrong.size++;
    g_quiet = true;
    bool accepts_right = assert_unchanged(t, &m, &right, key, probe);
    bool rejects_wrong = !assert_unchanged(t, &m, &wrong, key, probe);
    g_quiet = false;
    tval_release(probe);
    rb_destroy(t);
    g_freed = 0;
    if (!accepts_right || !rejects_wrong) {
        fprintf(stderr, "    self-test: assert_unchanged accepts_right=%d rejects_wrong=%d\n",
                accepts_right, rejects_wrong);
        return false;
    }
    return true;
}

/* ---- scenarios: fixed op lists over the key universe ---- */
enum op_kind { OP_INSERT, OP_DELETE, OP_OVERWRITE, OP_REINSERT_SAME };

struct op {
    enum op_kind kind;
    int id;
};

struct scenario {
    const char *name;
    struct op ops[MAX_OPS];
    size_t count;
};

static void push_op(struct scenario *s, enum op_kind kind, int id) {
    if (s->count >= MAX_OPS) {
        fprintf(stderr, "    scenario %s exceeds MAX_OPS\n", s->name);
        exit(EXIT_FAILURE);
    }
    s->ops[s->count++] = (struct op){.kind = kind, .id = id};
}

static void build_scenario_a(struct scenario *s) {
    s->name = "A: insert 200, delete 100, overwrite 50";
    s->count = 0;
    for (int i = 0; i < NKEYS; i++) push_op(s, OP_INSERT, perm(i));
    for (int j = 0; j < NDELETE; j++) push_op(s, OP_DELETE, perm(2 * j));
    for (int j = 0; j < NOVERWRITE; j++) push_op(s, OP_OVERWRITE, perm(2 * j + 1));
}

static void build_scenario_b(struct scenario *s) {
    s->name = "B: 50 interleaved rounds";
    s->count = 0;
    /* invariant: rounds < r inserted keys perm(0 .. PER_ROUND*r - 1) */
    for (int r = 0; r < NROUNDS; r++) {
        for (int k = 0; k < PER_ROUND; k++) push_op(s, OP_INSERT, perm(PER_ROUND * r + k));
        if (r == 0) continue;
        int prev = PER_ROUND * (r - 1);   /* first key of the previous round */
        push_op(s, OP_DELETE, perm(prev));
        push_op(s, OP_OVERWRITE, perm(prev + 1));
        push_op(s, OP_REINSERT_SAME, perm(prev + 2));
    }
}

/* ---- one scenario run with the n-th allocation armed ---- */
struct run_result {
    bool ok;
    bool fault_fired;
    long allocs;        /* rb_malloc calls this run made */
    int failures;       /* operations that reported allocation failure */
};

struct run_ctx {
    rbtree_t *t;
    struct model m;
    long n;
    long t0;
    struct run_result r;
};

/* A delete/overwrite/re-insert must not allocate, so it always runs under a
 * live fault: the run's own fault if still pending, else a fresh arm(1). */
static bool live_fault_begin(const struct run_ctx *c) {
    bool pending = fault_alloc_total() - c->t0 < c->n;
    if (!pending) fault_alloc_arm(1);
    return pending;
}

static void live_fault_end(bool pending) {
    if (!pending) fault_alloc_disarm();
}

static void fail(struct run_ctx *c) {
    c->r.ok = false;
}

/* Insert of a key absent from the model: the only operation that allocates. */
static void do_new_insert(struct run_ctx *c, int id, const char *key) {
    long rel = fault_alloc_total() - c->t0;
    bool expect_fail = rel < c->n && c->n <= rel + 2;
    const char *site = (c->n == rel + 1) ? "node" : "key-copy";
    struct tval *v = tval_new(id);
    struct before b = take_before(c->t, key);
    int rc = rb_insert(c->t, key, v);
    if (rc == 0) {
        if (expect_fail) {
            fprintf(stderr, "    n=%ld: insert \"%s\" succeeded, expected %s allocation "
                    "failure\n", c->n, key, site);
            fail(c);
        }
        c->m.val[id] = v;
        c->m.count++;
        return;
    }
    c->r.failures++;
    if (rc != -1 || !expect_fail) {
        fprintf(stderr, "    n=%ld: insert \"%s\" returned %d unexpectedly (rel=%ld)\n",
                c->n, key, rc, rel);
        fail(c);
    }
    if (!assert_unchanged(c->t, &c->m, &b, key, v)) {
        fprintf(stderr, "    n=%ld: tree not exactly as it was after failed insert "
                "\"%s\" (%s allocation)\n", c->n, key, site);
        fail(c);
    }
    tval_release(v);   /* caller still owns it; double free if the tree freed it */
}

static void do_delete(struct run_ctx *c, int id, const char *key) {
    struct tval *old = c->m.val[id];
    bool present = old != NULL;
    long old_serial = present ? old->serial : -1;
    long a0 = fault_alloc_total(), f0 = g_freed, b0 = g_bad_callbacks;
    g_last_freed_serial = -1;

    bool pending = live_fault_begin(c);
    g_check_tree = c->t;
    int rc = rb_delete(c->t, key);
    g_check_tree = NULL;
    live_fault_end(pending);

    if (fault_alloc_total() != a0) {
        fprintf(stderr, "    n=%ld: rb_delete(\"%s\") allocated\n", c->n, key);
        fail(c);
    }
    if (g_bad_callbacks != b0) {
        fprintf(stderr, "    n=%ld: rb_delete(\"%s\") ran value_free on an inconsistent "
                "tree\n", c->n, key);
        fail(c);
    }
    if (rc != (present ? 0 : -1) || g_freed != f0 + (present ? 1 : 0) ||
        (present && g_last_freed_serial != old_serial)) {
        fprintf(stderr, "    n=%ld: rb_delete(\"%s\") rc=%d freed=%ld freed-serial=%ld "
                "(want serial %ld, present=%d)\n", c->n, key, rc, g_freed - f0,
                g_last_freed_serial, old_serial, present);
        fail(c);
    }
    if (present) {
        c->m.val[id] = NULL;
        c->m.count--;
    }
}

static void do_overwrite(struct run_ctx *c, int id, const char *key) {
    struct tval *old = c->m.val[id];
    long old_serial = old->serial;
    struct tval *v = tval_new(id);
    long a0 = fault_alloc_total(), f0 = g_freed, b0 = g_bad_callbacks;
    g_last_freed_serial = -1;

    bool pending = live_fault_begin(c);
    g_check_tree = c->t;
    int rc = rb_insert(c->t, key, v);
    g_check_tree = NULL;
    live_fault_end(pending);

    if (rc != 0) {
        fprintf(stderr, "    n=%ld: overwrite of \"%s\" returned %d\n", c->n, key, rc);
        tval_release(v);
        fail(c);
        return;
    }
    if (fault_alloc_total() != a0 || g_freed != f0 + 1 ||
        g_last_freed_serial != old_serial || v->magic != TVAL_MAGIC ||
        g_bad_callbacks != b0) {
        fprintf(stderr, "    n=%ld: overwrite of \"%s\" allocs=%ld freed=%ld "
                "freed-serial=%ld (want 0, 1, %ld), new value intact=%d, "
                "consistent callback=%d\n", c->n, key, fault_alloc_total() - a0,
                g_freed - f0, g_last_freed_serial, old_serial,
                v->magic == TVAL_MAGIC, g_bad_callbacks == b0);
        fail(c);
    }
    c->m.val[id] = v;
}

static void do_reinsert_same(struct run_ctx *c, const char *key, struct tval *v) {
    long a0 = fault_alloc_total(), f0 = g_freed, b0 = g_bad_callbacks;

    bool pending = live_fault_begin(c);
    g_check_tree = c->t;
    int rc = rb_insert(c->t, key, v);
    g_check_tree = NULL;
    live_fault_end(pending);

    if (rc != 0 || fault_alloc_total() != a0 || g_freed != f0 ||
        rb_find(c->t, key) != v || g_bad_callbacks != b0) {
        fprintf(stderr, "    n=%ld: same-pointer re-insert of \"%s\" rc=%d allocs=%ld "
                "freed=%ld (want 0, 0, 0): the tree released a value it owns\n",
                c->n, key, rc, fault_alloc_total() - a0, g_freed - f0);
        fail(c);
    }
}

static struct run_result run_scenario(const struct scenario *s, long n) {
    struct run_ctx c = {.n = n, .r = {.ok = true}};
    c.t0 = fault_alloc_total();
    long freed0 = g_freed;
    fault_alloc_arm(n);

    c.t = rb_create(counting_free);
    if (c.t == NULL) {
        c.r.failures++;
        if (n != 1) {
            fprintf(stderr, "    n=%ld: rb_create failed, expected only at n=1\n", n);
            fail(&c);
        }
        if (g_freed != freed0) {
            fprintf(stderr, "    n=%ld: value_free ran during failed rb_create\n", n);
            fail(&c);
        }
        goto finish;
    }

    /* invariant: c.m mirrors the tree after every completed op */
    for (size_t i = 0; i < s->count; i++) {
        int id = s->ops[i].id;
        char key[8];
        key_of(id, key);
        struct tval *cur = c.m.val[id];
        switch (s->ops[i].kind) {
        case OP_INSERT:
            do_new_insert(&c, id, key);
            break;
        case OP_DELETE:
            do_delete(&c, id, key);
            break;
        case OP_OVERWRITE:
            /* a key whose insert failed earlier is absent: plain insert */
            if (cur != NULL) do_overwrite(&c, id, key);
            else do_new_insert(&c, id, key);
            break;
        case OP_REINSERT_SAME:
            if (cur != NULL) do_reinsert_same(&c, key, cur);
            break;
        }
    }

    if (rb_validate(c.t) != 0 || rb_size(c.t) != c.m.count || !model_matches(c.t, &c.m)) {
        fprintf(stderr, "    n=%ld: final tree disagrees with model (size %zu vs %zu)\n",
                n, rb_size(c.t), c.m.count);
        fail(&c);
    }
    long f0 = g_freed;
    rb_destroy(c.t);
    if (g_freed - f0 != (long)c.m.count) {
        fprintf(stderr, "    n=%ld: rb_destroy freed %ld values, model held %zu\n",
                n, g_freed - f0, c.m.count);
        fail(&c);
    }

finish:
    fault_alloc_disarm();
    c.r.allocs = fault_alloc_total() - c.t0;
    c.r.fault_fired = c.r.allocs >= n;
    if (c.r.failures != (c.r.fault_fired ? 1 : 0)) {
        fprintf(stderr, "    n=%ld: %d failed operation(s), fault fired=%d\n",
                n, c.r.failures, c.r.fault_fired);
        fail(&c);
    }
    return c.r;
}

/* Sweeps n = 1, 2, ... over one scenario; true iff every run passed, the
 * sweep terminated at the predicted N, and the clean run reproduces. */
static bool sweep(const struct scenario *s) {
    long n = 1;
    long bad = 0;
    struct run_result r;
    /* invariant: every n' < n fired its fault and (unless counted in bad)
     * left the tree exactly as it was */
    for (;;) {
        if (n > MAX_N) {
            fprintf(stderr, "FAIL [%s]: sweep did not terminate by n=%ld\n", s->name, MAX_N);
            return false;
        }
        r = run_scenario(s, n);
        if (!r.ok) bad++;
        if (!r.fault_fired) break;
        n++;
    }

    long total_n = r.allocs;
    struct run_result again = run_scenario(s, n);
    bool ok = true;
    if (total_n == 0) {
        fprintf(stderr, "FAIL [%s]: N == 0, injector not wired (rbtree.c bypasses "
                "rb_malloc)\n", s->name);
        ok = false;
    } else if (total_n != EXPECTED_N) {
        fprintf(stderr, "FAIL [%s]: N = %ld allocations, expected %ld\n",
                s->name, total_n, EXPECTED_N);
        ok = false;
    }
    if (again.fault_fired || again.allocs != total_n || !again.ok) {
        fprintf(stderr, "FAIL [%s]: clean run not reproducible (N=%ld, then %ld)\n",
                s->name, total_n, again.allocs);
        ok = false;
    }
    if (bad > 0) {
        fprintf(stderr, "FAIL [%s]: %ld of %ld runs violated the failure contract\n",
                s->name, bad, n);
        ok = false;
    }
    if (ok) {
        printf("fault sweep [%s]: N=%ld; n=1..%ld each failed at its predicted site and "
               "left the tree exactly as it was; clean at n=%ld (reproduced)\n",
               s->name, total_n, total_n, n);
    }
    return ok;
}

int main(void) {
    if (!self_test_assert_unchanged()) {
        fprintf(stderr, "FAIL: assert_unchanged self-test\n");
        return EXIT_FAILURE;
    }
    static struct scenario a, b;
    build_scenario_a(&a);
    build_scenario_b(&b);
    bool ok_a = sweep(&a);
    bool ok_b = sweep(&b);
    return (ok_a && ok_b) ? EXIT_SUCCESS : EXIT_FAILURE;
}
