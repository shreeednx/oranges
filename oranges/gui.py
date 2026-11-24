"""Builds the setup GUI (gui/oranges_setup.cpp): the program a player starts, which runs `oranges make` for them.

    python -m oranges.gui [--target linux|windows] [--out DIR]

It is a small SDL3 + Dear ImGui program (ImGui's SDL3 and SDL_Renderer backends: no Vulkan), with host/hos_options.c
and hos_port.c linked in (the game's options file). Compiled with the same compilers and libraries as the port
(oranges.build: the system's clang and pkg-config on Linux, llvm-mingw and oranges.mingw's prefix for Windows, or a
release's, oranges.release). The program goes to DIR (default work/gui, work/gui-windows): `oranges` or `oranges.exe`
(a GUI subsystem program), with SDL3.dll beside it for Windows. Run from there it finds this checkout (it looks for
oranges/ above itself) and the checkout's .venv Python; in a release, the release's src/ and python/.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

from . import build

ROOT = build.ROOT
GUI = ROOT / "gui"
IMGUI = build.IMGUI
SOURCES = [GUI / "oranges_setup.cpp"] + [IMGUI / f for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
                                                             "imgui_widgets.cpp", "backends/imgui_impl_sdl3.cpp",
                                                             "backends/imgui_impl_sdlrenderer3.cpp")]
C_SOURCES = [build.HOST / "hos_options.c", build.HOST / "hos_port.c"]


def compile_flags() -> list[str]:
    return build.target_flags() + ["-O2", "-g0", "-w", "-I" + str(IMGUI), "-I" + str(build.HOST),
                                   "-DIMGUI_DISABLE_DEMO_WINDOWS", "-DIMGUI_DISABLE_DEBUG_TOOLS"]


def link_flags() -> list[str]:
    if build.windows():
        return ["-static-libstdc++", "-static-libgcc", "-Wl,-Bstatic", "-lpthread", "-Wl,-Bdynamic", "-lole32",
                "-luuid", "-lshell32", "-lwinmm", "-mwindows"]
    return build.target_link_flags() + ["-lpthread", "-lm"]


def build_gui(out: Path | None = None, say=print) -> Path:
    out = out or ROOT / "work" / ("gui-windows" if build.windows() else "gui")
    obj = out / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    cflags, libs = build.optional_library("sdl3")
    if not libs:
        raise SystemExit("SDL3 not found for the %s build (pkg-config, or the libraries' prefix)" % build.target())
    objs = []
    for src in SOURCES + C_SOURCES:
        o = obj / (src.stem + ".o")
        cpp = src.suffix == ".cpp"
        cmd = [build.cxx() if cpp else build.cc(), "-c", str(src), "-o", str(o)] + compile_flags() + cflags
        cmd += ["-std=c++20"] + build.target_cxx_flags() if cpp else ["-std=gnu11"]
        if build.windows():
            cmd += ["-D_FILE_OFFSET_BITS=64"]
        if not o.exists() or o.stat().st_mtime < max(src.stat().st_mtime, *(h.stat().st_mtime for h in
                                                                             (build.HOST / "hos_options.h",
                                                                              build.HOST / "hos_port.h"))):
            say("compile %s" % src.name)
            subprocess.run(cmd, check=True)
        objs.append(o)
    exe = out / ("oranges.exe" if build.windows() else "oranges")
    say("link %s" % exe)
    subprocess.run([build.cxx(), "-fuse-ld=lld", "-o", str(exe)] + [str(o) for o in objs]
                   + [f for f in libs if f != "-mwindows"] + link_flags(), check=True)
    if build.windows():
        build.bundle_dlls(exe)
    return exe


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.gui", description=__doc__.split("\n\n")[0])
    ap.add_argument("--target", choices=build.TARGETS)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    if args.target:
        os.environ["ORANGES_TARGET"] = args.target
    print(build_gui(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
