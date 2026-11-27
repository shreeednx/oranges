"""oranges.mingw's repository database reading and dependency closure, on a synthetic database (no network)."""

import io
import tarfile
from compression import zstd
from pathlib import Path
from types import SimpleNamespace

import pytest

from oranges import mingw, mingw_dir

P = mingw.PREFIX


def make_db(packages: dict[str, dict]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for name, p in packages.items():
            lines = ["%NAME%", name, "", "%VERSION%", p.get("version", "1.0-1"), "",
                     "%FILENAME%", name + "-1.0-1-any.pkg.tar.zst", "", "%SHA256SUM%", "00" * 32, ""]
            for key in ("DEPENDS", "PROVIDES"):
                if p.get(key.lower()):
                    lines += ["%" + key + "%"] + p[key.lower()] + [""]
            data = "\n".join(lines).encode()
            info = tarfile.TarInfo("%s-1.0-1/desc" % name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return zstd.compress(buf.getvalue())


def test_read_db_and_closure():
    db = mingw.read_db(make_db({
        P + "sdl3": {"depends": [P + "libiconv>=1.0", P + "cc-libs"]},
        P + "libiconv": {"version": "1.19.1-1"},
        P + "libc++": {"provides": [P + "cc-libs"]},
        P + "opus": {},
        P + "unrelated": {"depends": [P + "opus"]},
    }))
    assert db[P + "libiconv"]["version"] == "1.19.1-1"
    assert db[P + "sdl3"]["depends"] == [P + "libiconv>=1.0", P + "cc-libs"]
    assert mingw.closure(db, ["sdl3"]) == [P + "libc++", P + "libiconv", P + "sdl3"]   # (version cut, provides)
    assert mingw.closure(db, ["opus", P + "sdl3"]) == [P + "libc++", P + "libiconv", P + "opus", P + "sdl3"]
    with pytest.raises(SystemExit):
        mingw.closure(db, ["nothing"])


def test_llvm_mingw_folder(monkeypatch, tmp_path):
    monkeypatch.setenv("ORANGES_MINGW", str(tmp_path))
    assert mingw_dir() == tmp_path
    monkeypatch.delenv("ORANGES_MINGW")
    assert mingw_dir() == Path("/opt/llvm-mingw") or mingw.os.name == "nt"


def test_ffmpeg_is_not_built_on_windows(monkeypatch, tmp_path):
    built, said = [], []
    monkeypatch.setattr(mingw, "build_ffmpeg", lambda prefix, say: built.append(prefix))
    monkeypatch.setattr(mingw, "os", SimpleNamespace(name="nt"))
    mingw.ffmpeg(tmp_path, said.append)
    assert not built and "left out" in said[-1] and str(tmp_path) in said[-1]
    (tmp_path / "bin").mkdir()
    (tmp_path / "bin" / "avcodec-63.dll").write_bytes(b"")
    mingw.ffmpeg(tmp_path, said.append)
    assert not built and "keeping" in said[-1]
    monkeypatch.setattr(mingw, "os", SimpleNamespace(name="posix"))
    mingw.ffmpeg(tmp_path, said.append)
    assert built == [tmp_path]
