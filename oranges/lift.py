"""The lift stage on the vendored a64-kit: exefs -> image (we are rtld) -> function discovery -> coverage report.

  image     hos/nx_image.py lays main, subsdk*, sdk out from 0x08000000 (1 MB aligned), applies every relocation (RELA
            and, through oranges.kit, RELR) and binds imports. Imports nothing exports go to trampolines that the
            lifter turns into calls of host hooks (imp_<name>).
  discover  lift/a64_lift.py: every entry (unwind-table starts, exports, init_array, relocated code pointers, BL
            targets) walked to the code reachable without crossing another entry. Jump tables recovered, PLT stubs
            resolved through the GOT. oranges.lift_fixes adds Longshot's jump-table fixes, oranges.insn the
            instruction forms suyu's translator lacks, oranges.spin compiler barriers in spin loops (all installed
            by install()).
  translate (optional) suyu's per-instruction translator (lift/a64emit.cpp, built with the system clang++)
            over every distinct instruction word discovery reached, to count what it cannot translate.

  write     (optional) the C: <work>/gen/code_NNN.c (functions in address order, ~24 MB a file), a64_table.c (address
            -> function, for indirect calls), a64_hooks.c, a64_fpx.h, and suyu's runtime pieces (a64_rtgen:
            a64_suyu.h/.c). About a gigabyte.

Writes <work>/image/{image.bin, image.json, functions.txt, report.json} (and <work>/gen). All of it is made from the
game and stays local.
"""

import bisect
import heapq
import json
import os
import time
from collections import Counter
from pathlib import Path

from . import kit  # noqa: F401  (sys.path)
from . import hle as hle_mod
from . import insn, lift_fixes, spin

import a64_lift  # noqa: E402  (the kit's)
import a64_rtgen  # noqa: E402
import nx_image  # noqa: E402


def build_image(exefs: str | os.PathLike, skip: tuple[str, ...] = (), hle: bool = False) -> nx_image.NxImage:
    """The guest image. hle: hook the host's HLE functions and stub the OS-facing imports (oranges.hle), as the native
    build needs; the counts are on the image as .hle_info"""
    info = None
    names: set[str] = set()
    if hle:
        import nso  # (the kit's)
        modules = [m for m in nso.load_exefs(os.fspath(exefs)) if m.name != "rtld" and m.name not in skip]
        hooked, stubbed, unused = hle_mod.hle_set(modules)
        names = hooked | stubbed
        info = {"hle": len(hooked), "stubbed": len(stubbed), "host_unused": unused}
    img = nx_image.NxImage(os.fspath(exefs), list(skip), sorted(names))
    img.hle_info = info
    return img


def install() -> None:
    """oranges' additions to the kit, once: the jump-table fixes, the extra instruction forms and the spin-loop
    barriers (oranges.spin)"""
    lift_fixes.install()
    insn.install()
    spin.install()


def discover(img, max_insns: int = 0) -> a64_lift.Lifter:
    install()
    lifter = a64_lift.Lifter(img)
    lifter.discover(max_insns)
    return lifter


def module_of(img, address: int) -> str:
    for lo, hi, name in img.code:
        if lo <= address < hi:
            return name
    return "?"


def coverage(img, lifter) -> dict:
    """Per module: entries, functions, instructions walked (shared tails counted once per function), distinct
    instructions reached, and how much of text that is."""
    per: dict[str, Counter] = {name: Counter() for _, _, name in img.code}
    reached: dict[str, set[int]] = {name: set() for _, _, name in img.code}
    for entry, (body, _labels) in lifter.funcs.items():
        name = module_of(img, entry)
        per[name]["functions"] += 1
        per[name]["walked"] += len(body)
        reached[name].update(body)
    for e in lifter.entries:
        if e not in lifter.hooks:
            per[module_of(img, e)]["entries"] += 1
    out = {}
    for lo, hi, name in img.code:
        words = (hi - lo) // 4
        c = per[name]
        out[name] = {"text_words": words, "entries": c["entries"], "functions": c["functions"], "walked": c["walked"],
                     "reached": len(reached[name]), "reached_pct": round(100 * len(reached[name]) / max(words, 1), 2)}
    return out


def unreached_runs(img, lifter, limit: int = 20) -> list[tuple[int, int, str]]:
    """The largest runs of text no function reaches (data in code, dead code, or missed entries)."""
    reached = sorted({pc for body, _ in lifter.funcs.values() for pc in body})
    runs = []
    for lo, hi, name in img.code:
        prev = lo - 4
        for pc in reached[bisect.bisect_left(reached, lo):bisect.bisect_left(reached, hi)] + [hi]:
            if pc - prev > 4:
                runs.append((prev + 4, pc - prev - 4, name))
            prev = pc
    return heapq.nlargest(limit, runs, key=lambda r: r[1])


def translate_coverage(img, lifter) -> dict:
    """Run suyu's translator over every distinct word discovery reached: the words it cannot translate, by count of
    sites. Builds lift/liba64emit.so on first use (system clang++)."""
    import a64emit  # (the kit's; builds the library on first call)
    install()
    sites: dict[int, list[int]] = {}
    for body, _ in lifter.funcs.values():
        for pc, (kind, word) in body.items():
            if kind[0] == "op":
                sites.setdefault(word, []).append(pc)
    words = list(sites)
    _texts, flags = a64emit.translate([(sites[w][0], w) for w in words])
    bad = {w: sites[w] for w, f in zip(words, flags, strict=True) if f & a64emit.UNHANDLED}
    return {"distinct_words": len(words), "untranslatable_words": len(bad),
            "untranslatable_sites": sum(len(set(v)) for v in bad.values()),
            "top": [{"word": "%08x" % w, "sites": len(set(v)), "first": "%#x" % min(v)}
                    for w, v in sorted(bad.items(), key=lambda kv: -len(set(kv[1])))[:40]]}


def write_c(lifter, gen: str | os.PathLike, exefs: str | os.PathLike | None = None,
            meta: dict | None = None) -> dict:
    """Emit the lifted C and the runtime pieces into gen. Returns what emission counted."""
    gen = os.fspath(gen)
    install()
    a64_rtgen.write(gen)
    host = hle_mod.write(gen, lifter.img, exefs, meta) if meta is not None else None
    files = lifter.write(gen)
    branches = sum(1 for b, _ in lifter.funcs.values() for k, _ in b.values() if k[0] == "br")
    return {"files": len(files), "bytes": sum(os.path.getsize(f) for f in files),
            "instructions": sum(len(b) for b, _ in lifter.funcs.values()),
            "unhandled_words": len(lifter.unhandled), "unhandled_sites": sum(lifter.unhandled.values()),
            "unhandled": {"%08x" % w: n for w, n in lifter.unhandled.most_common(40)},
            "indirect_br": len(lifter.ijump), "br": branches, "bad_targets": lifter.stats["bad_target"],
            "spin_barriers": lifter.stats["spin_barriers"], "host": host}


def run(exefs: str | os.PathLike, work: str | os.PathLike, skip: tuple[str, ...] = (), translate: bool = False,
        write: bool = False, say=print) -> dict:
    out = Path(work) / "image"
    t0 = time.monotonic()
    img = build_image(exefs, skip, hle=write)
    meta = img.write(os.fspath(out))
    t1 = time.monotonic()
    say("image: %s, %d entries, %d hooks, %d unresolved non-weak imports (%.1f s)" % (
        ", ".join("%s@%#x" % (m["name"], m["base"]) for m in meta["modules"]), len(img.entries), len(img.hooks),
        len(img.unresolved), t1 - t0))
    lifter = discover(img)
    t2 = time.monotonic()
    say("discover: %d functions (%.1f s)" % (len(lifter.funcs), t2 - t1))

    with open(out / "functions.txt", "w") as f:
        for entry in sorted(lifter.funcs):
            body, _ = lifter.funcs[entry]
            f.write("%08x %6d %-8s %s\n" % (entry, len(body), module_of(img, entry), img.names.get(entry, "")))
    report = {
        "modules": coverage(img, lifter),
        "jump_tables": len(lifter.jt),
        "jump_table_targets": sum(len(t) for t in lifter.jt.values()),
        "br_without_table": len(lifter.jt_missing),
        "plt_stubs": len(lifter.plt),
        "setjmp_owners": len(getattr(lifter, "setjmp_owner", {})),
        "unresolved_imports": dict(sorted(img.unresolved.items())),
        "stats": dict(sorted(lifter.stats.items())),
        "largest_functions": [{"entry": "%#x" % e, "insns": len(b), "module": module_of(img, e)}
                              for e, (b, _) in sorted(lifter.funcs.items(), key=lambda kv: -len(kv[1][0]))[:15]],
        "unreached_runs": [{"at": "%#x" % a, "bytes": n, "module": m} for a, n, m in unreached_runs(img, lifter)],
        "seconds": {"image": round(t1 - t0, 1), "discover": round(t2 - t1, 1)},
    }
    if translate:
        report["translate"] = translate_coverage(img, lifter)
        report["seconds"]["translate"] = round(time.monotonic() - t2, 1)
    if write:
        t3 = time.monotonic()
        report["emit"] = write_c(lifter, Path(work) / "gen", exefs, meta)
        report["hle"] = img.hle_info
        report["seconds"]["emit"] = round(time.monotonic() - t3, 1)
        say("emit: %d files, %.0f MB of C (%.1f s)" % (report["emit"]["files"], report["emit"]["bytes"] / 1e6,
                                                     report["seconds"]["emit"]))
    with open(out / "report.json", "w") as f:
        json.dump(report, f, indent=1)
    return report
