"""A release's own tools: when oranges runs from a release (its tree is <release>/src, beside release.json), the
pipeline builds with the compilers, sysroot and libraries the release carries, not the system's. oranges.release
lays a release out; the setup GUI (gui/oranges_setup.cpp) runs this tree with the release's Python.

    <release>/release.json    {"format": 1, "version", "platform": "linux-x64" | "windows-x64"}
    <release>/toolchain/      Linux: LLVM's clang and lld (oranges.linuxkit); Windows: llvm-mingw (oranges.wintools)
    <release>/sysroot/        Linux: the C library the port is built against
    <release>/libs/           the port's libraries (Linux: oranges.linuxkit's, static; Windows: oranges.mingw's)
    <release>/python/         the Python this runs on
    <release>/src/            oranges' tree, with a64-kit's translator library prebuilt

apply() sets the environment oranges.build and the kit read (only what is not set already, so a developer can still
point one at something else).
"""

import json
import os
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent


def root(src: Path = SRC) -> Path | None:
    """the release's folder, or None: oranges runs from a checkout"""
    r = src.parent
    return r if src.name == "src" and (r / "release.json").is_file() else None


def info(src: Path = SRC) -> dict:
    r = root(src)
    return json.loads((r / "release.json").read_text()) if r else {}


def environment(release: Path, platform: str) -> dict[str, str]:
    """the variables that make the pipeline use a release's tools"""
    tools = release / "toolchain"
    env = {"ORANGES_PREFIX": str(release / "libs"),
           "PATH": str(tools / "bin") + os.pathsep + os.environ.get("PATH", "")}
    if platform.startswith("windows"):
        env.update(ORANGES_MINGW=str(tools), ORANGES_MINGW_PREFIX=str(release / "libs"), LONGSHOT_MINGW=str(tools),
                   ORANGES_TARGET="windows")
    else:
        env.update(CC=str(tools / "bin" / "clang"), CXX=str(tools / "bin" / "clang++"),
                   ORANGES_SYSROOT=str(release / "sysroot"), ORANGES_TARGET="linux")
    return env


def apply(src: Path = SRC) -> Path | None:
    """in a release, its tools for the pipeline; returns the release's folder (None in a checkout)"""
    r = root(src)
    if r is None:
        return None
    for key, value in environment(r, info(src).get("platform", "linux-x64")).items():
        if key == "PATH":
            os.environ["PATH"] = value
        else:
            os.environ.setdefault(key, value)
    return r
