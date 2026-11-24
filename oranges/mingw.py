"""The Windows build's libraries: MSYS2's CLANG64 packages, fetched and unpacked into a prefix, and FFmpeg built there.

MSYS2's clang64 repository builds its packages with clang, libc++ and UCRT, as llvm-mingw does, so the program and
the libraries share one C and C++ runtime. This reads the repository's database (packages, their files' SHA-256s,
dependencies), takes the dependency closure of what is asked for, downloads each package, checks its SHA-256 against
the database and unpacks its clang64/ tree into the prefix. oranges/build.py then finds them with
ORANGES_MINGW_PREFIX=<prefix> (pkg-config --define-prefix on <prefix>/lib/pkgconfig), or in the default prefix
without it.

FFmpeg is not MSYS2's: theirs pulls in 124 packages (Python, llama.cpp, x265, ...), and the movie player needs three
decoders. It is built from FFmpeg's release with llvm-mingw (ORANGES_MINGW, default /opt/llvm-mingw): libavcodec and
libavutil as DLLs, the VP8, VP9 and H.264 decoders and parsers only (nn_movie.c's), LGPL, with its x86 assembly (VP9
is several times slower without), for which nasm is built from source first, for this machine.

    python -m oranges.mingw [--prefix DIR] [--no-ffmpeg] [package ...]   (default: the libraries the build uses)

The database comes over HTTPS from repo.msys2.org; its PGP signature is not checked. The two source releases are
checked against SHA-256s pinned here (FFmpeg's was checked against its release key's signature when pinned).
Downloads and builds are kept in <prefix>/.cache, so a second run only fetches what changed. The installed packages
are listed in <prefix>/.packages.
"""

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from compression import zstd
from pathlib import Path

REPO = "https://repo.msys2.org/mingw/clang64/"
PREFIX = "mingw-w64-clang-x86_64-"
DEFAULT_DIR = Path.home() / ".local" / "opt" / "msys2-clang64"
# (SDL3: the window, input and sound; the Vulkan loader; libopus: the Opus decoder; FFmpeg: the opening movie)
LIBRARIES = ("sdl3", "vulkan-loader", "vulkan-headers", "opus")
SOURCES = {   # (url, SHA-256)
    "nasm": ("https://www.nasm.us/pub/nasm/releasebuilds/3.02/nasm-3.02.tar.xz",
             "87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d"),
    "ffmpeg": ("https://ffmpeg.org/releases/ffmpeg-9.1.tar.xz",
               "1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433"),
}
FFMPEG_DECODERS = "vp8,vp9,h264"


def _get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "oranges-mingw-fetch"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def read_db(data: bytes) -> dict[str, dict]:
    """the repository database: {package name: {"filename", "sha256", "depends", "provides", "version"}}"""
    raw = zstd.decompress(data) if data[:4] == b"\x28\xb5\x2f\xfd" else data
    out = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for m in tar:
            if not m.isfile() or not m.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in tar.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line[1:-1]
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            out[name] = {"filename": fields["FILENAME"][0], "sha256": fields["SHA256SUM"][0],
                         "version": fields["VERSION"][0], "depends": fields.get("DEPENDS", []),
                         "provides": fields.get("PROVIDES", [])}
    return out


def _bare(dep: str) -> str:
    return re.split(r"[<>=]", dep, maxsplit=1)[0]


def closure(db: dict[str, dict], wanted: list[str]) -> list[str]:
    """the packages wanted and everything they depend on, by name (provides resolved)"""
    provides = {}
    for name, p in db.items():
        for prov in p["provides"]:
            provides.setdefault(_bare(prov), name)
    seen, todo = [], [w if w.startswith(PREFIX) else PREFIX + w for w in wanted]
    while todo:
        name = todo.pop()
        name = name if name in db else provides.get(name, name)
        if name in seen:
            continue
        if name not in db:
            raise SystemExit("%s: no such package in the repository" % name)
        seen.append(name)
        todo += [_bare(d) for d in db[name]["depends"]]
    return sorted(seen)


def install(prefix: Path, wanted: list[str], say=print) -> list[str]:
    prefix.mkdir(parents=True, exist_ok=True)
    cache = prefix / ".cache"
    cache.mkdir(exist_ok=True)
    db = read_db(_get(REPO + "clang64.db"))
    names = closure(db, wanted)
    say("%d packages: %s" % (len(names), " ".join(n.removeprefix(PREFIX) for n in names)))
    for name in names:
        p = db[name]
        pkg = cache / p["filename"]
        if not (pkg.exists() and hashlib.sha256(pkg.read_bytes()).hexdigest() == p["sha256"]):
            data = _get(REPO + p["filename"])
            if hashlib.sha256(data).hexdigest() != p["sha256"]:
                raise SystemExit("%s: the download's SHA-256 is not the database's" % p["filename"])
            pkg.write_bytes(data)
            say("  fetched %s (%.1f MB)" % (p["filename"], len(data) / 1e6))
        raw = pkg.read_bytes()
        raw = zstd.decompress(raw) if raw[:4] == b"\x28\xb5\x2f\xfd" else raw
        with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
            members = []
            for m in tar:   # (only the clang64/ tree, without its leading folder; nothing that leaves the prefix)
                if not m.name.startswith("clang64/") or m.name == "clang64/":
                    continue
                m.name = m.name[len("clang64/"):]
                members.append(m)
            tar.extractall(prefix, members=members, filter="data")
    (prefix / ".packages").write_text("".join("%s %s\n" % (n, db[n]["version"]) for n in names))
    return names


def _source(cache: Path, name: str, say=print) -> Path:
    """a source release, downloaded (checked against its pinned SHA-256) and unpacked into cache/src: its folder"""
    url, sha = SOURCES[name]
    tarball = cache / url.rsplit("/", 1)[1]
    if not (tarball.exists() and hashlib.sha256(tarball.read_bytes()).hexdigest() == sha):
        data = _get(url)
        if hashlib.sha256(data).hexdigest() != sha:
            raise SystemExit("%s: the download's SHA-256 is not the one pinned" % url)
        tarball.write_bytes(data)
        say("  fetched %s (%.1f MB)" % (tarball.name, len(data) / 1e6))
    src = cache / "src"
    folder = src / tarball.name.removesuffix(".tar.xz")
    if not folder.is_dir():
        src.mkdir(exist_ok=True)
        with tarfile.open(tarball) as tar:
            tar.extractall(src, filter="data")
    return folder


def _run(cmd: list[str], cwd: Path, env: dict, what: str):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("%s failed:\n%s%s" % (what, r.stdout[-3000:], r.stderr[-3000:]))


def build_ffmpeg(prefix: Path, say=print) -> None:
    """FFmpeg's libavcodec and libavutil for MinGW, into the prefix (nasm built first, for this machine)"""
    cache = prefix / ".cache"
    cache.mkdir(parents=True, exist_ok=True)
    mingw = Path(os.environ.get("ORANGES_MINGW", "/opt/llvm-mingw")) / "bin"
    if not shutil.which("x86_64-w64-mingw32-clang", path=str(mingw)) and not shutil.which("x86_64-w64-mingw32-clang"):
        raise SystemExit("x86_64-w64-mingw32-clang not found: set ORANGES_MINGW to llvm-mingw's folder")
    jobs = str(os.cpu_count() or 4)
    tools = cache / "tools"
    nasm = tools / "nasm"
    if not nasm.exists():
        src = _source(cache, "nasm", say)
        say("  building nasm")
        env = dict(os.environ)
        _run(["./configure", "-q"], src, env, "nasm's configure")
        _run(["make", "-j" + jobs, "nasm"], src, env, "nasm")
        tools.mkdir(exist_ok=True)
        shutil.copy2(src / "nasm", nasm)
    src = _source(cache, "ffmpeg", say)
    build = cache / ("build-" + src.name)
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    env = dict(os.environ, PATH=os.pathsep.join([str(mingw), str(tools), os.environ.get("PATH", "")]))
    say("  building %s (decoders %s)" % (src.name, FFMPEG_DECODERS))
    _run([str(src / "configure"), "--prefix=" + str(prefix), "--enable-cross-compile", "--arch=x86_64",
          "--target-os=mingw32", "--cross-prefix=x86_64-w64-mingw32-", "--cc=x86_64-w64-mingw32-clang",
          "--cxx=x86_64-w64-mingw32-clang++", "--x86asmexe=nasm", "--pkg-config=false",
          "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-network",
          "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swresample",
          "--disable-swscale", "--enable-shared", "--disable-static", "--enable-w32threads",
          "--enable-decoder=" + FFMPEG_DECODERS, "--enable-parser=" + FFMPEG_DECODERS], build, env,
         "FFmpeg's configure")
    _run(["make", "-j" + jobs], build, env, "FFmpeg")
    _run(["make", "install"], build, env, "FFmpeg's install")
    licenses = prefix / "share" / "licenses" / "ffmpeg"   # (where MSYS2's packages keep theirs)
    licenses.mkdir(parents=True, exist_ok=True)
    for f in ("LICENSE.md", "COPYING.LGPLv2.1"):
        shutil.copy2(src / f, licenses / f)
    with open(prefix / ".packages", "a") as f:
        f.write("%s (built: libavcodec, libavutil; %s)\n" % (src.name, FFMPEG_DECODERS))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.mingw", description=__doc__.split("\n\n")[0])
    ap.add_argument("--prefix", default=os.environ.get("ORANGES_MINGW_PREFIX") or str(DEFAULT_DIR))
    ap.add_argument("--no-ffmpeg", action="store_true", help="leave FFmpeg out (the opening movie is skipped)")
    ap.add_argument("packages", nargs="*", help="package names without %s (default: %s)" % (PREFIX,
                                                                                            " ".join(LIBRARIES)))
    args = ap.parse_args(argv)
    names = install(Path(args.prefix), args.packages or list(LIBRARIES))
    if not args.no_ffmpeg and not args.packages:
        build_ffmpeg(Path(args.prefix))
    print("%d packages in %s: build with ORANGES_MINGW_PREFIX=%s" % (len(names), args.prefix, args.prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
