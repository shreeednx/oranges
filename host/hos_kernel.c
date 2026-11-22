/* The kernel's threads, handles and synchronization, for the SDK's nn::os, which runs lifted over these SVCs.

   Each guest thread is a host thread with its own guest context and thread-local region. Its entry is the SDK's own
   thread entry (nn::os), which sets up the thread's TLS itself, as on the console.

   Synchronization follows Horizon's semantics, under one kernel lock (klock) with one condition variable (kwake)
   that every state change broadcasts. Simple and correct; a waiter re-checks its own condition when woken.
     - mutexes (ArbitrateLock/Unlock): the guest word holds the owner's handle, with bit 30 set while others wait. The
       kernel hands the mutex to the waiter it wakes.
     - condition variables (WaitProcessWideKeyAtomic / SignalProcessWideKey): the wait releases the mutex, and a woken
       waiter takes it back before it returns (or queues for it).
     - address arbiters (WaitForAddress / SignalToAddress), events, and WaitSynchronization on events and threads. */
#include "hos.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CUR_THREAD 0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u
#define MUTEX_WAITERS 0x40000000u
#define KERN_CANCELLED HOS_RESULT(1, 118)
#define KERN_INVALID_CURRENT_MEMORY HOS_RESULT(1, 106)
#define KERN_TERMINATION_REQUESTED HOS_RESULT(1, 59)

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kwake;   /* (CLOCK_MONOTONIC, set up in hos_kernel_init) */

typedef struct KThread {
    Ctx ctx;
    uint32_t handle;
    uint64_t entry, arg, stack_top, tlr;
    int priority, core, started, exited;
    pthread_t host;
    /* what the thread waits for, if anything */
    enum { W_NONE, W_MUTEX, W_CONDVAR, W_ARBITER } wait;
    uint64_t wait_addr, wait_key;   /* the mutex word; the condition variable's key */
    int woken, cancel;              /* handed the mutex / signalled; CancelSynchronization pending */
    uint64_t seq;                   /* when it began waiting (FIFO among equals) */
} KThread;

enum { H_FREE = HOS_H_FREE, H_THREAD = HOS_H_THREAD, H_EVENT = HOS_H_EVENT };
#define MAX_HANDLES 4096
static struct { int kind; void *obj; uint64_t a, b, c; } handles[MAX_HANDLES];
typedef struct { int signaled, autoclear; } KEvent;

#define MAX_THREADS 256
static KThread *threads[MAX_THREADS];
static int nthreads;
static uint64_t wait_seq;
static __thread KThread *self;

void hos_kernel_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&kwake, &a);
}

/* ---- handles */

uint32_t hos_handle_new(int kind, void *obj, uint64_t a, uint64_t b, uint64_t c) {
    pthread_mutex_lock(&klock);
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (handles[i].kind == H_FREE) {
            handles[i].kind = kind, handles[i].obj = obj, handles[i].a = a, handles[i].b = b, handles[i].c = c;
            pthread_mutex_unlock(&klock);
            return 0x1000u | i;
        }
    pthread_mutex_unlock(&klock);
    hos_fatal(NULL, "out of handles");
}

/* (under klock) the handle's slot, or -1 */
static int slot_of(uint32_t h) {
    if (h == CUR_THREAD && self) h = self->handle;
    const uint32_t i = h & 0xFFF;
    return (h & ~0xFFFu) == 0x1000u && i && i < MAX_HANDLES && handles[i].kind != H_FREE ? (int)i : -1;
}

static KThread *thread_of(uint32_t h) {
    const int i = slot_of(h);
    return i >= 0 && handles[i].kind == H_THREAD ? handles[i].obj : NULL;
}

/* ---- waiting, under klock */

static int deadline_of(int64_t ns, struct timespec *ts) {
    if (ns < 0) return 0;
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ns / 1000000000;
    ts->tv_nsec += ns % 1000000000;
    if (ts->tv_nsec >= 1000000000) ts->tv_sec++, ts->tv_nsec -= 1000000000;
    return 1;
}

/* wait for a broadcast: 0, or ETIMEDOUT once the deadline passed */
static int kwait(int timed, const struct timespec *ts) {
    return timed ? pthread_cond_timedwait(&kwake, &klock, ts) : pthread_cond_wait(&kwake, &klock);
}

static KThread *best_waiter(int kind, uint64_t addr) {   /* highest priority (lowest number), then the earliest */
    KThread *best = NULL;
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if (t->wait != kind || t->woken || (kind == W_CONDVAR ? t->wait_key : t->wait_addr) != addr) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->seq < best->seq)) best = t;
    }
    return best;
}

static int has_waiter(int kind, uint64_t addr) { return best_waiter(kind, addr) != NULL; }

/* hand the mutex at addr to its next waiter, or free it */
static void mutex_release(uint64_t addr) {
    KThread *next = best_waiter(W_MUTEX, addr);
    if (!next) {
        __atomic_store_n((uint32_t *)GP(addr), 0, __ATOMIC_RELEASE);
        return;
    }
    next->woken = 1;
    next->wait = W_NONE;
    __atomic_store_n((uint32_t *)GP(addr), next->handle | (has_waiter(W_MUTEX, addr) ? MUTEX_WAITERS : 0),
                     __ATOMIC_RELEASE);
    pthread_cond_broadcast(&kwake);
}

/* take the mutex at addr for t, or queue t for it (t then waits until it is handed over) */
static int mutex_take_or_queue(KThread *t, uint64_t addr) {
    uint32_t *word = GP(addr);
    uint32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!cur) {
            if (__atomic_compare_exchange_n(word, &cur, t->handle, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
            continue;
        }
        if (__atomic_compare_exchange_n(word, &cur, cur | MUTEX_WAITERS, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    t->wait = W_MUTEX, t->wait_addr = addr, t->woken = 0, t->seq = ++wait_seq;
    return 0;
}

/* ---- threads */

static void *thread_main(void *p) {
    KThread *t = p;
    self = t;
    hos_current = &t->ctx;
    Ctx *c = &t->ctx;
    c->x[0] = t->arg;
    c->x[30] = 0;
    c->x[31] = t->stack_top & ~15ULL;
    A64Fn f = a64_lookup(t->entry);
    if (!f) hos_fatal(c, "thread %#x: entry %#llx is not a function", t->handle, (unsigned long long)t->entry);
    if (hos_trace) hos_log("thread %#x: start at %#llx", t->handle, (unsigned long long)t->entry);
    f(c);
    hos_log("thread %#x: its entry returned", t->handle);
    pthread_mutex_lock(&klock);
    t->exited = 1;
    pthread_cond_broadcast(&kwake);
    pthread_mutex_unlock(&klock);
    return NULL;
}

static KThread *thread_new(uint64_t entry, uint64_t arg, uint64_t stack_top, int priority, int core) {
    KThread *t = calloc(1, sizeof *t);
    t->entry = entry, t->arg = arg, t->stack_top = stack_top, t->priority = priority, t->core = core;
    t->tlr = hos_sys_alloc(HOS_TLS_SIZE, HOS_TLS_SIZE);
    t->handle = hos_handle_new(H_THREAD, t, 0, 0, 0);
    G32(t->tlr + 0x110) = t->handle;
    t->ctx.tpidrro_el0 = t->tlr;
    t->ctx.thread = t;
    pthread_mutex_lock(&klock);
    if (nthreads == MAX_THREADS) hos_fatal(NULL, "too many threads");
    threads[nthreads++] = t;
    pthread_mutex_unlock(&klock);
    return t;
}

/* the main thread, which hos_main.c runs on the process's own host thread */
Ctx *hos_kernel_main_thread(uint64_t stack_top, int priority) {
    KThread *t = thread_new(0, 0, stack_top, priority, 0);
    t->started = 1;
    t->host = pthread_self();
    t->ctx.x[31] = stack_top & ~15ULL;
    self = t;
    hos_current = &t->ctx;
    return &t->ctx;
}

uint64_t hos_main_thread_handle(void) { return threads[0] ? threads[0]->handle : 0; }

/* ---- the SVCs: 1 when handled */

int hos_kernel_svc(Ctx *A64_NOESC c, uint32_t imm) {
    switch (imm) {
    case 0x08: {   /* CreateThread(-, entry, arg, stack top, priority, core) -> handle */
        const int core = (int32_t)c->x[5];
        KThread *t = thread_new(c->x[1], c->x[2], c->x[3], (int)c->x[4], core);
        if (hos_trace) hos_log("thread %#x: created, entry %#llx arg %#llx stack %#llx prio %d", t->handle,
                               (unsigned long long)t->entry, (unsigned long long)t->arg,
                               (unsigned long long)t->stack_top, t->priority);
        c->x[0] = 0, c->x[1] = t->handle;
        return 1;
    }
    case 0x09: {   /* StartThread(handle) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        const int ok = t && !t->started;
        if (ok) t->started = 1;
        pthread_mutex_unlock(&klock);
        if (!ok) { c->x[0] = KERN_INVALID_HANDLE; return 1; }
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 8 << 20);   /* (host frames of lifted code: deep guest call chains) */
        if (pthread_create(&t->host, &a, thread_main, t)) hos_fatal(c, "cannot start a host thread");
        pthread_attr_destroy(&a);
        c->x[0] = 0;
        return 1;
    }
    case 0x0A:     /* ExitThread */
        pthread_mutex_lock(&klock);
        if (self) self->exited = 1;
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        if (self == threads[0]) hos_fatal(c, "ExitThread on the main thread");
        pthread_exit(NULL);
    case 0x0B: {   /* SleepThread(ns): 0, -1 and -2 are yields */
        const int64_t ns = (int64_t)c->x[0];
        if (ns > 0) {
            struct timespec ts = {ns / 1000000000, ns % 1000000000};
            while (nanosleep(&ts, &ts) && errno == EINTR) {}
        } else {
            sched_yield();
        }
        return 1;
    }
    case 0x0C: {   /* GetThreadPriority(-, handle) -> priority */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[1]);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE, c->x[1] = t ? (uint64_t)t->priority : 0;
        pthread_mutex_unlock(&klock);
        return 1;
    }
    case 0x0D: {   /* SetThreadPriority(handle, priority) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        if (t) t->priority = (int)c->x[1];
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x0E: c->x[0] = 0, c->x[1] = 0, c->x[2] = 0x7; return 1;   /* GetThreadCoreMask */
    case 0x0F: c->x[0] = 0; return 1;                               /* SetThreadCoreMask */
    case 0x10: c->x[0] = 0; return 1;                               /* GetCurrentProcessorNumber */
    case 0x45: {   /* CreateEvent -> writable, readable (one event behind both) */
        KEvent *e = calloc(1, sizeof *e);
        c->x[1] = hos_handle_new(H_EVENT, e, 0, 0, 0);
        c->x[2] = hos_handle_new(H_EVENT, e, 0, 0, 0);
        c->x[0] = 0;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   /* SignalEvent, ClearEvent, ResetSignal(handle) */
        pthread_mutex_lock(&klock);
        const int i = slot_of((uint32_t)c->x[0]);
        if (i >= 0 && handles[i].kind == H_EVENT) {
            ((KEvent *)handles[i].obj)->signaled = imm == 0x11;
            pthread_cond_broadcast(&kwake);
        }
        pthread_mutex_unlock(&klock);
        c->x[0] = i >= 0 ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x16: {   /* CloseHandle */
        pthread_mutex_lock(&klock);
        const int i = slot_of((uint32_t)c->x[0]);
        if (i >= 0 && handles[i].kind != H_THREAD) handles[i].kind = H_FREE;   /* (thread objects stay: they run) */
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x18: {   /* WaitSynchronization(-, handles, count, timeout) -> index */
        const uint32_t *hs = GP(c->x[1]);
        const int n = (int)c->x[2];
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        for (;;) {
            for (int k = 0; k < n; k++) {
                const int i = slot_of(hs[k]);
                if (i < 0) { pthread_mutex_unlock(&klock); c->x[0] = KERN_INVALID_HANDLE; return 1; }
                int ready = 0;
                if (handles[i].kind == H_EVENT) {
                    KEvent *e = handles[i].obj;
                    ready = e->signaled;
                } else if (handles[i].kind == H_THREAD) {
                    ready = ((KThread *)handles[i].obj)->exited;
                }
                if (ready) { pthread_mutex_unlock(&klock); c->x[0] = 0, c->x[1] = (uint64_t)k; return 1; }
            }
            if (self && self->cancel) {
                self->cancel = 0;
                pthread_mutex_unlock(&klock);
                c->x[0] = KERN_CANCELLED;
                return 1;
            }
            if ((int64_t)c->x[3] == 0 || kwait(timed, &ts) == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&klock);
        c->x[0] = KERN_TIMED_OUT;
        return 1;
    }
    case 0x19: {   /* CancelSynchronization(thread) */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[0]);
        if (t) t->cancel = 1;
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE;
        return 1;
    }
    case 0x1A: {   /* ArbitrateLock(owner handle, mutex, requester handle) */
        const uint64_t addr = c->x[1];
        pthread_mutex_lock(&klock);
        const uint32_t cur = __atomic_load_n((uint32_t *)GP(addr), __ATOMIC_ACQUIRE);
        if (cur != ((uint32_t)c->x[0] | MUTEX_WAITERS)) {   /* (released meanwhile: the caller tries again) */
            pthread_mutex_unlock(&klock);
            c->x[0] = 0;
            return 1;
        }
        self->wait = W_MUTEX, self->wait_addr = addr, self->woken = 0, self->seq = ++wait_seq;
        while (!self->woken) kwait(0, NULL);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x1B:     /* ArbitrateUnlock(mutex) */
        pthread_mutex_lock(&klock);
        mutex_release(c->x[0]);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    case 0x1C: {   /* WaitProcessWideKeyAtomic(mutex, key, handle, timeout) */
        const uint64_t mutex = c->x[0], key = c->x[1];
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        mutex_release(mutex);
        __atomic_store_n((uint32_t *)GP(key), 1, __ATOMIC_RELEASE);
        self->wait = W_CONDVAR, self->wait_key = key, self->wait_addr = mutex, self->woken = 0, self->seq = ++wait_seq;
        uint32_t r = 0;
        while (!self->woken && self->wait == W_CONDVAR) {
            if ((int64_t)c->x[3] == 0 || kwait(timed, &ts) == ETIMEDOUT) {
                if (!self->woken && self->wait == W_CONDVAR) {   /* timed out: take the mutex back (or queue for it) */
                    self->wait = W_NONE;
                    r = KERN_TIMED_OUT;
                    if (!mutex_take_or_queue(self, mutex))
                        while (!self->woken) kwait(0, NULL);
                }
                break;
            }
        }
        while (self->wait == W_MUTEX && !self->woken) kwait(0, NULL);   /* (signalled, then queued for the mutex) */
        self->wait = W_NONE;
        pthread_mutex_unlock(&klock);
        c->x[0] = r;
        return 1;
    }
    case 0x1D: {   /* SignalProcessWideKey(key, count): -1 all */
        const uint64_t key = c->x[0];
        int n = (int32_t)c->x[1];
        pthread_mutex_lock(&klock);
        while (n != 0) {
            KThread *t = best_waiter(W_CONDVAR, key);
            if (!t) break;
            if (mutex_take_or_queue(t, t->wait_addr)) {   /* the mutex was free: it wakes owning it */
                t->wait = W_NONE;
                t->woken = 1;
            }
            if (n > 0) n--;
        }
        if (!has_waiter(W_CONDVAR, key)) __atomic_store_n((uint32_t *)GP(key), 0, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    case 0x25: {   /* GetThreadId(-, handle) -> id */
        pthread_mutex_lock(&klock);
        KThread *t = thread_of((uint32_t)c->x[1]);
        pthread_mutex_unlock(&klock);
        c->x[0] = t ? 0 : KERN_INVALID_HANDLE, c->x[1] = t ? t->handle : 0;
        return 1;
    }
    case 0x34: {   /* WaitForAddress(addr, type, value, timeout): 0 if less than, 1 decrement and wait if less than,
                      2 if equal */
        const uint64_t addr = c->x[0];
        const int type = (int)c->x[1];
        const int32_t value = (int32_t)c->x[2];
        int32_t *word = GP(addr);
        struct timespec ts;
        const int timed = deadline_of((int64_t)c->x[3], &ts);
        pthread_mutex_lock(&klock);
        const int32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        const int waits = type == 2 ? cur == value : cur < value;
        if (!waits) { pthread_mutex_unlock(&klock); c->x[0] = KERN_INVALID_STATE; return 1; }
        if (type == 1) __atomic_store_n(word, cur - 1, __ATOMIC_RELEASE);
        if ((int64_t)c->x[3] == 0) { pthread_mutex_unlock(&klock); c->x[0] = KERN_TIMED_OUT; return 1; }
        self->wait = W_ARBITER, self->wait_addr = addr, self->woken = 0, self->seq = ++wait_seq;
        uint32_t r = 0;
        while (!self->woken) {
            if (self->cancel) { self->cancel = 0; r = KERN_CANCELLED; break; }
            if (kwait(timed, &ts) == ETIMEDOUT && !self->woken) { r = KERN_TIMED_OUT; break; }
        }
        self->wait = W_NONE;
        pthread_mutex_unlock(&klock);
        c->x[0] = r;
        return 1;
    }
    case 0x35: {   /* SignalToAddress(addr, type, value, count): 0 signal, 1 increment if equal, 2 modify by waiters if
                      equal */
        const uint64_t addr = c->x[0];
        const int type = (int)c->x[1];
        const int32_t value = (int32_t)c->x[2];
        int count = (int32_t)c->x[3];
        int32_t *word = GP(addr);
        pthread_mutex_lock(&klock);
        if (type) {
            const int32_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
            if (cur != value) { pthread_mutex_unlock(&klock); c->x[0] = KERN_INVALID_STATE; return 1; }
            if (type == 1) {
                __atomic_store_n(word, value + 1, __ATOMIC_RELEASE);
            } else {   /* by how many wait (the kernel's rule as Atmosphère's KAddressArbiter has it; to check against
                          its source): none wait -> value + 1; count <= 0 (all) -> value - 2; all of them woken ->
                          value - 1; some left waiting -> value */
                int waiting = 0;
                for (int i = 0; i < nthreads; i++)
                    waiting += threads[i]->wait == W_ARBITER && !threads[i]->woken && threads[i]->wait_addr == addr;
                const int32_t next = !waiting ? value + 1 : count <= 0 ? value - 2 : waiting <= count ? value - 1 : value;
                __atomic_store_n(word, next, __ATOMIC_RELEASE);
            }
        }
        while (count != 0) {
            KThread *t = best_waiter(W_ARBITER, addr);
            if (!t) break;
            t->woken = 1;
            if (count > 0) count--;
        }
        pthread_cond_broadcast(&kwake);
        pthread_mutex_unlock(&klock);
        c->x[0] = 0;
        return 1;
    }
    default:
        return 0;
    }
}
