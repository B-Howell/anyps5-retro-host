<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# anyps5-retro-host protocol, version 1

This document is the whole contract between an AnyPS5 launcher (the Flutter
launcher on Windows and Linux, the Swift launcher on macOS) and
`anyps5-retro-host`, the separate program that runs emulator cores. Either
side can be written from this document and the fixtures in
[`fixtures/v1`](../fixtures/v1) alone.

Version 1 runs one PS1 game for local players: start, play, change discs,
change settings, stop. The `ps1Lockstep` capability adds the first step of
online play: a game that runs one frame per input the launcher hands it, and
can save and load its state, so two players' hosts stay frame for frame alike
(see [Lockstep](#lockstep) and [netplay.md](netplay.md)). Rollback, PS2 and
PS3 are later capabilities (see [Capabilities](#capabilities)).

The words MUST, MUST NOT and SHOULD are used as in RFC 2119.

## Licences

| Part | Licence |
| --- | --- |
| This document, the fixtures and the fixture checker | MIT |
| `anyps5-retro-host` | GPL-2.0-or-later |
| SwanStation (the only core version 1 loads) | GPL-3.0, [libretro/swanstation](https://github.com/libretro/swanstation), with the host's patches in [`cores/patches`](../cores/patches) |

The launcher never links the host or a core, and shares no code with the
host. A launcher MAY copy code from this document or the fixtures. Each host
release names the SwanStation commit it was tested with, and the distributed
SwanStation build is made from that commit's source with the host's
SwanStation patches applied ([host.md](host.md) "SwanStation patches"). A
launcher runs only the SwanStation a host release ships: the patched core's
states are not stock SwanStation's.

The host is GPL-2.0-or-later so that one host can later run GPL-3.0 cores
(SwanStation, LRPS2) and GPL-2.0-only cores (RPCS3). It only ever loads one
core at a time. A core's patches, and the repository and commit they apply
to, are published with the host.

## Processes

The launcher starts the host the first time the player opens a PS1 game. A
player who never opens one never starts it. One host runs games one after
another until the launcher exits or the host fails.

The host has no window, no console and no dialog boxes. It draws only into the
frame ring. The **launcher's window is the target for screenshots, capture,
Remote Play and focus** for an emulated game. The host plays the game's audio
itself, so per-process audio capture uses the host's process id.

**Process id.** The launcher uses the process id it got when it started the
host (from `CreateProcessW` or `posix_spawn`) for audio capture, focus and
killing. It never uses the `pid` in [`welcome`](#welcome) for those. A
`welcome` whose `pid` is not that process id is a protocol error.

**Network.** In version 1, the host and the core MUST NOT open any network
connection or listening socket. This holds for online play too: the launcher
carries every packet between players, and the host only runs the frames it is
given ([netplay.md](netplay.md), "Who owns the network"). The only channel the host has is the one the
launcher hands it. Reading a game or BIOS file that the operating system
serves from a network share (SMB, NFS) is ordinary file access, not a
network connection under this rule, and neither is playing sound through the
system's sound service over its local socket (PulseAudio or PipeWire on
Linux). The host repository's CI traces the host's network calls with
`strace -f -e trace=%network` on Linux while it runs the conformance
sessions, and fails on any `AF_INET`, `AF_INET6`, `AF_PACKET` or
`AF_NETLINK` socket, and on any `bind` or `listen`.

### Starting the host

| OS | Control stream | Command line |
| --- | --- | --- |
| Windows | Two anonymous pipes, one per direction | `--ipc-read <handle> --ipc-write <handle> --parent-pid <pid>` |
| Linux | `socketpair(AF_UNIX, SOCK_STREAM \| SOCK_CLOEXEC)` | `--ipc-fd 3 --parent-pid <pid>` |
| macOS | `socketpair(AF_UNIX, SOCK_STREAM)` with `FD_CLOEXEC` set on both ends | `--ipc-fd 3 --parent-pid <pid>` |

`<pid>` is the launcher's own process id, in decimal.

- **Windows.**
  - The launcher creates both pipe ends not inheritable. Just before it
    starts the host it makes the host's two ends inheritable, and passes
    exactly those two handles with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, so
    no other handle is inherited. Straight after `CreateProcessW` returns, it
    makes them not inheritable again and closes its copies. Handle values
    are decimal.
  - While the two ends are inheritable, no other process start in the
    launcher may take a copy. So every process the launcher starts with
    inherited handles passes its own `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, and
    one that cannot is started under the same lock as the host, never while
    the two ends are inheritable.
  - The host also opens the launcher with `SYNCHRONIZE` only and treats the
    launcher's exit like end-of-stream, so a copy of a pipe end left anywhere
    else cannot keep it running.
  - The host is started with `CREATE_SUSPENDED`, assigned to a job object
    with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` that only the launcher holds,
    and only then resumed. So the host never runs outside the job, and it
    ends when the launcher ends.
  - The host calls `SetErrorMode(SEM_FAILCRITICALERRORS |
    SEM_NOGPFAULTERRORBOX)` before loading a core.
- **Linux.**
  - The launcher's end keeps `SOCK_CLOEXEC`. The host's end is moved to
    descriptor 3 in the child (`posix_spawn_file_actions_adddup2`, which
    clears close-on-exec on the copy). The launcher spawns with
    `POSIX_SPAWN_SETSIGMASK` (an empty mask) and `POSIX_SPAWN_SETSIGDEF`
    (`SIGTERM` back to its default), so a signal the launcher blocks or
    ignores is not blocked in the host.
  - At start the host closes every descriptor above 3 (`close_range(4,
    ~0U, 0)`, or a loop over `/proc/self/fd` where that is missing),
    unblocks every signal, then calls `prctl(PR_SET_PDEATHSIG, SIGTERM)`.
    It then exits at once if `getppid()` is not `--parent-pid`. `SIGTERM`
    is handled like [`shutdown`](#shutdown).
  - `PR_SET_PDEATHSIG` fires when the thread that started the host exits,
    so the launcher MUST start the host from a thread that lives as long as
    the launcher.
- **macOS.**
  - The launcher spawns with `POSIX_SPAWN_CLOEXEC_DEFAULT` and a dup2 action
    for descriptor 3, so only that descriptor reaches the host. It also sets
    `POSIX_SPAWN_SETSIGMASK` and `POSIX_SPAWN_SETSIGDEF` as on Linux.
  - Both sides set `SO_NOSIGPIPE` on their end.
  - The host watches `--parent-pid` with a kqueue `EVFILT_PROC` /
    `NOTE_EXIT` filter and treats the launcher's exit like end-of-stream. If
    registering the filter fails with `ESRCH`, the launcher is already gone,
    and the host exits at once.
  - macOS has no `MSG_CMSG_CLOEXEC`, so each side sets `FD_CLOEXEC` on a
    received descriptor straight after `recvmsg` returns it.
- **Linux and macOS** send with `MSG_NOSIGNAL` where it exists, so a closed
  peer gives an error, not `SIGPIPE`.

### Finding and checking the host

A release launcher runs only a host it ships with or downloads into its own
storage: from its own directory on Windows and Linux, and from
`Contents/Helpers` in the app bundle on macOS. It finds its own directory with
`GetModuleFileNameW`, `/proc/self/exe` or `_NSGetExecutablePath`.

**Signed files.** A host release is signed the way launcher releases are:

- `SHA256SUMS` lists one file a line as `shasum` writes it, `<64 hex
  digits>  <file name>`, with names that contain no `/` or `\`.
- `SHA256SUMS.sig` holds `ed25519 <key id> <base64 signature>` lines: Ed25519
  signatures of `SHA256SUMS` by the release keys built into the launcher.
  One good signature by a built-in key is enough.
- For each system the release has `anyps5-retro-host-<version>-<os>-<arch>.zip`,
  the host and SwanStation, and
  `anyps5-retro-host-<version>-<os>-<arch>.files`, which lists every file in
  that zip in the same `shasum` format, with `/` between directories.
  `<version>` is the host's release, as in [`welcome`](#welcome).
- The launcher matches entries by exact file name. It looks only for the
  names of its own system.

**When it checks.**

- *On download*: it checks the signature, then the zip and the `.files`
  against `SHA256SUMS`, and only then unpacks the zip. It keeps
  `SHA256SUMS`, `SHA256SUMS.sig` and the `.files` beside the unpacked host.
- *Before each start*: it checks the signature again, the `.files` against
  `SHA256SUMS`, and the host executable and the SwanStation library against
  the `.files`.
- *Minimum version*: each launcher release has a minimum host version built
  in. It does not use an older host, even a signed one, so an old signed
  host with a known fault cannot be put back in its place.

On Windows the launcher hashes each file from a handle it opened without
`FILE_SHARE_WRITE` or `FILE_SHARE_DELETE`, and keeps those handles open
until the first game's `status` shows `ready` or `ended`; the host has loaded
the core by then. So the file it checked is the file that runs. On Linux the launcher hashes the host executable from a
descriptor it opened with `O_CLOEXEC` at a number above 3 (moved there with
`F_DUPFD_CLOEXEC` if it came out lower, so the `dup2` to descriptor 3 cannot
replace it), and starts the host through that same
descriptor (`/proc/self/fd/<n>` as the path given to `posix_spawn`; the
kernel opens it before close-on-exec descriptors are closed, so the
descriptor itself never reaches the host), so the file
it checked is the file that runs. On macOS the system itself checks the code
signature of the host and of the SwanStation library as they are loaded (see
macOS signing below), so a changed file does not run. The SwanStation
library on Linux is checked by path in the launcher's own storage, which only
the user can write.

These checks protect against a damaged download and a changed mirror, not
against another program the same user runs, which could change the launcher
itself.

If any check fails, the launcher does not start the host and shows that the
emulator needs to be downloaded again.

A release launcher only ever runs the SwanStation library from the signed
host release. It never runs a core the player picks. Debug builds of the
launcher MAY run the host named by the environment variable
`ANYPS5_RETRO_HOST`, and any core, without the checks. Release builds ignore
that variable.

**macOS signing.** The host is signed with Developer ID, the hardened runtime
and library validation, and notarized. The SwanStation library in a macOS
host release is signed by the same team, so library validation lets the host
load it without the `disable-library-validation` entitlement.

## Framing

Each message is a 4-byte little-endian unsigned length followed by that many
bytes of UTF-8 JSON. The JSON is one object. The length MUST be between 2 and
1,048,576 (1 MiB). Each side handles messages in the order they arrive.

On Linux and macOS, only a [`ring`](#ring) message carries a file descriptor.
It carries exactly one, as `SCM_RIGHTS` data on a `sendmsg` whose data starts
with the first byte of the message's length. If that `sendmsg` sends only part
of the message, the rest goes with plain sends.

The receiver keeps each descriptor with the message it belongs to:

- every read is a `recvmsg` with room for 4 descriptors, using
  `MSG_CMSG_CLOEXEC` where it exists;
- a read never asks for more bytes than are left of the current message: 4
  for a length, then the body's remaining bytes;
- descriptors that arrive on the read holding the first byte of a length
  belong to that message. A descriptor on any other read is a protocol
  error.

A Unix socket never returns data sent before some ancillary data together
with that ancillary data in one read, so these rules keep each descriptor
with its message. If the stream ends in the middle of a message, the receiver
closes any descriptor that came with it.

A write that has not finished after 5 seconds is a failure of the peer: the
launcher handles it as a [host failure](#host-failure), and the host exits.

### Protocol errors

The following are **protocol errors**, on whichever side receives them.

Framing:

- a length outside the range above;
- a body that is not valid UTF-8 or not one JSON object (see [Values](#values));
- a message without a string `type`;
- a known message with a missing or wrongly typed field, or a value out of
  its range. A call's `args` is not checked here: a method argument that is
  wrong is answered with an `argument` error (see [Methods](#methods)), and
  the connection goes on;
- a descriptor on any message other than `ring`, or more than one descriptor;
- a control message other than `SCM_RIGHTS`, or truncated ancillary data
  (`MSG_CTRUNC`).

Order, as the host sees it:

- a first message that is not `hello`;
- a second `hello`.

Order, as the launcher sees it:

- a first message that is not `welcome` or `unsupported`, a second
  `welcome` or `unsupported`, or any message after `unsupported`;
- a `welcome` whose `version` or `pixelFormat` the launcher did not offer,
  or whose `pid` is not the host's process id;
- a `reply` whose `id` is not that of the oldest call still waiting for one;
- a `pong` whose `id` is not that of the oldest ping still waiting for one;
- a `frame` before any `ring`, or a [ring check](#checks-the-launcher-makes)
  that fails.

The launcher sends nothing but `hello` before `welcome`, and nothing after
`unsupported`. The host sends nothing before it has received `hello`.

The receiver closes every descriptor that came with the message in error.
When the launcher sees a protocol error, it kills the host and handles it as a
[host failure](#host-failure). A `welcome` with a version or pixel format the
launcher did not offer is the exception: the launcher kills the host and shows
the [update message](#unsupported), not the crash message. When the host sees
a protocol error, it stops its game the way `shutdown` does and exits.

## Compatibility

- Both sides MUST ignore a `type` they do not know, and MUST ignore object
  fields they do not know, in messages and in method arguments and results.
  Adding a message, a field or a method does not change the version.
- Both sides MUST ignore values they do not know in these lists: the entries
  of `pixelFormats` in `hello` and `unsupported`, and the entries of
  `capabilities` in `welcome`.
- An error `code` or a status `errorCode` the launcher does not know is
  handled as a failure without a known cause: a call that failed, or a game
  that ended with an error.
- Any other value outside what this document allows is out of range, so it
  is a protocol error.
- A change that an existing peer would misread raises the version.

## Values

JSON is read as [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) describes,
with these rules:

- **Objects.** A key that appears twice in one object, at any depth, is a
  protocol error, because JSON parsers disagree on which one wins.
- **Strings.** A string MUST be valid Unicode. An escape for a lone
  surrogate (such as `\ud800` with no low surrogate after it) is a protocol
  error.
- **Integers.** *Integer* means a JSON number whose value is a whole number.
  A sender writes it with no fraction or exponent, but a receiver decides by
  the value, so `1.0` and `1e0` are the integer 1. Each field gives its range.
  No range in version 1 goes beyond -2147483648 to 4294967295, so a 64-bit
  integer or a double holds every value exactly.
- **Numbers.** *Number* means a JSON number whose value is finite as a
  64-bit IEEE double. `1e400` is out of range.
- No binary data crosses the control stream in version 1. A game's saved
  state goes through a file the launcher names (see [saveState](#savestate-and-loadstate)).

### Paths

A *path* is a string naming a file or directory with an absolute path for the
host's system, with no NUL character:

- **Windows**: a drive path (`C:\Games\a.cue`), a UNC path
  (`\\nas\games\a.cue`), or either one in long form (`\\?\C:\Games\a.cue`,
  `\\?\UNC\nas\games\a.cue`). `/` is accepted as a separator in the first two
  forms. Device paths (`\\.\…`), drive-relative paths (`C:a.cue`) and
  root-relative paths (`\a.cue`) are not absolute.
- **Linux and macOS**: a path that starts with `/`.

A path MUST NOT contain a `.` or `..` segment or an empty segment (two
separators in a row, other than the `\\` that starts a UNC path). The launcher
resolves those before it sends a path. A path that breaks these rules is an
`argument` error.

The launcher sends paths as UTF-8. A Linux file name that is not valid UTF-8,
or a Windows file name with an unpaired surrogate, cannot be sent. The
launcher does not offer such a game to the player; if the player picks one,
it says that the file name cannot be used and sends nothing.

File extensions (`.cue`, `.CUE`) match without regard to ASCII case.

A file named inside a `.cue` or `.m3u` is found as the core finds it: a
relative name against the directory of the file that names it, and an
absolute or UNC name as it is.

## Messages

| Type | Direction | Reply |
| --- | --- | --- |
| [`hello`](#hello) | launcher → host | `welcome` or `unsupported` |
| [`welcome`](#welcome) | host → launcher | — |
| [`unsupported`](#unsupported) | host → launcher | — |
| [`call`](#call-and-reply) | launcher → host | `reply` |
| [`reply`](#call-and-reply) | host → launcher | — |
| [`input`](#input) | launcher → host | none |
| [`ping`](#ping-and-pong) | launcher → host | `pong` |
| [`pong`](#ping-and-pong) | host → launcher | — |
| [`shutdown`](#shutdown) | launcher → host | none, the host exits |
| [`ring`](#ring) | host → launcher | none |
| [`frame`](#frame) | host → launcher | none |
| [`rumble`](#rumble) | host → launcher | none |
| [`step`](#step) | launcher → host | none (capability `ps1Lockstep`) |
| [`sync`](#sync) | host → launcher | none (capability `ps1Lockstep`) |

### hello

The first message of a connection, sent by the launcher. The host sends
nothing before it.

```json
{"type":"hello","minVersion":1,"maxVersion":1,"pixelFormats":["rgba8","bgra8"]}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `minVersion`, `maxVersion` | integer, 1 to 4294967295, `minVersion` ≤ `maxVersion` | The protocol versions the launcher speaks |
| `pixelFormats` | array of strings | The frame pixel formats the launcher can draw, preferred first. Version 1 defines `rgba8` and `bgra8` |

### welcome

The host's answer when it speaks a version in the launcher's range and one of
its pixel formats.

```json
{"type":"welcome","version":1,"pid":4242,"hostVersion":"1.0.0","capabilities":["ps1"],"pixelFormat":"rgba8"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer, 1 to 4294967295 | The highest version both sides speak; used for the rest of the connection |
| `pid` | integer, 1 to 4294967295 | The host's process id. The launcher checks it (see [Processes](#processes)) |
| `hostVersion` | string | The host's release, for logs and error reports |
| `capabilities` | array of strings | What this host can do (see [Capabilities](#capabilities)) |
| `pixelFormat` | string | The pixel format of every frame ring this host will send: the first entry of `pixelFormats` that it supports |

Calls, input and pings MUST NOT be sent before `welcome`. The launcher's
first call after every `welcome` is [`volume`](#volume), with the player's
current volume, before any `start`.

### unsupported

The host's answer when there is no common version or pixel format. The host
exits after sending it.

```json
{"type":"unsupported","minVersion":1,"maxVersion":1,"pixelFormats":["rgba8"]}
```

The fields are the versions and pixel formats the host supports. The launcher
tells the player that the emulator host does not match this launcher and
needs updating. This is not the crash message.

### Capabilities

A capability names a group of methods and messages. The launcher MUST NOT use
a method whose capability the host did not list.

| Capability | Adds |
| --- | --- |
| `ps1` | The methods below, for PS1 games, with controller ports 1 and 2 |
| `ps1Lockstep` | [Lockstep](#lockstep): `start.lockstep`, the methods `lockstep`, `saveState` and `loadState`, and the messages `step` and `sync` |

Later versions add capabilities such as PS1 rollback, PS2 and PS3.

### call and reply

The launcher calls a [method](#methods):

```json
{"type":"call","id":7,"method":"pause","args":true}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | integer, 1 to 2147483647 | Unique for the connection; the launcher counts up from 1. Calls and pings count separately. A launcher about to run out shuts the host down once no game is loaded, and starts a new one for the next game |
| `method` | string | The method name |
| `args` | any | The method's argument; `null` when the method takes none. May be absent, which means `null` |

The host sends exactly one reply for each call, in call order:

```json
{"type":"reply","id":7,"result":null}
{"type":"reply","id":3,"error":{"code":"busy","message":"A game is already loaded."}}
```

A reply has either `result` (any JSON value) or `error`, never both. `error`
has a `code` string, which programs act on, and a `message`, written for a
log. These codes are defined:

| Code | Meaning |
| --- | --- |
| `unknownMethod` | The host has no method of that name |
| `argument` | The argument is missing, the wrong type, or outside the range this document gives |
| `busy` | `start` was called while a game is loaded |
| `start` | The game could not be started; `message` says why |
| `tooLarge` | The result would not fit in one message (1 MiB); this does not happen with reasonable data |

The host MUST reply to every call within 5 seconds. Long work, such as
loading a game, runs after the reply, and `status` reports how it is going.

### input

The controller state of one port. It has no reply. The host keeps the last
state of each port and gives it to the core on its next frame. The launcher
sends it when the state changes, and once with all zeros when a game starts
or input focus is lost.

```json
{"type":"input","port":0,"buttons":12,"leftX":0,"leftY":-32768,"rightX":0,"rightY":0}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `port` | integer, 0 or 1 | Controller port 1 or 2 of the console |
| `buttons` | integer, 0 to 65535 | One bit per button, from the table below |
| `leftX`, `leftY`, `rightX`, `rightY` | integer, -32768 to 32767 | The sticks. -32768 is fully left or fully up |

| Bit | Button | Bit | Button |
| --- | --- | --- | --- |
| 0 | Cross | 8 | Circle |
| 1 | Square | 9 | Triangle |
| 2 | Select | 10 | L1 |
| 3 | Start | 11 | R1 |
| 4 | D-pad up | 12 | L2 |
| 5 | D-pad down | 13 | R2 |
| 6 | D-pad left | 14 | L3 |
| 7 | D-pad right | 15 | R3 |

These are the libretro RetroPad bits (`RETRO_DEVICE_ID_JOYPAD_*`) as
SwanStation maps them. Each connected port is a DualShock
(`RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)`), so the sticks always
reach the game. Input for a port that is not connected (see
[`connectedPorts`](#connectedports)) is kept and used once the port is
connected.

### ping and pong

```json
{"type":"ping","id":12}
{"type":"pong","id":12}
```

`id` is an integer from 1 to 2147483647, counted up from 1 like call ids. The
host answers each ping from the same thread that answers calls, in order with
the replies, so pongs come in ping order. The launcher sends a ping every 2
seconds while the host runs, whether or not a call is waiting. See
[Timeouts](#timeouts).

### shutdown

```json
{"type":"shutdown"}
```

The host stops its game as [`stop`](#stop) does (memory cards are written),
then exits. The host MUST exit within 10 seconds of receiving it.

### ring

New frame memory, sent before the first frame that needs it. That is the
host's first frame, and any frame larger than the current ring's slots.

```json
{"type":"ring","bytes":3690496,"handle":1234}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `bytes` | integer, 4108 to 805310464 | The size of the ring in bytes. It MUST equal `4096 + 3 × slotBytes` from the ring's header |
| `handle` | integer, 1 to 4294967295 | Windows only: the section handle's value in the host process |

On Windows the launcher takes the section with `DuplicateHandle` from the
host process. It holds the host's process handle, which includes
`PROCESS_DUP_HANDLE`. On Linux and macOS the memory comes as the message's
descriptor. See [Frame ring](#frame-ring) for what the launcher checks before
using it.

### frame

```json
{"type":"frame"}
```

A new frame has been published in the current ring. The launcher's raster
thread reads the newest frame when it next draws. Frames can be skipped; only
the newest frame matters.

### rumble

```json
{"type":"rumble","port":0,"strong":65535,"weak":0}
```

The core's vibration for one port changed. `port` is 0 or 1. `strong` and
`weak` are integers from 0 to 65535, for the large and small motor. The
launcher forwards them to the controller seated at that port, if it can
vibrate. The launcher itself sets both motors of every port to 0:

- when the game ends, and when it calls `release` or `start`;
- when it pauses the game (`pause` with `true`) and when input focus is lost;
- when a controller leaves its port;
- on a [host failure](#host-failure).

The host sends nothing for those resets. While the game is paused or the
launcher does not have input focus, the launcher does not forward `rumble`.

## Methods

All methods below have the `ps1` capability, except `lockstep`, `saveState`
and `loadState`, which have `ps1Lockstep`. A call whose argument has the wrong
type, or a value outside the range given below, is an `argument` error
whether or not a game is loaded. With a valid argument and no game loaded,
`status` returns `null`; `pause`, `stop` and `release` do nothing and return
`null`; and `connectedPorts`, `changeDisc` and `option` return `false`.

### start

Loads and starts a PS1 game.

```json
{"core":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\retro-host\\swanstation_libretro.dll",
 "disc":"D:\\Games\\PS1\\FF7\\FF7.m3u",
 "system":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\retro\\system",
 "saves":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\retro\\saves\\ps1",
 "options":{"swanstation_GPU_ResolutionScale":"4","swanstation_GPU_TextureFilter":"Bilinear"},
 "initialDisc":1}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `core` | path | The SwanStation library the launcher checked |
| `disc` | path | A `.cue`, `.chd`, `.pbp` or `.m3u` file |
| `system` | path | The core's system directory (BIOS files) |
| `saves` | path | Where memory cards are kept; the host creates it if needed. Memory card 1 is the file `<disc's name without its extension>.srm` in it, as libretro frontends name it, and is checked for changes every 5 seconds between frames, also while paused, and written whenever the game ends. A card file the host cannot read is left as it is and never written over |
| `options` | object of strings, optional | Core option values saved by the player, by key, including the internal resolution (`swanstation_GPU_ResolutionScale`). Unknown keys and values are ignored |
| `initialDisc` | integer 0 to 127, optional | Only for an `.m3u`: the index of the playlist entry to insert at boot. With any other `disc` it is an `argument` error |
| `lockstep` | object, optional | Capability `ps1Lockstep`: the game starts in [lockstep](#lockstep), as the `lockstep` method's argument says, and runs not even its first frame before a `step` |

The result is `{"epoch":<integer>}`. The epoch identifies this game's frames
in the ring (see [Frame ring](#frame-ring)). It is 1 for the host's first
game and one more for each later game, so it is never reused in a host's
run. A host that has already started 2147483647 games answers `start` with a
`start` error.

After the reply the game loads in the background. `status` shows when the
first frame is ready, or that the game ended with an error. A playlist with
no entry at `initialDisc` ends the game this way, with `errorCode` `disc`.

Errors:

- `busy`, if a game is loaded, running or ended. The launcher calls
  `release` for an ended game before the next `start`.
- `argument`.
- `start`, if this host has already started its last game (see above).

The host replies before it opens the core or the disc, so a reply always comes
within the time limit. A core that cannot be loaded, or that is not a libretro
version 1 core, ends the game with `errorCode` `core`; a disc file that cannot
be opened ends it with `errorCode` `disc`.

### status

No argument. Returns `null` when no game is loaded, or:

```json
{"ready":true,"ended":false,"error":"","errorCode":"","aspect":1.3333333333333333,"hardware":true,
 "resolutionError":"","graphicsPending":false,"progressLabel":null,"progressPercent":null,
 "options":{"swanstation_GPU_ResolutionScale":{"title":"Internal Resolution","values":["1","2","4"],
   "labels":["1x","2x","4x"],"selected":2,"activeValue":"4","restart":false}},
 "discs":{"labels":["Disc 1","Disc 2"],"inserted":0,"insertedDiscPath":"D:\\Games\\PS1\\FF7\\FF7 (Disc 1).cue",
   "playedDiscPath":"D:\\Games\\PS1\\FF7\\FF7 (Disc 1).cue","busy":false,"failed":false}}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `ready` | boolean | The first frame has been published |
| `ended` | boolean | The game has ended (stopped, failed or the core quit); it stays loaded until `release` |
| `error` | string | Why the game ended, in English for logs, or `""` |
| `errorCode` | string | Why the game ended, for the UI to show in the player's language (table below), or `""` |
| `aspect` | number, greater than 0 and at most 10 | The display aspect ratio the core reports; 1.3333333333333333 until it reports one |
| `hardware` | boolean | The core draws with its OpenGL renderer, in a context the host made, not with its own software renderer. The OpenGL implementation itself may run on the CPU (Mesa's or Apple's software renderer on a machine with no GPU driver for it) |
| `resolutionError` | string | Why the last internal resolution could not be used, or `""` |
| `graphicsPending` | boolean | A graphics option is waiting to be applied |
| `progressLabel` | string or null | What is loading right now, while there is something to show |
| `progressPercent` | integer 0 to 100, or null | How far it is, when known |
| `options` | object | Each core option the player can change now, by key |
| `discs` | object, optional | Present for multi-disc games |

| `errorCode` | Meaning |
| --- | --- |
| `""` | The game has not ended, or ended without an error (it was stopped) |
| `bios` | No BIOS was found in `system`, the way the core looks for one. A file the core takes as a BIOS but that is damaged shows as `didNotStart` instead |
| `disc` | The disc, or a file it names, could not be read |
| `core` | The core could not be loaded, or failed while loading or running the game |
| `stalled` | The game stopped producing frames (see [Timeouts](#timeouts)) |
| `didNotStart` | The game produced no first frame in time |
| `frameTooLarge` | The game's frame was larger than 8192 × 8192 |
| `other` | Any other cause |

The host always sends every field above (`discs` only for a multi-disc
game), including for a game that ended before its first frame. The shorter
form in [Host failure](#host-failure) is never sent by a host; only the
launcher's client reports it.

Each option has `title` (string), `values` (array of strings), `labels`
(array of strings, the same length as `values`), `selected` (index into
`values` of the value chosen, which may still be pending), `activeValue` (the
value the host gives the core now; a core's own per-game compatibility
settings can still override it) and `restart` (true when a change only takes
effect when the game is started again; until then the host keeps giving the
core the value the game started with).

For the internal resolution (`swanstation_GPU_ResolutionScale`), while
`hardware` is `true`, `values` lists only the whole-number scales whose
largest frame the host can pass on (at most 8192 × 8192 and no larger than
the GPU can draw), and any value that is not a whole number. A saved value
larger than those starts the game at the largest one listed, and
`resolutionError` says why until the player chooses another value. Without
the GPU, or when not even the smallest scale fits, every value is listed; a
frame larger than 8192 × 8192 then ends the game with `frameTooLarge`.

`discs` has `labels` (array of strings), `inserted` (index of the inserted
disc, or null while the tray is open), `insertedDiscPath` (string or null),
`playedDiscPath` (the inserted disc's path once the game has run with it,
otherwise `""`), `busy` (a disc change is in progress) and `failed` (the last
change failed).

### pause

Argument: `true` to pause, `false` to resume. Returns `null`. Audio stops
while paused.

### connectedPorts

Argument: an integer, 1 or 2. This is how many controller ports have a
controller in, counting from port 1. With 1, port 2 has no controller
(`RETRO_DEVICE_NONE`), so a game offers a second player only once one has
joined. A game starts with 1. The core is told between frames. Returns `true`
when the count was taken, and `false` when the game has ended or is in
lockstep, where each `step` carries the count.

### changeDisc

Argument: an integer from 0 to 127, the index into `discs.labels`. It opens
the tray, inserts that disc and closes the tray, between frames. It returns
`true` when the change has started, and `discs.busy` then shows its progress.
It returns `false` for a game with one disc, for an index past the last
disc, while another change is in progress, when the game has ended, or while
it is in lockstep.

### option

Argument: `{"key":<string>,"value":<string>}`. Sets a core option that
`status` lists. Returns `true` when the value is one of the option's `values`
and was accepted (it may be pending, see `graphicsPending` and `restart`), and
`false` otherwise, and always while the game is in lockstep: every player's
console must keep the options it started with.

### volume

Argument: `{"level":<number 0 to 1>,"muted":<boolean>}`. Sets the host's
output volume, for this and every later game, until the host exits. A new
host starts at `{"level":1,"muted":false}`, and the launcher sends the
player's volume after every `welcome` (see [welcome](#welcome)). Returns
`null`. This method works whether or not a game is loaded.

### stop

No argument. Asks the game to end. The host writes the memory cards and
unloads the core, then `status` shows `ended: true`. Returns `null` at once.
The game MUST end within 5 seconds of the call.

### release

No argument. Forgets an ended game, so `status` returns `null` and a new
`start` can follow. It does nothing while the game is still running. Returns
`null`.

A game that ended by a [game limit](#timeouts) may have left its core stuck
inside `retro_run`, where the host cannot unload it. Then the host sends the
reply to `release` and exits. No game is loaded at that point, so the
launcher does not show an error, and it starts a new host for the next game.

## Lockstep

Capability `ps1Lockstep`. A game in lockstep runs one frame for each `step`
the launcher sends, with exactly the input in it, and runs no frame without
one. Two hosts that start the same game with the same core, BIOS and options,
or that load the same saved state, and are given the same steps, stay alike
frame for frame. Each sends a [`sync`](#sync) with hashes of what the frame
left behind, so the launchers can compare them. The launcher does all the
networking; [netplay.md](netplay.md) is the design this is the first step of.

Lockstep frames count up from 0, from the moment the game enters lockstep
(`start.lockstep`, or the `lockstep` method on a running game), and are
integers from 0 to 4294967295. Every frame of a game in lockstep is a frame
run for a step, so:

- `input` messages are kept, as always, but reach the core only after the
  game leaves lockstep;
- `connectedPorts`, `changeDisc` and `option` return `false`;
- `pause` still works. A paused game takes steps but runs none until it is
  resumed;
- time spent waiting for the next step does not count toward the stall and
  first-frame [game limits](#timeouts), just as time paused does not.

### lockstep

Argument: `{"check":<integer 1 to 3600>,"paced":<boolean, optional>}`, or
`false`.

- With an object, the game enters lockstep, or, when it already is in
  lockstep, keeps its frame count and takes the new settings. `check` is how
  often a `sync` is sent: after every frame whose number is a multiple of it.
  `paced` (default `true`) keeps the game to its core's frame rate even when
  steps queue up; `false` runs queued steps as fast as the machine can, as a
  player catching up does. The change takes effect between two frames: the
  host waits for the frame being run to end before it replies. Returns
  `{"frame":<integer>}`, the frame the next `step` is for, which is 0 when the
  game has just entered lockstep. Returns `false` when the game has ended, or
  when the frame being run did not end within 4 seconds.
- With `false`, the game leaves lockstep and drops the steps it has not run.
  It then runs on its own, with the input of the last `input` messages, and
  with as many connected ports as the last step it ran had. It too takes
  effect between two frames. Returns `true`, or `false` when the game has
  ended or the frame being run did not end within 4 seconds.

`start.lockstep` takes the same object.

### saveState and loadState

Argument: `{"path":<path>}` or `{"slot":<integer 0 to 7>}`, not both. Both
are carried out between two frames, before any step still queued.

- **A file**, `path`, is for a state that leaves the host: the one a friend
  joins from. The launcher chooses the file and later deletes it.
- **A slot** is one of 8 states the game keeps in memory, for rollback: the
  launcher saves one before each frame it runs on a guessed input and loads
  it back when the guess was wrong ([netplay.md](netplay.md) "Lockstep first,
  rollback after"). A slot belongs to the game and to its lockstep count: it
  is lost when the game ends, and a slot saved before the game last entered
  lockstep does not load. A new save in a slot replaces what it held.

- `saveState` saves the core's state with the next lockstep frame's number:
  the frame the state will continue with, and the number of connected ports.
  Returns `{"frame":<integer>,"bytes":<integer>,"micros":<integer>}`: `bytes`
  is the file's size, or the size of the core's data in a slot, and `micros`
  how long the host spent saving it, in microseconds.
- `loadState` connects the ports the state was saved with (a port count
  `connectedPorts` would set), loads the state into the game, drops the steps
  not yet run, and sets the lockstep frame count to the frame in the state,
  so the next `step` is for that frame. A file whose frame is above
  4294967295 is not loaded. Returns the same object, with how long the load
  took.

Both return `false` when the game is not in lockstep or has ended, when the
file cannot be written or read, when the slot holds no state of this lockstep
count, or when the core refuses the state. They also
return `false` when the game's thread does not get to them within 4 seconds,
which can happen to a game started with `start.lockstep` that is still
loading: such a game shows no `ready` before its first step, so a launcher
that wants to load a state into a new game starts it without `lockstep` and
waits for `ready` first. A
`loadState` that the core refused, or that did not finish, leaves the console
in no known state: a launcher ends the match then. A load that started but
did not finish within 4 seconds ends the game with `errorCode` `other`.

The file is the 48-byte header below, then the core's
`retro_serialize` data. Integers are little-endian.

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 8 | The bytes `AP5STATE` |
| 8 | 4 | 2, the file's version |
| 12 | 4 | 1 when offset 24 holds the last picture's hash, else 0 |
| 16 | 8 | The lockstep frame the state continues with |
| 24 | 8 | The `video` hash of the last picture before the state was saved |
| 32 | 8 | The size of the core's data that follows |
| 40 | 4 | The number of connected ports, 1 or 2 |
| 44 | 4 | 0 |

While the host saves or loads a file, or hashes the state for a
[`sync`](#sync), it tells the core the state is for netplay
(`RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT` answers
`RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY`); for a slot it answers
`RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE`, the state never leaving
the process; at any other time `RETRO_SAVESTATE_CONTEXT_NORMAL`. With the
host's SwanStation patches, a state of either context:

- names no file or folder: the disc is named by its place in the game's disc
  list, so a state from another machine loads whatever the disc's file is
  called there, and tells that machine nothing of where the first one keeps
  its files;
- brings back each controller's buttons, analog mode, whether the game
  locked that mode, and whether the analog-mode button combination is still
  held, so a loaded game toggles analog mode at the same frames as the game
  it was saved from.

A state holds the console's memory cards. A launcher that loads a state from
another player starts that game with a `saves` directory of its own for the
match, so the host never writes that player's memory card over the
player's own.

### step

```json
{"type":"step","frame":120,"ports":[{"buttons":1,"leftX":-32768,"leftY":0,"rightX":0,"rightY":0},
 {"buttons":0,"leftX":0,"leftY":0,"rightX":0,"rightY":0}]}
```

The input of one lockstep frame. It has no reply.

| Field | Type | Meaning |
| --- | --- | --- |
| `frame` | integer, 0 to 4294967295 | The lockstep frame this is for |
| `ports` | array of 1 or 2 objects | One per connected controller port, from port 1. Each has the fields of [`input`](#input) other than `type` and `port`. The number of entries is the number of connected ports for this frame, as [`connectedPorts`](#connectedports) would set it |

Steps come in frame order with no gaps: the first is for the frame the
`lockstep` reply or the last `loadState` named, and each next one for the
frame after. At most 600 steps wait to be run. A `step` sent while no game is
loaded, while the game is not in lockstep, for any other frame, or as the
601st waiting, is a protocol error. A `step` for a game that has ended is
ignored.

### sync

```json
{"type":"sync","frame":120,"ram":"8f67e12ab88b0626","video":"2a2019e82fedaa1a","state":"edcd2ac6e2993c68"}
```

Sent after each lockstep frame whose number is a multiple of `check`, in
frame order. Each hash is the 64-bit FNV-1a hash of some bytes, as 16
lowercase hexadecimal digits, or `null` when the core cannot give them:

| Field | The hash of |
| --- | --- |
| `ram` | The console's main RAM (`RETRO_MEMORY_SYSTEM_RAM`) after the frame |
| `video` | The last picture published by this frame or an earlier one: its width and height as two 32-bit little-endian integers, then the red, green and blue bytes of each pixel, top row first, whichever pixel format the ring has. `null` before the first picture |
| `state` | The core's `retro_serialize` data after the frame, in a buffer of `retro_serialize_size` bytes that starts zeroed |

Two hosts that ran the same steps from the same start have the same `ram`
and `state`, wherever each keeps the disc: the state is saved for netplay, so
it names no path ([saveState and loadState](#savestate-and-loadstate)).
`video` is the same on two machines only when both draw in software; GPUs may
round differently. For the same reason `state`, which holds the console's
video memory read back from the GPU, may differ between two GPUs drawing at a
resolution scale above 1x.

## Frame ring

The ring is one block of shared memory: a 4096-byte header, then 3 slots of
`slotBytes` bytes. All integers are little-endian. Every platform AnyPS5 runs
on is little-endian.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `0x46523541` (the bytes `A5RF`) |
| 4 | 4 | `version` | 1 |
| 8 | 4 | `slotCount` | 3 |
| 12 | 4 | `headerBytes` | 4096 |
| 16 | 8 | `slotBytes` | A multiple of 4, from 4 to 268,435,456 |
| 24 | 4 | `latest` | Index of the newest frame's slot, or `0xFFFFFFFF` for none |
| 28 | 4 | `reading` | Index of the slot the launcher is reading, or `0xFFFFFFFF` |
| 32 | 4 | `pixelFormat` | 1 = `rgba8`, 2 = `bgra8`; as agreed in `welcome` |
| 36 | 4 | reserved | 0 |
| 40 + 24 × *i* | 4 | slot *i* `width` | 1 to 8192 |
| 44 + 24 × *i* | 4 | slot *i* `height` | 1 to 8192 |
| 48 + 24 × *i* | 4 | slot *i* `visible` | 1 when the frame has any pixel that is not black, else 0 |
| 52 + 24 × *i* | 4 | slot *i* `epoch` | The `epoch` of the game this frame belongs to |
| 56 + 24 × *i* | 8 | slot *i* `sequence` | Counts frames up from 1, over the host's whole run |
| 112 | 3984 | reserved | 0 |
| 4096 + *i* × `slotBytes` | `slotBytes` | slot *i* pixels | `height` rows of `width` × 4 bytes, no padding, top row first |

The ring is never larger than 4096 + 3 × 268,435,456 = 805,310,464 bytes.
A game whose frame would be wider or taller than 8192 pixels ends with
`errorCode` `frameTooLarge` instead of being drawn.

`latest` and `reading` are 4-byte aligned and accessed with sequentially
consistent atomic operations. The other header fields are plain memory. They
are ordered by the atomics, which works because each slot has one writer and
one reader at a time.

**Host (writer).** It picks a slot that is neither `latest` nor `reading`.
There is always one, because there are three slots. It writes the pixels and
the slot's fields, then stores the slot's index in `latest`. The host never
waits. Starting or releasing a game sets `latest` to none before the reply.

**Launcher (reader).** To read a frame:

1. It loads `latest` once. If that is `0xFFFFFFFF` there is no frame, and
   the launcher keeps showing what it showed before. Any value other than 0,
   1, 2 or none is a protocol error.
2. It stores the value in `reading` and loads `latest` again. If the two
   loads agree, the slot is pinned. If they still disagree after three
   tries, it skips this draw and tries again on the next one.
3. It loads the pinned slot's `width`, `height` and `epoch` once each, into
   its own copies, and checks those copies. From here on it uses only them.
4. It draws the frame only when its copy of `epoch` is the one `start`
   returned for the current game. A frame of an earlier game is skipped, and
   is not an error.

A pinned slot is never written. The launcher holds one pin at a time, until
its next read. It never reads `reading` back, and it never trusts any other
header field after the ring's arrival. A host that writes into a pinned slot
anyway can only spoil the picture: every offset and length the launcher uses
comes from its own checked copies.

### Checks the launcher makes

A failed check, or a failed `DuplicateHandle`, `MapViewOfFile` or `mmap`, is
a protocol error.

**When a ring arrives**, before using it, in this order:

1. **The memory.**
   - Windows: `DuplicateHandle` with `FILE_MAP_READ | FILE_MAP_WRITE` as the
     access (not `DUPLICATE_SAME_ACCESS`); `MapViewOfFile` of exactly `bytes`
     bytes; and `VirtualQuery` on the view shows `MEM_COMMIT` and a region of
     at least `bytes` bytes. A section made with `SEC_RESERVE` fails this
     check.
   - Linux: `fstat` gives exactly `bytes`, and `F_GET_SEALS` includes
     `F_SEAL_SHRINK` and `F_SEAL_SEAL`. Then `mmap` of `bytes` bytes.
   - macOS: `fcntl(fd, F_GETFL)` shows `O_RDWR`; `fstat` gives at least
     `bytes` and less than `bytes` plus one page (macOS rounds a shared memory
     object up to whole pages); and `ftruncate(fd, bytes)` fails. A POSIX
     shared memory object on macOS takes only its first `ftruncate` and
     refuses every later one. A writable regular file would accept the call,
     so this shows the object's size cannot change. Then `mmap` of `bytes`
     bytes.
2. **The header**, each field loaded once into the launcher's own copy:
   - `magic`, `version`, `slotCount`, `headerBytes` and `pixelFormat` have
     the values above;
   - `slotBytes` is a multiple of 4 from 4 to 268,435,456. Only then does
     the launcher compute `4096 + 3 × slotBytes`, in 64 bits, and check that
     it equals `bytes`.
3. **Growth.** When there is a current ring, the new `slotBytes` is larger
   than the current one, and at least twice it or 268,435,456, whichever is
   smaller. So a host can send at most 27 rings, and a ring at the limit is
   the last.

The launcher keeps its copies of `slotBytes` and `bytes` and never reads the
header's geometry again.

**For each frame**, on its copies of the pinned slot's fields: `width` and
`height` are 1 to 8192, and `width × height × 4` is at most the kept
`slotBytes`. This is computed in 64 bits; it cannot overflow with those
limits.

### Memory objects

| OS | Created with | Notes |
| --- | --- | --- |
| Windows | `CreateFileMappingW(INVALID_HANDLE_VALUE, …, PAGE_READWRITE \| SEC_COMMIT, …)`, unnamed | A section never shrinks |
| Linux | `memfd_create(…, MFD_CLOEXEC \| MFD_ALLOW_SEALING)`, `ftruncate`, then `F_ADD_SEALS` with `F_SEAL_SHRINK \| F_SEAL_GROW \| F_SEAL_SEAL` | Sealing stops a truncate that would give the launcher `SIGBUS` |
| macOS | `shm_open(name, O_RDWR \| O_CREAT \| O_EXCL, 0600)` with a random name of at most 30 characters, such as `/a5r-` and 12 random letters; `shm_unlink` at once; one `ftruncate` to the ring's size | The size is fixed after that `ftruncate`; the launcher checks it as above, and the host's macOS CI tests it |

A ring only grows. When a frame needs more room, the host makes a new ring,
sends `ring`, and publishes into the new one. The new `slotBytes` is the
larger of twice the old one and what the frame needs, capped at the limit,
which is what the launcher's [growth check](#checks-the-launcher-makes)
expects. The host keeps every ring it has announced until it exits, so on
Windows the launcher can always still duplicate the handle.

Both sides map the whole ring for reading and writing, shared:
`MapViewOfFile(…, FILE_MAP_READ | FILE_MAP_WRITE, …)` on Windows, and
`mmap(…, PROT_READ | PROT_WRITE, MAP_SHARED, …)` elsewhere. The launcher
unmaps the old ring once it holds no pin in it, so it maps at most two rings
at a time.

A zero-copy GPU path (a D3D11 shared texture, a dmabuf) may be added later
as another capability. The macOS mechanism for it (IOSurface) is undecided.

## Timeouts

**Process limits.** Missing one is a [host failure](#host-failure). A host
that misses its own limit ends its process at once.

| What | Limit |
| --- | --- |
| `welcome` or `unsupported` after the host starts | launcher: 10 s |
| A `reply` to a call | launcher: 5 s after the call |
| A `pong` to a ping | launcher: 5 s after the ping |
| A write on the control stream finishes | launcher and host: 5 s |
| The host exits after `shutdown` | host: 10 s; the launcher kills it at 15 s |
| The host exits after end-of-stream or the launcher's exit | host: 10 s |
| A game ends after `stop` | host: 5 s; the launcher kills the host at 10 s |

**Game limits.** These end only the game. The host keeps running, and
`status` shows `ended: true` with the `error` and `errorCode` given. Time
paused, and time a game in [lockstep](#lockstep) waits for its next step, is
not counted.

| What | Limit | `error` | `errorCode` |
| --- | --- | --- | --- |
| A running game's core produces no frame after its first one | 10 s | "The emulator stopped responding." | `stalled` |
| A game publishes its first frame after `start` | 60 s | "The game did not start." | `didNotStart` |

A frame here is a call of the core's video callback. A duplicate frame
(libretro passes no pixels because nothing changed) counts as a frame, though
the host publishes nothing new for it.

The launcher MUST NOT read, write or wait on the control stream, or wait for
the host to exit, on its UI thread or raster thread.

## Host failure

The host has failed if it exits without being asked, sends a protocol error,
or misses a launcher limit. The launcher then kills it if it is still
running. If a game was loaded:

- the launcher's client reports `status` as `{"ready":false,"ended":true,
  "error":"The emulator closed unexpectedly.","errorCode":"hostExited"}`, or
  with `"error":"The emulator stopped responding and was closed."` and
  `"errorCode":"hostHung"` after a missed limit. `pause`, `stop`, `release`
  and `volume` report `null`; `connectedPorts`, `changeDisc` and `option`
  report `false`. This lasts until the UI calls `release`.
- The UI shows the error and goes back to the library the same way it does
  for any game that has ended. The next game starts a new host.

A launcher that gets `unsupported`, or a `welcome` with a version or pixel
format it did not offer, shows its own message. That message says the
emulator needs updating, and is not the crash message.

## Fixtures

[`fixtures/v1`](../fixtures/v1) is the test data for both conformance tests:
a fake launcher that tests a host, and a fake host that tests a launcher.
Every file is one JSON object with a `description`.

**Paths.** A string `${name}` stands for a real file or directory that the
test provides on its own system:

- `${core}`: SwanStation;
- `${disc}`: a one-disc game;
- `${playlist}`: an `.m3u` of two discs, `${disc1}` and `${disc2}`;
- `${system}` and `${saves}`: directories.

**`messages/`** has one file per message and per method:

- `from` is `launcher` or `host`, and `message` is the message as sent;
- a call has its `reply`, and a ping has its `pong`;
- `os` is `windows` or `posix` when the message exists only on those systems;
- `anyValueFor` lists fields whose value differs between real runs, such as
  `pid`. A test checks only that those fields are present and have the right
  type. A name with a dot, such as `error.message`, names a field inside an
  object;
- `invalid` names the rule a message breaks on purpose, such as a call with
  an argument out of range, and the reply is the host's answer to it.

**`sessions/`** has whole conversations:

- `tests` lists the sides the session tests, `host` and/or `launcher`. For
  each, a fake plays the other side.
- `os`, when present, is `windows`, `posix`, `linux` or `macos`: the session
  runs only there.
- Steps from the fake are sent as written. Steps from the side under test are
  what it must send. A fake host's `welcome` carries its real process id.
- The side under test may also send `ping`, `pong` and `input` at any time.
  A test ignores those unless a step names them. A host under test also
  sends `frame` (one per published frame) and `rumble` (as its core asks),
  so a test ignores those too unless the next step names one.
- When the host is under test, a `ring` step's `header` is checked only for
  `magic`, `version`, `slotCount`, `headerBytes`, `slotBytes` and
  `pixelFormat`. The host keeps publishing, so by the time a test reads the
  ring, the other fields have moved on. At each `frame` step the test checks
  the slot named by `latest`, as the [launcher's checks](#checks-the-launcher-makes)
  do.
- A host under test runs whatever core the test provides as `${core}`. The
  host repository's CI uses a small test core, not SwanStation. So a status
  reply that a host is tested on lists the fields that depend on the core
  (`result.options`, `result.hardware`, `result.aspect`) in `anyValueFor`.
- `steps` run in order. Each step is one of:

| Step | Meaning |
| --- | --- |
| `{"from":…,"message":{…}}` | That side sends the message. Optional additions: `anyValueFor`; `descriptors` (how many descriptors are attached: 0 by default, 1 for `ring`); on a fake host's `welcome`, `"wrongPid":true` (it sends a process id other than its own); and, on a fake host's `frame`, `publish`: `{"latest":<index>,"slot":{…}}`, which the fake writes into the current ring (the slot's fields, then `latest`, as the [host](#frame-ring) does) before it sends the `frame`. Without `publish`, a `frame` announces what the ring's `header` already holds |
| `{"from":…,"message":{…},"ring":{…}}` | A `ring` message. `ring` describes the shared memory: `memoryBytes` (the size of the memory: the fake gives it `bytes` with `ftruncate` or `CreateFileMappingW`, and in a `macos` session it may be the size macOS reports after rounding up to a whole 16 KiB page), `sealed` (whether the Linux seals are present; used only on Linux) and `header` (every header field named as in the [table](#frame-ring), with `slots` as an array of three objects). On Windows the fake host adds `handle` and sends no descriptor |
| `{"from":…,"bytes":"<hex>"}` | That side writes these raw bytes on the stream |
| `{"pollWithin":<seconds>}` | The launcher side repeats the call before this step, with a new `id` each time, until the reply matches the next step. This may take up to that many seconds |
| `{"hostExits":<code>}` | The host process ends with that exit code |
| `{"silence":<seconds>}` | The fake sends nothing for that long, and answers nothing, pings included |
| `{"wait":<seconds>,"status":<result>}` | The fake host sends nothing of its own for that long, but answers each ping, and each `status` call with that result. The launcher under test sends no other call during it |
| `{"launcherKillsHost":true}` | The launcher ends the host |
| `{"launcherDraws":<boolean>}` | Whether the launcher's client hands the frame announced by the last `frame` to its renderer: true exactly when that frame's `epoch` is the current game's |
| `{"launcherReports":{"method":…,"result":…}}` | What the launcher's client gives its UI for that method from then on |

A step with `"invalid":"<rule>"` breaks the named rule on purpose. The fixture
checker reports that rule for the step, and fails if the step breaks no rule
or a different one.

`outcome` is where the side under test ends up:

| Outcome | Meaning |
| --- | --- |
| `clean` | Everything is answered and the host exits with code 0 |
| `protocolError` | The side under test ends the connection as [Protocol errors](#protocol-errors) says: a host exits; a launcher kills the host |
| `unsupported` | The launcher reports the version or pixel format mismatch, not a crash |
| `hostFailure` | The launcher handles a [host failure](#host-failure) |

[`tools/check_fixtures.py`](../tools/check_fixtures.py) checks every message
in the fixtures against the fields, types, ranges and order in this document.
