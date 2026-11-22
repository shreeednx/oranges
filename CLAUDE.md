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
.venv/bin/oranges lift [--translate] [--write]   # image + discovery (~20 s); --write emits the C (+40 s, 1.4 GB)
.venv/bin/oranges build                           # compile work/gen + rt + host/, link work/build/oranges
./work/build/oranges [image.bin]                  # boot (default image: work/image/image.bin); HOS_TRACE=1 logs every SVC
.venv/bin/python -m oranges.insncheck --game      # every instruction form the game uses vs Unicorn (~35 s)
.venv/bin/python -m oranges.insncheck --words 4e20e000,ac000400   # specific encodings vs Unicorn (game-free)
```

`oranges lift` writes `work/image/` (`image.bin`, `image.json`, `functions.txt`, `report.json`), and with `--write`
also `work/gen/` (`code_NNN.c`, `a64_table.c`, `a64_hooks.c`, `a64_fpx.h`, `a64_suyu.h/.c`). The first translator use
builds `third_party/a64-kit/lift/liba64emit.so` with the system `clang++` (gitignored). `oranges build` caches objects
in `work/build/obj` (one `.key` stamp beside each object) and runs as many compiles as fit in memory, about 2.5 GB each
(`ORANGES_JOBS`, `ORANGES_MARCH` (default x86-64-v3) and `ORANGES_OPT` (default O1) override).

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
- `oranges/spin.py`: (oranges) a compiler barrier after each loop-invariant load of a small loop (`Lifter.after`), so
  clang can't hoist a plain LDR out of a spin loop (it made the game's spinlock wait `jmp .`). `install()` wraps
  `Lifter.emit_function`; `report.json` `emit.spin_barriers` counts them (~159k).
- `oranges/insn.py`: Longshot's `nx2/insn.py` (forms suyu lacks: LSE, LDAPR, SQSHL #imm, SDIV overflow, ...), plus
  oranges' PMULL/PMULL2 and LDNP/STNP Q, marked `(oranges)`. `install()` wraps `a64_lift.classify` and
  `a64emit.translate`. A new form goes in `emit()` and gets encodings in `tests/test_insn.py`, checked against Unicorn.
- `oranges/insncheck.py`: drives the kit's `tests/insn_test.py` (Unicorn vs lifted C) on Linux. It wraps rather than
  edits: the harness's `VirtualAlloc` becomes `mmap`, the CPU model is Unicorn's "max", and `words=` tests chosen
  encodings without the game.
- `oranges/lift.py`: the lift stage driver (`install`, `build_image`, `discover`, `coverage`, `translate_coverage`,
  `write_c`, `run`). `install()` applies `lift_fixes` and `insn` and must run before discovery and emission.
- `oranges/build.py`: compiles and links. Chunk flags follow Longshot's (`-O1 -ffp-contract=off -fno-strict-aliasing
  -mcx16 -ffunction-sections -mllvm -rotation-max-header-size=0`).
- `oranges/hle.py`: the lift-time half of the OS layer (from Longshot's `nx2/standalone.py`). `hle_set` hooks every
  `HLE(sym)` / `void imp_sym(Ctx` that `host/*.c` defines, at the import and at the sdk's definition, and stubs every
  OS-facing `nn::<service ns>::f` the game imports (`OS_NAMESPACES`). It writes `gen/a64_stubs.c` (stubs log their
  first call and return 0) and `gen/hos_image.c` (boot constants: segments, DT_INITs, SDK boot entry points, main
  thread from `main.npdm`). `oranges lift --write` applies it. A new `HLE(...)` in host/ needs a lift again.
- `host/` (C): the Horizon OS layer, modeled on Longshot's `host/` but with `nn::os` run lifted over real SVCs.
  `hos.h` has the API and the guest address map (4 GB window: system region, image at 0x08000000, heap, alias and
  stack regions). `hos_main.c` loads `image.bin` and boots: `_init_libc0`, `nnosInitialize`, `_init_libc1`,
  `nninitStartup`, `_init_libc2`, each `DT_INIT`, `nnMain` (rtld is not run). `hos_mem.c` keeps the mapping record
  for QueryMemory, the system region and the heap. `hos_svc.c` is the SVCs (memory, info, debugging). `hos_ipc.c` is IPC:
  sessions, HIPC/TIPC/CMIF parsing (domains too), `sm:` (GetServiceHandle gives a session for any name), the CMIF
  control commands, and the emulated services (`services[]`, each a `HosIface`; `olsc:u` is in `nn_system.c`).
  Requests to services nobody emulates get a success with zeroed outputs. `hos_log.c` has logging, traps and stub notes.
  `hos_setjmp.c` holds stubs. `hos_kernel.c` has threads (each on a host pthread), handles, mutexes, condition
  variables, address arbiters, events and WaitSynchronization, under one kernel lock. `hos_hostcall.c` provides guest
  addresses that are host functions (through `a64_lookup_host`), for function pointers handed to the game.
  `nn_fs.c` is `nn::fs` (RomFS read-only from `work/romfs`, device save data in `work/user/save/device`). `nvn.c` has
  the nv:: driver setup HLE. `nvn_native.c` is NVN itself, ported from Longshot's NVN1-descended layer: objects live in
  the game's NVN object memory (`nvn/rx_nvn.h` layouts, checked against `nvn/nvn_api.h` sizes), GPU work is recorded
  and handed to a render back end at submit. `rx_null.c` is the back end for now: it applies the CPU-visible effects
  (events, counters, buffer copies/updates) and draws nothing. The proc table `nvn/nvn_procs.inc` lists every `NV()`
  (`tests/test_nvn.py` keeps them in sync: regenerate it with
  `grep -oP '^NV\(\K\w+' host/nvn_native.c | sed 's/.*/    E(&),/'` under its comment line). `nvn_layout.c` is the
  Tegra block-linear texture layout (deko3d-derived). `nn_audio.c` is the audio renderer at the API level (Longshot's,
  no sound device yet: `HOS_AUDIO_DUMP=<wav>`), `nn_system.c` nn::time / oe / account / vi / pl / ngc,
  `nn_hid.c` input (neutral until there is a window). Device limits are the game's own driver's (API 55.15), read by
  running the sdk's `nvnDeviceGetInteger` under Unicorn.
  IPC requests to services nobody emulates are answered empty (success, zeroed outputs) and logged once per service
  and command with a backtrace. Native
  crashes are logged with the guest registers and backtrace (`hos_crash_init`). The kit's `rt/a64_rt.c` weak hooks
  (`a64_svc`, `a64_on_trap`, `a64_lookup_host`) are overridden here.
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
- Translator (with `oranges.insn`): every instruction translates. The 10 untranslated words left (5,143 sites) are data
  that walks reach (`udf` padding, module header words), and they trap, as they should.
- Unicorn check: 596 forms, 2,869 encodings, 34,428 random states, 0 differences (registers, NZCV, FPSR, V regs,
  memory).
- Emission: 19.6M instructions (shared tails included) in 57 files, 1.4 GB of C. 25,789 of 43,097 BRs stay indirect
  (`a64_ijump`, mostly virtual tail calls).
- Build: one 25 MB chunk compiles in ~25 s at -O1 with a 1.4 GB peak (clang 23).
- Boot (`./work/build/oranges`): `_init_libc0` … `nnMain` run natively on host threads (~55 of them). The game sets up
  NVN, audio (`nn_audio.c`: renderer, first voice and wave buffers), fonts (a host TTF for the system's shared fonts:
  `HOS_SHARED_FONT`), input (`nn_hid.c`: a connected pad at rest), presents its first frame, runs its frame loop's
  job scheduler, mounts the device save, opens `olsc:u` through `sm:` (its own client, linked into main: served by
  `hos_ipc.c`), and runs its frame loop at 30 fps (90 s tested, nothing drawn, no input). To sample every thread: `(sleep 60; pkill -INT -x oranges) & gdb
  -batch -ex run -ex "thread apply all bt 25" ./work/build/oranges` (attaching is blocked by ptrace_scope).
- Two things the frame loop's job scheduler needs (the stall after the first frame): the kernel reports each thread's
  core (`GetCurrentProcessorNumber` from its SetThreadCoreMask ideal core / affinity), because a thread pinned to one
  core takes `core + 1` as its TLS index (`f_0b7e2374`) and the scheduler's per-thread slots are indexed by it
  (`f_086410b0`, barriers at `+0x298`); and spin loops keep reading memory (`oranges/spin.py`).
- `sm:` speaks TIPC on this SDK (request type 16 + command id, the result as the reply's first data word), and the
  SDK's clients read replies at fixed offsets without checking the header: a reply must have exactly the layout a
  real server sends.
- Next: the remaining stubs (`nn::aoc`,
  `nn::nifm`, `nn::friends`, `nn::bcat`, `nn::nfp`), a window with real input, a sound device for `nn_audio.c`, and the
  Vulkan back end in place of `rx_null.c`.
- Memory: svcGetInfo reports the console's application pool (3285 MB); the game's heap is 3174 MB and its framework
  carves a 2015 MB AppStaticHeap from it, so the total matters.
- Host HLE helpers: `hos_guest_call` calls lifted functions from an HLE (e.g. SDK exports found with
  `hos_symbol_lookup`); `hos_system_event_init` binds a game `SystemEventType` to a kernel event the host signals with
  `hos_kernel_event_signal` (audio renderer, vsync, account async contexts).
- For each wall, `hos_backtrace` (or gdb: lifted frames are `f_<guest address>`, `$rdi` is the guest `Ctx *`) shows the
  `nn::` entry point. Hardware watchpoints on `0x100000000000 + <guest address>` find who writes guest memory.
  host/ builds with `-g`, so gdb reads the kernel's state: `p *hos_current` (a thread's guest registers; `x29` walks
  the guest frame chain), `p 'hos_kernel.c'::threads[0]->wait` (what a thread waits for). A thread at 100% CPU in a
  lifted function: disassemble it for a `jmp` to itself (a hoisted load).

## Architecture to build toward (Longshot's design)

- **Lift pipeline** (Python): `.nup` → NSOs → find the functions → lift the AArch64 code to C → compile. Longshot's
  equivalent is `nx2/`.
- **Host runtime** (C/C++): a Horizon OS reimplementation (SVCs, IPC services named in `main.npdm`), plus the graphics
  API layer (NVN here).
- **Renderer** (C++): the host records an op stream and a Vulkan backend replays it. It is a separate module from the game.
