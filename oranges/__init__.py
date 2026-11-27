"""Static recompilation of the game to a native PC program."""

import os
from pathlib import Path


def tools_dir(name: str) -> Path:
    """where oranges keeps a set of tools it downloads (oranges.mingw's libraries, oranges.wintools'): on Windows
    %LOCALAPPDATA%\\oranges\\<name> (machine-local: not copied with a roaming profile, unlike %APPDATA%, which holds
    the saves and options), elsewhere ~/.local/opt/<name>"""
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA")
        return (Path(local) if local else Path.home() / "AppData" / "Local") / "oranges" / name
    return Path.home() / ".local" / "opt" / name


def mingw_dir() -> Path:
    """llvm-mingw's folder: ORANGES_MINGW, else on Windows the one oranges.wintools fetched (when it is there), else
    /opt/llvm-mingw"""
    if os.environ.get("ORANGES_MINGW"):
        return Path(os.environ["ORANGES_MINGW"])
    if os.name == "nt":
        fetched = tools_dir("wintools") / "llvm-mingw"
        if fetched.is_dir():
            return fetched
    return Path("/opt/llvm-mingw")
