# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project goal

A static recompilation of **the game** (Switch, title ID `01006F8002326000`) into a native Windows/Linux program,
modeled on **Longshot** (https://git.magicbean.dev/MagicBeanDev/Longshot.git), which does this for a Switch 2 Zelda
title. The goal is to have no emulator at runtime: the game's AArch64 code is lifted to C and compiled natively,
graphics run on Vulkan, and the console's system software (Horizon OS services and syscalls) is reimplemented.

## Commands

The Python environment is `.venv/` (Python ≥3.14, because `compression.zstd` is in the stdlib from 3.14).

```sh
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'   # setup
.venv/bin/pytest -q                                          # all tests (game-free, synthetic fixtures)
.venv/bin/pytest tests/test_nup.py::test_extract_writes_checks_and_skips   # one test
.venv/bin/ruff check .                                       # lint (line length 120; printf-style % is the house style)

.venv/bin/oranges info    game.nup                              # titles, metadata, exefs, supported-build check (<1 s)
.venv/bin/oranges check   game.nup                              # exit 0 only for the supported build
.venv/bin/oranges verify  game.nup                              # SHA-256 every file (~20 s)
.venv/bin/oranges extract game.nup work [--only exefs|romfs]
.venv/bin/oranges nso     game.nup [main ...] [-v]              # segments, symbols, relocs, SVCs, cross-module link
.venv/bin/oranges nso     work/exefs/main                       # same for an extracted NSO
```

## Layout

- `oranges/nup.py`: the `.nup` container reader (format documented in its docstring). `Nup.chunks()` streams a file's
  contents, `read()`/`verify()`/`extract()` check per-file SHA-256s. Names are validated on load, so extraction paths
  are safe by construction.
- `oranges/nso.py`: NSO loader. It builds the memory image (text/ro/data at their offsets, zeroed bss), then parses MOD0
  and `.dynamic`: symbols (count from DT_HASH or DT_GNU_HASH), RELA/JMPREL/RELR relocations, imports and exports. It also
  scans SVC instructions. `link()` reports imports that no loaded module exports.
- `oranges/game.py`: the single supported build (title id, main build id, SDK version, exefs load order).
- `tests/fakes.py`: builders for synthetic `.nup` packages and NSO modules. Tests must never need the real game.

## Game data rules

- `game.nup` (10 GB, repo root) is the user's own legal backup. Read it in place only. **Never commit, upload
  or modify it**, or anything extracted or built from it (exefs/romfs, NSOs, lifted C, saves). `.gitignore` covers
  `*.nup`, `work/` and `build/`. `work/` holds local extractions (`work/exefs/` is already there).

## License

GPL-3.0-or-later, the same as Longshot, so Longshot code can be reused. When copying from Longshot, keep its copyright
and attribution notices, and note the source file in a comment.

## The supported build (from the reference dump)

- Title `01006F8002326000`, main module id `ff1d1c05670db6021c85b624a710b963`. The `.nup` metadata label is
  `output_update`, i.e. the base game with its update applied. Main contains a lone string `3.0.3`, probably the game
  version (unconfirmed). Like Longshot, support exactly one build and refuse others (`game.check`).
- Nintendo SDK 20.5.4, `spec: NX`. This is a **Switch 1** program using **NVN (v1)**, not Longshot's NVN2. Longshot's
  graphics layer won't carry over directly.
- exefs: `rtld`, `main` (66 MB text, stripped: only 1068 dynamic symbols, so function discovery has to be done from
  code), `subsdk0` (multimedia/movie), `sdk` (nnSdk, 23k exports, where all the SVCs live), `main.npdm`.
- Middleware in main: NintendoWare (G3d, Ui2d, Font, Atk, Vfx2), Havok Animation/Cloth 2019.1, NEX/Pia 4.6.11 (online),
  libcurl.
- main's only imports nobody exports are `glslc*` (the NVN online shader compiler). The host must stub or provide them.

## Architecture to build toward (Longshot's design)

- **Lift pipeline** (Python): `.nup` → NSOs → find the functions → lift the AArch64 code to C → compile. Longshot's
  equivalent is `nx2/`.
- **Host runtime** (C/C++): a Horizon OS reimplementation (SVCs, IPC services named in `main.npdm`), plus the graphics
  API layer (NVN here).
- **Renderer** (C++): the host records an op stream and a Vulkan backend replays it. It is a separate module from the game.
