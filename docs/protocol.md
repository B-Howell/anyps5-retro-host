<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# anyps5-retro-host protocol, version 1

This document is the whole contract between an AnyPS5 launcher (the Flutter
launcher on Windows and Linux, the Swift launcher on macOS) and
`anyps5-retro-host`, the separate program that runs emulator cores. Either
side can be written from this document and the fixtures in
[`fixtures/v1`](../fixtures/v1) alone.

Version 1 runs one PS1 game for local players: start, play, change discs,
change settings, stop. Online play, PS2 and PS3 are later capabilities (see
[Capabilities](#capabilities)).

The words MUST, MUST NOT and SHOULD are used as in RFC 2119.

## Licences

| Part | Licence |
| --- | --- |
| This document, the fixtures and the fixture checker | MIT |
| `anyps5-retro-host` | GPL-2.0-or-later |
| SwanStation (the only core version 1 loads) | GPL-3.0, [libretro/swanstation](https://github.com/libretro/swanstation), unmodified |

The launcher never links the host or a core, and shares no code with the
host. A launcher MAY copy code from this document or the fixtures. Each host
release names the SwanStation commit it was tested with, and the distributed
SwanStation build is made from that commit's source.

The host is GPL-2.0-or-later so that one host can later run GPL-3.0 cores
(SwanStation, LRPS2) and GPL-2.0-only cores (RPCS3). It only ever loads one
core at a time. Before a modified core is added, its patches and the
repository and commit they apply to are published with the host.

## Processes

The launcher starts the host the first time the player opens a PS1 game. A
player who never opens one never starts it. One host runs games one after
another until the launcher exits or the host fails.

The host has no window, no console and no dialog boxes. It draws only into the
frame ring. The **launcher's window is the target for screenshots, capture,
Remote Play and focus** for an emulated game. The host plays the game's audio
itself, so per-process audio capture uses the host's process id (`pid` in
[`welcome`](#welcome)).

**Network.** In version 1, the host and the core MUST NOT open any network
connection or listening socket. The only channel the host has is the one the
launcher hands it.

### Starting the host

| OS | Control stream | Command line |
| --- | --- | --- |
| Windows | Two anonymous pipes, one per direction | `--ipc-read <handle> --ipc-write <handle> --parent-pid <pid>` |
| Linux | `socketpair(AF_UNIX, SOCK_STREAM \| SOCK_CLOEXEC)` | `--ipc-fd 3 --parent-pid <pid>` |
| macOS | `socketpair(AF_UNIX, SOCK_STREAM)` with `FD_CLOEXEC` set on both ends | `--ipc-fd 3 --parent-pid <pid>` |

- **Windows.** The launcher makes the host's two pipe ends inheritable and
  passes exactly those two handles with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`,
  so no other handle is inherited. Handle values are decimal. The host is put
  in a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` that only the
  launcher holds, so the host ends when the launcher ends. The host calls
  `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` before
  loading a core.
- **Linux.** The launcher's end keeps `SOCK_CLOEXEC`. The host's end is moved
  to descriptor 3 in the child (`posix_spawn_file_actions_adddup2`, which
  clears close-on-exec on the copy). The host calls
  `prctl(PR_SET_PDEATHSIG, SIGTERM)`, then exits at once if `getppid()` is
  not `--parent-pid`. `SIGTERM` is handled like [`shutdown`](#shutdown).
  `PR_SET_PDEATHSIG` fires when the thread that started the host exits, so
  the launcher MUST start the host from a thread that lives as long as the
  launcher.
- **macOS.** The launcher spawns with `POSIX_SPAWN_CLOEXEC_DEFAULT` and a
  dup2 action for descriptor 3, so only that descriptor reaches the host. Both
  sides set `SO_NOSIGPIPE` on their end. The host watches `--parent-pid` with
  a kqueue `EVFILT_PROC` / `NOTE_EXIT` filter and treats the launcher's exit
  like end-of-stream.
- **Linux and macOS** send with `MSG_NOSIGNAL` where it exists, so a closed
  peer gives an error, not `SIGPIPE`.

### Finding and checking the host

A release launcher runs only the host it ships with or downloads into its own
storage: from its own directory on Windows and Linux, and from
`Contents/Helpers` in the app bundle on macOS. It finds its own directory with
`GetModuleFileNameW`, `/proc/self/exe` or `_NSGetExecutablePath`. Before each
start it checks the host executable and the SwanStation library against the
release's `SHA256SUMS`. That file is signed with the Ed25519 key that already
signs launcher updates. If the check fails, it does not start the host.

Debug builds of the launcher MAY run the host named by the environment
variable `ANYPS5_RETRO_HOST`, without the checks. Release builds ignore it.

## Framing

Each message is a 4-byte little-endian unsigned length followed by that many
bytes of UTF-8 JSON. The JSON is one object. The length MUST be between 2 and
1,048,576 (1 MiB). Each side handles messages in the order they arrive.

On Linux and macOS, only a [`ring`](#ring) message carries a file descriptor.
It carries exactly one, as `SCM_RIGHTS` data on a `sendmsg` whose data starts
with the first byte of the message's length. If that `sendmsg` sends only part
of the message, the rest goes with plain sends.

The receiver keeps each descriptor with the message it belongs to:

- every read is a `recvmsg` with room for one descriptor, using
  `MSG_CMSG_CLOEXEC` where it exists;
- a read never asks for more bytes than are left of the current message: 4
  for a length, then the body's remaining bytes;
- a descriptor that arrives on the read holding the first byte of a length
  belongs to that message. A descriptor on any other read is a protocol
  error.

A Unix socket never returns data sent before some ancillary data together
with that ancillary data in one read, so these rules keep each descriptor
with its message. If the stream ends in the middle of a message, the receiver
closes any descriptor that came with it.

The following are **protocol errors**:

- a length outside the range above;
- bytes that are not valid UTF-8 or not a JSON object;
- a message without a string `type`;
- a known message with a missing or wrongly typed field;
- a descriptor on any message other than `ring`;
- a `ring` message without exactly one descriptor;
- truncated ancillary data (`MSG_CTRUNC`).

The receiver closes every descriptor that came with the message in error.

When the launcher sees a protocol error, it kills the host and handles it as a
[host failure](#host-failure). When the host sees one, it stops its game the
way `shutdown` does and exits.

## Compatibility

- Both sides MUST ignore a `type` they do not know, and MUST ignore object
  fields they do not know, in messages and in method arguments and results.
  Adding a message, a field or a method does not change the version.
- A change that an existing peer would misread raises the version.

## Values

JSON types are used as they are: `null`, `true`/`false`, strings, arrays and
objects. *Integer* means a JSON number with no fraction or exponent; each
field gives its range. No range in version 1 goes beyond -2147483648 to
4294967295, so a 64-bit integer holds every value. *Number* means any JSON number. A path is an absolute path as a
UTF-8 string with no NUL character. No binary data crosses the control
stream in version 1.

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

### hello

The first message of a connection, sent by the launcher. The host sends
nothing before it.

```json
{"type":"hello","minVersion":1,"maxVersion":1,"pixelFormats":["rgba8","bgra8"]}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `minVersion`, `maxVersion` | integer, 1 or more | The protocol versions the launcher speaks |
| `pixelFormats` | array of strings | The frame pixel formats the launcher can draw, preferred first: `rgba8` and/or `bgra8` |

### welcome

The host's answer when it speaks a version in the launcher's range and one of
its pixel formats.

```json
{"type":"welcome","version":1,"pid":4242,"hostVersion":"1.0.0","capabilities":["ps1"],"pixelFormat":"rgba8"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | The highest version both sides speak; used for the rest of the connection |
| `pid` | integer, 1 to 4294967295 | The host's process id |
| `hostVersion` | string | The host's release, for logs and error reports |
| `capabilities` | array of strings | What this host can do (see [Capabilities](#capabilities)) |
| `pixelFormat` | string | The pixel format of every frame ring this host will send: the first entry of `pixelFormats` that it supports |

Calls, input and pings MUST NOT be sent before `welcome`.

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

Later versions add capabilities such as PS1 online play, PS2 and PS3.

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
{"type":"reply","id":3,"error":{"code":"busy","message":"A game is already running."}}
```

A reply has either `result` (any JSON value) or `error`, never both. `error`
has a `code` string, which programs act on, and a `message`, written for a
log. These codes are defined:

| Code | Meaning |
| --- | --- |
| `unknownMethod` | The host has no method of that name |
| `argument` | The argument is missing, the wrong type or out of range |
| `busy` | `start` was called while a game is loaded |
| `start` | The game could not be started; `message` says why |

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

`id` is an integer from 1 to 2147483647. The host answers from the same
thread that answers calls, in order with the replies. The launcher sends a
ping every 2 seconds while the host runs, whether or not a call is waiting. See [Timeouts](#timeouts).

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
{"type":"ring","bytes":1576960,"handle":1234}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `bytes` | integer, 4108 to 805310464 | The size of the shared memory in bytes. It MUST equal `4096 + 3 × slotBytes` from the ring's header |
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
vibrate. The launcher itself sets both motors of every port to 0 when the
game ends, when it calls `release` or `start`, when a controller leaves its
port, and on a [host failure](#host-failure). The host sends nothing for
those resets.

## Methods

All methods have the `ps1` capability. When no game is loaded, `status`
returns `null`. `pause`, `stop` and `release` then do nothing and return
`null`, and `connectedPorts`, `changeDisc` and `option` return `false`.

### start

Loads and starts a PS1 game.

```json
{"core":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\cores\\swanstation_libretro.dll",
 "disc":"D:\\Games\\PS1\\FF7\\FF7.m3u",
 "system":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\retro\\system",
 "saves":"C:\\Users\\p\\AppData\\Roaming\\AnyPS5\\retro\\saves\\ps1",
 "internalResolution":"4","options":{"swanstation_GPU_TextureFilter":"Bilinear"},
 "initialDisc":{"index":1,"path":"D:\\Games\\PS1\\FF7\\FF7 (Disc 2).cue"}}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `core` | path | The SwanStation library the launcher checked |
| `disc` | path | A `.cue`, `.chd`, `.pbp` or `.m3u` file |
| `system` | path | The core's system directory (BIOS files) |
| `saves` | path | Where memory cards are kept; the host creates it if needed |
| `internalResolution` | string, optional | A value of `swanstation_GPU_ResolutionScale` |
| `options` | object of strings, optional | Core option values saved by the player, by key. Unknown keys and values are ignored |
| `initialDisc` | object, optional | Only for an `.m3u`, otherwise an `argument` error: the disc to insert at boot. `index` is 0 to 127, `path` is that disc's path |

The result is `{"epoch":<integer>}`. The epoch identifies this game's frames
in the ring (see [Frame ring](#frame-ring)). It is 1 for the host's first
game and one more for each later game, so it is never reused in a host's
run. A host that has already started 2147483647 games answers `start` with a
`start` error.
After the reply the game loads in the background. `status` shows when the
first frame is ready, or that the game ended with an error.

Errors: `busy`, if a game is loaded (an ended game is released first, so only
a running game counts); `argument`; `start`, if the core or disc cannot be
loaded.

### status

No argument. Returns `null` when no game is loaded, or:

```json
{"ready":true,"ended":false,"error":"","aspect":1.3333333333333333,"hardware":true,
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
| `error` | string | Why the game ended, or `""` |
| `aspect` | number | The display aspect ratio the core reports |
| `hardware` | boolean | The core draws with the GPU (not the software renderer) |
| `resolutionError` | string | Why the last internal resolution could not be used, or `""` |
| `graphicsPending` | boolean | A graphics option is waiting to be applied |
| `progressLabel` | string or null | What is loading right now, while there is something to show |
| `progressPercent` | integer 0 to 100, or null | How far it is, when known |
| `options` | object | Each core option the player can change now, by key |
| `discs` | object, optional | Present for multi-disc games |

After a [host failure](#host-failure), only `ready`, `ended` and `error` are
present.

Each option has `title` (string), `values` (array of strings), `labels`
(array of strings, the same length as `values`), `selected` (index into
`values` of the value chosen, which may still be pending), `activeValue` (the
value in use now) and `restart` (true when a change only takes effect when the
game is started again).

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
when the count was taken, and `false` when it is out of range or the game has
ended.

### changeDisc

Argument: an integer, the index into `discs.labels`. It opens the tray,
inserts that disc and closes the tray, between frames. It returns `true` when
the change has started and `discs.busy` then shows its progress. It returns
`false` for a game with one disc, for an index out of range, or while
another change is in progress.

### option

Argument: `{"key":<string>,"value":<string>}`. Sets a core option that
`status` lists. Returns `true` when the value is one of the option's `values`
and was accepted (it may be pending, see `graphicsPending` and `restart`), and
`false` otherwise.

### volume

Argument: `{"level":<number 0 to 1>,"muted":<boolean>}`. Sets the host's
output volume, for this and every later game, until the host exits. A new
host starts at `{"level":1,"muted":false}`. Returns `null`. This method works
whether or not a game is loaded.

### stop

No argument. Asks the game to end. The host writes the memory cards and
unloads the core, then `status` shows `ended: true`. Returns `null` at once.
The game MUST end within 5 seconds of the call.

### release

No argument. Forgets an ended game, so `status` returns `null` and a new
`start` can follow. It does nothing while the game is still running. Returns
`null`.

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
A game whose frame would be wider or taller than 8192 pixels ends with an
error instead of being drawn.

`latest` and `reading` are 4-byte aligned and accessed with sequentially
consistent atomic operations. The other header fields are plain memory. They
are ordered by the atomics, which works because each slot has one writer and
one reader at a time.

**Host (writer).** It picks a slot that is neither `latest` nor `reading`.
There is always one, because there are three slots. It writes the pixels and
the slot's fields, then stores the slot's index in `latest`. The host never
waits. Starting or releasing a game sets `latest` to none before the reply.

**Launcher (reader).** To read a frame, it loads `latest`. If that is
`0xFFFFFFFF` there is no frame, and the launcher keeps showing what it showed
before. Otherwise it stores the value in `reading` and loads `latest` again.
If the two loads agree, it reads that slot. If they still disagree after
three tries, it skips this draw and tries again on the next one. A pinned slot is never written. The launcher holds
one pin at a time, until its next read. It draws a frame only when the slot's
`epoch` is the one `start` returned for the current game.

**Checks the launcher makes.** A failed check is a protocol error.

- When a ring arrives, before using it:
  - on Windows, `VirtualQuery` on the mapped view shows at least `bytes`
    bytes;
  - on Linux, `fstat` gives exactly `bytes`, and `F_GET_SEALS` includes
    `F_SEAL_SHRINK` and `F_SEAL_SEAL`;
  - on macOS, `fcntl(fd, F_GETFL)` shows `O_RDWR`; `fstat` gives at least
    `bytes` and less than `bytes` plus one page (macOS rounds a shared memory
    object up to whole pages); and `ftruncate(fd, bytes)` fails. A POSIX
    shared memory object on macOS takes only its first `ftruncate` and
    refuses every later one. A writable regular file would accept the call,
    so this shows the object's size cannot change;
  - `magic`, `version`, `slotCount`, `headerBytes` and `pixelFormat` have the
    values above;
  - `slotBytes` is in range, and `bytes` equals `4096 + 3 × slotBytes`.

  The launcher then keeps its own copy of `slotBytes`. It never reads the
  header's geometry again.
- For each frame: the index in `latest` is 0, 1 or 2 (or none, as above); `width` and `height`
  are 1 to 8192; and `width × height × 4` is at most the kept `slotBytes`.
  This is computed in 64 bits; it cannot overflow with those limits.

**Memory objects.**

| OS | Created with | Notes |
| --- | --- | --- |
| Windows | `CreateFileMappingW(INVALID_HANDLE_VALUE, …, PAGE_READWRITE)`, unnamed | A section never shrinks |
| Linux | `memfd_create(…, MFD_CLOEXEC \| MFD_ALLOW_SEALING)`, `ftruncate`, then `F_ADD_SEALS` with `F_SEAL_SHRINK \| F_SEAL_GROW \| F_SEAL_SEAL` | Sealing stops a truncate that would give the launcher `SIGBUS` |
| macOS | `shm_open(name, O_RDWR \| O_CREAT \| O_EXCL, 0600)` with a random name of at most 30 characters, such as `/a5r-` and 12 random letters; `shm_unlink` at once; one `ftruncate` to the ring's size | The size is fixed after that `ftruncate`; the launcher checks it as above, and the host's macOS CI tests it |

A ring only grows. When a frame needs more room, the host makes a new ring,
sends `ring`, and publishes into the new one. The new `slotBytes` is the
larger of twice the old one and what the frame needs, capped at the limit.
So a host never has more than 27 rings, and their slots together take less
than twice the largest ring's. The host keeps every ring it has announced
until it exits, so on Windows the launcher can always still duplicate the
handle.

Both sides map the whole ring for reading and writing, shared:
`MapViewOfFile(…, FILE_MAP_READ | FILE_MAP_WRITE, …)` on Windows, and
`mmap(…, PROT_READ | PROT_WRITE, MAP_SHARED, …)` elsewhere. The launcher unmaps the old ring once it holds no pin
in it.

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
| The host exits after `shutdown` | host: 10 s; the launcher kills it at 15 s |
| The host exits after end-of-stream or the launcher's exit | host: 10 s |
| A game ends after `stop` | host: 5 s; the launcher kills the host at 10 s |

**Game limits.** These end only the game. The host keeps running, and
`status` shows `ended: true` with the error given.

| What | Limit | `error` |
| --- | --- | --- |
| A running game publishes no frame after its first one | 10 s, not counting time paused | "The emulator stopped responding." |
| A game publishes its first frame after `start` | 60 s, not counting time paused | "The game did not start." |

The launcher MUST NOT read, write or wait on the control stream, or wait for
the host to exit, on its UI thread or raster thread.

## Host failure

The host has failed if it exits without being asked, sends a protocol error,
or misses a launcher limit. The launcher then kills it if it is still
running. If a game was loaded:

- `status` returns `{"ready":false,"ended":true,"error":"The emulator closed
  unexpectedly."}`, or `"The emulator stopped responding and was closed."`
  after a missed limit. `pause`, `stop`, `release` and `volume` return
  `null`; `connectedPorts`, `changeDisc` and `option` return `false`. This
  lasts until the UI calls `release`.
- The UI shows the error and goes back to the library the same way it does
  for any game that has ended. The next game starts a new host.

A launcher that gets `unsupported`, or a version or pixel format it did not
offer, shows its own message. That message says the emulator needs updating,
and is not the crash message.

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
  type.

**`sessions/`** has whole conversations:

- `tests` lists the sides the session tests, `host` and/or `launcher`. For
  each, a fake plays the other side.
- Steps from the fake are sent as written. Steps from the side under test are
  what it must send.
- The side under test may also send `ping`, `pong` and `input` at any time.
  A test ignores those unless a step names them.
- `steps` run in order. Each step is one of:

| Step | Meaning |
| --- | --- |
| `{"from":…,"message":{…}}` | That side sends the message. Optional additions: `anyValueFor`, and `descriptors` (how many descriptors are attached: 0 by default, 1 for `ring`) |
| `{"from":…,"message":{…},"ring":{…}}` | A `ring` message. `ring` describes the shared memory: `memoryBytes` (its real size), `sealed` (whether the Linux seals are present) and `header` (every header field named as in the [table](#frame-ring), with `slots` as an array of three objects). On Windows the fake host adds `handle` and sends no descriptor |
| `{"from":…,"bytes":"<hex>"}` | That side writes these raw bytes on the stream |
| `{"pollWithin":<seconds>}` | The launcher side repeats the call before this step, with a new `id` each time, until the reply matches the next step. This may take up to that many seconds |
| `{"hostExits":<code>}` | The host process ends with that exit code |
| `{"silence":<seconds>}` | The fake sends nothing for that long, and answers nothing, pings included |
| `{"launcherKillsHost":true}` | The launcher ends the host |
| `{"launcherReports":{"method":…,"result":…}}` | What the launcher's client gives its UI for that method from then on |

A step with `"invalid":"<rule>"` breaks the named rule on purpose, and the
fixture checker does not check it against the rules.

`outcome` is where the side under test ends up:

| Outcome | Meaning |
| --- | --- |
| `clean` | Everything is answered and the host exits with code 0 |
| `protocolError` | The side under test ends the connection as [Framing](#framing) says: a host exits; a launcher kills the host |
| `unsupported` | The launcher reports the version or pixel format mismatch, not a crash |
| `hostFailure` | The launcher handles a [host failure](#host-failure) |

[`tools/check_fixtures.py`](../tools/check_fixtures.py) checks every message
in the fixtures against the fields, types and ranges in this document.
