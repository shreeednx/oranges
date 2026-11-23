"""Driving a running game through HOS_CONTROL (host/hos_window.c), steered by what is in its memory.

Every island is laid out at random, so input scripts go astray: this reads the player's position and the field's items
from guest memory (HOS_CONTROL's MEM / PEEK) and walks to them. Usage, with the game started with HOS_CONTROL=<fifo>:

    python -m oranges.drive find               locate the player's body and the item layer (walks the player a bit)
    python -m oranges.drive pos                the player's x y z
    python -m oranges.drive walk X Z [run]     walk (or run) there, side-stepping when stuck
    python -m oranges.drive path X Z X Z ... [run]
    python -m oranges.drive items ID           where the items with this id (hex) lie
    python -m oranges.drive collect ID N       pick N of them up (Y)
    python -m oranges.drive shake ID           shake the nearest unshaken tree with this id (A)
    python -m oranges.drive bodies             the characters' and objects' transforms, grouped by place

The FIFO is $HOS_CONTROL; `find` keeps what it found in <fifo>.json for the other commands (the addresses change with
every run). Dumps go to work/mem (1 GB each, sparse).

What was found in the game's memory (3.0.3, the prologue, 2026-11):
- World coordinates: x east, z south, y up; a tile is 10 units, an acre 160. The player's position is the
  translation of its physics bodies: 4x4 row-major float matrices (rows c 0 s 0 / 0 1 0 0 / -s 0 c 0 / x y z 0 for a
  turn about y). Every character has a set of them (the manager, villagers, ...); `bodies` lists them all. Villagers
  that pick a spot in the prologue are moved straight there (round coordinates).
- The field items: two layers of 288 x 192 eight-byte items {u16 id, u16 flags, u32 parameter}, column-major (index
  column * 192 + row), column = x / 5, row = (z - 160) / 5, so a tile is 2 x 2 items. A one-tile item is its id in
  the top-left item and 0xFFFD (part of an item) in the other three; 0xFFFE is nothing. Each layer is preceded by a
  0x20-byte header {ptr, end of the items, 0x2ff, size 0x6c000}. Ids seen: 0x09C4 tree branch, 0x08EE peach,
  0xEACA peach tree (word 3 set once shaken), 0xEA60 / 0xEA69 trees (the parameter is what shaking drops, e.g. a
  branch), 0xEC7F-0xEC85 weeds.
- Picking up (Y) takes the item in the tile the player faces or stands on. Shaking (A) a fruit tree drops three
  fruit around it.
"""
import json
import math
import os
import struct
import sys
import time

import numpy as np

UNIT = 5.0           # an item's size in world units (a tile is 2 x 2 items)
ROWS, COLS = 192, 288
Z0 = 160.0           # the item layer's first row's z
LAYER_BYTES = COLS * ROWS * 8
EMPTY, PART = 0xFFFE, 0xFFFD


# ---- MEM dumps: sparse files with each page at its guest address --------------------------------------------------
class Dump:
    def __init__(self, path):
        self.path = path
        self.mm = np.memmap(path, dtype=np.uint8, mode='r')
        self.extents = []
        fd = os.open(path, os.O_RDONLY)
        try:
            end, off = os.fstat(fd).st_size, 0
            while off < end:
                try:
                    data = os.lseek(fd, off, os.SEEK_DATA)
                except OSError:
                    break
                hole = os.lseek(fd, data, os.SEEK_HOLE)
                self.extents.append((data, hole))
                off = hole
        finally:
            os.close(fd)

    def pages(self):
        s = set()
        for a, b in self.extents:
            s.update(range(a >> 12, b >> 12))
        return s

    def read(self, a, n):
        return bytes(self.mm[a:a + n])


def runs(pages):
    """page numbers -> [(first, end)] of consecutive ones"""
    out = []
    for p in sorted(pages):
        if out and out[-1][1] == p:
            out[-1][1] += 1
        else:
            out.append([p, p + 1])
    return [(a << 12, b << 12) for a, b in out]


def bodies(d):
    """every y-rotation transform in a dump: [(address, x, y, z)]"""
    out = []
    for a, b in runs(d.pages()):
        v = np.frombuffer(d.mm[a:b], np.float32).reshape(-1, 4)
        if len(v) < 4:
            continue
        r0, r1, r2, r3 = v[:-3], v[1:-2], v[2:-1], v[3:]
        with np.errstate(all='ignore'):
            ok = ((r1[:, 0] == 0) & (r1[:, 1] == 1) & (r1[:, 2] == 0) & (r1[:, 3] == 0) &
                  (r0[:, 1] == 0) & (r0[:, 3] == 0) & (r2[:, 1] == 0) & (r2[:, 3] == 0) & (r3[:, 3] == 0) &
                  (np.abs(r0[:, 0] * r0[:, 0] + r0[:, 2] * r0[:, 2] - 1) < 1e-3) &
                  (np.abs(r0[:, 0] - r2[:, 2]) < 1e-3) & (np.abs(r0[:, 2] + r2[:, 0]) < 1e-3) &
                  (r3[:, 0] > 1) & (r3[:, 0] < 3000) & (r3[:, 2] > 1) & (r3[:, 2] < 3000))
        for i in np.nonzero(ok)[0]:
            out.append((a + 16 * int(i), *map(float, r3[i, :3])))
    return out


def find_layer(d):
    """the first item layer's address: after a header whose end pointer is its items' end and whose size is 0x6c000"""
    for a, b in runs(d.pages()):
        v = np.frombuffer(d.mm[a:b], np.uint64)
        if len(v) < 4:
            continue
        idx = np.arange(len(v) - 3, dtype=np.uint64)
        ok = (v[3:] == LAYER_BYTES) & (v[1:-2] == np.uint64(a) + idx * np.uint64(8) + np.uint64(0x20 + LAYER_BYTES))
        hits = np.nonzero(ok)[0]
        if len(hits):
            return a + 8 * int(hits[0]) + 0x20
    return None


def find_player(d0, d1, d2):
    """the player's body: a transform whose x went down by a step (d0 -> d1, walking west) and back up (d1 -> d2)"""
    at1 = {a: (x, y, z) for a, x, y, z in bodies(d1)}
    found = []
    for a, x0, _, _ in bodies(d0):
        if a not in at1:
            continue
        x1 = at1[a][0]
        x2 = struct.unpack('<f', d2.read(a + 0x30, 4))[0]
        step = x1 - x0
        if step < -1 and abs((x2 - x1) + step) < 0.4 * abs(step):
            found.append((a, at1[a][1]))
    if not found:
        return None
    return max(found, key=lambda f: f[1])[0] + 0x30   # (the highest body part: the y the old scripts used)


def items_in(raw, item_id):
    """the world (x, z) centres of the one-tile items with this id in a layer's bytes"""
    ids = np.frombuffer(raw, np.uint16).reshape(-1, 4)[:, 0]
    out = []
    for i in np.nonzero(ids == item_id)[0]:
        c, r = divmod(int(i), ROWS)
        out.append((c * UNIT + UNIT, Z0 + r * UNIT + UNIT))
    return out


# ---- the running game ------------------------------------------------------------------------------------------------
class Game:
    def __init__(self, fifo=None):
        self.fifo = fifo or os.environ.get('HOS_CONTROL') or sys.exit('HOS_CONTROL: the game\'s control FIFO')
        self.state_path = self.fifo + '.json'
        self.state = json.load(open(self.state_path)) if os.path.exists(self.state_path) else {}
        self.mem_dir = os.path.abspath(os.environ.get('ORANGES_MEM', 'work/mem'))

    def send(self, *lines):
        with open(self.fifo, 'w') as f:
            f.write(''.join(line + '\n' for line in lines))

    def peek(self, addr, n):
        out = self.fifo + '.peek'
        if os.path.exists(out):
            os.remove(out)
        self.send('PEEK %x %d %s' % (addr, n, out))
        for _ in range(500):   # (lines run in order: this waits for the ones before it too)
            if os.path.exists(out):
                return open(out, 'rb').read()
            time.sleep(0.02)
        raise RuntimeError('no answer to PEEK (is the game running with HOS_CONTROL=%s?)' % self.fifo)

    def dump(self, name, *before):
        os.makedirs(self.mem_dir, exist_ok=True)
        path = os.path.join(self.mem_dir, name)
        self.send(*before, 'MEM ' + path)
        self.peek(0x8000000, 4)   # (answered once the MEM before it is written)
        return Dump(path)

    def find(self):
        d0 = self.dump('find0')
        d1 = self.dump('find1', '0.3 LSTICK=-1,0', '1')
        d2 = self.dump('find2', '0.3 LSTICK=1,0', '1')
        player, layer = find_player(d0, d1, d2), find_layer(d2)
        if player is None:
            raise RuntimeError('no body moved west and back: is the player free to walk (and not against a wall)?')
        self.state = {'player': player, 'layer': layer}
        json.dump(self.state, open(self.state_path, 'w'))
        return self.state

    def _need(self, key):
        if self.state.get(key) is None:
            raise RuntimeError('run "find" first (no %s address for this run)' % key)
        return self.state[key]

    def pos(self):
        return struct.unpack('<3f', self.peek(self._need('player'), 12))

    def walk(self, x, z, run=False, tol=6.0, steps=120):
        """steer towards (x, z) in short holds; when stuck, side-step (alternating sides, longer each time)"""
        last, stuck, side = None, 0, 1
        for _ in range(steps):
            px, py, pz = self.pos()
            dx, dz = x - px, z - pz
            d = math.hypot(dx, dz)
            if d < tol:
                return True, (px, py, pz)
            ux, uz = dx / d, dz / d
            stuck = 0 if last is None or math.hypot(px - last[0], pz - last[2]) >= 1.0 else stuck + 1
            last = (px, py, pz)
            b = ' B' if run else ''
            if stuck >= 2:
                if stuck > 12:
                    return False, (px, py, pz)
                if stuck % 4 == 2:
                    side = -side
                t = 0.3 * (1 + stuck // 4)
                self.send('%.2f LSTICK=%.3f,%.3f%s' % (t, -uz * side, -ux * side, b))   # (stick y is -z)
            else:
                t = min(0.5, max(0.1, d / (60 if run else 30)))
                self.send('%.2f LSTICK=%.3f,%.3f%s' % (t, ux, -uz, b))
            time.sleep(t + 0.05)
        return False, self.pos()

    def items(self, item_id):
        return items_in(self.peek(self._need('layer'), LAYER_BYTES), item_id)

    def collect(self, item_id, want, run=False):
        """pick up `want` items with this id, nearest first: from the tile south of each (facing north), then on it"""
        got, skip = 0, set()
        while got < want:
            px, _, pz = self.pos()
            todo = [p for p in self.items(item_id) if p not in skip]
            if not todo:
                print('none left')
                break
            tx, tz = min(todo, key=lambda p: math.hypot(p[0] - px, p[1] - pz))
            ok, p = self.walk(tx, tz + 9, run, tol=2.5)
            if not ok:
                print('cannot reach %.0f %.0f' % (tx, tz))
                skip.add((tx, tz))
                continue
            self.send('0.1 LSTICK=0,1', '0.3', '0.2 Y', '1.4')
            time.sleep(2.1)
            if (tx, tz) in self.items(item_id):
                self.walk(tx, tz + 2, False, tol=2.0, steps=20)
                self.send('0.2 Y', '1.4')
                time.sleep(1.7)
            if (tx, tz) in self.items(item_id):
                print('not picked at %.0f %.0f (from %.1f %.1f)' % (tx, tz, p[0], p[2]))
                skip.add((tx, tz))
                continue
            got += 1
            print('picked %d at %.0f %.0f' % (got, tx, tz))
        return got

    def shake(self, tree_id):
        """shake the nearest tree with this id whose word 3 is still 0 (not shaken today), from the south"""
        raw = self.peek(self._need('layer'), LAYER_BYTES)
        v = np.frombuffer(raw, np.uint16).reshape(-1, 4)
        px, _, pz = self.pos()
        trees = []
        for i in np.nonzero((v[:, 0] == tree_id) & (v[:, 3] == 0))[0]:
            c, r = divmod(int(i), ROWS)
            x, z = c * UNIT + UNIT, Z0 + r * UNIT + UNIT
            trees.append((math.hypot(x - px, z - pz), x, z))
        if not trees:
            return None
        _, x, z = min(trees)
        ok, _ = self.walk(x, z + 10, True, tol=2.5)
        self.send('0.1 LSTICK=0,1', '0.3', '0.2 A', '3')
        time.sleep(3.7)
        return (x, z) if ok else None


def main(argv=None):
    a = list(sys.argv[1:] if argv is None else argv)
    if not a:
        print(__doc__)
        return 2
    g, cmd = Game(), a.pop(0)
    run = bool(a) and a[-1] == 'run'
    if run:
        a.pop()
    if cmd == 'find':
        s = g.find()
        print('player %#x, item layer %s' % (s['player'], '%#x' % s['layer'] if s['layer'] else 'not found'))
    elif cmd == 'pos':
        print('%.1f %.1f %.1f' % g.pos())
    elif cmd in ('walk', 'path'):
        for i in range(0, len(a), 2):
            ok, p = g.walk(float(a[i]), float(a[i + 1]), run)
            print('arrived' if ok else 'STUCK', '%.1f %.1f %.1f' % p)
            if not ok:
                return 1
    elif cmd == 'items':
        for x, z in g.items(int(a[0], 16)):
            print('%.0f %.0f' % (x, z))
    elif cmd == 'collect':
        g.collect(int(a[0], 16), int(a[1]), run)
    elif cmd == 'shake':
        t = g.shake(int(a[0], 16))
        print('shook %.0f %.0f' % t if t else 'no tree shaken')
    elif cmd == 'bodies':
        d = g.dump('bodies')
        groups = {}
        for b in bodies(d):
            groups.setdefault((round(b[1] / 8), round(b[3] / 8)), []).append(b)
        for _, grp in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            print('%4d  x %7.1f y %6.1f z %7.1f   first %#x' % (len(grp), grp[0][1], grp[0][2], grp[0][3], grp[0][0]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
