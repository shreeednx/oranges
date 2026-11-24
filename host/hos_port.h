/* Portability: what the host does differently on Linux and on Windows.

   The Windows build is MinGW-w64 (llvm-mingw, UCRT, cross-compiled: oranges/build.py's windows target). Its
   winpthreads, dirent.h, clock_gettime and stat cover most of the POSIX the host uses; the rest is here, so the other
   files call one function and keep no #ifdef of their own (the crash handler in hos_log.c is the exception: it is all
   platform). Two winpthreads gaps matter to the kernel: a condition variable can't wait on CLOCK_MONOTONIC
   (pthread_condattr_setclock fails, and a deadline is taken as wall-clock time), and nanosleep sleeps whole
   milliseconds (0.2 ms is no sleep at all); hos_cond_wait_until and hos_sleep_ns are the portable forms. */
#ifndef HOS_PORT_H
#define HOS_PORT_H

#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define lstat stat       /* (no symbolic links to tell apart) */
#define fsync _commit
#else
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0       /* (Windows' open() translates line ends unless told otherwise) */
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

void hos_port_init(void);   /* at start: on Windows the 1 ms timer resolution and a UTF-8 console */

/* the log (stderr). The Windows program is a GUI one, with no console window of its own: its log goes where stderr
   was sent (a redirect, a pipe, a terminal: MSYS2, wine), else to the console it was started from, else to
   <dir>/oranges.log (the run before's kept as oranges.old.log). Returns that file's path, or NULL: the log isn't in
   a file of ours (always on Linux) */
const char *hos_log_open(const char *dir);
/* the program stops on an error (why: one line): with the log in a file, a message box says so and where the log is
   (a player who started it from the desktop sees nothing else) */
void hos_port_stopped(const char *why);

/* time */
void hos_sleep_ns(int64_t ns);   /* sleep at least ns, interrupted or not */
void hos_cond_init_monotonic(pthread_cond_t *cv);
/* wait on cv until a CLOCK_MONOTONIC deadline: pthread_cond_timedwait's results (0, ETIMEDOUT) */
int hos_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *m, const struct timespec *deadline);
struct tm *hos_localtime(time_t t, struct tm *out, int32_t *gmtoff, char zone[8]);   /* zone: "CET" or "+01" */
struct tm *hos_gmtime(time_t t, struct tm *out);
time_t hos_timegm(struct tm *t);

/* threads */
int hos_thread_id(void);   /* the host thread's id, as the system's tools show it */
/* at the start of a host thread that runs lifted code: on Windows, stack kept back for the crash handler after a
   host stack overflow (deep guest call chains run on the host's stack); Linux has its signal stack */
void hos_thread_start(void);

/* the guest window's pages */
void *hos_vm_reserve(void *at, uint64_t size);              /* exactly at `at`, inaccessible; NULL if it can't */
int hos_vm_commit(void *p, uint64_t size);                  /* read-write; 0 or -1 */
void hos_vm_decommit(void *p, uint64_t size);               /* inaccessible again, and zeros when committed again */
int hos_vm_resident(void *p, uint64_t size, unsigned char *pages);   /* mincore(); -1 when unknown */

/* files (paths are UTF-8: the Windows build's manifest makes UTF-8 its code page) */
int hos_exe_path(char *out, size_t cap);                    /* this program's path; 0 or -1 */
void hos_dirname(char *path);                               /* in place: "a/b/c" -> "a/b", "c" -> "." */
int hos_mkdir(const char *path);                            /* mkdir(path, 0755) */
int64_t hos_pread(int fd, void *buf, size_t n, int64_t offset);
int64_t hos_pwrite(int fd, const void *buf, size_t n, int64_t offset);
int hos_ftruncate(int fd, int64_t size);                    /* (extending writes nothing: the end of file moves) */
int hos_file_sparse(int fd);   /* ranges never written take no disk space (NTFS needs to be told; Linux's are) */
int hos_rename(const char *from, const char *to);           /* rename(), replacing a file at `to` on Windows too */

/* HOS_CONTROL's channel: a FIFO at path (made if missing), or on Windows a named pipe (\\.\pipe\<path's last
   name>). hos_control_open blocks until a writer comes and returns what it writes, to its end; NULL on failure */
int hos_control_make(const char *path, char *shown, size_t cap);
void *hos_control_open(const char *path);   /* a FILE * */

#ifdef __cplusplus
}
#endif

#endif
