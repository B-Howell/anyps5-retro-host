# anyps5-retro-host protocol, version 1

AnyPS5 Desktop (the launcher, GPL-2.0-only) never loads a libretro core. It
starts `anyps5-retro-host` (this program, GPL-3.0) as a child process and
talks to it over this protocol. Control messages are small JSON documents.
Video goes through shared memory and audio is played by the host, so neither
crosses the control stream. No IPC library is used.

## Transport and framing

| OS | Control stream | How the host gets it |
| --- | --- | --- |
| Windows | Two anonymous pipes (one per direction) | The launcher passes the handle values of the host's ends and its own pid: `--ipc-read N --ipc-write N --parent-pid N`. The host opens the launcher with `PROCESS_DUP_HANDLE` and takes both ends with `DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE)`. No handle is ever inheritable, so no other child of the launcher can hold the pipes open. |
| Linux | One `AF_UNIX` `SOCK_STREAM` socket from `socketpair` | It is descriptor 3 in the host: `--ipc-fd 3 --parent-pid N`. |
| macOS (planned) | Same as Linux | Same as Linux. |

Every message is a 4-byte little-endian length followed by that many bytes of
UTF-8 JSON (at most 64 MiB). Messages are processed strictly in order. On
POSIX a message may carry file descriptors as `SCM_RIGHTS` ancillary data.

## Versioning

Every message has `"v": 1`. The host's first message is
`{"v":1,"event":"hello","protocol":1,"pid":<host pid>}`. A launcher that gets
a different `protocol`, an unknown `v`, or a message it cannot parse kills the
host and treats it as crashed. Any incompatible change raises the version;
adding an optional field does not. The launcher and host are released
separately, so the launcher pins the host build it accepts (see *Locating the
host*).

## Values

Arguments and results are the values of Flutter's standard method codec,
written as JSON:

| Codec value | JSON |
| --- | --- |
| null, bool, String | the same |
| int | an integer literal. It decodes as 32-bit when it fits and 64-bit otherwise, which is how Flutter's Windows codec reads a Dart int |
| double | a number that always has a fraction or exponent (`2.0`); NaN and infinities are `{"$f":"nan"}`, `{"$f":"inf"}`, `{"$f":"-inf"}` |
| Uint8List | `{"$u8":"<base64>"}` |
| Int32List, Int64List, Float32List, Float64List | `{"$i32":[…]}`, `{"$i64":[…]}`, `{"$f32":[…]}`, `{"$f64":[…]}` |
| List | an array |
| Map with only String keys, none starting with `$` | an object |
| any other Map | `{"$map":[[key,value],…]}` |

An object with exactly one member whose name starts with `$` is always one of
these tags; an unknown tag is an error.

## Messages

### Launcher to host

**Call** — one method call of the launcher's `anyps5/retro_core` channel,
forwarded unchanged:

```json
{"v":1,"id":7,"method":"start","args":{…},"texture":12}
```

`id` is unique per connection. `texture` is present only on calls that can
start a game (`start`, `ps1LocalCheckpointReopen`): it names the launcher's
Flutter texture the game is drawn into. The host uses it as the game's surface
token, so the `texture` arguments the UI already sends keep their meaning.

**Presented** — sent by the launcher's raster thread the first time it draws a
frame of a surface epoch, and again the first time a drawn frame has visible
pixels:

```json
{"v":1,"event":"presented","epoch":3,"visible":true}
```

The host ignores a stale epoch. This drives the `presented` and
`startupVisible` status fields the launch card waits for, exactly as the
in-process texture callback did.

**Shutdown** — `{"v":1,"event":"shutdown"}`: stop any game (memory cards are
written as on a normal close) and exit.

### Host to launcher

**Reply** — exactly one per call, in call order:

```json
{"v":1,"id":7,"result":<value>,"surface":12,"session":true}
{"v":1,"id":7,"error":{"code":"start","message":"…"},"surface":-1,"session":false}
{"v":1,"id":7,"notImplemented":true,"surface":-1,"session":false}
```

`surface` is the texture the host's current game draws into (-1 for none) and
`session` whether a game, or a PS3 update install, is loaded. The launcher
uses them to release textures and to know whether a host exit loses a game.

**Frame** — `{"v":1,"event":"frame","texture":12}`: a new frame is in shared
memory for that texture. The launcher marks the texture as having a frame.

**Ring** — announces new frame memory, sent before the first frame that needs
it (first frame of the host, or a frame larger than the current slots):

- Windows: `{"v":1,"event":"ring","size":N,"handle":H}`; `H` is a handle
  value in the host. The launcher duplicates it from the host process.
- POSIX: `{"v":1,"event":"ring","size":N}` with the memory's descriptor
  attached.

## Methods

The method names, arguments and results are those of the existing
`anyps5/retro_core` channel and are not redefined here: `start`, `status`,
`pause`, `input` (`[port?, mask, lx, ly, rx, ry]`), `changeDisc`, `option`,
`tuningOptions`, `stop`, `release`, the PS1 match calls (`netplayStart`,
`netplaySync`, `ps1Checkpoint*`, `ps1LocalCheckpoint*`, `ps1ResumeLocal`),
the PS2 LAN calls (`ps2LanReset`, `ps2LanExchange`) and the PS3 update calls
(`installPs3Packages`, `ps3PackageStatus`). Keeping them identical is what
lets the launcher UI stay unchanged.

The launcher answers one method itself: `hostProcessId` returns the host's pid
(0 when none runs). The UI reports it as the running game's pid, so per-process
capture works for embedded games.

`probeStats` and `probeReadRam` exist only when the host was started with
`--probe` by a test harness. The launcher never passes `--probe`.

## Video: shared frame ring

The ring is one shared memory object: a 4096-byte header, then three slots of
`slot_bytes` each. Pixels are tightly packed RGBA8, the format the launcher's
pixel-buffer texture already takes.

```
magic 'A5RF', version 1, slot_count 3, header_bytes 4096, slot_bytes
atomic latest   (slot index of the newest frame, or none)
atomic reading  (slot index the launcher has pinned, or none)
atomic epoch    (incremented when a game starts or is released)
slots[3] { width, height, visible, sequence }
```

The host writes into a slot that is neither `latest` nor `reading`, fills its
metadata, then publishes it as `latest`. The launcher pins `latest` by storing
it in `reading` and re-reading `latest` until both agree, then copies. Neither
side ever waits for the other, and a pinned frame is never overwritten.
Starting or releasing a game clears `latest` and bumps `epoch`, so a new game
never shows the previous game's last picture.

The launcher holds one pin at a time: a pin lasts until its next copy. Both
Flutter embedders upload a pixel buffer and release it before they ask for the
next one, so a pinned slot is never in use when it is replaced.

Rings only grow. The host keeps every ring it has announced mapped for its
lifetime, so a Windows handle the launcher has not duplicated yet is never
closed under it.

| OS | Memory object | Handoff |
| --- | --- | --- |
| Windows | Unnamed pagefile-backed `CreateFileMapping` | handle duplicated by the launcher from the host process |
| Linux | `memfd_create` | descriptor over the socket |
| macOS (planned) | `shm_open` + immediate `shm_unlink` | descriptor over the socket |

Every renderer already reads its frame back to RGBA before the texture copy
(D3D11 and OpenGL staging reads, Vulkan host-visible copies), so this keeps
today's picture byte for byte. A zero-copy GPU path can be added later as a
new `ring` kind, negotiated by version:

- Windows: a D3D11 texture with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
  SHARED_KEYEDMUTEX`; the PS3 renderer exports its image with
  `VK_KHR_external_memory_win32`.
- Linux: a dmabuf (`VK_KHR_external_memory_fd` / EGL export) over
  `SCM_RIGHTS`.
- macOS: an `IOSurface` sent as a Mach port (`IOSurfaceCreateMachPort`) over a
  bootstrap-free port pair set up at spawn; Metal and MoltenVK both import it.

## Audio

The host plays its own audio (waveOut on Windows, PulseAudio on Linux, Core
Audio planned on macOS). Per-process capture finds it through
`hostProcessId`.

## Lifetime

- The launcher starts the host on the first `anyps5/retro_core` call, so
  someone who never plays a PS1, PS2 or PS3 game never starts it.
- One host serves games one after another, as the in-process runner did. A PS1
  recovery checkpoint parked between games lives in the host.
- The launcher's quit waits only while a game runs (unchanged). On exit it
  sends `shutdown`, waits up to 15 s for the host to save and exit, then kills
  it.
- If the launcher disappears the host sees end-of-stream, stops its game the
  same way and exits. The reader thread arms a 20 s forced exit the moment it
  sees end-of-stream, so a method handler that never returns cannot keep an
  orphaned host alive.
- On Windows the launcher keeps private duplicates of the host's pipe ends
  until the host says hello. If the host exits before that, the launcher
  closes any end the host never took (checked with `CompareObjectHandles`),
  so the start fails at once instead of waiting for a timeout.
- The host runs without a console window. It suppresses the Windows error
  dialogs (`SEM_NOGPFAULTERRORBOX`, no abort message box), so a crashing core
  shows nothing on the desktop.

## Crashes and hangs

A host that exits, or sends something the launcher cannot parse, while a game
or install is loaded has *lost* that game:

- Pending calls and every call until the UI releases the game get the answer
  an ended game gives: `status` returns `{"ended":true,"error":"The emulator
  closed unexpectedly."}`; `stop`, `release`, `pause`, `input` succeed;
  boolean calls (`changeDisc`, `option`, `netplayStart`, …) return false; the
  rest return an error with that message.
- The UI then shows the error and returns to the library through its normal
  "game ended" path. The next game starts a new host.

The launcher kills the host as hung when a call has had no reply for 30 s, or
when a game was asked to `stop` and has not ended 20 s later. It then answers
as above with "The emulator stopped responding and was closed."

## Locating the host

The launcher runs `anyps5-retro-host(.exe)` from its own directory, or from
`ANYPS5_RETRO_HOST` when set (development and tests). A release either ships a
host build beside the launcher, or downloads it into launcher storage with a
pinned SHA-256 the way managed cores are installed. The host's source and
licence travel with its binary.

## macOS

Nothing above depends on Windows or Linux apart from the handoff rows: macOS
uses the POSIX socket and descriptor handoff, `shm_open` memory, Core Audio,
and later an IOSurface for zero-copy video. The host already builds its
libretro loading, PS1 match and PS2 LAN code portably; the macOS backend needs
an OpenGL (or Metal) software-readback renderer and a Core Audio sink.
