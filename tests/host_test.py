# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Host behaviour the fixtures do not cover: a stuck core, and many start, volume and stop calls
in a row."""

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
        self.host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['rgba8']})
        welcome = self.read(lambda m: m.get('type') in ('welcome', 'unsupported'))
        if welcome['type'] != 'welcome':
            raise c.Failed('the host did not welcome the launcher')
        self.call('volume', {'level': 1, 'muted': False})

    def read(self, wanted, timeout=10):
        while True:
            message, descriptors = self.host.read(timeout)
            if message.get('type') == 'ring':
                self.host.ring(message, descriptors)
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

    def start(self):
        result = self.call('start', {'core': self.paths['core'], 'disc': self.paths['disc'],
                                     'system': self.paths['system'], 'saves': self.paths['saves']})
        return result['epoch']

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
    parser.add_argument('names', nargs='*')
    options = parser.parse_args()
    log = open(options.log, 'ab') if options.log else c.subprocess.DEVNULL
    c.load_atomics(options.core.resolve())
    failures = 0
    tests = [test for test in (stuck_core, unloadable, many_games) if not options.names or test.__name__ in options.names]
    for test in tests:
        try:
            test(options.host.resolve(), options.core.resolve(), log)
            print(f'ok    {test.__name__}')
        except c.Failed as problem:
            failures += 1
            print(f'FAIL  {test.__name__}: {problem}')
    print(f'{len(tests) - failures} of {len(tests)} host tests passed.')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
