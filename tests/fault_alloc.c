// Purpose: Test-build fault injector behind the rb_malloc/rb_free seam.
// Counts every rb_malloc call (including the one it fails) and, when
// armed with n, makes the n-th allocation from that point return NULL.
// The fault is one-shot: it disarms itself after firing, so later
// allocations in the same run succeed.
#include "fault_alloc.h"

#include <stdlib.h>

static long total;      /* rb_malloc calls observed so far */
static long countdown;  /* 0 = disarmed; otherwise allocations left until the fault */

void *rb_malloc(size_t n) {
    total++;
    if (countdown > 0 && --countdown == 0) return NULL;
    return malloc(n);
}

void rb_free(void *p) {
    free(p);
}

void fault_alloc_arm(long n) {
    countdown = (n > 0) ? n : 0;
}

void fault_alloc_disarm(void) {
    countdown = 0;
}

long fault_alloc_total(void) {
    return total;
}
