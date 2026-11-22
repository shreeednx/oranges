/* setjmp in lifted code (rt/a64.h): the functions that call __setjmp are walked again from the call's return address,
   and their callers run through these. Not implemented yet: Longshot's host/hos_setjmp.c is the model. The game has 10
   setjmp owners (oranges lift's report). */
#include "hos.h"

void **recomp_sj_push(Ctx *A64_NOESC c, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3) {
    (void)r0; (void)r1; (void)r2; (void)r3;
    hos_fatal(c, "setjmp is not implemented yet");
}

uint64_t recomp_sj_target(void **slot) {
    (void)slot;
    hos_fatal(NULL, "setjmp is not implemented yet");
}

void recomp_sj_pop(void **slot) { (void)slot; }

__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot) {
    (void)slot;
    hos_fatal(NULL, "setjmp is not implemented yet");
}
