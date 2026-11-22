"""Load a Switch NSO module (exefs/main, sdk, subsdk*, rtld) into its memory image and read its dynamic linking info.

NSO header (little-endian, 0x100 bytes):
  +0x00 "NSO0"  +0x0c u32 flags (bits 0-2: text/ro/data LZ4-compressed, bits 3-5: check segment hash)
  +0x10/+0x20/+0x30 text/ro/data segment: u32 file offset, u32 memory offset, u32 size
  +0x3c u32 bss size   +0x40 module id (32 bytes; 16 or 20 significant)   +0x60 u32 x3 compressed sizes
  +0x88/+0x90/+0x98 api info, dynstr, dynsym: u32 offset (from ro start), u32 size
  +0xa0 SHA-256 of each decompressed segment
The image starts with a branch over a u32 at +4: the offset of the MOD0 header, which locates .dynamic, bss and
.eh_frame_hdr. Addresses inside the image are offsets from its base (the module is position independent).
"""

import hashlib
import re
import struct
from dataclasses import dataclass, field

import lz4.block

PAGE = 0x1000

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB = 0, 1, 2, 4, 5, 6
DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT, DT_INIT, DT_FINI = 7, 8, 9, 10, 11, 12, 13
DT_JMPREL, DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 23, 25, 26, 27, 28
DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
DT_GNU_HASH = 0x6FFFFEF5

R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027
RELOC_NAMES = {R_AARCH64_ABS64: "ABS64", R_AARCH64_GLOB_DAT: "GLOB_DAT", R_AARCH64_JUMP_SLOT: "JUMP_SLOT",
               R_AARCH64_RELATIVE: "RELATIVE"}

STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_FUNC = 2
SHN_UNDEF = 0

# SVC #imm, imm < 0x100 (every Horizon syscall): d4 00 0x 01 | imm << 5
_SVC = re.compile(rb"[\x01\x21\x41\x61\x81\xa1\xc1\xe1][\x00-\x1f]\x00\xd4", re.DOTALL)


class NsoError(Exception):
    pass


@dataclass(frozen=True, slots=True)
class Segment:
    name: str
    address: int   # from the image base
    size: int


@dataclass(frozen=True, slots=True)
class Symbol:
    index: int
    name: str
    value: int
    size: int
    bind: int
    type: int
    shndx: int

    @property
    def defined(self) -> bool:
        return self.shndx != SHN_UNDEF


@dataclass(frozen=True, slots=True)
class Reloc:
    address: int
    type: int
    symbol: int   # index into symbols, 0 for none
    addend: int


@dataclass
class Nso:
    module_id: bytes
    text: Segment
    ro: Segment
    data: Segment
    bss: Segment
    image: bytearray           # text, ro and data at their addresses, bss zeroed
    api_info: list[str]        # "SDK MW+<vendor>+<library>" strings
    module_path: str           # the path at the start of ro, when present
    mod0: int = 0
    dynamic: dict[int, list[int]] = field(default_factory=dict)
    symbols: list[Symbol] = field(default_factory=list)
    relocs: list[Reloc] = field(default_factory=list)

    @property
    def build_id(self) -> str:
        return self.module_id.rstrip(b"\0").hex()

    def u32(self, address: int) -> int:
        return struct.unpack_from("<I", self.image, address)[0]

    def u64(self, address: int) -> int:
        return struct.unpack_from("<Q", self.image, address)[0]

    def cstr(self, address: int) -> str:
        end = self.image.index(0, address)
        return self.image[address:end].decode("utf-8", "replace")

    def dyn(self, tag: int, default: int | None = None) -> int | None:
        values = self.dynamic.get(tag)
        return values[0] if values else default

    @property
    def needed(self) -> list[str]:
        strtab = self.dyn(DT_STRTAB)
        return [self.cstr(strtab + v) for v in self.dynamic.get(DT_NEEDED, [])] if strtab is not None else []

    @property
    def imports(self) -> list[Symbol]:
        return [s for s in self.symbols if not s.defined and s.name]

    @property
    def exports(self) -> list[Symbol]:
        return [s for s in self.symbols if s.defined and s.bind in (STB_GLOBAL, STB_WEAK) and s.name]

    def svcs(self) -> dict[int, int]:
        """{syscall number: count} of SVC instructions in text."""
        counts: dict[int, int] = {}
        text = bytes(self.image[self.text.address:self.text.address + self.text.size])
        for m in _SVC.finditer(text):
            if m.start() % 4 == 0:
                word = struct.unpack_from("<I", text, m.start())[0]
                imm = (word >> 5) & 0xFFFF
                counts[imm] = counts.get(imm, 0) + 1
        return counts


def load(blob: bytes, verify: bool = True) -> Nso:
    if len(blob) < 0x100 or blob[:4] != b"NSO0":
        raise NsoError("not an NSO (magic %r)" % blob[:4])
    flags = struct.unpack_from("<I", blob, 0x0C)[0]
    headers = [struct.unpack_from("<III", blob, at) for at in (0x10, 0x20, 0x30)]
    stored = struct.unpack_from("<III", blob, 0x60)
    bss_size = struct.unpack_from("<I", blob, 0x3C)[0]
    module_id = blob[0x40:0x60]
    api_off, api_size, _dynstr_off, _dynstr_size, _dynsym_off, _dynsym_size = struct.unpack_from("<6I", blob, 0x88)

    segments = []
    image = bytearray()
    for i, (name, (file_off, mem_off, size)) in enumerate(zip(("text", "ro", "data"), headers, strict=True)):
        compressed = flags >> i & 1
        n = stored[i] if compressed else size
        if file_off + n > len(blob):
            raise NsoError("%s segment outside the file" % name)
        raw = blob[file_off:file_off + n]
        try:
            seg = lz4.block.decompress(raw, uncompressed_size=size) if compressed else raw
        except lz4.block.LZ4BlockError as error:
            raise NsoError("%s segment: bad LZ4 data (%s)" % (name, error)) from error
        if len(seg) != size:
            raise NsoError("%s segment is %d bytes, %d expected" % (name, len(seg), size))
        if verify and flags >> (3 + i) & 1 and hashlib.sha256(seg).digest() != blob[0xA0 + 0x20 * i:0xC0 + 0x20 * i]:
            raise NsoError("%s segment SHA-256 mismatch" % name)
        if mem_off < len(image):
            raise NsoError("%s segment overlaps the one before it" % name)
        image.extend(bytes(mem_off - len(image)))
        image.extend(seg)
        segments.append(Segment(name, mem_off, size))
    text, ro, data = segments
    bss = Segment("bss", data.address + data.size, bss_size)
    image.extend(bytes(_align(bss.address + bss.size, PAGE) - len(image)))

    api = bytes(image[ro.address + api_off:ro.address + api_off + api_size]) if api_size else b""
    api_info = [s.decode("ascii", "replace") for s in api.split(b"\0") if s]
    path_len = struct.unpack_from("<I", image, ro.address + 4)[0] if ro.size >= 8 else 0
    module_path = ""
    if struct.unpack_from("<I", image, ro.address)[0] == 0 and 0 < path_len <= min(0x400, ro.size - 8):
        module_path = image[ro.address + 8:ro.address + 8 + path_len].split(b"\0")[0].decode("utf-8", "replace")

    nso = Nso(module_id, text, ro, data, bss, image, api_info, module_path)
    _read_dynamic(nso)
    return nso


def _align(n: int, a: int) -> int:
    return (n + a - 1) & ~(a - 1)


def _read_dynamic(nso: Nso) -> None:
    mod0 = nso.u32(4)
    if mod0 + 0x1C > len(nso.image) or nso.image[mod0:mod0 + 4] != b"MOD0":
        raise NsoError("no MOD0 header")
    nso.mod0 = mod0
    dynamic = mod0 + struct.unpack_from("<i", nso.image, mod0 + 4)[0]
    at = dynamic
    while True:
        if at + 16 > len(nso.image):
            raise NsoError(".dynamic runs past the image")
        tag, value = struct.unpack_from("<qQ", nso.image, at)
        if tag == DT_NULL:
            break
        nso.dynamic.setdefault(tag, []).append(value)
        at += 16

    symtab, strtab = nso.dyn(DT_SYMTAB), nso.dyn(DT_STRTAB)
    if symtab is not None and strtab is not None:
        syment = nso.dyn(DT_SYMENT, 24)
        for i in range(_symbol_count(nso, symtab, strtab, syment)):
            name_off, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", nso.image, symtab + i * syment)
            name = nso.cstr(strtab + name_off) if name_off else ""
            nso.symbols.append(Symbol(i, name, value, size, info >> 4, info & 0xF, shndx))

    for table, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        start, size = nso.dyn(table), nso.dyn(size_tag, 0)
        if start is None:
            continue
        for at in range(start, start + size, 24):
            offset, info, addend = struct.unpack_from("<QQq", nso.image, at)
            nso.relocs.append(Reloc(offset, info & 0xFFFFFFFF, info >> 32, addend))
    relr, relr_size = nso.dyn(DT_RELR), nso.dyn(DT_RELRSZ, 0)
    if relr is not None:   # packed relative relocations: an address, then bitmaps of the 63 words after it
        where = 0
        for at in range(relr, relr + relr_size, 8):
            entry = nso.u64(at)
            if entry & 1 == 0:
                nso.relocs.append(Reloc(entry, R_AARCH64_RELATIVE, 0, nso.u64(entry)))
                where = entry + 8
            else:
                for bit in range(63):
                    if entry >> (bit + 1) & 1:
                        address = where + bit * 8
                        nso.relocs.append(Reloc(address, R_AARCH64_RELATIVE, 0, nso.u64(address)))
                where += 63 * 8


def _symbol_count(nso: Nso, symtab: int, strtab: int, syment: int) -> int:
    hash_table = nso.dyn(DT_HASH)
    if hash_table is not None:
        return nso.u32(hash_table + 4)   # nchain
    gnu = nso.dyn(DT_GNU_HASH)
    if gnu is not None:
        nbuckets, symoffset, bloom_size, _shift = struct.unpack_from("<4I", nso.image, gnu)
        buckets = gnu + 16 + 8 * bloom_size
        chains = buckets + 4 * nbuckets
        last = max(struct.unpack_from("<%dI" % nbuckets, nso.image, buckets), default=0)
        if last < symoffset:
            return symoffset
        while not nso.u32(chains + 4 * (last - symoffset)) & 1:
            last += 1
        return last + 1
    if strtab > symtab:
        return (strtab - symtab) // syment
    raise NsoError("cannot tell how many dynamic symbols there are")


def link(modules: dict[str, Nso]) -> dict[str, list[str]]:
    """{module: [import names no other module exports]}: what the host has to provide itself."""
    exported = {s.name for m in modules.values() for s in m.exports}
    return {name: sorted({s.name for s in m.imports} - exported) for name, m in modules.items()}
