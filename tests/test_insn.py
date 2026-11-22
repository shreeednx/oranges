import importlib.util
import shutil

import pytest

needs = pytest.mark.skipif(not (shutil.which("clang") and importlib.util.find_spec("unicorn")
                                and importlib.util.find_spec("capstone")),
                           reason="needs clang, unicorn and capstone")

# Encodings from the game that suyu's translator lacks (oranges.insn), plus the variants of the same forms
PMULL = [0x4E20E000, 0x0E21E020, 0x0E22E041, 0x4E22E042, 0x0E27E0E5, 0x0EE2E020, 0x4EE2E020]
NONTEMPORAL_Q = [0xAC000400, 0xAC010C02, 0xAC400400, 0xAC3F07E0, 0xAC7F0BE2]
SQSHL = [0x4F287400, 0x4F2876F7]


@needs
@pytest.mark.parametrize("words", [PMULL, NONTEMPORAL_Q, SQSHL], ids=["pmull", "ldnp_stnp_q", "sqshl"])
def test_forms_match_unicorn(words):
    from oranges import insncheck
    assert insncheck.run(words, states=24) == 0


def test_emit_claims_only_its_forms():
    from oranges import insn
    assert all(insn.emit(w) for w in PMULL + NONTEMPORAL_Q + SQSHL)
    assert insn.emit(0xD503201F) is None       # nop: suyu's
    assert insn.emit(0x0E60E000) is None       # pmull size 01: reserved, left to suyu (which traps)
    assert insn.emit(0x6C000400) is None       # stnp d0, d1: suyu's
