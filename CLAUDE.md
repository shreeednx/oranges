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
.venv/bin/oranges lift [--translate]   # image + function discovery on work/exefs (~20 s; +10 s for the translator check)
```

`oranges lift` writes `work/image/` (`image.bin`, `image.json`, `functions.txt`, `report.json`). `--translate` builds
`third_party/a64-kit/lift/liba64emit.so` with the system `clang++` the first time (gitignored).

## Layout

- `oranges/nup.py`: the `.nup` container reader (format documented in its docstring). `Nup.chunks()` streams a file's
  contents, `read()`/`verify()`/`extract()` check per-file SHA-256s. Names are validated on load, so extraction paths
  are safe by construction.
- `oranges/nso.py`: NSO loader. It builds the memory image (text/ro/data at their offsets, zeroed bss), then parses MOD0
  and `.dynamic`: symbols (count from DT_HASH or DT_GNU_HASH), RELA/JMPREL/RELR relocations, imports and exports. It also
  scans SVC instructions. `link()` reports imports that no loaded module exports.
- `oranges/game.py`: the single supported build (title id, main build id, SDK version, exefs load order).
- `third_party/a64-kit/`: NotRexed's AArch64 lifter, vendored **unchanged** from Longshot (see its `PROVENANCE.md`).
  `hos/nx_image.py` is the image builder (we are rtld: modules from 0x08000000, relocations applied, imports bound).
  `lift/a64_lift.py` is the lifter (`Lifter.discover`, `walk`, `jump_table`, `emit_function`). `lift/a64emit.cpp` +
  `suyu/arm64_to_c.h` is suyu's per-instruction AArch64→C translator. `rt/` is the guest model the lifted C sees. Don't
  edit it. Change its behavior by wrapping its functions from `oranges/`, the way Longshot's `nx2/` does.
- `oranges/kit.py`: puts the kit on `sys.path` and adds DT_RELR to its NSO loader. Import it before any kit module
  (`nso`, `nx_image`, `a64_lift`, `a64emit`); ruff's import sorting is turned off for the files that do.
- `oranges/lift_fixes.py`: Longshot's jump-table and entry fixes to the kit (ported, `install()` wraps `Lifter`).
- `oranges/lift.py`: the lift stage driver (`build_image`, `discover`, `coverage`, `translate_coverage`, `run`).
- `oranges/nso.py` is separate from the kit's `tools/nso.py` (it backs `oranges nso` and its tests). The kit's
  pipeline uses the kit's loader.
- `tests/fakes.py`: builders for synthetic `.nup` packages and NSO modules. Tests must never need the real game.
  `make_nso(code={offset: word})` places instructions, and puts the symbol table before the string table (the kit's
  loader counts symbols as `(strtab - symtab) / 24`).

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
- main's only imports nobody exports are `glslc*` (the NVN online shader compiler). They are weak, so the image binds
  them to 0, as rtld does. A call to one would jump to 0.

## Lift status (a64-kit on this build)

- Discovery: 231k functions (main 178k, sdk 39k, subsdk0 14k). It reaches 99.35% of main's text, 98.8% of subsdk0's
  and 98.0% of sdk's (the rest is padding and data in code). 5,181 jump tables, 11k PLT stubs, 0 BRs left without a
  table.
- Translator: of 1.1M distinct instruction words, 49 are untranslatable. Almost all are data that walks reach (`udf`
  padding, module header words). The real gaps: **PMULL/PMULL2 .8h** (~40 sites, two main routines at 0xbec2xxx and
  0xbef9xxx), **SQSHL vector #imm** (2, sdk), **STNP q** (2). Add them by wrapping `a64emit.translate`, as Longshot's
  `nx2/insn.py` does.
- Next: emit C (`Lifter.write`, with `a64_rtgen`), then compile with `rt/`. After that, the Horizon OS layer.

## Architecture to build toward (Longshot's design)

- **Lift pipeline** (Python): `.nup` → NSOs → find the functions → lift the AArch64 code to C → compile. Longshot's
  equivalent is `nx2/`.
- **Host runtime** (C/C++): a Horizon OS reimplementation (SVCs, IPC services named in `main.npdm`), plus the graphics
  API layer (NVN here).
- **Renderer** (C++): the host records an op stream and a Vulkan backend replays it. It is a separate module from the game.
