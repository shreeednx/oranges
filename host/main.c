/* oranges host: the native program the lifted game links into.

   For now only what linking needs: the Horizon OS layer (loading image.bin into the guest window, threads, SVCs, IPC
   services, NVN) is not written yet. a64-kit's runtime (rt/a64_rt.c) has weak defaults for a64_svc, a64_on_trap and
   a64_cntpct_host. What it leaves to the host is here. */
#include "a64.h"

#include <stdio.h>
#include <stdlib.h>

extern const uint32_t a64_fn_count;

/* setjmp in lifted code (rt/a64.h): the functions that call __setjmp are walked again from the call's return address,
   and their callers run through these. Longshot's host/hos_setjmp.c is the model for the real version. */
static void unsupported(const char *what) {
    fprintf(stderr, "oranges: %s is not implemented yet\n", what);
    abort();
}

void **recomp_sj_push(Ctx *A64_NOESC c, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3) {
    (void)c; (void)r0; (void)r1; (void)r2; (void)r3;
    unsupported("setjmp");
    return NULL;
}

uint64_t recomp_sj_target(void **slot) {
    (void)slot;
    unsupported("setjmp");
    return 0;
}

void recomp_sj_pop(void **slot) { (void)slot; }

__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot) {
    (void)slot;
    unsupported("setjmp");
    return 0;
}

int main(void) {
    printf("oranges: %u lifted functions linked. The Horizon OS layer is not written yet, so nothing runs.\n",
           a64_fn_count);
    return 0;
}
