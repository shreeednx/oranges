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
      for each draw, programs are translated on the game's thread. Profile once more of the game draws
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

- [ ] Listen to it with the window (checked by level only: `HOS_AUDIO_DUMP`). The other sub mixes that were silent
      play now too (6 and 8 send 0.125 / 0.25 to a bus and the final mix, the look of reverb sends): check that the
      island's mix sounds right
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

## 1. Faster loading

The boot to the title screen and the title screen to the island (~30 s here) are long, and every scene change (a
building, the airport) waits too. Only what oranges adds can be cut without changing the game, so first find where the
time goes.

- [ ] Measure: time the boot, the title to the island, and a door (into the tent and out) on the day-1 save, from the
      log (`HOS_FS_LOG` shows what loads when). Several runs each: the numbers move with the disk cache.
- [ ] Profile the loading threads while it loads (`HOS_PROFILE <s> <thread>`, `HOS_SAMPLE`): file reads and
      decompression (`nn_fs.c`, the game's own zstd / zlib), shader translation (eden, on the game's submitting
      thread, `vk_shader.cpp`), texture deswizzling and ASTC decoding (`vk_texture.cpp`), the whole-storage texture
      hashing, our GPU waits at each submit.
- [ ] Cut what is ours: a shader cache on disk (translated SPIR-V by program hash, so a second run translates
      nothing), translation and ASTC decoding off the game's thread (or ahead of the draw that needs them), texture
      uploads batched rather than one submit each.
- [ ] The game's own pacing (fixed waits, frame-counted fades, loads it spreads over frames at 30 fps) stays as it is;
      write down how much of the time it is, so it is clear what is left.

Why first: felt on every start and every door, and its first step is only measuring; the shader cache and work off the
game's thread are also renderer performance (task 3).

## 2. Saves in the user's data folder

Saves go to `<work>/user/save/device` today, beside the build (`hos_main.c`: `work/build/oranges` -> `work/user`;
`ORANGES_USER` overrides). A save should outlive the build folder and sit where other games keep theirs, as the settings
file already does (`hos_options.c`: `$XDG_CONFIG_HOME/oranges/`, else `~/.config/oranges/`).

- [ ] The default user folder: `$XDG_DATA_HOME/oranges/`, else `~/.local/share/oranges/` on Linux; `%APPDATA%\oranges\`
      once there is a Windows build (task 4). Saves under a folder named by the title id
      (`.../saves/01006F8002326000/device/`), so a second build or game would not mix with this one. Made at the first
      write, as the options file's folder is. `ORANGES_USER` still overrides (runs on a copy of a save use it).
- [ ] Moving over: when the new folder has no save and `work/user/save` has one, copy it once (never move it or
      overwrite a save that is there) and log both paths. The save there now has been played past day 1;
      `work/user/save_day1` stays where it is (a test fixture, not a player's save).
- [ ] The log names the save folder at boot, so a player can find it.
- [ ] Other user files by the same rules: a shader cache on disk (task 1) in `$XDG_CACHE_HOME/oranges/`, screenshots
      (`nn::album`, a leftover above) in the data folder.
- [ ] CLAUDE.md: the new default and `ORANGES_USER`.

Why second: small and felt by anyone who rebuilds or moves the build folder, but nothing is lost today as long as `work/`
stays.

## 3. The renderer's gaps

The leftovers under "Done: a renderer" above: indirect draws and the other unreplayed commands when the game first
uses them (logged once), then performance and memory.

Why here: nothing unreplayed has turned up yet (each is logged once when it does), and 30 fps holds so far. It moves up
as soon as one of those logs or a wrong picture shows.

## 4. Clean-ups and checks

- [ ] `host/nvn_native.c`: the game's threads share control memory between command buffers. The grace period
      (`RETIRE_NS`) fixes stale handles. Check that memory stays bounded on long runs.
- [ ] Build the host code with warnings on (it uses `-Wno-everything` along with the lifted C today)
- [ ] A scheduler that honours priorities (core numbers are reported now, but every guest thread still runs freely on
      its own host thread), if a wall turns out to depend on it (the Resource Memory crash did not: other cores)
- [ ] Tests for the host side where they can run without the game (NVN object layouts, the audio mixer's buffer
      release, the `nn::time` calendar conversions)
- [ ] Windows build (POSIX-only today: pthreads, `mmap`, signals)

Why last: none of these blocks the game; take them on when one starts to matter (memory on long runs first).

## Future ideas

Not planned yet. Each needs a look at what the game allows first.

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
