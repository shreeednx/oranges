"""Compile the lifted C (<work>/gen) with a64-kit's runtime (rt/) and the host (host/), and link the program.

Objects go to <work>/build/obj and are rebuilt only when their source, the headers they include or the flags changed
(a stamp beside each object). Compiles run in parallel, as many as fit in memory (a64-kit's toolchain.default_jobs;
ORANGES_JOBS overrides). Lifted chunks build at -O1 with the flags Longshot uses (nx2/build.py flags()). The big ones
are FP-exact (-ffp-contract=off) and LoopRotate is capped, which a64-kit measured as a third of a chunk's compile time.

    ORANGES_MARCH  CPU level for every object (default x86-64-v3)
    ORANGES_OPT    optimization level of the lifted chunks (default O1)
    ORANGES_VULKAN 0: without the Vulkan back end (host/vk and eden's recompiler, ~160 C++ files)
    ORANGES_TARGET windows: the Windows build (or `oranges build --target windows`), see below

The Windows build is cross-compiled with llvm-mingw (MinGW-w64 on clang, UCRT): x86_64-w64-mingw32-clang from PATH,
else from ORANGES_MINGW (default /opt/llvm-mingw). It goes to <work>/build-windows/oranges.exe, with its own objects.
Its libraries (SDL3, the Vulkan loader, libopus, FFmpeg) come from a MinGW prefix: ORANGES_MINGW_PREFIX (an unpacked
MSYS2 CLANG64 tree, say), whose lib/pkgconfig is searched with pkg-config's --define-prefix, or
x86_64-w64-mingw32-pkg-config when that is on PATH. With neither, it builds without them (headless, the null back
end). winpthreads and libc++ are linked in statically, so the program needs only those libraries' DLLs beside it.
Run on Windows itself (MSYS2's CLANG64 shell), the target is windows and its own clang, pkg-config and windres are used.
"""

import functools
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


TARGETS = ("linux", "windows")
MINGW_TRIPLE = "x86_64-w64-mingw32"


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


def _pkg_config() -> tuple[list[str], dict] | None:
    """the pkg-config command line and environment for the target's libraries, or None: none to be had"""
    if not _cross():   # (on Windows: MSYS2's own pkg-config knows its CLANG64 libraries)
        return ["pkg-config"], dict(os.environ)
    if shutil.which(MINGW_TRIPLE + "-pkg-config"):
        return [MINGW_TRIPLE + "-pkg-config"], dict(os.environ)
    prefix = os.environ.get("ORANGES_MINGW_PREFIX")
    if not prefix:
        return None
    env = dict(os.environ, PKG_CONFIG_LIBDIR=str(Path(prefix) / "lib" / "pkgconfig"))
    env.pop("PKG_CONFIG_PATH", None)
    return ["pkg-config", "--define-prefix"], env


def vulkan() -> bool:
    """whether the Vulkan back end is built (host/vk, with eden's recompiler): the system has a Vulkan loader, and
    SDL3 for the window it presents to; ORANGES_VULKAN=0 leaves it out (the null back end draws nothing)"""
    if os.environ.get("ORANGES_VULKAN") == "0":
        return False
    return bool(optional_library("vulkan")[1]) and bool(optional_library("sdl3")[1])


def cxx_flags(gen: Path) -> list[str]:
    return ["-std=c++20", "-O2", "-march=" + os.environ.get("ORANGES_MARCH", "x86-64-v3"), "-fno-strict-aliasing",
            "-g0", "-w", "-I" + str(EDEN), "-I" + str(EDEN / "compat"), "-I" + str(EDEN / "sirit" / "include"),
            "-I" + str(EDEN / "sirit" / "src"), "-I" + str(KHRONOS), "-I" + str(HOST), "-I" + str(gen),
            "-I" + str(RT)]


def optional_library(name: str) -> tuple[list[str], list[str]]:
    """(compile flags, link flags) of a library pkg-config knows for the target, or ([], []): the host builds
    without it"""
    return _optional_library(name, target())


@functools.cache
def _optional_library(name: str, _target: str) -> tuple[list[str], list[str]]:
    pc = _pkg_config()
    if pc is None:
        return [], []
    cmd, env = pc
    try:
        cflags = subprocess.run(cmd + ["--cflags"] + name.split(), capture_output=True, text=True, check=True,
                                env=env).stdout
        libs = subprocess.run(cmd + ["--libs"] + name.split(), capture_output=True, text=True, check=True,
                              env=env).stdout
    except (OSError, subprocess.CalledProcessError):
        return [], []
    return cflags.split(), libs.split()


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
    return ["-march=" + os.environ.get("ORANGES_MARCH", "x86-64-v3"), "-ffp-contract=off", "-fno-strict-aliasing",
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


def _key(src: Path, flags: list[str], gen: Path) -> str:
    """What an object depends on: its flags, its source and the headers it includes"""
    h = hashlib.sha256("\0".join(flags).encode())
    deps = [src, RT / "a64.h", gen / "a64_suyu.h", gen / "a64_fpx.h"]
    if HOST in src.parents:   # (every host header and include file: few, and cheap to hash)
        deps += sorted(p for p in HOST.rglob("*") if p.suffix in (".h", ".inc"))
    if EDEN in src.parents or src.parent == HOST / "vk":   # (and eden's headers)
        deps += sorted(p for p in EDEN.rglob("*") if p.suffix in (".h", ".hpp", ".inc"))
    if IMGUI in src.parents or src.parent == HOST / "vk":   # (and Dear ImGui's)
        deps += sorted(p for p in IMGUI.rglob("*.h"))
    for dep in deps:
        if dep.exists():
            h.update(str(dep).encode() + b"\0" + _digest(dep))
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
    # main thread, which runs the game without a window; the console subsystem: the log goes to it)
    return ["-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic", "-static-libstdc++", "-static-libgcc",
            "-lwinmm", "-Wl,--stack,0x4000000"]


def build(work: str | os.PathLike, jobs: int | None = None, say=print, target_name: str | None = None) -> Path:
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
        r = subprocess.run([compiler_cxx if src.suffix == ".cpp" else compiler, "-c", str(src), "-o", str(tmp)] + flags,
                           capture_output=True, text=True)
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
    libs = host_libraries()[1] + link_flags()
    linker = compiler_cxx if any(o.name.startswith(("eden.", "vk.")) for o in objs) else compiler
    r = subprocess.run([linker, "-fuse-ld=lld", "-o", str(exe)] + [str(o) for o in objs] + libs,
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("link failed:\n%s" % r.stderr[-6000:])
    say("linked %.0f MB in %.0f s" % (exe.stat().st_size / 1e6, time.monotonic() - t1))
    return exe
