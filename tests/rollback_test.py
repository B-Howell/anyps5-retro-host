# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Rollback (protocol.md "Rollback") checked against lockstep. Three hosts boot the same game: a reference in
plain lockstep, given every frame's input for both ports, and two players' hosts in rollback, each of which
knows its own port's input at once and gets the other port's after a simulated network delay with jitter.
Each player steps every frame as soon as the rollback window lets it, leaving the other port unknown (the
host predicts it), and sends each late input as a correction. Wrong predictions make the host load the
state from before the frame and run the frames again. Every frame's sync, sent once the frame is confirmed,
must match the reference's: main RAM, picture and state. The players also unplug and plug in port 2 on the
way. Then they check every frame without the state (lockstep's state false: RAM and picture must match, and
no state is hashed), and last every 60 frames, as a match does. The test counts the rollbacks, checks that the
frames run again showed only their newest picture, and prints what a rollback cost in each phase.

With the test core it also checks the protocol errors and refusals of rollback. With --bios it runs the
real SwanStation core on one of make_test_disc.py's programs (--program), each of which reads both
controllers, so a wrong prediction changes the console: the pad program acts on the analog lock and turns
the SPU's interrupt on and off; the vram program builds its picture over many frames from video memory it
drew once and kept, a texture uploaded at boot among it; the disc program reads the disc all the time and
seeks where the input says. A rollback must bring each of these back as it was, which every frame's state
hash (all of video memory and the CD unit with the rest) and RAM hash check."""

import argparse
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402
import lockstep_test as ls  # noqa: E402
import make_test_disc as disc  # noqa: E402

WINDOW = 8
PROGRAMS = {'pad': disc.pad_program, 'vram': disc.vram_program, 'disc': disc.disc_program}


def held_script(frames, seed):
    """Each port's input, held for 1 to 12 frames at a time, with port 2 unplugged for a stretch: some
    predictions (the last known input) come true and some do not."""
    chance = random.Random(seed)
    script = []
    held = [None, None]
    left = [0, 0]
    for frame in range(frames):
        for index in range(2):
            if left[index] == 0:
                held[index] = ls.port(chance.randrange(65536), chance.choice((-32768, 0, 32767)))
                left[index] = chance.randrange(1, 13)
            left[index] -= 1
        unplugged = frames // 3 <= frame < frames // 3 + 20
        script.append([dict(held[0])] if unplugged else [dict(held[0]), dict(held[1])])
    return script


class Player:
    """One player's launcher: its own port's input is known at once, the other port's arrives late."""

    def __init__(self, peer, own, script, seed, most_delay):
        self.peer = peer
        self.other = 1 - own
        self.script = script
        self.next = 0
        self.delivered = 0
        self.unknown = []
        self.end = 0
        chance = random.Random(seed)
        self.arrival = []
        latest = 0
        for frame in range(len(script)):
            latest = max(latest, frame + chance.randrange(1, most_delay + 1))
            self.arrival.append(latest)

    def done(self):
        return self.next == self.end and self.delivered >= self.end

    def tick(self, now):
        """At simulated frame time now: sends the inputs that have arrived, as corrections for frames
        stepped without them, and steps the next frame when it is due and the rollback window allows."""
        while self.delivered < len(self.script) and self.arrival[self.delivered] <= now:
            frame = self.delivered
            if self.unknown and self.unknown[0] == frame:
                self.unknown.pop(0)
                self.peer.correct(frame, self.other, self.script[frame][self.other])
            self.delivered += 1
        if self.next >= self.end or self.next > now:
            return
        frame = self.next
        if self.unknown and frame - self.unknown[0] >= WINDOW:
            return
        ports = [dict(state) for state in self.script[frame]]
        if self.other < len(ports) and frame >= self.delivered:
            ports[self.other] = None
            self.unknown.append(frame)
        self.peer.step(frame, ports)
        self.next += 1


def rollback_args(check=1, state=True):
    return {'check': check, 'paced': False, 'state': state, 'rollback': {'frames': WINDOW}}


def frame_seconds(options, reference, frames, elapsed):
    """How long a simulated frame lasts: --tick, or longer on a host slower than that. A host that runs
    frames slower than they are stepped falls behind and takes each late input before it runs the frame,
    so it never predicts; twice the reference's time per frame plus a save in memory (each predicted frame
    saves first) keeps the players' hosts within a frame or two of their steps."""
    saves = []
    for slot in range(4):
        saved = reference.call('saveState', {'slot': slot})
        if not saved:
            raise c.Failed('reference: saveState in a slot was refused')
        saves.append(saved['micros'] / 1e6)
    return max(options.tick, 2 * (elapsed / frames + sum(saves) / len(saves)))


def play_phase(tick, players, first, count):
    """Runs both players through frames first to first + count - 1 on simulated time, each frame lasting
    tick seconds. Returns the seconds it took."""
    began = time.monotonic()
    deadline = began + 900
    now = first
    for player in players:
        player.end = first + count
    while not all(player.done() for player in players):
        for player in players:
            player.tick(now)
        # While a simulated frame lasts, the hosts run what they were sent; every host is read in turn, since
        # one left unread blocks on a full socket.
        tick_end = time.monotonic() + tick
        while time.monotonic() < tick_end:
            for player in players:
                try:
                    player.peer.pump(timeout=0.001)
                except c.Idle:
                    pass
        now += 1
        if time.monotonic() > deadline:
            raise c.Failed('the players did not finish within 15 minutes')
    return time.monotonic() - began


def wait_for_sync(peers, frame, seconds=120):
    """Waits until every peer has sent the sync of frame, reading each in turn: a host left unread blocks
    on a full socket and, after protocol.md's 5-second write limit, exits."""
    deadline = time.monotonic() + seconds
    waiting = list(peers)
    while waiting:
        for peer in list(waiting):
            if frame in peer.syncs:
                waiting.remove(peer)
                continue
            if time.monotonic() > deadline:
                raise c.Failed(f'{peer.name}: no sync for frame {frame}')
            try:
                peer.pump(lambda m: m.get('type') == 'sync', 0.05)
            except c.Idle:
                pass


def check_rollbacks(peer, reports, pictures, reference_pictures):
    """The rollback messages are in range, and the frames run again showed only their last picture: a
    rollback adds at most one picture to the ones lockstep showed for the same frames. Returns a summary and the
    average milliseconds of a whole rollback (its load and every frame it ran again)."""
    if not reports:
        raise c.Failed(f'{peer.name}: no prediction was ever wrong, so nothing was rolled back')
    for report in reports:
        if not (0 < report['frames'] <= WINDOW and report['loadMicros'] <= report['micros']):
            raise c.Failed(f'{peer.name}: a rollback message is out of range: {report}')
    if not reference_pictures <= pictures <= reference_pictures + len(reports):
        raise c.Failed(f'{peer.name}: {pictures} pictures where lockstep showed {reference_pictures}, with '
                       f'{len(reports)} rollbacks: frames run again must show only their last picture')
    again = sum(report['frames'] for report in reports)
    load = sum(report['loadMicros'] for report in reports) / len(reports) / 1000
    per_frame = sum(report['micros'] - report['loadMicros'] for report in reports) / again / 1000
    whole = sum(report['micros'] for report in reports) / len(reports) / 1000
    longest = max(report['micros'] for report in reports) / 1000
    return (f'{len(reports)} rollbacks ran {again} frames again (at most '
            f'{max(report["frames"] for report in reports)} at once); loading the state took {load:.2f} ms '
            f'and each frame run again {per_frame:.2f} ms, on average; a whole rollback {whole:.2f} ms on '
            f'average, {longest:.2f} ms at most'), whole


def check_hardware(peer):
    """The host drew with the core's OpenGL renderer, under which a rollback state keeps video memory in
    a GPU texture (SwanStation patch 7) rather than in the state's bytes."""
    if not peer.call('status')['hardware']:
        raise c.Failed(f'{peer.name}: this machine has an OpenGL renderer, but the host made no context for '
                       'the core')


def within(peer, what, whole, budget):
    """A rollback's load and frames run again all happen between two frames of play. The average is held
    to the budget; the longest is printed, since a 6-frame rollback may exceed a frame where 2 do not."""
    if budget is not None and whole > budget:
        raise c.Failed(f'{peer.name}: a whole rollback {what} took {whole:.2f} ms on average, over the '
                       f'{budget} ms budget')


def same_ram(name, ours, theirs):
    """Fails at the first frame whose RAM or picture differs from lockstep's, or that has a state hash
    though lockstep was told to leave the state out. Returns how many frames were compared."""
    if len(ours) != len(theirs):
        raise c.Failed(f'{name}: {len(ours)} frames against {len(theirs)}')
    for mine, other in zip(ours, theirs):
        if (mine['frame'], mine['ram'], mine['video']) != (other['frame'], other['ram'], other['video']):
            raise c.Failed(f'{name}: frame {mine["frame"]} differs: {mine} against {other}')
        if mine['ram'] is None or mine['state'] is not None:
            raise c.Failed(f'{name}: frame {mine["frame"]} should have a RAM hash and no state hash: {mine}')
    return len(ours)


def reset(peers):
    for peer in peers:
        peer.syncs.clear()
        peer.log.clear()
        peer.rollbacks.clear()
        peer.pictures = 0


def checked_length(start, frames):
    """About frames frames from start, ending on a frame a check every 60 frames is due after, so every
    picture has come with its sync."""
    return frames // 60 * 60 + (1 - start) % 60


def netplay_serializes(match):
    """How many times the test core was serialized for netplay (a sync's state hash) since the test
    began, or None with the real core."""
    if match.real:
        return None
    counted = Path(match.paths['system']) / 'fake-netplay-serialized'
    return counted.stat().st_size if counted.exists() else 0


def network(options, match):
    """Phase 1 compares every frame with lockstep: RAM, picture and state. Phase 2 checks every frame
    without the state (lockstep's state false), as a launcher that compares only RAM asks: no check
    serializes the core. Phase 3 checks every 60th frame, as a match would. Each prints what a rollback
    cost and, with the real core, holds it to its budget."""
    frames = options.frames
    later = options.frames // 2
    start = frames + later
    last = checked_length(start, options.frames // 2)
    script = held_script(start + last, 7)
    if not match.real:
        (Path(match.paths['system']) / 'fake-count-netplay').write_bytes(b'')
    reference = match.peer('reference')
    first, second = match.peer('player1'), match.peer('player2')
    reference.call('start', match.start_args('reference', lockstep=True))
    for peer in (first, second):
        peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
    began = time.monotonic()
    expected = ls.advance([reference], 0, frames, lambda peer, frame: script[frame])['reference']
    tick = frame_seconds(options, reference, frames, time.monotonic() - began)
    players = [Player(first, 0, script, 11, options.delay), Player(second, 1, script, 13, options.delay)]
    peers = [first, second]
    elapsed = play_phase(tick, players, 0, frames)
    wait_for_sync(peers, frames - 1)
    if options.expect_opengl:
        for peer in peers:
            check_hardware(peer)
    for peer in peers:
        if peer.log != list(range(frames)):
            raise c.Failed(f'{peer.name}: syncs came for frames {peer.log[:20]}..., not each frame once in order')
        compared = ls.same(f'{peer.name} against lockstep', [peer.syncs[f] for f in range(frames)], expected)
        summary, whole = check_rollbacks(peer, peer.rollbacks, peer.pictures, reference.pictures)
        print(f'{peer.name}: {compared} frames match lockstep in RAM, picture and state; {summary}')
        if match.real:
            within(peer, 'checking every frame', whole, options.state_budget_ms)
    print(f'rollback: two players with up to {options.delay} frames of delay and jitter, {tick * 1000:.0f} ms a '
          f'frame, {elapsed:.1f} s')

    serialized = netplay_serializes(match)
    if serialized == 0:
        raise c.Failed('checking every frame with the state never serialized the core for netplay')
    reset((reference, first, second))
    for peer in (reference, first, second):
        args = rollback_args(1, False) if peer is not reference else {'check': 1, 'paced': False, 'state': False}
        if peer.call('lockstep', args) != {'frame': frames}:
            raise c.Failed(f'{peer.name}: lockstep without the state did not keep the frame count')
    began = time.monotonic()
    expected = ls.advance([reference], frames, later, lambda peer, frame: script[frame])['reference']
    tick = frame_seconds(options, reference, later, time.monotonic() - began)
    play_phase(tick, players, frames, later)
    wait_for_sync(peers, start - 1)
    for peer in peers:
        if peer.log != list(range(frames, start)):
            raise c.Failed(f'{peer.name}: syncs came for frames {peer.log[:20]}..., not each frame once in order')
        compared = same_ram(f'{peer.name} without the state', [peer.syncs[f] for f in range(frames, start)],
                            expected)
        summary, whole = check_rollbacks(peer, peer.rollbacks, peer.pictures, reference.pictures)
        print(f'{peer.name}, checking every frame without the state ({tick * 1000:.0f} ms a frame): {compared} '
              f'frames match lockstep in RAM and picture; {summary}')
        if match.real:
            within(peer, 'checking every frame without the state', whole, options.ram_budget_ms)
    if netplay_serializes(match) != serialized:
        raise c.Failed('a check without the state serialized the core')

    reset((reference, first, second))
    for peer in (reference, first, second):
        args = rollback_args(60) if peer is not reference else {'check': 60, 'paced': False}
        if peer.call('lockstep', args) != {'frame': start}:
            raise c.Failed(f'{peer.name}: lockstep with a check every 60 frames did not keep the frame count')
    checked = [frame for frame in range(start, start + last) if frame % 60 == 0]
    began = time.monotonic()
    for frame in range(start, start + last):
        reference.step(frame, script[frame])
    wait_for_sync([reference], checked[-1])
    tick = frame_seconds(options, reference, last, time.monotonic() - began)
    play_phase(tick, players, start, last)
    wait_for_sync(peers, checked[-1])
    for peer in peers:
        if peer.log != checked:
            raise c.Failed(f'{peer.name}: syncs came for frames {peer.log}, not {checked}')
        ls.same(f'{peer.name} checked every 60 frames', [peer.syncs[f] for f in checked],
                [reference.syncs[f] for f in checked])
        summary, whole = check_rollbacks(peer, peer.rollbacks, peer.pictures, reference.pictures)
        print(f'{peer.name}, checking every 60 frames ({tick * 1000:.0f} ms a frame): {summary}')
        if match.real:
            within(peer, 'checking every 60 frames', whole, options.budget_ms)
    return players


def right_guesses(match):
    """Predictions that come true cost no rollback, and the frames still match lockstep."""
    peer = match.peer('steady')
    reference = match.peer('steady-reference')
    peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
    reference.call('start', match.start_args(reference.name, lockstep=True))
    steady = [ls.port(5), ls.port(9)]
    expected = ls.advance([reference], 0, 30, lambda p, f: steady)['steady-reference']
    ls.advance([peer], 0, 1, lambda p, f: steady)
    for frame in range(1, 30):
        peer.step(frame, [steady[0], None])
        if frame >= WINDOW:
            peer.correct(frame - WINDOW + 1, 1, steady[1])
    for frame in range(30 - WINDOW + 1, 30):
        peer.correct(frame, 1, steady[1])
    deadline = time.monotonic() + 30
    while 29 not in peer.syncs:
        if time.monotonic() > deadline:
            raise c.Failed('steady: no sync for frame 29')
        try:
            peer.pump(lambda m: m.get('type') == 'sync', 0.5)
        except c.Idle:
            pass
    ls.same('steady', [peer.syncs[f] for f in range(30)], expected)
    if peer.rollbacks:
        raise c.Failed(f'steady: right predictions caused rollbacks: {peer.rollbacks}')
    print('predictions that come true cause no rollback and match lockstep')


def predicted(peer, ports):
    """Steps frames 5 to 7 with ports, one of them unknown, and waits until the host has shown their
    three pictures, so it ran them on its prediction before any correction comes."""
    shown = peer.pictures
    for frame in range(5, 8):
        peer.step(frame, ports)
    deadline = time.monotonic() + 10
    while peer.pictures < shown + 3:
        if time.monotonic() > deadline:
            raise c.Failed(f'{peer.name}: the predicted frames showed {peer.pictures - shown} pictures, not 3')
        try:
            peer.pump(timeout=0.1)
        except c.Idle:
            pass


def settled(peer, last):
    """Waits for the sync of frame last and reads what followed. Returns the rollbacks, which must all
    end after frame last: the host takes whichever corrections have come at each frame's end, so the
    corrections of frames 5 to 7 may come as one rollback or several, each from its first wrong frame."""
    wait_for_sync([peer], last, 10)
    time.sleep(0.3)
    peer.drain()
    if not peer.rollbacks or peer.rollbacks[0]['frame'] != 5 or             any(report['frame'] + report['frames'] != last + 1 for report in peer.rollbacks):
        raise c.Failed(f'{peer.name}: expected rollbacks from frame 5 to {last}, got {peer.rollbacks}')
    return peer.rollbacks


def resimulated_picture(match):
    """After a rollback whose last frames repeat an earlier picture, the newest picture the frames made
    again is what the frame ring shows, not the predicted one; and frames run again show nothing before
    it."""
    system = Path(match.paths['system'])
    marker = system / 'fake-port2-picture'
    marker.write_bytes(b'')
    try:
        peer = match.peer('picture')
        peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
        ls.advance([peer], 0, 5, lambda p, f: [ls.port(0), ls.port(16)])
        time.sleep(0.3)
        peer.drain()
        shown = peer.pictures
        predicted(peer, [ls.port(0), None])
        if peer.first_pixel()[0] != 16:
            raise c.Failed(f'picture: the predicted frames show {peer.first_pixel()}, not 16 in red, from port 2')
        select = 1 << 2
        peer.correct(5, 1, ls.port(32))
        peer.correct(6, 1, ls.port(select))
        peer.correct(7, 1, ls.port(select))
        rollbacks = settled(peer, 7)
        # Each rollback shows one picture at most; the first, from frame 5, made one.
        if not shown + 4 <= peer.pictures <= shown + 3 + len(rollbacks):
            raise c.Failed(f'picture: {peer.pictures - shown} pictures for 3 predicted frames and '
                           f'{len(rollbacks)} rollbacks ending on a repeated picture')
        if peer.first_pixel()[0] != 32:
            raise c.Failed(f'picture: after the rollback the ring shows {peer.first_pixel()}, not 32 in red, '
                           'made by frame 5')
        peer.host.send_message({'type': 'shutdown'})
        if peer.host.exits(15) != 0:
            raise c.Failed('picture: the host did not exit cleanly')
    finally:
        marker.unlink(missing_ok=True)
    print('a rollback ending on a repeated picture shows the newest picture made again')


def resimulated_rumble(match):
    """The test core rumbles port 1's large motor while Cross is held there. Frames run again send no
    rumble of their own: once they are done, the player is told only of motors that ended differently
    from what it was last told."""
    cross = 1 << 0
    for name, corrections, ends_on in (('rumble-off', [0, 0, 0], False),
                                       ('rumble-on', [0, cross, cross], True)):
        peer = match.peer(name)
        peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
        ls.advance([peer], 0, 5, lambda p, f: [ls.port(cross), ls.port(0)])
        time.sleep(0.3)
        peer.drain()
        if [(m['port'], m['strong']) for m in peer.rumbles] != [(0, 65535)]:
            raise c.Failed(f'{name}: holding Cross sent {peer.rumbles}, not one rumble on')
        predicted(peer, [None, ls.port(0)])
        if len(peer.rumbles) != 1:
            raise c.Failed(f'{name}: the predicted frames, Cross still held, sent {peer.rumbles[1:]}')
        for frame, buttons in zip(range(5, 8), corrections):
            peer.correct(frame, 0, ls.port(buttons))
        rollbacks = settled(peer, 7)
        told = [m['strong'] for m in peer.rumbles if m['port'] == 0]
        if any(a == b for a, b in zip(told, told[1:])) or told[-1] != (65535 if ends_on else 0):
            raise c.Failed(f'{name}: after {len(rollbacks)} rollbacks the player was told {told}')
        if len(rollbacks) == 1 and len(told) != (1 if ends_on else 2):
            raise c.Failed(f'{name}: one rollback sent the rumble of the frames run again: {told}')
        peer.host.send_message({'type': 'shutdown'})
        if peer.host.exits(15) != 0:
            raise c.Failed(f'{name}: the host did not exit cleanly')
    print('frames run again send no rumble; motors that ended differently are told once')


def refusals(match):
    """What rollback refuses while frames are not confirmed, and what it forgets on a load."""
    peer = match.peer('refusals')
    peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
    ls.advance([peer], 0, 5)
    saved = match.work / 'confirmed.state'
    if not peer.call('saveState', {'path': str(saved)}):
        raise c.Failed('a game with every frame confirmed did not save its state')
    peer.step(5, [ls.port(1), None])
    peer.step(6, [ls.port(1), None])
    time.sleep(0.3)
    if peer.call('saveState', {'slot': 0}) is not False:
        raise c.Failed('a state was saved while frames were not confirmed')
    if peer.call('lockstep', {'check': 1, 'paced': False}) is not False:
        raise c.Failed('rollback was turned off while frames were not confirmed')
    if peer.call('lockstep', rollback_args()) != {'frame': 7}:
        raise c.Failed('lockstep with the same settings did not keep the frame count')
    peer.syncs.clear()
    peer.log.clear()
    peer.correct(5, 1, ls.port(2))
    peer.correct(6, 1, ls.port(2))
    deadline = time.monotonic() + 10
    while 6 not in peer.syncs:
        if time.monotonic() > deadline:
            raise c.Failed('corrected frames sent no sync')
        try:
            peer.pump(lambda m: m.get('type') == 'sync', 0.5)
        except c.Idle:
            pass
    if peer.log != [5, 6]:
        raise c.Failed(f'the corrected frames sent syncs {peer.log}')
    if not peer.call('saveState', {'slot': 0}):
        raise c.Failed('a game whose frames were all confirmed again did not save its state')
    peer.step(7, [ls.port(1), None])
    if peer.call('loadState', {'path': str(saved)}) is False:
        raise c.Failed('a state did not load over frames not confirmed')
    peer.step(5, [ls.port(1), ls.port(1)])
    if peer.call('saveState', {'slot': 1}) is False:
        raise c.Failed('a load did not forget the frames not confirmed')
    for args in ({'check': 1, 'rollback': {'frames': 0}}, {'check': 1, 'rollback': {'frames': 9}},
                 {'check': 1, 'rollback': 8}, {'check': 1, 'rollback': {}}):
        reply = peer.call_raw('lockstep', args)
        if 'error' not in reply or reply['error']['code'] != 'argument':
            raise c.Failed(f'lockstep {args} was not an argument error: {reply}')
    peer.host.send_message({'type': 'shutdown'})
    if peer.host.exits(15) != 0:
        raise c.Failed('refusals: the host did not exit cleanly')
    print('rollback refuses to save or turn off with frames not confirmed, and a load forgets them')


def protocol_errors(match):
    """Each of these breaks a rule of step or correct, and the host ends the connection."""
    known, unknown = ls.port(0), None
    cases = [
        ('an unknown port in plain lockstep', {'check': 1, 'paced': False}, [('step', 0, [known, unknown])]),
        ('a step beyond the rollback window', {'check': 1, 'paced': False, 'rollback': {'frames': 2}},
         [('step', 0, [known, unknown]), ('step', 1, [known, unknown]), ('step', 2, [known, unknown])]),
        ('a known input after an unknown one for the same port', rollback_args(),
         [('step', 0, [known, unknown]), ('step', 1, [known, known])]),
        ('a correction for a port already known', rollback_args(),
         [('step', 0, [known, unknown]), ('correct', 0, 0)]),
        ('a correction out of order', rollback_args(),
         [('step', 0, [known, unknown]), ('step', 1, [known, unknown]), ('correct', 1, 1)]),
        ('a correction for a frame not stepped', rollback_args(), [('step', 0, [known, unknown]), ('correct', 1, 1)]),
        ('a correction in plain lockstep', {'check': 1, 'paced': False}, [('step', 0, [known, known]),
                                                                        ('correct', 0, 1)]),
        ('a correction for a port not connected', rollback_args(), [('step', 0, [unknown]), ('correct', 0, 1)]),
    ]
    for what, lockstep, messages in cases:
        peer = match.peer('errors')
        peer.call('start', match.start_args('errors', lockstep=lockstep))
        for kind, frame, value in messages:
            if kind == 'step':
                peer.step(frame, value)
            else:
                peer.correct(frame, value, known)
        try:
            code = peer.host.exits(10)
        except c.Failed:
            raise c.Failed(f'the host did not end the connection after {what}')
        if code != 2:
            raise c.Failed(f'the host exited with {code} after {what}, not 2')
    print('steps and corrections that break rollback\'s rules are protocol errors')


def play(options):
    match = ls.Match(options)
    try:
        players = network(options, match)
        if not match.real:
            c.load_atomics(Path(options.core).resolve())
            right_guesses(match)
            resimulated_picture(match)
            resimulated_rumble(match)
            refusals(match)
            protocol_errors(match)
        for player in players:
            player.peer.host.send_message({'type': 'shutdown'})
            if player.peer.host.exits(15) != 0:
                raise c.Failed(f'{player.peer.name}: the host did not exit cleanly')
        print('rollback passed')
        return 0
    except c.Failed as error:
        print('FAILED:', error)
        return 1
    finally:
        match.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--core', required=True)
    parser.add_argument('--log')
    parser.add_argument('--bios', help='OpenBIOS, 512 KiB: run the real SwanStation core on a program of '
                        'make_test_disc.py')
    parser.add_argument('--program', choices=PROGRAMS, default='pad',
                        help='With --bios, the program: pad_program, vram_program or disc_program')
    parser.add_argument('--expect-opengl', action='store_true',
                        help='With --bios, fail unless the players\' hosts draw with the core\'s OpenGL renderer')
    parser.add_argument('--frames', type=int, default=600, help='Frames each player runs')
    parser.add_argument('--delay', type=int, default=6, help='The most frames a player\'s input arrives late')
    parser.add_argument('--budget-ms', type=float,
                        help='With the real core, the most a whole rollback (the load and every frame run '
                        'again) may take on average, while checking every 60 frames')
    parser.add_argument('--state-budget-ms', type=float,
                        help='The same while checking every frame, the state included')
    parser.add_argument('--ram-budget-ms', type=float,
                        help='The same while checking every frame without the state')
    parser.add_argument('--tick', type=float, default=0.004,
                        help='Seconds each simulated frame lasts at least, for the hosts to run what they were '
                        'sent; longer on a host slower than that')
    options = parser.parse_args()
    options.disc_data = disc.disc_data() if options.bios and options.program == 'disc' else None
    options.program = PROGRAMS[options.program]() if options.bios else None
    return play(options)


if __name__ == '__main__':
    sys.exit(main())
