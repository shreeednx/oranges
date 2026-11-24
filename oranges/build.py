"""Compile the lifted C (<work>/gen) with a64-kit's runtime (rt/) and the host (host/), and link the program.

Objects go to <work>/build/obj and are rebuilt only when their source, the headers they include or the flags changed
(a stamp beside each object). Compiles run in parallel, as many as fit in memory (a64-kit's toolchain.default_jobs;
ORANGES_JOBS overrides). Lifted chunks build at -O1 with the flags Longshot uses (nx2/build.py flags()). The big ones
are FP-exact (-ffp-contract=off) and LoopRotate is capped, which a64-kit measured as a third of a chunk's compile time.

    ORANGES_MARCH  CPU level for every object (default x86-64-v3)
    ORANGES_OPT    optimization level of the lifted chunks (default O1)
    ORANGES_VULKAN 0: without the Vulkan back end (host/vk and eden's recompiler, ~160 C++ files)
    ORANGES_TARGET windows: the Windows build (or `oranges build --target windows`), see below
    ORANGES_PREFIX the libraries' prefix (its lib/pkgconfig, read by oranges.pkgconf), for either target
    ORANGES_SYSROOT the C library to build against (a Linux release's: oranges.linuxkit), with libstdc++ and libgcc
                   linked in and $ORIGIN as the program's library path

The Windows build is cross-compiled with llvm-mingw (MinGW-w64 on clang, UCRT): x86_64-w64-mingw32-clang from PATH,
else from ORANGES_MINGW (default /opt/llvm-mingw). It goes to <work>/build-windows/oranges.exe, with its own objects.
Its libraries (SDL3, the Vulkan loader, libopus, FFmpeg) come from a MinGW prefix: ORANGES_MINGW_PREFIX, else
~/.local/opt/msys2-clang64 (on Windows %LOCALAPPDATA%\\oranges\\msys2-clang64) when it exists (`python -m
oranges.mingw` makes it), whose lib/pkgconfig files
oranges.pkgconf reads (no pkg-config needed, and the prefix may have moved); else pkg-config: MSYS2's own on Windows,
x86_64-w64-mingw32-pkg-config across. Without them it builds headless (the null back end). winpthreads and libc++
are linked in statically, so the program needs only those libraries' DLLs beside it.
Run on Windows itself, the target is windows, and clang, llvm-objdump and windres come from PATH or ORANGES_MINGW's
bin: llvm-mingw's Windows release (`python -m oranges.wintools` fetches it, with a Python to run oranges on) or
MSYS2's CLANG64 shell.
"""

import functools
import hashlib
import os
import shutil
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from . import kit, pkgconf, tools_dir

import toolchain  # noqa: E402,I001  (the kit's)

ROOT = Path(kit.ROOT)
HOST = ROOT / "host"
RT = Path(kit.KIT) / "rt"


TARGETS = ("linux", "windows")
# (a compiler's exit when Windows ended it for want of memory: STATUS_NO_MEMORY, as an unsigned exit code; on Linux
# the OOM killer's SIGKILL is a negative returncode)
KILLED_EXITS = (0xC0000017,)
MINGW_TRIPLE = "x86_64-w64-mingw32"
MINGW_PREFIX = tools_dir("msys2-clang64")   # (python -m oranges.mingw's default)


def target() -> str:
    """the system the program is built for: ORANGES_TARGET (build() sets it from --target), else the one this runs
    on"""
    t = os.environ.get("ORANGES_TARGET", "windows" if os.name == "nt" else "linux")
    if t not in TARGETS:
        raise SystemExit("ORANGES_TARGET is %r: one of %s" % (t, ", ".join(TARGETS)))
    return t


def windows() -> bool:
    return target() == "windows"


def _tool(name: str) -> str:
    """a compiler: from PATH, and for the Windows build also from ORANGES_MINGW's bin (default /opt/llvm-mingw)"""
    path = shutil.which(name)
    if not path and windows():
        path = shutil.which(name, path=str(Path(os.environ.get("ORANGES_MINGW", "/opt/llvm-mingw")) / "bin"))
    if not path:
        raise SystemExit("%s not found (set %s%s)" % (
            name, "CXX" if "++" in name else "CC", ", or ORANGES_MINGW to llvm-mingw's folder" if windows() else ""))
    return path


def _cross() -> bool:
    """whether the build is a cross build (the Windows program built on Linux)"""
    return windows() and os.name != "nt"


def cc() -> str:   # (on Windows, MSYS2's CLANG64 clang targets MinGW itself)
    return _tool(os.environ.get("CC", MINGW_TRIPLE + "-clang" if _cross() else "clang"))


def cxx() -> str:
    return _tool(os.environ.get("CXX", MINGW_TRIPLE + "-clang++" if _cross() else "clang++"))


def out_dir(work: Path) -> Path:
    """where the objects and the program go: <work>/build, <work>/build-windows for the Windows build"""
    return work / ("build-windows" if windows() else "build")


def exe_name() -> str:
    return "oranges.exe" if windows() else "oranges"


def mingw_prefix() -> Path | None:
    """the target's library prefix, whose lib/pkgconfig files oranges.pkgconf reads: ORANGES_PREFIX (a release's
    libs/, oranges.linuxkit's or oranges.mingw's), else for the Windows build ORANGES_MINGW_PREFIX, else
    oranges.mingw's default folder when it is there; None: pkg-config (the Linux build on the system's libraries)"""
    if os.environ.get("ORANGES_PREFIX"):
        return Path(os.environ["ORANGES_PREFIX"])
    if not windows():
        return None
    if os.environ.get("ORANGES_MINGW_PREFIX"):
        return Path(os.environ["ORANGES_MINGW_PREFIX"])
    return MINGW_PREFIX if MINGW_PREFIX.is_dir() else None


def _pkg_config() -> list[str] | None:
    """the pkg-config command for the target's libraries when there is no prefix, or None: none to be had"""
    if not _cross():   # (Linux; on Windows MSYS2's own pkg-config, which knows its CLANG64 libraries)
        return ["pkg-config"]
    return [MINGW_TRIPLE + "-pkg-config"] if shutil.which(MINGW_TRIPLE + "-pkg-config") else None


def vulkan() -> bool:
    """whether the Vulkan back end is built (host/vk, with eden's recompiler): the system has a Vulkan loader, and
    SDL3 for the window it presents to; ORANGES_VULKAN=0 leaves it out (the null back end draws nothing)"""
    if os.environ.get("ORANGES_VULKAN") == "0":
        return False
    return bool(optional_library("vulkan")[1]) and bool(optional_library("sdl3")[1])


def target_flags() -> list[str]:
    """compile flags every object gets for the target system: a release's sysroot (ORANGES_SYSROOT: the C library
    the program is built against, older than the player's, oranges.release)"""
    sysroot = os.environ.get("ORANGES_SYSROOT")
    return ["--sysroot=" + sysroot] if sysroot else []


def target_cxx_flags() -> list[str]:
    """C++ flags for the target system: with a release's sysroot, LLVM's libc++ (the toolchain's: the sysroot's
    libstdc++ 12 has no <format>, which eden uses)"""
    return ["-stdlib=libc++"] if os.environ.get("ORANGES_SYSROOT") and not windows() else []


def target_link_flags() -> list[str]:
    """link flags for the target system: with a release's sysroot, libc++ and the C runtime's support linked in (the
    player's system may have none, or an older one) and the libraries shipped beside the program found there
    ($ORIGIN)"""
    if windows() or not os.environ.get("ORANGES_SYSROOT"):
        return []
    return target_flags() + ["-stdlib=libc++", "-static-libstdc++", "-lc++abi", "-static-libgcc", "-Wl,-rpath,$ORIGIN",
                             "-Wl,--as-needed"]


def cxx_flags(gen: Path) -> list[str]:
    return target_flags() + target_cxx_flags() + [
            "-std=c++20", "-O2", "-march=" + os.environ.get("ORANGES_MARCH", "x86-64-v3"), "-fno-strict-aliasing",
            "-g0", "-w", "-I" + str(EDEN), "-I" + str(EDEN / "compat"), "-I" + str(EDEN / "sirit" / "include"),
            "-I" + str(EDEN / "sirit" / "src"), "-I" + str(KHRONOS), "-I" + str(HOST), "-I" + str(gen),
            "-I" + str(RT)]


def optional_library(name: str) -> tuple[list[str], list[str]]:
    """(compile flags, link flags) of a library pkg-config knows for the target, or ([], []): the host builds
    without it"""
    return _optional_library(name, target())


@functools.cache
def _optional_library(name: str, _target: str) -> tuple[list[str], list[str]]:
    prefix = mingw_prefix()
    if prefix:
        try:
            cflags, libs = pkgconf.flags(name.split(), [prefix / "lib" / "pkgconfig"])
        except LookupError:
            return [], []
    else:
        cmd = _pkg_config()
        if cmd is None:
            return [], []
        try:
            cflags = subprocess.run(cmd + ["--cflags"] + name.split(), capture_output=True, text=True,
                                    check=True).stdout.split()
            libs = subprocess.run(cmd + ["--libs"] + name.split(), capture_output=True, text=True,
                                  check=True).stdout.split()
        except (OSError, subprocess.CalledProcessError):
            return [], []
    # (SDL3's -mwindows: link_flags chooses the subsystem, with or without SDL3)
    return cflags, [f for f in libs if f != "-mwindows"]


OPTIONAL_LIBRARIES = {"opus": "HOS_HAVE_OPUS", "sdl3": "HOS_HAVE_SDL3",    # (pkg-config name: the macro host/ tests)
                      "vulkan": "HOS_HAVE_VULKAN", "libavcodec libavutil": "HOS_HAVE_FFMPEG"}
EDEN = ROOT / "third_party" / "eden"         # eden's shader recompiler and sirit (C++, with the Vulkan back end)
KHRONOS = ROOT / "third_party" / "khronos" / "include"
IMGUI = ROOT / "third_party" / "imgui"           # Dear ImGui: the settings menu (host/vk/vk_menu.cpp)
IMGUI_SOURCES = ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                 "backends/imgui_impl_vulkan.cpp")
IMGUI_FLAGS = ["-I" + str(IMGUI), "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS", "-DIMGUI_DISABLE_WIN32_FUNCTIONS",
               "-DIMGUI_DISABLE_DEMO_WINDOWS", "-DIMGUI_DISABLE_DEBUG_TOOLS"]


def host_libraries() -> tuple[list[str], list[str]]:
    cflags, libs = [], []
    for name, macro in OPTIONAL_LIBRARIES.items():
        c, ln = optional_library(name)
        if name == "vulkan" and not vulkan():
            continue
        if ln:
            cflags += ["-D" + macro] + c
            libs += ln
    return cflags, libs


def common_flags(gen: Path) -> list[str]:
    return target_flags() + [
            "-march=" + os.environ.get("ORANGES_MARCH", "x86-64-v3"), "-ffp-contract=off", "-fno-strict-aliasing",
            "-mcx16", "-maes", "-g0", "-Wno-everything", "-I" + str(gen), "-I" + str(RT)]   # (-maes: insn.aes)


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
    out += [(gen / n, support) for n in ("a64_table.c", "a64_hooks.c", "a64_suyu.c", "a64_stubs.c", "hos_image.c",
                                         "hos_symbols.c")
            if (gen / n).exists()]
    host = (["-I" + str(HOST)] + support + ["-g"]   # (debug info for the OS layer: cheap, and gdb can read its state)
            + (["-D_FILE_OFFSET_BITS=64"] if windows() else [])   # (64-bit off_t and st_size: MinGW's are 32)
            + host_libraries()[0])
    out += [(RT / "a64_rt.c", support)] + [(c, host) for c in sorted(HOST.glob("*.c"))]
    if vulkan():   # (C++: eden's recompiler, Dear ImGui and the back end; the back end with debug info, as host/)
        out += [(c, cxx_flags(gen)) for c in sorted(EDEN.rglob("*.cpp"))]
        out += [(IMGUI / f, cxx_flags(gen) + IMGUI_FLAGS) for f in IMGUI_SOURCES]
        out += [(c, cxx_flags(gen) + IMGUI_FLAGS + ["-g"] + host_libraries()[0])
                for c in sorted((HOST / "vk").glob("*.cpp"))]
    return out


_digests: dict[tuple[Path, int, int], bytes] = {}


def _digest(path: Path) -> bytes:
    """A file's content hash (by content: a lift rewrites every file, mostly with the same text)"""
    st = path.stat()
    k = (path, st.st_size, st.st_mtime_ns)
    if k not in _digests:
        _digests[k] = hashlib.sha256(path.read_bytes()).digest()
    return _digests[k]


def _places(gen: Path) -> list[tuple[str, str]]:
    """the folders a key names by a placeholder: moving the work folder, the tree or a release's tools (a player
    moving their oranges or build folder) changes no object (the chunks have no debug info; the host's only names
    its sources' paths in it)"""
    out = [(os.environ.get("ORANGES_SYSROOT", ""), "<sysroot>"), (os.environ.get("ORANGES_PREFIX", ""), "<prefix>"),
           (str(gen.parent), "<work>"), (str(ROOT), "<root>")]
    return [(path, token) for path, token in out if path]


def _neutral(text: str, places: list[tuple[str, str]]) -> str:
    for path, token in places:
        text = text.replace(path, token)
    return text


def _key(src: Path, flags: list[str], gen: Path) -> str:
    """What an object depends on: its flags, its source and the headers it includes (where folders are, left out)"""
    places = _places(gen)
    h = hashlib.sha256(_neutral("\0".join(flags), places).encode())
    deps = [src, RT / "a64.h", gen / "a64_suyu.h", gen / "a64_fpx.h"]
    if HOST in src.parents:   # (every host header and include file: few, and cheap to hash)
        deps += sorted(p for p in HOST.rglob("*") if p.suffix in (".h", ".inc"))
    if EDEN in src.parents or src.parent == HOST / "vk":   # (and eden's headers)
        deps += sorted(p for p in EDEN.rglob("*") if p.suffix in (".h", ".hpp", ".inc"))
    if IMGUI in src.parents or src.parent == HOST / "vk":   # (and Dear ImGui's)
        deps += sorted(p for p in IMGUI.rglob("*.h"))
    for dep in deps:
        if dep.exists():
            h.update(_neutral(str(dep), places).encode() + b"\0" + _digest(dep))
    return h.hexdigest()


def _obj(src: Path, obj_dir: Path) -> Path:
    if EDEN in src.parents:   # (by path: eden and sirit repeat file names)
        return obj_dir / ("eden." + "_".join(src.relative_to(EDEN).with_suffix("").parts) + ".o")
    if src.parent == HOST / "vk":
        return obj_dir / ("vk." + src.stem + ".o")
    if IMGUI in src.parents:
        return obj_dir / ("imgui." + src.stem + ".o")
    return obj_dir / (src.stem + (".rt" if src.parent in (RT, HOST) else "") + ".o")


def link_flags() -> list[str]:
    """the system libraries and linker options after the objects"""
    if not windows():
        return ["-lm", "-lpthread"]
    # (winpthreads and libc++ / libunwind linked in, so they need no DLL; a 64 MB stack reserve for the process's
    # main thread, which runs the game without a window; the GUI subsystem: no console window opens with it, and the
    # log goes to the console it was started from, or a file (hos_log_open))
    return ["-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic", "-static-libstdc++", "-static-libgcc",
            "-lwinmm", "-Wl,--stack,0x4000000", "-mwindows"]


_running: set[subprocess.Popen] = set()   # (the compiles under way: cancel() ends them)
_running_lock = threading.Lock()
_cancelled = threading.Event()


class Cancelled(Exception):
    """the build was stopped (cancel())"""


def cancel() -> None:
    """stops the build: the compiles under way are killed, and build() raises Cancelled (from any thread)"""
    _cancelled.set()
    with _running_lock:
        for proc in list(_running):
            proc.kill()


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    """subprocess.run that cancel() can stop"""
    if _cancelled.is_set():
        raise Cancelled()
    with _running_lock:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
        _running.add(proc)
    try:
        out, err = proc.communicate()
    finally:
        with _running_lock:
            _running.discard(proc)
    if _cancelled.is_set():
        raise Cancelled()
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


class CompileError(RuntimeError):
    """a compile or the link failed; killed: the compiler was killed by a signal (out of memory, usually)"""

    def __init__(self, message: str, killed: bool = False):
        super().__init__(message)
        self.killed = killed


def build(work: str | os.PathLike, jobs: int | None = None, say=print, target_name: str | None = None,
          progress=None) -> Path:
    """compiles what changed and links; progress(done, total, name, done_bytes, total_bytes) after each object (the
    bytes: of the sources, a better measure of the time left than the count, as the chunks are most of it)"""
    if target_name:
        os.environ["ORANGES_TARGET"] = target_name
    work = Path(work)
    gen, obj_dir = work / "gen", out_dir(work) / "obj"
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
    compiler, compiler_cxx = cc(), cxx()
    t0 = time.monotonic()

    def compile_one(item):
        src, flags, obj, stamp, key = item
        start = time.monotonic()
        tmp = obj.with_suffix(".o.tmp")
        r = _run([compiler_cxx if src.suffix == ".cpp" else compiler, "-c", str(src), "-o", str(tmp)] + flags)
        if r.returncode:
            raise CompileError("%s failed (exit %d):\n%s" % (src.name, r.returncode, r.stderr[-4000:]),
                               killed=r.returncode < 0 or r.returncode in KILLED_EXITS)
        os.replace(tmp, obj)
        stamp.write_text(key)
        return src.name, time.monotonic() - start, src.stat().st_size

    done = done_bytes = 0
    total_bytes = sum(t[0].stat().st_size for t in todo)
    if progress:
        progress(0, len(todo), "", 0, total_bytes)
    with ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(compile_one, t) for t in todo]
        try:
            for fut in as_completed(futures):
                name, secs, size = fut.result()
                done += 1
                done_bytes += size
                say("  [%d/%d] %s %.0f s (%.0f s elapsed)" % (done, len(todo), name, secs, time.monotonic() - t0))
                if progress:
                    progress(done, len(todo), name, done_bytes, total_bytes)
        except BaseException:   # (the first failure ends the build: the queued compiles don't start)
            for fut in futures:
                fut.cancel()
            raise

    if windows():   # (the manifest: UTF-8 as the code page)
        res = (obj_dir / "oranges.res.o").resolve()
        windres = _tool(MINGW_TRIPLE + "-windres" if _cross() else "windres")
        env = dict(os.environ, PATH=os.path.dirname(windres) + os.pathsep + os.environ.get("PATH", ""))
        r = subprocess.run([windres, str(HOST / "win" / "oranges.rc"), "-O", "coff", "-o", str(res)],
                           capture_output=True, text=True, cwd=HOST / "win", env=env)   # (llvm-rc beside it)
        if r.returncode:
            raise RuntimeError("oranges.rc failed:\n%s" % r.stderr[-4000:])
        objs.append(res)
    exe = out_dir(work) / exe_name()
    say("link: %s" % exe)
    t1 = time.monotonic()
    libs = host_libraries()[1] + link_flags() + target_link_flags()
    linker = compiler_cxx if any(o.name.startswith(("eden.", "vk.")) for o in objs) else compiler
    r = _run([linker, "-fuse-ld=lld", "-o", str(exe)] + [str(o) for o in objs] + libs)
    if r.returncode:
        raise CompileError("link failed (exit %d):\n%s" % (r.returncode, r.stderr[-6000:]),
                           killed=r.returncode < 0 or r.returncode in KILLED_EXITS)
    say("linked %.0f MB in %.0f s" % (exe.stat().st_size / 1e6, time.monotonic() - t1))
    if windows():
        copied = bundle_dlls(exe)
        if copied:
            say("beside it: %s" % " ".join(copied))
    elif mingw_prefix():
        bundle_licenses(exe, mingw_prefix())
    return exe


def bundle_licenses(exe: Path, prefix: Path) -> None:
    """the licence texts of the libraries linked in from a prefix (a Linux release's: static), and oranges' own,
    into licenses/ beside the program"""
    out = exe.parent / "licenses"
    out.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "LICENSE", out / "oranges.txt")
    for lic in sorted((prefix / "share" / "licenses").glob("*")):
        if lic.is_dir():
            shutil.copytree(lic, out / lic.name, dirs_exist_ok=True)


SYSTEM_DLLS = ("vulkan-1.dll",)   # (the GPU driver installs it; wine has its own, which reaches the host's driver)
# (the licence folder, under <prefix>/share/licenses, of each DLL the program is shipped with)
DLL_LICENSES = {"sdl3.dll": "SDL3", "libopus-0.dll": "opus", "libiconv-2.dll": "libiconv",
                "libcharset-1.dll": "libiconv", "avcodec": "ffmpeg", "avutil": "ffmpeg"}


def _imports(path: Path) -> list[str]:
    """the DLLs a PE file imports"""
    r = subprocess.run([shutil.which("llvm-objdump") or _tool("llvm-objdump"), "-p", str(path)], capture_output=True,
                       text=True)
    return [line.split(":", 1)[1].strip() for line in r.stdout.splitlines() if "DLL Name:" in line]


def bundle_dlls(exe: Path) -> list[str]:
    """copies the DLLs the program needs from the libraries' prefix(es) beside it, theirs too; the names copied"""
    dirs = []
    for flag in host_libraries()[1]:
        if flag.startswith("-L") and (Path(flag[2:]).parent / "bin").is_dir():
            d = (Path(flag[2:]).parent / "bin").resolve()
            if d not in dirs:
                dirs.append(d)
    found = {f.name.lower(): f for d in dirs for f in d.glob("*.dll")}
    copied, todo, seen = [], [exe], set()
    while todo:
        for name in _imports(todo.pop()):
            key = name.lower()
            if key in seen or key in SYSTEM_DLLS or key not in found:
                continue
            seen.add(key)
            src, dst = found[key], exe.parent / found[key].name
            if not dst.exists() or dst.stat().st_mtime_ns != src.stat().st_mtime_ns:
                shutil.copy2(src, dst)
                copied.append(src.name)
            todo.append(src)
    # the licence texts: oranges' own, and each shipped library's from its prefix (licenses/<package>/)
    out = exe.parent / "licenses"
    out.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "LICENSE", out / "oranges.txt")
    for key in seen:
        name = DLL_LICENSES.get(key) or DLL_LICENSES.get(key.split("-")[0])
        lic = next((d.parent / "share" / "licenses" / name for d in dirs
                    if name and (d.parent / "share" / "licenses" / name).is_dir()), None)
        if lic is None:
            print("warning: no licence text found for %s" % key)
        elif not (out / name).exists():
            shutil.copytree(lic, out / name)
    return copied
