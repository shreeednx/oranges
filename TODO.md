# Next tasks

The finished work first ("Done:", with what was left over from each), then the open bugs, then the open tasks by
priority (1 first).
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
- Screenshots (`nn::album::SaveScreenshot`) are task 3 now.
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
- [x] Key remapping (the settings menu's Controls tab, F1). Still open: more than one gamepad (Npad No.2-8),
      motion from a gamepad's gyro
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
      for each draw. Programs are made on maker threads now, render conditions don't wait ("Done: the renderer's time
      measured", with the per-draw breakdown)
- [ ] Memory: images and views are never freed (the game's transient targets alias one virtual pool region; each
      size/format there is its own image)
- [ ] Frames are paced by `nvn_native.c`'s clock; mailbox/immediate presents. Pace by the display (the menu's V-Sync
      is FIFO presents, still paced by that clock)
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

## Done: the rest of the prologue and the first save

Driven with `HOS_CONTROL`: the villagers' spots, 10 branches, 6 peaches, the campfire party (the island named on the
game's keyboard), the camping cot, the musician's dream, the phone and the loan. The game then saves
(`deviceSave:/main.dat` 10 MB, `mainHeader.dat`, `landname.dat`, `Villager0/`) and a fresh run loads it: title screen
(Ver. 3.0.3) and the island ~30 s after boot. No wall on the way. A copy of that save is in `work/user/save_day1`.
What made it possible: `HOS_CONTROL` has `MEM <file>` (the guest's writable pages as a sparse file), `PEEK` and
`POKE`, and `oranges/drive.py` (`python -m oranges.drive find|pos|walk|path|items|collect|shake|bodies`) finds the
player's physics body and the field item layer in memory and walks by position. Its docstring has the layouts found
(coordinates, the 288 x 192 item layer, item ids). A missing villager in the first run was my search by eye, not
the game: the memory search found both at once.

Left over from it:

- [x] `python -m oranges.drive find` ran on the game (the day-1 save at the plaza, 2026-11-24): it found the player's
      body and the item layer, and `pos` followed the stick. The body is the player model's matrix, which the game
      copies from the actor's own (actor +0x278: "Done: 60 fps researched")
- [x] Touch: a tap on the island-name keyboard typed nothing (the touch state's layout: "Done: the Graphics tab")
- [ ] The first save's time is the host clock (night here): the morning tutorial ran at 9 PM. Fine, but a
      `HOS_TIME` offset would let runs see daytime.

## Done: NPC talking sounds (sub mixes into splitters)

The villagers' talking was never heard. The game renders it itself (from `.bars` voice archives it
loads) and plays it as a mono PCM16 stream (256 samples a buffer, four in a ring, started with the first line) on
its own voice into a 1-buffer sub mix, and it sends that sub mix (and four others: 1, 6, 8, 10 of the 11) into a
splitter (`SetSubMixDestination` with a `SplitterType`, to its bus and the final mix). `nn_audio.c` dropped a sub mix's
splitter destination ("not used by TOTK so far"), so those mixes went nowhere. A sub mix into a splitter now feeds each
destination mix: its buffers are the splitter's source channels, at `SetSplitterMixVolume`'s volumes (the game sets them
buffer i to buffer i) times the sub mix volume, ramped as a voice's; the mix order follows splitters, and a released
splitter drops the sub mixes sent to it. The shopkeeper's lines at the airport counter are 2-3x the level of the music
alone (22-29 s of the prologue), no clipping; the day-1 save loads to the island as before.

Left over from it:

- [x] Listen to it with the window: the villagers' talking is heard and works (checked by the user, 2026-11-24)
- [x] The other sub mixes that were silent play now too (6 and 8 send 0.125 / 0.25 to a bus and the final mix, the
      look of reverb sends): the island's mix sounds right, no extra reverb or doubled sounds (the user, 2026-11-24)
- [ ] A splitter destination's biquad filters apply to voices only, not to a sub mix sent through it (unknown
      whether the console's do; the game hasn't set one there so far)

## Done: the settings menu (F1), with the Controls tab

F1 (or a gamepad's Home) opens an oranges settings menu over the game, modelled on Longshot's Insert menu but much
smaller: `host/hos_menu.c` is its model and input (tabs, rows, selection, a binding waiting for its input, questions),
`host/vk/vk_menu.cpp` draws it with Dear ImGui's draw lists (`third_party/imgui`, 1.92.9b from Longshot's tree) in
Longshot's Modern dark colours with the host's TrueType font. While it is open each present composes the picture and
the menu into a canvas of the picture's size, which the window and the screenshots get (so `SHOT` shows it headless
too); closed, it costs nothing. The game gets no input while it is open (`hos_input`, scripts included), and what is
held when it closes stays away from the game until it is let go. Choices are saved as they change in
`oranges_options.ini` (`host/hos_options.c`: `$XDG_CONFIG_HOME/oranges/`, else `~/.config/oranges/`;
`HOS_OPTIONS_FILE` overrides; unknown lines kept).

The Controls tab: every Switch button and stick direction has two keys and a gamepad input (`host/hos_controls.c`,
whose defaults are the old fixed map; `hos_window.c` reads the keyboard and gamepad through it). Enter / A waits for
the new input (Esc or 6 s cancels); an input another control has is moved only after a yes; Delete / X clears a cell,
R / Y resets a row, a Reset all row asks first. Also Stick dead zone (10%), Vibration (Off to 100%) and Mouse as
touch screen. Keyboard, gamepad (the buttons bound to A, B, X, Y; D-pad or stick with repeat) and mouse (point, click,
right click, wheel) move around it. `HOS_CONTROL` has `MENU <action>`, `MENU KEY <name>` and `MENU PAD <name>`.
Checked headless on the day-1 save (open, rebind with a conflict, reset all, the player not moving while it is open
and moving after) and once in the window (Wayland). `tests/test_host_menu.py` runs the options file, the bindings and
the model without the game or SDL.

Left over from it:

- [ ] Try it by hand in the window: keyboard, a gamepad (Home, the D-pad / stick repeat, triggers as bindings) and the
      mouse (hit-testing by the console's 1280x720 coordinates). Only `HOS_CONTROL` drove it so far.
- [x] Which gamepad is Npad No.1 (the Controller row); more than one player (Npad No.2-8): the Steam Controller's
      leftovers
- [ ] The menu is drawn only at the game's presents (30 fps): it freezes while the game does not present (a long load)
- [ ] Nothing needs a restart yet (the Graphics tab's settings all apply at once); the row flag (`restart`, "(after a
      restart)") stays for one that will

## Done: the Steam Controller through Steam Input

Started from Steam (a non-Steam game, Steam Input forced on), the Steam Controller acted as Steam's keyboard and mouse
(reported by the user, 2026-11-23). The user's logs settled it: on Wayland SDL opened Steam's pad but Steam kept its
Desktop layout; with X11 Steam switched to the game's layout, and every button could be rebound but no stick direction.
Now, checked by the user from Steam: the controller plays the game, stick directions bind, Select + Start opens the
menu.

- Started by Steam (`SteamGameId` / `SteamAppId`) the window is an X11 one (XWayland) when X11 is there: Steam Input
  follows the window in front through X11, and a native Wayland window kept its Desktop layout. `SDL_VIDEO_DRIVER`
  still chooses; `HOS_WAYLAND=1` keeps Wayland. Steam's variables and every gamepad SDL announces are in the log.
- The gamepad's stick directions are bindable inputs (`leftstick_up` ... `rightstick_right`): the stick rows default
  to the gamepad's own directions, an analog amount goes through the binding (sticks swapped, a stick direction on a
  button, a button on a stick), and a stick pushed past 60% is taken while a gamepad cell waits.
- Steam keeps Home for itself: Select + Start together on any gamepad open and close the menu (Longshot's chord; both
  kept from the game while held together; the first of the two may reach it for a moment).
- A Controller row on the Controls tab, while a gamepad is connected: Auto (Steam Input's pad when there is one, else
  the first connected) or a gamepad by name, saved as SDL's GUID of its model (`#2` for a second of the same model) and
  its name; one chosen and not connected shows as such, with Auto meanwhile. Every gamepad is opened; the chosen one is
  the Pro Controller (its rumble too), and gamepads coming and going are picked up.

Left over from it (low priority: the user has one controller to test with for now):

- [ ] By hand with two gamepads: the Controller row choosing between them, one unplugged and plugged in again
- [ ] More than one player (Npad No.2-8, a gamepad each)
- [ ] From Longshot (`host/steam_pads.h`, `host/pad_sdl.c` there): Valve's real pad read beside Steam's stand-in for
      its motion sensors (gyro aiming in the game's camera, the Switch's motion controls)

## Done: the Graphics tab (Docked or Handheld, live)

The settings menu (F1) opens on a Graphics tab now, beside Controls: Console mode, Full screen, V-Sync, Scaling.
What the game does with each mode, found by reporting Handheld (`GetOperationMode` 0, `GetPerformanceMode` Normal) on
the day-1 save and tracing its targets (`HOS_VK_LOG`): docked it renders 1920x1080 (with 960x540 and 480x272 chains, a
2048 shadow map), handheld 1280x720 (640x360, 320x180, a 1536 shadow map), into the same 1920x1080 window texture with
the crop at 1280x720. The viewports always cover the whole target: no dynamic resolution. Both draw right.

- Console mode: Docked (1920x1080) / Handheld (1280x720), `nn_system.c` reports it. A change while the game runs goes
  out as the console's: the game turns on both notifications and polls `TryPopNotificationMessage` every frame (its
  handler, `0xbb10b20`: message 30 reads `GetOperationMode` again into a docked flag, 31 the performance mode), so
  oranges queues OperationModeChanged (30) and PerformanceModeChanged (31). The game re-renders at the new size within a
  second and back again (the same targets as a boot in that mode). `HOS_HANDHELD=0/1` sets it for a run.
- Full screen (F11 too, kept in step with it and with the desktop's own changes), V-Sync (FIFO; `HOS_VSYNC` for a run),
  Scaling: Smooth (linear), Sharp (nearest), Sharp, whole steps (nearest at the largest integer scale that fits). The
  mouse maps through the rectangle the present really used (`hos_window_picture`), so whole steps keep it right.
- Screenshots (`SHOT`, `HOS_SCREENSHOT`, `HOS_VK_SHOTS`) take the crop, as the window shows it: 1280x720 handheld.
- Touch types now, docked and handheld (the name keyboard: q, w, then e after switching to docked). The mode was not
  the cause: `nn_hid.c` wrote the touch's x and y one field early (x in the finger id, y in x) and gave no
  TouchAttribute. The touch screen is a history now as the Npads are (4 ms samples), with Start on a finger's first
  sample and a last state with End after it lifts.

Left over from it:

- [ ] Try by hand in the window: Full screen from the menu and F11, V-Sync on and off, the three Scaling choices (the
      mouse on the menu and as touch with whole steps). Only built and checked headless so far.
- [ ] Which of the two touch fixes mattered (the offsets, the attributes) was not isolated: both are the SDK's layout.
- [ ] Handheld still reports the Pro Controller as Npad No.1; a handheld console has its Joy-Cons attached as the
      Handheld Npad (style Handheld, id 0x20). The game plays either way; a game that checks the style would not.
- [ ] Each mode switch leaves the old size's targets allocated (the renderer never frees images: its memory leftover).
- [x] Above 1080p: decided against enlarging the game's render targets (Longshot's "Output resolution": the UI's
      layout, the post effects' texel offsets, the game's reads of its own targets would all need care). FSR 1 at the
      present instead (done: "Done: FSR 1"); FSR 3.1 is ruled out (see "Looked at and ruled out").

## Done: FSR 1 (a sharper picture on big screens)

The Graphics tab's Scaling has a fourth choice, FSR 1, with a Sharpness row (Off, 10-100%, default 80%) shown only
while it is chosen. When the window is larger than the picture, `host/vk/vk_fsr.cpp` runs AMD's EASU (upscaling to
the size the window shows it at) and then RCAS (sharpening) as compute passes, and the present copies the result 1:1
into the swapchain. When the window isn't larger, it scales as Smooth. The passes are `host/vk/shaders/fsr1_*.comp` on
the FidelityFX SDK's FSR 1 headers (`third_party/fidelityfx-fsr1`, v1.1.4 as Longshot pins it, unchanged, MIT).
`python -m oranges.shaders` compiles them into checked-in SPIR-V headers, so a build needs no glslang, and
`tests/test_shaders.py` checks those headers against their sources.

- Input: the final picture with the UI (the crop of the window texture, or the menu's canvas while the menu is open),
  blitted 1:1 into an RGBA8 image first, so a crop, a wider format or an sRGB texture all work the same way.
- Encoding: the game presents a UNORM picture holding gamma-encoded values, which is what EASU wants. The passes work on
  the stored values (through a UNORM view when a texture is sRGB) and RCAS writes into an image of the picture's own
  encoding, so the blit to the swapchain (same encoding) passes the bytes through. The mean brightness of a 4K shot is
  the same as Smooth's (0.312 for both).
- `HOS_CONTROL`'s `SHOT <bmp> <w>x<h>` saves the picture as a window of that size would show it, Scaling applied, so
  this was checked headless on the day-1 save's island (an RTX 5080): 4K and 1440p, docked and handheld (a 1280x720
  crop upscaled 3x), Sharpness 80% and Off, and a window smaller than the picture (Smooth). The HUD clock's text and
  the outlines are clearly sharper than with Smooth at 4K, with no halos or colour shift. Plain screenshots keep the
  game's size.

Left over from it:

- [ ] Try it by hand in the window at 1440p / 4K (full screen): only the headless `SHOT` path was looked at. A window
      resized live rebuilds FSR's images each frame of the resize (a device wait each time).
- [ ] Its GPU cost was not measured (two compute passes over the window's pixels, between full barriers, every present).
- [ ] The 16-bit (half) paths of EASU / RCAS are unused: the 32-bit ones run everywhere.

## Done: 60 fps researched (no: the world steps per frame, system by system)

The game's loop runs faster when let (`HOS_NO_PACING=1`: ~120 fps; `HOS_PRESENT_INTERVAL=1`: 55-60 fps, the picture
right), but its world steps once per frame: at 60 fps the player walks twice as far in a second. The question was
whether the game has one time step to halve, as Longshot's game has (`host/oot_high_fps.h` there: TotK already steps
by the frame's time, so only the places that assume a whole 60 fps step needed fixing). The game has none:

- The frame loop is sead's `GameFrameworkNx` (vtable `0xca63678`; `procFrame_` `0xb7b8c24`: draw, calc `0xb7b90f4`,
  then the present wait `0xb7b94d0`), on MainThread; "Presentation Thread" only runs the NVN presents. The calc gets
  no time argument. The vblank interval (framework +0xa0: 2) comes from the window setup (`0xb7c24d0`, the byte at
  +0x48 of its argument). `calcFps` (vtable +0xa8) is the measured rate (tick frequency over the last frame's ticks).
  The framework can drop to 20 fps by itself (+0x245: the window's interval 2 -> 3 after slow frames), but the game
  never changes the interval (none in the log).
- The player's movement (`0xa211ea0`, under the calc's job scheduler): the position (actor +0x278, last frame's at
  +0x284) moves by the move speed along the facing, times the actor's scale (+0x1608: not a time scale, `0x960b950`
  sets it, at least 0.001). A distance per update, with no time in it. Found with gdb watchpoints from the model's
  matrix (`0x863c15c` copies the actor's matrix into its model) back to the actor's position.
- Where the game uses time, 30 Hz is a constant in the code: 1/30 at 21 sites (e.g. `x += v * (1/30)` at `0x96548e0`,
  a choice of 0.1 or 1/30 at `0x8604828`, `{1/30, count}` handed to a system every frame at `0x86ba000`), 1/60 at 7,
  30.0 at 271 (seconds to frames and back). Integer frame counters (timers, fades, waits) don't show up in such a
  count at all.

So 60 fps would mean halving every actor's movement, every animation's frame step, every frame-counted timer (an
integer can't be halved: it would have to tick every other frame), the effects (Vfx2), the UI's animations and the
camera's per-frame smoothing (a lerp factor per frame isn't linear in the step). Each would have to be found one by one
in a stripped 66 MB binary, and a missed one shows only as something at double speed somewhere in a day of play. Not
worth it. `HOS_PRESENT_INTERVAL` stays as a debugging switch.

## Done: faster loading (measured; the play-report poll, AES-NI, a renderer lock)

The boot to the title took ~21 s (the iris; "Press A" ~5 s later), now ~14 s. Measured on the day-1 save, headless,
with `HOS_LOG_TIME=1` and screenshots every 0.5-1 s (repeated runs agree within ~0.3 s), per-thread CPU from `/proc`,
`HOS_SAMPLE`, `HOS_PROFILE` and `HOS_LOG_SLEEPS` (now also waits whose timeout ran out). Most of the time is the game's
own; what was cut:

- **The play-report poll** (patched, the user's choice: `oranges/patches.py`, `0xbe26ecc`). At each step of loading or
  writing its save, SaveProc queues a play report, then polls every 0.5 s (`0xbe26e50`) until the PlayReporter thread
  has sent it; the reporter takes ~30 ms, but the first check comes right after the push, so each step slept 0.5 s:
  14 steps, 7 s of the boot (the console pays the same). Now every 3.9 ms. A save written with it loads again.
- **AES on AES-NI** (`oranges/insn.py`, chunks built with `-maes`). suyu's C did AESE/AESD/AESMC/AESIMC a byte and a
  bit at a time; the save goes through nn::crypto's AES-CTR: ~1.5 s of SaveProc for each save written or loaded,
  ~0.3 s at the boot. A save made mid-day now loads onto the island ~2 s sooner.
- **Texture registrations off `big_lock`** (`vk_texture.cpp`). The game registers ~500-650 textures a second, always,
  under a lock of its own around `nvnTexturePoolRegisterTexture` (`0xb7fc8e0`), and our registration waited for each
  submit's replay and GPU wait, so MainThread did too (42% of its time before the title, 24% in play). Not a shorter
  load (the game's waits set those), but the frame loop no longer waits for the renderer.

Where the time goes now:

- Boot: setup and ~1,900 files to ~4.5 s (SaveProc decodes the save, ~1.5 s of one core); the island loads ~8-9.5 s;
  the title scene is up at ~9.3 s, and its event flow (`System_Title.bfevfl`: `EventFlowActionWaitFrame`, fades by
  speed) holds ~140 frames behind the loading icon before the iris at ~14.3 s. Frame-counted (the same frame count
  unpaced, where only ~66 fps run there), so left as it is.
- Title to island: ~3 s on a save made at the start of a day (the island is loaded at boot: the title is drawn on it),
  ~12 s on one made mid-day (a ~9 s loading screen: the save written again, then the island loaded).
- A door (the tent): the door's animation ~2.5 s after A, then ~2.7 s of black. The loading in it (~1.7 s) is the
  game's pipeline: no loader is more than ~50% busy (ResLoad1 waits for work 79% of the time, SceneLoad polls with
  1 ms sleeps at `0xa807b4c` and waits on light events); file reads are instant, our renderer isn't in its way.
- The loading screen stays at least 3 s (`0xad29f0c`: 3000 ms since a tick at `[obj+0x358]` / `+0x360`). At ~0
  (tried) it saved ~0.5 s at boot and nothing on the mid-day load: not patched (it is something the player sees).
- Ours that is left (none of it on a load's critical path, now under the renderer's performance): program translation
  (679 ms for the 201 programs by the island, on the submitting thread under `big_lock`), memory pool imports (4,245
  by the title, 0.56 s on the loader threads, every pool but one GPU-visible; 1,270 replace an older one behind a
  `vkDeviceWaitIdle`), other `big_lock` waits from game threads (~0.3 s over the boot).

Left over from it:

- [x] Program translation and ASTC off the submitting thread, pools freed without `vkDeviceWaitIdle`: "Done: the
      renderer's time measured"
- [ ] Lifted code's own speed is what's left in the CPU-bound parts (the save's decoding, the island's load across
      ~6 threads): the chunks are built at `-O1`. Try `-O2` on the hot chunks if loads matter again.
- [ ] A shader cache on disk, if weaker hardware stutters: under "Future ideas".

## Done: saves in the user's data folder

The save outlives the build folder now: the user folder is `$XDG_DATA_HOME/oranges` (when absolute, as the XDG spec
asks), else `~/.local/share/oranges` (`%APPDATA%\oranges` in a branch for the Windows build to come), and the device
save is `<user>/saves/01006F8002326000/device` (the program id from `main.npdm`), so another game or build would not
mix with it. `ORANGES_USER` still chooses the user folder (the same layout under it). `hos_fs_init` (`nn_fs.c`, at
boot) chooses the folder and logs it (`fs: the save folder is ...`, with "(no save yet)" when it is empty). When it
holds nothing and the old default `work/user/save/device` holds a save (not with `ORANGES_USER`), that save is copied
over once: into `device.part`, renamed into place when complete, the old one left as it was. A copy that fails stops
the program with the reason, rather than starting a new game whose first save would keep the old one from ever being
moved. Checked headless with a scratch `XDG_DATA_HOME`: the copy is identical (`diff -r`) and loads to the title
screen; a second boot copies nothing; `ORANGES_USER` on an empty folder starts fresh; a stale `device.part` and an
empty `device` folder are handled; an unwritable data folder stops with "Permission denied". The player's own save is
still only in `work/user/save`: the first run without `ORANGES_USER` moves it. `work/user/save_day1` stays (a test
fixture: copy it to `<dir>/saves/01006F8002326000/device` and run with `ORANGES_USER=<dir>`).

Left over from it:

- [ ] Other user files by the same rules when they come: a shader cache on disk (a future idea) in
      `$XDG_CACHE_HOME/oranges/`, screenshots (task 3) in the data folder.
- [ ] Deleting `work/user/save` once the moved save has been played on is the player's call (nothing reads it again
      while the new folder holds a save).

## Done: the renderer's time measured, and its waits cut

`HOS_VK_STATS=<s>` (new) shows where the back end's time goes (translation, shader creation, fills, ASTC, pool imports,
draws, descriptors, GPU waits) every few seconds, and logs each frame whose back-end work took 10 ms or more. Measured
headless on the day-1 save (boot, title, the island), an RTX 5080:

- **Render conditions without GPU round trips.** The UI tests ~100 `SetRenderEnableConditional` a frame, and each
  flushed the recorded work and waited for the GPU (98 submits and waits a frame). Their counters are the game's CPU
  writes, or counter reports queued just before, whose values are ours. Now the replay tracks the guest memory the
  unrun work writes (buffer copies, updates, fills, written storage buffers; a texture-to-buffer copy counts as
  "anywhere"), flushes only when that overlaps the counters, and applies the queued reports and events to its read as
  `cpu_ops` will. 0 waits a frame now. `HOS_VK_COND_CHECK=1` flushes anyway and compares: no difference in 160k
  conditions. Island frames: back-end time 19 → 17.5 ms, GPU waits 8.5 → 6.2 ms.
- **Programs made ahead, on maker threads.** Each submit is scanned first (state ops only, on a copy of the state), and
  each program a draw or dispatch will need that is new goes to maker threads (`HOS_VK_MAKERS`, default a third of the
  cores, at most 6) with a copy of its draw's state. The replay takes a program when its draw comes: ready, or wait for
  the maker at it, or make it itself if none started it. Shader creation is ~4x eden's translation (~2 ms vs ~0.5 ms a
  program; the driver doesn't cache it across runs). The frame the title scene loads on (~90 programs): 345 → ~165 ms
  of back-end work, and the frames after it (50-100 ms each, more programs) are normal now. The scan costs ~0.15 ms a
  frame. The character creator's compute programs go through it too.
- **ASTC on several threads**: a 2D level over 128x128 is decoded in bands of block rows (~50 ns a pixel on one
  thread: a 774x496 UI texture 19 → 3.8 ms).
- **Memory pools**: the import (pinning the pages, ~0.1 ms each, ~4,250 by the island) runs outside `big_lock`, so the
  game's submits don't wait behind it; an overlapped pool is freed without `vkDeviceWaitIdle` (no GPU work is in flight
  outside `big_lock`: every submit and present runs to completion under it).
- Checked: the title, the island with the manager's announcement and the airport prologue drawn right, 4 runs at once at
  29.9 fps with no crash, the character creator's dispatches.

Left over from it:

- [ ] Per-draw CPU cost, now the bulk on the island (~2,700 draws, ~11 ms a frame): descriptors ~4.5 ms (each texture
      ~0.15 µs: `texture_of`'s mutex, a byte-wise hash of the sampler's parameters for each `sampler_of`, the view's
      key; the uniform buffers and c0/c2 ~0.45 µs a draw), and every dynamic state set again for every draw. Rebuilding
      the c0/c2 banks directly (not word by word) was tried and saved nothing measurable. Set only the state that
      changed, cache samplers by pool id, if a slower machine needs it.
- [ ] Each submit still waits for its GPU work (~6 ms a frame on the island). The design depends on it (fences reached
      at once, CPU-visible results written after it, pools dropped without a wait): overlapping CPU and GPU means
      fences and a retire list.
- [ ] The program key is the code and the inputs, not the texture types the translation asked about: a program is
      translated for its first draw's textures (the views follow the program's types, so the picture stays
      consistent). A cache on disk would need those answers in its key.

## Done: a README

`README.md` (2026-11-27): the vibecoded disclaimer, what a player and a developer need, the GUI and the command line,
controls, saves, the patches to the game (keep that section in step with `oranges/patches.py`) and
troubleshooting. Written from the code; the install commands were not tried on a fresh machine.

Left over from it:

- [ ] Try the from-source steps on a clean system (the Arch package line, a fresh clone and `.venv`), and check that
      the `git clone` URL (taken from the `origin` remote) is one others can reach
- [ ] Keep its Status table current (Windows on real hardware, screenshots, published releases)

## Bugs (found by playing, 2026-11-26)

Before the numbered tasks: each is small and a player meets it.

- [x] **Typing on the keyboard reached the game as a USB keyboard** (2026-11-27). The game takes a USB keyboard as a
      text input (chat, names), so keys bound to Switch buttons typed as well. `nn_hid.c`'s `GetKeyboardState` now
      reports no keyboard (attributes 0, no modifiers, no keys), `GetKeyCode` no character, and `HosInput` has no
      `keys` / `modifiers`. Both HLEs stay, so no lift is needed. The bindings and the F1 menu read SDL's keyboard as
      before. Checked on a headless new game with only pad presses: the player's name is entered with the game's own
      on-screen keyboard and used in the dialogue after it, 29.9 fps up to the flight's announcement
- [ ] Left over: the island name (the campfire party, far into the prologue) not entered again since that change, nor
      physical typing tried by hand in the window
- [ ] **`oranges make --progress json` (the GUI's build) ends with a Python fatal error after the link.** The build
      succeeds (`done: ... (431 s)`, the port runs), then at exit: `Fatal Python error: _enter_buffered_busy: could not
      acquire lock for <_io.BufferedReader name='<stdin>'> at interpreter shutdown, possibly due to daemon threads`.
      The cause: `make.py` `_watch_stdin`'s daemon thread is blocked in `sys.stdin.buffer.read1()` and holds the
      BufferedReader's lock while the interpreter shuts down. Fix: read the raw descriptor (`os.read(0, 4096)`, no
      buffer and no lock), or leave `main` with `os._exit` once the log is flushed. Check that the exit status is still
      0, that the GUI shows the build as done, and that Cancel still works (Linux, and Windows under wine)

## Don't name the game anywhere (2026-11-26)

oranges never names the game it builds. The GUI shows the supported build only by its title ID (`01006F8002326000`)
and version (3.0.3), as `oranges info` / `check` do, and nothing else about it. Everywhere else in the tree the
game's name becomes "the game", in code, comments, strings and docs alike, and so do its characters' names (the
prologue's shopkeepers and the like) and the short forms of its name.

- [x] The tree and the whole history (2026-11-26): every mention in files and commit messages reworded (the GUI's
      welcome text, host/ and oranges/ comments and strings, the docs, `third_party/eden/PROVENANCE.md`), the docs'
      example backup file is `game.nup`, and the history was rewritten so that no commit names it. Tests and ruff pass
      and the Linux GUI builds; the Windows GUI was not rebuilt
- [ ] A test that fails when a tracked file outside `third_party/` names the game, so a new mention is caught (its
      pattern list mustn't name the game in plain text either: hashes of the words, or the names spelled out at run
      time)

## 1. A Windows build (MinGW)

The target is a Windows x64 build of the same program, built with MinGW-w64 so that most of the POSIX code stays as it
is. clang stays the compiler, since the chunks' flags (`-mllvm -rotation-max-header-size=0`) and `-mcx16` / `-maes`
are clang's: llvm-mingw (`x86_64-w64-mingw32-clang`, UCRT) cross from Linux, or MSYS2's CLANG64 on Windows.

Where it is (2026-11-24): `oranges build --target windows` cross-compiles `work/build-windows/oranges.exe` with
llvm-mingw (`/opt/llvm-mingw`, LLVM 23.1), everything included: eden, Dear ImGui and the Vulkan back end compiled
under MinGW's libc++ without a change. Under wine 11.22 (its Vulkan reaching the host's NVIDIA driver) it does what
the Linux build does, at 29.6-30 fps: the day-1 save to the title and onto the island (the manager's announcement),
drawn right, headless and in a window (Vulkan swapchain, sound through WASAPI, the F1 menu with its font, `HOS_CONTROL`
through the named pipe from wine's `cmd`), and a new game to the opening movie (all 1960 pictures decoded by FFmpeg,
the end of stream as on Linux). winpthreads and libc++ are linked in; the build copies the DLLs it needs beside the
exe (SDL3, libopus, libiconv, avcodec, avutil; not `vulkan-1.dll`: the driver's, or wine's, is the one to use) with
their licence texts in `licenses/` (oranges' own GPL too).

The libraries: `python -m oranges.mingw` makes `~/.local/opt/msys2-clang64` (the build finds it there, or by
`ORANGES_MINGW_PREFIX`): MSYS2's CLANG64 packages (SDL3, the Vulkan loader and headers, opus; clang, libc++ and UCRT
like llvm-mingw), checked against the repository database's SHA-256s, and FFmpeg 9.1 built from its release
(signature checked, SHA-256 pinned) with only libavcodec / libavutil and the VP8, VP9, H.264 decoders (MSYS2's
FFmpeg pulls in 124 packages), with its assembly (nasm built from source for it). ~37 s from nothing.

The differences live in `host/hos_port.h` / `hos_port.c`:

- the guest window: `VirtualAlloc` reserve at the fixed address, commit / decommit per mapping (Linux: `mmap`,
  `mprotect`, `madvise`); the image is read into its segments (no `mmap` of the file);
- winpthreads gaps the kernel would have hit: `pthread_condattr_setclock(CLOCK_MONOTONIC)` fails and a timed wait
  takes its deadline as wall-clock time (every timed kernel wait would have timed out at once: probed under wine), and
  `nanosleep` sleeps whole milliseconds (0.2 ms: none). `hos_cond_wait_until` turns the monotonic deadline into the
  time left; `hos_sleep_ns` is a high-resolution waitable timer; `timeBeginPeriod(1)` at start (Sleep and
  winpthreads' waits tick at 15.6 ms otherwise). Every `nanosleep` in host/ is `hos_sleep_ns` now;
- files: `hos_pread` / `hos_pwrite` (`ReadFile` / `WriteFile` with an offset), `O_BINARY` on every `open`,
  `hos_rename` (`MoveFileEx`, replacing), `hos_mkdir`, `hos_ftruncate`, `-D_FILE_OFFSET_BITS=64` (MinGW's `off_t`
  and `st_size` are 32 bits otherwise), `hos_exe_path` (`GetModuleFileNameW`), both path separators in `mkdirs`;
- time: `localtime_s` / `gmtime_s` / `_mkgmtime`; the zone's name is its offset ("+01": Windows names zones in words);
- native crashes: `SetUnhandledExceptionFilter` (not a vectored handler: that sees the exceptions GPU drivers and C++
  raise and handle themselves) with the same guest registers and backtrace;
- `HOS_CONTROL`: a named pipe `\\.\pipe\<the path's last name>` (Linux: the FIFO);
- `host/win/oranges.manifest` (compiled in by `oranges.rc`): UTF-8 as the code page (paths, `getenv`, `argv`), long
  paths. The options file is `%APPDATA%\oranges\oranges_options.ini`, beside the saves.
- LLP64: the host C has no bare `long` used as 64 bits and no `-Wformat` warnings under MinGW.

- [x] The libraries for MinGW, the C++ under MinGW, and runs under wine with the window, sound, renderer and movie
- [ ] Run it on Windows itself: boot the day-1 save to the island with the window, sound, a gamepad and the F1 menu,
      with the save in `%APPDATA%\oranges\saves\01006F8002326000\device`. `VK_EXT_external_memory_host` is on
      NVIDIA's and AMD's Windows drivers
- [x] The Python side on Windows, under wine (2026-11-24): `python -m oranges.wintools` (new) fetches the embeddable
      Python 3.14.10 (with lz4's wheel) and llvm-mingw's Windows release (LLVM 23.1.6), SHA-256s pinned, into
      `~/.local/opt/oranges-wintools` (955 MB; on Windows `%LOCALAPPDATA%\oranges\wintools`, as the library
      prefix is `%LOCALAPPDATA%\oranges\msys2-clang64` there), with a `.pth` naming this checkout. With them, under wine:
      `oranges lift --write` (94 s; the kit builds `a64emit.dll` with that clang++, static) writes the same C as the
      Linux lift, byte for byte (oranges' own generated files are written with `\n` now; the kit's `functions.txt`
      and `image.json` stay CRLF on Windows), and `oranges build` with that clang (the chunks ~12 min under wine,
      the rest ~30 s) makes an `oranges.exe` that boots the day-1 save to the title in a window, with sound, at
      30 fps. The libraries' `.pc` files are read by `oranges/pkgconf.py` (new) now, not pkg-config, for the prefix:
      a build on Windows needs no MSYS2, and the prefix may move (FFmpeg's `.pc` files hold absolute paths, which
      `--define-prefix` left as they were). Recipe in CLAUDE.md
- [ ] The same on Windows itself (the lift's and the build's times there; MSYS2's CLANG64 shell is the other way,
      untried)
- [x] Tools found on Windows without environment variables (2026-11-27, from a first try in a Windows VM, where
      `oranges.mingw` stopped looking for `/opt/llvm-mingw`): `oranges.mingw_dir` takes wintools' llvm-mingw,
      `oranges.kit` hands it to the kit (`LONGSHOT_MINGW`), and `oranges.mingw` leaves FFmpeg out on Windows (its
      configure and make can't run there) unless the prefix holds one. Checked under wine with the Windows Python: the
      compilers found, `a64emit.dll` built, the setup GUI built. Not yet on Windows itself
- [ ] FFmpeg for a build made only on Windows (prebuilt DLLs, or building it in an MSYS2 shell): without it the movies
      are skipped. Check that the port gets past the opening movie without FFmpeg
- [ ] A fresh Windows fails the downloads with `CERTIFICATE_VERIFY_FAILED` (seen from GitHub's asset server: Python
      doesn't make Windows fetch a missing root). The README has the `curl.exe` workaround; the downloaders could fall
      back to `curl.exe` (the SHA-256s are checked either way)
- [x] Shipping, for players: a GUI subsystem build (no console window opens with it). Its log (`hos_log_open`) goes
      where stderr was sent (a redirect, a pipe, a terminal: MSYS2's, wine's), else to the command prompt it was
      started from (`AttachConsole`), else to `<user folder>\oranges.log` (`%APPDATA%\oranges`; the run before's kept
      as `oranges.old.log`). With the log in that file, a fatal error or a crash shows a message box with the reason
      and the log's path (`hos_port_stopped`): a player who started it from the desktop would see it vanish otherwise.
      Checked under wine (`wine start /wait /unix` starts it without std handles): the file, its rotation, the box
      (its text in `WINEDEBUG=+msgbox`), the window and a headless run as before. A lost or hung GPU device is a fatal
      error now too (it was `abort()`).
- [ ] The MSYS2 database's PGP signature is not checked (HTTPS only)
- [ ] Gamepads and the keyboard by hand in the window (only `HOS_CONTROL` drove the wine runs), and SDL3's gamepad
      backends on Windows (XInput, RawInput, Steam Input's)
- [x] Small things: `HOS_CONTROL MEM`'s file is sparse on NTFS (`FSCTL_SET_SPARSE`; `hos_ftruncate` moves the end
      of file with `SetFileInformationByHandle` now, where `_chsize_s` wrote 4 GB of zeros: under wine 954 MB on disk
      for the 4 GB file); each thread's sleep timer is closed when it ends (a pthread key's destructor)
- [ ] A host stack overflow (a guest call chain too deep for its 8 MB host stack) on Windows itself: the threads that
      run lifted code keep 64 KB back for the handler (`hos_thread_start`, `SetThreadStackGuarantee`), which logs
      it by name. Unverifiable under wine: it ends the thread silently before any handler runs
      (`err:virtual:virtual_setup_exception stack overflow`), guarantee or not (a test program showed it)

## 2. A downloadable oranges with a graphical setup (build the port without a terminal)

What oranges is for, as Longshot is: a player downloads an oranges release, runs its GUI (`oranges.exe` on Windows,
an executable on Linux), points it at their own `.nup` (the exact supported build for now: `game.check`), and gets
their own PC port of the game built on their machine. The release comes with everything the build needs: the player
installs no toolchain, no Python, no libraries, and needs no terminal. The release holds nothing from the game; the
game's code and data only ever come from the player's own copy (CLAUDE.md's game data rules).

Today the same build takes a developer's setup: a Python 3.14 venv, clang (llvm-mingw for Windows), pkg-config and the
libraries (`python -m oranges.mingw` for Windows), then `oranges check`, `oranges extract --only romfs`,
`oranges lift --write`, `oranges build`, and running `work/build/oranges`. That is ~12.5 GB on disk (RomFS 10 GB,
generated C 1.4 GB, objects 0.8 GB) and ~2.5 GB of memory per compile job.

Where it is (2026-11-24): the pipeline is one command, the GUI exists, and the Linux release builds the port with
nothing of the system's: from its archive, in a clean Ubuntu 22.04 container, a port that boots here at 30 fps with
its window and sound. The Windows release does the same under wine: its pipeline builds `oranges.exe` (14.4 min), which boots at 29.9 fps.

- [x] The pipeline in one command: `oranges make <nup> --dir FOLDER [--jobs N] [--clean] [--progress json]`
      (`oranges/make.py`): check the build, check the room (what is still missing: RomFS files, the C, objects),
      extract, lift, compile, link, into FOLDER with the work layout (the program finds its image and RomFS beside
      it, as before). Resumable: extracted files kept, the lift skipped while `gen/.lifted` (a hash of the exefs and
      the lift's modules, the kit and host/) matches, objects cached. `--progress json`: one event per line (stages,
      progress by bytes with the chunk count, log, done, error by kind), and the end of stdin cancels (running
      compiles killed). `FOLDER/make.log` keeps every run's output, `FOLDER/oranges.json` what was built. Measured on
      `work`: 6.4 min from a stale build (all 267 objects), 77 s with a new lift and cached objects, 2 s with nothing
      to do; the program boots at 30 fps
- [x] The GUI is a native program on SDL3 and Dear ImGui (`gui/oranges_setup.cpp`, ImGui's SDL3 and SDL_Renderer
      backends vendored from the same release; `python -m oranges.gui` builds it into `work/gui/`, or
      `work/gui-windows/` with `--target windows`). It starts without Python and runs the pipeline as a child process
      (`<python> -m oranges make ... --progress json`; pythonw.exe on Windows, so no console window opens). In a
      checkout it uses `.venv`; in a release, the release's `python/` and `src/` (it looks above itself for either)
- [x] The steps, one page each: Game (the `.nup` files in Downloads, Desktop, Documents, Games and the home folder,
      or a file dialog; `oranges check` says whether it is the supported build, or why not), Folder (free space
      against the ~12.5 GB needed), Options (build jobs from the memory size, deleting the build's files afterwards,
      Console mode written to `oranges_options.ini`), Build, Play. Its own settings: `setup.ini` in the user folder.
      `--nup`, `--dir`, `--page`, `--build` start it somewhere; `--shot` saves its picture (for checks)
- [x] The build with progress: each stage with its state, a bar and the time left (by bytes), the whole build's
      bar, the log underneath, Cancel (stdin closed; killed after 20 s) and Continue (it resumes). Errors from
      make's kinds in words (wrong build, no room, out of memory: fewer jobs), the log's path for a report
- [x] After the build: Play (the program started detached), a shortcut (`~/.local/share/applications/
      oranges.desktop`; on Windows a Start menu `.lnk`), Open the save folder / the build folder, delete the
      build's own files (gen/ and the objects, 2.2 GB). A release newer than the one that built the port says to
      build again (saves and options are in the user folder, untouched)
- [x] The release, one per platform (`python -m oranges.release linux|windows`, `oranges/release.py`):
      `oranges[.exe]` (the GUI), `release.json`, `src/` (the files git tracks under oranges/, host/, gui/,
      third_party/, so nothing from work/ can go in, with VERSION and the translator library prebuilt), `python/`,
      `toolchain/`, `sysroot/` (Linux), `libs/`, `SHA256SUMS` (without bytecode; `release verify <folder>` checks
      it), and a `.tar.zst` / `.zip` beside it. In a release `oranges.bundled` points the pipeline at those tools
      (CC, CXX, ORANGES_SYSROOT, ORANGES_PREFIX; on Windows ORANGES_MINGW, ORANGES_MINGW_PREFIX, LONGSHOT_MINGW).
      `oranges.build` reads a prefix for Linux too (ORANGES_PREFIX); with a sysroot it builds C++ on LLVM's libc++
      and links libc++, libc++abi and libgcc in, `$ORIGIN` as the library path, the libraries' licence texts beside
      the program. Object stamps name the sysroot, prefix, work folder and tree by placeholders, so moving a release
      or a build folder recompiles nothing
  - Linux (658 MB, archive 178 MB): `python -m oranges.linuxkit` (`~/.local/opt/oranges-linuxkit`, ~5 min from
    nothing) makes LLVM 23.1.6's own Linux release pruned and stripped (clang, lld, clang's headers, libc++: 390 MB;
    runs on glibc 2.34+ with the system's zlib and libstdc++), two sysroots from Ubuntu 22.04's frozen release
    pocket (index SHA-256 pinned, packages by dependency closure, absolute links made relative): `sysroot/` for
    players (34 packages, 81 MB: glibc 2.35, the Vulkan loader) and `build-sysroot/` (184 packages: what SDL3 is
    built against; its build reads each library's real .so for the name it loads), python-build-standalone's
    CPython 3.14.10 with lz4, and static SDL3 3.4.20 (X11, Wayland, PipeWire, PulseAudio, ALSA, hidraw; libusb off),
    FFmpeg 9.1 (the movie's decoders), Ubuntu's libopus, libatomic. eden needs `<format>`, which libstdc++ 12 lacks:
    hence libc++ (checked: it needs nothing of glibc newer than 2.35)
  - Checked: the archive unpacked fresh verifies and its GUI starts and checks the game; in podman's ubuntu:22.04
    (no compiler, no Python; the release and the `.nup` mounted read-only) `python/bin/python3 -m oranges make`
    extracts, lifts and builds the port (7.5 min); the port needs only libc, libm and libvulkan.so.1 (GLIBC_2.35),
    and here it boots at 30 fps headless, as the dev build, and with a Wayland window, Vulkan swapchain and PipeWire
    sound. The release's GUI drives a build (it found its own Python and tools); quitting it mid-build cancels the
    pipeline (its compiles killed, "cancelled" in make.log); a folder without room is refused before anything is
    written
  - Windows (643 MB): oranges.wintools' embeddable Python (`..\src` in its `._pth`) and llvm-mingw without its
    other targets, oranges.mingw's prefix as `libs/`, the GUI and `a64emit.dll` cross-compiled (static) with
    llvm-mingw, SDL3.dll beside the GUI. Under wine its GUI runs pythonw through pipes: the check, a build's stages
    and log, cancel on quit. With its own Python and llvm-mingw (`python\\python.exe -m oranges make`, the RomFS
    already there) it lifts and builds the port in 14.4 min under wine; that `oranges.exe` (SDL3, opus, FFmpeg
    DLLs and licences beside it) boots at 29.9 fps. Archive: a .zip beside the folder
- [ ] Checksums signed for each release (SHA256SUMS exists, unsigned); the GUI could run `release verify` at its
      first start
- [ ] The Linux toolchain needs the player's zlib and libstdc++ (every desktop distribution has them; a container
      image may not); the release's own Python and clang are untested on distributions other than this one and
      Ubuntu 22.04 (Fedora, Arch, SteamOS: try them in containers)
- [ ] The Windows release on Windows itself (task 1's open item), and its first lift there builds nothing (the
      translator is prebuilt): check it
- [ ] Where releases are published and how the GUI learns of a new one (none yet). Updating oranges never downloads
      anything of the game.
- [ ] Smaller: read the RomFS from the `.nup` in place rather than extracting 10 GB (`Nup` streams files; check
      whether random access into them is cheap enough for `nn_fs.c`).
- [ ] Never: the release or the GUI downloads or uploads anything from the game. The setup reads the player's own copy
      only.

Why here: it is what oranges is meant to be (a player builds their own port, as with Longshot), and it needs task 1
for its Windows half: the Windows build itself (done under wine), and next the toolchain and libraries as files to
ship rather than this machine's `/opt/llvm-mingw` and `~/.local/opt/msys2-clang64`.

## 3. Screenshots

A player can't take one: on the console the Capture button is the system's job, and the game's own saves
(`nn::album`, `host/nn_online.c`) are logged and dropped. Only test runs have them (`HOS_CONTROL`'s `SHOT`,
`HOS_SCREENSHOT`, `HOS_VK_SHOTS`: BMP files, `host/vk/vk_device.cpp`).

- [ ] Capture: a bindable key and gamepad input (`hos_controls.c`; "Share / Capture", SDL's `misc1`, as the gamepad
      default) that saves the next presented picture, at the picture's own size, without the F1 menu
- [ ] `nn::album::SaveScreenshot` and `SaveAndShareScreenshot` (saved, never posted): write the game's image the same
      way (new HLE bodies only; the symbols are hooked already). Find out where the game calls them
- [ ] PNG files with dated names, in a `screenshots` folder in the user folder (`ORANGES_USER`'s rules); the path in
      the log
- [ ] Check on Linux and Windows (under wine), in a window and headless

Why here: a player expects it, and the renderer already reads the picture back.

## 4. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it (the Resource Memory crash did not: other cores)
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)

Why here: none of these blocks the game; take them on when one starts to matter (memory on long runs first).

## 5. The renderer's gaps

The leftovers under "Done: a renderer" above: indirect draws and the other unreplayed commands when the game first
uses them (logged once), then performance ("Done: the renderer's time measured": the per-draw cost) and memory.

Why last: the renderer is in good shape ("Done: the renderer's time measured": no GPU round trips for the UI, programs
made on threads, 30 fps with room to spare on this machine), and nothing unreplayed has turned up yet (each is logged
once when it does). It moves up as soon as one of those logs or a wrong picture shows, or a slower machine misses
30 fps.

## Future ideas

Not planned yet. Each needs a look at what the game allows first.

- **A shader cache on disk, if weaker hardware stutters.** Each NVN program is translated (eden) and made into Vulkan
  shader objects the first time a draw uses it, ~1.5-6 ms each, on maker threads ahead of the draw (several at
  once; the draw waits only if its program isn't done yet). On this machine (an RTX 5080, a fast CPU) it doesn't
  show. On a slower CPU with fewer cores, a frame that meets many new programs (a new area, a menu) could still wait.
  - The cache: each program's translated SPIR-V written to `$XDG_CACHE_HOME/oranges/` (the user folders' rules),
    keyed by the program's hash and everything the translation asked about (the bound textures' types,
    `vk_replay.cpp:197`; the previous stage; c1), and read back instead of translating; optionally loaded and made
    into shader objects on a background thread at boot.
  - Not ahead-of-time precompiling from the RomFS: a translation depends on draw-time state the files don't hold, so
    a guess would draw wrong. One play fills the cache instead.
  - Measured (2026-11-24): the driver's `vkCreateShadersEXT` is ~4x eden's translation (~2 ms vs ~0.5 ms a program)
    and the driver doesn't cache it across runs. Both now run on maker threads ahead of the draw ("Done: the
    renderer's time measured"). A cache would keep the driver's binaries (`vkGetShaderBinaryDataEXT`, with
    `VK_EXT_shader_object`'s compatibility UUID) besides the SPIR-V. Still to measure: how many distinct programs a
    long session uses.

- **DLSS (and FSR 3.1) with motion vectors we make ourselves.** DLSS Super Resolution is temporal: NVIDIA's API needs
  per-pixel motion vectors, depth and camera jitter, and there is no single-frame mode. Zero motion vectors would smear
  everything that moves (the camera moves whenever the player walks). The game has none of these (see FSR 3.1 under
  "Looked at and ruled out"), but Longshot's toolchain makes them for a game that has none either (`gpu/vk/vk_dlss.cpp`,
  `shaders/dlss_prep.comp`, `dlss_actor.vert`, `temporal_jitter.h` in
  `~/.local/share/longshot/launcher/toolchains/v3.0.0/usr/share/longshot/`):
  - the camera's motion: the depth taken back through this frame's and the last frame's view-projection matrices;
  - characters' own motion: skinned draws drawn again with the last frame's bone poses;
  - jitter added to the scene passes' projection.
  - The gain: the game has no anti-aliasing at all, so DLSS (or DLAA) would anti-alias as well as upscale, clearly
    better than FSR 1 from 1080p to 4K.
  - What it needs here: the camera matrices and the skinned draws found in the game's uniform buffers, and DLSS run in
    the tonemap pass before the first UI draw (the UI is drawn into the same RGB10A2 target).
  - What stays wrong: things that move without a skeleton (swaying trees, water, particles) get the motion of what is
    behind them.
  - Other costs: NVIDIA RTX only, and NVIDIA's closed NGX library loaded at runtime (check it against GPL-3.0; Longshot
    ships it). The same motion vectors would also allow FSR 3.1, which runs on every GPU.
  - First step: read Longshot's `vk_dlss.cpp` and estimate the port. NVIDIA Image Scaling (single-frame, no motion
    vectors) is not worth adding: about what FSR 1 gives now.

## Looked at and ruled out

- **60 fps game logic**, researched 2026-11-24: the world steps once per frame, with 30 Hz assumed system by system
  and no shared time step to halve ("Done: 60 fps researched").
- **FSR 3.1 (temporal upscaling and frame generation)**, checked 2026-11-24 on a frame of the day-1 save
  (`HOS_VK_LOG`): the game draws no motion vectors and has no temporal anti-aliasing (so no camera jitter). The scene is
  one R11G11B10F target with a D32F depth (a depth pre-pass, then the colour pass with EQUAL depth: 800-950 draws each,
  single target), then a linear depth copy (R32F), half-size passes (R32F, R16F, R8, RG16F: a handful of full-screen
  draws each, not per object), a bloom chain (R11G11B10F 960x540 down to 30x17), the tonemap into RGB10A2, where the
  UI is drawn too, and a copy to the RGBA8 window texture. FSR 3.1 needs per-pixel motion vectors, the jitter and a
  picture without UI; frame generation needs the same motion vectors. They could only be faked: camera-only motion
  vectors from depth and the view-projection matrices (found in the uniform buffers), wrong for everything that moves
  (villagers, the player, swaying trees, water), jitter patched into the projection of the scene passes only, the UI
  split out of the final target. Too fragile for what it would give; FSR 1 instead (done).
