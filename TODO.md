# Next tasks

Ordered by what blocks the boot first. `CLAUDE.md` has the background (boot status, debugging recipes).

## Done: services reached through `sm:` (the `olsc:u` abort)

About 3 s in, the game's own olsc client (`f_085b7b00` → `f_085b8260` → `f_085b8170`, linked into main, so no `nn::`
HLE stub sees it) asked `sm:` for `olsc:u` and aborted: `sm:` speaks TIPC on this SDK (request type 16 + command), the
client reads the handle at word 3 and the result at word 4 without checking the header, and the old empty CMIF reply
put `SFCO` where the result goes. `host/hos_ipc.c` now parses HIPC/TIPC/CMIF requests (domains included) and serves
`sm:` (GetServiceHandle hands out a session for any name), the CMIF control commands (ConvertCurrentObjectToDomain,
Clone, QueryPointerBufferSize) and the emulated services in its `services[]`. `olsc:u` is one (`nn_system.c`:
Initialize, GetSaveDataBackupSetting = off). With it the game runs its frame loop at 30 fps for 90 s and more.

Left over from it:

- [ ] `olsc:u` is the only service the game opens through `sm:` in the first 90 s. Watch the `ipc:` lines for more
      once input gets it further (a request nobody emulates is logged once per service and command, with its caller).
- [ ] A command that returns an interface but is answered empty gives the client no handle / object id (it reads 0;
      requests to object 0 are answered empty too). Emulate such commands when one turns up.
- [ ] Session objects (`Session` in `hos_ipc.c`) are never freed when their handle is closed.

## Done: the stall after the first frame

Two causes, both fixed:

- The kernel reported every thread on core 0 (SetThreadCoreMask was a no-op). The game gives a thread pinned to one
  core the TLS index `core + 1` (`f_0b7e2374`), and the frame loop's job scheduler (`f_086410b0`, object at `+0x268`:
  per-thread slots, `+0x290` phase, barriers at `+0x298`) indexes its slots by it. All three of its threads (main on
  core 0, workers on cores 1 and 2) used slot 0, so the barrier loop never ended. `host/hos_kernel.c` now tracks
  each thread's ideal core and affinity mask, and `GetCurrentProcessorNumber` / `GetThreadCoreMask` report them.
- clang hoisted a plain LDR out of a spinlock wait (`f_0b8729e0`, `0xb872a64`: `ldr w9, [x8]; cmp w9, #1; b.eq`) and
  made it `jmp .`, so a worker spun forever. `oranges/spin.py` puts a compiler barrier after the loop-invariant loads
  of small loops at lift time (159k loads).

Left over from it:

- [ ] `WaitForAddress` consumes a pending `CancelSynchronization`; the console's kernel only cancels
      `WaitSynchronization`. A cancel aimed at a thread in a light-event wait would make that wait abort.
- [ ] Check the `SignalToAddress` type 2 (ModifyByWaitingCountIfEqual) value rule against Atmosphère's
      `KAddressArbiter` source (the comment in `hos_kernel.c` marks it unverified). Auto-clear light events use it.
- [ ] Measure what the spin barriers cost once frames run (they should cost nothing: the guest re-reads those words
      anyway). If they cost something, leave out loops with calls (a call already stops the hoisting).

## 1. Remaining stubbed services

`oranges lift` reports 147 stubbed OS-facing imports. Each one returns 0 without writing its outputs.

- [ ] `nn::aoc` (add-on content: none, with the outputs written)
- [ ] `nn::nifm` (network: not connected, reported properly)
- [ ] `nn::friends`, `nn::bcat`, `nn::nfp` (amiibo), `nn::prepo`, `nn::err` (Longshot's `host/nn_applet.c` has most
      of these)
- [ ] The `nn::oe` notification setters and the performance-configuration calls (harmless today; check them)
- [ ] Go through the stub list for any call whose result the game uses as a size, pointer or state

## 2. A window and input

- [ ] Open a window (SDL or similar) and drive the frame from it
- [ ] Map keyboard and gamepad input into `host/nn_hid.c` (`hos_input()` is where they come in)
- [ ] Mouse and touch from the window
- [ ] Optional: scripted input for test runs (Longshot's `HOS_INPUT_SCRIPT`)

## 3. Sound

- [ ] A sound device for `host/nn_audio.c`. The renderer already mixes and `HOS_AUDIO_DUMP=<file.wav>` writes what
      would play. Swap `output_open` / `output_frame` for a real sink.
- [ ] Process the effects that only pass through today (delay, reverb, biquad filter effects), if the game needs them

## 4. The renderer

- [ ] A Vulkan back end in place of `host/rx_null.c`, replaying the recorded NVN commands (Longshot's `gpu/vk`, made
      for NVN2: adapt it to NVN1)
- [ ] Shader translation: the game's Maxwell shaders (`nvn_prog` stages) to SPIR-V
- [ ] Textures: deswizzle the block-linear layouts (`host/nvn_layout.c` already computes them) and ASTC/BCn formats
- [ ] Present the window textures to the real window

## 5. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Fix the sign-compare warning in `host/hos_kernel.c` (`best_waiter`)
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)
- [ ] Windows build (POSIX-only today: pthreads, `mmap`, signals)
