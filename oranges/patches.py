"""(oranges) Fixes to the game's own code, applied to the image before the lift (lift.build_image).

Only for a fault in the game that the console's timing hides and that no faithful OS behaviour can hide here, or (asked
for by the user, 2026-11-24: "faster loading") a wait of the game's that only costs time, changing nothing the player
sees. Each patch names the supported build's original words, and apply() refuses an image where they differ. It
changes the image (image.bin too), so the lifted C and the data the game reads agree.

The Resource Memory crash (about 1 boot in 10 with several processes running at once)
-------------------------------------------------------------------------------------
The resource threads ("Resource Memory", "Resource Loading", "ResLoad1/2") share a message handler,
main+0x6d6514 (vtable +0x88). Until its object's +0x18c is set, a message other than 4 makes it look at a singleton
(main+0x5a05378). If that exists, it reads ((singleton+8)+8) and calls through it:

    086d6590  adrp x8, 0xda05000
    086d6594  add  x8, x8, #0x378
    086d6598  ldr  x8, [x8]              the singleton
    086d659c  cbz  x8, 0x86d65bc         none yet: try again with the next message
    086d65a0  ldr  x8, [x8, #8]          its +8: 0 until its init has run
    086d65a4  ldr  x0, [x8, #8]          (guest address 8 when +8 is 0)

The Prepare thread creates the singleton (f_0bdfc054, called at main+0x6bf828) and publishes it before its init
(vtable +0x10, f_0bdfc2b8) stores +8, which comes after creating a 32 MB heap: about 10 ms of guest work. Meanwhile the
loaders send the handler message 1 (wake up) several times a millisecond, since the host's file reads are instant, so
one lands in that window often. Other threads are on other cores, so no scheduling closes the window. The game's
other reader of this singleton (main+0x5e01aa8) checks +8 for 0; the patch makes the handler do the same, as if the
singleton did not exist yet. The `add` folds into the load's offset, which frees the slot for the check:

    086d6594  ldr  x8, [x8, #0x378]
    086d6598  cbz  x8, 0x86d65bc
    086d659c  ldr  x8, [x8, #8]
    086d65a0  cbz  x8, 0x86d65bc         (new) not initialized yet: try again with the next message

The play-report poll (7 s of every boot, 0.5 s at each later save step)
-----------------------------------------------------------------------
At each step of loading (and saving) the save, SaveProc queues a play report for the PlayReporter thread, then waits
for it to be sent: 0xbe26e50(reporter, 5000 ms) returns once bit 0 of the queue's +0x120 is clear, and sleeps between
checks. The reporter takes the queue at its next wake-up (~30 ms here: a frame), but the first check comes right after
the push, so every step sleeps one whole period, and the period is half a second of ticks:

    0be26ec0  ldr  x24, [x9]             the tick frequency
    0be26ec4  cmp  x24, #0
    0be26ec8  cinc x9, x24, lt
    0be26ecc  asr  x21, x9, #1           the sleep: frequency / 2 ticks (ConvertToTimeSpan, SleepThread at 0be26f00)

The boot does 14 such steps (5.8-13.6 s with the day-1 save, the process otherwise idle). The console runs the same
code, so it waits the same. The patch polls every 1/256 s (3.9 ms) instead; the 5 s timeout counts ticks, unchanged:

    0be26ecc  asr  x21, x9, #8
"""
import struct
from dataclasses import dataclass

from . import game


@dataclass(frozen=True)
class Patch:
    name: str
    main_offset: int              # in main (the guest address less 0x08000000)
    original: tuple[int, ...]     # the supported build's words there
    words: tuple[int, ...]        # what goes there instead


PATCHES = (
    Patch("resource handler: wait for the singleton's init", 0x6D6594,
          (0x910DE108, 0xF9400108, 0xB4000108, 0xF9400508),
          (0xF941BD08, 0xB4000128, 0xF9400508, 0xB40000E8)),
    Patch("play-report poll: every 3.9 ms, not 0.5 s", 0x3E26ECC,
          (0x9341FD35,),
          (0x9348FD35,)),
)


class PatchError(Exception):
    pass


def apply(img, patches=PATCHES, build_id=game.MAIN_BUILD_ID) -> list[str]:
    """Write the patches into a kit NxImage whose main is the supported build (other images are left alone). Returns
    the names of the patches applied."""
    main = next((m for m in img.modules if m.name == "main"), None)
    if main is None or main.build_id != build_id:
        return []
    done = []
    for p in patches:
        if len(p.words) != len(p.original):
            raise PatchError("%s: %d words for %d" % (p.name, len(p.words), len(p.original)))
        at = main.base + p.main_offset - (img.end - len(img.mem))   # (an offset into img.mem)
        n = len(p.original)
        found = struct.unpack_from("<%dI" % n, img.mem, at)
        if found != p.original:
            raise PatchError("%s: main+%#x holds %s, not the supported build's %s" % (
                p.name, p.main_offset, " ".join("%08x" % w for w in found),
                " ".join("%08x" % w for w in p.original)))
        struct.pack_into("<%dI" % n, img.mem, at, *p.words)
        done.append(p.name)
    return done
