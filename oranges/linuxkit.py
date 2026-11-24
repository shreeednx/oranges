"""The Linux release's build kit: what a player's machine needs to build the port, made on a developer's machine
(TODO.md task 2). oranges.release copies it into a release.

    python -m oranges.linuxkit [--dir DIR]          (default ~/.local/opt/oranges-linuxkit)

DIR gets:
  toolchain/  clang, clang++ and lld from LLVM's own Linux release (pruned and stripped: the compilers, the linker,
              the archive tools, clang's headers and libc++, static), which runs on glibc 2.34 and newer (with
              the system's zlib and libstdc++, which every desktop distribution has);
  sysroot/    Ubuntu 22.04's C library (glibc 2.35), libstdc++ 12, the Linux headers and the Vulkan loader: the port
              is built against it, so it runs on glibc 2.35 and newer whatever the player's distribution is
              (libstdc++ and libgcc are linked in);
  build-sysroot/  the same, with the headers and libraries SDL3 is built with (X11, Wayland, sound, D-Bus, ...: what
              it loads at run time; its build reads their .so files for the names), which the release leaves out;
  libs/       the port's libraries, built here with that toolchain and build-sysroot: SDL3 (static; its video and sound
              back ends load their system libraries at run time, as SDL does), libopus (Ubuntu's static library) and
              FFmpeg's libavcodec / libavutil (static, the movie's decoders only, as oranges.mingw builds them for
              Windows), with lib/pkgconfig files oranges.pkgconf reads and their licence texts in share/licenses;
  python/     python-build-standalone's CPython 3.14 (glibc 2.17 and newer) with oranges' dependency (lz4).

The Vulkan loader is the player's own (libvulkan.so.1, which their GPU driver needs anyway): the port links against
the sysroot's copy and loads theirs.

Each download is checked against a SHA-256 pinned here: LLVM's and Python's from GitHub's release asset digests,
SDL's and FFmpeg's from their releases (FFmpeg's in oranges.mingw), lz4's from PyPI, Ubuntu's packages from the
release pocket's package index (frozen since April 2022, so its hash is pinned here too; the index's PGP signature
is not checked). Downloads are kept in DIR/.cache.
"""

import argparse
import hashlib
import io
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

from . import mingw, tools_dir

DEFAULT_DIR = tools_dir("oranges-linuxkit")
UBUNTU = "https://archive.ubuntu.com/ubuntu/"
INDEXES = (   # (jammy's release pocket: frozen)
    ("dists/jammy/main/binary-amd64/Packages.xz", "37cb57f1554cbfa71c5a29ee9ffee18a9a8c1782bb0568e0874b7ff4ce8f9c11"),
    ("dists/jammy/universe/binary-amd64/Packages.xz",
     "d29cb24c93fec5f43255706bce7eb46d4779952039d8c68ac1bb14a6f3655ce2"),
)
# the sysroot's packages, and what they depend on (closure()): SDL3's build reads each library's real .so file for
# the name it loads at run time, so the -dev packages' runtime libraries must be there too
SYSROOT = (
    # the C and C++ libraries, the kernel's headers
    "libc6", "libc6-dev", "linux-libc-dev", "libcrypt1", "libcrypt-dev", "libgcc-s1", "libgcc-12-dev", "libstdc++6",
    "libstdc++-12-dev",
    # the Vulkan loader (linked against; the player's is loaded), libopus (static)
    "libvulkan1", "libvulkan-dev", "libopus0", "libopus-dev",
    # SDL3's build: the headers of what it loads at run time (X11, Wayland, sound, input, D-Bus)
    "libx11-dev", "x11proto-dev", "libxcb1-dev", "libxau-dev", "libxdmcp-dev", "libxext-dev", "libxrandr-dev",
    "libxrender-dev", "libxcursor-dev", "libxi-dev", "libxfixes-dev", "libxss-dev", "libxtst-dev",
    "libxkbcommon-dev", "libwayland-dev", "wayland-protocols", "libdecor-0-dev", "libegl-dev", "libgl-dev",
    "libgles-dev", "libglx-dev", "libdrm-dev", "libgbm-dev", "libpulse-dev", "libasound2-dev",
    "libpipewire-0.3-dev", "libspa-0.2-dev", "libudev-dev", "libdbus-1-dev", "libffi-dev",
)
# the release's sysroot, what a player's build of the port links against: the C and C++ libraries and the Vulkan
# loader (SDL3, opus and FFmpeg are static in libs/, their headers there)
PLAYER_SYSROOT = ("libc6", "libc6-dev", "linux-libc-dev", "libcrypt1", "libcrypt-dev", "libgcc-s1", "libgcc-12-dev",
                  "libstdc++6", "libstdc++-12-dev", "libvulkan1", "libvulkan-dev")
# (the sanitizers' runtimes, which come with libgcc-12-dev: not used)
SANITIZERS = ("libasan", "libtsan", "liblsan", "libubsan", "libhwasan")
# (dependencies not followed: programs and their data, which a sysroot does not run)
NOT_FOLLOWED = ("perl", "perl-base", "perl-modules-5.34", "python3", "python3-minimal", "python3.10",
                "python3.10-minimal", "python3-distutils", "python3-lib2to3", "dpkg", "debconf", "tar", "pkg-config",
                "lsb-base", "readline-common", "xkb-data", "media-types", "xorg-sgml-doctools", "x11-common",
                "install-info", "sensible-utils", "init-system-helpers", "adduser", "passwd")
LLVM = ("https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.6/LLVM-23.1.6-Linux-X64.tar.xz",
        "bd4d1c434ff69ef06d19e7934c04708da677e5f2eb51f5bcf052ab91818f0094")
LLVM_KEEP = ("bin/clang", "bin/clang++", "bin/clang-23", "bin/clang-cpp", "bin/lld", "bin/ld.lld",
             "bin/llvm-ar", "bin/llvm-ranlib", "bin/llvm-nm", "bin/llvm-objdump", "bin/llvm-strip", "bin/llvm-objcopy",
             "lib/clang/", "LICENSE.TXT",
             # (LLVM's C++ library, linked in: eden needs C++20's <format>, which the sysroot's libstdc++ 12 lacks)
             "include/c++/v1/", "include/x86_64-unknown-linux-gnu/c++/v1/",
             "lib/x86_64-unknown-linux-gnu/libc++.a", "lib/x86_64-unknown-linux-gnu/libc++abi.a",
             "lib/x86_64-unknown-linux-gnu/libunwind.a")
PYTHON = ("https://github.com/astral-sh/python-build-standalone/releases/download/20261123/"
          "cpython-3.14.10%2B20261123-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
          "92102b15f5149d124b5b4b2f00f1ff2f827cf1a99dc1a22447ad52861bd80ace")
WHEELS = (
    ("https://files.pythonhosted.org/packages/f1/01/d52c7b11eaa286d49dae619c0eec4aabc0bf3cda7a7467eb77c62c4471f3/"
     "lz4-4.4.5-cp314-cp314-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl",
     "0bba042ec5a61fa77c7e380351a61cb768277801240249841defd2ff0a10742f"),
)
SDL3 = ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz",
        "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f")
TRIPLE = "x86_64-linux-gnu"


def fetch(url: str, sha256: str, cache: Path, say=print) -> Path:
    """the file at url, from the cache or downloaded, checked against its SHA-256"""
    path = cache / url.rsplit("/", 1)[1].replace("%2B", "+")
    if path.exists() and _sha256(path) == sha256:
        return path
    say("download %s" % url)
    cache.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "oranges-release-fetch"})
    h = hashlib.sha256()
    tmp = path.with_name(path.name + ".part")
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        while chunk := r.read(1 << 20):
            h.update(chunk)
            f.write(chunk)
    if h.hexdigest() != sha256:
        tmp.unlink()
        raise SystemExit("%s: SHA-256 %s, expected %s" % (url, h.hexdigest(), sha256))
    os.replace(tmp, path)
    return path


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


# ---- Ubuntu's packages ----

def read_index(text: str) -> dict[str, dict]:
    """a Packages file: {package: {"Filename", "SHA256", "Version", ...}}"""
    out = {}
    for stanza in text.split("\n\n"):
        fields, key = {}, None
        for line in stanza.splitlines():
            if line[:1] in (" ", "\t") and key:
                fields[key] += "\n" + line.strip()
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
        if "Package" in fields:
            out.setdefault(fields["Package"], fields)
    return out


def ar_members(data: bytes) -> dict[str, bytes]:
    """the members of an ar archive (a .deb)"""
    if data[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    out, pos = {}, 8
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        out[name] = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    return out


def _relative_links(tar: tarfile.TarFile, root: Path):
    """the members, with absolute symlinks made relative to the sysroot (lib/x.so -> /lib/... becomes ../...)"""
    for m in tar:
        if m.issym() and m.linkname.startswith("/"):
            here = os.path.dirname("/" + m.name.lstrip("./"))
            m.linkname = os.path.relpath(m.linkname, here)
        if m.islnk() or m.isdev():
            continue   # (no hard links or device files in what the sysroot needs)
        yield m


def unpack_deb(deb: bytes, dest: Path) -> None:
    members = ar_members(deb)
    name = next(n for n in members if n.startswith("data.tar"))
    raw = members[name]
    if name.endswith(".zst"):
        from compression import zstd
        raw = zstd.decompress(raw)
    elif name.endswith(".xz"):
        raw = lzma.decompress(raw)
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        tar.extractall(dest, members=list(_relative_links(tar, dest)), filter="data")


def closure(index: dict[str, dict], wanted) -> list[str]:
    """the packages wanted and what they depend on (Depends, Pre-Depends; an alternative's first choice; virtual
    packages through Provides), without NOT_FOLLOWED"""
    provides = {}
    for name, p in index.items():
        for prov in re.split(r",\s*", p.get("Provides", "")):
            if prov:
                provides.setdefault(prov.split()[0], name)
    seen, todo = [], list(wanted)
    while todo:
        name = todo.pop(0)
        name = name if name in index else provides.get(name, name)
        if name in seen or name in NOT_FOLLOWED:
            continue
        if name not in index:
            raise SystemExit("%s: not in Ubuntu 22.04's index" % name)
        seen.append(name)
        for field in ("Pre-Depends", "Depends"):
            for dep in re.split(r",\s*", index[name].get(field, "")):
                if dep:
                    todo.append(dep.split("|")[0].split()[0].split(":")[0])
    return seen


def make_sysroot(dest: Path, cache: Path, say=print, wanted=SYSROOT) -> list[str]:
    """the packages wanted and their dependencies unpacked into dest: SYSROOT into the kit's build-sysroot/ (the
    libraries are built against it), PLAYER_SYSROOT into sysroot/ (the release's)"""
    index = {}
    for path, sha in INDEXES:
        f = fetch(UBUNTU + path, sha, cache / path.split("/")[2], say)
        index.update({k: v for k, v in read_index(lzma.decompress(f.read_bytes()).decode()).items() if k not in index})
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    got = []
    for name in closure(index, wanted):
        p = index[name]
        deb = fetch(UBUNTU + p["Filename"], p["SHA256"], cache / "debs", say)
        unpack_deb(deb.read_bytes(), dest)
        got.append("%s %s" % (name, p["Version"]))
    for d in ("usr/share/doc", "usr/share/man", "usr/share/lintian", "usr/share/info", "usr/bin", "usr/sbin",
              "sbin", "usr/lib/x86_64-linux-gnu/gconv", "usr/share/locale", "usr/libexec"):
        shutil.rmtree(dest / d, ignore_errors=True)
    for f in list((dest / "usr" / "lib").rglob("*")):
        if f.name.startswith(SANITIZERS) and (f.is_file() or f.is_symlink()):
            f.unlink()
    (dest / "PACKAGES").write_text("".join(g + "\n" for g in got))
    return got


# ---- LLVM ----

def make_toolchain(dest: Path, cache: Path, say=print) -> None:
    archive = fetch(*LLVM, cache, say)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    say("unpack %s (the compilers, the linker, clang's headers)" % archive.name)
    with tarfile.open(archive) as tar:
        keep = []
        for m in tar:
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            if any(rel == k or (k.endswith("/") and rel.startswith(k)) for k in LLVM_KEEP):
                m.name = rel
                keep.append(m)
        tar.extractall(dest, members=keep, filter="data")
    for exe in ("clang-23", "lld", "llvm-ar", "llvm-nm", "llvm-objdump", "llvm-objcopy"):   # (-13%: unstripped)
        subprocess.run([str(dest / "bin" / "llvm-strip"), str(dest / "bin" / exe)], check=True)
    # (compiler-rt's sanitizers and profile runtimes: not needed, most of lib/clang's size)
    for d in (dest / "lib" / "clang").glob("*/lib/*"):
        for f in d.iterdir():
            if not f.name.startswith(("libclang_rt.builtins",)):
                f.unlink() if f.is_file() else shutil.rmtree(f)


# ---- Python ----

def make_python(dest: Path, cache: Path, say=print) -> None:
    archive = fetch(*PYTHON, cache, say)
    if dest.exists():
        shutil.rmtree(dest)
    with tarfile.open(archive) as tar:   # (its one folder, python/)
        tar.extractall(dest.parent, filter="data")
    site = next((dest / "lib").glob("python3.*")) / "site-packages"
    for url, sha in WHEELS:
        with zipfile.ZipFile(fetch(url, sha, cache, say)) as z:
            z.extractall(site)
    for junk in ("lib/python3.14/test", "lib/python3.14/idlelib", "lib/python3.14/tkinter", "lib/python3.14/turtledemo",
                 "lib/python3.14/ensurepip", "include"):
        shutil.rmtree(dest / junk, ignore_errors=True)


# ---- the libraries ----

def env_for(kit: Path, sysroot_name: str = "sysroot") -> dict:
    """the environment that builds for the release's Linux: its clang against its sysroot (build-sysroot: the
    libraries' builds)"""
    sysroot, bin_ = kit / sysroot_name, kit / "toolchain" / "bin"
    return dict(os.environ, CC=str(bin_ / "clang"), CXX=str(bin_ / "clang++"), AR=str(bin_ / "llvm-ar"),
                RANLIB=str(bin_ / "llvm-ranlib"), NM=str(bin_ / "llvm-nm"), ORANGES_SYSROOT=str(sysroot),
                ORANGES_PREFIX=str(kit / "libs"),
                PKG_CONFIG_SYSROOT_DIR=str(sysroot),
                PKG_CONFIG_LIBDIR=os.pathsep.join(str(sysroot / d) for d in (
                    "usr/lib/x86_64-linux-gnu/pkgconfig", "usr/share/pkgconfig", "usr/lib/pkgconfig")))


def _run(cmd: list[str], cwd: Path, env: dict, what: str) -> None:
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("%s failed:\n%s%s" % (what, r.stdout[-4000:], r.stderr[-4000:]))


def build_sdl3(kit: Path, cache: Path, say=print) -> None:
    archive = fetch(*SDL3, cache, say)
    src_root = cache / "src"
    src = src_root / archive.name.removesuffix(".tar.gz")
    if not src.is_dir():
        src_root.mkdir(exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(src_root, filter="data")
    build = cache / ("build-" + src.name)
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    env = env_for(kit, "build-sysroot")
    sysroot = kit / "build-sysroot"
    flags = "--sysroot=%s" % sysroot
    say("build %s (static)" % src.name)
    _run(["cmake", "-S", str(src), "-B", str(build), "-G", "Ninja" if shutil.which("ninja") else "Unix Makefiles",
          "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + str(kit / "libs"), "-DCMAKE_INSTALL_LIBDIR=lib",
          "-DCMAKE_C_COMPILER=" + env["CC"], "-DCMAKE_CXX_COMPILER=" + env["CXX"], "-DCMAKE_AR=" + env["AR"],
          "-DCMAKE_RANLIB=" + env["RANLIB"], "-DCMAKE_SYSROOT=" + str(sysroot),
          "-DCMAKE_FIND_ROOT_PATH=" + str(sysroot), "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
          "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
          "-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags, "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld",
          "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
          "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL_DOCS=OFF",
          "-DSDL_DEPS_SHARED=ON", "-DSDL_X11=ON", "-DSDL_WAYLAND=ON", "-DSDL_PIPEWIRE=ON", "-DSDL_PULSEAUDIO=ON",
          "-DSDL_ALSA=ON", "-DSDL_VULKAN=ON", "-DSDL_OPENGL=ON", "-DSDL_HIDAPI=ON", "-DSDL_JACK=OFF",
          "-DSDL_HIDAPI_LIBUSB=OFF",   # (gamepads through hidraw and udev, as SDL does without libusb on Linux)
          "-DSDL_SNDIO=OFF", "-DSDL_KMSDRM=OFF", "-DSDL_RPI=OFF", "-DSDL_VIVANTE=OFF", "-DSDL_OFFSCREEN=ON",
          "-DSDL_LIBURING=OFF", "-DSDL_CAMERA=OFF"], build, env, "SDL3's cmake")
    _run(["cmake", "--build", str(build), "-j", str(os.cpu_count() or 4)], build, env, "SDL3")
    _run(["cmake", "--install", str(build)], build, env, "SDL3's install")
    lic = kit / "libs" / "share" / "licenses" / "SDL3"
    lic.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE.txt", lic / "LICENSE.txt")


def install_opus(kit: Path) -> None:
    """Ubuntu's libopus.a and headers (from the sysroot) into libs/, with a .pc file for static linking"""
    libs, sysroot = kit / "libs", kit / "build-sysroot"
    (libs / "include").mkdir(parents=True, exist_ok=True)
    (libs / "lib" / "pkgconfig").mkdir(parents=True, exist_ok=True)
    shutil.copytree(sysroot / "usr" / "include" / "opus", libs / "include" / "opus", dirs_exist_ok=True)
    shutil.copy2(sysroot / "usr" / "lib" / TRIPLE / "libopus.a", libs / "lib" / "libopus.a")
    (libs / "lib" / "pkgconfig" / "opus.pc").write_text(
        "prefix=/libs\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: Opus\nDescription: Opus IETF "
        "audio codec (Ubuntu 22.04's static library)\nVersion: 1.3.1\nLibs: ${libdir}/libopus.a -lm\n"
        "Cflags: -I${includedir}/opus\n")
    lic = libs / "share" / "licenses" / "opus"
    lic.mkdir(parents=True, exist_ok=True)
    shutil.copy2(sysroot / "usr" / "share" / "doc" / "libopus0" / "copyright", lic / "copyright") \
        if (sysroot / "usr" / "share" / "doc" / "libopus0" / "copyright").exists() else None


def build_ffmpeg(kit: Path, cache: Path, say=print) -> None:
    """FFmpeg's libavcodec and libavutil, static, with only the movie's decoders (oranges.mingw's choice)"""
    tools = cache / "tools"
    nasm = tools / "nasm"
    env = env_for(kit, "build-sysroot")
    if not nasm.exists():   # (nasm for this machine, as oranges.mingw builds it)
        src = mingw._source(cache, "nasm", say)
        say("build nasm")
        host = dict(os.environ)
        _run(["./configure", "-q"], src, host, "nasm's configure")
        _run(["make", "-j" + str(os.cpu_count() or 4), "nasm"], src, host, "nasm")
        tools.mkdir(exist_ok=True)
        shutil.copy2(src / "nasm", nasm)
    src = mingw._source(cache, "ffmpeg", say)
    build = cache / ("build-linux-" + src.name)
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    env["PATH"] = os.pathsep.join([str(tools), env.get("PATH", "")])
    sysroot = kit / "build-sysroot"
    say("build %s (static; decoders %s)" % (src.name, mingw.FFMPEG_DECODERS))
    _run([str(src / "configure"), "--prefix=" + str(kit / "libs"), "--cc=" + env["CC"], "--cxx=" + env["CXX"],
          "--ar=" + env["AR"], "--ranlib=" + env["RANLIB"], "--nm=" + env["NM"], "--x86asmexe=nasm",
          "--sysroot=" + str(sysroot), "--extra-cflags=--sysroot=" + str(sysroot), "--enable-pic",
          "--extra-ldflags=--sysroot=%s -fuse-ld=lld" % sysroot, "--pkg-config=false",
          "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-network",
          "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swresample",
          "--disable-swscale", "--enable-static", "--disable-shared", "--enable-pthreads",
          "--enable-decoder=" + mingw.FFMPEG_DECODERS, "--enable-parser=" + mingw.FFMPEG_DECODERS], build, env,
         "FFmpeg's configure")
    _run(["make", "-j" + str(os.cpu_count() or 4)], build, env, "FFmpeg")
    _run(["make", "install"], build, env, "FFmpeg's install")
    lic = kit / "libs" / "share" / "licenses" / "ffmpeg"
    lic.mkdir(parents=True, exist_ok=True)
    for f in ("LICENSE.md", "COPYING.LGPLv2.1"):
        shutil.copy2(src / f, lic / f)


def finish_libs(kit: Path) -> None:
    """libs/ for static linking: each .pc file's Libs.private folded into its Libs (oranges.pkgconf reads Libs, as
    for shared libraries), libatomic static, and a vulkan.pc for the sysroot's loader (its headers are
    third_party/khronos's)"""
    pcdir = kit / "libs" / "lib" / "pkgconfig"
    for pc in pcdir.glob("*.pc"):
        lines = pc.read_text().splitlines()
        private = [ln.split(":", 1)[1].strip() for ln in lines if ln.startswith("Libs.private:")]
        out = []
        for ln in lines:
            if ln.startswith("Libs.private:"):
                continue
            if ln.startswith("Libs:") and private:
                ln = ln + " " + " ".join(private)
            if ln.startswith("Libs:"):   # (libatomic linked in: not every distribution installs libatomic.so.1)
                ln = ln.replace(" -latomic", " -l:libatomic.a")
            out.append(ln)
        pc.write_text("\n".join(out) + "\n")
    (pcdir / "vulkan.pc").write_text("Name: Vulkan-Loader\nDescription: the player's Vulkan loader (linked against "
                                     "the sysroot's)\nVersion: 1.3.204\nLibs: -lvulkan\nCflags:\n")


def install(kit: Path, say=print, parts: tuple[str, ...] = ("sysroot", "toolchain", "python", "libs")) -> None:
    cache = kit / ".cache"
    if "sysroot" in parts:
        got = make_sysroot(kit / "build-sysroot", cache, say, SYSROOT)
        say("build-sysroot: %d packages" % len(got))
        got = make_sysroot(kit / "sysroot", cache, say, PLAYER_SYSROOT)
        say("sysroot: %d packages" % len(got))
    if "toolchain" in parts:
        make_toolchain(kit / "toolchain", cache, say)
    if "python" in parts:
        make_python(kit / "python", cache, say)
    if "libs" in parts:
        shutil.rmtree(kit / "libs", ignore_errors=True)
        install_opus(kit)
        build_sdl3(kit, cache, say)
        build_ffmpeg(kit, cache, say)
        finish_libs(kit)
    say("kit: %s" % kit)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.linuxkit", description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR, help="where (default %(default)s)")
    ap.add_argument("parts", nargs="*", default=["sysroot", "toolchain", "python", "libs"],
                    help="sysroot, toolchain, python, libs (default: all)")
    args = ap.parse_args(argv)
    install(args.dir, parts=tuple(args.parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
