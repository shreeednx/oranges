"""(oranges) Loads in spin loops stay loads: a compiler barrier after each loop-invariant load of a small loop.

suyu's translator spells a plain LDR as a plain C load (recomp_load32: a memcpy from guest memory). In a loop that
only reads, the C compiler may load once and spin on the register: a data race is undefined behaviour in C, so it may
assume no other thread writes the word. On the console the core reads memory again on every pass. The game's job
scheduler waits for a spinlock that way (main+0x3872a64, in f_0b8729e0):

    0b872a64  ldr  w9, [x8]
    0b872a68  cmp  w9, #1
    0b872a6c  b.eq 0b872a64

clang -O1 made that `jmp .`: the worker spun forever and the frame loop waited for it (the stall after the first
frame). LDAR is already an atomic load (oranges.insn ldar_stlr). This covers the plain loads.

The pass: for each backward branch whose loop body (the branch target up to the branch, in address order) is in the
function and at most MAX_LOOP instructions, every load whose address registers the loop does not write gets
`__asm__ __volatile__("" ::: "memory")` after it (Lifter.after: text emitted after the instruction). Those are the
loads the compiler could hoist; a load whose address changes on every pass can't be hoisted, so it doesn't need the
barrier. The barrier emits no instructions. It only makes the compiler read guest memory again, and the guest
registers that localize_regs keeps in locals are not affected. Errors go the safe way (an unneeded barrier):
  - the address registers are the registers the load's C text reads, minus its destinations (Rt, and Rt2 for a
    pair), plus its base Rn (a load into its own base, `ldr x8, [x8]`, walks a list). A load that writes its base
    (writeback) has it among the loop's writes.
  - calls (BL/BLR/SVC) write x0-x18 and x30 as the ABI allows. A barrier after a load that the call already
    keeps from being hoisted costs nothing.
Not covered: a loop whose only read is in a callee that clang inlines (no such spin loop is known).
"""
import re

from . import kit  # noqa: F401  (sys.path)
import a64_lift  # noqa: E402

BARRIER = '__asm__ __volatile__("" ::: "memory");'
MAX_LOOP = 64                                        # instructions: spin loops are a handful
LOAD = re.compile(r"\brecomp_(?:load(?:8|16|32|64)|ldp(?:32|64))\(c,")
PAIR = re.compile(r"\brecomp_ldp(?:32|64)\(c,")
REG_REF = re.compile(r"c->x\[(\d+)\]")
CALL_WRITES = (1 << 19) - 1 | 1 << 30                # x0-x18, x30
BRANCHES = ("b", "bcond", "cbz", "tbz")

_installed = False


def reg_refs(text: str) -> int:
    """every guest register an instruction's C text names (a mask)"""
    m = 0
    for n in REG_REF.findall(text):
        m |= 1 << int(n)
    return m


def spin_loads(body: dict, texts: dict) -> set[int]:
    """the pcs of the loads that need a barrier, in one function's body ({pc: (kind, word)})"""
    def text_of(pc):
        k, w = body[pc]
        if k[0] != "op":
            return None
        text, flags = texts[(w, pc) if a64_lift.pc_relative(w) else w]
        return None if flags & a64_lift.a64emit.UNHANDLED else text

    out = set()
    for pc, (k, _) in body.items():
        if k[0] not in BRANCHES or not (pc - 4 * MAX_LOOP < k[1] <= pc):
            continue
        loop = range(k[1], pc + 4, 4)
        if any(p not in body for p in loop):
            continue
        writes, loads = 0, []
        for p in loop:
            kind = body[p][0][0]
            if kind in ("bl", "blr", "svc"):
                writes |= CALL_WRITES
                continue
            text = text_of(p)
            if text is None:
                continue
            writes |= a64_lift.reg_writes(text)
            if LOAD.search(text):
                loads.append((p, text))
        for p, text in loads:
            w = body[p][1]
            dest = 1 << (w & 31)
            if PAIR.search(text):
                dest |= 1 << ((w >> 10) & 31)
            if not ((reg_refs(text) & ~dest | 1 << ((w >> 5) & 31)) & writes):
                out.add(p)
    return out


def install():
    """wrap Lifter.emit_function: each function's spin-loop loads get the barrier (stats: spin_barriers)"""
    global _installed
    if _installed:
        return
    _installed = True
    base_emit = a64_lift.Lifter.emit_function

    def emit_function(self, e, texts):
        body, _ = self.funcs[e]
        for pc in spin_loads(body, texts):
            cur = self.after.get(pc, "")
            if BARRIER not in cur:
                self.after[pc] = (cur + " " + BARRIER).strip()
                self.stats["spin_barriers"] = self.stats.get("spin_barriers", 0) + 1
        return base_emit(self, e, texts)

    a64_lift.Lifter.emit_function = emit_function
