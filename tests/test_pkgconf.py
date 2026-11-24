from pathlib import Path

import pytest

from oranges import pkgconf


def write(prefix: Path, name: str, text: str) -> Path:
    d = prefix / "lib" / "pkgconfig"
    d.mkdir(parents=True, exist_ok=True)
    (d / (name + ".pc")).write_text(text)
    return d


def test_moved_prefix_and_absolute_libdir(tmp_path):
    prefix = tmp_path / "my libs"   # (a space: stays one flag)
    d = write(prefix, "codec", "prefix=/old/place\nexec_prefix=${prefix}\nlibdir=/old/place/lib\n"
                               "includedir=/old/place/include\n"
                               "Requires: \nRequires.private: util >= 1.2.3\n"
                               "Libs: -L${libdir}  -lcodec \nLibs.private: -lm\nCflags: -I${includedir}\n")
    write(prefix, "util", "prefix=/old/place\nlibdir=${prefix}/lib\nincludedir=${prefix}/include/util\n"
                          "Libs: -L${libdir} -lutil\nCflags: -I${includedir} -DUTIL\n")
    p = prefix.as_posix()
    cflags, libs = pkgconf.flags(["codec"], [d])
    assert cflags == ["-I%s/include" % p, "-I%s/include/util" % p, "-DUTIL"]   # (Requires.private's cflags too)
    assert libs == ["-L%s/lib" % p, "-lcodec"]                                 # (not its libs)
    cflags, libs = pkgconf.flags(["codec", "util"], [d])
    assert libs == ["-L%s/lib" % p, "-lcodec", "-lutil"]                       # (-L given once)


def test_requires_continuations_comments(tmp_path):
    d = write(tmp_path, "a", "# a comment\nprefix=/x\nv=1\nName: a\nRequires: b, c >= ${v}\n"
                             "Libs: -la \\\n  -lmore # trailing\nCflags:\n")
    write(tmp_path, "b", "Libs: -lb\n")
    write(tmp_path, "c", "Libs: -lc\nCflags: -DC\n")
    cflags, libs = pkgconf.flags(["a"], [d])
    assert libs == ["-la", "-lmore", "-lb", "-lc"]
    assert cflags == ["-DC"]


def test_missing(tmp_path):
    d = write(tmp_path, "a", "Requires: gone\nLibs: -la\n")
    with pytest.raises(LookupError):
        pkgconf.flags(["a"], [d])
    with pytest.raises(LookupError):
        pkgconf.flags(["nothing"], [d])
