<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# How the host is put together

The host implements [protocol version 1](protocol.md) with the `ps1` and
`ps1Lockstep` capabilities. It is a small C++17 program with three threads:

| Thread | Does |
| --- | --- |
| Control (`main`) | Reads each message, answers calls and pings in order, and enforces the connection's order rules |
| Game | Started once the `start` reply is sent. Opens the core and the disc, then `retro_init`, `retro_load_game` and `retro_run` paced to the core's frame rate, or in lockstep one `retro_run` per step. Publishes frames, sends `ring`, `frame`, `rumble` and `sync`, and saves and loads states between frames. It marks each frame (`in_frame_`), and the control thread's `lockstep` calls wait on a condition variable for the frame to end, so the change falls between two frames |
| Watch | Every 100 ms: the 5 s write limit, the stall and first-frame game limits, the `stop` limit, and the 10 s limit for leaving |

On Linux and macOS a fourth thread waits for `SIGTERM` (sent by
`PR_SET_PDEATHSIG` on Linux), and on macOS a fifth for the launcher's exit
through kqueue. On Windows a thread waits on the launcher's process handle.

## Files

| File | What it holds |
| --- | --- |
| [`src/main.cpp`](../src/main.cpp) | The command line, the process setup, then the host |
| [`src/host.cpp`](../src/host.cpp) | The protocol: `hello`, the order rules, every method's argument checks and replies, and the limits |
| [`src/game.cpp`](../src/game.cpp) | One loaded game: the libretro callbacks, core options, disc control, the memory card, input, rumble, pixel conversion, and lockstep: the queue of steps, the `sync` hashes, and the state files of `saveState` and `loadState` |
| [`src/resolution.cpp`](../src/resolution.cpp) | Which internal resolutions a GPU game is offered (see "Resolution limits" below) |
| [`src/ring.cpp`](../src/ring.cpp) | The frame ring writer: the header from protocol.md's byte table, slot choice, and capped doubling when a frame needs more room |
| [`src/json.cpp`](../src/json.cpp) | A strict JSON reader (duplicate keys, lone surrogates, invalid UTF-8 and non-finite numbers are errors) and writer |
| [`src/paths.cpp`](../src/paths.cpp) | The path rules of protocol.md "Paths" for Windows and POSIX |
| [`src/channel.cpp`](../src/channel.cpp) | Framing for the control stream, and the clock the watch thread reads for the write limit |
| [`src/platform_posix.cpp`](../src/platform_posix.cpp), [`src/platform_windows.cpp`](../src/platform_windows.cpp) | The control stream, process setup, watching the launcher, shared memory, loading the core and files |
| [`src/audio.cpp`](../src/audio.cpp) | Sound output through miniaudio, with the volume, mute and pause |
| [`tools/package_release.py`](../tools/package_release.py) | Packs, lists and checks a release (see "Releases" below) |

## Choices made for version 1

- **GPU rendering.** The host answers SwanStation's request for an OpenGL
  3.3 core context (`RETRO_ENVIRONMENT_SET_HW_RENDER`; `GET_PREFERRED_HW_RENDER`
  says OpenGL core) with an offscreen context on the game's thread: EGL on
  Linux (loaded at run time), WGL with a hidden window on Windows, and CGL on
  macOS ([`src/gpu.cpp`](../src/gpu.cpp) and one file per system). The core
  draws into the host's framebuffer, which grows with the core's largest
  geometry; each frame is read back with `glReadPixels`, turned the right way
  up, and published to the ring like a software frame. `status.hardware` is
  then `true` and the internal resolution option works, within the limits
  below. On macOS the host asks
  CGL for an accelerated renderer (`kCGLPFAAccelerated`) first and, only when
  there is none, for Apple's software renderer
  (`kCGLPFARendererID` = `kCGLRendererGenericFloatID`, "Apple Software
  Renderer" in `CGLRenderers.h`, OpenGL 4.1 core). A Mac whose GPU has an
  OpenGL renderer takes the first; a Mac virtual machine whose paravirtual GPU offers Metal but
  no OpenGL renderer takes the second. GitHub's `macos-latest` runner was such a
  machine when checked in November 2026: its only CGL renderer is `0x01020400` (`kCGLRendererGenericFloatID`
  under `kCGLRendererIDMatchingMask`), not accelerated, and asking for
  acceleration fails with `kCGLBadPixelFormat`. This matches Linux, where EGL
  takes Mesa's software rasterizer when there is no GPU driver, so on all
  systems `status.hardware` means the core draws with its OpenGL renderer,
  whether a GPU or the CPU runs it, as protocol.md "status" defines it. The software renderer keeps the internal
  resolution option and the core's OpenGL accuracy settings working; the
  host does not prefer SwanStation's own software renderer over it, as no
  system has a way to know which is faster on a given machine. When no context
  can be made, the host turns the request down and SwanStation draws in software;
  `resolutionError` then says a scale above 1x needs an OpenGL context, and keeps saying
  so after that game ends. A context that was
  made but then fails ends the game with `errorCode` `other`: when no framebuffer
  can be made for the core's largest size, when a later geometry change cannot
  get a bigger one, or when reading a frame back reports an OpenGL error (a lost
  context among them) or the frame is larger than the core said it could be.
  The game never keeps running on stale pictures. The renderer
  option stays at `Auto` and is not listed, so the host decides. As RetroArch
  does, the core's `context_destroy` runs before `retro_unload_game`. The
  hidden WGL window is a top-level window, so Windows and other programs send
  it messages (a `SendMessage` to `HWND_BROADCAST` waits for every top-level
  window); the game's thread answers them between frames (`Gpu::Pump`), so it
  never shows as hung, and the window ignores `WM_CLOSE`, which `taskkill`
  without `/f` sends and which would otherwise destroy the window under the
  context. On Linux the context is dropped on the game's thread, which ends
  with `eglReleaseThread` before libEGL is unloaded: libglvnd before 1.5
  (Ubuntu 22.04 has 1.4) keeps the thread's EGL state under a pthread key
  whose destructor is in libEGL and leaves it set when unloaded, so the thread
  would crash as it ended, right after SwanStation switched to software at
  `stop`. [`tests/gpu_thread_test.cpp`](../tests/gpu_thread_test.cpp) makes
  and drops a context on two threads that end in turn, and is skipped where
  no OpenGL 3.3 core context can be made.
- **Resolution limits.** SwanStation's internal resolution goes up to 16x, and
  its largest geometry is PS1 VRAM, 1024 × 512, times the scale
  (`GetSystemAVInfo` in `src/libretro/libretro_host_interface.cpp`): 16384 ×
  8192 at 16x. Its frames are the screen's size times the scale, so a 640-wide
  screen, such as the Sony BIOS logo, is 10240 pixels wide at 16x. Measured on
  Windows with an NVIDIA GPU (largest texture 32768): Crash Team Racing set to
  16x ended at the BIOS logo with `frameTooLarge` before its first frame, ran at
  11x (7040 × 5258) and 12x (7680 × 5736), and the OpenBIOS test program, which
  shows a 320-wide screen, ran at 16x (5120 × 3840). A GPU whose largest
  framebuffer is smaller than the core's largest geometry ended the game at
  start with `other` instead. So, once the core has given its largest geometry
  for the value it was given and the context is made, the host offers only the
  scales whose largest geometry, the reported one times scale / that value,
  fits within 8192 × 8192 and the context's `GL_MAX_RENDERBUFFER_SIZE` and
  `GL_MAX_VIEWPORT_DIMS`: 1x to 8x with SwanStation on any GPU that can draw
  8192 × 8192. libretro promises no frame is larger than the largest geometry,
  so no offered scale can end a game with `frameTooLarge`; 9x to 12x, which
  only some screens can use, are not offered. A start value that does not fit
  is changed to the largest that does, before the core's first frame (the host
  reserves the framebuffer for that one, calls `context_reset`, and the core
  takes the new value at the first `retro_run`, as it does an `option` call);
  `resolutionError` says which value was asked for, how large its pictures could
  be, the limit, and the value used, until the player chooses an internal
  resolution again. When the player chose a value that fits while the core was
  loading at one that does not, the core is given the chosen one in the same
  way, without a `resolutionError`. SwanStation still makes its renderer at the asked-for scale
  in `context_reset` (it clamps to the GPU's largest texture itself) and
  remakes it a frame later. The values the core gives before it reports its
  geometry are listed whole. Values that are not whole numbers cannot be
  judged and stay offered, and when no value fits nothing changes and the game
  ends as before.
- **Options.** The host reports core options version 2 and takes the
  definitions the core gives (`SET_CORE_OPTIONS_V2` or `_V2_INTL`, English
  text). `start.options` sets the starting values. An `option` call is
  applied when the core next asks for `GET_VARIABLE_UPDATE`; until then
  `graphicsPending` is `true`. libretro has no flag for options that need a
  restart. The host reports `restart: true` for the options a core marks
  "(Restart)" at the end of their text, as libretro cores do, and for
  SwanStation's console region, 8 MB RAM and fast boot options, which
  SwanStation applies only when the console boots. While a game runs the core
  keeps getting the value it started with; the new one is used at the next
  start. An option counts toward `graphicsPending` only once the core has asked
  for it.
- **Options kept at SwanStation's default and not listed:** the memory card
  options (the host keeps memory card 1 itself, and SwanStation formats and
  writes over its own card files when it cannot read them), multitap (version 1
  has controller ports 1 and 2 only), and the log level (the host finds a
  missing BIOS in SwanStation's information messages).
- **activeValue** is the value the host gives the core. SwanStation's own
  per-game compatibility settings (`swanstation_Main_ApplyGameSettings`) can
  override some of them inside the core without telling the frontend.
- **Missing BIOS.** SwanStation keeps going without a usable BIOS, on its
  built-in OpenBIOS, which runs few games. It logs the region it chose
  (`Console Region: …`), then loads the file its BIOS option names for that
  region from the system folder when the file has a BIOS's size, and otherwise
  searches the folder, logging `Using BIOS '<file>'` or `Falling back to
  possibly-incompatible image` when it finds one it knows
  (`src/core/host_interface.cpp`, `GetBIOSImage`). The host applies the same
  steps to the log and the folder; when neither finds a BIOS, it ends the game
  with `errorCode` `bios`. SwanStation takes a file of BIOS size under the
  configured name without looking inside it, so a damaged one boots and never
  shows a frame; that game ends with `didNotStart` after 60 seconds.
- **Missing disc tracks.** SwanStation boots the BIOS with no disc, logging
  `Booting BIOS with no disc`, when it cannot open the disc image (a `.cue`
  whose `.bin` is missing). The host ends that game with `errorCode` `disc`.
- **Disc changes.** These happen between two frames: open the tray, choose
  the disc, close the tray, then check the result. A change that fails puts
  the previous disc back and sets `discs.failed`.
- **A stuck core.** A game limit marks the game ended at once. If the core
  never returns from `retro_run`, `release` replies and then the host exits,
  as protocol.md "release" says.

## Tests

- `json_test` checks the JSON and path rules.
- `resolution_test` checks which scales [`src/resolution.cpp`](../src/resolution.cpp)
  offers for a geometry and a limit, and which value it moves to.
- [`tests/conformance.py`](../tests/conformance.py) is the fake launcher of
  protocol.md "Fixtures". It starts the host as a launcher would (a
  socketpair at descriptor 3, or two pipes passed with a handle list on
  Windows), plays every session whose `tests` includes `host`, and checks the
  outcome. `--strace` (Linux) records the host's network calls for the CI
  check. A read's timeout is for a message to begin: once its first byte has
  come, the reader waits the 5-second write limit of protocol.md "Framing",
  counted from that byte, for the rest of the length and the body, because
  giving up in the middle of a message would read the next body as a length.
  A read that times out before a message begins raises `Idle`, after which
  reading again is safe; one that times out in the middle raises `Failed`,
  which a caller must not read past; the Windows reader keeps a partial
  message and applies the same write limit to it. `real_core.py` waits out quiet spells by
  catching `Idle` only.
  [`tests/framing_test.py`](../tests/framing_test.py) checks that rule
  without the host, on a socketpair on Linux and macOS and on a pipe on Windows.
- [`tests/fake_core.c`](../tests/fake_core.c) is the core the sessions run:
  640 × 480 frames, one option, `.m3u` playlists through the disc interface,
  a memory card, rumble while Cross is held, and a `fake_hang` option that
  makes `retro_run` never return. With a `fake-gpu` file in the system folder
  it also offers `swanstation_GPU_ResolutionScale` (1x to 16x) and asks for an
  OpenGL 3.3 core context; with one it clears the host's framebuffer and gives
  SwanStation's sizes for a 640-wide screen (frames 640 × 480 times the scale,
  largest geometry 1024 × 512 times the scale), and tells the host of a new
  scale through `SET_SYSTEM_AV_INFO` before the next frame. `fake-hold-load`
  and `fake-hold-reset` files make it wait after reading the scale in
  `retro_load_game` and in `context_reset`, so a test can act between them;
  `fake-hold-run` makes it wait inside `retro_run`, so a test can act while a
  frame is being run.
- [`tests/host_test.py`](../tests/host_test.py) covers what the fixtures do
  not: a stuck core (the stall limit, then `release` replying and the host
  exiting), a core that is not a library and a missing disc (each ending the
  game with `errorCode` `core` or `disc`), and eight games in a row with
  `volume`, `pause` and `stop` calls in between, and the resolution limits
  (`gpu_limits`): the fake core started at 16x must start at 8x, offer 1x to
  8x, explain itself in `resolutionError`, draw 5120-wide frames, refuse 16x
  through `option`, take 2x and clear `resolutionError`; then, with 2x chosen
  after the core read a saved 16x but before the host saw its geometry, the
  core must be given 2x by `context_reset`, with no 16x offered and nothing
  refused. Without an OpenGL
  context that test is skipped, unless `--require-gpu` is given, as the Linux
  CI job does with Mesa installed. Run it like `conformance.py`.
- [`tests/lockstep_test.py`](../tests/lockstep_test.py) runs two hosts in
  [lockstep](protocol.md#lockstep), as two online players' hosts
  ([netplay.md](netplay.md) "Tests"): both fed the same input for every
  frame, then a saved state loaded back and replayed on one, and loaded into
  a newly started game on the other; every frame's `sync` must match. With
  the fake core it also checks that input reaches the console, the refusals
  of lockstep and its protocol errors, and, holding the fake core inside a
  frame, that leaving lockstep is answered only once that frame has ended and
  that nothing of the old frame count follows. It reads every host in turn
  while it waits, since an unread host exits at the 5-second write limit.
  With `--bios` and `--program` it runs
  the real SwanStation on the `make_test_disc.py` disc, which the
  `real-core` workflow does for 600 frames on each system. For lockstep the
  fake core keeps four words of "system RAM" (the frame count, a mix of every
  input it read, and the last buttons of each port) and serializes them with
  its rumble state and memory card.
- [`tests/ring_access.c`](../tests/ring_access.c) gives the fake launcher the
  sequentially consistent loads and stores of `latest` and `reading` that
  protocol.md asks for; Python cannot make them on shared memory itself. It
  is built next to `fake_core`, where both scripts look for it.
- [`tests/real_core.py`](../tests/real_core.py) runs the real SwanStation
  through the host. The `real-core` workflow builds OpenBIOS (with `BOOT_MODE=fast`, which
  boots the disc without first running OpenBIOS's own menu program) and the psyqo
  `hello` program from pcsx-redux's nugget (MIT) at a fixed commit, pads the
  BIOS to the 512 KiB SwanStation requires, and builds SwanStation at a fixed
  commit. [`tests/make_test_disc.py`](../tests/make_test_disc.py) puts the
  program on a one-track Mode 2 disc image (`.cue` and `.bin`, with the EDC
  and ECC a pressed disc has), because `start` takes only disc files. The test
  starts it at 2x, then checks that 100 frames come within 10 seconds, that
  two pictures a second or more apart are not black and differ (the
  program animates its background), and that `hardware` and `resolutionError`
  agree (a machine without an OpenGL context draws in software and must say
  so). With `--expect-opengl`, which `real-core.yml` passes on Linux and macOS (the
  release workflow's check of the unpacked zips does not), the test also
  fails when the host made no OpenGL context, so neither system can slip back
  to the core's software renderer unnoticed. The Windows runner makes no
  OpenGL 3.3 context, so there the core draws in software. The `hello`
  picture is one flat colour, so it cannot show which way up a picture is. After a clean `stop` the test therefore starts a second game,
  a program `make_test_disc.py` writes itself (`corner_program`), which fills
  the screen blue and only its top-left corner red; the test checks those
  colours near all four corners, which a picture turned upside down, mirrored
  or with red and blue swapped fails. The second game also makes a second
  OpenGL context in the same host. The test then checks a clean `stop` and
  exit, and keeps the last picture (`--picture`) as a workflow artifact. No
  Sony BIOS or game is used.
- CI runs everything on Linux (Ubuntu 22.04, the oldest the launcher
  supports, and 24.04), macOS (arm64) and Windows, and the Linux tests again
  under ThreadSanitizer (on 24.04). GCC 11's ThreadSanitizer reports a false
  "double lock" in `Game::Join`: libstdc++ 11 waits with
  `pthread_cond_clockwait`, which that version does not intercept. Use GCC 12
  or later, or clang 14 or later.

## Releases

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds the
files protocol.md "Finding and checking the host" names, and
[`tools/package_release.py`](../tools/package_release.py) packs them, so a
release can be packed and checked by hand the same way.

- **Systems.** `windows-x64` (Windows Server 2022, MSVC, with the static C and
  C++ runtime so no Visual C++ redistributable is needed; the workflow fails
  if the host or SwanStation imports it), `linux-x64` (Ubuntu 22.04, the oldest
  Linux the launcher supports, so it needs glibc 2.35 or later) and
  `macos-arm64` (built on `macos-latest`, an arm64 runner, for macOS 11 or
  later).
- **What a zip holds.** The host (`anyps5-retro-host.exe` on Windows,
  `anyps5-retro-host` elsewhere), SwanStation
  (`swanstation_libretro.dll`, `.so` or `.dylib`) built from
  `SWANSTATION_COMMIT`, `licenses/` with the host's and SwanStation's licences
  and every licence, copying, copyright or notice file under SwanStation's
  `dep/` (the libraries it builds in, such as libchdr, xxHash and VIXL, as
  `licenses/swanstation-dep/<path>`), the Apache-2.0 text for SwanStation's
  Vulkan headers (`licenses/swanstation-vulkan-headers-Apache-2.0.txt`: the
  headers name the licence only by URL, so the workflow fetches SPDX's copy at
  a fixed tag and checks its sha256), and `SOURCE.txt` naming the host commit,
  the SwanStation commit and the source archive. Everything sits at the
  top of the zip except the licences. The zip is the same bytes for the same
  inputs (entries sorted by name, one fixed time, fixed permissions), so a
  rebuild can be compared with a release.
- **Tested with.** `SWANSTATION_COMMIT` in `release.yml` must be the one
  `real-core.yml` tests, and the workflow fails otherwise. Each system's zip
  is unpacked again on its own runner and boots OpenBIOS and the `hello`
  program through [`tests/real_core.py`](../tests/real_core.py), so the
  files that are released are the files that were tested.
- **Version.** `<version>` is `project(... VERSION)` in `CMakeLists.txt`,
  which is also `hostVersion` in `welcome`. A tag `v<version>` runs the
  workflow for a release and must match it. Pushes and pull requests that
  change the workflow or the packer run it too, and `workflow_dispatch` runs it
  by hand.
- **Source.** The `source` job packs the host and SwanStation at the released
  commits, with all their submodules and without `.git`, into
  `anyps5-retro-host-<version>-source.tar.gz` (sorted, with fixed times and
  owners). It is the complete corresponding source the GPL asks for, and is
  published with the binaries, and carries the same Apache-2.0 text at its
  top. stb, glad and libretro-common, which are public domain or MIT, keep
  their notices in their source files, which the archive holds.
- **Result.** The artifact `anyps5-retro-host-<version>-unsigned` holds the
  three zips, the three `.files`, the source archive and `SHA256SUMS`, written as `shasum -a 256`
  writes it and sorted by name as AnyPS5-ARM's `tools/release/sign.sh` sorts
  it. `python3 tools/package_release.py --check <folder>` checks a folder of
  them as a launcher checks a download, without the signature.

**Where the maintainers sign.** The workflow holds no key, so `SHA256SUMS` is
unsigned and nothing is published. A maintainer with the release key:

1. Downloads the `unsigned` artifact.
2. Runs `tools/release/sign.sh -o <folder> <folder>/*.zip <folder>/*.files <folder>/*.tar.gz`
   from AnyPS5-ARM. It writes the same `SHA256SUMS` again (same lines, same
   order) and `SHA256SUMS.sig`, `ed25519 <key id> <base64 signature>` a line,
   with the keys listed in its `tools/release/release-signing.pub`, which are
   the keys built into the launchers (`lib/core/release_keys.dart` in the
   Flutter launcher).
3. Publishes a GitHub release `v<version>` with the zips, the `.files`, the
   source archive, `SHA256SUMS` and `SHA256SUMS.sig`. The source archive must
   be published with every release that carries the binaries.

On macOS the host and SwanStation must also carry a Developer ID signature with
the hardened runtime, and the host must be notarized (protocol.md "macOS
signing"). That signing changes the files, so it goes into the `package` job
after the build and before `Pack`, where the workflow marks it; it needs the
team's certificate as a secret, which this repository does not hold. Until it
is added, the macOS zip carries only the linker's ad-hoc signature: it is not
yet the release protocol.md describes, and Gatekeeper refuses a downloaded copy.
