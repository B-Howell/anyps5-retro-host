# anyps5-retro-host

The emulator host for [AnyPS5 Desktop](https://github.com/AnyTogether/AnyPS5-Desktop).
It loads libretro cores (SwanStation for PS1, LRPS2 for PS2, RPCS3 Libretro
for PS3), runs the game, plays its audio and hands each frame to the launcher
through shared memory. The launcher draws that frame in its own window and
talks to this program over the [protocol](docs/protocol.md).

It is a separate program under the GPL-3.0 (see [LICENSE](LICENSE)) so that
GPL-3.0 cores never load into the GPL-2.0-only launcher, and so a crashing or
hung core cannot take the launcher down. AnyPS5 Desktop starts it on the first
emulated game and never links it.

## Layout

- `protocol/` — the wire format: JSON values, framing, the child-process
  transport, the shared frame ring, and the launcher-side client the tests use.
  The launcher carries the same protocol code; `docs/protocol.md` is the
  contract between them.
- `src/host_main.cpp`, `src/host_server.*` — process entry, request loop,
  frame publishing.
- `src/core/` — the portable session: libretro loading, PS1 match timeline,
  rollback and checkpoints, PS2 virtual LAN, PS3 network policy and the Vulkan
  renderer (Linux).
- `src/linux/`, `src/windows/` — each OS's backend: method handling, OpenGL
  or D3D11 readback, PulseAudio or waveOut.
- `tests/` — protocol tests against fixture cores, and `ps1_host_probe`, which
  drives real host processes for the mixed Windows/Linux PS1 rollback harness.

## Build

Windows (MSVC 2019 or later):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RETRO_HOST_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux needs `libegl-dev`, `libepoxy-dev` and `libpulse-dev`. The PS1 match
storage must be private, so run the tests with a `TMPDIR` that supports
owner-only permissions (`/dev/shm` or `/tmp`). The session's own tests are in
`src/core/test` (`cmake -S src/core/test -B build/core -DANYPS5_RETRO_VULKAN=OFF`).

Put the built `anyps5-retro-host` (or `.exe`) beside the AnyPS5 Desktop
executable, or point `ANYPS5_RETRO_HOST` at it. `ANYPS5_RETRO_HOST_LOG` sends
the host's output to a file.
