# Next tasks

The finished work first ("Done:", with what was left over from each), then the open tasks by priority (1 first).
`CLAUDE.md` has the background (boot status, debugging recipes).

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

- [ ] 5 stubs remain, with no caller in main: the polyphase resampler (4), `nn::err::ReportMultimediaError` (subsdk0's
      movie player, probably; the opening movie plays without calling them)
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

- [x] Going further blind is guesswork: it needs the renderer. It has one now (`HOS_SCREENSHOT` / `HOS_VK_SHOTS` to
      script input against)

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

- [ ] Frames are still paced by `nvn_native.c`'s clock (60 Hz over the present interval), not by the window (the
      Vulkan back end presents in mailbox mode): see the renderer's leftovers
- [ ] Key remapping (a config file: task 3, the settings menu), more than one gamepad (Npad No.2-8), motion from a
      gamepad's gyro
- [ ] The rumble value is resent each window frame; a game that stops without sending 0 keeps it rumbling

## Done: sound

`host/hos_sound.c` plays what `nn_audio.c`'s renderer mixes on SDL3's default output (48 kHz stereo PCM16; on when
there is a window, `HOS_AUDIO=0/1` overrides it). The mixer runs on the host's clock and the device on its own, so the
stream's frequency ratio follows the queue's average to a target (the device buffer and a half plus 20 ms: 52 ms on
PipeWire's 1024 frames). On this machine it settles at +0.03%, no underruns in 40 s at 30 fps. After a stall (the
process stopped for 1.5 s) the mixer renders the owed frames back to back: those that would take the queue past
target + 40 ms are dropped, and the queue is back at its target in ~4 s. (The opening movie plays through AudioOut:
"Done: the opening movie".)

Left over from it:

- [ ] Process the effects that only pass through today (delay, reverb, biquad filter effects), if the game needs them
- [ ] 5.1: the game's device sink has 6 channels and `render_frame` can output them; the output is stereo today
- [ ] Choosing the output device (`HOS_AUDIO_DEVICE`), volume, mute when the window loses focus

## Done: a renderer (the prologue is drawn)

`host/vk/` replays the recorded NVN commands on Vulkan, written for NVN1 rather than ported from Longshot's NVN2 back
end (3.7 MB of C++ with its game's features); it takes eden's shader recompiler (yuzu's Maxwell frontend and SPIR-V
backend, vendored from the Longshot toolchain into `third_party/eden`). The airport prologue renders at 30 fps:
the two shopkeepers at the counter, lit and textured, the dialogue boxes and choices, in the window (Wayland, mailbox)
and headless (`HOS_SCREENSHOT`). Design in `host/vk/vk.h`: one queue, each submit run to completion, guest memory
pools imported (`VK_EXT_external_memory_host`), shader objects with all state dynamic, pushed descriptors, images in
`GENERAL` with full barriers. What it took to get the first picture (each was a black screen):

- NVN1 shader packages: control section magic `0x98761234` version 1; the code area is a 0x30-byte header, the SPH at
  +0x30, code from +0x80; the frontend's addresses are the code area's (scheduling words on its 32-byte boundaries),
  start 0x30. c1 (the program's constants and branch tables, which eden folds into the translation) is the code area
  from control +0x700 to +0x704. All 383 programs of the first minute translate and pass `spirv-val`.
- The first stage's inputs: eden declares a vertex attribute only when the previous stage stores it, so the vertex
  stage gets `previous_stage_stores.mask.set()` (every position was 0).
- NVN's viewport puts clip +y at the top in its upper-left origin mode (the game's projections are GL's): a y-flipped
  Vulkan viewport, which also keeps NVN's front faces front faces.
- `gl_Position` invariant (eden's `position_invariant`): the colour pass tests depth EQUAL against a pre-pass.
- Conditional rendering (`SetRenderEnableConditional`): the UI composites each layer under a condition; ignored, its
  disabled clears wiped the picture. Evaluated after flushing the GPU work before it (a sync point).
- 3D textures: a render target view takes the slice the NVN view names; a 3D texture a shader samples as 2D gets a 2D
  view of its first slice (`VK_EXT_image_2d_view_of_3d`).

Left over from it:

- [x] Capital letters were missing from the UI text: the font's glyph cache (a 2048x4096 R8 texture) gets each glyph
      written in place when text first needs it, with no flush call; the 64-span fingerprint missed them. Every
      texture's whole storage is now hashed every few frames within 32 MB a frame (`vk_texture.cpp`), and
      `nvnMemoryPoolFlushMappedRange` / `nvnBufferFlushMappedRange` mark ranges stale (`rx_nvn_flushed`)
- [x] ASTC: decoded on the CPU with suyu's decoder (`third_party/eden/video_core/textures`); the choice cursor shows
- [ ] The whole-storage check costs up to 32 MB of hashing a frame on the game's submitting thread: measure, and move
      it (or the ASTC decodes) off that thread if it shows
- [ ] Not replayed yet: indirect draws, texel buffers, alpha test, line widths,
      multisample resolves, depth-stencil fills from memory. None seen in the prologue so far (logged once if they are)
- [ ] Performance: every submit waits for the GPU, every pass ends with a full barrier, the driver banks are rebuilt
      for each draw, programs are translated on the game's thread. Profile once more of the game draws
- [ ] Memory: images and views are never freed (the game's transient targets alias one virtual pool region; each
      size/format there is its own image)
- [ ] Frames are paced by `nvn_native.c`'s clock; mailbox/immediate presents. Pace by the display (`HOS_VSYNC=1` is
      FIFO today)
- [ ] Run once with the Khronos validation layer (`HOS_VK_VALIDATE=1`; not installed on this machine)
- [ ] Occlusion queries and other counters write 0 (only timestamps seen so far)

## Done: compute dispatches (the character creator)

The game's first compute dispatches come with the character creator (~80 s with A and + every 1.6 s): one program,
1x1x1 groups of 32 threads, reading six textures and its uniform buffers and writing 16-byte results to storage buffers
(a per-frame lighting or exposure reduction, by the look of it). An NVN1 compute stage differs from a graphics one: its
code area has no header (the code starts at +0, with a scheduling word), and its control section has the workgroup size
at +0x748/+0x74c/+0x750 and the shared memory bytes at +0x754 (32, 1, 1 and 0x780 here; the code's STS/LDS confirm the
shared memory). Found by comparing the compute stage's control section with three graphics stages': +0x708 is the
code's start in the code area (0x30 graphics, 0 compute), +0x70c the register count, +0x714 the NVN stage.
`vk_replay.cpp` replays `DispatchCompute` and `DispatchComputeIndirect` (the counts read by the GPU from the imported
buffer; the driver bank's grid at c0 +0x200 from guest memory) between full barriers, and storage images are bound now
(an image handle is its texture pool id; a storage view of one level, sRGB through its linear twin).

Left over from it:

- [ ] Check the compute program's results against a capture if a picture ever looks wrong around it (nothing to compare
      against yet: the creator looks as it did without it)
- [ ] Indirect draws, texel buffers: still not seen through the opening (to ~195 s)

## Done: the opening movie

After the island questions the game plays `Movie/OpeningMovie.webm` (VP9 1920x1080 30 fps + Vorbis 48 kHz stereo, 65 s)
through subsdk0's `movie::` library: Android's stagefright ported (`movie::Decoder` holds a `DecoderImpl` that drives
an `android::MediaCodec`). Its video codecs are NVIDIA's hardware OMX components, which open an nvhost channel
(`NvRmChannelOpen` -> `NvOsDrvOpen` aborted). `host/nn_movie.c` HLEs every `movie::Decoder` method (all virtual, hooked
at their definitions in subsdk0): a video decoder is ours, on FFmpeg's libavcodec (optional, `HOS_HAVE_FFMPEG`), with
`DecoderImpl`'s statuses, index lists and client events (read from its code); an audio decoder is passed through to
the lifted `DecoderImpl` (its software Vorbis codec runs as it is), built as the library's constructor builds it. The
extractor (WebM) runs lifted. The movie plays to its end (1960 pictures, then the end-of-stream buffer), and + skips
it as on the console.

Its sound goes out through AudioOut, which was released on a clock and never heard: `nn_audio.c` now mixes each
running AudioOut's queue into the renderer thread's frames (a buffer is released once played; Stop releases them all,
as yuzu's does), with its volume and `GetAudioOutPlayedSampleCount`. The dump matches FFmpeg's decode of the movie's
Vorbis track sample for sample (correlation 1.000, no drift over 40 s). `hos_symbol_lookup` also finds data objects now
(`gen/hos_symbols.c` lists them: the `movie::Decoder` vtable).

Left over from it:

- [ ] Video output formats other than NV12 (2: ARGB; mode 2 / format 3: into an NVN texture): the game uses NV12 only
- [ ] The movie's pictures and sound are in sync by the game's player; not measured here

## Done: the stall after the movie (sub mixes never acquired again)

About 15 s after the opening movie, while the landing scene loaded, the main thread waited forever in its frame end
(`0x86ba430`) on a job manager's slot event (manager `[[0xcbd1a78]+8]`: workers at `+0x70`, 64-byte slots at `+0x30`
with the light event at +0x30, signalled by `0xbe42690` once no worker runs a job of that slot's group). The job left
running on a SupportWorker was the sound system's effect teardown (`0xba80e20`): it polls each effect's attached flag
every 5 ms, and the audio thread clears it in the detach (`0xba59b90`) only once the effect's `nn::audio::Is*Removable`
says yes. `IsAuxRemovable` said no forever: our `aux_of` did not know those Auxes, because `AddAux` had refused them
(mix -1), because their sub mixes were unknown. On the first scene change (~3 s) the game releases its 10 sub mixes and
acquires again only those it does not see as acquired, by the SubMixType's first word (the SDK's Release* / Remove* zero
the object's first word, see `MixManager::Release`); ours kept our handle, so they were never acquired again.
`nn_audio.c` now zeroes the word on every release and remove (voices, mixes, splitters, Auxes, effects, memory pools;
sinks keep it, as in the SDK): the game acquires sub mixes 1-10 again and its Auxes and reverbs land on them, and the
landing plays (the plane, the island from above, a shopkeeper at the dock) with frames flowing through 300 s. The first
minute's sound has the same level as before, 5 s window by window (why the voices were heard with their sub mixes gone
was not looked into). `HOS_AUDIO_TRACE` also logs refused Auxes, removed Auxes and released mixes.

Left over from it:

- [ ] Other fields the SDK writes into the game's audio objects that a game might read (only the first word is
      modelled). Check an `nn::audio` object's SDK accessor when a game reads one directly.
- [ ] A gdb hardware watchpoint set before `run` on the event word (a page mapped later) never fired, though the word
      was written (signalled at init, a waiter's 1). Not understood; set watchpoints once the page exists.

## Done: the orientation and the tent (driving the game live)

From the dock (~265 s) the player walks to the plaza, the manager gives an orientation, a shopkeeper hands over a tent,
the player pitches it (pockets, Build Here, the preview, the landscaping question) and reports back. The manager then
sends the player off to help the villagers find their spots. No wall and nothing drawn wrong on the way (the minimap,
the pockets, the placement grid included), 29.9 fps throughout.

A fixed script can't get there: every island is laid out at random (the map, the airport's colour, the villagers,
the player's look), so a route that works once walks into a tree the next time. The game seeds sead's RNGs from the
tick (`nn::os::GetSystemTick`, inlined in 671 functions), so a fixed `RandomEntropy` changes nothing. Instead
`HOS_CONTROL=<fifo>` (`hos_window.c`) drives a running game: lines of `<seconds> <inputs>`, `SHOT <bmp>` (the next
presented picture, the line waits for it), `DUMP` (every thread), `PROFILE <s> [thread]`, `QUIT`. CLAUDE.md has the
recipe. Things learned on the way:

- An apparent wall at the plaza (no talk, no tree shake, the manager's orientation never starting) was the input script:
  cut after `265.8 A`, its last line held A down for good. A script's last line is held to the end (documented now).
- The orientation starts when the player talks to the manager (or to a shopkeeper: the manager's talk
  class, `0x8a06560`, takes over a talk with `rcm` / `rct`). Then a shopkeeper gives the tent; the manager only
  repeats where to get it.
- `nn_hid.c`'s Npad states are a real history now (the input sampled every 5 ms, GetNpadStates the latest 16).
  Before, every state of a read held the input of the moment and one counter shared by every hid read numbered them.
  It wasn't the cause of anything seen, but the console's history is a time series and the game reads 16 a frame.
- `vk_replay.cpp` logs each counter type the game reports, once: only timestamps (0, 16) so far.

## 1. Further into the prologue: the villagers

The manager asks the player to help the two villagers find spots for their tents (somewhere on the island; the minimap
shows the player only). Then on the console: their building sites, the campfire (fruit and branches), the first
night and the first save (`deviceSave:/main.dat`; nothing has been written so far). Drive it with `HOS_CONTROL`.

Why first: it is what finds the next wall, and the first save lets later runs load a game instead of replaying the
prologue's ~5 minutes each time.

## 2. Check touch input: the mouse on the game's keyboard

The window's left mouse button is a finger on the touch screen (`hos_window.c`: the 1280x720 picture, letterboxed;
`TOUCH=x,y` in `HOS_INPUT_SCRIPT` and `HOS_CONTROL`), but no one has seen the game react to it. The game reads the
touch screen from 0.8 s, and from another caller (`lr 0xb1c3bc4`) once its keyboard for the player's name is up
(~58 s): tapping its keys should type. Check it with `HOS_CONTROL` (`TOUCH=x,y` held a moment, then `SHOT`), and with
the mouse in the window. If it doesn't react, look at the touch state's layout and the sampling (`nn_hid.c`
`touch_state`: the delta time, the attributes for a finger going down and up, the finger id).

Why here: small and quick (one run to 60 s), and the result matters to the settings menu's "mouse as touch".

## 3. An oranges settings menu (controls first)

An in-game settings menu like Longshot's "Insert menu" (its `gpu/vk/vk_menu.cpp`, the look in `vk_ui.h`, the file in
`host/options_path.c`; its user guide is `docs/user/graphics-and-controls.md`). That menu is drawn with Dear ImGui over
the window's image at the end of a present, so it costs nothing while closed and never touches the game's picture.
F1 opens it (a gamepad's Home too), and the game gets no input while it is open. It has tabs (Graphics, Audio,
Gameplay, Controls, ...), its choices are saved as they change in an options file, and rows that need a restart are
marked. Only the Controls tab matters for now; the tabs can come later as there are settings for them.

- [ ] The menu itself: Dear ImGui on the Vulkan back end (`host/vk`), drawn over the presented image (not with
      `HOS_VK=0`). Keyboard, gamepad and mouse to move around it. While it is open, `hos_input()` gives the game
      nothing.
- [ ] An options file (`oranges_options.ini`, in the user's config folder), read at start and written on each change.
- [ ] Controls tab: rebind each Switch button (and the left stick's and the camera's directions on the keyboard) to a
      key and a gamepad input. This replaces the fixed key map in `hos_window.c`'s header. Also: reset a row or all of
      them, and warn when an input is already bound. Then, as needed: stick dead zone, vibration on/off and strength,
      which gamepad is Npad No.1, the mouse as touch.
- [ ] Keep `HOS_INPUT_SCRIPT` and `HOS_CONTROL` independent of the bindings: they speak Switch buttons.

Why here: what makes the game playable by a person rather than by scripts (fixed keys today).

## 4. The renderer's gaps

The leftovers under "Done: a renderer" above: indirect draws and the other unreplayed commands when the game first
uses them (logged once), then performance and memory.

Why here: nothing unreplayed has turned up yet (each is logged once when it does), and 30 fps holds so far. It moves up
as soon as one of those logs or a wrong picture shows.

## 5. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it (the Resource Memory crash did not: other cores)
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)
- [ ] Windows build (POSIX-only today: pthreads, `mmap`, signals)

Why last: none of these blocks the game; take them on when one starts to matter (memory on long runs first).
