# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Host behaviour the fixtures do not cover: a stuck core, many start, volume and stop calls
in a row, the internal resolutions a GPU game is offered, the frame rate a game is paced to, and a
PS2 game (capability ps2, docs/ps2.md) as the host runs LRPS2."""

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402


class Session:
    def __init__(self, program, core, log):
        self.work = Path(tempfile.mkdtemp(prefix='retro-host-test-'))
        self.paths = c.place(self.work, core)
        self.host = c.WindowsHost(program, log) if c.WINDOWS else c.PosixHost(program, log)
        self.next_id = 0
        self.ring = None
        self.host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['rgba8']})
        welcome = self.read(lambda m: m.get('type') in ('welcome', 'unsupported'))
        if welcome['type'] != 'welcome':
            raise c.Failed('the host did not welcome the launcher')
        self.call('volume', {'level': 1, 'muted': False})

    def read(self, wanted, timeout=10):
        while True:
            message, descriptors = self.host.read(timeout)
            if message.get('type') == 'ring':
                self.ring = self.host.ring(message, descriptors)
            if wanted(message):
                return message

    def send(self, method, args=None):
        self.next_id += 1
        message = {'type': 'call', 'id': self.next_id, 'method': method}
        if args is not None:
            message['args'] = args
        self.host.send_message(message)
        return self.next_id

    def call(self, method, args=None):
        ident = self.send(method, args)
        reply = self.read(lambda m: m.get('type') == 'reply' and m.get('id') == ident, 6)
        if 'error' in reply:
            raise c.Failed(f'{method} failed: {reply["error"]}')
        return reply['result']

    def error(self, method, args=None):
        """The error code of a call that must fail."""
        ident = self.send(method, args)
        reply = self.read(lambda m: m.get('type') == 'reply' and m.get('id') == ident, 6)
        if 'error' not in reply:
            raise c.Failed(f'{method} {args} did not fail: {reply}')
        return reply['error']['code']

    def start(self, options=None, lockstep=None):
        args = {'core': self.paths['core'], 'disc': self.paths['disc'], 'system': self.paths['system'],
                'saves': self.paths['saves']}
        if options is not None:
            args['options'] = options
        if lockstep is not None:
            args['lockstep'] = lockstep
        return self.call('start', args)['epoch']

    def frame_rate(self, seconds):
        """The frames a second the game published over seconds, counted from the host's frame messages."""
        self.read(lambda m: m.get('type') == 'frame')
        began = time.monotonic()
        frames = 0
        while time.monotonic() < began + seconds:
            try:
                message = self.read(lambda m: True, max(0.01, began + seconds - time.monotonic()))
            except c.Idle:
                continue
            if message.get('type') == 'frame':
                frames += 1
        return frames / (time.monotonic() - began)

    def wait(self, test, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            status = self.call('status')
            if test(status):
                return status
            time.sleep(0.2)
        raise c.Failed(f'the status never matched; last {status}')

    def close(self):
        self.host.close()
        shutil.rmtree(self.work, ignore_errors=True)


def stuck_core(program, core, log):
    session = Session(program, core, log)
    try:
        session.start()
        session.wait(lambda s: s and s['ready'], 10)
        if session.call('option', {'key': 'fake_hang', 'value': 'yes'}) is not True:
            raise c.Failed('the hang option was not taken')
        status = session.wait(lambda s: s['ended'], 15)
        if status['errorCode'] != 'stalled' or 'aspect' not in status:
            raise c.Failed(f'a stuck core gave {status}')
        if session.call('release') is not None:
            raise c.Failed('release did not reply null')
        code = session.host.exits(5)
        if code == 0:
            raise c.Failed('the host exited with 0 after a stuck core')
    finally:
        session.close()


def unloadable(program, core, log):
    session = Session(program, core, log)
    try:
        cases = [('core', dict(session.paths, core=session.paths['disc'])),
                 ('disc', dict(session.paths, disc=str(Path(session.paths['disc']).with_name('missing.cue'))))]
        for code, paths in cases:
            epoch = session.call('start', {'core': paths['core'], 'disc': paths['disc'], 'system': paths['system'],
                                           'saves': paths['saves']})['epoch']
            status = session.wait(lambda s: s and s['ended'], 5)
            if status['errorCode'] != code or status['ready'] or 'aspect' not in status:
                raise c.Failed(f'game {epoch} ended with {status}, not errorCode {code}')
            session.call('release')
    finally:
        session.close()


def core_identity(program, core, log):
    """status.core gives the core's library_name and library_version as printable ASCII of at most
    256 bytes, "" for anything else, and null for a core that did not load."""
    cases = [(None, '1'), ('anyps5-1-' + 'a' * 64, 'anyps5-1-' + 'a' * 64), ('x' * 256, 'x' * 256),
             ('x' * 257, ''), ('caf\u00e9', ''), ('tab\there', '')]
    saved = c.os.environ.get('FAKE_CORE_VERSION')
    try:
        for given, shown in cases:
            if given is None:
                c.os.environ.pop('FAKE_CORE_VERSION', None)
            else:
                c.os.environ['FAKE_CORE_VERSION'] = given
            session = Session(program, core, log)
            try:
                session.start()
                status = session.wait(lambda s: s and s['ready'], 10)
                if status.get('core') != {'name': 'fake', 'version': shown}:
                    raise c.Failed(f'a core reporting version {given!r} gave core {status.get("core")!r}')
                session.call('stop')
                status = session.wait(lambda s: s['ended'], 5)
                if status.get('core') != {'name': 'fake', 'version': shown}:
                    raise c.Failed(f'an ended game gave core {status.get("core")!r}')
                session.call('release')
                session.call('start', {'core': session.paths['disc'], 'disc': session.paths['disc'],
                                       'system': session.paths['system'], 'saves': session.paths['saves']})
                status = session.wait(lambda s: s and s['ended'], 5)
                if status['errorCode'] != 'core' or status.get('core', 'missing') is not None:
                    raise c.Failed(f'a core that did not load gave {status}')
            finally:
                session.close()
    finally:
        if saved is None:
            c.os.environ.pop('FAKE_CORE_VERSION', None)
        else:
            c.os.environ['FAKE_CORE_VERSION'] = saved


def game_timing(program, core, log):
    """status.timing gives the frame rate the host paces the game to and the core's region: the fake
    core's NTSC game at 60, its PAL game at 50, and a game that switches to 50 through
    SET_SYSTEM_AV_INFO while it runs. A paced game, on its own or in lockstep, publishes no more frames
    a second than that; timing is known before a game started in lockstep runs its first step, follows
    a loaded state's rate, and is null for a core that did not load."""
    pal = {'fps': 50.0, 'region': 'pal'}
    session = Session(program, core, log)
    try:
        for options, timing in ((None, {'fps': 60.0, 'region': 'ntsc'}), ({'fake_region': 'pal'}, pal)):
            session.start(options)
            status = session.wait(lambda s: s and s['ready'], 10)
            if status.get('timing') != timing:
                raise c.Failed(f'a game started with {options} gave timing {status.get("timing")!r}, not {timing}')
            rate = session.frame_rate(3)
            print(f'      {timing["region"]} game: {rate:.1f} frames a second')
            if not 0.5 * timing['fps'] < rate < 1.05 * timing['fps']:
                raise c.Failed(f'a {timing["fps"]:g} Hz game ran at {rate:.1f} frames a second')
            session.call('stop')
            status = session.wait(lambda s: s['ended'], 5)
            if status.get('timing') != timing:
                raise c.Failed(f'an ended game gave timing {status.get("timing")!r}')
            session.call('release')

        session.start()
        session.wait(lambda s: s and s['ready'], 10)
        (Path(session.paths['system']) / 'fake-refresh-50').write_bytes(b'')
        status = session.wait(lambda s: (s.get('timing') or {}).get('fps') == 50.0, 5)
        if status['timing'] != {'fps': 50.0, 'region': 'ntsc'}:
            raise c.Failed(f'a game that switched to 50 Hz gave timing {status["timing"]!r}')
        rate = session.frame_rate(3)
        if not rate < 1.05 * 50:
            raise c.Failed(f'a game that switched to 50 Hz ran at {rate:.1f} frames a second')
        (Path(session.paths['system']) / 'fake-refresh-50').unlink()
        session.call('stop')
        session.wait(lambda s: s['ended'], 5)
        session.call('release')

        session.start({'fake_region': 'pal'}, {'check': 1})
        status = session.wait(lambda s: s and s.get('timing') is not None, 10)
        if status['timing'] != pal or status['ready']:
            raise c.Failed(f'a PAL game waiting for its first step gave {status}')
        steps = 100
        began = time.monotonic()
        for frame in range(steps):
            session.host.send_message({'type': 'step', 'frame': frame, 'ports': [
                {'buttons': 0, 'leftX': 0, 'leftY': 0, 'rightX': 0, 'rightY': 0}]})
        session.read(lambda m: m.get('type') == 'sync' and m['frame'] == steps - 1, 30)
        took = time.monotonic() - began
        print(f'      {steps} paced lockstep steps of a PAL game: {took:.2f} s')
        if took < 0.95 * (steps - 1) / 50:
            raise c.Failed(f'{steps} paced lockstep steps of a 50 Hz game took {took:.2f} s')
        session.call('stop')
        session.wait(lambda s: s['ended'], 5)
        session.call('release')

        # A loaded state brings back the rate it was saved at, as SwanStation's video timings come
        # back with its state: the host takes it after the next frame.
        def run(first, count):
            for frame in range(first, first + count):
                session.host.send_message({'type': 'step', 'frame': frame, 'ports': [
                    {'buttons': 0, 'leftX': 0, 'leftY': 0, 'rightX': 0, 'rightY': 0}]})
            session.read(lambda m: m.get('type') == 'sync' and m['frame'] == first + count - 1, 30)

        refresh = Path(session.paths['system']) / 'fake-refresh-50'
        session.start(None, {'check': 1, 'paced': False})
        run(0, 10)
        if session.call('saveState', {'slot': 0}) is False:
            raise c.Failed('saveState in a slot was refused')
        refresh.write_bytes(b'')
        run(10, 10)
        refresh.unlink()
        status = session.call('status')
        if status['timing'] != {'fps': 50.0, 'region': 'ntsc'}:
            raise c.Failed(f'a lockstep game that switched to 50 Hz gave timing {status["timing"]!r}')
        if session.call('loadState', {'slot': 0}) is False:
            raise c.Failed('loadState from a slot was refused')
        run(10, 3)
        status = session.call('status')
        if status['timing'] != {'fps': 60.0, 'region': 'ntsc'}:
            raise c.Failed(f'a state saved at 60 Hz, loaded, gave timing {status["timing"]!r}')
        session.call('stop')
        session.wait(lambda s: s['ended'], 5)
        session.call('release')

        session.call('start', {'core': session.paths['disc'], 'disc': session.paths['disc'],
                               'system': session.paths['system'], 'saves': session.paths['saves']})
        status = session.wait(lambda s: s and s['ended'], 5)
        if status['errorCode'] != 'core' or status.get('timing', 'missing') is not None:
            raise c.Failed(f'a core that did not load gave {status}')
    finally:
        session.close()


def restart_option(program, core, log):
    session = Session(program, core, log)
    try:
        session.start()
        status = session.wait(lambda s: s and s['ready'], 10)
        region = status['options'].get('fake_region')
        if not region or region['restart'] is not True or status['options']['fake_color']['restart'] is not False:
            raise c.Failed(f'restart flags are wrong: {status["options"]}')
        if session.call('option', {'key': 'fake_region', 'value': 'pal'}) is not True:
            raise c.Failed('the restart option was not taken')
        time.sleep(0.5)
        status = session.call('status')
        region = status['options']['fake_region']
        if region['values'][region['selected']] != 'pal' or region['activeValue'] != 'ntsc' or status['graphicsPending']:
            raise c.Failed(f'a restart option was applied at once: {region}, pending {status["graphicsPending"]}')
        if session.call('option', {'key': 'fake_color', 'value': 'green'}) is not True:
            raise c.Failed('the color option was not taken')
        status = session.wait(lambda s: s['options']['fake_color']['activeValue'] == 'green', 3)
        if status['graphicsPending']:
            raise c.Failed('graphicsPending stayed true after the core read the value')
    finally:
        session.close()


def gpu_limits(program, core, log, require_gpu):
    """The fake core's largest geometry is 1024 x 512 times the scale and a 640-wide screen's frames
    640 x 480 times it, as SwanStation's are. A saved 16x would make frames 10240 pixels wide, more
    than protocol.md's 8192, so the host offers only the scales whose largest geometry fits, starts
    at the largest of them and says why in resolutionError."""
    session = Session(program, core, log)
    key = 'swanstation_GPU_ResolutionScale'
    try:
        (Path(session.paths['system']) / 'fake-gpu').write_bytes(b'')
        session.start({key: '16'})
        status = session.wait(lambda s: s and (s['ready'] or s['ended']), 30)
        if status['ended']:
            raise c.Failed(f'a saved 16x ended the game: {status["errorCode"]!r} {status["error"]!r}')
        if not status['hardware']:
            if require_gpu:
                raise c.Failed('the host made no OpenGL context')
            return 'skipped: the host made no OpenGL context here'
        option = status['options'][key]
        largest = max(int(value) for value in option['values'])
        if any(1024 * int(value) > 8192 for value in option['values']) or largest < 1:
            raise c.Failed(f'scales whose frames can pass 8192 are offered: {option["values"]}')
        if option['activeValue'] != str(largest) or option['values'][option['selected']] != str(largest):
            raise c.Failed(f'a saved 16x did not start at the largest scale offered: {option}')
        if '16x' not in status['resolutionError']:
            raise c.Failed(f'resolutionError does not say why 16x is not used: {status["resolutionError"]!r}')

        def frame_width(width):
            def test(s):
                slot = session.ring.latest() if session.ring else None
                return s['ended'] or (slot is not None and slot['width'] == width)
            return test

        status = session.wait(frame_width(640 * largest), 30)
        if status['ended']:
            raise c.Failed(f'the game ended at {largest}x: {status["errorCode"]!r} {status["error"]!r}')
        if session.call('option', {'key': key, 'value': '16'}) is not False:
            raise c.Failed('a scale that is not offered was taken')
        if session.call('option', {'key': key, 'value': '2'}) is not True:
            raise c.Failed('2x was not taken')
        status = session.wait(frame_width(1280), 30)
        if status['ended'] or status['resolutionError'] or status['options'][key]['activeValue'] != '2':
            raise c.Failed(f'after choosing 2x: {status}')
        session.call('stop')
        status = session.wait(lambda s: s['ended'], 10)
        if status['errorCode'] or status['resolutionError']:
            raise c.Failed(f'the game stopped with {status}')
        session.call('release')

        # 2x chosen after the core read 16x but before the host saw its geometry: by the time the
        # core's context is reset, the host gives it 2x, offers no 16x, and refused nothing.
        system = Path(session.paths['system'])
        for step in ('load', 'reset'):
            (system / f'fake-hold-{step}').write_bytes(b'')

        def held(step):
            deadline = time.monotonic() + 10
            while not (system / f'fake-held-{step}').exists():
                if time.monotonic() > deadline:
                    raise c.Failed(f'the fake core never held at {step}')
                time.sleep(0.05)

        session.start({key: '16'})
        held('load')
        if session.call('option', {'key': key, 'value': '2'}) is not True:
            raise c.Failed('2x was not taken while the game loaded')
        (system / 'fake-hold-load').unlink()
        held('reset')
        status = session.call('status')
        option = status['options'][key]
        if status['resolutionError'] or option['activeValue'] != '2' or '16' in option['values']:
            raise c.Failed(f'2x chosen while loading at 16x, at context_reset: {status}')
        (system / 'fake-hold-reset').unlink()
        status = session.wait(frame_width(1280), 30)
        if status['ended'] or status['resolutionError'] or status['options'][key]['activeValue'] != '2':
            raise c.Failed(f'2x chosen while loading at 16x: {status}')
    finally:
        session.close()


def many_games(program, core, log):
    session = Session(program, core, log)
    try:
        for game in range(1, 9):
            if session.start() != game:
                raise c.Failed('epochs do not count up')
            for level in range(10):
                session.send('volume', {'level': level / 10, 'muted': level % 2 == 0})
            session.call('pause', True)
            session.call('stop')
            for level in range(10):
                session.send('volume', {'level': level / 10, 'muted': False})
            session.wait(lambda s: s['ended'], 5)
            session.call('release')
            if session.call('status') is not None:
                raise c.Failed('status is not null after release')
        session.host.send_message({'type': 'shutdown'})
        if session.host.exits(10) != 0:
            raise c.Failed('the host did not exit cleanly')
    finally:
        session.close()


def ps2_game(program, core, log):
    """A PS2 game (protocol.md "start", console): the host takes only PS2 discs for it, reports the
    console, a missing BIOS as errorCode bios the way LRPS2 says it found none, and any other failed
    load as core; it gives the core LRPS2's DualShock 2 and its own memory card, renderer and
    multitap values whatever the launcher asks, keeps them out of status, and keeps a PS2 game out
    of lockstep and states."""
    session = Session(program, core, log)
    try:
        work = session.work
        system = Path(session.paths['system'])
        (system / 'fake-ps2').write_bytes(b'')
        iso = work / 'discs' / 'game.iso'
        iso.write_bytes(b'\0' * 2048)
        ps2 = {'console': 'ps2', 'core': session.paths['core'], 'disc': str(iso), 'system': str(system),
               'saves': session.paths['saves']}
        asked = {'pcsx2_shared_memory_cards': 'enabled', 'pcsx2_renderer': 'Vulkan', 'pcsx2_multitap': 'port 1'}

        refused = [dict(ps2, console='ps3'), dict(ps2, console=2), dict(ps2, disc=str(work / 'discs' / 'game.pbp')),
                   dict(ps2, disc=session.paths['playlist']), dict(ps2, lockstep={'check': 1}),
                   dict(ps2, disc=session.paths['playlist'], initialDisc=0), dict(ps2, console='ps1', disc=str(iso))]
        for args in refused:
            if session.error('start', args) != 'argument':
                raise c.Failed(f'start {args} was not an argument error')

        def ended(args):
            session.call('start', args)
            status = session.wait(lambda s: s and s['ended'], 10)
            session.call('release')
            return status

        status = ended(dict(ps2, options=asked))
        if (status['errorCode'] != 'bios' or status['error'] != 'No PS2 BIOS was found.' or status['ready'] or
                status.get('console') != 'ps2' or status.get('core') != {'name': 'fake', 'version': '1'} or
                status.get('timing', 'missing') is not None):
            raise c.Failed(f'a PS2 game with no BIOS gave {status}')
        (system / 'fake-ps2-fail').write_bytes(b'')
        status = ended(ps2)
        if status['errorCode'] != 'core' or status.get('console') != 'ps2':
            raise c.Failed(f'a PS2 game the core could not load gave {status}')
        (system / 'fake-ps2-fail').unlink()

        (system / 'pcsx2' / 'bios').mkdir(parents=True)
        (system / 'pcsx2' / 'bios' / 'fake-ps2-bios.bin').write_bytes(b'')
        ports = system / 'fake-ps2-ports.txt'
        if ports.exists():
            ports.unlink()
        session.call('start', dict(ps2, options=asked))
        status = session.wait(lambda s: s and s['ready'], 10)
        if status.get('console') != 'ps2' or set(asked) & set(status['options']):
            raise c.Failed(f'a running PS2 game gave {status}')
        given = (system / 'fake-ps2-options.txt').read_text(encoding='utf-8').split()
        if given != ['pcsx2_shared_memory_cards=disabled', 'pcsx2_renderer=Auto', 'pcsx2_multitap=disabled']:
            raise c.Failed(f'the core was given {given}')
        if session.call('connectedPorts', 2) is not True:
            raise c.Failed('a PS2 game did not take a second controller')
        session.wait(lambda s: ports.exists() and '1 1' in ports.read_text(encoding='utf-8').splitlines(), 5)
        devices = ports.read_text(encoding='utf-8').splitlines()
        if devices[0] != '0 1' or '1 0' not in devices:
            raise c.Failed(f'the PS2 controller ports were set to {devices}, not the DualShock 2 (1)')
        if session.call('lockstep', {'check': 1}) is not False or session.call('lockstep', False) is not False:
            raise c.Failed('a PS2 game went into lockstep')
        if session.call('saveState', {'slot': 0}) is not False:
            raise c.Failed('a PS2 game saved a lockstep state')
        session.call('stop')
        session.wait(lambda s: s['ended'], 5)
        session.call('release')

        ports.unlink()
        session.call('start', {'console': 'ps1', 'core': session.paths['core'], 'disc': session.paths['disc'],
                               'system': str(system), 'saves': session.paths['saves']})
        status = session.wait(lambda s: s and s['ready'], 10)
        if status.get('console') != 'ps1' or ports.read_text(encoding='utf-8').splitlines()[0] != '0 261':
            raise c.Failed(f'a PS1 game gave {status} and ports {ports.read_text(encoding="utf-8")!r}')
        session.call('stop')
        session.wait(lambda s: s['ended'], 5)
        session.call('release')
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=Path)
    parser.add_argument('--core', required=True, type=Path)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--require-gpu', action='store_true', help='fail, not skip, when the host makes no OpenGL context')
    parser.add_argument('names', nargs='*')
    options = parser.parse_args()
    log = open(options.log, 'ab') if options.log else c.subprocess.DEVNULL
    c.load_atomics(options.core.resolve())
    failures = 0
    tests = {'stuck_core': stuck_core, 'unloadable': unloadable, 'core_identity': core_identity,
             'game_timing': game_timing, 'restart_option': restart_option,
             'many_games': many_games, 'ps2_game': ps2_game,
             'gpu_limits': lambda program, core, log: gpu_limits(program, core, log, options.require_gpu)}
    chosen = [(name, test) for name, test in tests.items() if not options.names or name in options.names]
    for name, test in chosen:
        try:
            skipped = test(options.host.resolve(), options.core.resolve(), log)
            print(f'ok    {name}' + (f' ({skipped})' if skipped else ''))
        except c.Failed as problem:
            failures += 1
            print(f'FAIL  {name}: {problem}')
    print(f'{len(chosen) - failures} of {len(chosen)} host tests passed.')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
