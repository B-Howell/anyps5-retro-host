# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Two hosts on one machine in lockstep (protocol.md "Lockstep"), as two players' hosts are in an
online match: both boot the same game, are fed the same input frame by frame, and must report the
same RAM, picture and state hashes after every frame. Then one host saves its state, runs on,
loads it back and replays (a rollback), and the other host leaves a freely running game, loads
that state and catches up (a player joining a running game); both must again match frame for frame,
state hashes included, though each player's disc is in another folder under another name. Last, one
host saves its state in memory before each frame, runs it, loads the state and runs it again, and
both runs must match: what rollback does on every wrong guess, timed.

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

# SwanStation's analog-mode combo as the test sets it (swanstation_Controller_AnalogCombo 3): L1 + R1 +
# Select.
COMBO = (1 << 10) | (1 << 11) | (1 << 2)


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

    def call_raw(self, method, args=None, timeout=6.0):
        self.next_id += 1
        ident = self.next_id
        message = {'type': 'call', 'id': ident, 'method': method}
        if args is not None:
            message['args'] = args
        self.host.send_message(message)
        return self.pump(lambda m: m.get('type') == 'reply' and m.get('id') == ident, timeout)

    def call(self, method, args=None, timeout=6.0):
        reply = self.call_raw(method, args, timeout)
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
    """The input both players' launchers agree on for a frame: two ports, every button, changing every
    frame."""
    chance = random.Random(frame)
    return [port(chance.randrange(65536), chance.randrange(-32768, 32768)), port(chance.randrange(65536))]


def scripted_around(save, end):
    """The script, except that both ports hold the analog-mode combo from three frames before save to two
    after it, and let go of every button for the three frames before end. A host that runs to end, then
    loads the state saved at save, starts its replay with the combo held since before the state, as the
    run it was saved from did, and with the analog mode of that run. A core that keeps either outside its
    state toggles analog mode at another frame (netplay.md "Determinism")."""
    def inputs(peer, frame):
        if save - 3 <= frame < save + 3:
            return [port(COMBO), port(COMBO)]
        if end - 3 <= frame < end:
            return [port(0), port(0)]
        return script(frame)
    return inputs


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
        # Read every host in turn: one left unread blocks on a full socket and, after protocol.md's
        # 5-second write limit, exits.
        waiting = list(peers)
        while waiting:
            for peer in list(waiting):
                if end - 1 in peer.syncs:
                    waiting.remove(peer)
                    continue
                if time.monotonic() > deadline:
                    raise c.Failed(f'{peer.name}: no sync for frame {end - 1}')
                try:
                    peer.pump(lambda m: m.get('type') == 'sync', 0.05)
                except c.Idle:
                    pass
    return {peer.name: [peer.syncs[f] for f in range(first, first + count)] for peer in peers}


def hashes(sync):
    return sync['ram'], sync['video'], sync['state']


def same(name, ours, theirs):
    """Fails at the first frame whose hashes differ; returns how many frames were compared. The state
    hashes match too, though each player's disc is in another folder under another name: a state for
    netplay names no path (netplay.md "Determinism")."""
    if len(ours) != len(theirs):
        raise c.Failed(f'{name}: {len(ours)} frames against {len(theirs)}')
    for mine, other in zip(ours, theirs):
        if mine['frame'] != other['frame'] or hashes(mine) != hashes(other):
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
            # Each player has their own copy of the disc, in a folder of its own and under a name of
            # its own, as on two machines. A state for netplay must name neither.
            self.discs = {'host': self.work / 'host-disc' / 'program.cue',
                          'guest': self.work / 'guest-disc' / 'my copy.cue'}
            for disc in self.discs.values():
                disc.parent.mkdir()
                make_test_disc.build(options.program, disc.with_suffix('.bin'), disc)
            self.core = Path(options.core).resolve()
        else:
            self.paths = c.place(self.work, Path(options.core).resolve())

    def start_args(self, name, lockstep):
        if self.real:
            disc = self.discs['host' if name == 'host' else 'guest']
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
    inputs = scripted_around(frames, frames + later)
    try:
        first, second = match.peer('host'), match.peer('guest')
        for peer in (first, second):
            peer.epoch = peer.call('start', match.start_args(peer.name, lockstep=True))['epoch']
        began = time.monotonic()
        boot = advance([first, second], 0, frames, inputs)
        compared = same('cold boot', boot['host'], boot['guest'])
        print(f'cold boot: {compared} frames in lockstep match in RAM, picture and state '
              f'({time.monotonic() - began:.1f} s); last {boot["host"][-1]}')
        pictures = len({s['video'] for s in boot['host'] if s['video']})
        print(f'cold boot: {pictures} different pictures among them')
        if pictures < 2 or len({s['ram'] for s in boot['host']}) < 2:
            raise c.Failed('the RAM or the picture never changes: the game is not running')

        checkpoint = match.work / 'checkpoint.state'
        saved = first.call('saveState', {'path': str(checkpoint)})
        if not saved or saved['frame'] != frames or saved['bytes'] != checkpoint.stat().st_size:
            raise c.Failed(f'saveState gave {saved}, file {checkpoint.stat().st_size} bytes')
        if match.real:
            no_paths(checkpoint, match)
        ahead = advance([first], frames, later, inputs)['host']
        loaded = first.call('loadState', {'path': str(checkpoint)})
        if (loaded['frame'], loaded['bytes']) != (saved['frame'], saved['bytes']):
            raise c.Failed(f'loadState gave {loaded}, not {saved}')
        first.syncs.clear()
        replay = advance([first], frames, later, inputs)['host']
        print(f'rollback: the host replayed {same("rollback", replay, ahead)} frames from its saved state '
              f'({saved["bytes"]} bytes), the analog-mode combo held across it, and they match')

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
        joined_at = second.call('loadState', {'path': str(checkpoint)})
        if not joined_at or joined_at['frame'] != saved['frame']:
            raise c.Failed(f'the guest could not load the host\'s state from a disc of another name: {joined_at}')
        joined = advance([second], frames, later, inputs)['guest']
        print(f'join: a running game loaded the host\'s state and matched it for {same("join", joined, ahead)} '
              'frames, state hashes included')

        if not match.real:
            def different(peer, frame):
                ports = script(frame)
                if peer is second and frame == frames + later:
                    ports[1] = port(ports[1]['buttons'] ^ 1)
                return ports

            split = advance([first, second], frames + later, 1, different)
            if split['host'][0]['ram'] == split['guest'][0]['ram']:
                raise c.Failed('a different input on port 2 left the same RAM hash')
            print('a different input on port 2 shows up in the next sync')

        after = frames + later + (0 if match.real else 1)
        cost = memory_states(first, after, options.memory_frames)
        if match.real and options.budget_ms is not None and cost > options.budget_ms * 1000:
            raise c.Failed(f'saving and loading a state in memory took {cost / 1000:.2f} ms on average, '
                           f'over the {options.budget_ms} ms budget')

        if not match.real:
            refusals(first, match)
            reentry(first, match)
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


def no_paths(checkpoint, match):
    """A state sent to a friend names no folder or file of the player who saved it."""
    data = checkpoint.read_bytes()
    disc = match.discs['host']
    for secret in (str(match.work), str(disc), disc.name, disc.stem, disc.parent.name):
        for encoding in ('utf-8', 'utf-16-le'):
            if secret.encode(encoding) in data:
                raise c.Failed(f'the saved state holds {secret!r} ({encoding})')
    print('the saved state names no folder or file of the player who saved it')


def memory_states(peer, first, count):
    """A sync test, as GGPO's: before each of count frames the host saves its state in memory, runs the
    frame, loads the state back and runs the frame again, and both runs must leave the same hashes. It
    measures what a rollback costs each frame: one save, and on a wrong guess one load (protocol.md
    "saveState and loadState"). Returns the average microseconds of a save and a load together."""
    saves, loads = [], []
    for frame in range(first, first + count):
        saved = peer.call('saveState', {'slot': frame % 8})
        if not saved or saved['frame'] != frame:
            raise c.Failed(f'saveState in slot {frame % 8} gave {saved} before frame {frame}')
        peer.syncs.clear()
        once = advance([peer], frame, 1)[peer.name]
        loaded = peer.call('loadState', {'slot': frame % 8})
        if not loaded or loaded['frame'] != frame or loaded['bytes'] != saved['bytes']:
            raise c.Failed(f'loadState from slot {frame % 8} gave {loaded}, not {saved}')
        peer.syncs.clear()
        again = advance([peer], frame, 1)[peer.name]
        same(f'memory state before frame {frame}', again, once)
        saves.append(saved['micros'])
        loads.append(loaded['micros'])
    average = (sum(saves) + sum(loads)) / count
    print(f'memory states: {count} frames each saved, run, loaded and run again match; '
          f'save {sum(saves) / count / 1000:.2f} ms on average (most {max(saves) / 1000:.2f}), '
          f'load {sum(loads) / count / 1000:.2f} ms (most {max(loads) / 1000:.2f})')
    return average


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
    for args in ({'slot': 8}, {'slot': -1}, {'slot': 0, 'path': str(bad)}, {}):
        reply = peer.call_raw('saveState', args)
        if 'error' not in reply:
            raise c.Failed(f'saveState {args} was not an argument error: {reply}')
    print('lockstep refuses option, connectedPorts, states it cannot write or read, and slots out of range')


def reentry(peer, match):
    """Leaving and entering lockstep while a frame is being run: the host answers only once that
    frame has ended, so its sync comes before the reply, nothing of the old count follows, and the
    console does not move again until the new count's first step."""
    system = Path(match.paths['system'])
    hold, held = system / 'fake-hold-run', system / 'fake-held-run'
    start = peer.call('lockstep', {'check': 1, 'paced': False})['frame']
    hold.write_bytes(b'')
    held.unlink(missing_ok=True)
    try:
        for frame in range(start, start + 50):
            peer.step(frame, script(frame))
        deadline = time.monotonic() + 10
        while not held.exists():
            if time.monotonic() > deadline:
                raise c.Failed('the fake core never held inside a frame')
            time.sleep(0.01)
        peer.log.clear()
        peer.next_id += 1
        leave = peer.next_id
        peer.host.send_message({'type': 'call', 'id': leave, 'method': 'lockstep', 'args': False})
        try:
            early = peer.pump(lambda m: m.get('type') == 'reply' and m.get('id') == leave, 1.0)
            raise c.Failed(f'lockstep false was answered while a frame was being run: {early}')
        except c.Idle:
            pass
    finally:
        hold.unlink(missing_ok=True)
    reply = peer.pump(lambda m: m.get('type') == 'reply' and m.get('id') == leave, 10)
    if reply.get('result') is not True:
        raise c.Failed(f'leaving lockstep gave {reply}')
    if peer.log != [start]:
        raise c.Failed(f'the frame being run should send its sync before the reply; got {peer.log}')
    peer.log.clear()
    if peer.call('lockstep', {'check': 1, 'paced': False}) != {'frame': 0}:
        raise c.Failed('entering lockstep again did not start at frame 0')
    if peer.call('loadState', {'slot': 0}) is not False:
        raise c.Failed('a state saved in memory before lockstep was entered again was loaded')
    before = match.work / 'reentry-1.state'
    after = match.work / 'reentry-2.state'
    peer.call('saveState', {'path': str(before)})
    time.sleep(0.3)
    peer.call('saveState', {'path': str(after)})
    peer.drain()
    if peer.log:
        raise c.Failed(f'syncs arrived after leaving lockstep: {peer.log}')
    if before.read_bytes() != after.read_bytes():
        raise c.Failed('the console ran a frame in lockstep without a step')
    peer.syncs.clear()
    advance([peer], 0, 10)
    if peer.log != list(range(10)):
        raise c.Failed(f'the new count sent syncs {peer.log}')
    print('leaving lockstep waits for the frame being run; nothing of the old count follows')


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
    parser.add_argument('--memory-frames', type=int, default=120,
                        help='Frames each saved in memory, run, loaded and run again')
    parser.add_argument('--budget-ms', type=float,
                        help='With the real core, the most a save and a load in memory may take together, '
                             'on average')
    options = parser.parse_args()
    if (options.bios is None) != (options.program is None):
        parser.error('--bios and --program go together')
    return play(options)


if __name__ == '__main__':
    sys.exit(main())
