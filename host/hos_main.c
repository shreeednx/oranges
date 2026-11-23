/* The native program: reserve the guest window, load the image, make the main thread and run the SDK's boot sequence.

   We stand in for rtld: hos/nx_image.py already relocated and bound the image, so rtld is not run. Its three callbacks
   are done here, in nn::init::Start's order (after Longshot's host/hos_main.c): _init_libc0, nnosInitialize(main
   thread handle, no arguments), _init_libc1, nninitStartup (the game's own: heap and allocator), _init_libc2, every
   module's DT_INIT, nnMain. nninitInitializeSdkModule only opens the SDK's service clients over IPC, which the nn::
   HLE replaces, so it is not run.

   usage: oranges [image.bin]   (default: ../image/image.bin beside the executable, i.e. work/image/image.bin)
          HOS_TRACE=1           log every SVC, stub call and file opened
          ORANGES_ROMFS=<dir>   the extracted RomFS (default work/romfs)
          ORANGES_USER=<dir>    saves (default work/user) */
#include "hos.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void load_image(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) hos_fatal(NULL, "cannot open %s (%s): run `oranges lift` first", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    const uint64_t want = hos_image_end - hos_image_base;
    if ((uint64_t)st.st_size != want)
        hos_fatal(NULL, "%s is %lld bytes, this build's image is %llu: lift and build again", path,
                  (long long)st.st_size, (unsigned long long)want);
    const uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) hos_fatal(NULL, "cannot map %s (%s)", path, strerror(errno));
    for (int i = 0; i < hos_segment_count; i++) {
        const uint64_t addr = hos_segment[i][0], size = hos_segment[i][1];
        const int perm = (int)hos_segment[i][2];
        const int state = perm == HOS_PERM_RW ? HOS_MEM_CODE_DATA : HOS_MEM_CODE;
        if (hos_map(addr, size, state, perm))
            hos_fatal(NULL, "cannot map the image segment at %#llx", (unsigned long long)addr);
        const uint64_t have = addr + size <= hos_image_end ? size : hos_image_end - addr;
        memcpy(GP(addr), file + (addr - hos_image_base), have);
    }
    munmap((void *)file, st.st_size);
    close(fd);
    hos_log("image: %llu MB at %#llx, %d segments, %u lifted functions, main build %s",
            (unsigned long long)(want >> 20), (unsigned long long)hos_image_base, hos_segment_count, a64_fn_count,
            hos_main_build_id);
}

void hos_call(Ctx *c, uint64_t addr, const char *what) {
    A64Fn f = a64_lookup(addr);
    if (!addr || !f) hos_fatal(c, "%s: no function at %#llx", what, (unsigned long long)addr);
    hos_log("boot: %s (%#llx)", what, (unsigned long long)addr);
    c->x[30] = 0;
    f(c);
}

uint64_t hos_guest_call(Ctx *c, uint64_t addr, int n, const uint64_t *args) {
    A64Fn f = a64_lookup(addr);
    if (!addr || !f) hos_fatal(c, "no function at %#llx to call", (unsigned long long)addr);
    uint64_t saved[19], lr = c->x[30];
    memcpy(saved, c->x, sizeof saved);
    for (int i = 0; i < n && i < 8; i++) c->x[i] = args[i];
    c->x[30] = 0;
    f(c);
    const uint64_t r = c->x[0];
    memcpy(c->x, saved, sizeof saved);
    c->x[30] = lr;
    return r;
}

uint32_t hos_system_event_init(Ctx *c, uint64_t event, int autoclear) {
    static uint64_t attach;
    if (!attach) attach = hos_symbol_lookup("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    if (!attach) hos_fatal(c, "nn::os::AttachReadableHandleToSystemEvent is not in the image");
    uint32_t readable;
    const uint32_t writable = hos_kernel_event_new(&readable);
    const uint64_t args[4] = {event, readable, 1, (uint64_t)(autoclear != 0)};
    hos_guest_call(c, attach, 4, args);
    return writable;
}

static Ctx *main_thread(void) {
    const uint64_t stack = hos_sys_alloc(hos_main_stack_size, 0x1000);
    const uint64_t tls = hos_sys_alloc(0x1000, 0x1000);   /* the C library's TLS block (TPIDR_EL0) */
    Ctx *c = hos_kernel_main_thread(stack + hos_main_stack_size, hos_main_priority);
    c->tpidr_el0 = tls + 0x800;
    hos_log("main thread: stack %#llx (%llu KB), thread-local region %#llx, handle %#llx, priority %d",
            (unsigned long long)stack, (unsigned long long)(hos_main_stack_size >> 10),
            (unsigned long long)c->tpidrro_el0, (unsigned long long)hos_main_thread_handle(), hos_main_priority);
    return c;
}

static void boot_call(Ctx *c, uint64_t sp, uint64_t addr, const char *what) {
    c->x[0] = c->x[1] = 0;
    c->x[31] = sp;
    hos_call(c, addr, what);
}

static void boot(void);

int main(int argc, char **argv) {
    const char *t = getenv("HOS_TRACE");
    hos_trace = t && *t && strcmp(t, "0");
    char path[PATH_MAX];
    if (argc > 1) {
        snprintf(path, sizeof path, "%s", argv[1]);
    } else {
        char exe[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        snprintf(path, sizeof path, "%s/../image/image.bin", dirname(exe));
    }
    {   /* the RomFS and user folders: beside the image's work folder unless the environment says otherwise */
        char exe[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        const char *work = dirname(dirname(exe));   /* (work/build/oranges -> work) */
        const char *romfs = getenv("ORANGES_ROMFS"), *user = getenv("ORANGES_USER");
        if (romfs) snprintf(hos_romfs_dir, sizeof hos_romfs_dir, "%s", romfs);
        else snprintf(hos_romfs_dir, sizeof hos_romfs_dir, "%s/romfs", work);
        if (user) snprintf(hos_user_dir, sizeof hos_user_dir, "%s", user);
        else snprintf(hos_user_dir, sizeof hos_user_dir, "%s/user", work);
    }
    hos_crash_init();
    hos_kernel_init();
    hos_mem_init();
    load_image(path);
    hos_window_run(boot);
    return 0;
}

/* the guest's boot, on the thread that is its main thread: the SDK's entry points in order, then nnMain */
static void boot(void) {
    Ctx *c = main_thread();
    const uint64_t sp = c->x[31];

    boot_call(c, sp, hos_entry_init_libc0, "_init_libc0");
    c->x[0] = hos_main_thread_handle();
    c->x[1] = 0;   /* program arguments: none */
    c->x[31] = sp;
    hos_call(c, hos_entry_nnosInitialize, "nnosInitialize");
    boot_call(c, sp, hos_entry_init_libc1, "_init_libc1");
    boot_call(c, sp, hos_entry_nninitStartup, "nninitStartup");
    boot_call(c, sp, hos_entry_init_libc2, "_init_libc2");
    for (int i = 0; i < hos_module_count; i++) {
        char what[64];
        snprintf(what, sizeof what, "%s DT_INIT", hos_module_name[i]);
        boot_call(c, sp, hos_module_init[i], what);
    }
    boot_call(c, sp, hos_entry_nnMain, "nnMain");
    hos_log("nnMain returned");
}
