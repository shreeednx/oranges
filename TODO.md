# Next tasks

Ordered by what blocks the boot first. `CLAUDE.md` has the background (boot status, debugging recipes).

## 1. Services reached through `sm:` (the `olsc:u` abort)

About 3 s in, after the first frame and the device-save mounts, the main thread aborts (`nn::diag::Abort` on a
result, `svc Break`). The game's own olsc client (`f_085b7b00` → `f_085b8260` → `f_085b8170`, linked into main, so
no `nn::` HLE stub sees it) calls `nn::sf::hipc::ConnectToHipcService("olsc:u")`. The IPC auto-reply to `sm:`
`GetServiceHandle` moves no handle, so `ConvertToDomainImpl` runs on handle 0 and fails (`f_085b85e4`, abort at
`0x85b833c`). Reached from the frame loop: `f_0a809b90` / `f_0a80a810`.

- [ ] Answer `sm:` `GetServiceHandle` with a session handle (a move handle in the reply), so that unemulated services
      get sessions whose requests are answered empty, like `ConnectToNamedPort` already does. `ConvertToDomain` then
      needs a domain object id in its reply.
- [ ] Or answer `olsc:u` properly: no cloud backup (Longshot's `host/` may have it). Check what the game does with
      the olsc results after that.
- [ ] See which other services the game opens through `sm:` from here on (the `ipc:` lines in the log).

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

## 2. Remaining stubbed services

`oranges lift` reports 147 stubbed OS-facing imports. Each one returns 0 without writing its outputs.

- [ ] `nn::aoc` (add-on content: none, with the outputs written)
- [ ] `nn::nifm` (network: not connected, reported properly)
- [ ] `nn::friends`, `nn::bcat`, `nn::nfp` (amiibo), `nn::prepo`, `nn::err` (Longshot's `host/nn_applet.c` has most
      of these)
- [ ] The `nn::oe` notification setters and the performance-configuration calls (harmless today; check them)
- [ ] Go through the stub list for any call whose result the game uses as a size, pointer or state

## 3. A window and input

- [ ] Open a window (SDL or similar) and drive the frame from it
- [ ] Map keyboard and gamepad input into `host/nn_hid.c` (`hos_input()` is where they come in)
- [ ] Mouse and touch from the window
- [ ] Optional: scripted input for test runs (Longshot's `HOS_INPUT_SCRIPT`)

## 4. Sound

- [ ] A sound device for `host/nn_audio.c`. The renderer already mixes and `HOS_AUDIO_DUMP=<file.wav>` writes what
      would play. Swap `output_open` / `output_frame` for a real sink.
- [ ] Process the effects that only pass through today (delay, reverb, biquad filter effects), if the game needs them

## 5. The renderer

- [ ] A Vulkan back end in place of `host/rx_null.c`, replaying the recorded NVN commands (Longshot's `gpu/vk`, made
      for NVN2: adapt it to NVN1)
- [ ] Shader translation: the game's Maxwell shaders (`nvn_prog` stages) to SPIR-V
- [ ] Textures: deswizzle the block-linear layouts (`host/nvn_layout.c` already computes them) and ASTC/BCn formats
- [ ] Present the window textures to the real window

## 6. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Fix the sign-compare warning in `host/hos_kernel.c` (`best_waiter`)
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)
- [ ] Windows build (POSIX-only today: pthreads, `mmap`, signals)
