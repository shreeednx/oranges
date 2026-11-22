import shutil
import struct

import pytest
from fakes import make_nso

from oranges import lift

B_0x40, BL_0x80, CBZ_X0_8, NOP, RET, SVC_26 = 0x14000010, 0x94000010, 0xB4000040, 0xD503201F, 0xD65F03C0, 0xD40004C1
# 0x00 b 0x40 | 0x40 bl 0x80; cbz x0, 0x4c; nop; ret | 0x80 svc 0x26; ret | 0x100 svc 0x26; svc 0x7f; ret
CODE = {0x00: B_0x40, 0x40: BL_0x80, 0x44: CBZ_X0_8, 0x48: NOP, 0x4C: RET, 0x80: SVC_26, 0x84: RET, 0x108: RET}


@pytest.fixture
def exefs(tmp_path):
    d = tmp_path / "exefs"
    d.mkdir()
    (d / "main").write_bytes(make_nso(code=CODE))
    (d / "sdk").write_bytes(make_nso(exports=("imported",), imports=(), code=CODE))
    return d


def test_image_relocates_and_binds(exefs):
    img = lift.build_image(exefs)
    main, sdk = img.modules
    assert (main.name, sdk.name) == ("main", "sdk") and sdk.base > main.base

    def q(a):
        return struct.unpack_from("<Q", img.mem, a - lift.nx_image.MAIN_BASE)[0]
    assert q(main.base + 0x2180) == main.base + 0x100            # RELA RELATIVE
    assert q(main.base + 0x2188) == sdk.base + 0x100             # JUMP_SLOT bound to sdk's export
    for a in (0x2100, 0x2108, 0x2118):                           # RELR (oranges.kit)
        assert q(main.base + a) == main.base + 0x40 + a
    assert not img.unresolved


def test_discover_finds_functions_and_bodies(exefs):
    img = lift.build_image(exefs)
    lifter = lift.discover(img)
    base = img.modules[0].base
    main_funcs = {e - base: sorted(pc - base for pc in body)
                  for e, (body, _) in lifter.funcs.items() if lift.module_of(img, e) == "main"}
    assert main_funcs == {0x0: [0x0, 0x40, 0x44, 0x48, 0x4C], 0x80: [0x80, 0x84], 0x100: [0x100, 0x104, 0x108]}
    cov = lift.coverage(img, lifter)
    assert cov["main"]["functions"] == 3 and cov["main"]["reached"] == 10


def test_run_writes_report(exefs, tmp_path):
    report = lift.run(exefs, tmp_path / "work", say=lambda *_: None)
    out = tmp_path / "work" / "image"
    assert {p.name for p in out.iterdir()} == {"image.bin", "image.json", "functions.txt", "report.json"}
    assert report["modules"]["main"]["functions"] == 3
    assert len((out / "functions.txt").read_text().splitlines()) == 6


@pytest.mark.skipif(not shutil.which("clang++"), reason="needs clang++ to build suyu's translator")
def test_translator_covers_the_fake_code(exefs):
    img = lift.build_image(exefs)
    t = lift.translate_coverage(img, lift.discover(img))
    assert t["distinct_words"] == 1 and t["untranslatable_words"] == 0   # the nop (branches and svcs are the lifter's)
