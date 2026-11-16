<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# PS1 online play through the host

This is the design for playing a PS1 game online with a friend when the
game runs in `anyps5-retro-host`. It says which program owns what, the
protocol it needs, how a match starts from the launcher's lobby, and how it is
tested. Lockstep and rollback are built: [protocol.md](protocol.md)
"Lockstep" and "Rollback" are their contract,
[`tests/lockstep_test.py`](../tests/lockstep_test.py) runs two real SwanStation
hosts frame for frame alike, and [`tests/rollback_test.py`](../tests/rollback_test.py)
runs two players' hosts in rollback, with delayed input and wrong predictions,
to the same frames as a host in lockstep.

## What already exists

**The launcher's own PS1 online play.** Before games moved into the host, the
AnyPS5 Desktop launcher ran PS1 couch co-op over the internet itself. It is in
the launcher's integrated builds, described in its PS1 netplay plan,
determinism harness and emulator multiplayer design documents:

- Every player runs the same game in SwanStation, and the two copies are
  given the same controller input for every frame. Nothing but input crosses
  the network during play. A native input timeline keeps a 2-frame input
  delay, predicts the friend's input for up to 8 frames, and when a
  prediction was wrong restores a state saved before that frame and replays
  with the real input.
- Input travels in a 108-byte datagram holding up to eight frames of
  controller state, with the match epoch, the sender's seat and a cumulative
  acknowledgement, so a lost packet is covered by the next one.
- A friend joins a running game: the host's game is held, its state goes to
  the friend over the reliable peer stream in 64 KiB windows checked with
  SHA-256, the friend replays up to 120 recorded frames of input to catch
  up, and both start a new input epoch at frame 0.
- Both sides compare main-RAM fingerprints at agreed frames and stop on a
  mismatch.
- AnyTogether finds the friend, carries the invitation, and connects the two
  PCs with WebRTC: a direct path where possible, or through a TURN relay.
  The lobby opens from the PS menu of a running game and shows Player 1 to 4
  seats.
- The evidence: a full Crash Team Racing race of 13,706 frames between a
  Windows and a Linux PC, with 114 matching fingerprints, through the
  AnyTogether service and a direct WebRTC path. It needed SwanStation
  patches: one so a restored state does not reschedule the sound unit's
  sample event from the discarded timeline, one for a portable checkpoint
  that names the disc by its hash rather than its path, and one for a blank
  memory card.

The first `anyps5-retro-host` (the `main` branch of B-Howell/anyps5-retro-host)
took that timeline into the host behind the launcher's old method channel.
Version 1 of this protocol replaced that channel with a small, checked
contract for single-player play. This design brings online play back into
version 1 the same way: as explicit messages, one capability at a time.

**RetroArch.** RetroArch's netplay runs the core on every peer and sends only
input. It saves the core's state every frame, predicts late input, and on a
wrong prediction loads the older state and replays. An input latency setting
delays local input by a few frames so that fewer frames need replaying.
Peers compare checksums of serialized states at intervals to find a desync.
Clients connect to the host over TCP, or through a relay server when they
cannot reach it directly. It works only with cores whose serialization is
complete and deterministic; libretro lets a core say otherwise
(`RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS`) and lets the frontend say why
it is saving (`RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT`, whose
`ROLLBACK_NETPLAY` value means the state may go to another machine).

**SwanStation and DuckStation.** SwanStation, the libretro core version 1
loads, has no netplay of its own and leaves it to the frontend. The host
builds it with seven patches of its own (host.md "SwanStation patches"):
five make its states and controllers fit for netplay ("Determinism" below
says why), one gives it a build identity two players can compare ("Core
build identity"), and one makes a rollback's states cheap ("What a rollback
costs"). Its parent,
DuckStation, carried a GGPO-based rollback netplay for a time and later
removed it. GGPO is the same model as RetroArch's: input delay plus rollback
on saved states, with a sync test that saves, loads and replays every frame
to find state the core forgets to save.

What every one of these agrees on: online PS1 play is deterministic emulation
on every machine, with only input on the network; the emulator must save and
load states that bring back everything that affects later frames; and
desyncs are found by comparing hashes.

## Who owns the network

**The launcher owns the network. The host never opens a socket.**

- The launcher already has everything a match needs: the signed-in account,
  the friend list, invitations, the lobby, and AnyTogether's WebRTC
  connection with its TURN relay. PS2 and PS3 online play use the same
  transport, so it stays in one place.
- The host runs third-party emulator code. Keeping it off the network keeps
  protocol.md's rule, which CI enforces by tracing the host's network calls,
  and means a flaw in a core cannot be reached from the internet.
- The host owns what only it can do well: running exactly one frame per
  input, saving and loading the core's state between frames, and hashing
  what each frame left behind. These need the core and the game's thread.
- The cost is one more local hop for each frame's input, over a pipe or
  socket on the same machine. It is far below a frame's 16.7 ms (20 ms in a
  PAL game).

So each launcher is the network peer and the host is a deterministic
machine it feeds:

```
 Player A's PC                                        Player B's PC
 ┌───────────────────────────┐   AnyTogether rooms,   ┌───────────────────────────┐
 │ launcher                  │   WebRTC data channel, │ launcher                  │
 │  lobby, seats, input wire ├────── TURN relay ──────┤  lobby, seats, input wire │
 │        │ step   ▲ sync    │                        │        │ step   ▲ sync    │
 │        ▼        │         │                        │        ▼        │         │
 │ anyps5-retro-host         │                        │ anyps5-retro-host         │
 │  SwanStation, no sockets  │                        │  SwanStation, no sockets  │
 └───────────────────────────┘                        └───────────────────────────┘
```

## Lockstep first, rollback after

**Lockstep with input delay** (capability `ps1Lockstep`, built). A frame
runs only when every player's input for it is known. Each launcher sends its
own input for frame *n* + *d*, where *d* is the input delay, and hands the
host the step for frame *n* once the friend's input for *n* has arrived. With
*d* frames of delay covering half the round trip plus some jitter, the game
never waits. A late packet makes both players wait; it never makes them
differ.

**Rollback** (capability `ps1Rollback`, built). The host runs ahead on
predicted input and, when the real input differs, loads the state from before
that frame and replays. It hides more of the round trip than delay alone, and
costs a state save before every frame run on a prediction and a burst of
replayed frames on each wrong guess.

Lockstep comes first because:

- it needs the fewest new parts: the host waits, the launcher's input wire
  stays as it is, and nothing is undone;
- it is the control rollback is checked against: a rollback run must reach
  the same hashes as a lockstep run of the same input;
- a file-based state took about 60 ms to load and 9 ms to save through the
  protocol (SwanStation at 631f0fb, 11 MiB of state, Windows), far over a
  16.7 ms frame. So `saveState` and `loadState` also keep up to 8 states in
  memory (protocol.md "saveState and loadState"), saved and loaded in the
  core's same-binary form
  (`RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY`, which keeps the
  recompiled code and needs no file), and report what each took. The test
  below saves before every frame, runs it, loads and runs it again: on a
  desktop PC drawing with OpenGL on an NVIDIA GPU (Windows, 1x) a save took 2.4 ms
  on average (5.8 ms at most) and a load 3.2 ms (4.6 ms at most), so one of
  each fits a frame. CI's runners took 1.4 to 3.5 ms to save and 2.4 to
  4.4 ms to load on average (Windows in software, Linux with Mesa), but the
  hosted macOS runner, whose GPU is a virtual one, took 73 ms to save: a
  save under a GPU reads the console's video memory back from it, which is
  slow there. These are the slots' figures before the seventh SwanStation
  patch; "What a rollback costs" below has what a rollback's own states cost
  now, whose video memory stays on the GPU as DuckStation's memory states
  keep it. Replaying several frames after a wrong guess costs their
  emulation on top;
- the launcher's own harness found that rollback with the stock core is not
  exact (the sound unit restore below), so rollback also depended on that
  patch, which the host now carries.

A match picks *d* from the round trip it measures while the lobby is open:
*d* = ⌈(round trip ÷ 2) ÷ frame⌉ + 1, at most 8, where a frame lasts
1 ÷ `status.timing.fps` (16.7 ms in an NTSC game, 20 ms in a PAL one;
"Frame rate" below). Above 8 frames (a round trip above about 230 ms in an NTSC
game, 280 ms in a PAL one) the lobby says the connection is too slow for this game,
rather than offering a match that feels broken. Rollback later lowers the
delay players feel to a fixed 1 or 2 frames, with the rest predicted.

### Rollback

`ps1Rollback`, on top of `ps1Lockstep`, is GGPO's model, as RetroArch's
netplay and DuckStation's former netplay use it, with the network left to
the launcher ([protocol.md](protocol.md) "Rollback" is the contract, host.md
"Rollback" the code):

- `lockstep` takes `"rollback":{"frames":<1 to 8>}`: at most that many frames
  are stepped beyond the last confirmed one (GGPO's prediction window, 8 by
  default there too).
- A `step` may leave a port `null` when the friend's input for that frame has
  not arrived. The host runs the frame at once with that port's latest known
  input, as GGPO predicts, and saves its state in memory before it.
- `correct` gives one port's real input for such a frame, in frame order per
  port, as the friend's packets arrive. Between two frames the host takes the
  corrections that came; if a frame ran with input other than the real one,
  it loads the state from before the first such frame and runs it and every
  later frame again, as fast as it can, with the real input and new
  predictions for what is still missing. Only the newest picture they made
  is shown, once they are done; none plays sound again (the core is told
  audio is off for them, as RetroArch tells it for its replays); the motors
  come back with the state, and rumble is sent once, for motors that ended
  differently. A `rollback` message reports the frames and
  the time.
- A frame is confirmed once every port's input in it and every earlier frame
  is known. The host decides that itself from the steps and corrections, so
  no `confirm` message is needed, and sends a frame's `sync` only then, with
  what the frame left behind the last time it ran: two players compare the
  same thing however often each of them ran the frame.
- `saveState` is refused while a frame is not confirmed (the state would hold
  a prediction), so the state a friend joins from is always a confirmed one.

The launcher's side is small: its input wire already sends each frame's input
ahead and acknowledges it, so it steps every frame on time with the friend's
port `null` when that input is late, sends `correct` when it arrives, and
lowers its input delay to 1 or 2 frames, leaving the rest of the round trip to
prediction.

### Frame rate

In rollback the launcher's frame clock is the game's only clock (the host
runs each step as it comes, `paced` `false`), so that clock must run at the
game's own rate. A PS1 game does not always run at 60 frames a second:
SwanStation gives the vertical frequency of the video mode the game set,
about 60 for NTSC and 50 for PAL (a European game sets PAL), and gives a new
one when the game changes its mode. At 631f0fb `real_core.py` measured
59.817 for a progressive NTSC mode and 49.747 for a PAL mode, the same on
Windows, Linux and macOS (release run 43790112529). A clock fixed at 60 would run a PAL game
a fifth too fast, and an NTSC game slightly off its console's rate.

RetroArch paces its main loop to the core's `retro_system_av_info`
`timing.fps` and follows `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO`; GGPO leaves
the frame rate to the game, which advances its own clock. The host does the
first already when it keeps its own time, and with capability `gameTiming`
it reports that rate, and the core's `retro_get_region`, as `status.timing`
([protocol.md](protocol.md) "status"), so the launcher's clock is the
second: it steps a frame every 1 ÷ `fps` seconds and takes a new `fps` from
the next `status`. The value comes from the emulated console, so both
players' hosts report the same rate at the same frame; it is not a measure of
either machine's speed. The input delay above, the round trip in frames and
whether a rollback fits in a frame are all counted in frames of that length.

## Joining a running game

A player opens the PS menu of a game that is already running, opens the
lobby, and invites a friend. Using the methods that exist now:

1. Both launchers check the match is possible before anything is held: the
   same disc (by a hash of its tracks), the same core build and host
   version, the same BIOS, and the same options. Any difference shows a
   reason and stops.
2. The host player's launcher calls `lockstep` on the running game. The game
   holds at the next frame and the host player sees *Waiting for <friend>*.
3. It calls `saveState` into a folder of its own for the match and sends the
   file to the friend over the peer stream, compressed and checked with
   SHA-256, as the launcher's checkpoint transfer does today. The 11 MiB
   SwanStation state is mostly zeros and shrinks to a fraction of that.
4. The friend's launcher starts the same game with the same options and a
   new `saves` directory for the match, muted and not shown, waits for
   `status` to say `ready`, then calls `lockstep` and `loadState`. Both are
   now at the same frame.
5. Both launchers start sending steps from that frame. Player 1 is the host
   player and Player 2 the friend. The first `sync` both hosts send is the
   first check that the join worked.
6. When either player leaves, both launchers call `lockstep` with `false`
   (or `stop`). The host player's game carries on from where the match left
   it, or, if they choose, goes back to the state saved at step 3. The
   friend's game is stopped and its match folder deleted.

A match can also start both games from a cold boot with `start.lockstep`.
Then no state crosses the network, and the test below shows two hosts stay
alike from the first frame.

## Lobby, seats and controller ports

The launcher's lobby has seats for Player 1 to 4. Each seat is held by one
controller on one PC. For a PS1 game:

- Player 1 is controller port 1 and Player 2 is port 2. Seats 3 and 4 show as
  *Not in this game*: version 1 keeps SwanStation's Multitap off (its option
  is one the host fixes), so a PS1 game has two ports. Four players need a
  later change that lets a lockstep match turn the Multitap on for every
  player at boot.
- The match host's launcher assigns seats, and every launcher builds the same
  `step` for each frame from all seats' input: port 1's input from whoever
  holds seat 1, wherever they are. A PC can hold both seats (couch co-op) or
  one, so a local second player and an online friend are the same thing to
  the host.
- How many ports are connected is part of each step (`ports` has one or two
  entries), so a second player joining or leaving takes effect at the same
  frame on both machines. `connectedPorts` is refused in lockstep for that
  reason.
- Pausing from the PS menu during a match pauses for both players: the
  launcher stops sending steps and tells the friend's launcher, which does the
  same. The host itself needs nothing more; it simply waits.

## NAT and relays

Nothing new is needed. The launcher reuses AnyTogether's rooms for the
invitation and the WebRTC data channel it already uses for PS1 and PS2 online
play: a direct path when the two PCs can reach each other, and the TURN relay
when they cannot. The lobby shows which one it got. Input is small (one
frame's input for two ports is under 30 bytes, and the input wire sends up to
eight frames in one 108-byte packet), so a relay carries it easily. The one
large transfer is the joining state, sent once over the reliable channel.
Relay use and its traffic policy remain AnyTogether's decision, as for the
launcher's existing online play.

## Determinism

What keeps two hosts alike, and what is known to break it:

- **Same inputs to the core.** In lockstep, the host gives the core only the
  step's input, and the port count from the step, before each frame.
  `input` messages wait until lockstep ends. Options and discs cannot change.
- **Same start.** Both games use the same core build, BIOS and options. The
  match passes the options at `start`, so every player boots with the same.
  The launchers confirm the core build with `status.core` ("Core build
  identity" below).
- **RAM and state are the cross-machine checks.** `sync.ram` and
  `sync.state` must match on every machine. `sync.video` matches only when
  both draw in software, since GPUs may round differently; for the same
  reason `sync.state` may differ between two GPUs at a resolution scale above
  1x, since it holds video memory read back from the GPU.
- **A state names no path.** Stock SwanStation's state names the disc's full
  path, and its title, on the machine that saved it. A state sent to a friend
  would tell them where the host player keeps the game, which can include
  their user name; the friend's copy would have to have the same file name
  (SwanStation found a loaded state's disc by its path, or else by its file
  name); and `sync.state` would differ between two machines whose discs are
  in different places. libretro lets a frontend say a state is for netplay
  (`RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY`: it "will be sent over the
  network"), and the host says so for every file and every `sync`. The
  host's SwanStation patch `swanstation-2-pathless-netplay-state` then leaves
  out the title and the path and marks the state pathless; a pathless state
  names its disc by its place in the frontend's disc list (0 for a one-disc
  game, the playlist order otherwise), which is the same on every machine
  that starts the same game. Only that core reads such a state.
- **The analog-mode button combination and the controllers.** SwanStation
  toggles a DualShock's analog mode when the player holds a button
  combination (L1 + R1 + Select by default). Stock SwanStation kept whether
  the combination was still held in one variable of its libretro layer, for
  all ports, outside the saved state, and its state load kept each
  controller's current buttons and analog mode instead of the saved ones (a
  player loading a state keeps their own pad as it is). It did not save
  whether the game had locked the analog mode either. So a loaded game could
  toggle analog mode at another frame, or be in another mode, than the game
  it was saved from: the lockstep test hit this, and avoided Select until the
  fix. The patch `swanstation-1-netplay-input-state` keeps the held
  combination per controller and saves it, with the lock, in the
  controller's state (SwanStation's state version 58; upstream DuckStation
  saves the lock too since its version 76), and loads every controller's
  input state when the frontend says the load is for netplay, rollback or
  runahead. It also puts a port device the frontend sets just before a load
  in place before the state is read: SwanStation applied it at the next
  frame, after the load, so a friend's port 2, connected only by the next
  step, began as a new controller rather than the host player's. The host
  saves the port count with each state and connects those ports before it
  loads one.
- **Video memory from a state.** With a GPU, SwanStation turned 5-bit colour
  channels into 8-bit ones in two ways: on the CPU, exactly (as when the game
  fills a rectangle, giving the nearest byte), and in a shader as *x* / 31,
  which is no exact byte and which a GPU may store as the byte below or above
  (OpenGL and Vulkan allow either; an NVIDIA GPU stored 164 where the CPU
  gives 165). Video memory loaded
  from a state therefore showed some colours one step off what the game had
  drawn, until it drew them again. RAM and state still matched, but the
  memory-state test below saw the picture differ after every load. The patch
  `swanstation-3-vram-upload-colour` makes the shader give the CPU's bytes,
  as exact multiples of 1/255.
- **Restoring a state from another timeline.** The launcher's harness found
  that stock SwanStation, on loading a state, reschedules its sound unit's
  sample event with timing from the discarded timeline: when the frame after
  the state changed how often the sound unit runs (it runs every sample while
  the SPU's interrupt is on, in larger slices otherwise), the load ran the
  newer timeline's pending sample ticks against the loaded sound unit and its
  tick carry, so its timing, came out different. The patch
  `swanstation-4-spu-sample-event-restore`, the launcher's
  `swanstation-spu-checkpoint.patch`, leaves the event to be restored with the
  other timing events. The pad program below turns the SPU's interrupt on and
  off with the input, so the memory-state test fails at its first frame
  without the patch (the state hash differs; RAM does not yet).
- **Plugging a controller in.** Stock SwanStation made every port's
  controller anew when any port's controller type changed, so a friend's
  controller joining in port 2 put the host player's controller back in
  digital mode and dropped the game's lock on its mode. Both players' hosts do
  that alike, so it does not break a match, but a game that locked the
  controller in analog mode would find it in digital mode. The patch
  `swanstation-5-port-change-keeps-other-controllers` gives a new controller
  only to the port whose type changed, as a console does.
- **Memory cards.** A state carries both memory cards. The friend's game
  uses a `saves` directory made for the match, so the host never writes the
  host player's card over the friend's own (protocol.md "saveState and
  loadState").

## Desync checks

Determinism is checked in play by comparing hashes of what confirmed frames
left behind: a `sync` every `check` frames (protocol.md "sync"). Which hash,
over which bytes and how often decide what a check costs, and in rollback a
check frame may be run several times between two frames of play.

**What others do.**

Read at GGPO [7ddadef](https://github.com/pond3r/ggpo/tree/7ddadef8546a7d99ff0b3530c6056bc8ee4b9c0a),
GGRS [25bd998](https://github.com/gschup/ggrs/tree/25bd9983dee3ecb0677be03b9e2e23d2bd6d29cb),
RetroArch [08a6f54](https://github.com/libretro/RetroArch/blob/08a6f540cbdf9c0554bf7459076b654bb92ed06f/network/netplay/netplay_frontend.c)
and DuckStation's netplay fork [2cce4a4](https://github.com/HeatXD/duckstation-netplay/blob/2cce4a404b7a80a9cba56388654ccb93a9ec70af/src/core/netplay.cpp)
(`GenerateChecksumForFrame`, `NpSaveFrameCb`):

- **GGPO** asks the game for a checksum with every state it saves
  (`save_game_state`'s `checksum`); its sync test session runs each frame
  again and compares them, and its peer-to-peer session sends none. Its
  VectorWar example uses Fletcher-32 over the whole game state.
- **GGRS**, GGPO's Rust port, takes an optional checksum with each saved
  state and, with `DesyncDetection::On { interval }`, sends the checksum of
  a confirmed frame every `interval` frames and reports a mismatch.
- **RetroArch** checks every `netplay_check_frames` frames (600 by default):
  zlib's CRC-32 over the serialized state of a confirmed check frame, taken
  from the state it already keeps for every frame; the server sends its CRC,
  and a client whose CRC differs asks it for a state. Its lockstep mode
  hashes the core's system RAM instead, which "costs no serialize" and leaves
  out bookkeeping that may differ between peers (`netplay_frontend.c`,
  `netplay_lockstep_take_crc`).
- **DuckStation's former netplay** (its GGPO fork) computed XXH32 with every
  save, over a 16 KiB window of the memory state that moves with the frame
  number, and sent confirmed frames' checksums with the input.

**What the host does.**

- **XXH3-64, exact.** Every byte compared is hashed, unlike DuckStation's
  window, which can miss a changed byte for as many frames as the state has
  windows. XXH3 reads memory 64 bytes at a time and gives the same value on
  every system and byte order; FNV-1a, which version 1 used, multiplies once
  per byte, each multiplication waiting for the last, so 2 MiB of RAM and a
  4 MiB state took milliseconds a check. The hash is 64 bits, where
  RetroArch's CRC-32 is 32. The host builds it in from the xxHash submodule
  (`3rdparty/xxHash`, 0.8.3); SwanStation builds the same library in for its
  own use.
- **What is compared is the launcher's choice.** The launcher compares main
  RAM (`sync.ram`), as RetroArch's lockstep does, and should set
  `lockstep.state` `false` (it does not yet: launcher work to come): the state may hold video
  memory a GPU rounds differently, and only reading it back gives it to the
  host, which on hosted macOS's virtual GPU took about 64 ms a check
  ("What a rollback costs"). With
  `lockstep.state` `false`, a `sync` has no state hash and the host does not
  serialize the core for it; RAM and the picture cost a fraction of a
  millisecond. The tests keep the state, since it holds everything the
  console is, and run the real core with both.
- **How often is the launcher's choice too** (`check`): every frame in plain
  lockstep, where each `sync` also tells the launcher a frame was run, and
  every 60th frame in rollback, a tenth of RetroArch's default.
- **Frames run again are hashed again.** A check frame is hashed each time it
  runs, as GGPO and GGRS take a checksum with every save: before it is
  confirmed, the host cannot know which run will be the last, and its
  hashes must be of the last. RetroArch can wait for the confirmation
  because it keeps a serialized state of every frame; the host's rollback
  states are not comparable between machines (they name the disc's path and
  keep video memory on the GPU), and copying the RAM at every run would cost
  about what hashing it does.
- **Versioned.** The hashes change with the algorithm, so a host with the
  new one lists capability `syncXxh3` (protocol.md "Capabilities"). The
  contract is that two players' launchers play together only when both hosts
  list it or neither does, so a host of each kind is refused before the match
  starts instead of reporting a desync at its first check. The launcher does
  not check it yet (it compares `hostVersion`, which a release with
  `syncXxh3` changes); that check is launcher work to come.

## Core build identity

Two players' hosts stay alike only if their cores compute alike. A Windows
player and a Linux player each run the SwanStation their own release ships,
built on another runner with another compiler, so before a match the
launchers need to know that the two builds are the same emulator: the same
SwanStation source, with the same patches, built with the same options.

**What others do.**

- **RetroArch** sends each peer's core name and core version
  (`library_name` and `library_version` from `retro_get_system_info`) and
  the content's CRC in its netplay handshake. A different core name refuses
  the connection; a different core version or CRC only warns. Its own build
  is compared through a value mixed from RetroArch's version and the netplay
  protocol version. A core's `library_version` is usually its release or git
  description, which says nothing about local patches.
- **Dolphin** refuses a netplay client whose version string, its git
  revision, differs from the host's, so both players need the same build.
  Its bug tracker shows where a git string goes wrong: builds made without
  git had an empty version, and builds marked `-dirty` could not play with
  the same revision built elsewhere.
- **GGPO** compares no builds at all; a game using it adds its own version
  or checksum to its handshake, and GGPO's checksums find a desync only
  after it happened.

So the established check is the RetroArch one, the core's own name and
version compared before the match, made strict as Dolphin makes it, with a
version that names exactly what changes emulation and that no checkout
detail (git tags, a dirty tree, line endings) can change.

**The identity.** The host's SwanStation reports `library_version`
`anyps5-1-<SHA-256>`, and the host passes it, with `library_name`, in
`status.core` (protocol.md "status", capability `coreIdentity`). The SHA-256
is of this manifest, one line each, with LF endings:

```
anyps5-retro-host core source 1
swanstation <SwanStation commit, 40 hex digits>
patch <SHA-256 of the patch file> <patch file name>     (one per patch, in the order applied)
option MSB_FIRST=OFF
```

- `cores/apply-swanstation-patches.sh` writes the first lines as
  `anyps5-core-source.txt` into the SwanStation checkout after applying the
  patches: the commit checked out and each patch's bytes, which are LF in
  every checkout (`.gitattributes`). The release's source archive holds that
  file, so building it again gives the same identity.
- The patch `swanstation-6-core-build-identity` adds the `option` lines in
  SwanStation's CMake, from the values the build is configured with, and
  hashes the whole. The only build option of SwanStation that changes what
  the core computes is `MSB_FIRST` (the byte order). Compiler, build type
  among the release ones, MSVC runtime and macOS deployment target change
  how fast it runs, not what it computes; a Debug build already reports
  another `library_name`. The CPU recompiler SwanStation builds for x64 or
  arm64 is meant to compute exactly as its interpreter does, and a fault in
  one shows as a desync in `sync`, not as a different identity.
- The script refuses a SwanStation checkout that differs from its commit in
  any way (a staged or unstaged change, an untracked file), so the manifest
  names the source it patched exactly, as Dolphin's `-dirty` mark is meant
  to. It cannot see an edit made after it ran, or a compiler flag that
  changes what the code computes. The identity is therefore a promise about
  builds made the way the `release` workflow makes them, and a release
  launcher runs only those: the signed host releases (protocol.md "Finding
  and checking the host"). A changed comment or patch name changes the
  identity although the emulation is the same; that errs on the safe side.
  Equal identities do not prove that two machines run alike either; the
  `sync` hashes of every match are that check.
- Without `anyps5-core-source.txt` (a SwanStation built without the script)
  the core keeps SwanStation's own `library_version`, which is not of the
  `anyps5-1-` form, so a launcher does not play online with it.
- `tools/core_identity.py` computes the same value from `cores/patches` and
  the commit without building anything. The `real-core` workflow checks that
  each system's core reports it, and its `same-core` job that Windows, the
  two Linux runners and macOS all report the same `status.core`; the
  `release` workflow checks the core in each zip.

**Why not something else.**

- A hash of the core file differs on every system and with every compiler,
  so two players on different systems would never match.
- A hash computed by the host from the patches it was built with would say
  nothing about the core file the launcher hands it at `start`; the core
  itself carries its identity, as `library_version` does in libretro.
- `hello` cannot carry it: the host loads the core only for a game, from the
  path `start` names, and may load another core for the next game.

**What it does not cover.** The core options are not in the identity: they
are chosen per match, the launcher sends both players the same at `start`,
and `status.options` shows each one's `activeValue`. Nor are the BIOS and the
disc, which the launchers compare by content (the launcher's disc digest).
Everything else that can still differ between two machines, such as a GPU's
rounding at a resolution scale above 1x, is what the `sync` hashes are for.
When a patch is added or changed, or the SwanStation commit moves, the
identity changes, and players need the same host release to play together.

## Tests

Built now:

- [`tests/lockstep_test.py`](../tests/lockstep_test.py) runs two hosts as two
  players' hosts. Both start in lockstep and are fed the same input, which
  changes every frame on both ports; every frame's `sync` must match. One
  host then saves its state, runs 300 more frames, loads the state and replays
  them; the replay must match the first run, state hashes included. The other
  host leaves its game, starts a new one that runs on its own for a second,
  enters lockstep, loads the first host's state and runs the same 300 frames;
  it must match too, state hashes included. Both ports hold the analog-mode
  combination across the saved frame and let go of every button before the
  load, so a core that keeps the combination, or the controllers' analog
  mode, outside its state fails the replay and the join. Last, before each of
  120 frames one host saves its state in a memory slot, runs the frame, loads
  the slot and runs the frame again; both runs must leave the same hashes,
  and the average save and load are printed, and checked against
  `--budget-ms` when given.
  - With the test core (`tests/fake_core.c`, whose "RAM" mixes every input
    it reads) it also checks that a different input on port 2 changes the
    next `sync.ram`, that lockstep refuses option and port changes and bad
    state files, and that a step out of order or outside lockstep is a
    protocol error. CI runs it on Linux, macOS, Windows and under
    ThreadSanitizer.
  - With `--bios` and `--program`, it runs real SwanStation on a disc that
    `tests/make_test_disc.py` makes, each player with their own copy of the
    disc in a different folder under a different name. Every frame's main
    RAM, picture and state must match, and the saved state file must not hold
    the disc's folder or name. The `real-core` workflow runs it on Linux,
    macOS and Windows for 600 frames, with the host's SwanStation patches
    applied, on two programs: the psyqo `hello` example, and the pad program
    below, whose input changes the console. It requires a save and a load in
    memory to take under 16 ms together on average, except on macOS (see
    "Lockstep first, rollback after"). Stock SwanStation fails it at the
    first frame (the state names the disc's path); without the first patch it
    fails at the frame the state was saved at (the combination); without the
    third the memory-state run fails on the picture of a frame whose video
    memory came from the state; without the fourth the pad program's
    memory-state run fails at its first frame (the sound unit's timing).
- The pad program (`make_test_disc.pad_program`, MIPS written out in Python
  as the corner program is, so CI needs no PS1 compiler for it) reads both
  controller ports every frame through the controller port's registers, keeps
  the replies in main RAM and draws them; Triangle on port 1 makes it lock
  that controller in digital mode with the DualShock's configuration
  commands, Square unlocks it in analog mode, and Circle turns the SPU's
  interrupt on. [`tests/pad_test.py`](../tests/pad_test.py) runs it on two
  hosts in lockstep and reads the replies back from each host's state after
  each frame of a script: both consoles must read exactly the buttons,
  sticks and mode given, the combination must switch analog mode and do
  nothing while the game holds the lock, and port 1's mode and lock must
  survive port 2 being unplugged and plugged in (the fifth patch).
- [`tests/rollback_test.py`](../tests/rollback_test.py) runs a host in plain
  lockstep with every frame's input as the reference, and two players' hosts
  in rollback (window 8). Each player knows its own port at once and gets the
  other's 1 to 6 frames late with jitter, steps every frame as soon as the
  window allows with the late port `null`, and corrects it when it arrives.
  The input is held for 1 to 12 frames at a time, so some predictions are
  wrong, and port 2 is unplugged for 20 frames. Every frame's `sync` from both
  players must match the reference's, RAM, picture and state, after dozens of
  rollbacks each; the frames run again may add at most one picture per
  rollback. A second phase checks every frame with `lockstep.state` `false`,
  as a launcher comparing only RAM asks (RAM and picture must match, no state
  is hashed, and the test core counts that it was not serialized for netplay),
  and a third every 60 frames, as in a match. Each prints the cost of a
  rollback: loading the state, and each frame run again. With the test
  core it also checks predictions that come true (no rollback), the
  refusals, and the protocol errors. CI runs it with the test core on every
  system and under ThreadSanitizer, and with the real core on the pad program
  on Linux, macOS and Windows. A simulated frame lasts at least twice the
  reference host's time per frame plus a save in memory, so a slow host
  (ThreadSanitizer, hosted macOS) does not fall so far behind its steps that
  every late input arrives before its frame runs and nothing is predicted.
  With the test core it also reads the frame ring after a rollback ending on
  repeated pictures (the newest picture made again must be there) and checks
  that frames run again send no `rumble`.

What a rollback costs, measured by `rollback_test.py` on the real core (pad
program, with a check every 60 frames as in a match, 34 to 48 rollbacks of 1
to 6 frames per player; averages over both players, and each frame run again
includes the save in memory before it when it is still predicted). It was
profiled first (Linux `perf` with frame pointers, and the host's own timers
around each part), which found where the time went:

- **Hashing zeros.** SwanStation asks for an 11 MiB state buffer and fills
  about a third of it. Every `sync` cleared the whole buffer and ran FNV-1a
  over all of it a byte at a time: 13 ms of a frame with a check, two thirds
  of the host's time in a run checking every frame. The host now clears only
  what the last state used and hashed the zero tail as one power of the FNV
  prime, which gave the same hash; since `syncXxh3` the state's hash is
  seeded with the buffer's size and reads no zero tail at all ("The checks'
  own cost" below).
- **Video memory read back from the GPU.** A state saved under a GPU read the
  console's video memory back from it, a stall on every save; on hosted
  macOS's virtual GPU, most of the 63 ms of a frame run again. A rollback
  state now keeps it in a GPU texture (SwanStation patch 7); a slot, which
  can wait any time to be loaded, still reads it back.
- **Taking the disc out at every load.** SwanStation's load took the disc
  out of the drive and put it back, reading its system area and waiting on
  the CD reader's thread twice, though the CD unit's own state restores the
  drive (read from the code: the profile showed the load's time spent
  waiting, not copying). Patch 7 leaves the disc in for a memory state of
  the same disc.
- **One stream call per array element.** A state wrote the memory cards and
  the CD sector buffers a byte at a time through a virtual call, half of a
  save's own work; patch 7 writes such arrays in one copy (the same bytes).
- **The picture's conversion.** The software picture's conversion decided
  its pixel format for every pixel; it now has a loop for each.

Run again on the same machine before (`netplay-design` fb85606, patches 1 to
6) and after, each twice, alternating:

| Machine | Loading the state | Each frame run again | Each frame run again, checking every frame |
| --- | --- | --- | --- |
| Desktop PC, Windows 11, OpenGL on an NVIDIA GPU | 5.4 and 18 → 0.9 and 1.1 ms | 4.9 and 5.4 → 2.9 and 3.0 ms | 22.5 and 25.2 → 12.1 and 12.4 ms |
| The same PC under WSL 2 (Ubuntu 22.04, no OpenGL: software rendering) | 3.9 and 5.7 → 1.4 and 1.5 ms | 3.0 and 5.3 → 2.2 and 6.6 ms | 19.7 and 23.6 → 10.6 and 11.3 ms |

The WSL figures are from a PC busy with other work, which is why each pair
differs (the second "after" run of each frame run again took 4.1 and 9.1 ms
for the two players). With the same changes `lockstep_test.py`'s slots, saved
in memory and loaded before every frame, took 1.4 to 1.9 ms to save in three
of the four runs (one Windows run took 4.2 ms, one save in it 68 ms; 2.6 to
3.2 ms before) and 1.3 to 1.8 ms to load (4.2 to 12 ms before).

In CI, real-core run 43755458414 (before) against run 43792444819 (after,
on 098cc0a):

| Runner | Loading the state | Each frame run again | Each frame run again, checking every frame |
| --- | --- | --- | --- |
| Windows Server 2022 (software rendering) | 3.1 → 1.2 ms | 3.1 → 1.8 ms | 22 to 27 → 9.0 ms |
| Ubuntu 22.04 (Mesa) | 6.0 → 0.8 ms | 4.3 → 1.6 ms | 22 to 27 → 9.6 ms |
| Ubuntu 24.04 (Mesa) | 6.8 → 1.3 ms | 5.9 → 2.6 ms | 22 to 27 → 12.7 ms |
| hosted macOS (virtual GPU) | 5.9 → 10.0 ms | 63 → 4.5 ms | 77 ms |

A rollback there ran about 4 frames again on average and at most 6. The
whole rollback, measured by the next run (43794161139, on 7bb0a32), took on
average (and at most) 5.3 to 5.6 ms (13 ms) on Windows, 12.3 to 12.8 ms
(25 ms) on Ubuntu 22.04, 12.1 to 12.6 ms (24 ms) on 24.04 and 25 to 28 ms
(111 ms) on hosted macOS; runners vary from run to run, which is why
Ubuntu 22.04's frames run again cost less in the first run.
`rollback_test.py --budget-ms` holds the average whole rollback (the load
and every frame it ran again) to the budget: `real-core` gives 24 ms on
Windows and Linux and 80 ms on hosted macOS. The same commit's runs differed
by up to half on one runner (Ubuntu 22.04 took 12.3 to 16.1 ms, hosted
macOS 25 to 39 ms), so a 16 ms budget failed on a slow run; before these
changes a whole rollback was about 32 ms on Ubuntu 24.04 (6.8 ms and about 4
frames of 5.9 ms) and about 270 ms on hosted macOS. A 6-frame rollback can still take longer than a frame; the test
prints the longest. On hosted macOS the load got slower (the texture copy on
its virtual GPU) and a check with the state still reads video memory back,
as a `sync`'s state hash needs it, so checking every frame with the state
stays at about 70 ms there (without it, about 4 ms: "The checks' own cost"). Whether
video memory came back right is checked by that hash: in the first phase
every frame's `sync.state`, which holds all of video memory read back, must
equal the lockstep reference's after dozens of rollbacks whose loads came
from GPU textures, and the pad program draws what it reads from the
controllers, so a stale texture shows. Persistent video memory a game draws
once and keeps, and a disc being read during a rollback, are not in the
test programs.

The disc and the arrays were changed together and not measured apart; the
profile put the load's time outside its own copies (waiting), and the arrays
at half of a save's work in the core.

**The checks' own cost.** After those changes a check was still a spike of
its own: main RAM (2 MiB), the state (about 4 MiB) and every picture were
hashed with FNV-1a a byte at a time, and the state's serialization read
video memory back from the GPU. Checking every frame, a frame run again cost
8.9 ms on Windows against 1.6 ms without a check, 12.7 to 14.8 ms on Ubuntu
against 2.6 to 3.2, and about 70 ms on hosted macOS against about 4. The
hashes are now XXH3-64 (capability `syncXxh3`), and `lockstep.state` `false`
leaves the state, and so the serialization, out ("Desync checks" above).
`rollback_test.py`'s whole rollback, the range of the two players' averages
(and the longest of either), in real-core run 43796871007 (before, on 7b769cd) against run
43801358387 (after, on b1ea5ef):

| Runner | Checking every frame, the state included | Checking every frame, no state (new) | Checking every 60 frames |
| --- | --- | --- | --- |
| Windows Server 2022 (software rendering) | 37.8 to 39.7 (59.5) → 11.8 to 12.0 ms (24.7) | 8.1 to 8.6 ms (14.1) | 7.5 to 8.0 (17.0) → 7.4 to 7.6 ms (11.8) |
| Ubuntu 22.04 (Mesa) | 61.8 to 64.9 (101) → 24.9 to 25.3 ms (48.2) | 11.4 to 11.9 ms (19.9) | 14.9 to 15.8 (29.0) → 10.4 to 10.6 ms (17.3) |
| Ubuntu 24.04 (Mesa) | 53.6 to 56.0 (82.7) → 19.7 to 19.9 ms (40.7) | 9.2 to 9.4 ms (15.6) | 12.2 to 12.7 (23.9) → 8.3 ms (14.0) |
| hosted macOS (virtual GPU) | 299 to 314 (553) → 290 to 297 ms (547) | 24.4 to 24.5 ms (35.6) | 26.7 to 30.4 (100) → 24.9 to 30.2 ms (93.0) |

A rollback ran about 4 frames again on average and at most 6 in every
phase. With the state, a frame run again still pays for the state's
serialization (on Ubuntu 2.5 to 3.2 ms of its 4.4 to 5.7 ms: video memory
is read back from Mesa) and on hosted macOS for the read-back from its virtual
GPU, about 64 ms, which no hash makes cheaper; without it every runner's
frame run again costs what it does with no check at all (1.6 to 3.7 ms
against 1.5 to 5.1 ms checking every 60 frames). So a launcher that
compares only RAM and sets `state` `false` checks every frame for about
what one check every 60 frames cost. The longest rollback on Linux, 29 ms before
when checking every 60 frames, was 17 ms checking every 60 frames and 20 ms
checking every frame without the state. The longest rollbacks on hosted macOS checking
every 60 frames (93 ms) are the ones that ran a check frame with the state;
`state` `false` leaves those out too.

`real-core` holds each phase's average to a budget (`rollback_test.py`
`--state-budget-ms`, `--ram-budget-ms`, `--budget-ms`): with the state,
24 ms on Windows and 36 ms on Linux, which every runner exceeded before;
without it, 24 ms on Windows and Linux and 50 ms on hosted macOS (about 300
ms there before); every 60 frames as before, 24 and 80 ms. Hosted macOS has
no budget with the state, whose read-back is the virtual GPU's.

- The core build identity: `host_test.py`'s `core_identity` test runs the
  test core with several `library_version` values and checks `status.core`
  passes printable ASCII of at most 256 bytes as it is, gives `""` for
  anything else, keeps the identity after the game ends, and is `null` for a
  core that did not load. With the real core, `real-core`'s `same-core` job
  compares what Windows, Linux and macOS report, as "Core build identity"
  says.
- The frame rate: `host_test.py`'s `game_timing` test checks `status.timing`
  for the test core's NTSC (60) and PAL (50) games and for one that switches
  to 50 while it runs, and that a paced game, on its own or in lockstep, runs
  no faster than that; `real_core.py` checks SwanStation's rate and region on
  an auto-detected console and a PAL one ("Frame rate" above).

Next, for the launcher:

- Set `lockstep.state` `false` (it compares only `sync.ram`), and refuse a
  friend whose host differs from its own in listing `syncXxh3` ("Desync
  checks").
- The frame clock of a match follows `status.timing.fps` (the launcher's
  rollback clock is fixed at 60 Hz until then), and the input delay and the
  rollback window are counted in frames of that length.
- In the launcher, the existing input-wire, checkpoint-transfer and
  network-simulation tests, pointed at two hosts instead of the in-process
  runner, under delay, jitter, loss and reordering, with rollback.
- Two PCs, Windows and Linux, through the AnyTogether service, first direct
  and then forced through the relay, playing a real game to the end.
