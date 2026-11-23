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

- [ ] Watch the `ipc:` lines for services the game opens through `sm:` (a request nobody emulates is logged once per
      service and command, with its caller). The second one: `prepo:u` command 10105 from `nn::prepo::PlayReport::Save(
      Uid)` (lifted: its PlayReporter thread, during the name entry), answered empty. HLE `PlayReport::Save` (both
      overloads) as a dropped report, which needs a lift.
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

## Done: the game reads the pad (it waited for the player)

The game reads the Npads, motion, mouse, keyboard and touch screen from 0.8 s on (`nn_hid.c` now logs each kind's
first read; the earlier gdb count was wrong). With no save (`deviceSave:/main.dat` missing) it plays the new-game
prologue: the airport counter (`Tutorials_Prologue1_Airport`, ~18 s), whose dialogue waits for A. With A every 1.5 s
(`HOS_INPUT_SCRIPT`) it reaches the game's own keyboard for the player's name (~58 s), and after a few letters and +,
the birthday (`DateInput`). Tools from finding it: `HOS_SAMPLE`, `HOS_PROFILE`, `HOS_FS_LOG` (CLAUDE.md).

Left over from it:

- [ ] Going further blind is guesswork: it needs the renderer (or a screenshot of the window textures) to script
      input against

## Done: the intermittent frame stall

About 1 run in 7 stopped presenting frames around 15-25 s: main waited for a SupportWorker's light event while that
worker slept on its semaphore with a count of 1. WaitProcessWideKeyAtomic released the mutex before it set the
condition variable's has-waiter word, and the SDK's Signal skips the SVC while that word is 0, so a release in between
was lost. The kernel now sets the word first (0 stalls in 40 runs after, against 2-4 in 32 before). The watchdog
(`HOS_WATCHDOG`, on by default at 5 s) dumps every thread when frames stop, the first frame included. Two more kernel
fixes from reading it against Atmosphère's: a condition variable wait that times out returns without the mutex (the
SDK's TimedWait takes it itself on TimedOut, so the old way deadlocked the thread on itself), and SignalToAddress /
WaitForAddress update the word with one compare-and-swap (guest atomics changed it between our load and store).

## Done: the startup crash in "Resource Memory" (a race in the game)

About 1 run in 10 with 4 processes at once crashed before the first frame (thread 0x100d, guest address 8). The
resource threads share a message handler (main+0x6d6514) that reads a singleton (main+0x5a05378) and its +8 field.
The Prepare thread publishes the singleton (f_0bdfc054) about 10 ms of guest work (a 32 MB heap, f_0bdfc704) before
its init stores +8 (f_0bdfc2b8), and the loaders send the handler message 1 several times a millisecond (the host's
file reads are instant), so one often landed in between. The threads are on different cores, so priority scheduling
would not have closed it. `oranges/patches.py` (new: game fixes written into the image before the lift) gives the
handler the null check the game's other reader of the singleton has: 0 crashes in 80 runs, and the prologue still
plays.

Left over from it:

- [ ] The loaders' message rate is a host artefact (instant reads). If other races of this kind turn up, consider
      giving `nn_fs.c` reads a console-like cost rather than patching each one

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

## Done: sound

`host/hos_sound.c` plays what `nn_audio.c`'s renderer mixes on SDL3's default output (48 kHz stereo PCM16; on when
there is a window, `HOS_AUDIO=0/1` overrides it). The mixer runs on the host's clock and the device on its own, so the
stream's frequency ratio follows the queue's average to a target (the device buffer and a half plus 20 ms: 52 ms on
PipeWire's 1024 frames). On this machine it settles at +0.03%, no underruns in 40 s at 30 fps. After a stall (the
process stopped for 1.5 s) the mixer renders the owed frames back to back: those that would take the queue past
target + 40 ms are dropped, and the queue is back at its target in ~4 s. The game uses only the renderer (no AudioOut).

Left over from it:

- [ ] Process the effects that only pass through today (delay, reverb, biquad filter effects), if the game needs them
- [ ] 5.1: the game's device sink has 6 channels and `render_frame` can output them; the output is stereo today
- [ ] AudioOut buffers are released on time but not played (the game doesn't use AudioOut; subsdk0's movie player may)
- [ ] Choosing the output device (`HOS_AUDIO_DEVICE`), volume, mute when the window loses focus

## 1. The renderer (the next wall: the prologue can't be played blind)

- [ ] A Vulkan back end in place of `host/rx_null.c`, replaying the recorded NVN commands (Longshot's `gpu/vk`, made
      for NVN2: adapt it to NVN1)
- [ ] Shader translation: the game's Maxwell shaders (`nvn_prog` stages) to SPIR-V
- [ ] Textures: deswizzle the block-linear layouts (`host/nvn_layout.c` already computes them) and ASTC/BCn formats
- [ ] Present the window textures to the real window

## 2. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it (the Resource Memory crash did not: other cores)
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)
- [ ] Windows build (POSIX-only today: pthreads, `mmap`, signals)
