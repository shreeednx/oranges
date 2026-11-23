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

## Done: the stubbed services

`oranges lift` stubbed 148 OS-facing imports (each returned 0 without writing its outputs). 142 are HLE now, answered
as an offline console, with the results the game checks at its call sites (switchbrew's error codes where it lists
them):

- `host/nn_online.c`: nifm (not connected, 2110-1100), bcat (an empty delivery cache: the storage mounts, nothing
  opens), friends (no friends, nothing online), pctl (no parental controls), prepo, aoc (none), album (screenshots not
  kept), ec / err (logged), ldn (local wireless off), nn::socket (no network: ENETDOWN / EBADF / EAI_AGAIN, the
  address helpers for real).
- `host/nn_nfp.c`: an amiibo reader that never sees a tag. `host/nn_codec.c`: the hardware Opus decoder on libopus
  (Longshot's; `oranges build` links it when pkg-config finds it). `host/nn_diag.c`: diag (backtrace, symbol names)
  and ro. `nn_system.c`: the nn::oe setters (no-ops).
- A failed `MountDeliveryCacheStorage` stopped the frame loop (main waits on an address nobody signals), so bcat
  mounts and reports every directory as not found.

Left over from it:

- [ ] 6 stubs remain, with no caller in main: the polyphase resampler (4), `nn::audio::GetAudioOutPlayedSampleCount`,
      `nn::err::ReportMultimediaError` (subsdk0's movie player, probably). Implement them if a movie plays.
- [ ] The ldn Initialize result (2203-0022) is yuzu's ResultDisabled, not on switchbrew's list. Check it.
- [ ] Amiibo: port Longshot's tag library (`host/nn_nfp.c` there: the player's own dumps) once there is input.
- [ ] Screenshots (`nn::album::SaveScreenshot`): write them to a folder as PNG.
- [ ] Why the game stalls when the bcat mount fails is not understood (a real console may fail it too, without a
      network). Look again if another service's failure stalls it the same way.

## 1. The game never reads the pad (next wall)

It initializes hid (InitializeNpad, the style set, hold type, mouse, keyboard, touch screen) and presents frames at
30 fps, but in 100 s it never calls GetNpadStates or any other state read (counted with gdb breakpoints on the
`imp__ZN2nn3hid*` HLEs). So it is still in a stage before its title screen, which reads no input: input can't move it.

- [ ] Find what that stage waits for (sample the main thread's guest backtrace over several frames; look for a loader
      thread, a GPU result rx_null.c does not give, or a service answer)

## 2. An intermittent frame stall

About 1 run in 7 stops presenting frames within the first 20 s (seen with the last commit's host code too, and not
under gdb in 6 tries). The bcat mount failure made it happen every time. In those samples the main thread waited in
ArbitrateLock (a guest mutex) or WaitForAddress.

- [ ] Catch one: a watchdog that dumps every thread's guest backtrace (`hos_backtrace`) when no frame was presented
      for 5 s would find it without gdb's timing changes

## Done: a window and input

`host/hos_window.c` (SDL3, optional like libopus): a 1280x720 window on the process's main thread (the guest's main
thread moved to a host thread of its own), with a status page until there is a renderer (frames, fps, the input).
Keyboard and the first gamepad are Npad No.1 (a Pro Controller: buttons by position, stick directions as the console
sets them), the mouse is the USB mouse and its left button a finger on the touch screen, the keyboard is also the USB
keyboard (`GetKeyCode`: US layout), the game's vibration goes to the gamepad. `HOS_INPUT_SCRIPT` scripts input (with
QUIT), `HOS_SCREENSHOT` saves the window's picture, `HOS_NO_WINDOW=1` runs without a window. Every exit goes through
`hos_exit` (`_Exit`: library teardown crashed while other threads ran).

Left over from it:

- [ ] Frames are still paced by `nvn_native.c`'s clock (60 Hz over the present interval), not by the window. Pace
      them by the window's presents once the Vulkan back end draws into it
- [ ] Key remapping (a config file), more than one gamepad (Npad No.2-8), motion from a gamepad's gyro
- [ ] The rumble value is resent each window frame; a game that stops without sending 0 keeps it rumbling

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
