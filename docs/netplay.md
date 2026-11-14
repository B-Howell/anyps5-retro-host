<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors -->

# PS1 online play through the host

This is the design for playing a PS1 game online with a friend when the
game runs in `anyps5-retro-host`. It says which program owns what, the
protocol it needs, how a match starts from the launcher's lobby, and how it is
tested. The first step, lockstep, is built: [protocol.md](protocol.md)
"Lockstep" is its contract, and [`tests/lockstep_test.py`](../tests/lockstep_test.py)
runs two real SwanStation hosts frame for frame alike.

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
builds it with three patches of its own (host.md "SwanStation patches") that
make its states fit for netplay; "Determinism" below says why. Its parent,
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
  socket on the same machine. It is far below a frame's 16.7 ms.

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

**Rollback** (capability `ps1Rollback`, proposed). The host runs ahead on
predicted input and, when the real input differs, loads the state from before
that frame and replays. It hides more of the round trip than delay alone, and
costs a state save every frame and a burst of replayed frames on each wrong
guess.

Lockstep comes first because:

- it needs the fewest new parts: the host waits, the launcher's input wire
  stays as it is, and nothing is undone;
- it is the control rollback is checked against: a rollback run must reach
  the same hashes as a lockstep run of the same input;
- a file-based state took about 60 ms to load and 9 ms to save through the
  protocol (SwanStation at 631f0fb, 11 MiB of state, Windows), far over a
  16.7 ms frame. So `saveState` and `loadState` also keep up to 8 states in
  memory (protocol.md "saveState and loadState"), saved and loaded in the
  core's same-instance form
  (`RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE`, which keeps the
  recompiled code and needs no file), and report what each took. The test
  below saves before every frame, runs it, loads and runs it again: on a
  desktop PC drawing with OpenGL on an NVIDIA GPU (Windows, 1x) a save took 2.4 ms
  on average (5.8 ms at most) and a load 3.2 ms (4.6 ms at most), so one of
  each fits a frame. CI's runners took 1.4 to 3.5 ms to save and 2.4 to
  4.4 ms to load on average (Windows in software, Linux with Mesa), but the
  hosted macOS runner, whose GPU is a virtual one, took 73 ms to save: a
  save under a GPU reads the console's video memory back from it, which is
  slow there. Whether a real Mac does better is not measured yet; if it does
  not, SwanStation would have to keep a same-instance state's video memory
  on the GPU, as DuckStation's own memory states do. Replaying several frames
  after a wrong guess costs their emulation on top;
- the launcher's own harness found that rollback with the stock core is not
  exact (the sound unit restore below), so rollback also depends on that
  patch, which the host does not carry yet.

A match picks *d* from the round trip it measures while the lobby is open:
*d* = ⌈(round trip ÷ 2) ÷ 16.7 ms⌉ + 1, at most 8. Above 8 frames (about a
270 ms round trip) the lobby says the connection is too slow for this game,
rather than offering a match that feels broken. Rollback later lowers the
delay players feel to a fixed 1 or 2 frames, with the rest predicted.

**Proposed `ps1Rollback`.** On top of `ps1Lockstep`:

- `lockstep` takes `"rollback":{"frames":<1 to 8>}`. The host then keeps
  that many states in memory, one before each frame not yet confirmed.
- `step` may carry `"guessed":[<port>,…]`, the ports whose input is a
  prediction. The host runs the frame at once.
- A new message, `correct`, gives the real input for a guessed frame. If it
  differs from the guess, the host loads the state from before that frame and
  replays to the newest frame, publishing only the last picture, and plays
  the replayed frames' sound once, not twice.
- A new message, `confirm`, says every input up to a frame is final. The host
  drops older states and sends `sync` only for confirmed frames, so the two
  players compare the same thing.

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
  sample event with timing from the discarded timeline. It has a patch for
  this, needed before rollback and not yet carried by the host. The `hello`
  program plays no sound, so the memory-state test cannot show it.
- **Memory cards.** A state carries both memory cards. The friend's game
  uses a `saves` directory made for the match, so the host never writes the
  host player's card over the friend's own (protocol.md "saveState and
  loadState").

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
  - With `--bios` and `--program`, it runs real SwanStation on the OpenBIOS
    and psyqo `hello` disc that `tests/make_test_disc.py` makes, each player
    with their own copy of the disc in a different folder under a different
    name. Every frame's main RAM, picture and state must match, and the saved
    state file must not hold the disc's folder or name. The `real-core`
    workflow runs it on Linux, macOS and Windows for 600 frames, with the
    host's SwanStation patches applied, and requires a save and a load in
    memory to take under 16 ms together on average, except on macOS (see
    "Lockstep first, rollback after"). Stock SwanStation fails
    it at the first frame (the state names the disc's path); without the
    first patch it fails at the frame the state was saved at (the
    combination); without the third the memory-state run fails on the
    picture of a frame whose video memory came from the state.

Next, for rollback and the launcher:

- A `ps1Rollback` test that runs the same input through a lockstep host and a
  rollback host fed late, guessed and corrected input, and requires the same
  confirmed `sync` hashes.
- In the launcher, the existing input-wire, checkpoint-transfer and
  network-simulation tests, pointed at two hosts instead of the in-process
  runner, under delay, jitter, loss and reordering.
- A test program on the disc that reads the controller, so RAM depends on
  input in a real game too. The psyqo `hello` program does not read it, so
  the real-core test shows that the two consoles stay alike, and the test
  core shows that input reaches the console.
- Two PCs, Windows and Linux, through the AnyTogether service, first direct
  and then forced through the relay, playing a real game to the end.
