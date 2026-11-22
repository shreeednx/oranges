import pytest
from fakes import BSS_SIZE, DATA, DATA_SIZE, RELR_ADDRESSES, RO, make_nso

from oranges import nso


@pytest.mark.parametrize("compress", [True, False])
def test_segments_and_image(compress):
    m = nso.load(make_nso(compress=compress))
    assert [(s.name, s.address) for s in (m.text, m.ro, m.data, m.bss)] == [
        ("text", 0), ("ro", RO), ("data", DATA), ("bss", DATA + DATA_SIZE)]
    assert m.bss.size == BSS_SIZE
    assert len(m.image) % nso.PAGE == 0 and len(m.image) >= DATA + DATA_SIZE + BSS_SIZE
    assert m.build_id == "1234" * 8
    assert m.module_path == "fake.nss"
    assert m.api_info == ["SDK MW+Nintendo+Fake-1_0_0"]
    assert m.mod0 == 8


@pytest.mark.parametrize("hash_style", ["sysv", "gnu"])
def test_symbols(hash_style):
    m = nso.load(make_nso(exports=("a", "b"), imports=("c",), hash_style=hash_style))
    assert [s.name for s in m.symbols] == ["", "a", "b", "c"]
    assert [s.name for s in m.exports] == ["a", "b"]
    assert [s.name for s in m.imports] == ["c"]
    assert m.needed == ["libfake.nss"]


def test_relocations():
    m = nso.load(make_nso())
    got = [(r.address, nso.RELOC_NAMES[r.type], r.symbol, r.addend) for r in m.relocs]
    assert got[:2] == [(0x2180, "RELATIVE", 0, 0x100), (0x2188, "JUMP_SLOT", 2, 0)]
    assert got[2:] == [(a, "RELATIVE", 0, 0x40 + a) for a in RELR_ADDRESSES]


def test_svcs_only_counts_aligned_instructions():
    assert nso.load(make_nso()).svcs() == {0x26: 1, 0x7F: 1}


def test_segment_hash_is_checked():
    blob = bytearray(make_nso(compress=False))
    blob[0xA0] ^= 1
    with pytest.raises(nso.NsoError, match="SHA-256"):
        nso.load(bytes(blob))
    assert nso.load(bytes(blob), verify=False).build_id


def test_not_an_nso():
    with pytest.raises(nso.NsoError, match="not an NSO"):
        nso.load(b"MOD0" + bytes(0x200))


def test_link_reports_imports_nobody_exports():
    a = nso.load(make_nso(exports=("a",), imports=("b", "missing")))
    b = nso.load(make_nso(exports=("b",), imports=("a",)))
    assert nso.link({"a": a, "b": b}) == {"a": ["missing"], "b": []}
