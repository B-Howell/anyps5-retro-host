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

- **Software rendering.** The host turns down `RETRO_ENVIRONMENT_SET_HW_RENDER`,
  so SwanStation draws with its software renderer and `status.hardware` is
  `false`. A GPU path (an offscreen context, read back into the ring) is a
  later change. Until then the internal resolution option has no effect.
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
  check.
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
- [`tests/fake_host.py`](../tests/fake_host.py) is the fake host of
  protocol.md "Fixtures", for testing a launcher; see below.
  [`tests/fake_host_selftest.py`](../tests/fake_host_selftest.py) tests it
  by playing the launcher side of every launcher session, started as a
  launcher starts the host, directly, from environment variables and through
  a shim. Run it with `python3 tests/fake_host_selftest.py`.
- CI runs everything on Linux, macOS (arm64) and Windows, and the Linux tests
  again under ThreadSanitizer. GCC 11's ThreadSanitizer reports a false
  "double lock" in `Game::Join`: libstdc++ 11 waits with
  `pthread_cond_clockwait`, which that version does not intercept. Use GCC 12
  or later, or clang 14 or later.

## Testing a launcher with the fake host

The launcher under test starts `fake_host.py` through its own start path, as
if it were the host, and the fake plays one session whose `tests` includes
`launcher`:

```bash
python3 -I tests/fake_host.py --session <session.json> --report <report.json> \
  [--paths <paths.json>] --ipc-fd 3 --parent-pid <pid>
python -I tests\fake_host.py --session <session.json> --report <report.json> ^
  [--paths <paths.json>] --ipc-read <handle> --ipc-write <handle> --parent-pid <pid>
```

The last arguments are the host's own (protocol.md "Starting the host"); the
fake checks them. A launcher starts one executable with only those
arguments, so the other three can come from `ANYPS5_FAKE_HOST_SESSION`,
`ANYPS5_FAKE_HOST_REPORT` and `ANYPS5_FAKE_HOST_PATHS`, and the launcher
runs a shim that starts the fake. A command-line option wins over its
variable.

- **Linux and macOS.** The shim must `exec`, so the process the launcher
  started is the fake: `exec python3 -I /path/to/fake_host.py "$@"`.
- **Windows.** A `.cmd` shim works: `@python -I C:\path\fake_host.py %*`
  then `@exit /b %ERRORLEVEL%`. The fake finds the process the launcher
  started by walking up from itself to the process whose parent is
  `--parent-pid`, and uses it as the host: its id goes in `welcome`, each
  ring's section handle is duplicated into it, and its exit counts as the
  launcher ending the host.

`--paths` names a JSON object of placeholder names to strings, such as
`{"core": "/opt/a/swanstation_libretro.so", "disc": "/games/a.cue"}`. A
session that uses a placeholder the file does not give fails.

What the fake does with each step:

- **From the host**: sends it as written. `welcome` carries the started
  process's id, or another with `wrongPid`. A `reply` or `pong` whose `id` is
  in `anyValueFor` echoes the id the launcher used in the matching step. A
  `ring` step creates the memory object of `memoryBytes` (a sealed or
  unsealed memfd, an unlinked POSIX shared memory object, or an unnamed
  `SEC_COMMIT` section whose handle goes in the message), writes the header,
  fills each slot's `width × height × 4` pixel bytes with `0x80` when it is
  `visible` (zero otherwise, cut off at the slot), and keeps every ring until
  it exits. `publish` writes the slot, then `latest`. Python has no atomic
  store, so `latest` is a plain aligned 4-byte store; the `frame` message
  that follows is a system call, so a launcher that reads the ring after the
  message sees every write.
- **From the launcher**: reads the next message and compares it with
  `anyValueFor`. It answers `ping` with `pong`, and skips `pong` and
  `input`, unless the step names them. It also checks the launcher's own
  rules: framing and [Values](protocol.md#values), nothing but `hello`
  before `welcome`, one `hello`, nothing after `unsupported`, and call and
  ping ids counting up. It waits `--timeout` seconds (default 30).
- **`pollWithin`**: nothing. The reply that follows already matches, so the
  launcher's first call is the last.
- **`silence`**: reads nothing and answers nothing. **`wait`**: answers pings
  and `status` calls only. **`hostExits`**: exits with that code.
- **`launcherKillsHost`**, and the end of a session whose outcome is not
  `clean`: waits `--kill-within` seconds (default 10) to be ended, and fails
  after that. End-of-stream is ignored then.
- **`launcherDraws` and `launcherReports`** check the launcher's own API, so
  the fake only records that it reached them. To hold it there while the test
  checks, pass `--hold <seconds>` or `--resume <file>`: the fake then waits
  until the file holds a step index at least that step's. While it waits, a
  call to a method it has already answered gets the same reply again.
- **The end of a `clean` session**: waits for `shutdown` or end-of-stream,
  then exits with 0.

`--timeout`, `--kill-within`, `--hold` and `--resume` can also come from
`ANYPS5_FAKE_HOST_TIMEOUT`, `ANYPS5_FAKE_HOST_KILL_WITHIN`,
`ANYPS5_FAKE_HOST_HOLD` and `ANYPS5_FAKE_HOST_RESUME`.

The report is written again at every step, so a fake that was killed still
shows how far it got:

```json
{"session": "hung-call", "passed": true, "failures": [], "step": 7, "steps": 10, "done": false,
 "awaitingKill": true, "killed": false, "reached": [], "stepStartedAt": 1794548238.55}
```

`step` is the index of the step the fake is at, and `stepStartedAt` the Unix
time it got there. `passed` is true when there are no `failures` and the fake
either finished (`done`) or is at a point where being ended is the expected
end (`awaitingKill`): a `silence` or `wait` before `launcherKillsHost`, or a
last host step in a session whose outcome is not `clean`. `killed` is true
when the fake saw the started process end (a shim, on Windows). `reached`
lists the `launcherDraws` and `launcherReports` steps reached. On a failure
the fake writes the report and exits with 3. `passed` says the fake saw what
the session expects; the test still checks the outcome on the launcher side.
