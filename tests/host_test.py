# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Host behaviour the fixtures do not cover: a stuck core, many start, volume and stop calls
in a row, and the internal resolutions a GPU game is offered."""

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

    def start(self, options=None):
        args = {'core': self.paths['core'], 'disc': self.paths['disc'], 'system': self.paths['system'],
                'saves': self.paths['saves']}
        if options is not None:
            args['options'] = options
        return self.call('start', args)['epoch']

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
    tests = {'stuck_core': stuck_core, 'unloadable': unloadable, 'restart_option': restart_option,
             'many_games': many_games,
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
