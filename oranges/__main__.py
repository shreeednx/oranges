"""oranges command line: python -m oranges <command> ..."""

import argparse
import sys
import time
from pathlib import Path

from . import game, nso
from .nup import META_NAMES, Nup, NupError


class Bar:
    def __init__(self, label: str):
        self.label, self.start, self.last = label, time.monotonic(), 0.0

    def __call__(self, done: int, total: int) -> None:
        now = time.monotonic()
        if now - self.last < 0.5 and done != total:
            return
        self.last = now
        rate = done / max(now - self.start, 1e-6) / 1e6
        sys.stderr.write("\r%s %5.1f%%  %d / %d MB  %.0f MB/s " % (self.label, 100 * done / max(total, 1), done >> 20,
                                                                   total >> 20, rate))
        if done == total:
            sys.stderr.write("\n")
        sys.stderr.flush()


def cmd_info(args) -> int:
    with Nup(args.nup) as p:
        print("%s: %d titles, %d dirs, %d files, %.2f GB" % (args.nup, len(p.titles), p.nd, p.nf, p.file_size / 1e9))
        for t in p.titles:
            print("title %s  version %d" % (t.id_hex, t.version))
            for kind, data in p.metadata(t):
                shown = data.decode("utf-8", "replace") if kind == 5 else (
                    data.rstrip(b"\0").hex() if kind == 7 else "%d bytes" % len(data))
                print("  meta %-8s %s" % (META_NAMES.get(kind, "type %d" % kind), shown))
            for part, directory in p.parts(t).items():
                files = list(p.walk(directory))
                print("  %s/: %d files, %.1f MB" % (part, len(files), sum(e.size for _, e in files) / 1e6))
                if part == "exefs":
                    for path, e in files:
                        print("    %-12s %10d bytes%s" % (path, e.size, "  (zstd)" if e.codec else ""))
        ok, why = game.check(p)
        print("check: %s" % why)
    return 0


def cmd_check(args) -> int:
    with Nup(args.nup) as p:
        ok, why = game.check(p)
    print(why)
    return 0 if ok else 1


def cmd_verify(args) -> int:
    with Nup(args.nup) as p:
        bad = p.verify(Bar("verify"))
    for path, problem in bad:
        print("BAD %s: %s" % (path, problem))
    print("%d bad files" % len(bad) if bad else "all files match their hashes")
    return 1 if bad else 0


def cmd_extract(args) -> int:
    with Nup(args.nup) as p:
        titles = [t for t in p.titles if args.title in (None, t.id_hex, str(t.index))]
        if not titles:
            print("no title %s in the package" % args.title, file=sys.stderr)
            return 1
        for t in titles:
            dest = Path(args.dest) / t.id_hex if len(p.titles) > 1 else Path(args.dest)
            n = p.extract(t, dest, args.only, args.overwrite, Bar("extract %s" % t.id_hex))
            print("%s: wrote %d files to %s" % (t.id_hex, n, dest))
    return 0


def _print_nso(name: str, m: nso.Nso, verbose: bool) -> None:
    print("%s  build %s%s" % (name, m.build_id, "  (%s)" % m.module_path if m.module_path else ""))
    for s in (m.text, m.ro, m.data, m.bss):
        print("  %-4s %#010x-%#010x  %8.2f MB" % (s.name, s.address, s.address + s.size, s.size / 1e6))
    kinds: dict[str, int] = {}
    for r in m.relocs:
        kinds[nso.RELOC_NAMES.get(r.type, str(r.type))] = kinds.get(nso.RELOC_NAMES.get(r.type, str(r.type)), 0) + 1
    print("  symbols %d (%d exports, %d imports), relocations %s" % (len(m.symbols), len(m.exports), len(m.imports),
                                                                     kinds or "none"))
    if m.needed:
        print("  needs %s" % ", ".join(m.needed))
    svcs = m.svcs()
    if svcs:
        print("  svc %s" % " ".join("%#x" % n for n in sorted(svcs)))
    if verbose:
        for line in m.api_info:
            print("  api %s" % line)


def cmd_nso(args) -> int:
    path = Path(args.path)
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic != b"NUP\x01":
        _print_nso(path.name, nso.load(path.read_bytes()), args.verbose)
        return 0
    modules: dict[str, nso.Nso] = {}
    with Nup(path) as p:
        title = next((t for t in p.titles if t.id == game.TITLE_ID), p.titles[0])
        for name in args.modules or game.EXEFS_MODULES:
            try:
                e = p.find(title, "exefs/" + name)
            except KeyError:
                continue
            modules[name] = nso.load(p.read(e))
            _print_nso(name, modules[name], args.verbose)
    if len(modules) > 1:
        for name, missing in nso.link(modules).items():
            print("%s: %d imports no module exports%s" % (name, len(missing), "" if not args.verbose else
                                                           "".join("\n  " + s for s in missing)))
    return 0


def cmd_lift(args) -> int:
    from . import lift
    exefs = Path(args.exefs or Path(args.work) / "exefs")
    if not (exefs / "main").is_file():
        print("no %s: run `oranges extract <nup> %s --only exefs` first" % (exefs / "main", args.work), file=sys.stderr)
        return 1
    report = lift.run(exefs, args.work, tuple(args.skip.split(",")) if args.skip else (), args.translate, args.write)
    print("%-8s %10s %9s %9s %11s %8s" % ("module", "text insns", "entries", "functions", "reached", "of text"))
    for name, m in report["modules"].items():
        print("%-8s %10d %9d %9d %11d %7.2f%%" % (name, m["text_words"], m["entries"], m["functions"], m["reached"],
                                                  m["reached_pct"]))
    print("jump tables %d (%d targets), BRs without a table %d, PLT stubs %d, setjmp owners %d" % (
        report["jump_tables"], report["jump_table_targets"], report["br_without_table"], report["plt_stubs"],
        report["setjmp_owners"]))
    if report["unresolved_imports"]:
        print("unresolved imports: %s" % ", ".join(report["unresolved_imports"]))
    if "translate" in report:
        t = report["translate"]
        print("translator: %d distinct words, %d untranslatable (%d sites)" % (
            t["distinct_words"], t["untranslatable_words"], t["untranslatable_sites"]))
    if report.get("hle"):
        h = report["hle"]
        print("host: %d functions replaced (HLE), %d OS-facing imports stubbed%s" % (
            h["hle"], h["stubbed"], ", host functions no module has: %s" % ", ".join(h["host_unused"])
            if h["host_unused"] else ""))
    if "emit" in report:
        e = report["emit"]
        print("emitted: %d instructions (shared tails included), indirect BR %d of %d, untranslated words %d (%d sites)"
              % (e["instructions"], e["indirect_br"], e["br"], e["unhandled_words"], e["unhandled_sites"]))
    print("details: %s" % (Path(args.work) / "image" / "report.json"))
    return 0


def cmd_build(args) -> int:
    from . import build
    build.build(args.work, args.jobs)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="oranges", description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    s = sub.add_parser("info", help="titles, metadata, exefs and whether the build is supported")
    s.add_argument("nup")
    s.set_defaults(run=cmd_info)
    s = sub.add_parser("check", help="exit 0 when the package holds the supported build")
    s.add_argument("nup")
    s.set_defaults(run=cmd_check)
    s = sub.add_parser("verify", help="hash every file in the package")
    s.add_argument("nup")
    s.set_defaults(run=cmd_verify)
    s = sub.add_parser("extract", help="write exefs/ and romfs/ to a folder")
    s.add_argument("nup")
    s.add_argument("dest")
    s.add_argument("--only", choices=("exefs", "romfs"))
    s.add_argument("--title", help="title id or index (default: all)")
    s.add_argument("--overwrite", action="store_true", help="rewrite files already there with the right size")
    s.set_defaults(run=cmd_extract)
    s = sub.add_parser("nso", help="segments, symbols, relocations and syscalls of NSO modules (a file, or a .nup)")
    s.add_argument("path")
    s.add_argument("modules", nargs="*", help="exefs modules to load from a .nup (default: all)")
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(run=cmd_nso)
    s = sub.add_parser("lift", help="build the guest image and discover every function (a64-kit), report coverage")
    s.add_argument("--work", default="work", help="work folder (default: work)")
    s.add_argument("--exefs", help="exefs folder (default: <work>/exefs)")
    s.add_argument("--skip", help="modules not to load, comma-separated (their exports become hooks)")
    s.add_argument("--translate", action="store_true", help="also run suyu's translator over every reached instruction")
    s.add_argument("--write", action="store_true", help="also write the lifted C to <work>/gen (about a gigabyte)")
    s.set_defaults(run=cmd_lift)
    s = sub.add_parser("build", help="compile <work>/gen with the runtime and host, link <work>/build/oranges")
    s.add_argument("--work", default="work")
    s.add_argument("--jobs", type=int, help="parallel compiles (default: what fits in memory, or ORANGES_JOBS)")
    s.set_defaults(run=cmd_build)
    args = ap.parse_args(argv)
    try:
        return args.run(args)
    except (NupError, nso.NsoError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
