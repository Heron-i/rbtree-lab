// Purpose: Allocation-failure fault sweep (M5, Mutation 1). For n = 1, 2,
// 3, ... re-runs one fixed, deterministic scenario (insert 200 keys,
// delete 100, overwrite 50) with the n-th rb_malloc failing, until a run
// completes without the fault ever firing. Every failed operation must
// leave the tree exactly as it was (assert_unchanged) and must not consume
// the caller's value (counting_free). Allocation map for this scenario:
// #1 = rb_create, then for the i-th insert (0-based) #(2i+2) = node,
// #(2i+3) = key copy, so N = 1 + 2*NKEYS and every failure site is known
// in advance. Delete and overwrite must not allocate at all.
#include "rbtree.h"
#include "fault_alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NKEYS 200
#define NDELETE 100
#define NOVERWRITE 50
#define STRIDE 77            /* coprime to NKEYS, so perm() is a permutation */
#define MAX_N 100000L        /* termination guard */
#define EXPECTED_N (1L + 2L * NKEYS)

#define TVAL_MAGIC 0x7E57BEEFu
#define TVAL_POISON 0xDEADDEADu

/* ---- task 2: counting destructor ---- */
struct tval {
    uint32_t magic;
    int id;
};

static long g_freed;   /* values the tree has released through value_free */
static bool g_quiet;   /* silences assert_unchanged during its self-test */

static struct tval *tval_new(int id) {
    struct tval *v = malloc(sizeof *v);   /* plain malloc: must not shift n */
    if (v == NULL) {
        fprintf(stderr, "    test harness out of memory\n");
        exit(EXIT_FAILURE);
    }
    v->magic = TVAL_MAGIC;
    v->id = id;
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

/* ---- task 3: one scenario run with the n-th allocation armed ---- */
struct run_result {
    bool ok;
    bool fault_fired;
    long allocs;        /* rb_malloc calls this run made */
    int failures;       /* operations that reported allocation failure */
};

/* Which operation the n-th allocation belongs to (see the file header). */
static void expected_site(long n, int *insert_idx, bool *is_key_copy) {
    *insert_idx = (int)((n - 2) / 2);
    *is_key_copy = (n % 2) == 1;
}

static struct run_result run_scenario(long n) {
    struct run_result r = {.ok = true};
    struct model m = {0};
    long t0 = fault_alloc_total();
    long freed0 = g_freed;
    fault_alloc_arm(n);

    rbtree_t *t = rb_create(counting_free);
    if (t == NULL) {
        r.failures++;
        if (n != 1) {
            fprintf(stderr, "    n=%ld: rb_create failed, expected only at n=1\n", n);
            r.ok = false;
        }
        if (g_freed != freed0) {
            fprintf(stderr, "    n=%ld: value_free ran during failed rb_create\n", n);
            r.ok = false;
        }
        goto finish;
    }

    /* insert phase. invariant: model holds exactly the keys whose insert
     * returned 0 so far */
    for (int i = 0; i < NKEYS; i++) {
        int id = perm(i);
        char key[8];
        key_of(id, key);
        struct tval *v = tval_new(id);
        struct before b = take_before(t, key);
        int rc = rb_insert(t, key, v);
        if (rc == 0) {
            m.val[id] = v;
            m.count++;
            continue;
        }
        r.failures++;
        int want_idx;
        bool want_key_copy;
        expected_site(n, &want_idx, &want_key_copy);
        if (rc != -1 || i != want_idx) {
            fprintf(stderr, "    n=%ld: insert #%d (\"%s\") returned %d, expected failure "
                    "only at insert #%d\n", n, i, key, rc, want_idx);
            r.ok = false;
        }
        if (!assert_unchanged(t, &m, &b, key, v)) {
            fprintf(stderr, "    n=%ld: tree not exactly as it was after failed "
                    "insert #%d (%s allocation)\n",
                    n, i, want_key_copy ? "key-copy" : "node");
            r.ok = false;
        }
        tval_release(v);   /* caller still owns it; double free if the tree freed it */
    }

    /* delete phase: must never allocate. invariant: model mirrors the tree */
    for (int j = 0; j < NDELETE; j++) {
        int id = perm(2 * j);
        char key[8];
        key_of(id, key);
        bool present = m.val[id] != NULL;
        long a0 = fault_alloc_total();
        long f0 = g_freed;
        int rc = rb_delete(t, key);
        if (fault_alloc_total() != a0) {
            fprintf(stderr, "    n=%ld: rb_delete(\"%s\") allocated\n", n, key);
            r.ok = false;
        }
        if (rc != (present ? 0 : -1) || g_freed != f0 + (present ? 1 : 0)) {
            fprintf(stderr, "    n=%ld: rb_delete(\"%s\") rc=%d freed=%ld (present=%d)\n",
                    n, key, rc, g_freed - f0, present);
            r.ok = false;
        }
        if (present) {
            m.val[id] = NULL;
            m.count--;
        }
    }

    /* overwrite phase: an overwrite must never allocate. A key whose insert
     * failed earlier is absent, so its "overwrite" is a plain insert.
     * invariant: model mirrors the tree */
    for (int j = 0; j < NOVERWRITE; j++) {
        int id = perm(2 * j + 1);
        char key[8];
        key_of(id, key);
        bool present = m.val[id] != NULL;
        struct tval *v = tval_new(id);
        long a0 = fault_alloc_total();
        long f0 = g_freed;
        int rc = rb_insert(t, key, v);
        if (rc != 0) {
            fprintf(stderr, "    n=%ld: overwrite of \"%s\" returned %d\n", n, key, rc);
            tval_release(v);
            r.ok = false;
            continue;
        }
        if (present && (fault_alloc_total() != a0 || g_freed != f0 + 1)) {
            fprintf(stderr, "    n=%ld: overwrite of \"%s\" allocs=%ld freed=%ld "
                    "(want 0 and 1)\n", n, key, fault_alloc_total() - a0, g_freed - f0);
            r.ok = false;
        }
        if (!present) m.count++;
        m.val[id] = v;
    }

    if (rb_validate(t) != 0 || rb_size(t) != m.count || !model_matches(t, &m)) {
        fprintf(stderr, "    n=%ld: final tree disagrees with model (size %zu vs %zu)\n",
                n, rb_size(t), m.count);
        r.ok = false;
    }
    long f0 = g_freed;
    rb_destroy(t);
    if (g_freed - f0 != (long)m.count) {
        fprintf(stderr, "    n=%ld: rb_destroy freed %ld values, model held %zu\n",
                n, g_freed - f0, m.count);
        r.ok = false;
    }

finish:
    fault_alloc_disarm();
    r.allocs = fault_alloc_total() - t0;
    r.fault_fired = r.allocs >= n;
    if (r.failures != (r.fault_fired ? 1 : 0)) {
        fprintf(stderr, "    n=%ld: %d failed operation(s), fault fired=%d\n",
                n, r.failures, r.fault_fired);
        r.ok = false;
    }
    return r;
}

int main(void) {
    if (!self_test_assert_unchanged()) {
        fprintf(stderr, "FAIL: assert_unchanged self-test\n");
        return EXIT_FAILURE;
    }

    long n = 1;
    long bad = 0;
    struct run_result r;
    /* invariant: every n' < n fired its fault and (unless counted in bad)
     * left the tree exactly as it was */
    for (;;) {
        if (n > MAX_N) {
            fprintf(stderr, "FAIL: sweep did not terminate by n=%ld\n", MAX_N);
            return EXIT_FAILURE;
        }
        r = run_scenario(n);
        if (!r.ok) bad++;
        if (!r.fault_fired) break;
        n++;
    }

    long total_n = r.allocs;
    struct run_result again = run_scenario(n);
    bool ok = true;
    if (total_n == 0) {
        fprintf(stderr, "FAIL: N == 0, injector not wired (rbtree.c bypasses rb_malloc)\n");
        ok = false;
    } else if (total_n != EXPECTED_N) {
        fprintf(stderr, "FAIL: N = %ld allocations, expected %ld\n", total_n, EXPECTED_N);
        ok = false;
    }
    if (again.fault_fired || again.allocs != total_n || !again.ok) {
        fprintf(stderr, "FAIL: clean run not reproducible (N=%ld, then %ld)\n",
                total_n, again.allocs);
        ok = false;
    }
    if (bad > 0) {
        fprintf(stderr, "FAIL: %ld of %ld runs violated the failure contract\n", bad, n);
        ok = false;
    }
    if (!ok) return EXIT_FAILURE;

    printf("fault sweep: N=%ld; n=1..%ld each failed at its predicted site and left "
           "the tree exactly as it was; clean at n=%ld (reproduced)\n",
           total_n, total_n, n);
    return EXIT_SUCCESS;
}
