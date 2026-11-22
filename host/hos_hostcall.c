/* Host calls: guest addresses that are host functions, for function pointers the host hands the game (NVN procs, for
   one). Each is a 16-byte slot from HOS_HOSTCALL_BASE with no memory behind it. rt/a64_rt.c's a64_lookup asks
   a64_lookup_host for an address no lifted function has, so a guest call through such a pointer (BLR, a vtable, a
   proc table) reaches the thunk for its slot, which calls the registered function with its user data. */
#include "hos.h"

#include <pthread.h>

#define MAX_HOSTCALLS 4096

static struct { HosHostFn fn; void *user; const char *name; } calls[MAX_HOSTCALLS];
static uint32_t ncalls;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t hos_hostcall(const char *name, HosHostFn fn, void *user) {
    pthread_mutex_lock(&lock);
    if (ncalls == MAX_HOSTCALLS) hos_fatal(NULL, "out of host calls (%s)", name);
    const uint32_t i = ncalls;
    calls[i].fn = fn, calls[i].user = user, calls[i].name = name;
    __atomic_store_n(&ncalls, i + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return HOS_HOSTCALL_BASE + 16ULL * i;
}

const char *hos_hostcall_name(uint64_t addr) {
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    return addr >= HOS_HOSTCALL_BASE && i < __atomic_load_n(&ncalls, __ATOMIC_ACQUIRE) ? calls[i].name : NULL;
}

static inline void dispatch(Ctx *c, uint32_t i) { calls[i].fn(c, calls[i].user); }

/* the slot of a thunk named by its six base-4 digits, read as an octal literal (0 ## digits): each octal digit is one
   base-4 digit (constant-folded) */
static inline uint32_t quad(uint32_t octal) {
    uint32_t i = 0;
    for (int k = 5; k >= 0; k--) i = i * 4 + ((octal >> (3 * k)) & 7);
    return i;
}

/* one thunk a slot (a64_lookup caches functions, not data, so each slot needs a function of its own) */
#define T1(n) static void hc_##n(Ctx *A64_NOESC c) { dispatch(c, quad(0##n)); }
#define T4(n) T1(n##0) T1(n##1) T1(n##2) T1(n##3)
#define T16(n) T4(n##0) T4(n##1) T4(n##2) T4(n##3)
#define T64(n) T16(n##0) T16(n##1) T16(n##2) T16(n##3)
#define T256(n) T64(n##0) T64(n##1) T64(n##2) T64(n##3)
#define T1024(n) T256(n##0) T256(n##1) T256(n##2) T256(n##3)
#define T4096 T1024(0) T1024(1) T1024(2) T1024(3)
/* (the thunks are hc_000000 .. hc_333333, in slot order, so the table lists slot i at index i) */
#define R1(n) hc_##n,
#define R4(n) R1(n##0) R1(n##1) R1(n##2) R1(n##3)
#define R16(n) R4(n##0) R4(n##1) R4(n##2) R4(n##3)
#define R64(n) R16(n##0) R16(n##1) R16(n##2) R16(n##3)
#define R256(n) R64(n##0) R64(n##1) R64(n##2) R64(n##3)
#define R1024(n) R256(n##0) R256(n##1) R256(n##2) R256(n##3)
T4096
static A64Fn const thunks[MAX_HOSTCALLS] = {R1024(0) R1024(1) R1024(2) R1024(3)};

A64Fn a64_lookup_host(uint64_t addr) {
    if (addr < HOS_HOSTCALL_BASE || addr & 15) return NULL;
    const uint64_t i = (addr - HOS_HOSTCALL_BASE) / 16;
    return i < __atomic_load_n(&ncalls, __ATOMIC_ACQUIRE) ? thunks[i] : NULL;
}
