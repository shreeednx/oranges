/* nn::diag and nn::ro: what the game asks about its own code. After Longshot's host/nn_diag.c. */
#include "hos.h"

#include <stdio.h>
#include <string.h>

/* nn::detail::UnexpectedDefaultImpl(): a switch reached a case the SDK says cannot happen */
HLE(_ZN2nn6detail21UnexpectedDefaultImplEv) { hos_fatal(c, "nn::detail::UnexpectedDefaultImpl"); }

/* GetBacktrace(uintptr_t* out, int max) -> int: the guest frame chain (x29), return addresses first to last */
HLE(_ZN2nn4diag12GetBacktraceEPmi) {
    const int max = (int)ARG(1);
    uint64_t fp = c->x[29];
    int n = 0;
    while (n < max && fp && !(fp & 15) && hos_guest_range(fp, 16)) {
        const uint64_t lr = G64(fp + 8), next = G64(fp);
        if (!lr) break;
        if (ARG(0)) G64(ARG(0) + 8 * (uint64_t)n) = lr;
        n++;
        if (next <= fp) break;
        fp = next;
    }
    RET(n);
}
HLE(_ZN2nn4diag18IsDebuggerAttachedEv) { RET(0); }
/* GetSymbolName(char* out, size_t size, uintptr_t address) -> the symbol's address, or 0 */
HLE(_ZN2nn4diag13GetSymbolNameEPcmm) {
    uint64_t off = 0;
    const char *name = hos_symbol(ARG(2), &off);
    if (ARG(0) && ARG(1)) snprintf(GP(ARG(0)), ARG(1), "%s", name ? name : "");
    RET(name ? ARG(2) - off : 0);
}

/* nn::ro::ProtectRelro(...): main's own RELRO, made read-only after its relocations. The image is laid out writable
   by the host and stays so */
HLE(_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_) { RET(0); }
