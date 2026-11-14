# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Rollback (protocol.md "Rollback") checked against lockstep. Three hosts boot the same game: a reference in
plain lockstep, given every frame's input for both ports, and two players' hosts in rollback, each of which
knows its own port's input at once and gets the other port's after a simulated network delay with jitter.
Each player steps every frame as soon as the rollback window lets it, leaving the other port unknown (the
host predicts it), and sends each late input as a correction. Wrong predictions make the host load the
state from before the frame and run the frames again. Every frame's sync, sent once the frame is confirmed,
must match the reference's: main RAM, picture and state. The players also unplug and plug in port 2 on the
way. The test counts the rollbacks, checks that the frames run again showed only their last picture, and
prints what a rollback cost per frame run again.

With the test core it also checks the protocol errors and refusals of rollback. With --bios it runs the
real SwanStation core on make_test_disc.py's pad program, which reads both controllers, acts on the analog
lock and turns the SPU's interrupt on and off with the input, so a wrong prediction changes the console."""

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


def rollback_args(check=1):
    return {'check': check, 'paced': False, 'rollback': {'frames': WINDOW}}


def play_phase(options, players, first, count):
    """Runs both players through frames first to first + count - 1 on simulated time. Returns the seconds
    it took."""
    began = time.monotonic()
    deadline = began + 900
    now = first
    for player in players:
        player.end = first + count
    while not all(player.done() for player in players):
        for player in players:
            player.tick(now)
        # A simulated frame lasts options.tick seconds, while the hosts run what they were sent; every host is
        # read in turn, since one left unread blocks on a full socket.
        tick_end = time.monotonic() + options.tick
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
    deadline = time.monotonic() + seconds
    for peer in peers:
        while frame not in peer.syncs:
            if time.monotonic() > deadline:
                raise c.Failed(f'{peer.name}: no sync for frame {frame}')
            try:
                peer.pump(lambda m: m.get('type') == 'sync', 0.5)
            except c.Idle:
                pass


def check_rollbacks(peer, reports, pictures, reference_pictures):
    """The rollback messages are in range, and the frames run again showed only their last picture: a
    rollback adds at most one picture to the ones lockstep showed for the same frames."""
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
    return (f'{len(reports)} rollbacks ran {again} frames again (at most '
            f'{max(report["frames"] for report in reports)} at once); loading the state took {load:.2f} ms '
            f'and each frame run again {per_frame:.2f} ms, on average')


def network(options, match):
    """Phase 1 compares every frame with lockstep. Phase 2 checks every 60th frame, as a match would, so a
    frame run again costs what it does in play, and prints that cost."""
    frames = options.frames
    # About half as many again, ending on a frame a check is due after, so every picture has come with
    # its sync.
    later = options.frames // 2 // 60 * 60 + (1 - frames) % 60
    script = held_script(frames + later, 7)
    reference = match.peer('reference')
    first, second = match.peer('player1'), match.peer('player2')
    reference.call('start', match.start_args('reference', lockstep=True))
    for peer in (first, second):
        peer.call('start', match.start_args(peer.name, lockstep=rollback_args()))
    expected = ls.advance([reference], 0, frames, lambda peer, frame: script[frame])['reference']
    players = [Player(first, 0, script, 11, options.delay), Player(second, 1, script, 13, options.delay)]
    peers = [first, second]
    elapsed = play_phase(options, players, 0, frames)
    wait_for_sync(peers, frames - 1)
    for peer in peers:
        if peer.log != list(range(frames)):
            raise c.Failed(f'{peer.name}: syncs came for frames {peer.log[:20]}..., not each frame once in order')
        compared = ls.same(f'{peer.name} against lockstep', [peer.syncs[f] for f in range(frames)], expected)
        summary = check_rollbacks(peer, peer.rollbacks, peer.pictures, reference.pictures)
        print(f'{peer.name}: {compared} frames match lockstep in RAM, picture and state; {summary}')
    print(f'rollback: two players with up to {options.delay} frames of delay and jitter, {elapsed:.1f} s')

    for peer in (reference, first, second):
        peer.syncs.clear()
        peer.log.clear()
        peer.rollbacks.clear()
        peer.pictures = 0
        args = rollback_args(60) if peer is not reference else {'check': 60, 'paced': False}
        if peer.call('lockstep', args) != {'frame': frames}:
            raise c.Failed(f'{peer.name}: lockstep with a check every 60 frames did not keep the frame count')
    checked = [frame for frame in range(frames, frames + later) if frame % 60 == 0]
    for frame in range(frames, frames + later):
        reference.step(frame, script[frame])
    wait_for_sync([reference], checked[-1])
    play_phase(options, players, frames, later)
    wait_for_sync(peers, checked[-1])
    for peer in peers:
        if peer.log != checked:
            raise c.Failed(f'{peer.name}: syncs came for frames {peer.log}, not {checked}')
        ls.same(f'{peer.name} checked every 60 frames', [peer.syncs[f] for f in checked],
                [reference.syncs[f] for f in checked])
        summary = check_rollbacks(peer, peer.rollbacks, peer.pictures, reference.pictures)
        print(f'{peer.name}, checking every 60 frames: {summary}')
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
            right_guesses(match)
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
    parser.add_argument('--bios', help='OpenBIOS, 512 KiB: run the real SwanStation core on the pad program')
    parser.add_argument('--frames', type=int, default=600, help='Frames each player runs')
    parser.add_argument('--delay', type=int, default=6, help='The most frames a player\'s input arrives late')
    parser.add_argument('--tick', type=float, default=0.004,
                        help='Seconds each simulated frame lasts, for the hosts to run what they were sent')
    options = parser.parse_args()
    options.program = disc.pad_program() if options.bios else None
    return play(options)


if __name__ == '__main__':
    sys.exit(main())
