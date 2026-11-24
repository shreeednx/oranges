/* Portability: the Linux and Windows forms of what hos_port.h declares (its header comment has the why). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* (gettid, MAP_FIXED_NOREPLACE) */
#endif
#include "hos_port.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <mmsystem.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2   /* (Windows 10 1803 on) */
#endif
#else
#include <sys/mman.h>
#endif

#ifdef _WIN32

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void hos_port_init(void) {
    timeBeginPeriod(1);   /* (Sleep, and the condition variable waits winpthreads builds on, tick at 15.6 ms otherwise) */
    SetConsoleOutputCP(CP_UTF8);
}

/* nanosleep is Sleep() in whole milliseconds on winpthreads: a high-resolution waitable timer instead, one per
   thread (kept for the thread's life) */
void hos_sleep_ns(int64_t ns) {
    static __thread HANDLE timer;
    static __thread int tried;
    if (ns <= 0) { SwitchToThread(); return; }
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);   /* (relative, in 100 ns) */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

void hos_cond_init_monotonic(pthread_cond_t *cv) { pthread_cond_init(cv, NULL); }

/* winpthreads takes a deadline as wall-clock time and waits whole milliseconds: the time left, as a wall-clock
   deadline; a wait that ends before the monotonic deadline is a spurious wakeup (the callers wait again) */
int hos_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *m, const struct timespec *deadline) {
    const int64_t left = ((int64_t)deadline->tv_sec * 1000000000 + deadline->tv_nsec) - mono_ns();
    if (left <= 0) return ETIMEDOUT;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    const int64_t ns = until.tv_nsec + left % 1000000000 + 999999;   /* (rounded up to the next millisecond) */
    until.tv_sec += left / 1000000000 + ns / 1000000000;
    until.tv_nsec = ns % 1000000000;
    const int r = pthread_cond_timedwait(cv, m, &until);
    if (r == ETIMEDOUT && mono_ns() < (int64_t)deadline->tv_sec * 1000000000 + deadline->tv_nsec) return 0;
    return r;
}

struct tm *hos_localtime(time_t t, struct tm *out, int32_t *gmtoff, char zone[8]) {
    if (localtime_s(out, &t)) return NULL;
    struct tm copy = *out;
    const int32_t off = (int32_t)(_mkgmtime(&copy) - t);
    if (gmtoff) *gmtoff = off;
    if (zone) {   /* (Windows names zones in words, "W. Europe Standard Time": the offset, as tzdata does unnamed ones) */
        const int a = off < 0 ? -off : off;
        if (a % 3600) snprintf(zone, 8, "%c%02d%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        else snprintf(zone, 8, "%c%02d", off < 0 ? '-' : '+', a / 3600);
    }
    return out;
}

struct tm *hos_gmtime(time_t t, struct tm *out) { return gmtime_s(out, &t) ? NULL : out; }
time_t hos_timegm(struct tm *t) { return _mkgmtime(t); }

int hos_thread_id(void) { return (int)GetCurrentThreadId(); }

void *hos_vm_reserve(void *at, uint64_t size) {
    void *p = VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p && p != at) VirtualFree(p, 0, MEM_RELEASE), p = NULL;
    return p;
}

int hos_vm_commit(void *p, uint64_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1; }
void hos_vm_decommit(void *p, uint64_t size) { VirtualFree(p, size, MEM_DECOMMIT); }
int hos_vm_resident(void *p, uint64_t size, unsigned char *pages) { (void)p, (void)size, (void)pages; return -1; }

int hos_exe_path(char *out, size_t cap) {
    wchar_t w[32768];
    const DWORD n = GetModuleFileNameW(NULL, w, sizeof w / sizeof w[0]);
    if (!n || n >= sizeof w / sizeof w[0]) return -1;
    w[n] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0 ? 0 : -1;
}

int hos_mkdir(const char *path) { return mkdir(path); }

int64_t hos_pread(int fd, void *buf, size_t n, int64_t offset) {
    const HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)offset, o.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!ReadFile(h, buf, n > 0x40000000 ? 0x40000000 : (DWORD)n, &got, &o)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return got;
}

int64_t hos_pwrite(int fd, const void *buf, size_t n, int64_t offset) {
    const HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)offset, o.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!WriteFile(h, buf, n > 0x40000000 ? 0x40000000 : (DWORD)n, &put, &o)) {
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return put;
}

int hos_ftruncate(int fd, int64_t size) { return _chsize_s(fd, size) ? -1 : 0; }

int hos_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    const DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_ACCESS_DENIED ? EACCES : e == ERROR_ALREADY_EXISTS || e == ERROR_DIR_NOT_EMPTY ? EEXIST : EIO;
    return -1;
}

static void pipe_name(const char *path, char *out, size_t cap) {
    if (!strncmp(path, "\\\\.\\pipe\\", 9)) { snprintf(out, cap, "%s", path); return; }
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    snprintf(out, cap, "\\\\.\\pipe\\%s", *name ? name : "oranges");
}

int hos_control_make(const char *path, char *shown, size_t cap) {
    pipe_name(path, shown, cap);
    return 0;
}

void *hos_control_open(const char *path) {
    char name[512];
    pipe_name(path, name, sizeof name);
    const HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0,
                                      65536, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(h); return NULL; }
    const int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_TEXT);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE *f = _fdopen(fd, "r");   /* (reads end when the writer closes: ERROR_BROKEN_PIPE is the end of the file) */
    if (!f) _close(fd);
    return f;
}

#else

void hos_port_init(void) {}

void hos_sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

void hos_cond_init_monotonic(pthread_cond_t *cv) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
}

int hos_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *m, const struct timespec *deadline) {
    return pthread_cond_timedwait(cv, m, deadline);
}

struct tm *hos_localtime(time_t t, struct tm *out, int32_t *gmtoff, char zone[8]) {
    if (!localtime_r(&t, out)) return NULL;
    if (gmtoff) *gmtoff = (int32_t)out->tm_gmtoff;
    if (zone) snprintf(zone, 8, "%s", out->tm_zone ? out->tm_zone : "UTC");
    return out;
}

struct tm *hos_gmtime(time_t t, struct tm *out) { return gmtime_r(&t, out); }
time_t hos_timegm(struct tm *t) { return timegm(t); }

int hos_thread_id(void) { return (int)gettid(); }

void *hos_vm_reserve(void *at, uint64_t size) {
    void *p = mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && p != at) munmap(p, size);
    return p == at ? p : NULL;
}

int hos_vm_commit(void *p, uint64_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) ? -1 : 0; }

void hos_vm_decommit(void *p, uint64_t size) {
    madvise(p, size, MADV_DONTNEED);   /* (the pages read as zeros when committed again) */
    mprotect(p, size, PROT_NONE);
}

int hos_vm_resident(void *p, uint64_t size, unsigned char *pages) { return mincore(p, size, pages); }

int hos_exe_path(char *out, size_t cap) {
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return -1;
    out[n] = 0;
    return 0;
}

int hos_mkdir(const char *path) { return mkdir(path, 0755); }
int64_t hos_pread(int fd, void *buf, size_t n, int64_t offset) { return pread(fd, buf, n, (off_t)offset); }
int64_t hos_pwrite(int fd, const void *buf, size_t n, int64_t offset) { return pwrite(fd, buf, n, (off_t)offset); }
int hos_ftruncate(int fd, int64_t size) { return ftruncate(fd, (off_t)size); }
int hos_rename(const char *from, const char *to) { return rename(from, to); }

int hos_control_make(const char *path, char *shown, size_t cap) {
    snprintf(shown, cap, "%s", path);
    struct stat st;
    return stat(path, &st) && mkfifo(path, 0600) ? -1 : 0;
}

void *hos_control_open(const char *path) {
    return fopen(path, "r");   /* (a FIFO: blocks until a writer opens it) */
}

#endif

void hos_dirname(char *path) {
    char *slash = NULL;
    for (char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) strcpy(path, ".");
    else if (slash == path) path[1] = 0;
    else *slash = 0;
}
