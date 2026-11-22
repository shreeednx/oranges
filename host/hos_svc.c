/* The kernel calls (SVCs) the lifted SDK makes. Arguments in x0..x7, the result code in x0, outputs in x1.. (the 64-bit
   Horizon ABI). Overrides rt/a64_rt.c's weak a64_svc.

   Memory, information and debugging are here. Threads, handles and synchronization are hos_kernel.c's, sessions and
   their requests hos_ipc.c's. */
#include "hos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


static const char *const svc_name[0x80] = {
    [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute", [0x04] = "MapMemory",
    [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess", [0x08] = "CreateThread",
    [0x09] = "StartThread", [0x0A] = "ExitThread", [0x0B] = "SleepThread", [0x0C] = "GetThreadPriority",
    [0x0D] = "SetThreadPriority", [0x0E] = "GetThreadCoreMask", [0x0F] = "SetThreadCoreMask",
    [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent", [0x12] = "ClearEvent", [0x13] = "MapSharedMemory",
    [0x14] = "UnmapSharedMemory", [0x15] = "CreateTransferMemory", [0x16] = "CloseHandle", [0x17] = "ResetSignal",
    [0x18] = "WaitSynchronization", [0x19] = "CancelSynchronization", [0x1A] = "ArbitrateLock",
    [0x1B] = "ArbitrateUnlock", [0x1C] = "WaitProcessWideKeyAtomic", [0x1D] = "SignalProcessWideKey",
    [0x1E] = "GetSystemTick", [0x1F] = "ConnectToNamedPort", [0x20] = "SendSyncRequestLight", [0x21] = "SendSyncRequest",
    [0x22] = "SendSyncRequestWithUserBuffer", [0x23] = "SendAsyncRequestWithUserBuffer", [0x24] = "GetProcessId",
    [0x25] = "GetThreadId", [0x26] = "Break", [0x27] = "OutputDebugString", [0x28] = "ReturnFromException",
    [0x29] = "GetInfo", [0x2A] = "FlushEntireDataCache", [0x2B] = "FlushDataCache", [0x2C] = "MapPhysicalMemory",
    [0x2D] = "UnmapPhysicalMemory", [0x2E] = "GetDebugFutureThreadInfo", [0x2F] = "GetLastThreadInfo",
    [0x30] = "GetResourceLimitLimitValue", [0x31] = "GetResourceLimitCurrentValue", [0x32] = "SetThreadActivity",
    [0x33] = "GetThreadContext3", [0x34] = "WaitForAddress", [0x35] = "SignalToAddress",
    [0x36] = "SynchronizePreemptionState", [0x40] = "CreateSession", [0x41] = "AcceptSession",
    [0x43] = "ReplyAndReceive", [0x44] = "ReplyAndReceiveWithUserBuffer", [0x45] = "CreateEvent",
    [0x4B] = "CreateCodeMemory", [0x4C] = "ControlCodeMemory", [0x50] = "CreateSharedMemory",
    [0x51] = "MapTransferMemory", [0x52] = "UnmapTransferMemory", [0x70] = "CreatePort", [0x71] = "ManageNamedPort",
    [0x72] = "ConnectToPort",
};

static uint64_t random64(void) {
    static uint64_t s = 0x9E3779B97F4A7C15ULL;
    if (s == 0x9E3779B97F4A7C15ULL) s ^= (uint64_t)time(NULL) * 0xBF58476D1CE4E5B9ULL;
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return s;
}

static int get_info(uint32_t type, uint32_t handle, uint64_t sub, uint64_t *out) {
    (void)handle;
    switch (type) {
    case 0: *out = 0x7; return 0;                                   /* AllowedCoreMask: the application's cores 0-2 */
    case 1: *out = 0xFFFFFFFFFFFFFFFFULL; return 0;                 /* AllowedPriorityMask */
    case 2: *out = HOS_ALIAS_BASE; return 0;                        /* AliasRegionAddress */
    case 3: *out = HOS_ALIAS_SIZE; return 0;
    case 4: *out = HOS_HEAP_BASE; return 0;                         /* HeapRegionAddress */
    case 5: *out = HOS_HEAP_MAX; return 0;
    case 6: case 21: *out = hos_total_memory; return 0;             /* TotalMemorySize, TotalNonSystemMemorySize */
    case 7: case 22:                                                /* UsedMemorySize, UsedNonSystemMemorySize */
        *out = hos_heap_size + (hos_image_end - hos_image_base);
        return 0;
    case 8: *out = 0; return 0;                                     /* DebuggerAttached */
    case 9: *out = 0; return 0;                                     /* ResourceLimit: none to query */
    case 10: *out = 0; return 0;                                    /* IdleTickCount */
    case 11: if (sub > 3) return KERN_INVALID_COMBINATION; *out = random64(); return 0;   /* RandomEntropy */
    case 12: *out = HOS_SYSTEM_BASE; return 0;                      /* AslrRegionAddress: the whole window */
    case 13: *out = HOS_WINDOW - HOS_SYSTEM_BASE; return 0;
    case 14: *out = HOS_STACK_BASE; return 0;                       /* StackRegionAddress */
    case 15: *out = HOS_STACK_SIZE; return 0;
    case 16: case 17: *out = 0; return 0;                           /* SystemResourceSize total, used */
    case 18: *out = hos_program_id; return 0;                       /* ProgramId */
    case 20: *out = 0; return 0;                                    /* UserExceptionContextAddress */
    case 23: *out = 1; return 0;                                    /* IsApplication */
    case 24: *out = 100; return 0;                                  /* FreeThreadCount */
    case 25: *out = 0; return 0;                                    /* ThreadTickCount */
    case 26: *out = 1; return 0;                                    /* IsSvcPermitted */
    case 27: *out = 0; return 0;                                    /* IoRegionHint */
    case 28: *out = 0; return 0;                                    /* AliasRegionExtraSize (kernel 18+) */
    default: return KERN_INVALID_ENUM;
    }
}

static void log_svc(Ctx *c, uint32_t imm) {
    hos_log("svc %#x %s(%#llx, %#llx, %#llx, %#llx) lr %#llx", imm, imm < 0x80 && svc_name[imm] ? svc_name[imm] : "?",
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
            (unsigned long long)c->x[3], (unsigned long long)c->x[30]);
}

static void not_implemented(Ctx *c, uint32_t imm) {
    if (!hos_trace) log_svc(c, imm);
    hos_log("  not implemented: answers %#x", KERN_NOT_IMPLEMENTED);
    c->x[0] = KERN_NOT_IMPLEMENTED;
}

void a64_svc(Ctx *A64_NOESC c, uint32_t imm) {
    if (hos_trace) log_svc(c, imm);
    uint64_t v = 0;
    switch (imm) {
    case 0x01: {   /* SetHeapSize(size) -> addr */
        const int r = hos_set_heap_size(c->x[1], &v);
        hos_log("heap: %llu MB%s", (unsigned long long)(c->x[1] >> 20), r ? " refused" : "");
        c->x[0] = r, c->x[1] = v;
        return;
    }
    case 0x02: hos_set_perm(c->x[0], c->x[1], (int)c->x[2]); c->x[0] = 0; return;   /* SetMemoryPermission */
    case 0x03: c->x[0] = 0; return;                                                 /* SetMemoryAttribute */
    case 0x04: {   /* MapMemory(dst, src, size): the alias of src at dst. One guest window: a copy, and src is gone */
        const uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
        if (hos_map(dst, size, HOS_MEM_STACK, HOS_PERM_RW)) { c->x[0] = KERN_INVALID_ADDRESS; return; }
        memcpy(GP(dst), GP(src), size);
        hos_set_perm(src, size, HOS_PERM_NONE);
        c->x[0] = 0;
        return;
    }
    case 0x05: {   /* UnmapMemory(dst, src, size) */
        const uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
        memcpy(GP(src), GP(dst), size);
        hos_set_perm(src, size, HOS_PERM_RW);
        hos_unmap(dst, size);
        c->x[0] = 0;
        return;
    }
    case 0x06: {   /* QueryMemory(MemoryInfo *out, -, addr) -> page info */
        uint64_t start, size;
        int state, perm;
        hos_query(c->x[2], &start, &size, &state, &perm);
        uint8_t *mi = GP(c->x[0]);
        memset(mi, 0, 0x28);
        memcpy(mi, &start, 8);
        memcpy(mi + 8, &size, 8);
        memcpy(mi + 0x10, &state, 4);
        memcpy(mi + 0x18, &perm, 4);
        c->x[0] = 0, c->x[1] = 0;
        return;
    }
    case 0x07: hos_log("ExitProcess"); exit(0);
    case 0x15:     /* CreateTransferMemory(-, addr, size, perm) -> handle */
        c->x[1] = hos_handle_new(HOS_H_TRANSFER, NULL, c->x[1], c->x[2], c->x[3]);
        c->x[0] = 0;
        return;
    case 0x1E: { extern uint64_t a64_cntpct_host(void); c->x[0] = a64_cntpct_host(); return; }   /* GetSystemTick */
    case 0x1F: {   /* ConnectToNamedPort(-, name) -> session (hos_ipc.c) */
        char name[12] = {0};
        memcpy(name, GP(c->x[1]), 11);
        c->x[1] = hos_ipc_connect(name);
        hos_log("ipc: ConnectToNamedPort(\"%s\") -> session %#llx", name, (unsigned long long)c->x[1]);
        c->x[0] = 0;
        return;
    }
    case 0x21:     /* SendSyncRequest(session): the request is in the thread's IPC buffer (TLS + 0) */
        hos_ipc_request(c, GP(c->tpidrro_el0), 0x100, (uint32_t)c->x[0]);
        return;
    case 0x22:     /* SendSyncRequestWithUserBuffer(buffer, size, session) */
        hos_ipc_request(c, GP(c->x[0]), (uint32_t)c->x[1], (uint32_t)c->x[2]);
        return;
    case 0x24: c->x[0] = 0, c->x[1] = 0x51; return;                                           /* GetProcessId */
    case 0x26: {   /* Break(reason, arg, size): bit 31 of the reason = notification only */
        const uint64_t reason = c->x[0];
        log_svc(c, imm);
        if (reason & 0x80000000u) { c->x[0] = 0; return; }
        if (c->x[2] && c->x[2] <= 0x100) {
            const uint8_t *p = GP(c->x[1]);
            char hex[0x201] = {0};
            for (uint64_t i = 0; i < c->x[2]; i++) sprintf(hex + 2 * i, "%02x", p[i]);
            hos_log("  break data: %s", hex);
        }
        hos_fatal(c, "the guest called Break (reason %#llx)", (unsigned long long)reason);
    }
    case 0x27: {   /* OutputDebugString(str, len) */
        const uint64_t n = c->x[1] < 4096 ? c->x[1] : 4096;
        hos_log("guest: %.*s", (int)n, (const char *)GP(c->x[0]));
        c->x[0] = 0;
        return;
    }
    case 0x29: {   /* GetInfo(-, type, handle, sub) -> value */
        const int r = get_info((uint32_t)c->x[1], (uint32_t)c->x[2], c->x[3], &v);
        if (r) hos_log("GetInfo type %llu sub %llu: unknown", (unsigned long long)c->x[1], (unsigned long long)c->x[3]);
        c->x[0] = r, c->x[1] = v;
        return;
    }
    case 0x2A: case 0x2B: c->x[0] = 0; return;   /* cache flushes */
    case 0x2C:     /* MapPhysicalMemory(addr, size): backs part of the alias region */
        c->x[0] = hos_map(c->x[0], c->x[1], HOS_MEM_NORMAL, HOS_PERM_RW) ? KERN_INVALID_ADDRESS : 0;
        return;
    case 0x2D: hos_unmap(c->x[0], c->x[1]); c->x[0] = 0; return;   /* UnmapPhysicalMemory */
    case 0x36: c->x[0] = 0; return;              /* SynchronizePreemptionState */
    default:
        if (!hos_kernel_svc(c, imm)) not_implemented(c, imm);
        return;
    }
}
