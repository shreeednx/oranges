/* nn::fs as HLE: mounts map to host folders (after Longshot's host/nn_fs.c, on POSIX calls).
     MountRom(name)            hos_romfs_dir: the game's RomFS (`oranges extract <nup> work --only romfs`), read-only
     MountDeviceSaveData(name) <hos_user_dir>/save/device
     MountAddOnContent         no add-on content: "target not found"
   A path is "<mount>:/<path>". FileHandle and DirectoryHandle (one pointer each, passed in a register) are indexes
   into the tables here, plus one. Reads and writes are positional (pread/pwrite), so threads may share a file. Results
   are nn::fs's (module 2). Writes go straight to the host files, so Commit has nothing left to do.
   HOS_FS_LOG=1 logs every path the game asks for. */
#include "hos.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FS_RESULT(desc) HOS_RESULT(2, desc)
#define R_PATH_NOT_FOUND FS_RESULT(1)
#define R_PATH_ALREADY_EXISTS FS_RESULT(2)
#define R_DIRECTORY_NOT_EMPTY FS_RESULT(8)
#define R_MOUNT_NAME_ALREADY_EXISTS FS_RESULT(60)
#define R_TARGET_NOT_FOUND FS_RESULT(1002)
#define R_OUT_OF_RANGE FS_RESULT(3005)
#define R_INVALID_PATH FS_RESULT(6001)
#define R_INVALID_ARGUMENT FS_RESULT(6002)
#define R_INVALID_MOUNT_NAME FS_RESULT(6065)
#define R_READ_ONLY FS_RESULT(6371)
#define R_TOO_MANY_HANDLES FS_RESULT(6705)

char hos_romfs_dir[1024], hos_user_dir[1024];

#define PATH_CAP 2048
#define MAX_MOUNTS 32
#define MAX_FILES 4096
#define MAX_DIRS 512

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[32], host[1024]; int writable; } mounts[MAX_MOUNTS];
static struct { int fd, writable; } files[MAX_FILES];
typedef struct { char name[0x301]; int8_t type; int64_t size; } DirItem;
static struct { DirItem *items; int64_t count, pos; } dirs[MAX_DIRS];

static const char *gstr(uint64_t a) { return a ? (const char *)GP(a) : ""; }

static int mkdirs(const char *path) {
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static uint32_t add_mount(const char *name, const char *host, int writable) {
    pthread_mutex_lock(&lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].name[0] && !strcmp(mounts[i].name, name)) {
            pthread_mutex_unlock(&lock);
            return R_MOUNT_NAME_ALREADY_EXISTS;
        }
        if (!mounts[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || strlen(name) >= sizeof mounts[0].name) {
        pthread_mutex_unlock(&lock);
        return R_INVALID_MOUNT_NAME;
    }
    snprintf(mounts[free_slot].name, sizeof mounts[0].name, "%s", name);
    snprintf(mounts[free_slot].host, sizeof mounts[0].host, "%s", host);
    mounts[free_slot].writable = writable;
    pthread_mutex_unlock(&lock);
    hos_log("fs: mounted %s: -> %s%s", name, host, writable ? "" : " (read-only)");
    return 0;
}

/* "<mount>:/<path>" -> host path: 0, or a result. writable: whether the mount takes writes */
static uint32_t host_path(const char *guest, char *out, int *writable) {
    static int log = -1;
    if (log < 0) log = getenv("HOS_FS_LOG") != NULL;
    if (log) hos_log("fs: %s (lr %#llx)", guest, hos_current ? (unsigned long long)hos_current->x[30] : 0ULL);
    const char *colon = strchr(guest, ':');
    if (!colon || colon == guest || colon - guest >= 32) return R_INVALID_MOUNT_NAME;
    char name[32];
    memcpy(name, guest, colon - guest);
    name[colon - guest] = 0;
    const char *rest = colon + 1;
    while (*rest == '/') rest++;
    for (const char *s = rest; *s; s++)   /* no way out of the mount */
        if (s[0] == '.' && s[1] == '.' && (s == rest || s[-1] == '/') && (!s[2] || s[2] == '/')) return R_INVALID_PATH;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].name[0] && !strcmp(mounts[i].name, name)) {
            snprintf(out, PATH_CAP, "%s/%s", mounts[i].host, rest);
            const size_t n = strlen(out);
            if (n > 1 && out[n - 1] == '/') out[n - 1] = 0;
            if (writable) *writable = mounts[i].writable;
            pthread_mutex_unlock(&lock);
            return 0;
        }
    pthread_mutex_unlock(&lock);
    return R_INVALID_MOUNT_NAME;
}

static uint32_t errno_result(void) {
    switch (errno) {
    case ENOENT: case ENOTDIR: return R_PATH_NOT_FOUND;
    case EEXIST: return R_PATH_ALREADY_EXISTS;
    case ENOTEMPTY: return R_DIRECTORY_NOT_EMPTY;
    case EROFS: case EACCES: case EPERM: return R_READ_ONLY;
    default: return R_INVALID_ARGUMENT;
    }
}

static int file_fd(uint64_t h, int *writable) {
    if (!h || h > MAX_FILES) return -1;
    pthread_mutex_lock(&lock);
    const int fd = files[h - 1].fd;
    if (writable) *writable = files[h - 1].writable;
    pthread_mutex_unlock(&lock);
    return fd > 0 ? fd : -1;
}

/* ---- mounts */

HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { G64(ARG(0)) = 0x10000; RET(0); }
HLE(_ZN2nn2fs8MountRomEPKcPvm) {
    if (!hos_romfs_dir[0]) hos_fatal(c, "MountRom: no RomFS folder (set ORANGES_ROMFS)");
    RET(add_mount(gstr(ARG(0)), hos_romfs_dir, 0));
}
HLE(_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi) { G64(ARG(0)) = 0x10000; RET(0); }
HLE(_ZN2nn2fs17MountAddOnContentEPKciPvm) { RET(R_TARGET_NOT_FOUND); }
HLE(_ZN2nn2fs19MountDeviceSaveDataEPKc) {
    char host[PATH_CAP];
    snprintf(host, sizeof host, "%s/save/device", hos_user_dir);
    if (mkdirs(host)) hos_fatal(c, "cannot make the save folder %s (%s)", host, strerror(errno));
    RET(add_mount(gstr(ARG(0)), host, 1));
}
HLE(_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_) {   /* (size, journal size): room enough for any save */
    G64(ARG(0)) = 0x20000000;
    G64(ARG(1)) = 0x20000000;
    RET(0);
}
HLE(_ZN2nn2fs20ExtendDeviceSaveDataEll) { RET(0); }
HLE(_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc) { *(uint8_t *)GP(ARG(0)) = 0; RET(0); }
HLE(_ZN2nn2fs7UnmountEPKc) {
    const char *name = gstr(ARG(0));
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (!strcmp(mounts[i].name, name)) mounts[i].name[0] = 0;
    pthread_mutex_unlock(&lock);
    RET(0);
}
HLE(_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE) { RET(0); }
HLE(_ZN2nn2fs6CommitEPKPKci) { RET(0); }
HLE(_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE) { RET(0); }

/* ---- files */

HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {   /* (FileHandle *out, path, mode: 1 read, 2 write, 4 append) */
    char path[PATH_CAP];
    int writable = 0;
    const int mode = (int)ARG(2);
    uint32_t r = host_path(gstr(ARG(1)), path, &writable);
    if (r) { RET(r); return; }
    if (mode & 2 && !writable) { RET(R_READ_ONLY); return; }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        if (hos_trace) hos_log("fs: OpenFile %s: not found", gstr(ARG(1)));
        RET(R_PATH_NOT_FOUND);
        return;
    }
    const int fd = open(path, mode & 2 ? O_RDWR : O_RDONLY);
    if (fd < 0) { RET(errno_result()); return; }
    pthread_mutex_lock(&lock);
    int i = 0;
    while (i < MAX_FILES && files[i].fd > 0) i++;
    if (i == MAX_FILES) {
        pthread_mutex_unlock(&lock);
        close(fd);
        RET(R_TOO_MANY_HANDLES);
        return;
    }
    files[i].fd = fd, files[i].writable = (mode & 2) != 0;
    pthread_mutex_unlock(&lock);
    if (hos_trace) hos_log("fs: OpenFile %s -> %d", gstr(ARG(1)), i + 1);
    G64(ARG(0)) = (uint64_t)i + 1;
    RET(0);
}

static uint32_t read_file(uint64_t h, int64_t offset, uint64_t buf, uint64_t size, uint64_t *got) {
    const int fd = file_fd(h, NULL);
    if (fd < 0) return R_INVALID_ARGUMENT;
    struct stat st;
    fstat(fd, &st);
    if (offset < 0 || offset > st.st_size) return R_OUT_OF_RANGE;
    uint64_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, GP(buf + done), size - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (uint64_t)n;
    }
    *got = done;
    return 0;
}

HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {   /* (handle, offset, buffer, size): all of it, or out of range */
    uint64_t got = 0;
    const uint32_t r = read_file(ARG(0), (int64_t)ARG(1), ARG(2), ARG(3), &got);
    RET(r ? r : got == ARG(3) ? 0 : R_OUT_OF_RANGE);
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {   /* (size_t *read, handle, offset, buffer, size) */
    uint64_t got = 0;
    const uint32_t r = read_file(ARG(1), (int64_t)ARG(2), ARG(3), ARG(4), &got);
    if (!r) G64(ARG(0)) = got;
    RET(r);
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE) {
    uint64_t got = 0;
    const uint32_t r = read_file(ARG(1), (int64_t)ARG(2), ARG(3), ARG(4), &got);
    if (!r) G64(ARG(0)) = got;
    RET(r);
}
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    const int fd = file_fd(ARG(1), NULL);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { RET(R_INVALID_ARGUMENT); return; }
    G64(ARG(0)) = (uint64_t)st.st_size;
    RET(0);
}
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {   /* (handle, offset, data, size, option) */
    int writable = 0;
    const int fd = file_fd(ARG(0), &writable);
    if (fd < 0 || !writable) { RET(R_INVALID_ARGUMENT); return; }
    uint64_t done = 0;
    while (done < ARG(3)) {
        const ssize_t n = pwrite(fd, GP(ARG(2) + done), ARG(3) - done, (off_t)ARG(1) + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { RET(errno_result()); return; }
        done += (uint64_t)n;
    }
    RET(0);
}
HLE(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl) {
    int writable = 0;
    const int fd = file_fd(ARG(0), &writable);
    RET(fd < 0 || !writable ? R_INVALID_ARGUMENT : ftruncate(fd, (off_t)ARG(1)) ? errno_result() : 0);
}
HLE(_ZN2nn2fs9FlushFileENS0_10FileHandleE) { RET(0); }
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) {
    const uint64_t h = ARG(0);
    if (h && h <= MAX_FILES) {
        pthread_mutex_lock(&lock);
        if (files[h - 1].fd > 0) close(files[h - 1].fd);
        files[h - 1].fd = 0;
        pthread_mutex_unlock(&lock);
    }
}

/* ---- paths */

static uint32_t writable_path(uint64_t guest, char *path) {
    int writable = 0;
    const uint32_t r = host_path(gstr(guest), path, &writable);
    return r ? r : writable ? 0 : R_READ_ONLY;
}

HLE(_ZN2nn2fs10CreateFileEPKcl) {   /* (path, size) */
    char path[PATH_CAP];
    uint32_t r = writable_path(ARG(0), path);
    if (r) { RET(r); return; }
    const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { RET(errno_result()); return; }
    r = ftruncate(fd, (off_t)ARG(1)) ? errno_result() : 0;
    close(fd);
    RET(r);
}
HLE(_ZN2nn2fs10DeleteFileEPKc) {
    char path[PATH_CAP];
    const uint32_t r = writable_path(ARG(0), path);
    RET(r ? r : unlink(path) ? errno_result() : 0);
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) {
    char path[PATH_CAP];
    const uint32_t r = writable_path(ARG(0), path);
    RET(r ? r : mkdir(path, 0755) ? errno_result() : 0);
}

static int remove_tree(const char *path, int keep_top) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[PATH_CAP];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(sub, &st)) continue;
        bad |= S_ISDIR(st.st_mode) ? remove_tree(sub, 0) : unlink(sub);
    }
    closedir(d);
    return bad | (keep_top ? 0 : rmdir(path));
}

HLE(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc) {
    char path[PATH_CAP];
    const uint32_t r = writable_path(ARG(0), path);
    RET(r ? r : remove_tree(path, 0) ? errno_result() : 0);
}
HLE(_ZN2nn2fs25CleanDirectoryRecursivelyEPKc) {
    char path[PATH_CAP];
    const uint32_t r = writable_path(ARG(0), path);
    RET(r ? r : remove_tree(path, 1) ? errno_result() : 0);
}
HLE(_ZN2nn2fs15RenameDirectoryEPKcS2_) {
    char from[PATH_CAP], to[PATH_CAP];
    uint32_t r = writable_path(ARG(0), from);
    if (!r) r = writable_path(ARG(1), to);
    if (!r) {
        struct stat st;
        r = !stat(to, &st) ? R_PATH_ALREADY_EXISTS : rename(from, to) ? errno_result() : 0;
    }
    RET(r);
}
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {   /* (type *out: 0 directory, 1 file, path) */
    char path[PATH_CAP];
    const uint32_t r = host_path(gstr(ARG(1)), path, NULL);
    struct stat st;
    if (r) { RET(r); return; }
    if (stat(path, &st)) { RET(R_PATH_NOT_FOUND); return; }
    G32(ARG(0)) = S_ISDIR(st.st_mode) ? 0 : 1;
    RET(0);
}

/* ---- directories. DirectoryEntry: char name[0x301], 3 reserved, int8 type (0 directory, 1 file), 3 more, int64 size:
   0x310 bytes. The listing is taken at OpenDirectory. */

static int by_name(const void *a, const void *b) { return strcmp(((const DirItem *)a)->name, ((const DirItem *)b)->name); }

HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {   /* (handle *out, path, mode: 1 directories, 2 files) */
    char path[PATH_CAP];
    const int mode = (int)ARG(2);
    const uint32_t r = host_path(gstr(ARG(1)), path, NULL);
    if (r) { RET(r); return; }
    DIR *d = opendir(path);
    if (!d) { RET(errno_result()); return; }
    DirItem *items = NULL;
    int64_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[PATH_CAP];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st)) continue;
        const int is_dir = S_ISDIR(st.st_mode);
        if (!(mode & (is_dir ? 1 : 2))) continue;
        if (n == cap) items = realloc(items, (cap = cap ? cap * 2 : 64) * sizeof *items);
        memset(&items[n], 0, sizeof *items);
        snprintf(items[n].name, sizeof items[n].name, "%s", e->d_name);
        items[n].type = is_dir ? 0 : 1;
        items[n].size = is_dir ? 0 : st.st_size;
        n++;
    }
    closedir(d);
    qsort(items, n, sizeof *items, by_name);
    pthread_mutex_lock(&lock);
    int i = 0;
    while (i < MAX_DIRS && dirs[i].items) i++;
    if (i == MAX_DIRS) {
        pthread_mutex_unlock(&lock);
        free(items);
        RET(R_TOO_MANY_HANDLES);
        return;
    }
    dirs[i].items = items ? items : malloc(1), dirs[i].count = n, dirs[i].pos = 0;
    pthread_mutex_unlock(&lock);
    G64(ARG(0)) = (uint64_t)i + 1;
    RET(0);
}
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {   /* (count *out, entries, h, max) */
    const uint64_t h = ARG(2);
    if (!h || h > MAX_DIRS || !dirs[h - 1].items) { RET(R_INVALID_ARGUMENT); return; }
    int64_t n = 0;
    uint8_t *out = GP(ARG(1));
    while (n < (int64_t)ARG(3) && dirs[h - 1].pos < dirs[h - 1].count) {
        const DirItem *it = &dirs[h - 1].items[dirs[h - 1].pos++];
        memset(out, 0, 0x310);
        memcpy(out, it->name, sizeof it->name);
        out[0x304] = (uint8_t)it->type;
        memcpy(out + 0x308, &it->size, 8);
        out += 0x310;
        n++;
    }
    G64(ARG(0)) = (uint64_t)n;
    RET(0);
}
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    const uint64_t h = ARG(1);
    if (!h || h > MAX_DIRS || !dirs[h - 1].items) { RET(R_INVALID_ARGUMENT); return; }
    G64(ARG(0)) = (uint64_t)dirs[h - 1].count;
    RET(0);
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    const uint64_t h = ARG(0);
    if (!h || h > MAX_DIRS) return;
    pthread_mutex_lock(&lock);
    free(dirs[h - 1].items);
    dirs[h - 1].items = NULL;
    pthread_mutex_unlock(&lock);
}
