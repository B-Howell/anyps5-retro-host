<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# anyps5-retro-host

The separate program that runs emulator cores for the AnyPS5 launchers, and
the contract between them. Version 1 runs single-player PS1 games with
SwanStation.

- [docs/protocol.md](docs/protocol.md): the protocol, the contract both sides
  are written from.
- [docs/host.md](docs/host.md): how this host is put together.
- [docs/netplay.md](docs/netplay.md): the design for PS1 online play, of
  which lockstep (protocol.md "Lockstep") is built.
- [fixtures/v1](fixtures/v1): every message, and whole conversations
  (valid and invalid), for the host's and the launcher's conformance tests.
- [tools/check_fixtures.py](tools/check_fixtures.py): checks the fixtures
  against the document. Run `python3 tools/check_fixtures.py`.
- [tools/package_release.py](tools/package_release.py): packs and checks a
  release; [docs/host.md](docs/host.md) "Releases" says how releases are built
  and where the maintainers sign them.
- [tests/conformance.py](tests/conformance.py): the fake launcher. It plays
  every fixture session that tests the host against a built host.

## Building and testing

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python3 tests/conformance.py --host build/anyps5-retro-host --core build/fake_core.so
python3 tests/host_test.py --host build/anyps5-retro-host --core build/fake_core.so
python3 tests/lockstep_test.py --host build/anyps5-retro-host --core build/fake_core.so
```

The real-core tests need SwanStation built with the host's patches:
`bash cores/apply-swanstation-patches.sh <swanstation checkout>` before
building it ([docs/host.md](docs/host.md) "SwanStation patches").

On Windows the programs are `build/Release/anyps5-retro-host.exe` and
`build/Release/fake_core.dll`; on macOS the core is `build/fake_core.so`.
`fake_core` is a small libretro core made for these tests; the sessions run
against it rather than SwanStation, which needs a BIOS and a game.

## Licences

| Part | Licence |
| --- | --- |
| `docs/`, `fixtures/`, `tools/`, CI files | MIT ([LICENSES/MIT.txt](LICENSES/MIT.txt)) |
| The host (`src/`, `tests/`, `CMakeLists.txt`) | GPL-2.0-or-later ([LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt)) |
| `3rdparty/libretro-common` | MIT, submodule of [libretro/libretro-common](https://github.com/libretro/libretro-common) (only `libretro.h` is used) |
| `3rdparty/miniaudio` | Public domain or MIT-0, submodule of [mackron/miniaudio](https://github.com/mackron/miniaudio) at 0.11.26 |
| `cores/patches` | GPL-3.0-only ([LICENSES/GPL-3.0-only.txt](LICENSES/GPL-3.0-only.txt)), patches to [libretro/swanstation](https://github.com/libretro/swanstation) that the host's SwanStation is built with ([docs/host.md](docs/host.md) "SwanStation patches") |
| `cores/apply-swanstation-patches.sh` | MIT |

The JSON fixtures and the patches cannot hold a comment, so
[REUSE.toml](REUSE.toml) gives their licences. A launcher may copy anything that is MIT here.
