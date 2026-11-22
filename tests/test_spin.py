import shutil

import pytest
from fakes import make_nso

from oranges import lift, spin

B_0x40, RET = 0x14000010, 0xD65F03C0
LDR_W9_X8, CMP_W9_1, BEQ_0x40 = 0xB9400109, 0x7100053F, 0x54FFFFC0      # 0x40 ldr w9, [x8]; cmp w9, #1; b.eq 0x40
LDR_X8_X8, CBNZ_X8_0x4C = 0xF9400108, 0xB5FFFFE8                         # 0x4c ldr x8, [x8]; cbnz x8, 0x4c
# 0x00 b 0x40 | 0x40 a spinlock wait (an invariant load) | 0x4c a list walk (the load writes its own base) | ret
CODE = {0x00: B_0x40, 0x40: LDR_W9_X8, 0x44: CMP_W9_1, 0x48: BEQ_0x40, 0x4C: LDR_X8_X8, 0x50: CBNZ_X8_0x4C,
        0x54: RET}
SPIN_LOAD = "c->x[9]=recomp_load32(c,c->x[8] + 0);"
WALK_LOAD = "c->x[8]=recomp_load64(c,c->x[8] + 0);"


def body_and_texts(extra=None):
    """CODE as the lifter has it, with suyu's C for the three ops (the branches are the lifter's)"""
    body = {0x40: (("op",), LDR_W9_X8), 0x44: (("op",), CMP_W9_1), 0x48: (("bcond", 0x40, 0), BEQ_0x40),
            0x4C: (("op",), LDR_X8_X8), 0x50: (("cbz", 0x4C, 8, 1, 1), CBNZ_X8_0x4C), 0x54: (("ret",), RET)}
    texts = {LDR_W9_X8: (SPIN_LOAD, 0), LDR_X8_X8: (WALK_LOAD, 0),
             CMP_W9_1: ("{ uint64_t _a=c->x[9], _b=1ULL; uint64_t _r=_a-_b; c->z=(_r==0); }", 0)}
    body.update(extra or {})
    return body, texts


def test_invariant_load_of_a_loop_gets_the_barrier():
    body, texts = body_and_texts()
    assert spin.spin_loads(body, texts) == {0x40}   # (not 0x4c: its address changes on every pass)


def test_loop_that_writes_the_base_or_calls_needs_none():
    # a call in the spinlock loop writes x8 (caller-saved): its load is no longer invariant
    body, texts = body_and_texts({0x44: (("bl", 0x100), 0x94000000)})
    assert spin.spin_loads(body, texts) == set()
    # a loop with a hole (an address the function doesn't own) is not looked at
    body, texts = body_and_texts()
    del body[0x44]
    assert spin.spin_loads(body, texts) == set()


@pytest.fixture
def exefs(tmp_path):
    d = tmp_path / "exefs"
    d.mkdir()
    (d / "main").write_bytes(make_nso(code=CODE))
    (d / "sdk").write_bytes(make_nso(exports=("_init_libc0", "imported"), imports=()))
    npdm = bytearray(0x80)
    npdm[:4] = b"META"
    npdm[0x0E], npdm[0x0F] = 44, 0
    npdm[0x1C:0x20] = (0x10000).to_bytes(4, "little")
    (d / "main.npdm").write_bytes(bytes(npdm))
    return d


@pytest.mark.skipif(not shutil.which("clang++"), reason="needs clang++ to build suyu's translator")
def test_written_c_has_the_barrier_after_the_spin_load(exefs, tmp_path):
    report = lift.run(exefs, tmp_path / "work", write=True, say=lambda *_: None)
    text = "".join(p.read_text() for p in sorted((tmp_path / "work" / "gen").glob("code_*.c")))
    fn = text[text.index("void f_08000000(Ctx *__restrict"):]
    fn = fn[:fn.index("\n}\n")]
    lines = [ln.strip() for ln in fn.splitlines()]
    spin_at = next(i for i, ln in enumerate(lines) if "recomp_load32(c," in ln)
    walk_at = next(i for i, ln in enumerate(lines) if "recomp_load64(c," in ln)
    assert lines[spin_at + 1] == spin.BARRIER
    assert spin.BARRIER not in lines[walk_at + 1]
    assert report["emit"]["spin_barriers"] == 1
