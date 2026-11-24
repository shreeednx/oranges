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
/* end the program now: output flushed, no exit handlers (other threads run on, in libraries' teardown they crash) */
__attribute__((noreturn)) void hos_exit(int code);
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
int64_t hos_mem_dump(const char *path);   /* HOS_CONTROL MEM: the mapped writable pages into a sparse file */

/* ---- host folders (hos_main.c): the RomFS (ORANGES_ROMFS, default work/romfs) and the user folder with saves
   (ORANGES_USER, default $XDG_DATA_HOME/oranges, else ~/.local/share/oranges). hos_old_user_dir: where saves were
   before (work/user), copied from once (empty when ORANGES_USER is set) */
extern char hos_romfs_dir[1024], hos_user_dir[1024], hos_old_user_dir[1024];
void hos_fs_init(void);   /* (nn_fs.c) the save folder: chosen, an old save moved over, logged */

/* ---- the kernel's threads, handles and synchronization (hos_kernel.c) */
enum { HOS_H_FREE, HOS_H_THREAD, HOS_H_EVENT, HOS_H_SESSION, HOS_H_TRANSFER, HOS_H_OTHER };
void hos_kernel_init(void);
int hos_kernel_svc(Ctx *A64_NOESC c, uint32_t imm);   /* 1 when it is one of the kernel's */
uint32_t hos_handle_new(int kind, void *obj, uint64_t a, uint64_t b, uint64_t c);
void *hos_handle_object(uint32_t handle, int kind);   /* the object of a handle of that kind, or NULL */
Ctx *hos_kernel_main_thread(uint64_t stack_top, int priority);
uint64_t hos_main_thread_handle(void);
/* every running thread to the log: its name, what it waits for (or the SVC it is in) and its guest backtrace */
void hos_kernel_dump(const char *why);
void hos_kernel_profile(double seconds, const char *thread);   /* HOS_PROFILE now, on the calling thread */
void hos_kernel_svc_mark(Ctx *c, uint32_t imm_plus_1);   /* (hos_svc.c) the SVC c's thread is in, plus 1; 0 after */
/* a host thread for debugging (hos_kernel.c): HOS_SAMPLE=<s> hos_kernel_dump every s seconds; HOS_WATCHDOG=<s>
   (default 5, 0 off) hos_kernel_dump once when no frame was presented for s seconds (the first one included);
   HOS_PROFILE=<s>[,thread name] samples a thread's guest call chain each millisecond for s seconds once frames run
   (default thread: MainThread) and logs its return addresses by how often they were on the chain */
void hos_sampler_start(void);

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

/* ---- the window and the player's input (hos_window.c) */
/* HidNpadButton bits */
#define HOS_BTN_A (1ull << 0)
#define HOS_BTN_B (1ull << 1)
#define HOS_BTN_X (1ull << 2)
#define HOS_BTN_Y (1ull << 3)
#define HOS_BTN_LS (1ull << 4)
#define HOS_BTN_RS (1ull << 5)
#define HOS_BTN_L (1ull << 6)
#define HOS_BTN_R (1ull << 7)
#define HOS_BTN_ZL (1ull << 8)
#define HOS_BTN_ZR (1ull << 9)
#define HOS_BTN_PLUS (1ull << 10)
#define HOS_BTN_MINUS (1ull << 11)
#define HOS_BTN_LEFT (1ull << 12)
#define HOS_BTN_UP (1ull << 13)
#define HOS_BTN_RIGHT (1ull << 14)
#define HOS_BTN_DOWN (1ull << 15)
#define HOS_BTN_LSTICK_LEFT (1ull << 16)
#define HOS_BTN_LSTICK_UP (1ull << 17)
#define HOS_BTN_LSTICK_RIGHT (1ull << 18)
#define HOS_BTN_LSTICK_DOWN (1ull << 19)
#define HOS_BTN_RSTICK_LEFT (1ull << 20)
#define HOS_BTN_RSTICK_UP (1ull << 21)
#define HOS_BTN_RSTICK_RIGHT (1ull << 22)
#define HOS_BTN_RSTICK_DOWN (1ull << 23)
typedef struct {
    uint64_t buttons;              /* Npad No.1's, HOS_BTN_* (stick directions included) */
    int32_t stick[4];              /* left x, y, right x, y: -32767..32767, up positive */
    int32_t mouse_x, mouse_y;      /* in the console's 1280x720 screen */
    int32_t wheel;                 /* wheel notches turned since the start (up positive) */
    uint32_t mouse_buttons;        /* nn::hid MouseButton bits: left, right, middle, forward, back */
    int mouse_in;                  /* the pointer is over the picture */
    int touch;                     /* one finger on the touch screen, at touch_x, touch_y (1280x720) */
    int32_t touch_x, touch_y;
    uint32_t modifiers;            /* nn::hid KeyboardModifier bits */
    uint8_t keys[32];              /* keys down, a bit per HID usage id */
} HosInput;
void hos_input(HosInput *out);     /* the input now: the window's, and HOS_INPUT_SCRIPT's */
void hos_rumble(int motor, float amplitude);   /* a gamepad's motor (0 low, 1 high frequency): 0..1 */
/* HOS_CONTROL's SHOT, for the back end at each present: whether a picture is asked for (its path in path; w and h
   the window size to show it at, as the present scales it, or 0: the picture itself), and that it is saved */
int hos_control_shot(char *path, size_t size, int *w, int *h);
void hos_control_shot_done(void);
/* where the present put the picture: x, y, w, h in the swapchain's pixels of sw x sh (the mouse maps through it) */
void hos_window_picture(int x, int y, int w, int h, int sw, int sh);
/* run the game (guest_main): beside a window when there is one, else on this thread */
void hos_window_run(void (*guest_main)(void));
/* the Vulkan back end (host/vk/vk_device.cpp), started on the main thread: window an SDL_Window (null: headless);
   0 when it does not start (the null back end draws nothing) */
int rx_vk_init(void *window);

/* ---- the sound device (hos_sound.c) */
void hos_sound_init(int window);   /* (the main thread, before the game) whether there is sound, and SDL's audio */
/* one audio frame of interleaved PCM16, from the audio renderer's thread: to the device (opened at the first) */
void hos_sound_play(const int16_t *pcm, int frames, int channels, int frequency);

/* ---- IPC (hos_ipc.c): sessions, sm:, and the services the host emulates. A service is a HosIface whose dispatch
   reads r->cmd and r->in, writes its output with hos_ipc_out*, and returns a result code (HOS_IPC_UNKNOWN: a command
   it does not know, answered empty and logged). */
typedef struct { uint64_t addr, size; } HosIpcBuffer;
typedef struct HosIpc HosIpc;
typedef struct HosIface { const char *name; uint32_t (*dispatch)(HosIpc *r); } HosIface;
struct HosIpc {
    Ctx *c;
    const char *service;            /* the name the session was opened with */
    const HosIface *iface;          /* the interface the request is for */
    uint32_t cmd;
    const uint8_t *in;              /* the input raw data (after the CMIF header) */
    uint32_t in_size;
    int pid_sent;
    uint64_t pid;
    uint32_t in_handle[16];
    int n_in_handles;
    HosIpcBuffer x[16], a[16], b[16], w[16], cbuf[16];   /* send statics, send, receive, exchange, receive list */
    int nx, na, nb, nw, nc;
    /* the reply */
    uint32_t result;
    uint8_t out[0x100];
    uint32_t out_size;
    uint32_t out_handle[8];
    int out_handle_move[8], n_handles;
    const HosIface *out_object[8];  /* interfaces returned: move handles to new sessions, or domain objects */
    int n_objects;
};
#define HOS_IPC_UNKNOWN 0xFFFFFFFFu
uint32_t hos_ipc_connect(const char *name);   /* a session to a named port (svcConnectToNamedPort) */
void hos_ipc_request(Ctx *A64_NOESC c, uint8_t *msg, uint32_t msg_size, uint32_t session);
void hos_ipc_out(HosIpc *r, const void *data, uint32_t n);
void hos_ipc_out_handle(HosIpc *r, uint32_t handle, int move);
void hos_ipc_out_object(HosIpc *r, const HosIface *iface);
extern const HosIface hos_olsc_iface;   /* olsc:u (nn_system.c) */

/* ---- kernel results */
#define HOS_RESULT(module, desc) ((uint32_t)(module) | (uint32_t)(desc) << 9)
#define KERN_INVALID_SIZE HOS_RESULT(1, 101)
#define KERN_INVALID_ADDRESS HOS_RESULT(1, 102)
#define KERN_INVALID_STATE HOS_RESULT(1, 125)   /* (106 is InvalidCurrentMemory) */
#define KERN_INVALID_HANDLE HOS_RESULT(1, 114)
#define KERN_INVALID_COMBINATION HOS_RESULT(1, 116)
#define KERN_TIMED_OUT HOS_RESULT(1, 117)
#define KERN_INVALID_ENUM HOS_RESULT(1, 120)
#define KERN_NOT_FOUND HOS_RESULT(1, 121)
#define KERN_NOT_IMPLEMENTED HOS_RESULT(1, 33)
#define KERN_OUT_OF_MEMORY HOS_RESULT(1, 104)
#endif
