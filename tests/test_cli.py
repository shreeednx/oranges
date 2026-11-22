from fakes import make_nso, make_nup

from oranges.__main__ import main


def test_commands_on_a_fake_package(tmp_path, capsys):
    nup = tmp_path / "game.nup"
    nup.write_bytes(make_nup({"exefs/main": make_nso(), "exefs/sdk": make_nso(exports=("imported",)),
                              "romfs/a.bin": b"abc"}, packed={"exefs/main"}))
    assert main(["info", str(nup)]) == 0
    assert "supported build" in capsys.readouterr().out
    assert main(["check", str(nup)]) == 0
    assert main(["verify", str(nup)]) == 0
    assert main(["nso", str(nup), "-v"]) == 0
    out = capsys.readouterr().out
    assert "main: 0 imports no module exports" in out and "svc 0x26 0x7f" in out
    assert main(["extract", str(nup), str(tmp_path / "out"), "--only", "exefs"]) == 0
    assert main(["nso", str(tmp_path / "out" / "exefs" / "main")]) == 0


def test_errors_exit_2(tmp_path, capsys):
    bad = tmp_path / "bad.nup"
    bad.write_bytes(b"nope" + bytes(0x100))
    assert main(["info", str(bad)]) == 2
    assert "not a .nup" in capsys.readouterr().err
