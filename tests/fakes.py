"""Synthetic .nup packages and NSO modules, so tests never need the game."""

import hashlib
import struct
from compression import zstd

import lz4.block

from oranges import game, nso
from oranges.nup import META_BUILD_ID, META_LABEL


def _align(n: int, a: int = 16) -> int:
    return (n + a - 1) & ~(a - 1)


def make_nup(tree: dict[str, bytes], *, packed: set[str] = frozenset(), title_id: int = game.TITLE_ID,
             build_id: str | None = game.MAIN_BUILD_ID, version: int = 0) -> bytes:
    """A one-title package. tree maps 'exefs/...' and 'romfs/...' paths to contents. Paths in packed are stored
    as zstd."""
    children: dict[str, list[tuple[str, str]]] = {"": []}
    order = [""]
    for path in tree:
        parts = path.split("/")
        for i in range(1, len(parts)):
            d, parent = "/".join(parts[:i]), "/".join(parts[:i - 1])
            if d not in children:
                children[d] = []
                order.append(d)
                children[parent].append(("d", d))
        children["/".join(parts[:-1])].append(("f", path))
    dir_index = {d: i for i, d in enumerate(order)}
    paths = list(tree)
    file_index = {p: i for i, p in enumerate(paths)}
    nd, nf = len(order), len(paths)

    strings = bytearray()

    def string(name: str) -> tuple[int, int]:
        raw = name.encode()
        strings.extend(raw)
        return len(strings) - len(raw), len(raw)

    kids: list[int] = []
    dir_records = []
    for d in order:
        first = len(kids)
        kids += [dir_index[p] if kind == "d" else nd + file_index[p] for kind, p in children[d]]
        dir_records.append((*string(d.rsplit("/", 1)[-1] if d else ""), first, len(children[d])))
    file_names = [string(p.rsplit("/", 1)[-1]) for p in paths]

    tables = 0x30 + 0x40 + 24 * nd + 80 * nf + 4 * len(kids)
    toc_size = tables + len(strings)
    data_offset = _align(0x80 + toc_size)
    data = bytearray()
    extents = []
    for p in paths:
        stored = zstd.compress(tree[p]) if p in packed else tree[p]
        at = data_offset + len(data)
        extents.append((at, len(stored)) if tree[p] else (0, 0))
        data += stored + bytes(_align(len(stored)) - len(stored))
    meta = b""
    if build_id is not None:
        meta += struct.pack("<II", META_BUILD_ID, 32) + bytes.fromhex(build_id).ljust(32, b"\0")
    meta += struct.pack("<II", META_LABEL, 13) + b"output_update"
    meta_offset = data_offset + len(data)

    toc = bytearray(b"TOC\x01" + struct.pack("<IQQQQ", 1, nd, nf, tables, len(strings)).ljust(0x2C, b"\0"))
    title = struct.pack("<QIIII", title_id, 0, dir_index["exefs"] if "exefs" in dir_index else 0xFFFFFFFF,
                        dir_index["romfs"] if "romfs" in dir_index else 0xFFFFFFFF, version)
    toc += title.ljust(0x20, b"\0") + struct.pack("<QQ", meta_offset, len(meta)).ljust(0x20, b"\0")
    for r in dir_records:
        toc += struct.pack("<4IQ", *r, 0)
    for p, (no, nl), (at, stored) in zip(paths, file_names, extents, strict=True):
        toc += struct.pack("<IIQQQ", no, nl, len(tree[p]), at, stored)
        toc += hashlib.sha256(tree[p]).digest() + bytes([1 if p in packed else 0, 0]) + bytes(14)
    toc += struct.pack("<%dI" % len(kids), *kids) + strings
    assert len(toc) == toc_size

    body = bytes(toc).ljust(data_offset - 0x80, b"\0") + bytes(data) + meta
    file_size = 0x80 + len(body)
    header = b"NUP\x01" + struct.pack("<HHQQ", 0x80, 1, 0x80, toc_size) + hashlib.sha256(toc).digest()
    header += struct.pack("<QQ", data_offset, file_size)
    return header.ljust(0x80, b"\0") + body


TEXT, RO, DATA = 0x0, 0x1000, 0x2000
DATA_SIZE, BSS_SIZE = 0x200, 0x800
RELR_ADDRESSES = [0x2100, 0x2108, 0x2118]


def make_nso(*, exports: tuple[str, ...] = ("exported",), imports: tuple[str, ...] = ("imported",),
             hash_style: str = "sysv", compress: bool = True, module_id: bytes = b"\x12\x34" * 8,
             code: dict[int, int] | None = None) -> bytes:
    """A module with MOD0, .dynamic, symbols, RELA/JMPREL/RELR relocations and two SVCs. code: {text offset: word}
    placed over the defaults. The symbol table comes before the string table, which a64-kit's loader needs (it
    counts symbols as (strtab - symtab) / 24)."""
    text = bytearray(0x1000)
    struct.pack_into("<II", text, 0, 0x14000002, 8)   # b +8; MOD0 at 8
    text[8:12] = b"MOD0"
    struct.pack_into("<iii", text, 12, DATA - 8, DATA + DATA_SIZE - 8, DATA + DATA_SIZE + BSS_SIZE - 8)
    struct.pack_into("<II", text, 0x100, 0xD4000001 | 0x26 << 5, 0xD4000001 | 0x7F << 5)   # svc 0x26, svc 0x7f
    text[0x201:0x205] = struct.pack("<I", 0xD4000001 | 0x11 << 5)   # misaligned: not an instruction
    for at, word in (code or {}).items():
        struct.pack_into("<I", text, at, word)

    ro = bytearray(0x1000)
    path = b"fake.nss"
    struct.pack_into("<II", ro, 0, 0, len(path))
    ro[8:8 + len(path)] = path
    api = b"SDK MW+Nintendo+Fake-1_0_0\0"
    ro[0x40:0x40 + len(api)] = api
    strtab = bytearray(b"\0")
    names = {}
    for name in ("libfake.nss", *exports, *imports):
        names[name] = len(strtab)
        strtab += name.encode() + b"\0"
    ro[0x200:0x200 + len(strtab)] = strtab
    symbols = [(0, 0, 0, 0, 0)]
    symbols += [(names[n], nso.STB_GLOBAL << 4 | nso.STT_FUNC, 1, 0x100 + 4 * i, 4) for i, n in enumerate(exports)]
    symbols += [(names[n], nso.STB_GLOBAL << 4 | nso.STT_FUNC, 0, 0, 0) for n in imports]
    for i, (no, info, shndx, value, size) in enumerate(symbols):
        struct.pack_into("<IBBHQQ", ro, 0x100 + 24 * i, no, info, 0, shndx, value, size)
    if hash_style == "sysv":
        struct.pack_into("<II", ro, 0x600, 1, len(symbols))
    else:   # gnu: one bucket that starts at symbol 1, chain ends at the last symbol
        struct.pack_into("<4IQI", ro, 0x600, 1, 1, 1, 0, 0, 1)
        for i in range(1, len(symbols)):
            struct.pack_into("<I", ro, 0x600 + 28 + 4 * (i - 1), 1 if i == len(symbols) - 1 else 0)
    relas = [(0x2180, nso.R_AARCH64_RELATIVE, 0, 0x100), (0x2188, nso.R_AARCH64_JUMP_SLOT, len(symbols) - 1, 0)]
    for i, (offset, kind, sym, addend) in enumerate(relas):
        struct.pack_into("<QQq", ro, 0x700 + 24 * i, offset, sym << 32 | kind, addend)
    struct.pack_into("<QQ", ro, 0x780, RELR_ADDRESSES[0], 0b1011)

    data = bytearray(DATA_SIZE)
    dynamic = [(nso.DT_NEEDED, names["libfake.nss"]), (nso.DT_STRTAB, RO + 0x200), (nso.DT_STRSZ, len(strtab)),
               (nso.DT_SYMTAB, RO + 0x100), (nso.DT_SYMENT, 24),
               (nso.DT_HASH if hash_style == "sysv" else nso.DT_GNU_HASH, RO + 0x600),
               (nso.DT_RELA, RO + 0x700), (nso.DT_RELASZ, 24), (nso.DT_RELAENT, 24),
               (nso.DT_JMPREL, RO + 0x718), (nso.DT_PLTRELSZ, 24),
               (nso.DT_RELR, RO + 0x780), (nso.DT_RELRSZ, 16), (nso.DT_RELRENT, 8), (nso.DT_NULL, 0)]
    for i, (tag, value) in enumerate(dynamic):
        struct.pack_into("<qQ", data, 16 * i, tag, value)
    for address in RELR_ADDRESSES:
        struct.pack_into("<Q", data, address - DATA, 0x40 + address)   # implicit addends

    segments = [bytes(text), bytes(ro), bytes(data)]
    blobs = [lz4.block.compress(s, store_size=False) if compress else s for s in segments]
    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    struct.pack_into("<I", header, 0x0C, (0b111 if compress else 0) | 0b111000)
    at = 0x100
    for i, (mem, seg, blob) in enumerate(zip((TEXT, RO, DATA), segments, blobs, strict=True)):
        struct.pack_into("<III", header, 0x10 + 0x10 * i, at, mem, len(seg))
        struct.pack_into("<I", header, 0x60 + 4 * i, len(blob))
        header[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(seg).digest()
        at += len(blob)
    struct.pack_into("<I", header, 0x3C, BSS_SIZE)
    header[0x40:0x40 + len(module_id)] = module_id
    struct.pack_into("<II", header, 0x88, 0x40, len(api))
    return bytes(header) + b"".join(blobs)
