/* oranges' Horizon OS layer: HLE of the Nintendo SDK at the nn:: boundary, plus the kernel calls the lifted SDK makes.

   The design follows Longshot's host/ (and TOTKPort's before it):
     - An SDK function the host replaces is `HLE(<mangled name>) { ... }`: arguments in c->x[0..7], the result in
       c->x[0]. The lift hooks it where the game imports it and at the sdk's definition (oranges/hle.py).
     - Every other OS-facing function the game imports is a generated stub (gen/a64_stubs.c -> hos_unimplemented).
     - Everything else runs lifted. Its SVCs are hos_svc.c's.

   Guest address map. The guest is one 4 GB window at A64_HOST_BASE: rt/a64.h's A64_PTR truncates guest addresses to
   32 bits.
     0x01000000 - 0x07000000   system region: our own objects (stacks, thread-local regions), never freed
     0x07000000 -              trampolines (no memory: addresses the lifter turns into calls of imp_ hooks)
     0x08000000 - image end    main, subsdk0, sdk, as hos/nx_image.py laid them out (gen/hos_image.c)
     HOS_HEAP_BASE -           the heap (svcSetHeapSize)
     HOS_ALIAS_BASE -          the alias region (svcMapMemory)
     HOS_STACK_BASE -          the stack region (thread stacks the SDK maps there) */
#ifndef HOS_H
#define HOS_H
#include "a64.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define HLE(sym) void imp_##sym(Ctx *A64_NOESC c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)A64_PTR(a))           /* host pointer of a guest address */
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))

#define HOS_WINDOW (1ULL << 32)
#define HOS_SYSTEM_BASE 0x01000000ULL
#define HOS_SYSTEM_END 0x07000000ULL
#define HOS_HEAP_BASE 0x10000000ULL
#define HOS_HEAP_MAX 0xD0000000ULL     /* nninitStartup asks for the total memory less what is used (~3170 MB) */
#define HOS_ALIAS_BASE 0xE0000000ULL
#define HOS_ALIAS_SIZE 0x10000000ULL
#define HOS_STACK_BASE 0xF0000000ULL
#define HOS_STACK_SIZE 0x0F000000ULL
#define HOS_TLS_SIZE 0x200ULL          /* a thread-local region: the IPC buffer, then the SDK's words; +0x1f8 ThreadType* */

/* the host pointer of the n guest bytes at a when they lie in the window (mapped or not), else NULL */
static inline void *hos_guest_range(uint64_t a, uint64_t n) { return a < HOS_WINDOW && n <= HOS_WINDOW - a ? GP(a) : NULL; }
/* nanoseconds since the program started (CLOCK_MONOTONIC), and the console's system tick (19.2 MHz, as
   svcGetSystemTick: rt/a64_rt.c a64_cntpct_host) */
int64_t hos_now_ns(void);
uint64_t a64_cntpct_host(void);
#define HOS_TICK_HZ 19200000ULL

/* gen/hos_image.c */
extern const uint64_t hos_image_base, hos_image_end, hos_main_stack_size, hos_program_id;
extern const int hos_main_priority, hos_main_core, hos_module_count;
extern const uint64_t hos_module_init[];
extern const char *const hos_module_name[];
extern const char hos_main_build_id[];
extern const uint64_t hos_thread_start_hook, hos_thread_exit_hook;
extern const int hos_segment_count;
extern const uint64_t hos_segment[][3];   /* address, size, permission (HOS_PERM_*) */
extern const uint64_t hos_entry_init_libc0, hos_entry_init_libc1, hos_entry_init_libc2, hos_entry_nnosInitialize,
    hos_entry_nninitStartup, hos_entry_nnMain, hos_entry_nninitInitializeSdkModule, hos_entry_nvnBootstrapLoader;
extern const uint32_t a64_fn_count;   /* (gen/a64_table.c) */

/* ---- logging (hos_log.c). HOS_TRACE=1: every SVC and every stub call, not only the first */
extern int hos_trace;
void hos_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void hos_fatal(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hos_dump(Ctx *c);   /* the guest registers, to the log */
void hos_crash_init(void);   /* native crashes (signals) logged with the guest state of the thread */
extern __thread Ctx *hos_current;   /* the guest context this host thread runs */
void hos_backtrace(Ctx *c);   /* the guest call chain (its frame pointers), with the nearest named function */
const char *hos_symbol(uint64_t addr, uint64_t *offset);   /* the named function at or below addr, or NULL */

/* ---- guest memory (hos_mem.c) */
/* the kernel's MemoryState (its low byte, which svcQueryMemory reports) */
enum { HOS_MEM_FREE = 0, HOS_MEM_IO = 1, HOS_MEM_STATIC = 2, HOS_MEM_CODE = 3, HOS_MEM_CODE_DATA = 4, HOS_MEM_NORMAL = 5,
       HOS_MEM_SHARED = 6, HOS_MEM_ALIAS = 7, HOS_MEM_IPC = 10, HOS_MEM_STACK = 11, HOS_MEM_TLS = 12,
       HOS_MEM_INACCESSIBLE = 16 };
enum { HOS_PERM_NONE = 0, HOS_PERM_R = 1, HOS_PERM_W = 2, HOS_PERM_X = 4, HOS_PERM_RW = 3, HOS_PERM_RX = 5 };
void hos_mem_init(void);
int hos_map(uint64_t addr, uint64_t size, int state, int perm);   /* commit and record a range: 0 or -1 */
int hos_unmap(uint64_t addr, uint64_t size);
void hos_set_perm(uint64_t addr, uint64_t size, int perm);
/* the mapped range holding addr, or the free gap around it: start, size, state, perm */
void hos_query(uint64_t addr, uint64_t *start, uint64_t *size, int *state, int *perm);
uint64_t hos_sys_alloc(uint64_t size, uint64_t align);   /* zeroed, from the system region, never freed */
int hos_set_heap_size(uint64_t size, uint64_t *addr);    /* svcSetHeapSize: 0 or a result code */
extern uint64_t hos_heap_size, hos_total_memory;

/* ---- host folders (hos_main.c): the RomFS (ORANGES_ROMFS, default work/romfs) and the user folder with saves
   (ORANGES_USER, default work/user) */
extern char hos_romfs_dir[1024], hos_user_dir[1024];

/* ---- the kernel's threads, handles and synchronization (hos_kernel.c) */
enum { HOS_H_FREE, HOS_H_THREAD, HOS_H_EVENT, HOS_H_SESSION, HOS_H_TRANSFER, HOS_H_OTHER };
void hos_kernel_init(void);
int hos_kernel_svc(Ctx *A64_NOESC c, uint32_t imm);   /* 1 when it is one of the kernel's */
uint32_t hos_handle_new(int kind, void *obj, uint64_t a, uint64_t b, uint64_t c);
Ctx *hos_kernel_main_thread(uint64_t stack_top, int priority);
uint64_t hos_main_thread_handle(void);

/* ---- host calls (hos_hostcall.c): a guest address that calls fn(c, user), for function pointers handed to the game */
#define HOS_HOSTCALL_BASE 0x06F00000ULL   /* (to HOS_SYSTEM_END: 4096 slots of 16 bytes, no memory behind them) */
typedef void (*HosHostFn)(Ctx *A64_NOESC c, void *user);
uint64_t hos_hostcall(const char *name, HosHostFn fn, void *user);
const char *hos_hostcall_name(uint64_t addr);   /* the name a host call was registered with, or NULL */

/* ---- calls into the guest */
void hos_call(Ctx *c, uint64_t addr, const char *what);   /* call the lifted function at addr, with c as it is */
/* call the lifted function at addr from an HLE function, with up to 8 integer arguments: the caller-saved registers
   are kept around the call (the HLE's own return value goes in x0 after). Returns the callee's x0. */
uint64_t hos_guest_call(Ctx *c, uint64_t addr, int n, const uint64_t *args);
uint64_t hos_symbol_lookup(const char *name);   /* an export's address by its (mangled) name, or 0 */
/* kernel events for the host's use: a new event's writable handle (*readable: its readable handle), and a signal of it
   from any host thread */
uint32_t hos_kernel_event_new(uint32_t *readable);
void hos_kernel_event_signal(uint32_t handle);
/* an nn::os::SystemEventType of the game's, bound to a new kernel event (as an IPC service's event would be): the
   writable handle, for hos_kernel_event_signal. autoclear: EventClearMode_AutoClear */
uint32_t hos_system_event_init(Ctx *c, uint64_t event, int autoclear);

/* ---- kernel results */
#define HOS_RESULT(module, desc) ((uint32_t)(module) | (uint32_t)(desc) << 9)
#define KERN_INVALID_SIZE HOS_RESULT(1, 101)
#define KERN_INVALID_ADDRESS HOS_RESULT(1, 102)
#define KERN_INVALID_STATE HOS_RESULT(1, 106)
#define KERN_INVALID_HANDLE HOS_RESULT(1, 114)
#define KERN_INVALID_COMBINATION HOS_RESULT(1, 116)
#define KERN_TIMED_OUT HOS_RESULT(1, 117)
#define KERN_INVALID_ENUM HOS_RESULT(1, 120)
#define KERN_NOT_FOUND HOS_RESULT(1, 121)
#define KERN_NOT_IMPLEMENTED HOS_RESULT(1, 33)
#define KERN_OUT_OF_MEMORY HOS_RESULT(1, 104)
#endif
