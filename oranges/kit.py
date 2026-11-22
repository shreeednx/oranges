"""The vendored a64-kit (third_party/a64-kit) on sys.path, with oranges' additions installed by wrapping, never by
editing the kit.

    from oranges import kit      # then: import nso, nx_image, a64_lift (the kit's modules)

DT_RELR (dynamic tags 35/36/37). The game's modules (SDK 20.5.4) keep their relative relocations in the packed RELR
format as well as RELA, and the kit's tools/nso.py reads only RELA. Each RELR entry is either an address (even: relocate
that word, then continue after it) or a bitmap (odd: bit i of the 63 bits above bit 0 relocates the word i words further
on, then the cursor moves 63 words). The addend is implicit: the module offset stored at the location. They are added to
Nso.relocs as ordinary RELATIVE entries, so hos/nx_image.py applies them unchanged. Without them every vtable and
function pointer they cover stays a module offset.
Adapted from Longshot's nx2/nso2.py (GPL-3.0-or-later, commit 67ba729d).
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
for _sub in ("lift", "tools", "hos", "rt"):
    _path = os.path.join(KIT, _sub)
    if _path not in sys.path:
        sys.path.insert(0, _path)

import nso  # noqa: E402  (the kit's tools/nso.py)

DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
nso.DT.update({DT_RELRSZ: "RELRSZ", DT_RELR: "RELR", DT_RELRENT: "RELRENT", 21: "DEBUG", 0x6FFFFFFB: "FLAGS_1"})


def relr_offsets(image, start: int, size: int) -> list[int]:
    out, where = [], 0
    for off in range(start, start + size, 8):
        e = struct.unpack_from("<Q", image, off)[0]
        if not e & 1:
            out.append(e)
            where = e + 8
        else:
            bits, i = e >> 1, 0
            while bits:
                if bits & 1:
                    out.append(where + 8 * i)
                bits >>= 1
                i += 1
            where += 63 * 8
    return out


if not getattr(nso.Nso, "_oranges_relr", False):
    _base_mod0 = nso.Nso._mod0

    def _mod0(self):
        _base_mod0(self)
        d = lambda k: self.dynamic.get(k, [0])[0]   # noqa: E731
        start, size = d("RELR"), d("RELRSZ")
        self.relr_count = 0
        if start and size:
            if d("RELRENT") not in (0, 8):
                raise ValueError("%s: RELRENT %d" % (self.name, d("RELRENT")))
            for off in relr_offsets(self.image, start, size):
                self.relocs.append({"offset": off, "type": "RELATIVE", "sym": None, "addend": self.u64(off)})
                self.relr_count += 1

    nso.Nso._mod0 = _mod0
    nso.Nso._oranges_relr = True
