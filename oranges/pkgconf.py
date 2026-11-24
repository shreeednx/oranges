"""A library prefix's pkg-config files, read without pkg-config: what the Windows build needs from the MinGW prefix
(oranges.mingw's), so that a build on Windows needs no MSYS2, and the oranges release none either (TODO.md task 2).

The prefix may have moved since its files were written: as pkg-config's --define-prefix does, `prefix` becomes the
folder two above the .pc file, and (unlike it) so does the old prefix wherever another variable spells it out
(FFmpeg's configure writes libdir and includedir as absolute paths). Fields are split into flags before variables
are expanded, so a prefix with spaces in it stays one flag. Paths use forward slashes (clang takes them on Windows).

flags(["libavcodec", "libavutil"], [<prefix>/lib/pkgconfig]) is `pkg-config --cflags` and `--libs` (shared
linking): the Cflags of the packages and of everything they require (Requires and Requires.private), the Libs of the
packages and of what they require through Requires; flags already given are not repeated. Versions are not checked.
"""

import re
import shlex
from pathlib import Path

_VAR = re.compile(r"\$\{(\w+)\}")


class Package:
    def __init__(self, path: Path):
        self.path = path
        self.vars: dict[str, str] = {"pcfiledir": path.parent.as_posix()}
        self.fields: dict[str, str] = {}
        lines, cont = [], ""
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = cont + raw
            if line.endswith("\\"):
                cont = line[:-1]
                continue
            cont = ""
            lines.append(line.split("#", 1)[0].strip())
        old_prefix = None
        for line in lines:
            m = re.match(r"([\w.]+)\s*([=:])\s*(.*)$", line)
            if not m:
                continue
            key, kind, value = m.groups()
            if kind == "=":
                if key == "prefix":
                    old_prefix = value
                    value = path.parent.parent.parent.as_posix()   # (<prefix>/lib/pkgconfig/x.pc)
                elif old_prefix and "$" not in old_prefix and (value == old_prefix
                                                                or value.startswith(old_prefix + "/")):
                    value = "${prefix}" + value[len(old_prefix):]
                self.vars[key] = value
            else:
                self.fields[key.lower()] = value

    def expand(self, text: str, depth: int = 0) -> str:
        if depth > 32:
            raise ValueError("%s: variables nest too deep" % self.path)
        return _VAR.sub(lambda m: self.expand(self.vars.get(m.group(1), ""), depth + 1), text)

    def flags(self, field: str) -> list[str]:
        return [self.expand(tok) for tok in shlex.split(self.fields.get(field, ""))]

    def requires(self, field: str) -> list[str]:
        """the package names a Requires field lists (version conditions dropped)"""
        out, skip = [], False
        for tok in re.split(r"[\s,]+", self.expand(self.fields.get(field, ""))):
            if not tok:
                continue
            if skip:
                skip = False
            elif tok in ("=", "<", ">", "<=", ">=", "!="):
                skip = True
            else:
                out.append(tok)
        return out


def find(name: str, dirs: list[Path]) -> Package:
    for d in dirs:
        if (d / (name + ".pc")).is_file():
            return Package(d / (name + ".pc"))
    raise LookupError("no %s.pc in %s" % (name, ", ".join(str(d) for d in dirs)))


def flags(names: list[str], dirs: list[Path]) -> tuple[list[str], list[str]]:
    """(cflags, libs) of the packages; LookupError when one (or one they require) isn't there"""
    cflags, libs = [], []

    def walk(name: str, seen: set, for_libs: bool) -> None:
        if name in seen:
            return
        seen.add(name)
        pkg = find(name, dirs)
        out = libs if for_libs else cflags
        for f in pkg.flags("libs" if for_libs else "cflags"):
            if f not in out:
                out.append(f)
        for req in pkg.requires("requires") + ([] if for_libs else pkg.requires("requires.private")):
            walk(req, seen, for_libs)

    seen_c, seen_l = set(), set()
    for n in names:
        walk(n, seen_c, False)
        walk(n, seen_l, True)
    return cflags, libs
