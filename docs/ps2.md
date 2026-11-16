<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# PS2 games through the host

PS2 is the next console after PS1. This document says which libretro core the
host runs PS2 games with and why, how that core is built, how the player's
BIOS reaches it, how it draws, what the protocol gains, and what stands
between it and online play. [protocol.md](protocol.md) stays the contract;
this is the design behind its `ps2` capability.

**Built so far (the first step).** The core, LRPS2, is the `cores/lrps2`
submodule, built from source by the host's CMake and in CI on Windows and
Linux with the host's one patch. The host lists `ps2`, takes
`start.console: "ps2"`, loads LRPS2, reports its name and core build identity
in `status.core` and its console in `status.console`, and ends a game with
`errorCode` `bios` when LRPS2 finds no BIOS. A real-core CI test checks all of
that. No PS2 program has been booted: LRPS2 cannot boot anything without a
Sony BIOS, and none is free to use (see "The BIOS"). Drawing, sound, input
and memory cards of a running PS2 game are therefore designed and coded but
not yet seen working.

## Which core

The libretro PS2 cores, as they stood on November 16, 2026:

| | LRPS2 | The earlier pcsx2 libretro port | Play! |
| --- | --- | --- | --- |
| Source | [libretro/parallel-ps2](https://github.com/libretro/parallel-ps2) (it was `libretro/ps2`; GitHub redirects the old name), a hard fork of PCSX2 made for libretro | `libretro/pcsx2`, the libretro team's first port of PCSX2; the repository no longer resolves (GitHub answers 404), and libretro's own [`pcsx2_libretro.info`](https://github.com/libretro/libretro-core-info/blob/master/pcsx2_libretro.info) now describes LRPS2 under that file name. Upstream PCSX2 builds a Qt program, not a libretro core | [jpd002/Play-](https://github.com/jpd002/Play-), with its libretro front end in `Source/ui_libretro` |
| Licence | GPL-3.0 (`COPYING.GPLv3`); older PCSX2 files carry LGPL-3.0-or-later headers | as LRPS2 | BSD-2-Clause (`License.txt`; libretro's info file says MIT) |
| Upkeep | Commits most days (about 70 from August to November 2026; c0d39a58e on November 16). Recent work: an arm64 recompiler port, rewriting the tree in C89, a UBSan CI row | Gone | Active (last commit October 26, 2026) |
| Compatibility | PCSX2's, which libretro's info file calls "mature and highly-compatible" and "a good first choice" | — | libretro's info file: "significantly lower compatibility … than PCSX2", marked experimental |
| BIOS | Required: a dump in `<system>/pcsx2/bios` (below) | — | Not required: its kernel is high-level emulation, so it boots homebrew ELFs alone |
| Hardware rendering | OpenGL (3.3 core, 3.0, or ES 3.1), Vulkan 1.1 (its own renderer and paraLLEl-GS), Direct3D 11 and 12 on Windows; and a software renderer | — | OpenGL |
| Save states | `retro_serialize`; libretro's info file says `savestate_features = "basic"` | — | `basic` |
| Build | CMake 3.12 or later, no system library but threads and libc (zlib, zstd, lzma, libzip and libchdr were replaced by libretro-common's decoders); MSVC, GCC and Clang; x86-64 and arm64 recompilers | — | CMake |

**The host runs LRPS2.** It is the core most PS2 games run on, it is kept up
by the libretro team itself, and it fits the host as SwanStation does:

- It builds from source with nothing but CMake and a compiler on Windows and
  Linux. On the Windows desktop the whole core (378 build steps) built with
  MSVC 2022 and Ninja in about ten minutes at three compilers, as a 9 MB DLL
  that needs no Visual C++ runtime (it imports only `KERNEL32`, `WS2_32`,
  `IPHLPAPI`, `WINMM`, `dxgi` and `D3DCOMPILER_47`, all part of Windows).
- It draws through an OpenGL 3.3 core context, which the host already makes
  for SwanStation (see "Drawing").
- Its `library_version` can carry the core build identity with a small patch,
  as SwanStation's does (see "The core").
- GPL-3.0 is what SwanStation is under, and the host is GPL-2.0-or-later so it
  can load it.

What it costs: it needs the player's own BIOS, so CI cannot boot a game with it
(see "The BIOS"), and it is a large multi-threaded recompiling emulator whose
determinism is unproven (see "What stops PS2 online play").

Play! is the only core that would boot a program in CI, thanks to its
high-level kernel. Its lower compatibility rules it out as the player's core;
it could later serve as a second, test-only core for the host's own PS2 paths
(frames, input, memory cards) if those need a booting program before a BIOS
question is settled.

**How RetroArch hosts LRPS2.** From LRPS2's
[core info](https://github.com/libretro/libretro-core-info/blob/master/pcsx2_libretro.info)
and its `libretro/main.c` at c0d39a58e:

- Firmware is looked for in `pcsx2/bios` under the system directory, by
  content not by name; RetroArch only checks that the folder exists.
- `required_hw_api = "Direct3D >= 11.0 | OpenGL Core >= 3.3 | OpenGL >= 3.0"`.
  The core asks for the frontend's preferred context first
  (`RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER`), then, on Windows, Direct3D 12
  and 11, then OpenGL core, OpenGL, OpenGL ES and Vulkan; with none it uses its
  software renderer. Vulkan goes through the context negotiation interface.
- Memory cards are `Mcd001.ps2` and `Mcd002.ps2` in `pcsx2/memcards` under the
  system directory, shared by every game, or, with
  `pcsx2_shared_memory_cards` off, one card per game named after it in the
  save directory.
- `savestate_features = "basic"`: RetroArch takes states from it but does not
  count it as deterministic, the level its netplay and run-ahead rely on.

## The core

`cores/lrps2` is a submodule of
[libretro/parallel-ps2](https://github.com/libretro/parallel-ps2), pinned at
c0d39a58e0417189abdd2d77ba1b73126083b41f. It has no submodules of its own.
Moving it is an ordinary submodule commit; the patches must then apply to the
new commit, and its BIOS message (see "The BIOS") must still be what the host
looks for.

Configuring the host with `-DANYPS5_RETRO_HOST_LRPS2=ON` builds it
([`CMakeLists.txt`](../CMakeLists.txt)) as an `ExternalProject`:

1. [`cores/prepare-lrps2.cmake`](../cores/prepare-lrps2.cmake) refuses a
   submodule checkout with any change or untracked file, clones the
   submodule's commit into `lrps2-source` in the build folder, applies
   `cores/patches/lrps2-*.patch` in the order of their names to git's index
   and checks the files out (so the LF patches also apply where git writes
   CRLF), and writes `anyps5-core-source.txt` there: the LRPS2 commit and
   each patch's SHA-256 and name. The submodule itself is never changed. A
   build runs it again before building and keeps the copy only while it is
   exactly that source (its commit; its index the tree the patches make of
   that commit, worked out again in an index of its own; its files the
   index's; no other file but the manifest), and otherwise makes it again, so
   an edit in the copy cannot be built under the identity.
2. LRPS2's own CMake builds `pcsx2_libretro` in `lrps2-build` with
   `-DCMAKE_BUILD_TYPE=Release -DC89_EMITTER=ON -DDISABLE_ADVANCE_SIMD=ON
   -DUSE_OPENGL=ON -DUSE_VULKAN=ON`, the generator and compilers of the host's
   build, and `--parallel` (`ANYPS5_RETRO_HOST_LRPS2_JOBS` sets how many).
3. The library is copied to `cores/` in the host's build folder
   (`pcsx2_libretro.dll`, `.so` or `.dylib`).

The patch, as SwanStation's sixth does:

| Patch | What it changes |
| --- | --- |
| [`lrps2-1-core-build-identity`](../cores/patches/lrps2-1-core-build-identity.patch) | When the source holds `anyps5-core-source.txt`, LRPS2's CMake adds the build options that change what the core computes or which renderers it has (`C89_EMITTER`, `DISABLE_ADVANCE_SIMD`, `USE_OPENGL`, `USE_VULKAN`, each `=ON` or `=OFF`), writes the whole as `anyps5-core-manifest.txt` in its build folder, and `library_version` becomes `anyps5-1-<SHA-256 of it>`. Without the file the core keeps its own `v2.0.0-<git revision>` |

So `status.core` for the host's LRPS2 is
`{"name": "ParaLLEl PS2", "version": "anyps5-1-<64 hex digits>"}`.
[`tools/core_identity.py`](../tools/core_identity.py) `--lrps2-commit
<commit>` computes the same value without a build
(`anyps5-1-82851e32…25c5` at c0d39a58e), and the `real-core` workflow checks
every system's build against it and against each other. The launcher's rule is
the one for PS1 ([netplay.md](netplay.md) "Core build identity").

`DISABLE_ADVANCE_SIMD=ON` is LRPS2's default and builds its GS, sound and
video decoder three times (SSE4.1, AVX, AVX2), choosing one when the core
starts; without it the build is for the building machine's processor
(`-march=native`), which would make two players' cores different programs.

## The BIOS

A PS2 BIOS is Sony's code, so it is **never shipped, downloaded or put in a
test**. The player dumps it from their own console.

**Where the launcher puts it.** Settings › Emulators gets a PS2 row beside the
PS1 one. The player picks their BIOS file there, and the launcher copies it
into `pcsx2/bios` under the system directory it already sends as
`start.system` (the PS1 BIOS lives at the top of that same folder), keeping
its file name. It copies the `.nvm` and `.mec` files of the same name with it
when they sit beside the picked file. LRPS2 keeps the console's settings in
that `.nvm` beside the BIOS and writes new default `.nvm` and `.mec` files
there when they are missing (`pcsx2/CDVD/CDVD.c`, `cdvdLoadNVRAM`). It never looks for a BIOS anywhere else, and never sends one to a
friend.

**How it is checked.** When the player picks a file, the launcher checks it
the way LRPS2 does (`pcsx2/ps2/BiosTools.c`, `IsBIOS`): it is 4 MiB to 8 MiB;
somewhere in its first 8 MiB, on a 16-byte boundary, is the ROM directory
entry `RESET` (ten bytes, padded with zeros); and the table that follows has a
`ROMVER` entry whose 14 bytes are in the file. The launcher shows what
`ROMVER` says (the region letter at its fifth byte, the version, the date) and
refuses any other file with a message saying why. This is a check of the
format, not a list of known dumps; a damaged image of the right shape is only
found when the game fails to start.

**How the host finds it.** It does not judge it. LRPS2 looks in
`<system>/pcsx2/bios` itself when it is initialised: every file of 4 MiB to
8 MiB that `IsBIOS` accepts becomes a value of the `pcsx2_bios` option, the
first is the default, and the launcher may name another (the option is listed
in `status.options` once a BIOS is found). With none, `retro_load_game` logs
`Could not find any valid PS2 BIOS File in "<folder>".` and fails, and the host,
which reads the core's log as it does SwanStation's for PS1, ends the game with
`errorCode` `bios` and `error` `No PS2 BIOS was found.`. Any other failed load
ends with `errorCode` `core` (`The core could not start the game.`), which
covers a BIOS the core takes but cannot start. The core creates the
`pcsx2/bios`, `memcards`, `cheats`, `cheats_ws`, `cheats_ni` and `cache`
folders under the system directory when they are missing.

**Why CI boots nothing.** LRPS2, like PCSX2, has no built-in kernel: even a
homebrew ELF (`.elf` content) is loaded by the BIOS's own loader. There is no
freely licensed PS2 BIOS that boots one in LRPS2 (`BiosTools.c` mentions
FPS2BIOS, an incomplete open BIOS from the early 2000s), and a fake image that
only passes the format check would test nothing but a crash. So
[`tests/ps2_core.py`](../tests/ps2_core.py) checks the missing-BIOS path with
the real core: a system directory with no `pcsx2` folder, and one whose
`pcsx2/bios` holds a 4 MiB file that is not a BIOS, both end with `bios`
before a first frame; the core loads again for a third game; and the host
exits cleanly. The game is a small looping PS2 ELF the test writes, which the
core refuses before reading it.

## Drawing

The host makes one kind of context, OpenGL (WGL on Windows, EGL on Linux,
CGL on macOS), and reads each frame back into the frame ring
([host.md](host.md)). It answers `GET_PREFERRED_HW_RENDER` with OpenGL core,
and fixes `pcsx2_renderer` at `Auto`, so LRPS2 asks for an OpenGL 3.3 core
context first on every system, which the host already makes. The OpenGL
renderer uses 4.3 and 4.4 features (copy image, buffer storage) when the
driver has them and works without them. If the host could not make the
context, LRPS2 would fall back to its software renderer, and `status.hardware`
would say so.

**Vulkan is the target after that.** It is LRPS2's main renderer, and the only
way to its paraLLEl-GS renderer, which draws the GS in compute shaders the same
way on every GPU and so is the likeliest to give two players the same picture.
The host would need a Vulkan device of its own, the libretro Vulkan render
interface and context negotiation (`RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN`),
and to copy each image into the ring. Direct3D is not planned: it is Windows
only, and the launcher's Linux build must behave the same.

The internal resolution is `pcsx2_upscale_multiplier` (1x, 2x, 4x, 8x, applied
at the next start). The host's resolution limits (host.md "Resolution limits")
know SwanStation's option only, so for PS2 the value goes to the core as given;
a frame larger than 8192 × 8192 still ends the game with `frameTooLarge`.

## What the protocol gains

All of it is behind the `ps2` capability ([protocol.md](protocol.md)
"Capabilities"); a launcher sends none of it to a host that does not list it.
`hello` itself is unchanged.

- **`start.console`**: `"ps1"` (the default) or `"ps2"`. The field is not
  called `system`, which already names the system directory. For `"ps2"`,
  `core` is LRPS2, `disc` is a `.iso`, `.chd`, `.cso`, `.zso`, `.cue` or
  `.elf` file, and `initialDisc` and `lockstep` are `argument` errors. Multi-disc
  `.m3u` playlists are left for later: few PS2 games have two discs, and
  `initialDisc` follows SwanStation's playlist rules.
- **`status.console`**: the console the game was started as. `status.core`
  is LRPS2's name and identity; `status.timing` comes from the core as for PS1
  (LRPS2 starts at 60/1.001 for NTSC and 50 for PAL).
- **`errorCode`**: `bios` when LRPS2 found no BIOS; `core` when the core could
  not start the game for any other reason.
- **Controllers**: two ports, as for PS1. The host gives LRPS2's DualShock 2
  (`RETRO_DEVICE_JOYPAD` in LRPS2) to each port with a controller in, and
  answers its pressure-sensitive button reads with full pressure for a
  pressed button. The multitap option is fixed off.
- **Memory cards**: one card per game, as for PS1. The host fixes
  `pcsx2_shared_memory_cards` off, so LRPS2 keeps card 1 as `<disc's name
  without its extension>.ps2` in `saves` (and creates it, formatted, when it
  is missing) and has no card 2. Unlike PS1's `.srm`, the core reads and
  writes this file itself, so the host's "a card it cannot read is never
  written over" rule does not cover it yet. Shared cards are not used: they
  would live in the system directory, and one card holding every game's saves
  is harder to keep and to send to a friend.
- **Options** the host sets itself and leaves out of `status.options`:
  `pcsx2_shared_memory_cards` (`disabled`), `pcsx2_renderer` (`Auto`) and
  `pcsx2_multitap` (`disabled`), whatever `start.options` says.
- **Lockstep and states**: not for PS2. `lockstep` answers `false` for a PS2
  game, `saveState` and `loadState` too.

## What stops PS2 online play

Online PS2 play would reuse PS1's design ([netplay.md](netplay.md)): the
launcher carries the packets, the host runs frames in lockstep or rollback,
and the launchers compare `sync` hashes, core identities and BIOS hashes.
None of it is turned on for PS2, for these reasons, roughly in order:

1. **No proof of determinism, and no way to get one in CI.** Every PS1 claim in
   netplay.md rests on two real cores fed the same input in CI. With no free
   PS2 BIOS, the same tests could only run on a developer's machine with their
   own BIOS, kept out of the repository.
2. **Threads.** LRPS2 runs the GS on its own thread (MTGS) and, on a
   processor of three cores or more, VU1 on another (MTVU, on by default),
   with the EE and IOP recompilers on the main one. Whether their hand-offs can
   change what the game sees is something upstream PCSX2 never had to promise,
   and PCSX2 offers MTVU as a speed hack. A netplay
   build would at least fix MTVU off and prove the GS thread cannot feed back
   into the emulated machine.
3. **The processor decides code paths.** The SSE4.1, AVX and AVX2 builds of the
   GS, sound and video decoder are chosen per machine; two players' cores
   would only match if every path gives the same bytes, which needs proving or
   one path fixed for netplay. The EE's floats are another: LRPS2 has its own
   bit-exact PS2 float model (`ps2float`), but whether it is on for every
   game's needs is a core option.
4. **States are large and live partly on the GPU.** A PS2 state holds 32 MiB of
   EE RAM, 4 MiB of GS memory, 2 MiB each of IOP and sound memory and more;
   with a hardware renderer the GS memory is read back from the GPU for every
   save. Rollback saves a state a frame, and a friend joins with a whole state
   file: both need measuring, and probably same-instance states that keep GS
   memory on the GPU, as SwanStation patch 7 does for PS1.
5. **libretro says it is not deterministic.** `savestate_features = "basic"`;
   RetroArch does not offer netplay or run-ahead on it.
6. **The host has no PS2 lockstep**: `start.lockstep`, `lockstep`, `saveState`
   and `loadState` refuse a PS2 game, and `sync` would need PS2's RAM (LRPS2
   gives `RETRO_MEMORY_SYSTEM_RAM` as EE RAM and the scratchpad).
7. **Each player's files must match**: the BIOS (compared by hash), its `.nvm`
   (the console's settings, which the BIOS reads), and the memory card the game
   reads, which a friend joining a running game would need sent.

## Releases

Release zips hold no PS2 core yet: `release.yml` builds SwanStation only, and
its source archive leaves `cores/lrps2` out. Shipping PS2 means building LRPS2
in `release.yml` with the host's option, adding it and its licences to the
zips, `.files` and `SOURCE.txt` (`tools/package_release.py`), putting the
patched LRPS2 source in the source archive, checking on Windows that it still
needs no Visual C++ runtime, and booting it in the release check, which again
needs a BIOS CI does not have.

## Not done yet

- Booting anything: drawing, sound, input, pacing and memory cards of a running
  PS2 game are unverified.
- macOS: LRPS2 builds there upstream, but the host's PS2 jobs run on Windows
  and Linux only.
- The launcher side: the Settings › Emulators PS2 row, the BIOS check and copy,
  starting PS2 games with `console`, and showing `bios`.
- Vulkan, `.m3u` playlists for PS2, the host's resolution limits for
  `pcsx2_upscale_multiplier`, and protecting a memory card the core cannot read.
