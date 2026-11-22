"""Compile the lifted C (<work>/gen) with a64-kit's runtime (rt/) and the host (host/), and link the program.

Objects go to <work>/build/obj and are rebuilt only when their source, the headers they include or the flags changed
(a stamp beside each object). Compiles run in parallel, as many as fit in memory (a64-kit's toolchain.default_jobs;
ORANGES_JOBS overrides). Lifted chunks build at -O1 with the flags Longshot uses (nx2/build.py flags()). The big ones
are FP-exact (-ffp-contract=off) and LoopRotate is capped, which a64-kit measured as a third of a chunk's compile time.

    ORANGES_MARCH  CPU level for every object (default x86-64-v3)
    ORANGES_OPT    optimization level of the lifted chunks (default O1)
"""

import hashlib
import os
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from . import kit

import toolchain  # noqa: E402,I001  (the kit's)

ROOT = Path(kit.ROOT)
HOST = ROOT / "host"
RT = Path(kit.KIT) / "rt"


def cc() -> str:
    path = shutil.which(os.environ.get("CC", "clang"))
    if not path:
        raise SystemExit("clang not found (set CC)")
    return path


def common_flags(gen: Path) -> list[str]:
    return ["-march=" + os.environ.get("ORANGES_MARCH", "x86-64-v3"), "-ffp-contract=off", "-fno-strict-aliasing",
            "-mcx16", "-g0", "-Wno-everything", "-I" + str(gen), "-I" + str(RT)]


def chunk_flags(gen: Path) -> list[str]:
    return common_flags(gen) + ["-" + os.environ.get("ORANGES_OPT", "O1"), "-ffunction-sections",
                                "-mllvm", "-rotation-max-header-size=0",
                                "-mllvm", "-capture-tracking-max-uses-to-explore=5000"]


def sources(gen: Path) -> list[tuple[Path, list[str]]]:
    """(source, flags) for every object of the program"""
    chunks = sorted(gen.glob("code_*.c"))
    if not chunks:
        raise SystemExit("no lifted C in %s: run `oranges lift --write` first" % gen)
    support = ["-O2"] + common_flags(gen)
    out = [(c, chunk_flags(gen)) for c in chunks]
    out += [(gen / n, support) for n in ("a64_table.c", "a64_hooks.c", "a64_suyu.c")]
    out += [(RT / "a64_rt.c", support), (HOST / "main.c", support)]
    return out


def _key(src: Path, flags: list[str], gen: Path) -> str:
    """What an object depends on: its flags, its source and the headers every object includes (by size and mtime)"""
    h = hashlib.sha256("\0".join(flags).encode())
    for dep in [src, RT / "a64.h", gen / "a64_suyu.h", gen / "a64_fpx.h"]:
        if dep.exists():
            st = dep.stat()
            h.update(b"%s %d %d\0" % (str(dep).encode(), st.st_size, st.st_mtime_ns))
    return h.hexdigest()


def _obj(src: Path, obj_dir: Path) -> Path:
    return obj_dir / (src.stem + (".rt" if src.parent in (RT, HOST) else "") + ".o")


def build(work: str | os.PathLike, jobs: int | None = None, say=print) -> Path:
    work = Path(work)
    gen, obj_dir = work / "gen", work / "build" / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    todo, objs = [], []
    for src, flags in sources(gen):
        obj = _obj(src, obj_dir)
        objs.append(obj)
        stamp = obj.with_suffix(".key")
        key = _key(src, flags, gen)
        if not (obj.exists() and stamp.exists() and stamp.read_text() == key):
            todo.append((src, flags, obj, stamp, key))
    # (2.5 GB a compile: a 25 MB chunk peaks at 1.4 GB at -O1)
    jobs = jobs or int(os.environ.get("ORANGES_JOBS", 0)) or toolchain.default_jobs(2.5)
    # biggest first: the long compiles start early instead of finishing last
    todo.sort(key=lambda t: -t[0].stat().st_size)
    say("compile: %d of %d objects, %d at a time" % (len(todo), len(objs), jobs))
    compiler = cc()
    t0 = time.monotonic()

    def compile_one(item):
        src, flags, obj, stamp, key = item
        start = time.monotonic()
        tmp = obj.with_suffix(".o.tmp")
        r = subprocess.run([compiler, "-c", str(src), "-o", str(tmp)] + flags, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("%s failed:\n%s" % (src.name, r.stderr[-4000:]))
        os.replace(tmp, obj)
        stamp.write_text(key)
        return src.name, time.monotonic() - start

    done = 0
    with ThreadPoolExecutor(jobs) as pool:
        for fut in as_completed([pool.submit(compile_one, t) for t in todo]):
            name, secs = fut.result()
            done += 1
            say("  [%d/%d] %s %.0f s (%.0f s elapsed)" % (done, len(todo), name, secs, time.monotonic() - t0))

    exe = work / "build" / "oranges"
    say("link: %s" % exe)
    t1 = time.monotonic()
    r = subprocess.run([compiler, "-fuse-ld=lld", "-o", str(exe)] + [str(o) for o in objs] + ["-lm", "-lpthread"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("link failed:\n%s" % r.stderr[-6000:])
    say("linked %.0f MB in %.0f s" % (exe.stat().st_size / 1e6, time.monotonic() - t1))
    return exe
