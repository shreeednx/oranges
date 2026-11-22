/* Logging, fatal stops, traps and the generated stubs' first-call notes. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int hos_trace;

int64_t hos_now_ns(void) {
    static int64_t base;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    int64_t b = __atomic_load_n(&base, __ATOMIC_RELAXED);
    if (!b && !__atomic_compare_exchange_n(&base, &b, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return ns - b;
    return b ? ns - b : 0;
}

void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[hos] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

void hos_dump(Ctx *c) {
    for (int i = 0; i < 32; i += 4)
        hos_log("  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  %s %016llx", i, (unsigned long long)c->x[i],
                i + 1, (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2],
                i + 3 == 31 ? "sp " : i + 3 < 10 ? (const char *[]){"x3 ", "x7 "}[i / 4] :
                (const char *[]){"", "", "x11", "x15", "x19", "x23", "x27"}[i / 4], (unsigned long long)c->x[i + 3]);
    hos_log("  tpidrro_el0 %016llx  tpidr_el0 %016llx  thread %#x", (unsigned long long)c->tpidrro_el0,
            (unsigned long long)c->tpidr_el0, c->tpidrro_el0 ? G32(c->tpidrro_el0 + 0x110) : 0);
}

extern const uint32_t hos_symbol_count, hos_symbol_addr[];   /* (gen/hos_symbols.c) */
extern const char *const hos_symbol_name[];

const char *hos_symbol(uint64_t addr, uint64_t *offset) {
    uint32_t lo = 0, hi = hos_symbol_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (hos_symbol_addr[mid] <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || addr >> 32) return NULL;
    *offset = addr - hos_symbol_addr[lo - 1];
    return hos_symbol_name[lo - 1];
}

static void frame_line(int i, uint64_t pc) {
    uint64_t off = 0;
    const char *name = hos_symbol(pc, &off);
    if (name && off < 0x100000) hos_log("  #%-2d %#llx  %s+%#llx", i, (unsigned long long)pc, name, (unsigned long long)off);
    else hos_log("  #%-2d %#llx", i, (unsigned long long)pc);
}

/* Lifted code keeps the guest's frame records ([x29] = the caller's x29, [x29 + 8] = the return address) in guest
   memory, as the hardware would, so the chain walks as it does on the console. */
void hos_backtrace(Ctx *c) {
    frame_line(0, c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 1; i < 32 && fp && !(fp & 7) && fp < HOS_WINDOW - 16; i++) {
        const uint64_t lr = G64(fp + 8), next = G64(fp);
        if (!lr) break;
        frame_line(i, lr);
        if (next <= fp) break;
        fp = next;
    }
}

void hos_fatal(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[hos] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (c) {
        hos_dump(c);
        hos_backtrace(c);
    }
    fflush(stderr);
    exit(70);
}

/* ---- native crashes: a signal in lifted or host code. The guest registers are those of the context the thread runs
   (hos_current), as last stored: lifted code keeps them in the context across calls. */
#include <signal.h>
#include <unistd.h>

__thread Ctx *hos_current;

static void on_signal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    const uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)A64_PTR(0);
    if (a >= base && a < base + HOS_WINDOW)
        hos_log("crash: signal %d at guest address %#llx (an unmapped page of the guest window)", sig,
                (unsigned long long)(a - base));
    else
        hos_log("crash: signal %d at host address %p", sig, si->si_addr);
    if (hos_current) {
        hos_dump(hos_current);
        hos_backtrace(hos_current);
    }
    fflush(stderr);
    _exit(71);
}

void hos_crash_init(void) {
    static uint8_t alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig = SIGILL; sig <= SIGSEGV; sig++)
        if (sig == SIGILL || sig == SIGBUS || sig == SIGFPE || sig == SIGSEGV) sigaction(sig, &sa, NULL);
}

/* rt/a64_rt.c's weak hook: a thread that cannot go on (a trap, a branch to an address with no function) */
void a64_on_trap(Ctx *c, uint64_t pc, uint32_t insn, const char *why) {
    hos_fatal(c, "%s at %#llx (insn %08x), lr %#llx", why, (unsigned long long)pc, insn, (unsigned long long)c->x[30]);
}

/* gen/a64_stubs.c: an OS-facing SDK function, or an import nothing exports, that the host does not implement yet.
   It returns 0, which is success for the nn::Result functions, and logs its first call (every call with HOS_TRACE). */
void hos_unimplemented(Ctx *A64_NOESC c, const char *sym, const char *name, int *seen) {
    (void)sym;
    if (!*seen || hos_trace)
        hos_log("stub: %s (x0 %#llx x1 %#llx x2 %#llx, lr %#llx)", name, (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
    *seen = 1;
    c->x[0] = 0;
}
