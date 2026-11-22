"""Instruction forms run in Unicorn and as lifted C, compared: a64-kit's tests/insn_test.py, with oranges.insn
installed, on Linux as well as Windows.

Wrapped, not edited (the kit is vendored unchanged):
  - insn.install(): the forms oranges/insn.py writes translate;
  - Unicorn runs its "max" CPU model (the default Cortex-A72 rejects the ARMv8.1+ forms);
  - memory operands based on x29/x30 get their scratch address too (after Longshot's nx2/test_insn.py);
  - PAC and MTE forms are left out (insn.py makes them the identity by design);
  - the harness reserves the guest window with mmap where the kit's uses VirtualAlloc (not on Windows);
  - words=[...] tests exactly those encodings instead of sampling a game's modules.

    python -m oranges.insncheck --words 4e20e000,ac000400 [--states 64]        (game-free)
    python -m oranges.insncheck --game [--per 6] [--states 12] [--only oranges]  (samples work/modules)
"""

import argparse
import inspect
import os
import subprocess
import sys

from . import insn, kit

insn.install()
sys.path.insert(0, os.path.join(kit.KIT, "tests"))
import insn_test  # noqa: E402,I001
import toolchain  # noqa: E402
import unicorn  # noqa: E402
from unicorn import arm64_const as A  # noqa: E402

_BASE = {"build_harness": insn_test.build_harness, "sample": insn_test.sample, "target": toolchain.native_target}


class _Unicorn:
    """insn_test's view of the unicorn module: every engine it makes runs the ARMv8.x "max" CPU"""
    UcError = unicorn.UcError
    UC_ARCH_ARM64 = unicorn.UC_ARCH_ARM64
    UC_MODE_ARM = unicorn.UC_MODE_ARM

    @staticmethod
    def Uc(arch, mode):
        uc = unicorn.Uc(arch, mode)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        return uc


def mem_regs(w):
    cs = insn_test.capstone
    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM)
    md.detail = True
    ins = next(md.disasm(insn_test.struct.pack("<I", w), insn_test.CODE), None)
    alias = {"fp": 29, "lr": 30, "x29": 29, "x30": 30}
    for op in ins.operands if ins else ():
        if op.type == cs.arm64.ARM64_OP_MEM:
            def num(r):
                if not r:
                    return None
                name = ins.reg_name(r)
                if name in ("sp", "wsp"):
                    return "sp"
                if name in alias:
                    return alias[name]
                return int(name[1:]) if name[1:].isdigit() else None
            return num(op.mem.base), num(op.mem.index)
    return None


_POSIX_HARNESS = [
    ('"#include <windows.h>"', '"#include <sys/mman.h>"'),
    ("if (VirtualAlloc(A64_MEM, %d, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != A64_MEM) return 3;",
     "if (mmap(A64_MEM, %d, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) "
     "!= (void *)A64_MEM) return 3;"),
]


def _posix_build_harness():
    """insn_test.build_harness with its two Windows calls replaced, compiled in insn_test's namespace"""
    src = inspect.getsource(_BASE["build_harness"])
    for old, new in _POSIX_HARNESS:
        if src.count(old) != 1:
            raise RuntimeError("a64-kit's insn_test.build_harness changed: %r not found once" % old)
        src = src.replace(old, new)
    ns = {}
    exec(compile(src, insn_test.__file__, "exec"), insn_test.__dict__, ns)
    return ns["build_harness"]


def _install(words=None, only=None):
    insn_test.unicorn = _Unicorn
    insn_test.mem_regs = mem_regs
    toolchain.native_target = lambda *a, **k: _BASE["target"](*a, **k) + ["-mcx16"]   # (CASP on 128 bits)
    if os.name != "nt":
        insn_test.build_harness = _posix_build_harness()
    base_sample = _BASE["sample"]

    def sample(image_dir, mods, per, rng):
        if words is not None:
            md = insn_test.capstone.Cs(insn_test.capstone.CS_ARCH_ARM64, insn_test.capstone.CS_MODE_ARM)
            picked = []
            for w in words:
                ins = next(md.disasm(w.to_bytes(4, "little"), insn_test.CODE), None)
                picked.append(((ins.mnemonic if ins else "%08x" % w, ()), w))
            return picked
        picked = [(k, w) for k, w in base_sample(image_dir, mods, per, rng) if not insn.is_identity_model(w)]
        return [(k, w) for k, w in picked if only(w)] if only else picked

    insn_test.sample = sample


ONLY = {"oranges": lambda w: insn.emit(w) is not None}


def run(words=None, states=32, seed=1, image_dir=None, modules="main,sdk", per=6, only=None, fpsr=True,
        listing=False) -> int:
    """0 when every case matches Unicorn"""
    _install(words, ONLY[only] if only else None)
    argv = [image_dir or "-", "--modules", modules, "--per", str(per), "--states", str(states), "--seed", str(seed)]
    argv += ["--fpsr"] * fpsr + ["--list"] * listing
    saved = sys.argv
    sys.argv = ["insn_test"] + argv
    try:
        return insn_test.main()
    finally:
        sys.argv = saved


def module_images(exefs: str, out: str) -> str:
    """The kit's per-module images (tools/nso.py <exefs> <out>), which insn_test samples, made on first use"""
    if not os.path.exists(os.path.join(out, "main.json")):
        subprocess.run([sys.executable, os.path.join(kit.KIT, "tools", "nso.py"), exefs, out], check=True,
                       stdout=subprocess.DEVNULL)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.insncheck", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--words", help="comma-separated hex encodings to test (game-free)")
    ap.add_argument("--game", action="store_true", help="sample every form the game's modules use")
    ap.add_argument("--work", default="work")
    ap.add_argument("--modules", default="main,subsdk0,sdk")
    ap.add_argument("--per", type=int, default=6)
    ap.add_argument("--states", type=int, default=12)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", choices=sorted(ONLY))
    ap.add_argument("--list", action="store_true", help="show every failing form")
    a = ap.parse_args(argv)
    if bool(a.words) == a.game:
        ap.error("give --words or --game")
    if a.words:
        return run([int(w, 16) for w in a.words.split(",")], a.states, a.seed, listing=a.list)
    images = module_images(os.path.join(a.work, "exefs"), os.path.join(a.work, "modules"))
    return run(None, a.states, a.seed, images, a.modules, a.per, a.only, listing=a.list)


if __name__ == "__main__":
    sys.exit(main())
