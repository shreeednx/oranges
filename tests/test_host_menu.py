"""The settings menu's C parts without the game or SDL: the options file (host/hos_options.c), the bindings
(hos_controls.c) and the menu's model (hos_menu.c), built with the system's C compiler: tests/host_menu_test.c."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "host"
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


@pytest.mark.skipif(not CC, reason="no C compiler")
def test_menu_bindings_and_options_file(tmp_path):
    exe = tmp_path / "host_menu_test"
    subprocess.run([CC, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Werror", "-I" + str(HOST),
                    str(ROOT / "tests" / "host_menu_test.c"), str(HOST / "hos_options.c"), str(HOST / "hos_controls.c"),
                    str(HOST / "hos_menu.c"), str(HOST / "hos_port.c"), "-lpthread", "-o", str(exe)], check=True)
    ini = tmp_path / "options" / "oranges_options.ini"   # (its folder made at the first write)
    for step in ("first", "again"):
        r = subprocess.run([str(exe), str(ini), step], capture_output=True, text=True)
        assert r.stdout.strip() == "ok", "%s: %s%s" % (step, r.stdout, r.stderr)
    text = ini.read_text()
    assert text.startswith("# oranges settings")
    assert "future_setting=7" in text and "stick_dead_zone=15" in text and "key_" not in text
