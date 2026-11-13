# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Two hosts on one machine in lockstep (protocol.md "Lockstep"), as two players' hosts are in an
online match: both boot the same game, are fed the same input frame by frame, and must report the
same RAM, picture and state hashes after every frame. Then one host saves its state, runs on,
loads it back and replays (a rollback), and the other host leaves a freely running game, loads
that state and catches up (a player joining a running game); both must again match frame for frame.

With the test core the inputs reach its RAM, so the test also checks that a different input shows
up as a different hash, and the protocol errors and refusals of lockstep. With --bios and
--program it runs the real SwanStation core on a disc made by make_test_disc.py."""

import argparse
import random
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402
import make_test_disc  # noqa: E402

# Every button but Select (bit 2): SwanStation's default analog-mode combo is L1 + R1 + Select, and
# whether that combo is still held lives outside its saved state (netplay.md "Determinism"), so a
# loaded state would toggle analog mode at a different frame than the run it was saved from.
BUTTONS = 0xFFFF & ~(1 << 2)


class Peer:
    """One host, driven as a launcher drives it: calls, steps, and the sync messages it sends."""

    def __init__(self, name, program, log):
        self.name = name
        self.host = (c.WindowsHost if c.WINDOWS else c.PosixHost)(program, log)
        self.next_id = 0
        self.syncs = {}
        self.log = []
        self.epoch = None
        self.host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['rgba8']})
        welcome = self.pump(lambda m: m.get('type') in ('welcome', 'unsupported'))
        if welcome['type'] != 'welcome' or 'ps1Lockstep' not in welcome['capabilities']:
            raise c.Failed(f'{name}: the host does not offer ps1Lockstep: {welcome}')
        self.call('volume', {'level': 0.0, 'muted': True})

    def pump(self, wanted=None, timeout=10.0):
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise c.Idle(f'{self.name}: the host sent nothing that was waited for')
            message, descriptors = self.host.read(left)
            c.close_all(descriptors)
            if message.get('type') == 'sync':
                self.syncs[message['frame']] = message
                self.log.append(message['frame'])
            if wanted is None or wanted(message):
                return message

    def drain(self):
        """Reads every message already sent, up to the first pause between messages."""
        try:
            while True:
                self.pump(timeout=0.01)
        except c.Idle:
            pass

    def call(self, method, args=None, timeout=6.0):
        self.next_id += 1
        ident = self.next_id
        message = {'type': 'call', 'id': ident, 'method': method}
        if args is not None:
            message['args'] = args
        self.host.send_message(message)
        reply = self.pump(lambda m: m.get('type') == 'reply' and m.get('id') == ident, timeout)
        if 'error' in reply:
            raise c.Failed(f'{self.name}: {method} failed: {reply["error"]}')
        return reply['result']

    def step(self, frame, ports):
        self.host.send_message({'type': 'step', 'frame': frame, 'ports': ports})

    def status(self, test, seconds, what):
        deadline = time.monotonic() + seconds
        status = None
        while time.monotonic() < deadline:
            status = self.call('status')
            if test(status):
                return status
            if status and status['ended']:
                raise c.Failed(f'{self.name}: the game ended: {status["errorCode"]!r} {status["error"]!r}')
            self.drain()
            time.sleep(0.1)
        raise c.Failed(f'{self.name}: {what} within {seconds} s; last status {status}')

    def close(self):
        self.host.close()


def port(buttons, left_x=0):
    return {'buttons': buttons, 'leftX': left_x, 'leftY': 0, 'rightX': 0, 'rightY': 0}


def script(frame):
    """The input both players' launchers agree on for a frame: two ports, changing every frame."""
    chance = random.Random(frame)
    return [port(chance.randrange(BUTTONS + 1) & BUTTONS, chance.randrange(-32768, 32768)),
            port(chance.randrange(BUTTONS + 1) & BUTTONS)]


def scripted(peer, frame):
    return script(frame)


def advance(peers, first, count, inputs=scripted, batch=120, timeout=120):
    """Steps every peer through frames first to first + count - 1 and waits for their syncs."""
    deadline = time.monotonic() + timeout
    for start in range(first, first + count, batch):
        end = min(start + batch, first + count)
        for frame in range(start, end):
            for peer in peers:
                peer.step(frame, inputs(peer, frame))
        for peer in peers:
            while end - 1 not in peer.syncs:
                if time.monotonic() > deadline:
                    raise c.Failed(f'{peer.name}: no sync for frame {end - 1}')
                peer.pump(lambda m: m.get('type') == 'sync', max(0.1, deadline - time.monotonic()))
            peer.drain()
    return {peer.name: [peer.syncs[f] for f in range(first, first + count)] for peer in peers}


def hashes(sync, state=True):
    return sync['ram'], sync['video'], sync['state'] if state else None


def same(name, ours, theirs, state=True):
    """Fails at the first frame whose hashes differ; returns how many frames were compared. A state
    hash is compared only between games of the same disc path: SwanStation's state names the
    disc's path (netplay.md "Determinism")."""
    if len(ours) != len(theirs):
        raise c.Failed(f'{name}: {len(ours)} frames against {len(theirs)}')
    for mine, other in zip(ours, theirs):
        if mine['frame'] != other['frame'] or hashes(mine, state) != hashes(other, state):
            raise c.Failed(f'{name}: frame {mine["frame"]} differs: {mine} against {other}')
        if mine['ram'] is None or mine['state'] is None:
            raise c.Failed(f'{name}: frame {mine["frame"]} has no RAM or state hash: {mine}')
    return len(ours)


class Match:
    def __init__(self, options):
        self.options = options
        self.work = Path(tempfile.mkdtemp(prefix='lockstep-'))
        self.log = open(options.log, 'ab') if options.log else c.subprocess.DEVNULL
        self.peers = []
        self.real = options.bios is not None
        if self.real:
            self.system = self.work / 'system'
            self.system.mkdir()
            shutil.copy(options.bios, self.system / 'openbios.bin')
            # Each player has their own copy of the disc, in a folder of its own, as on two machines.
            # SwanStation finds the disc a loaded state names by its path, or else by its file name.
            host_disc = self.work / 'host-disc'
            host_disc.mkdir()
            make_test_disc.build(options.program, host_disc / 'program.bin', host_disc / 'program.cue')
            shutil.copytree(host_disc, self.work / 'guest-disc')
            self.core = Path(options.core).resolve()
        else:
            self.paths = c.place(self.work, Path(options.core).resolve())

    def start_args(self, name, lockstep):
        if self.real:
            disc = self.work / ('host-disc' if name == 'host' else 'guest-disc') / 'program.cue'
            args = {'core': str(self.core), 'disc': str(disc), 'system': str(self.system),
                    'saves': str(self.work / f'saves-{name}'),
                    'options': {'swanstation_BIOS_PathNTSCU': 'openbios.bin',
                                'swanstation_BIOS_PathNTSCJ': 'openbios.bin',
                                'swanstation_BIOS_PathPAL': 'openbios.bin',
                                'swanstation_GPU_ResolutionScale': '1',
                                'swanstation_Controller_AnalogCombo': '3'}}
        else:
            args = {'core': self.paths['core'], 'disc': self.paths['disc'], 'system': self.paths['system'],
                    'saves': str(self.work / f'saves-{name}')}
        if lockstep:
            args['lockstep'] = {'check': 1, 'paced': False}
        return args

    def peer(self, name):
        peer = Peer(name, Path(self.options.host).resolve(), self.log)
        self.peers.append(peer)
        return peer

    def close(self):
        for peer in self.peers:
            peer.close()
        if self.log is not c.subprocess.DEVNULL:
            self.log.close()
        shutil.rmtree(self.work, ignore_errors=True)


def play(options):
    match = Match(options)
    frames, later = options.frames, options.frames // 2
    try:
        first, second = match.peer('host'), match.peer('guest')
        for peer in (first, second):
            peer.epoch = peer.call('start', match.start_args(peer.name, lockstep=True))['epoch']
        began = time.monotonic()
        boot = advance([first, second], 0, frames)
        compared = same('cold boot', boot['host'], boot['guest'], not match.real)
        print(f'cold boot: {compared} frames in lockstep match in RAM and picture'
              f'{"" if match.real else " and state"} '
              f'({time.monotonic() - began:.1f} s); last {boot["host"][-1]}')
        pictures = len({s['video'] for s in boot['host'] if s['video']})
        print(f'cold boot: {pictures} different pictures among them')
        if pictures < 2 or len({s['ram'] for s in boot['host']}) < 2:
            raise c.Failed('the RAM or the picture never changes: the game is not running')

        checkpoint = match.work / 'checkpoint.state'
        saved = first.call('saveState', {'path': str(checkpoint)})
        if not saved or saved['frame'] != frames or saved['bytes'] != checkpoint.stat().st_size:
            raise c.Failed(f'saveState gave {saved}, file {checkpoint.stat().st_size} bytes')
        ahead = advance([first], frames, later)['host']
        loaded = first.call('loadState', {'path': str(checkpoint)})
        if loaded != saved:
            raise c.Failed(f'loadState gave {loaded}, not {saved}')
        first.syncs.clear()
        replay = advance([first], frames, later)['host']
        print(f'rollback: the host replayed {same("rollback", replay, ahead)} frames from its saved state '
              f'({saved["bytes"]} bytes) and they match')

        second.call('stop')
        second.status(lambda s: s['ended'], 10, 'the game did not stop')
        second.call('release')
        second.syncs.clear()
        second.call('start', match.start_args(second.name, lockstep=False))
        second.status(lambda s: s['ready'], 60, 'the game was not ready')
        second.drain()
        time.sleep(1)
        if second.call('lockstep', {'check': 1, 'paced': False}) != {'frame': 0}:
            raise c.Failed('a running game did not enter lockstep at frame 0')
        if second.call('loadState', {'path': str(checkpoint)}) != saved:
            raise c.Failed('the guest could not load the host\'s state')
        joined = advance([second], frames, later)['guest']
        print(f'join: a running game loaded the host\'s state and matched it for {same("join", joined, ahead, not match.real)} frames')

        if not match.real:
            refusals(first, match)

            def different(peer, frame):
                ports = script(frame)
                if peer is second and frame == frames + later:
                    ports[1] = port(ports[1]['buttons'] ^ 1)
                return ports

            split = advance([first, second], frames + later, 1, different)
            if split['host'][0]['ram'] == split['guest'][0]['ram']:
                raise c.Failed('a different input on port 2 left the same RAM hash')
            print('a different input on port 2 shows up in the next sync')
            reentry(first)
            protocol_errors(match)

        for peer in (first, second):
            peer.host.send_message({'type': 'shutdown'})
            if peer.host.exits(15) != 0:
                raise c.Failed(f'{peer.name}: the host did not exit cleanly')
        print('lockstep passed')
        return 0
    except c.Failed as error:
        print('FAILED:', error)
        return 1
    finally:
        match.close()


def refusals(peer, match):
    """What a game in lockstep refuses, since it would change the console between agreed frames."""
    if peer.call('option', {'key': 'fake_color', 'value': 'green'}) is not False:
        raise c.Failed('option was taken in lockstep')
    if peer.call('connectedPorts', 2) is not False:
        raise c.Failed('connectedPorts was taken in lockstep')
    if peer.call('saveState', {'path': str(match.work / 'missing' / 'x.state')}) is not False:
        raise c.Failed('a state was saved where no directory is')
    if peer.call('loadState', {'path': str(match.work / 'checkpoint.missing')}) is not False:
        raise c.Failed('a missing state was loaded')
    bad = match.work / 'bad.state'
    bad.write_bytes(b'AP5STATE' + bytes(40))
    if peer.call('loadState', {'path': str(bad)}) is not False:
        raise c.Failed('a state file with the wrong size was loaded')
    print('lockstep refuses option, connectedPorts and states it cannot write or read')


def reentry(peer):
    """Leaving and entering lockstep while steps are being run: no frame of the old count may send
    its sync into the new one, which starts at 0."""
    start = peer.call('lockstep', {'check': 1, 'paced': False})['frame']
    for frame in range(start, start + 500):
        peer.step(frame, script(frame))
    if peer.call('lockstep', False) is not True:
        raise c.Failed('leaving lockstep did not reply true')
    if peer.call('lockstep', {'check': 1, 'paced': False}) != {'frame': 0}:
        raise c.Failed('entering lockstep again did not start at frame 0')
    peer.log.clear()
    peer.syncs.clear()
    peer.drain()
    if peer.log:
        raise c.Failed(f'syncs of the old count arrived after entering again: {peer.log[:5]}')
    advance([peer], 0, 10)
    if peer.log != list(range(10)):
        raise c.Failed(f'the new count sent syncs {peer.log}')
    print('leaving and entering lockstep mid-run keeps the old frames out of the new count')


def protocol_errors(match):
    """A step out of order, or one sent to a game not in lockstep, is a protocol error."""
    cases = [('a step for a frame that is not next', True, 5), ('a step to a game not in lockstep', False, 0)]
    for what, lockstep, frame in cases:
        peer = match.peer('errors')
        peer.call('start', match.start_args('errors', lockstep))
        if not lockstep:
            peer.status(lambda s: s['ready'], 30, 'the game was not ready')
        peer.step(frame, [port(0)])
        try:
            code = peer.host.exits(10)
        except c.Failed:
            raise c.Failed(f'the host did not end the connection after {what}')
        if code != 2:
            raise c.Failed(f'the host exited with {code} after {what}, not 2')
    print('steps out of order or outside lockstep are protocol errors')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--core', required=True)
    parser.add_argument('--log')
    parser.add_argument('--bios', help='OpenBIOS, 512 KiB: run the real SwanStation core')
    parser.add_argument('--program', help='The PS-EXE to put on the disc, with --bios')
    parser.add_argument('--frames', type=int, default=600, help='Frames to compare from a cold boot')
    options = parser.parse_args()
    if (options.bios is None) != (options.program is None):
        parser.error('--bios and --program go together')
    return play(options)


if __name__ == '__main__':
    sys.exit(main())
