"""An oranges release: one folder (and archive) per platform that a player downloads, with everything the build of
their port needs, and nothing of the game (TODO.md task 2; CLAUDE.md's game data rules).

    python -m oranges.release linux   [--version V] [--out DIR] [--no-archive]
    python -m oranges.release windows [--version V] [--out DIR] [--no-archive]
    python -m oranges.release verify <release folder>

makes DIR/oranges-<V>-<platform>/ (DIR default work/release, V default the checkout's `git describe`) and its
archive (.tar.zst for Linux, .zip for Windows):

    oranges / oranges.exe   the setup GUI (gui/oranges_setup.cpp), which runs the pipeline below
    release.json            the version and platform (oranges.bundled reads it: the pipeline uses the tools here)
    src/                    oranges' tracked tree (oranges/, host/, gui/, third_party/, LICENSE), its VERSION, and
                            a64-kit's translator library prebuilt for the platform (the lift would build it otherwise)
    python/                 the Python the pipeline runs on (Linux: python-build-standalone's, oranges.linuxkit;
                            Windows: the embeddable one, oranges.wintools), with lz4 and src/ on its path
    toolchain/              Linux: LLVM's clang and lld (oranges.linuxkit); Windows: llvm-mingw (oranges.wintools)
    sysroot/                Linux only: the C library the port is built against (glibc 2.35, oranges.linuxkit)
    libs/                   the port's libraries (Linux: oranges.linuxkit's static ones; Windows: oranges.mingw's)
    SHA256SUMS              every file's hash (`verify` checks a folder against it)

Their parts come from the folders those modules fill (each checked against pinned SHA-256s when downloaded):
oranges.linuxkit's ~/.local/opt/oranges-linuxkit for Linux; for Windows oranges.wintools' and oranges.mingw's, with
the GUI and the translator library cross-compiled by llvm-mingw (ORANGES_MINGW, default /opt/llvm-mingw).
Only files git tracks go into src/, so nothing from work/ (the game's files, the lifted C) can.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TREE = ("oranges", "host", "gui", "third_party", "LICENSE", "pyproject.toml")
KIT_LIFT = ROOT / "third_party" / "a64-kit" / "lift"
# (the kit's tools that only building its libraries needs: a player's build compiles and links with clang and lld)
LINUX_BUILD_ONLY = ("llvm-ar", "llvm-ranlib", "llvm-nm", "llvm-objdump", "llvm-objcopy", "llvm-strip")
# (llvm-mingw's targets other than x86_64: not needed to build the port)
MINGW_OTHER = ("aarch64-w64-mingw32", "armv7-w64-mingw32", "i686-w64-mingw32", "arm64ec-w64-mingw32")


def git_version() -> str:
    r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True)
    return "dev-" + r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else "dev"


def tracked_files() -> list[str]:
    """the tree's files git tracks, under TREE (never work/, the game's or anything made from it)"""
    out = subprocess.run(["git", "ls-files", "-z", "--"] + list(TREE), cwd=ROOT, capture_output=True, check=True)
    return [f for f in out.stdout.decode().split("\0") if f]


def copy_tree(dest: Path, version: str) -> None:
    src = dest / "src"
    for rel in tracked_files():
        target = src / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, target)
    (src / "VERSION").write_text(version + "\n")


def emit_key() -> str:
    """the stamp build_emit.py checks: the translator's sources' SHA-1"""
    files = [KIT_LIFT / "a64emit.cpp", KIT_LIFT / "suyu" / "arm64_to_c.h"]
    return hashlib.sha1(b"".join(f.read_bytes() for f in files)).hexdigest()


def sha256sums(folder: Path) -> str:
    lines = []
    for f in sorted(p for p in folder.rglob("*") if p.is_file() and not p.is_symlink()
                    and "__pycache__" not in p.parts):   # (bytecode: Python's, rewritten as it runs)
        rel = f.relative_to(folder).as_posix()
        if rel == "SHA256SUMS":
            continue
        h = hashlib.sha256()
        with open(f, "rb") as fh:
            while chunk := fh.read(1 << 20):
                h.update(chunk)
        lines.append("%s  %s\n" % (h.hexdigest(), rel))
    return "".join(lines)


def verify(folder: Path) -> list[str]:
    """the files that differ from SHA256SUMS, or are missing"""
    bad = []
    for line in (folder / "SHA256SUMS").read_text().splitlines():
        digest, rel = line.split("  ", 1)
        f = folder / rel
        if not f.is_file():
            bad.append("%s: missing" % rel)
            continue
        h = hashlib.sha256()
        with open(f, "rb") as fh:
            while chunk := fh.read(1 << 20):
                h.update(chunk)
        if h.hexdigest() != digest:
            bad.append("%s: changed" % rel)
    return bad


def _copytree(src: Path, dst: Path, ignore=None) -> None:
    def skip(d, names):
        return [n for n in names if n == "__pycache__"] + (list(ignore(d, names)) if ignore else [])
    shutil.copytree(src, dst, symlinks=True, ignore=skip, dirs_exist_ok=True)


def finish(dest: Path, version: str, platform: str, say=print) -> None:
    (dest / "release.json").write_text(json.dumps({"format": 1, "version": version, "platform": platform},
                                                  indent=1) + "\n")
    (dest / "SHA256SUMS").write_text(sha256sums(dest))
    size = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file() and not f.is_symlink())
    say("release: %s (%.0f MB)" % (dest, size / 1e6))


# ---- Linux ----

def linux(dest: Path, version: str, kit: Path, say=print) -> None:
    from . import linuxkit
    for part in ("toolchain", "sysroot", "libs", "python"):
        if not (kit / part).is_dir():
            raise SystemExit("%s missing: run `python -m oranges.linuxkit` first" % (kit / part))
    os.environ.update(linuxkit.env_for(kit))   # (the GUI and the translator: the release's own toolchain)
    os.environ["ORANGES_TARGET"] = "linux"
    copy_tree(dest, version)
    clangxx = kit / "toolchain" / "bin" / "clang++"
    lift = dest / "src" / "third_party" / "a64-kit" / "lift"
    say("build liba64emit.so")
    subprocess.run([str(clangxx), "--sysroot=" + str(kit / "sysroot"), "-std=c++20", "-O2", "-shared", "-fPIC",
                    "-static-libstdc++", "-static-libgcc", "-fuse-ld=lld", "-Wno-everything",
                    "-o", str(lift / "liba64emit.so"), str(KIT_LIFT / "a64emit.cpp")], check=True)
    (lift / "liba64emit.so.sha1").write_text(emit_key())
    for part in ("toolchain", "sysroot", "libs"):
        say("copy %s" % part)
        _copytree(kit / part, dest / part, ignore=lambda d, names: [n for n in names if n in LINUX_BUILD_ONLY])
    _copytree(kit / "python", dest / "python")
    site = next((dest / "python" / "lib").glob("python3.*")) / "site-packages"
    (site / "oranges-release.pth").write_text("../../../../src\n")   # (relative to site-packages: <release>/src)
    from . import gui
    say("build the setup GUI")
    gui.build_gui(dest / "work-gui", say=lambda s: None)
    shutil.move(dest / "work-gui" / "oranges", dest / "oranges")
    shutil.rmtree(dest / "work-gui")
    finish(dest, version, "linux-x64", say)


# ---- Windows ----

def windows(dest: Path, version: str, wintools: Path, prefix: Path, say=print) -> None:
    for need in (wintools / "python" / "pythonw.exe", wintools / "llvm-mingw" / "bin" / "clang.exe",
                 prefix / "lib" / "pkgconfig"):
        if not need.exists():
            raise SystemExit("%s missing: run `python -m oranges.wintools` and `python -m oranges.mingw` first" % need)
    os.environ["ORANGES_TARGET"] = "windows"
    os.environ["ORANGES_PREFIX"] = str(prefix)
    from . import build
    copy_tree(dest, version)
    lift = dest / "src" / "third_party" / "a64-kit" / "lift"
    say("build a64emit.dll")
    subprocess.run([build.cxx(), "-std=c++20", "-O2", "-shared", "-static", "-fms-extensions", "-Wno-everything",
                    "-D_CRT_SECURE_NO_WARNINGS", "-fuse-ld=lld", "-o", str(lift / "a64emit.dll"),
                    str(KIT_LIFT / "a64emit.cpp")], check=True)
    (lift / "a64emit.dll.sha1").write_text(emit_key())
    say("copy python")
    py = dest / "python"
    _copytree(wintools / "python", py)
    (py / "Lib" / "site-packages" / "oranges-source.pth").unlink(missing_ok=True)   # (wintools': the checkout)
    pth = next(py.glob("python3*._pth"))
    lines = [ln for ln in pth.read_text().splitlines() if ln.strip()]
    pth.write_text("\n".join(lines[:1] + ["..\\src"] + lines[1:]) + "\n")   # (relative to python/: <release>\src)
    say("copy toolchain")
    _copytree(wintools / "llvm-mingw", dest / "toolchain",
              ignore=lambda d, names: [n for n in names if n in MINGW_OTHER or n.startswith(
                  tuple(t + "-" for t in MINGW_OTHER))])
    say("copy libs")
    _copytree(prefix, dest / "libs", ignore=lambda d, names: [n for n in names if n == ".cache"])
    from . import gui
    say("build the setup GUI")
    exe = gui.build_gui(dest / "work-gui", say=lambda s: None)
    for f in exe.parent.iterdir():   # (oranges.exe, SDL3.dll, licenses/)
        if f.name != "obj":
            shutil.move(f, dest / f.name) if not (dest / f.name).exists() else None
    shutil.rmtree(dest / "work-gui")
    finish(dest, version, "windows-x64", say)


def archive(dest: Path, say=print) -> Path:
    """the release's archive beside its folder (without the bytecode Python writes when it runs from there)"""
    if (dest / "release.json").is_file() and "windows" in json.loads((dest / "release.json").read_text())["platform"]:
        out = dest.with_suffix(".zip")
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for f in sorted(dest.rglob("*")):
                if f.is_file() and "__pycache__" not in f.parts:
                    z.write(f, (Path(dest.name) / f.relative_to(dest)).as_posix())
    else:
        from compression import zstd
        out = dest.with_name(dest.name + ".tar.zst")
        with zstd.open(out, "wb", level=12) as f, tarfile.open(fileobj=f, mode="w") as tar:
            tar.add(dest, arcname=dest.name, filter=lambda t: None if "__pycache__" in t.name.split("/") else t)
    say("archive: %s (%.0f MB)" % (out, out.stat().st_size / 1e6))
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.release", description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=("linux", "windows", "verify"))
    ap.add_argument("folder", nargs="?", type=Path, help="verify: the release's folder")
    ap.add_argument("--version")
    ap.add_argument("--out", type=Path, default=ROOT / "work" / "release")
    ap.add_argument("--no-archive", action="store_true")
    ap.add_argument("--kit", type=Path, help="Linux: oranges.linuxkit's folder")
    ap.add_argument("--wintools", type=Path, help="Windows: oranges.wintools' folder")
    ap.add_argument("--prefix", type=Path, help="Windows: oranges.mingw's prefix")
    args = ap.parse_args(argv)
    if args.what == "verify":
        bad = verify(args.folder)
        print("\n".join(bad) if bad else "every file matches SHA256SUMS")
        return 1 if bad else 0
    version = args.version or git_version()
    dest = args.out / ("oranges-%s-%s-x64" % (version, args.what))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    if args.what == "linux":
        from . import linuxkit
        linux(dest, version, args.kit or linuxkit.DEFAULT_DIR)
    else:
        from . import mingw, wintools
        windows(dest, version, args.wintools or wintools.DEFAULT_DIR, args.prefix or mingw.DEFAULT_DIR)
    if not args.no_archive:
        archive(dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
