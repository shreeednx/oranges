import json

import pytest
from fakes import make_nso, make_nup

from oranges import build, lift, make
from oranges.__main__ import main


@pytest.fixture
def nup(tmp_path):
    path = tmp_path / "game.nup"
    path.write_bytes(make_nup({"exefs/main": make_nso(), "exefs/sdk": make_nso(), "romfs/a.bin": b"abc",
                               "romfs/d/b.bin": b"defg"}))
    return path


@pytest.fixture
def pipeline(monkeypatch):
    """lift.run and build.build replaced: they record their calls and write what the real ones would"""
    calls = []

    def fake_lift(exefs, work, skip=(), translate=False, write=False, say=print):
        calls.append("lift")
        (work / "gen").mkdir(parents=True, exist_ok=True)
        (work / "gen" / "code_000.c").write_text("int x;\n")
        say("image: fake")
        say("discover: fake")
        say("emit: fake")
        return {}

    def fake_build(work, jobs=None, say=print, target_name=None, progress=None):
        calls.append("build")
        progress(0, 2, "", 0, 10)
        progress(2, 2, "code_000.c", 10, 10)
        say("link: fake")
        exe = build.out_dir(work) / build.exe_name()
        (exe.parent / "obj").mkdir(parents=True, exist_ok=True)
        exe.write_bytes(b"exe")
        return exe

    monkeypatch.setattr(lift, "run", fake_lift)
    monkeypatch.setattr(build, "build", fake_build)
    return calls


def events(out: str) -> list[dict]:
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def test_make_builds_and_resumes(nup, tmp_path, pipeline, capsys, monkeypatch):
    monkeypatch.setattr(make, "_watch_stdin", lambda rep: None)
    folder = tmp_path / "port"
    assert main(["make", str(nup), "--dir", str(folder), "--progress", "json"]) == 0
    ev = events(capsys.readouterr().out)
    assert ev[0]["event"] == "stages" and [s["id"] for s in ev[0]["stages"]] == [s for s, _ in make.STAGES]
    assert [e["id"] for e in ev if e["event"] == "stage"] == ["check", "extract", "lift", "compile", "link"]
    assert ev[-1]["event"] == "done"
    assert (folder / "romfs" / "d" / "b.bin").read_bytes() == b"defg"
    assert (folder / "exefs" / "main").is_file()
    info = json.loads((folder / "oranges.json").read_text())
    assert info["exe"] == ev[-1]["exe"] and info["nup"] == str(nup.resolve())
    assert any(e["event"] == "progress" and e["id"] == "compile" and e["count"] == 2 for e in ev)
    assert "oranges make" in (folder / "make.log").read_text()
    assert pipeline == ["lift", "build"]
    # again: the lift's stamp matches, so only the build runs (it compiles only what it lacks)
    assert main(["make", str(nup), "--dir", str(folder), "--clean"]) == 0
    assert pipeline == ["lift", "build", "build"]
    assert "up to date" in capsys.readouterr().err
    assert not (folder / "gen").exists() and not (build.out_dir(folder) / "obj").exists()
    # cleaned: the next run lifts again
    assert main(["make", str(nup), "--dir", str(folder)]) == 0
    assert pipeline[-2:] == ["lift", "build"]


def test_make_refuses_another_build(tmp_path, pipeline, capsys, monkeypatch):
    monkeypatch.setattr(make, "_watch_stdin", lambda rep: None)
    other = tmp_path / "other.nup"
    other.write_bytes(make_nup({"exefs/main": make_nso()}, build_id="00" * 16))
    assert main(["make", str(other), "--dir", str(tmp_path / "p"), "--progress", "json"]) == 3
    ev = events(capsys.readouterr().out)
    assert ev[-1]["event"] == "error" and ev[-1]["kind"] == "game" and "not the one oranges supports" in \
        ev[-1]["message"]
    assert pipeline == []


def test_make_checks_the_room(nup, tmp_path, pipeline, capsys, monkeypatch):
    monkeypatch.setattr(make, "_watch_stdin", lambda rep: None)
    monkeypatch.setattr(make, "free_bytes", lambda folder: 1000)
    assert main(["make", str(nup), "--dir", str(tmp_path / "p"), "--progress", "json"]) == 4
    ev = events(capsys.readouterr().out)
    assert ev[-1]["kind"] == "space" and "Not enough free space" in ev[-1]["message"]
    assert pipeline == []


def test_make_reports_a_killed_compile_as_memory(nup, tmp_path, pipeline, capsys, monkeypatch):
    monkeypatch.setattr(make, "_watch_stdin", lambda rep: None)

    def oom(*a, **kw):
        raise build.CompileError("code_001.c failed (exit -9):\n", killed=True)

    monkeypatch.setattr(build, "build", oom)
    assert main(["make", str(nup), "--dir", str(tmp_path / "p"), "--progress", "json"]) == 5
    assert events(capsys.readouterr().out)[-1]["kind"] == "memory"


def test_needed_bytes_counts_what_is_missing(tmp_path):
    (tmp_path / "romfs").mkdir()
    (tmp_path / "romfs" / "a").write_bytes(b"12")
    files = [("a", 2), ("b", 5)]
    assert make.needed_bytes(tmp_path, files, True, True) == 5 + (256 << 20)
    assert make.needed_bytes(tmp_path, files, False, False) == 5 + make.GEN_BYTES + make.OBJ_BYTES + (256 << 20)


def test_cancel_kills_running_compiles():
    import sys
    import threading
    build._cancelled.clear()
    t = threading.Timer(0.3, build.cancel)
    t.start()
    with pytest.raises(build.Cancelled):
        build._run([sys.executable, "-c", "import time; time.sleep(30)"])
    assert not build._running
    build._cancelled.clear()
