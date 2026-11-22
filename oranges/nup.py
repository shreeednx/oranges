"""Read a .nup package: a decrypted Switch dump holding one or more titles, each with an exefs/ and a romfs/ tree.

Layout (little-endian), worked out from the reference dump. Longshot's nx2/nup.py describes the same format:

  header (0x80 bytes)
    +0x00 "NUP\\x01"   +0x04 u16 header size (0x80)   +0x06 u16 1
    +0x08 u64 TOC offset   +0x10 u64 TOC size   +0x18 SHA-256 of the TOC
    +0x38 u64 data offset (where file extents begin, 16-aligned)   +0x40 u64 file size   +0x48.. unknown
  TOC
    +0x00 "TOC\\x01", u32 title count, u64 dir count, u64 file count, u64 strings offset, u64 strings size
          (offsets inside the TOC are from its start)
    +0x30 titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir (0xffffffff: none),
          u32 version, ..., +0x20 u64 offset and u64 size of the title's metadata blob
    dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 child count, u64 0
    files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size,
          SHA-256 of the contents, u8 codec (0 raw, 1 one zstd frame of the contents), u8 flags, 14 bytes 0
    children, u32 each: a node index, a dir below the dir count, else file (index - dir count)
    name strings, UTF-8, not terminated
  metadata blob: records of u32 type, u32 length, then the data (see META_*)

A file's codec is the package's own compression. A file that is zstd inside the game (romfs *.zs) is codec 0 unless the
package compressed it again.
"""

import hashlib
import os
import struct
import threading
from collections.abc import Callable, Iterator
from compression import zstd
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
NO_DIR = 0xFFFFFFFF
CODEC_RAW, CODEC_ZSTD = 0, 1
CHUNK = 8 << 20

# metadata record types seen in the reference dump
META_NPDM = 1       # the title's main.npdm
META_LABEL = 5      # text, e.g. "output_update" (base game with its update applied)
META_BUILD_ID = 7   # main's NSO module id, 32 bytes
META_NAMES = {META_NPDM: "npdm", META_LABEL: "label", META_BUILD_ID: "build_id"}

Progress = Callable[[int, int], None]   # (bytes done, bytes total)


class NupError(Exception):
    pass


@dataclass(frozen=True, slots=True)
class Entry:
    """A file record."""
    index: int
    name: str
    size: int        # size of the contents
    offset: int      # absolute offset of the stored bytes
    stored: int      # stored size (equals size for raw files)
    sha256: bytes    # of the contents
    codec: int
    flags: int


@dataclass(frozen=True, slots=True)
class Title:
    index: int
    id: int
    root: int
    exefs: int
    romfs: int
    version: int
    meta_offset: int
    meta_size: int

    @property
    def id_hex(self) -> str:
        return "%016X" % self.id


def _check_name(name: str) -> None:
    if name in ("", ".", "..") or any(c in name for c in "/\\\0"):
        raise NupError("unsafe name in the package: %r" % name)


class Nup:
    """An open .nup package. Parsing checks the TOC's hash and its tree. File contents are checked by verify()."""

    def __init__(self, path: str | os.PathLike):
        self.path = os.fspath(path)
        self._f = open(self.path, "rb")
        self._lock = threading.Lock()
        try:
            self._parse()
        except BaseException:
            self._f.close()
            raise

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> Nup:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def read_at(self, offset: int, n: int) -> bytes:
        if hasattr(os, "pread"):
            data = os.pread(self._f.fileno(), n, offset)
        else:   # (Windows)
            with self._lock:
                self._f.seek(offset)
                data = self._f.read(n)
        if len(data) != n:
            raise NupError("short read at %#x (%d of %d bytes): the package is incomplete" % (offset, len(data), n))
        return data

    def _parse(self) -> None:
        physical = os.fstat(self._f.fileno()).st_size
        if physical < 0x80:
            raise NupError("not a .nup package (too small)")
        h = self.read_at(0, 0x80)
        if h[:4] != MAGIC:
            raise NupError("not a .nup package (magic %r)" % h[:4])
        toc_offset, toc_size = struct.unpack_from("<QQ", h, 0x08)
        self.toc_sha256 = h[0x18:0x38]
        self.data_offset, self.file_size = struct.unpack_from("<QQ", h, 0x38)
        if self.file_size != physical:
            raise NupError("the package is %d bytes but its header says %d: it is incomplete or damaged"
                           % (physical, self.file_size))
        if (toc_offset < 0x80 or toc_size < 0x30 or toc_offset + toc_size > self.data_offset
                or self.data_offset > physical):
            raise NupError("TOC or data offset outside the package")
        toc = self.read_at(toc_offset, toc_size)
        if hashlib.sha256(toc).digest() != self.toc_sha256:
            raise NupError("TOC SHA-256 mismatch: the package is damaged")
        if toc[:4] != TOC_MAGIC:
            raise NupError("no TOC magic")

        nt, nd, nf, soff, ssz = struct.unpack_from("<IQQQQ", toc, 4)
        dirs_at = 0x30 + 0x40 * nt
        files_at = dirs_at + 24 * nd
        kids_at = files_at + 80 * nf
        if not nt or kids_at > soff or (soff - kids_at) % 4 or soff + ssz > toc_size:
            raise NupError("TOC tables overlap or exceed the TOC")
        strings = toc[soff:soff + ssz]
        self.nd, self.nf = nd, nf

        def name(off: int, n: int) -> str:
            if off + n > len(strings):
                raise NupError("name outside the string table")
            return strings[off:off + n].decode("utf-8")

        self.titles: list[Title] = []
        for i in range(nt):
            at = 0x30 + 0x40 * i
            tid, root, exefs, romfs, version = struct.unpack_from("<QIIII", toc, at)
            meta_offset, meta_size = struct.unpack_from("<QQ", toc, at + 0x20)
            if meta_offset + meta_size > physical:
                raise NupError("title %d metadata outside the package" % i)
            self.titles.append(Title(i, tid, root, exefs, romfs, version, meta_offset, meta_size))

        self._kids = struct.unpack_from("<%dI" % ((soff - kids_at) // 4), toc, kids_at)
        self.dirs: list[tuple[str, int, int]] = []   # (name, first child, child count)
        for i in range(nd):
            no, nl, first, count = struct.unpack_from("<4I", toc, dirs_at + 24 * i)
            if first + count > len(self._kids):
                raise NupError("dir %d's children are outside the child table" % i)
            self.dirs.append((name(no, nl), first, count))
        self.files: list[Entry] = []
        for i in range(nf):
            at = files_at + 80 * i
            no, nl, size, offset, stored = struct.unpack_from("<IIQQQ", toc, at)
            codec, flags = toc[at + 64], toc[at + 65]
            e = Entry(i, name(no, nl), size, offset, stored, toc[at + 32:at + 64], codec, flags)
            if codec not in (CODEC_RAW, CODEC_ZSTD):
                raise NupError("%s: unknown codec %d" % (e.name, codec))
            if size and (offset < self.data_offset or offset + stored > physical or not stored):
                raise NupError("%s: data outside the package" % e.name)
            if codec == CODEC_RAW and stored != size:
                raise NupError("%s: raw file with stored size %d != size %d" % (e.name, stored, size))
            _check_name(e.name)
            self.files.append(e)
        if any(k >= nd + nf for k in self._kids):
            raise NupError("child index outside the dir and file tables")
        for d in self.dirs[1:]:
            _check_name(d[0])
        for t in self.titles:
            for part in (t.root, t.exefs, t.romfs):
                if part != NO_DIR and part >= nd:
                    raise NupError("title %s has an invalid dir index" % t.id_hex)
            for _ in self.walk(t.root):   # cycle check
                pass

    # -- tree

    def walk(self, directory: int) -> Iterator[tuple[str, Entry]]:
        """(path relative to directory, file) for every file below it, in package order."""
        if directory == NO_DIR:
            return
        stack = [(directory, "", iter(self._children(directory)))]
        active = {directory}   # dirs on the current path, to catch cycles
        while stack:
            node, prefix, children = stack[-1]
            k = next(children, None)
            if k is None:
                stack.pop()
                active.discard(node)
            elif k < self.nd:
                if k in active:
                    raise NupError("dir cycle in the package")
                active.add(k)
                stack.append((k, prefix + self.dirs[k][0] + "/", iter(self._children(k))))
            else:
                e = self.files[k - self.nd]
                yield prefix + e.name, e

    def _children(self, directory: int) -> tuple[int, ...]:
        _, first, count = self.dirs[directory]
        return self._kids[first:first + count]

    def parts(self, title: Title) -> dict[str, int]:
        return {p: d for p, d in (("exefs", title.exefs), ("romfs", title.romfs)) if d != NO_DIR}

    def find(self, title: Title, path: str) -> Entry:
        """The file at path ('exefs/main', 'romfs/Model/x.bfres') in title."""
        part, _, rest = path.partition("/")
        directory = self.parts(title).get(part)
        if directory is None:
            raise KeyError(path)
        for comp in rest.split("/")[:-1]:
            directory = next((k for k in self._children(directory) if k < self.nd and self.dirs[k][0] == comp), None)
            if directory is None:
                raise KeyError(path)
        leaf = rest.rsplit("/", 1)[-1]
        for k in self._children(directory):
            if k >= self.nd and self.files[k - self.nd].name == leaf:
                return self.files[k - self.nd]
        raise KeyError(path)

    # -- contents

    def chunks(self, e: Entry) -> Iterator[bytes]:
        """The file's contents in pieces of up to CHUNK bytes. Checks sizes and frame bounds, not the hash."""
        if not e.size:
            return
        if e.codec == CODEC_RAW:
            for at in range(0, e.size, CHUNK):
                yield self.read_at(e.offset + at, min(CHUNK, e.size - at))
            return
        d = zstd.ZstdDecompressor()
        got = 0
        for at in range(0, e.stored, CHUNK):
            raw = self.read_at(e.offset + at, min(CHUNK, e.stored - at))
            if d.eof:
                raise NupError("%s: data after the end of its zstd frame" % e.name)
            try:
                out = d.decompress(raw, CHUNK)
                while True:
                    got += len(out)
                    if got > e.size:
                        raise NupError("%s: unpacks past its size of %d bytes" % (e.name, e.size))
                    if out:
                        yield out
                    if d.eof or d.needs_input:
                        break
                    out = d.decompress(b"", CHUNK)
            except zstd.ZstdError as error:
                raise NupError("%s: bad zstd data (%s)" % (e.name, error)) from error
        if not d.eof or d.unused_data:
            raise NupError("%s: its zstd frame does not end where its stored size does" % e.name)
        if got != e.size:
            raise NupError("%s: unpacked to %d bytes, %d expected" % (e.name, got, e.size))

    def read(self, e: Entry, verify: bool = True) -> bytes:
        data = b"".join(self.chunks(e))
        if verify and hashlib.sha256(data).digest() != e.sha256:
            raise NupError("%s: SHA-256 mismatch" % e.name)
        return data

    def metadata(self, title: Title) -> list[tuple[int, bytes]]:
        """The title's metadata records as (type, data)."""
        blob = self.read_at(title.meta_offset, title.meta_size)
        records, at = [], 0
        while at < len(blob):
            if at + 8 > len(blob):
                raise NupError("truncated metadata record")
            kind, n = struct.unpack_from("<II", blob, at)
            if at + 8 + n > len(blob):
                raise NupError("metadata record past the end of its blob")
            records.append((kind, blob[at + 8:at + 8 + n]))
            at += 8 + n
        return records

    def build_id(self, title: Title) -> str | None:
        """main's module id from the metadata, as hex without the zero padding."""
        for kind, data in self.metadata(title):
            if kind == META_BUILD_ID:
                return data.rstrip(b"\0").hex()
        return None

    # -- whole-package operations

    def verify(self, progress: Progress | None = None) -> list[tuple[str, str]]:
        """Hash every file's contents. Returns (path, problem) for each bad file."""
        todo: dict[int, str] = {}
        for t in self.titles:
            for path, e in self.walk(t.root):
                todo.setdefault(e.index, "%s/%s" % (t.id_hex, path))
        total = sum(self.files[i].stored for i in todo)
        done, bad = 0, []
        for index, path in todo.items():
            e = self.files[index]
            h, n = hashlib.sha256(), 0
            try:
                for piece in self.chunks(e):
                    h.update(piece)
                    n += len(piece)
                if h.digest() != e.sha256:
                    bad.append((path, "SHA-256 mismatch"))
            except NupError as error:
                bad.append((path, str(error)))
            done += e.stored
            if progress:
                progress(done, total)
        return bad

    def extract(self, title: Title, dest: str | os.PathLike, only: str | None = None, overwrite: bool = False,
                progress: Progress | None = None) -> int:
        """Write title's exefs/ and romfs/ (or only one) under dest, checking each file's hash. A file already there
        with the right size is kept unless overwrite. Returns the number of files written."""
        dest = Path(dest)
        jobs = []
        for part, directory in self.parts(title).items():
            if only in (None, part):
                jobs += [(dest / part / path, e) for path, e in self.walk(directory)]
        total = sum(e.size for _, e in jobs)
        done = written = 0
        for target, e in jobs:
            if overwrite or not (target.is_file() and target.stat().st_size == e.size):
                target.parent.mkdir(parents=True, exist_ok=True)
                tmp = target.with_name(target.name + ".part")
                h = hashlib.sha256()
                with open(tmp, "wb") as out:
                    for piece in self.chunks(e):
                        h.update(piece)
                        out.write(piece)
                if h.digest() != e.sha256:
                    tmp.unlink()
                    raise NupError("%s: SHA-256 mismatch, the package is damaged" % target)
                os.replace(tmp, target)
                written += 1
            done += e.size
            if progress:
                progress(done, total)
        return written
