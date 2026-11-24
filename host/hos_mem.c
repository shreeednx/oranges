/* Guest memory: the 4 GB window, the record of what is mapped (for svcQueryMemory), the system region and the heap.

   The whole window is reserved at start (inaccessible) and ranges are committed read-write as the guest maps them
   (hos_port.c: mmap and mprotect, VirtualAlloc on Windows). Guest
   permissions are only recorded: lifted code never executes guest memory, and host-side protection would only turn
   the guest's own bugs into host crashes in other places. */
#include "hos.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000ULL

typedef struct Range { uint64_t start, size; int state, perm; } Range;
static Range *ranges;   /* sorted by start, never overlapping */
static int nranges, cap;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t hos_heap_size;
/* what svcGetInfo reports as the program's memory: the console's application pool, 3285 MB. The game sizes its heaps
   from it (its AppStaticHeap alone is 2015 MB, which a 3 GB total left no room for) */
uint64_t hos_total_memory = 0xCD500000ULL;
static uint64_t sys_next = HOS_SYSTEM_BASE;

static void remove_span(uint64_t start, uint64_t end) {
    for (int i = 0; i < nranges; i++) {
        Range *r = &ranges[i];
        const uint64_t rs = r->start, re = r->start + r->size;
        if (re <= start || rs >= end) continue;
        if (rs < start && re > end) {   /* split in two */
            if (nranges == cap) {
                cap = cap ? cap * 2 : 64;
                ranges = realloc(ranges, cap * sizeof *ranges);
                r = &ranges[i];
            }
            memmove(&ranges[i + 2], &ranges[i + 1], (nranges - i - 1) * sizeof *ranges);
            ranges[i + 1] = (Range){end, re - end, r->state, r->perm};
            r->size = start - rs;
            nranges++;
            return;
        }
        if (rs < start) { r->size = start - rs; continue; }
        if (re > end) { r->size = re - end; r->start = end; continue; }
        memmove(&ranges[i], &ranges[i + 1], (nranges - i - 1) * sizeof *ranges);
        nranges--;
        i--;
    }
}

static void insert_range(uint64_t start, uint64_t size, int state, int perm) {
    remove_span(start, start + size);
    if (nranges == cap) {
        cap = cap ? cap * 2 : 64;
        ranges = realloc(ranges, cap * sizeof *ranges);
    }
    int i = 0;
    while (i < nranges && ranges[i].start < start) i++;
    memmove(&ranges[i + 1], &ranges[i], (nranges - i) * sizeof *ranges);
    ranges[i] = (Range){start, size, state, perm};
    nranges++;
}

void hos_mem_init(void) {
    void *want = A64_PTR(0);
    if (hos_vm_reserve(want, HOS_WINDOW) != want)
        hos_fatal(NULL, "cannot reserve the 4 GB guest window at %p (%s)", want, strerror(errno));
}

int hos_map(uint64_t addr, uint64_t size, int state, int perm) {
    if ((addr | size) & (PAGE - 1) || !size || addr + size > HOS_WINDOW || addr + size < addr) return -1;
    if (hos_vm_commit(GP(addr), size)) return -1;
    pthread_mutex_lock(&lock);
    insert_range(addr, size, state, perm);
    pthread_mutex_unlock(&lock);
    return 0;
}

int hos_unmap(uint64_t addr, uint64_t size) {
    if ((addr | size) & (PAGE - 1) || !size || addr + size > HOS_WINDOW) return -1;
    hos_vm_decommit(GP(addr), size);   /* (the pages read as zeros when mapped again) */
    pthread_mutex_lock(&lock);
    remove_span(addr, addr + size);
    pthread_mutex_unlock(&lock);
    return 0;
}

void hos_set_perm(uint64_t addr, uint64_t size, int perm) {
    pthread_mutex_lock(&lock);
    const uint64_t end = addr + size;
    Range pieces[64];   /* (the mapped parts of [addr, end), with the new permission) */
    int n = 0;
    for (int i = 0; i < nranges && n < 64; i++) {
        const Range *r = &ranges[i];
        const uint64_t lo = r->start > addr ? r->start : addr;
        const uint64_t hi = r->start + r->size < end ? r->start + r->size : end;
        if (lo < hi) pieces[n++] = (Range){lo, hi - lo, r->state, perm};
    }
    for (int i = 0; i < n; i++) insert_range(pieces[i].start, pieces[i].size, pieces[i].state, perm);
    pthread_mutex_unlock(&lock);
}

void hos_query(uint64_t addr, uint64_t *start, uint64_t *size, int *state, int *perm) {
    pthread_mutex_lock(&lock);
    uint64_t gap_start = 0, gap_end = HOS_WINDOW;
    for (int i = 0; i < nranges; i++) {
        const Range *r = &ranges[i];
        if (addr >= r->start && addr < r->start + r->size) {
            *start = r->start, *size = r->size, *state = r->state, *perm = r->perm;
            pthread_mutex_unlock(&lock);
            return;
        }
        if (r->start + r->size <= addr) gap_start = r->start + r->size;
        else if (r->start > addr) { gap_end = r->start; break; }
    }
    pthread_mutex_unlock(&lock);
    if (addr >= HOS_WINDOW) {   /* above the window: one free block up to the end of the 39-bit space */
        gap_start = HOS_WINDOW, gap_end = 1ULL << 39;
    }
    *start = gap_start, *size = gap_end - gap_start, *state = HOS_MEM_FREE, *perm = HOS_PERM_NONE;
}

uint64_t hos_sys_alloc(uint64_t size, uint64_t align) {
    pthread_mutex_lock(&lock);
    const uint64_t a = (sys_next + align - 1) & ~(align - 1);
    const uint64_t end = (a + size + PAGE - 1) & ~(PAGE - 1);
    if (end > HOS_HOSTCALL_BASE) hos_fatal(NULL, "system region full (%#llx bytes asked)", (unsigned long long)size);
    sys_next = end;
    pthread_mutex_unlock(&lock);
    const uint64_t first = a & ~(PAGE - 1);
    if (hos_map(first, end - first, HOS_MEM_STATIC, HOS_PERM_RW))
        hos_fatal(NULL, "cannot commit the system region at %#llx", (unsigned long long)first);
    return a;
}

/* svcSetHeapSize: the heap grows or shrinks at HOS_HEAP_BASE in 2 MB steps */
int hos_set_heap_size(uint64_t size, uint64_t *addr) {
    if (size & 0x1FFFFF) return KERN_INVALID_SIZE;
    if (size > HOS_HEAP_MAX) return KERN_OUT_OF_MEMORY;
    if (size > hos_heap_size) {
        if (hos_map(HOS_HEAP_BASE + hos_heap_size, size - hos_heap_size, HOS_MEM_NORMAL, HOS_PERM_RW))
            return KERN_OUT_OF_MEMORY;
    } else if (size < hos_heap_size) {
        hos_unmap(HOS_HEAP_BASE + size, hos_heap_size - size);
    }
    hos_heap_size = size;
    *addr = HOS_HEAP_BASE;
    return 0;
}

/* HOS_CONTROL's MEM: every mapped writable range into <path> at its guest address (a sparse file: pages that are all
   zero, or not resident, are left out), so a debugger's search can run on the guest's memory while the game runs on.
   Not a snapshot: the other threads keep writing while it is copied. Returns the bytes written, or -1. */
int64_t hos_mem_dump(const char *path) {
    pthread_mutex_lock(&lock);
    Range *copy = malloc(sizeof *copy * (size_t)(nranges ? nranges : 1));
    const int n = nranges;
    memcpy(copy, ranges, sizeof *copy * (size_t)n);
    pthread_mutex_unlock(&lock);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return free(copy), -1;
    static const uint8_t zero[PAGE];
    int64_t written = 0;
    unsigned char *core = NULL;
    size_t core_cap = 0;
    for (int i = 0; i < n; i++) {
        const Range *r = &copy[i];
        if (!(r->perm & HOS_PERM_W) || r->state == HOS_MEM_IO) continue;
        const size_t pages = r->size / PAGE;
        if (pages > core_cap) core = realloc(core, core_cap = pages);
        if (hos_vm_resident(GP(r->start), r->size, core)) memset(core, 1, pages);
        for (size_t p = 0; p < pages; p++) {
            if (!(core[p] & 1)) continue;
            const uint64_t a = r->start + p * PAGE;
            if (!memcmp(GP(a), zero, PAGE)) continue;
            if (hos_pwrite(fd, GP(a), PAGE, (int64_t)a) != (int64_t)PAGE) { written = -1; goto out; }
            written += PAGE;
        }
    }
    if (hos_ftruncate(fd, HOS_WINDOW)) written = -1;
out:
    close(fd);
    free(core);
    free(copy);
    return written;
}
