<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# How the host is put together

The host implements [protocol version 1](protocol.md) with the `ps1`
capability. It is a small C++17 program with three threads:

| Thread | Does |
| --- | --- |
| Control (`main`) | Reads each message, answers calls and pings in order, and enforces the connection's order rules |
| Game | Started once the `start` reply is sent. Opens the core and the disc, then `retro_init`, `retro_load_game` and `retro_run` paced to the core's frame rate. Publishes frames and sends `ring`, `frame` and `rumble` |
| Watch | Every 100 ms: the 5 s write limit, the stall and first-frame game limits, the `stop` limit, and the 10 s limit for leaving |

On Linux and macOS a fourth thread waits for `SIGTERM` (sent by
`PR_SET_PDEATHSIG` on Linux), and on macOS a fifth for the launcher's exit
through kqueue. On Windows a thread waits on the launcher's process handle.

## Files

| File | What it holds |
| --- | --- |
| [`src/main.cpp`](../src/main.cpp) | The command line, the process setup, then the host |
| [`src/host.cpp`](../src/host.cpp) | The protocol: `hello`, the order rules, every method's argument checks and replies, and the limits |
| [`src/game.cpp`](../src/game.cpp) | One loaded game: the libretro callbacks, core options, disc control, the memory card, input, rumble and pixel conversion |
| [`src/ring.cpp`](../src/ring.cpp) | The frame ring writer: the header from protocol.md's byte table, slot choice, and capped doubling when a frame needs more room |
| [`src/json.cpp`](../src/json.cpp) | A strict JSON reader (duplicate keys, lone surrogates, invalid UTF-8 and non-finite numbers are errors) and writer |
| [`src/paths.cpp`](../src/paths.cpp) | The path rules of protocol.md "Paths" for Windows and POSIX |
| [`src/channel.cpp`](../src/channel.cpp) | Framing for the control stream, and the clock the watch thread reads for the write limit |
| [`src/platform_posix.cpp`](../src/platform_posix.cpp), [`src/platform_windows.cpp`](../src/platform_windows.cpp) | The control stream, process setup, watching the launcher, shared memory, loading the core and files |
| [`src/audio.cpp`](../src/audio.cpp) | Sound output through miniaudio, with the volume, mute and pause |

## Choices made for version 1

- **GPU rendering.** The host answers SwanStation's request for an OpenGL
  3.3 core context (`RETRO_ENVIRONMENT_SET_HW_RENDER`; `GET_PREFERRED_HW_RENDER`
  says OpenGL core) with an offscreen context on the game's thread: EGL on
  Linux (loaded at run time), WGL with a hidden window on Windows, and CGL on
  macOS ([`src/gpu.cpp`](../src/gpu.cpp) and one file per system). The core
  draws into the host's framebuffer, which grows with the core's largest
  geometry; each frame is read back with `glReadPixels`, turned the right way
  up, and published to the ring like a software frame. `status.hardware` is
  then `true` and the internal resolution option works. On macOS the host asks
  CGL for an accelerated renderer (`kCGLPFAAccelerated`) first and, only when
  there is none, for Apple's software renderer
  (`kCGLPFARendererID` = `kCGLRendererGenericFloatID`, "Apple Software
  Renderer" in `CGLRenderers.h`, OpenGL 4.1 core). A Mac whose GPU has an
  OpenGL renderer takes the first; a Mac virtual machine whose paravirtual GPU offers Metal but
  no OpenGL renderer takes the second. GitHub's `macos-latest` runner is such a
  machine: its only CGL renderer is `0x01020400` (`kCGLRendererGenericFloatID`
  under `kCGLRendererIDMatchingMask`), not accelerated, and asking for
  acceleration fails with `kCGLBadPixelFormat`. This matches Linux, where EGL
  takes Mesa's software rasterizer when there is no GPU driver, so on all
  systems `status.hardware` means the core draws with its OpenGL renderer,
  whether a GPU or the CPU runs it. The software renderer keeps the internal
  resolution option and the core's OpenGL accuracy settings working; the
  host does not prefer SwanStation's own software renderer over it, as no
  system has a way to know which is faster on a given machine. When no context
  can be made, the host turns the request down and SwanStation draws in software;
  `resolutionError` then says a scale above 1x needs the GPU. A context that was
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
  context.
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
- [`tests/conformance.py`](../tests/conformance.py) is the fake launcher of
  protocol.md "Fixtures". It starts the host as a launcher would (a
  socketpair at descriptor 3, or two pipes passed with a handle list on
  Windows), plays every session whose `tests` includes `host`, and checks the
  outcome. `--strace` (Linux) records the host's network calls for the CI
  check. A read's timeout is for a message to begin: once its first byte has
  come, the reader waits the 5-second write limit of protocol.md "Framing" for
  the rest, because giving up in the middle of a message would read the next
  body as a length.
- [`tests/fake_core.c`](../tests/fake_core.c) is the core the sessions run:
  640 × 480 frames, one option, `.m3u` playlists through the disc interface,
  a memory card, rumble while Cross is held, and a `fake_hang` option that
  makes `retro_run` never return.
- [`tests/host_test.py`](../tests/host_test.py) covers what the fixtures do
  not: a stuck core (the stall limit, then `release` replying and the host
  exiting), a core that is not a library and a missing disc (each ending the
  game with `errorCode` `core` or `disc`), and eight games in a row with
  `volume`, `pause` and `stop` calls in between. Run it like `conformance.py`.
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
  so). With `--expect-gpu`, which CI passes on Linux and macOS, the test also
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
- CI runs everything on Linux, macOS (arm64) and Windows, and the Linux tests
  again under ThreadSanitizer. GCC 11's ThreadSanitizer reports a false
  "double lock" in `Game::Join`: libstdc++ 11 waits with
  `pthread_cond_clockwait`, which that version does not intercept. Use GCC 12
  or later, or clang 14 or later.
