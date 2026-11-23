import os
import struct

import numpy as np

from oranges import drive

PAGE = 0x1000


def sparse(path, pages):
    """a MEM-style dump: {guest address: bytes} written at their addresses, holes elsewhere"""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        for a, data in pages.items():
            data = data + bytes(-len(data) % PAGE)
            os.pwrite(fd, data, a)
        os.ftruncate(fd, 1 << 32)
    finally:
        os.close(fd)
    return drive.Dump(str(path))


def transform(x, y, z, c=1.0, s=0.0):
    return struct.pack('<16f', c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, x, y, z, 0)


def test_bodies_finds_y_rotations_only(tmp_path):
    page = bytearray(PAGE)
    page[0x40:0x80] = transform(600, 11.5, 1050, 0.6, 0.8)
    page[0x100:0x140] = transform(600, 11.5, 1050, 0.6, 0.9)   # not a rotation
    d = sparse(tmp_path / 'd', {0x48c13000: bytes(page)})
    (a, x, y, z), = drive.bodies(d)
    assert (a, x, y, z) == (0x48c13040, 600.0, 11.5, 1050.0)


def test_find_player_takes_the_body_that_went_west_and_back(tmp_path):
    def dump(name, px, other):
        page = bytearray(PAGE)
        page[0x40:0x80] = transform(px, 5, 1050)          # the player's feet
        page[0x80:0xc0] = transform(px, 11.6, 1050)       # its upper body
        page[0x200:0x240] = transform(other, 7, 900)      # someone walking east the whole time
        return sparse(tmp_path / name, {0x48c13000: bytes(page)})
    d0, d1, d2 = dump('0', 610, 700), dump('1', 600, 710), dump('2', 609.5, 720)
    assert drive.find_player(d0, d1, d2) == 0x48c13080 + 0x30


def test_find_layer_and_items(tmp_path):
    base = 0xc7e8b000
    header = struct.pack('<4Q', 0xc7e89d50, base + 0x870 + 0x20 + drive.LAYER_BYTES, 0x2ff, drive.LAYER_BYTES)
    items = np.full((drive.COLS * drive.ROWS, 4), 0, np.uint16)
    items[:, 0] = drive.EMPTY
    c, r = 192, 160                                      # a branch at x 960-970, z 960-970
    for dc, dr in ((0, 0), (1, 0), (0, 1), (1, 1)):
        items[(c + dc) * drive.ROWS + r + dr, 0] = drive.PART
    items[c * drive.ROWS + r, 0] = 0x09C4
    blob = bytes(0x870) + header + items.tobytes()
    d = sparse(tmp_path / 'd', {base: blob})
    layer = drive.find_layer(d)
    assert layer == base + 0x890
    assert drive.items_in(d.read(layer, drive.LAYER_BYTES), 0x09C4) == [(965.0, 965.0)]
