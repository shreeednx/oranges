"""The whole pipeline in one command, for a player (and for the setup GUI, which runs it as a child process):

    oranges make <game.nup> [--dir FOLDER] [--jobs N] [--progress json] [--clean]

checks that the package holds the supported build, that FOLDER's drive has room, extracts the exefs and the RomFS,
lifts (--write), compiles and links. FOLDER (default: work) gets the same layout as a developer's work folder, so the
program finds its image and RomFS beside it: FOLDER/build/oranges (FOLDER/build-windows/oranges.exe on Windows),
FOLDER/image/image.bin, FOLDER/romfs. FOLDER/make.log keeps everything a run said, and FOLDER/oranges.json what was
built (the package, the program, when, which oranges).

A run picks up where the last one stopped: extracted files of the right size are kept, the lift is skipped while its
stamp (FOLDER/gen/.lifted: a hash of the exefs and of oranges' lift-time sources) matches, and the build compiles only
the objects it lacks. --clean deletes what only the build needed (the generated C and the objects, ~2.2 GB) once the
program is linked; a later rebuild lifts and compiles everything again.

--progress json writes one JSON object per line to stdout instead of text (the GUI reads them):

    {"event": "stages", "stages": [{"id": "check", "label": "..."}, ...]}
    {"event": "stage", "id": "extract"}                        a stage begins
    {"event": "progress", "id": "extract", "done": n, "total": m, "item": "...", "count": i, "of": k}
    {"event": "log", "text": "..."}
    {"event": "done", "exe": "...", "seconds": s}
    {"event": "error", "kind": "game|space|memory|compile|cancelled|internal", "message": "...", "log": "..."}

With --progress json, the end of stdin (the GUI closed its end, or went away) cancels the run: the compiles under
way are killed and it exits with 130. Exit codes: 0 built, 1 error, 3 not the supported build, 4 no room, 5 out of
memory, 130 cancelled.
"""

import datetime
import hashlib
import json
import os
import shutil
import sys
import threading
import time
import traceback
from pathlib import Path

from . import game
from .nup import Nup, NupError

ROOT = Path(__file__).resolve().parent.parent
STAGES = (("check", "Check the game"), ("extract", "Extract the game's files"), ("lift", "Translate the code to C"),
          ("compile", "Compile"), ("link", "Link the program"))
# (what a build adds besides the RomFS: the generated C 1.4 GB, objects 0.8 GB, the image 0.12 GB, the exefs and the
# program; with a margin, as the compiles write temporary files)
BUILD_BYTES = 3 << 30
GEN_BYTES, OBJ_BYTES = 1500 << 20, 900 << 20
# (the lift's share of its time, step by step, measured on this build: the image, discovery, writing the C)
LIFT_STEPS = (("image:", 0.08), ("discover:", 0.30), ("emit:", 1.0))
# (oranges' modules the lift's output depends on)
LIFT_MODULES = ("kit", "lift", "lift_fixes", "insn", "spin", "patches", "hle", "game", "nso")
EXIT = {"game": 3, "space": 4, "memory": 5, "cancelled": 130}


class MakeError(Exception):
    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind


class Reporter:
    """says what happens: text on stderr, or JSON lines on stdout; everything also goes to the log file"""

    def __init__(self, json_lines: bool, log: Path | None = None):
        self.json, self.lock, self.log, self.last = json_lines, threading.Lock(), None, {}
        if log:
            log.parent.mkdir(parents=True, exist_ok=True)
            self.log = open(log, "a", encoding="utf-8", newline="\n")
            self.log.write("---- oranges make, %s\n" % datetime.datetime.now().isoformat(timespec="seconds"))

    def emit(self, event: dict) -> None:
        with self.lock:
            if self.json:
                sys.stdout.write(json.dumps(event) + "\n")
                sys.stdout.flush()
            if self.log and event["event"] != "progress":
                self.log.write("%s\n" % (event.get("text") or json.dumps(event)))
                self.log.flush()

    def say(self, text: str) -> None:
        for line in str(text).splitlines() or [""]:
            if not self.json:
                print(line, file=sys.stderr, flush=True)
            self.emit({"event": "log", "text": line})

    def stage(self, sid: str) -> None:
        label = dict(STAGES)[sid]
        if not self.json:
            print("== %s" % label, file=sys.stderr, flush=True)
        self.emit({"event": "stage", "id": sid, "label": label})

    def progress(self, sid: str, done: int, total: int, **extra) -> None:
        now = time.monotonic()
        if now - self.last.get(sid, 0) < 0.2 and done < total:   # (at most five a second)
            return
        self.last[sid] = now
        self.emit(dict({"event": "progress", "id": sid, "done": done, "total": total}, **extra))
        if not self.json:
            sys.stderr.write("\r  %5.1f%% %s " % (100 * done / max(total, 1), extra.get("item", "")[:60].ljust(60)))
            if done >= total:
                sys.stderr.write("\n")
            sys.stderr.flush()


def _hash_files(h, files) -> None:
    for f in sorted(files):
        h.update(str(f.relative_to(ROOT) if ROOT in f.parents else f.name).encode() + b"\0")
        h.update(hashlib.sha256(f.read_bytes()).digest())


def lift_key(exefs: Path) -> str:
    """what the lift's output depends on: the exefs and oranges' sources that shape the C (the lift, its fixes and
    patches, the kit, and host/, whose HLE( ) functions the lift hooks)"""
    h = hashlib.sha256(b"oranges lift 1\0")
    _hash_files(h, [f for f in exefs.iterdir() if f.is_file()])
    _hash_files(h, [ROOT / "oranges" / (m + ".py") for m in LIFT_MODULES])
    kit = ROOT / "third_party" / "a64-kit"
    _hash_files(h, [f for d in ("lift", "hos", "tools", "rt") for f in (kit / d).glob("*") if f.is_file()
                    and f.suffix in (".py", ".c", ".h", ".cpp")])
    _hash_files(h, [f for f in (ROOT / "host").rglob("*") if f.suffix in (".c", ".h", ".inc")])
    return h.hexdigest()


def version() -> str:
    """this oranges' version: a release's VERSION file, else the checkout's commit"""
    v = ROOT / "VERSION"
    if v.is_file():
        return v.read_text().strip()
    head = ROOT / ".git" / "HEAD"
    try:
        ref = head.read_text().strip()
        if ref.startswith("ref: "):
            return "dev-" + (ROOT / ".git" / ref[5:]).read_text().strip()[:12]
        return "dev-" + ref[:12]
    except OSError:
        return "dev"


def free_bytes(folder: Path) -> int:
    probe = folder
    while not probe.exists():
        probe = probe.parent
    return shutil.disk_usage(probe).free


def needed_bytes(folder: Path, romfs_files: list[tuple[str, int]], lifted: bool, built: bool) -> int:
    """the room the rest of the build takes in folder: the RomFS files not extracted yet, the C and objects not made
    yet"""
    need = 0
    for path, size in romfs_files:
        p = folder / "romfs" / path
        if not (p.is_file() and p.stat().st_size == size):
            need += size
    if not lifted:
        need += GEN_BYTES
    if not built:
        need += OBJ_BYTES
    return need + (256 << 20)


def _gb(n: int) -> str:
    return "%.1f GB" % (n / 1e9)


def make(nup_path: Path, folder: Path, rep: Reporter, jobs: int | None = None, clean: bool = False) -> Path:
    from . import build
    t_start = time.monotonic()
    rep.emit({"event": "stages", "stages": [{"id": i, "label": label} for i, label in STAGES]})
    rep.say("oranges %s: building into %s" % (version(), folder))

    rep.stage("check")
    if not nup_path.is_file():
        raise MakeError("game", "%s is not a file" % nup_path)
    try:
        nup = Nup(nup_path)
    except NupError as e:
        raise MakeError("game", "%s is not a game package oranges can read: %s" % (nup_path.name, e)) from e
    with nup:
        ok, why = game.check(nup)
        rep.say("%s: %s" % (nup_path.name, why))
        if not ok:
            raise MakeError("game", "This copy of the game is not the one oranges supports (%s). oranges needs "
                                    "version 3.0.3, the base game with that update applied." % why)
        title = next(t for t in nup.titles if t.id == game.TITLE_ID)
        parts = nup.parts(title)
        romfs_files = [(path, e.size) for path, e in nup.walk(parts["romfs"])] if "romfs" in parts else []
        exefs = folder / "exefs"
        gen = folder / "gen"
        stamp = gen / ".lifted"

        folder.mkdir(parents=True, exist_ok=True)
        rep.say("extract: exefs")
        nup.extract(title, folder, "exefs")
        key = lift_key(exefs)
        lifted = stamp.is_file() and stamp.read_text().strip() == key
        exe = build.out_dir(folder) / build.exe_name()
        need = needed_bytes(folder, romfs_files, lifted, lifted and exe.is_file())
        free = free_bytes(folder)
        rep.say("room: %s needed, %s free" % (_gb(need), _gb(free)))
        if free < need:
            raise MakeError("space", "Not enough free space in %s: the build needs %s more, and %s is free." % (
                folder, _gb(need - free), _gb(free)))

        rep.stage("extract")
        total = sum(size for _, size in romfs_files)
        n = nup.extract(title, folder, "romfs", progress=lambda d, t: rep.progress("extract", d, t, unit="bytes"))
        rep.progress("extract", total, total, unit="bytes")
        rep.say("extract: %d RomFS files written, %d were there (%s)" % (n, len(romfs_files) - n, _gb(total)))

    rep.stage("lift")
    if lifted and any(gen.glob("code_*.c")):
        rep.say("lift: the C in %s is up to date" % gen)
    else:
        from . import lift
        stamp.unlink(missing_ok=True)
        rep.progress("lift", 0, 1000)

        def lift_say(text: str) -> None:
            rep.say(text)
            for prefix, share in LIFT_STEPS:
                if text.startswith(prefix):
                    rep.progress("lift", int(share * 1000), 1000, item=prefix.rstrip(":"))

        try:
            lift.run(exefs, folder, write=True, say=lift_say)
        except MemoryError as e:
            raise MakeError("memory", "The computer ran out of memory while translating the code (it needs about "
                                      "8 GB). Close other programs and try again.") from e
        stamp.write_text(key + "\n")
    rep.progress("lift", 1000, 1000)

    rep.stage("compile")
    linking = threading.Event()

    def build_say(text: str) -> None:
        if text.startswith("link:"):
            rep.stage("link")
            linking.set()
        rep.say(text)

    def build_progress(n: int, of: int, name: str, done: int, total: int) -> None:
        rep.progress("compile", done, total, item=name, count=n, of=of)

    try:
        exe = build.build(folder, jobs, say=build_say, progress=build_progress)
    except build.Cancelled:
        raise
    except build.CompileError as e:
        if e.killed:
            raise MakeError("memory", "A compile was stopped, most likely because the computer ran out of memory. "
                                      "Try again with fewer build jobs.\n\n%s" % e) from e
        raise MakeError("compile", str(e)) from e
    except OSError as e:
        if e.errno == 28:   # (ENOSPC)
            raise MakeError("space", "The disk is full: %s" % e) from e
        raise
    rep.progress("link", 1, 1)

    if clean:
        for d in (gen, build.out_dir(folder) / "obj"):
            if d.is_dir():
                rep.say("clean: delete %s" % d)
                shutil.rmtree(d, ignore_errors=True)
    info = {"format": 1, "oranges": version(), "nup": str(nup_path.resolve()), "exe": str(exe.resolve()),
            "built": datetime.datetime.now(datetime.UTC).isoformat(timespec="seconds"), "cleaned": clean}
    (folder / "oranges.json").write_text(json.dumps(info, indent=1) + "\n")
    secs = time.monotonic() - t_start
    rep.say("done: %s (%.0f s)" % (exe, secs))
    rep.emit({"event": "done", "exe": str(exe.resolve()), "seconds": round(secs, 1)})
    return exe


def _watch_stdin(rep: Reporter) -> None:
    """the end of stdin cancels the run (the GUI's cancel, or the GUI gone)"""
    from . import build

    def run():
        try:
            while sys.stdin.buffer.read1(4096):
                pass
        except (OSError, ValueError):
            pass
        rep.say("cancelled")
        build.cancel()
        rep.emit({"event": "error", "kind": "cancelled", "message": "The build was cancelled."})
        # (the lift runs in this process and can't be stopped midway: what it wrote is redone next time, as its stamp
        # is written last; objects and extracted files are written under temporary names first)
        time.sleep(0.5)
        os._exit(EXIT["cancelled"])

    threading.Thread(target=run, name="stdin watch", daemon=True).start()


def main(args) -> int:
    folder = Path(args.dir).resolve()
    rep = Reporter(args.progress == "json", folder / "make.log")
    if args.progress == "json":
        _watch_stdin(rep)
    try:
        make(Path(args.nup), folder, rep, args.jobs, args.clean)
        return 0
    except MakeError as e:
        rep.say("error: %s" % e)
        rep.emit({"event": "error", "kind": e.kind, "message": str(e), "log": str(folder / "make.log")})
        return EXIT.get(e.kind, 1)
    except Exception as e:
        from . import build
        if isinstance(e, build.Cancelled):
            return EXIT["cancelled"]
        rep.say(traceback.format_exc())
        rep.emit({"event": "error", "kind": "internal", "message": "%s: %s" % (type(e).__name__, e),
                  "log": str(folder / "make.log")})
        return 1
