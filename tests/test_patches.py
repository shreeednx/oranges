import struct

import pytest
from fakes import make_nso
from test_lift import CODE, exefs  # noqa: F401  (the fixture)

from oranges import lift, patches

FAKE_ID = "1234" * 8   # (make_nso's default module id)
NOP, RET = 0xD503201F, 0xD65F03C0


def word(img, a):
    return struct.unpack_from("<I", img.mem, a - lift.nx_image.MAIN_BASE)[0]


def test_other_builds_are_left_alone(exefs):  # noqa: F811
    img = lift.build_image(exefs)   # (the fake main is not the supported build: no patch, no error)
    assert img.patches == []
    assert word(img, img.modules[0].base + 0x48) == CODE[0x48]


def test_a_patch_replaces_the_words_it_names(exefs):  # noqa: F811
    img = lift.build_image(exefs)
    p = patches.Patch("test", 0x48, (CODE[0x48], CODE[0x4C]), (RET, NOP))
    assert patches.apply(img, (p,), FAKE_ID) == ["test"]
    base = img.modules[0].base
    assert (word(img, base + 0x48), word(img, base + 0x4C)) == (RET, NOP)


def test_a_patch_refuses_other_words(exefs):  # noqa: F811
    img = lift.build_image(exefs)
    with pytest.raises(patches.PatchError, match="main\\+0x48 holds"):
        patches.apply(img, (patches.Patch("test", 0x48, (RET,), (NOP,)),), FAKE_ID)
    with pytest.raises(patches.PatchError, match="2 words for 1"):
        patches.apply(img, (patches.Patch("test", 0x48, (CODE[0x48],), (NOP, NOP)),), FAKE_ID)


def test_the_resource_handler_patch_is_what_its_comment_says():
    capstone = pytest.importorskip("capstone")
    p = patches.PATCHES[0]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def dis(words):
        return ["%s %s" % (i.mnemonic, i.op_str)
                for i in md.disasm(struct.pack("<%dI" % len(words), *words), 0x08000000 + p.main_offset)]
    assert dis(p.original) == ["add x8, x8, #0x378", "ldr x8, [x8]", "cbz x8, #0x86d65bc", "ldr x8, [x8, #8]"]
    assert dis(p.words) == ["ldr x8, [x8, #0x378]", "cbz x8, #0x86d65bc", "ldr x8, [x8, #8]", "cbz x8, #0x86d65bc"]


def test_make_nso_module_id_is_the_fake_id():
    from oranges import nso
    assert nso.load(make_nso()).build_id == FAKE_ID
