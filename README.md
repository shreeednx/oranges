# oranges

A static recompilation of one Nintendo Switch game (title ID `01006F8002326000`, version 3.0.3) into a native Linux or
Windows program. No emulator runs while you play: the game's AArch64 code is translated to C and compiled for your PC,
graphics go through Vulkan, and the parts of the console's system software the game needs are reimplemented.

oranges is modeled on [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot).

> [!WARNING]
> **This project is fully vibecoded.** Its code and documentation (this README included) were written by AI coding
> agents. Don't expect it to have had human code review. It runs a large amount of generated native code on your
> machine, so read what you are about to run, keep backups of anything you care about, and use it at your own risk.
> It comes with no warranty (see [LICENSE](LICENSE)). The code under `third_party/` comes from other projects and
> keeps their authorship.

**oranges contains nothing from the game.** You need your own legally made copy. The code and data of the game come
only from that copy, on your machine. Don't share it, or anything extracted or built from it (including the program
oranges builds). oranges is not affiliated with or endorsed by Nintendo or the game's developers.

- [Status](#status)
- [What you need to build a port of the game](#what-you-need-to-build-a-port-of-the-game)
- [What you need to build oranges from source](#what-you-need-to-build-oranges-from-source)
- [Usage: the GUI](#usage-the-gui)
- [Usage: the command line](#usage-the-command-line)
- [Playing](#playing)
- [Patches to the game](#patches-to-the-game)
- [Troubleshooting](#troubleshooting)
- [Development](#development)
- [License](#license)

## Status

Early, work in progress. [TODO.md](TODO.md) has the details.

| | |
| --- | --- |
| Linux | Boots and plays at the game's 30 fps, with picture, sound, movie and saves. Tested through the prologue to the first save and loading it again. Later parts of the game are untested. |
| Windows | Cross-compiled on Linux and tested under wine only. **Not yet run on real Windows.** |
| Online and wireless play | Not supported: the game sees an offline console. |
| Screenshots | Not supported yet. |
| Downloadable releases | **None are published yet.** For now you build oranges from source (below). |
| GPUs | Developed on one NVIDIA card (RTX 5080). Others are untested. |

## What you need to build a port of the game

These apply however you run the build, from a release or from source.

### Your copy of the game

- Title ID `01006F8002326000`, **version 3.0.3** (the base game with that update applied).
- As a single `.nup` file (about 10 GB): a decrypted package holding the title's `exefs/` and `romfs/`. The format is
  described in [oranges/nup.py](oranges/nup.py). Other formats (`.nsp`, `.xci`, ...) are not read.
- Exactly one build is supported (main module ID `ff1d1c05670db6021c85b624a710b963`). Anything else is refused.
  `oranges check game.nup` tells you whether yours is the one.

### Your computer

| | Needed |
| --- | --- |
| CPU | x86-64 with AVX2 and the rest of the `x86-64-v3` level, plus AES-NI. |
| GPU | Vulkan 1.3 with `VK_EXT_shader_object`, `VK_KHR_push_descriptor` and `VK_EXT_external_memory_host`, and a current driver. |
| Memory | About 2.5 GB per parallel compile job; fewer jobs need less memory and take longer. Running the game takes several GB (it is given the console's 3.2 GB pool). |
| Disk | About 12.5 GB free while building: 10 GB of game files, 1.4 GB of generated C, 0.8 GB of objects, plus the program. About 2.2 GB of that can be deleted afterwards. Your `.nup` is extra. |
| Time | A few minutes to tens of minutes, depending on the CPU and the number of jobs (about 6 minutes on the development machine). |
| Font | One TrueType font for the game's system text: Noto Sans, DejaVu Sans or Liberation Sans on Linux, Arial on Windows. Any other: set `HOS_SHARED_FONT=/path/to/font.ttf`. |

### With a release

A release bundles Python, the compiler and the libraries, so there is nothing else to install. On Linux it still
expects what a desktop distribution already has: glibc 2.35 or newer, zlib, libstdc++, and your GPU driver's Vulkan
loader (`libvulkan.so.1`). No releases are published yet; you can [make one yourself](#making-a-release).

## What you need to build oranges from source

Building from a checkout uses your system's compiler and libraries.

### Linux

- Python **3.14 or newer**
- clang (with `clang++`) and lld. Development uses clang 23.
- pkg-config
- A C++20 standard library that has `<format>` (for example libstdc++ from GCC 13 or newer)
- Development files for:
  - SDL3 (window, input, sound, and the setup GUI)
  - the Vulkan loader
  - libopus (optional: the console's hardware audio decoder)
  - FFmpeg's libavcodec and libavutil (optional: the movies)
- A TrueType font (see the table above)

Without SDL3 or the Vulkan loader the program still builds, but it has no renderer and draws nothing.

On Arch-based distributions:

```bash
sudo pacman -S --needed git python clang lld pkgconf sdl3 vulkan-icd-loader opus ffmpeg ttf-dejavu
```

Other distributions have the same packages under their own names. You also need your GPU's Vulkan driver.

### Get the source and set up Python

```bash
git clone https://github.com/shreeednx/oranges.git
```

```bash
cd oranges
```

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install -e .
```

Use `.venv/bin/pip install -e '.[dev]'` instead if you want to run the tests.

### The Windows program, cross-compiled on Linux

You also need [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (in `/opt/llvm-mingw`, or point `ORANGES_MINGW` at
it), plus `make` and a host C compiler. Then fetch the Windows libraries (SDL3, the Vulkan loader, opus, and an FFmpeg
built from source) into `~/.local/opt/msys2-clang64`:

```bash
.venv/bin/python -m oranges.mingw
```

Building on Windows itself is meant to work (`python -m oranges.wintools` fetches a Python and llvm-mingw for it), but
it has only been tried under wine. See [CLAUDE.md](CLAUDE.md) for those commands.

## Usage: the GUI

The setup program takes you from your `.nup` to a game you can start.

### Start it

From a release, run `oranges` (`oranges.exe` on Windows) in the release folder.

From a source checkout, build it once and run it:

```bash
.venv/bin/python -m oranges.gui
```

```bash
./work/gui/oranges
```

It finds the checkout and its `.venv` by itself.

### The five steps

1. **Game.** Pick your `.nup`. The ones found in your usual folders are listed, or use **Browse...**. The page says
   whether it is the supported version, and if not, why.
2. **Folder.** Choose where to build. It shows the free space there against what the build needs. The port stays in
   this folder; saves and settings go to your user folder.
3. **Options.** The number of build jobs (the default fits your memory), whether to delete the build's own files when
   done (2.2 GB; keeping them makes rebuilds faster), and the game's console mode: Docked (1920x1080) or Handheld
   (1280x720, lighter on the GPU).
4. **Build.** Shows each stage with its progress, time left and the log. **Cancel** stops it, and **Continue** picks
   up where it stopped. If it runs out of memory, lower the build jobs under Options and build again.
5. **Play.** Start the game, add a shortcut to your applications or Start menu, open the save or build folder, or
   delete the leftover build files.

The setup program can also be started on a given step:
`oranges [--nup FILE] [--dir FOLDER] [--page game|folder|options|build|play] [--build]`.

## Usage: the command line

All commands below are run from the checkout. Replace `game.nup` with your file.

### Build the port in one command

Check your copy first:

```bash
.venv/bin/oranges check game.nup
```

Then build (check, extract, lift, compile, link):

```bash
.venv/bin/oranges make game.nup --dir work
```

And play:

```bash
./work/build/oranges
```

| `make` option | Meaning |
| --- | --- |
| `--dir FOLDER` | Where to build (default: `work`). The program is `FOLDER/build/oranges`. |
| `--jobs N` | Parallel compiles (default: what fits in memory). |
| `--clean` | Delete the generated C and the objects once the program is linked. |
| `--progress json` | One JSON event per line, as the GUI reads them; closing stdin cancels. |

A run that was interrupted resumes: extracted files are kept, an up-to-date lift is skipped and compiled objects are
reused. `FOLDER/make.log` keeps the output of every run. Exit codes: `0` built, `1` error, `3` unsupported build,
`4` not enough disk space, `5` out of memory, `130` cancelled.

After updating oranges, run the same `make` command again to rebuild the port.

### Step by step

The same pipeline in separate stages, for development:

```bash
.venv/bin/oranges extract game.nup work
```

```bash
.venv/bin/oranges lift --write
```

```bash
.venv/bin/oranges build
```

For the Windows program (see the requirements above), the build step is:

```bash
.venv/bin/oranges build --target windows
```

It writes `work/build-windows/oranges.exe` with its DLLs and their licences beside it.

### Looking inside a package

| Command | What it does |
| --- | --- |
| `oranges info game.nup` | Titles, metadata, executables, and whether the build is supported |
| `oranges check game.nup` | Exit status 0 only for the supported build |
| `oranges verify game.nup` | Check the SHA-256 of every file in the package |
| `oranges extract game.nup DIR [--only exefs\|romfs]` | Write the package's files to a folder |
| `oranges nso game.nup [MODULE ...] [-v]` | Segments, symbols, relocations and system calls of the executables |

Run these as `.venv/bin/oranges ...`. `.venv/bin/oranges <command> --help` lists every option. Your `.nup` is only ever read.

## Playing

The window opens at 1280x720. **F1** (a gamepad's Home button, or Select + Start) opens the settings menu, and **F11**
toggles full screen.

### Default controls

| Switch | Keyboard | Gamepad |
| --- | --- | --- |
| Left stick | W A S D | Left stick |
| Right stick | I J K L | Right stick |
| D-pad | Arrow keys | D-pad |
| A | Space or Enter | Right face button |
| B | Left Shift or Backspace | Bottom face button |
| X | E | Top face button |
| Y | F | Left face button |
| L / R | Q / R | Shoulder buttons |
| ZL / ZR | Z / C | Triggers |
| + / - | Escape / Tab | Start / Select |
| Stick clicks (left / right) | X / N | Stick clicks |

Face buttons are mapped by position, as on a Nintendo controller. The left mouse button acts as a finger on the touch
screen. Text (names and so on) is entered with the game's own on-screen keyboard, by controller, key bindings or mouse;
the game does not see a USB keyboard.

### Settings menu

- **Graphics:** console mode (docked or handheld, switchable while playing), full screen, V-Sync, scaling (including
  AMD FSR 1) and its sharpness.
- **Controls:** which controller to use, every key and gamepad binding, stick dead zone, vibration, and mouse as touch
  screen.

### Saves and settings

| | Linux | Windows |
| --- | --- | --- |
| Saves | `~/.local/share/oranges/saves/01006F8002326000/device` (under `$XDG_DATA_HOME` if set) | `%APPDATA%\oranges\saves\01006F8002326000\device` |
| Settings | `~/.config/oranges/oranges_options.ini` (under `$XDG_CONFIG_HOME` if set) | `%APPDATA%\oranges\oranges_options.ini` |

`ORANGES_USER=/some/folder` uses another user folder for saves, and `HOS_OPTIONS_FILE=/some/file.ini` another settings
file. Rebuilding the port touches neither. Back up your saves.

### Started from Steam

When Steam starts the program, it opens an X11 window (XWayland) so that Steam Input applies the game's controller
layout. Set `HOS_WAYLAND=1` to keep a native Wayland window.

## Patches to the game

oranges changes the game's own code in two places. Neither changes gameplay or anything you see, apart from loading
faster. The patches are applied to the memory image while the port is built, before the code is translated to C. Your
`.nup` and the extracted executables are not modified.

| Patch | Where (in `main`) | What it changes | Why |
| --- | --- | --- | --- |
| Resource handler waits for its singleton | `+0x6d6594`, 4 instructions | The resource threads' message handler now also treats an object that exists but isn't initialized yet as not there, and tries again at the next message. | A race in the game. The console's timing hides it, but here file reads are near instant and it crashed about 1 boot in 10 when several copies ran at once. |
| Faster play-report poll | `+0x3e26ecc`, 1 instruction | While loading or saving, the game checks every 3.9 ms (instead of every 0.5 s) whether its queued play report was taken. The 5 s timeout is unchanged. | A pure wait: about 7 s of every boot, and half a second at each later save step. |

Both are on in every build; there is no switch to turn them off. Each one records the original instructions of the
supported build, and the build stops with an error if they don't match.
[oranges/patches.py](oranges/patches.py) has the full analysis. A new or changed patch takes effect after the port is
lifted and built again.

A patch is only added to fix a fault in the game that the console's timing hides and that no faithful behaviour of the
reimplemented system can hide, or to remove a wait that only costs time.

### Other differences from the console

These are not patches to the game's logic, but the port doesn't run the original code unchanged either:

- **System functions are replaced.** Many functions of the console's SDK that talk to the system (graphics, audio,
  input, files, online services, ...) call oranges' own implementations in `host/`. The ones oranges doesn't
  implement are stubs that report success and do nothing. This includes the movie decoder.
- **Spin loops keep reading memory.** The translation adds compiler barriers
  ([oranges/spin.py](oranges/spin.py)) so the C compiler doesn't turn a loop that waits for another thread into an
  endless one. They add no instructions.
- **Online features see an offline console**, and the system's fonts are replaced by a font from your computer.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| "not the supported build" | You need version 3.0.3 with exactly the supported build. Run `oranges info game.nup` to see what the package holds. |
| The compiler is killed, or the machine runs out of memory | Lower the jobs: `--jobs 2`, or the slider under Options. |
| Not enough disk space | Pick another folder. The build needs about 12.5 GB. |
| Nothing is drawn, only a status page | The Vulkan back end is missing or did not start. Check the log for lines starting `vk:`, your driver, and the GPU requirements above. When building from source, check that SDL3 and Vulkan were found. |
| Text is missing or the game fails on its first text | No usable font: set `HOS_SHARED_FONT` to a TrueType (`.ttf`) file. |
| It crashes or hangs | The log goes to the terminal (on Windows without one: `%APPDATA%\oranges\oranges.log`). `FOLDER/make.log` covers build failures. Check both before sharing them: they contain paths from your machine. |

## Development

```bash
.venv/bin/pytest -q
```

```bash
.venv/bin/ruff check .
```

The tests use synthetic fixtures and never need the game.

| Path | Contents |
| --- | --- |
| `oranges/` | The Python pipeline: package reader, lifter fixes, build, release tooling |
| `host/` | The reimplemented system software (C) and the Vulkan back end (`host/vk/`, C++) |
| `gui/` | The setup program |
| `third_party/` | Vendored code, each with its provenance and licence |
| `tests/` | Game-free tests |
| `work/` | Local extractions and builds (never committed) |

[CLAUDE.md](CLAUDE.md) is the detailed technical reference: the design, the debugging environment variables, scripted
input and headless runs, and the cross-build recipes. [TODO.md](TODO.md) lists what is done and what is open.

Never commit or upload a game package, or anything extracted or built from one.

### Making a release

A release is a self-contained folder and archive for players. For Linux, first build its toolchain, sysroot, Python and
libraries (this also needs `cmake`, `make`, `wayland-scanner` for SDL3's Wayland support, and optionally `ninja`):

```bash
.venv/bin/python -m oranges.linuxkit
```

```bash
.venv/bin/python -m oranges.release linux
```

For Windows, it needs llvm-mingw as above, plus the two kits:

```bash
.venv/bin/python -m oranges.wintools
```

```bash
.venv/bin/python -m oranges.mingw
```

```bash
.venv/bin/python -m oranges.release windows
```

Releases go to `work/release/`. They hold only files tracked by git and the downloaded tools, nothing from the game.

## License

oranges is licensed under the **GPL-3.0-or-later** ([LICENSE](LICENSE)). It builds on:

| Component | Used for | Details |
| --- | --- | --- |
| [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (GPL-3.0-or-later) | The design, and ported code (noted in the files) | |
| a64-kit, with suyu's translator | AArch64 to C | [provenance](third_party/a64-kit/PROVENANCE.md) |
| eden / yuzu shader recompiler and sirit | Shader translation | [provenance](third_party/eden/PROVENANCE.md) |
| AMD FidelityFX FSR 1 (MIT) | Upscaling | [provenance](third_party/fidelityfx-fsr1/PROVENANCE.md) |
| Dear ImGui | The menu and the setup program | [provenance](third_party/imgui/PROVENANCE.md) |
| Khronos Vulkan and SPIR-V headers | Vulkan | `third_party/khronos/` |

The game belongs to its rights holders. The licence above covers oranges only.
