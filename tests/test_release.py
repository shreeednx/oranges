import io
import json
import os
import tarfile
from compression import zstd
from pathlib import Path

from oranges import build, bundled, linuxkit, release


def _ar(members: dict[str, bytes]) -> bytes:
    out = bytearray(b"!<arch>\n")
    for name, data in members.items():
        out += ("%-16s%-12s%-6s%-6s%-8s%-10d`\n" % (name, 0, 0, 0, 100644, len(data))).encode()
        out += data + (b"\n" if len(data) & 1 else b"")
    return bytes(out)


def _deb(files: dict[str, bytes], links: dict[str, str]) -> bytes:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo("./" + name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        for name, target in links.items():
            info = tarfile.TarInfo("./" + name)
            info.type, info.linkname = tarfile.SYMTYPE, target
            tar.addfile(info)
    return _ar({"debian-binary": b"2.0\n", "control.tar.zst": b"", "data.tar.zst": zstd.compress(raw.getvalue())})


def test_deb_unpacks_with_absolute_links_made_relative(tmp_path):
    deb = _deb({"lib/x86_64-linux-gnu/libm.so.6": b"elf", "usr/include/m.h": b"#pragma once\n"},
               {"usr/lib/x86_64-linux-gnu/libm.so": "/lib/x86_64-linux-gnu/libm.so.6"})
    assert set(linuxkit.ar_members(deb)) == {"debian-binary", "control.tar.zst", "data.tar.zst"}
    linuxkit.unpack_deb(deb, tmp_path)
    link = tmp_path / "usr" / "lib" / "x86_64-linux-gnu" / "libm.so"
    assert os.readlink(link) == "../../../lib/x86_64-linux-gnu/libm.so.6"
    assert link.read_bytes() == b"elf"   # (it resolves inside the sysroot)


def test_read_index():
    text = ("Package: libc6\nVersion: 2.35-0ubuntu3\nFilename: pool/main/g/glibc/libc6.deb\nSHA256: ab\n"
            "Description: GNU C Library\n more lines\n\nPackage: libopus0\nVersion: 1.3.1-0.1build2\n"
            "Filename: pool/main/o/opus/libopus0.deb\nSHA256: cd\n")
    idx = linuxkit.read_index(text)
    assert idx["libc6"]["Version"] == "2.35-0ubuntu3" and idx["libopus0"]["SHA256"] == "cd"
    assert "more lines" in idx["libc6"]["Description"]


def test_static_pc_files_fold_libs_private(tmp_path):
    pcdir = tmp_path / "libs" / "lib" / "pkgconfig"
    pcdir.mkdir(parents=True)
    (pcdir / "sdl3.pc").write_text("prefix=/x\nlibdir=${prefix}/lib\nName: sdl3\nVersion: 3\nLibs: -L${libdir} "
                                   "-lSDL3\nLibs.private: -lm -ldl\nCflags: -I${prefix}/include\n")
    linuxkit.finish_libs(tmp_path)
    from oranges import pkgconf
    cflags, libs = pkgconf.flags(["sdl3", "vulkan"], [pcdir])
    assert libs == ["-L%s/libs/lib" % tmp_path.as_posix(), "-lSDL3", "-lm", "-ldl", "-lvulkan"]
    assert cflags == ["-I%s/libs/include" % tmp_path.as_posix()]


def test_release_folder_is_detected(tmp_path):
    src = tmp_path / "rel" / "src"
    (src / "oranges").mkdir(parents=True)
    assert bundled.root(src) is None   # (no release.json: a checkout)
    (tmp_path / "rel" / "release.json").write_text(json.dumps({"format": 1, "version": "v1", "platform": "linux-x64"}))
    assert bundled.root(src) == tmp_path / "rel"
    env = bundled.environment(tmp_path / "rel", "linux-x64")
    assert env["CC"].endswith(os.path.join("toolchain", "bin", "clang"))
    assert env["ORANGES_SYSROOT"] == str(tmp_path / "rel" / "sysroot")
    assert env["ORANGES_PREFIX"] == str(tmp_path / "rel" / "libs")
    win = bundled.environment(tmp_path / "rel", "windows-x64")
    assert win["ORANGES_MINGW"] == win["LONGSHOT_MINGW"] == str(tmp_path / "rel" / "toolchain")
    assert "ORANGES_SYSROOT" not in win and win["ORANGES_TARGET"] == "windows"


def test_the_prefix_and_sysroot_reach_the_build(monkeypatch, tmp_path):
    monkeypatch.setenv("ORANGES_TARGET", "linux")
    monkeypatch.delenv("ORANGES_PREFIX", raising=False)
    monkeypatch.delenv("ORANGES_SYSROOT", raising=False)
    assert build.mingw_prefix() is None and build.target_flags() == [] and build.target_link_flags() == []
    monkeypatch.setenv("ORANGES_PREFIX", str(tmp_path))
    monkeypatch.setenv("ORANGES_SYSROOT", "/sr")
    assert build.mingw_prefix() == tmp_path
    assert build.target_flags() == ["--sysroot=/sr"]
    assert "-static-libstdc++" in build.target_link_flags() and "-Wl,-rpath,$ORIGIN" in build.target_link_flags()
    assert build.common_flags(Path("gen"))[0] == "--sysroot=/sr"
    assert "-stdlib=libc++" in build.cxx_flags(Path("gen")) and "-stdlib=libc++" not in build.common_flags(Path("g"))


def test_tracked_files_hold_nothing_of_the_game():
    files = release.tracked_files()
    assert "oranges/make.py" in files and "gui/oranges_setup.cpp" in files and "LICENSE" in files
    assert not [f for f in files if f.startswith(("work/", "build/")) or f.endswith((".nup", ".nso", ".bin"))]
    assert not [f for f in files if f.startswith("tests/")]


def test_sha256sums_and_verify(tmp_path):
    (tmp_path / "a").write_text("one")
    (tmp_path / "d").mkdir()
    (tmp_path / "d" / "b").write_text("two")
    (tmp_path / "SHA256SUMS").write_text(release.sha256sums(tmp_path))
    assert release.verify(tmp_path) == []
    (tmp_path / "d" / "b").write_text("changed")
    (tmp_path / "a").unlink()
    assert sorted(release.verify(tmp_path)) == ["a: missing", "d/b: changed"]


def test_object_keys_do_not_depend_on_where_folders_are(tmp_path, monkeypatch):
    src = tmp_path / "a.c"
    src.write_text("int x;\n")
    keys = []
    for where in ("one", "two"):
        sysroot, gen = tmp_path / where / "sysroot", tmp_path / where / "work" / "gen"
        monkeypatch.setenv("ORANGES_SYSROOT", str(sysroot))
        keys.append(build._key(src, ["--sysroot=%s" % sysroot, "-I%s" % gen, "-O1"], gen))
    assert keys[0] == keys[1]
    assert build._key(src, ["-O2"], gen) != keys[0]
