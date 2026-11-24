"""The Windows-hosted tools oranges' Python side runs on: the embeddable Python 3.14 with oranges' dependency (lz4's
wheel), and llvm-mingw's Windows release (clang++ for the translator's a64emit.dll, and clang for the build). It is
what the Windows release will bundle (TODO.md task 2). Here it lets the pipeline run on Windows, or under wine:

    python -m oranges.wintools [--dir DIR] [--source REPO]

DIR (default ~/.local/opt/oranges-wintools; on Windows %LOCALAPPDATA%\\oranges\\wintools) gets python/ (python.exe,
with lz4 in Lib/site-packages) and llvm-mingw/ (bin/clang.exe ...). The embeddable Python reads only its
python314._pth (no PYTHONPATH, no site by default): this turns on `import site`, adds Lib/site-packages, and puts a
.pth there naming REPO (default this checkout, as Windows sees it: under wine Z: + its path), so `python -m oranges`
runs this tree, as `pip install -e` would. Under wine:

    LONGSHOT_MINGW='Z:<DIR>/llvm-mingw' wine <DIR>/python/python.exe -m oranges lift --write --work <a work folder>

(LONGSHOT_MINGW: the kit's toolchain.py takes clang++ from there for a64emit.dll, built static with llvm-mingw's
target, so no DLL of the toolchain is needed beside it.)

Each download is checked against a SHA-256 pinned here: Python's from its sigstore bundle on python.org (the bundle's
message digest; the signature itself was not checked), lz4's from PyPI's JSON, llvm-mingw's from GitHub's release
asset digest. Downloads are kept in DIR/.cache.
"""

import argparse
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

from . import tools_dir

DEFAULT_DIR = tools_dir("oranges-wintools" if os.name != "nt" else "wintools")
PYTHON = ("https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip",
          "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159")
WHEELS = (   # (oranges' dependencies, pyproject.toml's: built for CPython 3.14 on 64-bit Windows)
    ("https://files.pythonhosted.org/packages/18/2c/8332080fd293f8337779a440b3a143f85e374311705d243439a3349b81ad/"
     "lz4-4.4.5-cp314-cp314-win_amd64.whl", "92159782a4502858a21e0079d77cdcaade23e8a5d252ddf46b0652604300d7be"),
)
LLVM_MINGW = ("https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip",
              "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412")   # (LLVM 23.1.6)


def fetch(url: str, sha256: str, cache: Path, say=print) -> Path:
    """the file at url, from the cache or downloaded, checked against its SHA-256"""
    path = cache / url.rsplit("/", 1)[1]
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == sha256:
        return path
    say("download %s" % url)
    cache.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "oranges-wintools-fetch"})
    h = hashlib.sha256()
    tmp = path.with_suffix(path.suffix + ".part")
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        while chunk := r.read(1 << 20):
            h.update(chunk)
            f.write(chunk)
    if h.hexdigest() != sha256:
        tmp.unlink()
        raise SystemExit("%s: SHA-256 %s, expected %s" % (url, h.hexdigest(), sha256))
    os.replace(tmp, path)
    return path


def unzip(archive: Path, dest: Path, strip: bool = False) -> None:
    """the archive's files into dest (strip: without its one top folder); names that would leave dest are refused"""
    root = dest.resolve()
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename.split("/", 1)[1] if strip and "/" in info.filename else info.filename
            if not name or name.endswith("/"):
                continue
            out = (dest / name).resolve()
            if root not in out.parents:
                raise SystemExit("%s: %s is outside the folder" % (archive.name, info.filename))
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)


def windows_path(path: Path) -> str:
    """path as Windows sees it: itself on Windows, under wine its Z: drive's name"""
    return str(path) if os.name == "nt" else "Z:" + str(path.resolve()).replace("/", "\\")


def install(dest: Path, source: Path | None, say=print) -> None:
    cache = dest / ".cache"
    py = dest / "python"
    if py.exists():
        shutil.rmtree(py)
    unzip(fetch(*PYTHON, cache, say), py)
    site = py / "Lib" / "site-packages"
    site.mkdir(parents=True)
    for wheel in WHEELS:
        unzip(fetch(*wheel, cache, say), site)
    pth = next(py.glob("python3*._pth"))
    lines = [ln for ln in pth.read_text().splitlines() if ln.strip() not in ("#import site", "import site")]
    pth.write_text("\n".join(lines + ["Lib\\site-packages", "import site", ""]))
    if source:
        (site / "oranges-source.pth").write_text(windows_path(source) + "\n")
    tools = dest / "llvm-mingw"
    if not (tools / "bin" / "clang.exe").exists():
        if tools.exists():
            shutil.rmtree(tools)
        say("unpack %s" % LLVM_MINGW[0].rsplit("/", 1)[1])
        unzip(fetch(*LLVM_MINGW, cache, say), tools, strip=True)
    say("python: %s" % (py / "python.exe"))
    say("llvm-mingw: %s (LONGSHOT_MINGW=%s)" % (tools, windows_path(tools)))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m oranges.wintools", description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR, help="where (default %(default)s)")
    ap.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="the oranges tree python -m oranges runs (default: this one)")
    args = ap.parse_args(argv)
    install(args.dir, args.source)
    return 0


if __name__ == "__main__":
    sys.exit(main())
