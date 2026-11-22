import hashlib
import struct

import pytest
from fakes import make_nup

from oranges import game
from oranges.nup import CHUNK, Nup, NupError

TREE = {
    "exefs/main": b"NSO0" + bytes(100),
    "exefs/main.npdm": b"META" + bytes(20),
    "romfs/Model/a.bfres.zs": b"\x28\xb5\x2f\xfd game's own zstd",
    "romfs/Model/sub/b.bin": bytes(range(256)) * 50,
    "romfs/empty": b"",
    "romfs/top.txt": "unicode é".encode(),
}
PACKED = {"exefs/main", "romfs/Model/sub/b.bin"}


@pytest.fixture
def package(tmp_path):
    path = tmp_path / "game.nup"
    path.write_bytes(make_nup(TREE, packed=PACKED))
    return path


def write(tmp_path, blob):
    path = tmp_path / "x.nup"
    path.write_bytes(blob)
    return path


def test_walk_lists_every_file_in_order(package):
    with Nup(package) as p:
        (t,) = p.titles
        listed = ["exefs/" + path for path, _ in p.walk(t.exefs)] + ["romfs/" + path for path, _ in p.walk(t.romfs)]
        assert listed == list(TREE)
        assert [path for path, _ in p.walk(t.root)] == list(TREE)


def test_read_raw_and_packed(package):
    with Nup(package) as p:
        (t,) = p.titles
        for path, contents in TREE.items():
            e = p.find(t, path)
            assert e.codec == (path in PACKED)
            assert p.read(e) == contents
        with pytest.raises(KeyError):
            p.find(t, "romfs/Model/missing")


def test_large_packed_file_streams_in_chunks(tmp_path):
    big = hashlib.sha256(b"seed").digest() * (CHUNK // 32 * 2 + 5)
    path = write(tmp_path, make_nup({"exefs/main": big}, packed={"exefs/main"}))
    with Nup(path) as p:
        pieces = list(p.chunks(p.files[0]))
        assert len(pieces) > 2 and all(len(x) <= CHUNK for x in pieces)
        assert b"".join(pieces) == big


def test_metadata_and_supported_build(package, tmp_path):
    with Nup(package) as p:
        assert p.build_id(p.titles[0]) == game.MAIN_BUILD_ID
        assert game.check(p)[0]
    other = write(tmp_path, make_nup(TREE, build_id="00" * 16))
    with Nup(other) as p:
        ok, why = game.check(p)
        assert not ok and "not the supported build" in why
    wrong_title = write(tmp_path, make_nup(TREE, title_id=0x0100000000010000))
    with Nup(wrong_title) as p:
        assert not game.check(p)[0]


def test_verify_finds_a_damaged_file(package):
    with Nup(package) as p:
        assert p.verify() == []
        target = p.find(p.titles[0], "romfs/top.txt")
    blob = bytearray(package.read_bytes())
    blob[target.offset] ^= 0xFF
    package.write_bytes(blob)
    with Nup(package) as p:
        assert p.verify() == [("%s/romfs/top.txt" % game.TITLE_ID.__format__("016X"), "SHA-256 mismatch")]
        with pytest.raises(NupError, match="SHA-256"):
            p.read(target)


def test_damaged_zstd_frame_is_reported(package):
    with Nup(package) as p:
        target = p.find(p.titles[0], "exefs/main")
    blob = bytearray(package.read_bytes())
    blob[target.offset + target.stored - 2] ^= 0xFF   # inside the frame's checksum or last block
    package.write_bytes(blob)
    with Nup(package) as p:
        assert [path for path, _ in p.verify()] == ["%016X/exefs/main" % game.TITLE_ID]


def test_damaged_toc_is_refused(package):
    blob = bytearray(package.read_bytes())
    blob[0x80 + 0x40] ^= 1
    package.write_bytes(blob)
    with pytest.raises(NupError, match="TOC SHA-256"):
        Nup(package)


def test_incomplete_package_is_refused(package):
    package.write_bytes(package.read_bytes()[:-1])
    with pytest.raises(NupError, match="incomplete"):
        Nup(package)


def test_not_a_package(tmp_path):
    with pytest.raises(NupError, match="not a .nup"):
        Nup(write(tmp_path, b"PFS0" + bytes(0x100)))


@pytest.mark.parametrize("bad", ["romfs/../escape", "romfs/a\\b"])
def test_unsafe_names_are_refused(tmp_path, bad):
    with pytest.raises(NupError, match="unsafe name"):
        Nup(write(tmp_path, make_nup({"exefs/main": b"x", bad: b"y"})))


def test_dir_cycle_is_refused(tmp_path):
    blob = bytearray(make_nup({"romfs/a/b": b"x"}))
    nd = struct.unpack_from("<Q", blob, 0x80 + 8)[0]
    nf = struct.unpack_from("<Q", blob, 0x80 + 16)[0]
    kids_at = 0x80 + 0x30 + 0x40 + 24 * nd + 80 * nf
    # dirs are root, romfs, romfs/a; children: root -> romfs, romfs -> a, a -> file. Point a's child at romfs.
    struct.pack_into("<I", blob, kids_at + 8, 1)
    toc_size = struct.unpack_from("<Q", blob, 0x10)[0]
    blob[0x18:0x38] = hashlib.sha256(blob[0x80:0x80 + toc_size]).digest()
    with pytest.raises(NupError, match="cycle"):
        Nup(write(tmp_path, bytes(blob)))


def test_extract_writes_checks_and_skips(package, tmp_path):
    out = tmp_path / "out"
    with Nup(package) as p:
        t = p.titles[0]
        assert p.extract(t, out) == len(TREE)
        for path, contents in TREE.items():
            assert (out / path).read_bytes() == contents
        assert p.extract(t, out) == 0
        assert p.extract(t, out, overwrite=True) == len(TREE)
        assert p.extract(t, tmp_path / "exefs_only", only="exefs") == 2
        assert not (tmp_path / "exefs_only" / "romfs").exists()
        assert not list(out.rglob("*.part"))
