# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Tests tests/fake_host.py: starts it as protocol.md "Starting the host" says a launcher does, plays
the launcher side of every session that tests a launcher, and checks what the fake sent and reported."""

import argparse
import array
import ctypes
import json
import mmap
import os
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SESSIONS = ROOT / 'fixtures' / 'v1' / 'sessions'
FAKE = ROOT / 'tests' / 'fake_host.py'
WINDOWS = os.name == 'nt'
SYSTEM = 'windows' if WINDOWS else ('macos' if sys.platform == 'darwin' else 'linux')
NONE = 0xFFFFFFFF
INPUT = {'type': 'input', 'port': 0, 'buttons': 0, 'leftX': 0, 'leftY': 0, 'rightX': 0, 'rightY': 0}


class Failed(Exception):
    pass


def runs_here(session):
    wanted = session.get('os')
    return wanted is None or wanted == SYSTEM or (wanted == 'posix' and not WINDOWS)


def fill(value, paths):
    if isinstance(value, str) and value.startswith('${') and value.endswith('}'):
        return paths[value[2:-1]]
    if isinstance(value, list):
        return [fill(item, paths) for item in value]
    if isinstance(value, dict):
        return {key: fill(item, paths) for key, item in value.items()}
    return value


def compare(expected, actual, loose, where=''):
    if where in loose:
        if type(expected) is not type(actual) and not (isinstance(expected, (int, float)) and
                                                       isinstance(actual, (int, float))):
            raise Failed(f'{where}: {actual!r} is not the type of {expected!r}')
        return
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            raise Failed(f'{where}: expected an object, got {actual!r}')
        for key, value in expected.items():
            if key not in actual:
                raise Failed(f'{where}.{key} is missing')
            compare(value, actual[key], loose, f'{where}.{key}'.lstrip('.'))
        return
    if type(expected) is not type(actual) or expected != actual:
        raise Failed(f'{where or "message"}: expected {expected!r}, got {actual!r}')


class Stream:
    def __init__(self):
        self.buffer = bytearray()
        self.base = 0
        self.received = 0
        self.descriptors = {}
        self.closed = False
        self.arrived = threading.Condition()
        threading.Thread(target=self.pump, daemon=True).start()

    def pump(self):
        while True:
            data, descriptors = self.receive()
            with self.arrived:
                if descriptors:
                    self.descriptors[self.received] = descriptors
                self.received += len(data)
                self.buffer += data
                if not data:
                    self.closed = True
                self.arrived.notify_all()
            if not data:
                return

    def need(self, count, deadline):
        with self.arrived:
            while len(self.buffer) < count and not self.closed:
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self.arrived.wait(left)
            return len(self.buffer) >= count

    def take(self, count):
        with self.arrived:
            data = bytes(self.buffer[:count])
            start = self.base
            found = [(at, fds) for at, fds in self.descriptors.items() if start <= at < start + count]
            for at, _ in found:
                del self.descriptors[at]
            del self.buffer[:count]
            self.base += count
        if any(at != start for at, _ in found):
            raise Failed('a descriptor came after the first byte of a message')
        return data, [fd for _, fds in found for fd in fds]

    def read_raw(self, count, timeout=10):
        if not self.need(count, time.monotonic() + timeout):
            raise Failed(f'the fake sent fewer than {count} bytes in time')
        return self.take(count)

    def read_message(self, timeout=10):
        deadline = time.monotonic() + timeout
        if not self.need(4, deadline):
            raise Failed('the fake sent nothing in time' if not self.closed else 'the fake closed the stream')
        size = struct.unpack('<I', bytes(self.buffer[:4]))[0]
        if not 2 <= size <= 1 << 20:
            raise Failed(f'the fake sent a length of {size}')
        if not self.need(4 + size, deadline):
            raise Failed('the fake sent part of a message')
        data, descriptors = self.take(4 + size)
        return json.loads(data[4:].decode('utf-8')), descriptors

    def silent_for(self, seconds):
        start = self.received
        time.sleep(seconds)
        return self.received == start

    def send_message(self, message):
        body = json.dumps(message, separators=(',', ':')).encode('utf-8')
        self.send(struct.pack('<I', len(body)) + body)


class PosixStarted(Stream):
    def __init__(self, command, env, log):
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket = ours
        child = theirs.fileno()

        def to_three():
            os.dup2(child, 3)

        self.process = subprocess.Popen(command + ['--ipc-fd', '3', '--parent-pid', str(os.getpid())],
                                        pass_fds=tuple(sorted({child, 3})), preexec_fn=to_three, env=env,
                                        stdout=log, stderr=log)
        theirs.close()
        super().__init__()

    def receive(self):
        try:
            data, control, _, _ = self.socket.recvmsg(65536, socket.CMSG_SPACE(4 * 4))
        except OSError:
            return b'', []
        descriptors = []
        for level, kind, payload in control:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                descriptors += array.array('i', payload[:len(payload) - len(payload) % 4]).tolist()
        return data, descriptors

    def send(self, data):
        try:
            self.socket.sendall(data)
        except OSError:
            pass

    def close(self):
        self.socket.close()


class WindowsStarted(Stream):
    def __init__(self, command, env, log):
        import msvcrt
        to_fake_read, self.to_fake = os.pipe()
        self.from_fake, from_fake_write = os.pipe()
        handles = [msvcrt.get_osfhandle(to_fake_read), msvcrt.get_osfhandle(from_fake_write)]
        for handle in handles:
            os.set_handle_inheritable(handle, True)
        startup = subprocess.STARTUPINFO(lpAttributeList={'handle_list': handles})
        self.process = subprocess.Popen(
            command + ['--ipc-read', str(handles[0]), '--ipc-write', str(handles[1]), '--parent-pid',
                       str(os.getpid())], close_fds=True, startupinfo=startup, env=env, stdout=log, stderr=log)
        os.close(to_fake_read)
        os.close(from_fake_write)
        super().__init__()

    def receive(self):
        try:
            return os.read(self.from_fake, 65536), []
        except OSError:
            return b'', []

    def send(self, data):
        try:
            os.write(self.to_fake, data)
        except OSError:
            pass

    def close(self):
        for fd in (self.to_fake, self.from_fake):
            try:
                os.close(fd)
            except OSError:
                pass


class View:
    def __init__(self, memory, size):
        self.memory = memory
        self.size = size

    def u32(self, offset):
        return struct.unpack_from('<I', self.memory, offset)[0]

    def u64(self, offset):
        return struct.unpack_from('<Q', self.memory, offset)[0]

    def header(self):
        return {'magic': self.u32(0), 'version': self.u32(4), 'slotCount': self.u32(8), 'headerBytes': self.u32(12),
                'slotBytes': self.u64(16), 'latest': self.u32(24), 'reading': self.u32(28),
                'pixelFormat': self.u32(32), 'slots': [self.slot(index) for index in range(3)]}

    def slot(self, index):
        at = 40 + 24 * index
        return {'width': self.u32(at), 'height': self.u32(at + 4), 'visible': self.u32(at + 8),
                'epoch': self.u32(at + 12), 'sequence': self.u64(at + 16)}

    def pixel(self, index, slot_bytes):
        at = 4096 + index * slot_bytes
        return self.memory[at] if at < self.size else None


def map_windows(started, message, size):
    import ctypes.wintypes as w
    import _winapi
    if not isinstance(message.get('handle'), int):
        raise Failed('a Windows ring came without a handle')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.MapViewOfFile.restype = ctypes.c_void_p
    kernel.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
    section = _winapi.DuplicateHandle(int(started.process._handle), message['handle'], _winapi.GetCurrentProcess(),
                                      0x0004 | 0x0002, False, 0)
    view = kernel.MapViewOfFile(section, 0x0004 | 0x0002, 0, 0, message['bytes'])
    if not view:
        raise Failed(f'MapViewOfFile failed ({ctypes.get_last_error()})')

    class Region(ctypes.Structure):
        _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                    ('AllocationProtect', w.DWORD), ('PartitionId', w.WORD), ('RegionSize', ctypes.c_size_t),
                    ('State', w.DWORD), ('Protect', w.DWORD), ('Type', w.DWORD)]

    region = Region()
    kernel.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.POINTER(Region), ctypes.c_size_t]
    if not kernel.VirtualQuery(view, ctypes.byref(region), ctypes.sizeof(region)):
        raise Failed('VirtualQuery failed')
    if region.State != 0x1000 or region.RegionSize < size:
        raise Failed(f'the ring is not committed memory of {size} bytes')
    return View(memoryview((ctypes.c_ubyte * size).from_address(view)).cast('B'), size)


def map_posix(descriptor, ring):
    import fcntl
    size = os.fstat(descriptor).st_size
    memory_bytes = ring['memoryBytes']
    if SYSTEM == 'linux':
        seals = fcntl.fcntl(descriptor, fcntl.F_GET_SEALS)
        wanted = fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL if ring.get('sealed', True) else 0
        if size != memory_bytes or seals != wanted:
            raise Failed(f'the Linux ring is {size} bytes with seals {seals:#x}, not {memory_bytes} with {wanted:#x}')
    else:
        if fcntl.fcntl(descriptor, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDWR:
            raise Failed('the macOS ring is not open for reading and writing')
        if not memory_bytes <= size < memory_bytes + 16384:
            raise Failed(f'the macOS ring is {size} bytes')
        try:
            os.ftruncate(descriptor, memory_bytes)
        except OSError:
            pass
        else:
            raise Failed('the macOS ring took a second ftruncate')
    memory = mmap.mmap(descriptor, memory_bytes, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    os.close(descriptor)
    return View(memory, memory_bytes)


class Launcher:
    def __init__(self, path, mode, work, extra=()):
        self.path = path
        self.session = json.loads(path.read_text(encoding='utf-8'))
        self.steps = self.session['steps']
        self.work = work
        self.report_path = work / 'report.json'
        self.paths = {name: str(work / name) for name in ('core', 'disc', 'playlist', 'disc1', 'disc2', 'system',
                                                          'saves')}
        paths_file = work / 'paths.json'
        paths_file.write_text(json.dumps(self.paths), encoding='utf-8')
        files = ['--session', str(path), '--report', str(self.report_path), '--paths', str(paths_file)]
        env = dict(os.environ)
        if mode == 'env':
            files = []
            env.update(ANYPS5_FAKE_HOST_SESSION=str(path), ANYPS5_FAKE_HOST_REPORT=str(self.report_path),
                       ANYPS5_FAKE_HOST_PATHS=str(paths_file))
        command = [sys.executable, '-I', str(FAKE)] + files + list(extra)
        if mode == 'shim':
            command = [str(self.shim(command))]
        self.log = open(work / 'fake.log', 'wb')
        self.started = (WindowsStarted if WINDOWS else PosixStarted)(command, env, self.log)
        self.call_id = 0
        self.ping_id = 0
        self.calls = {}
        self.pings = {}
        self.ours = set()
        self.welcomed = False
        self.shut = False
        self.epoch = None
        self.view = None
        self.model = None
        self.slot_bytes = 0
        self.status = None
        self.methods = []
        self.killed = False

    def shim(self, command):
        if WINDOWS:
            shim = self.work / 'fake-host.cmd'
            line = ' '.join(f'"{part}"' for part in command)
            shim.write_text(f'@{line} %*\r\n@exit /b %ERRORLEVEL%\r\n', encoding='utf-8')
        else:
            shim = self.work / 'fake-host'
            line = ' '.join("'" + part.replace("'", "'\\''") + "'" for part in command)
            shim.write_text(f'#!/bin/sh\nexec {line} "$@"\n', encoding='utf-8')
            shim.chmod(shim.stat().st_mode | stat.S_IXUSR)
        return shim

    def following(self, index):
        return self.steps[index + 1] if index + 1 < len(self.steps) else {}

    def send(self, message):
        self.started.send_message(message)

    def extra_ping(self):
        self.ping_id += 1
        self.ours.add(self.ping_id)
        self.send({'type': 'ping', 'id': self.ping_id})

    def next_message(self, timeout=10):
        while True:
            message, descriptors = self.started.read_message(timeout)
            if message.get('type') == 'pong' and message.get('id') in self.ours:
                self.ours.discard(message['id'])
                continue
            return message, descriptors

    def play(self, stop_at=None, wrong_first_call=False, never_kill=False):
        for index, step in enumerate(self.steps):
            if index == stop_at:
                return
            if step.get('from') == 'launcher':
                self.launcher_step(step, wrong_first_call)
                wrong_first_call = wrong_first_call and step['message']['type'] != 'call'
            elif step.get('from') == 'host':
                self.host_step(step)
            elif 'hostExits' in step:
                code = self.started.process.wait(15)
                if code != step['hostExits']:
                    raise Failed(f'the fake exited with {code}, not {step["hostExits"]}')
            elif 'silence' in step:
                self.drain()
                if self.welcomed and not self.shut:
                    self.extra_ping()
                kill = 'launcherKillsHost' in self.following(index)
                if not self.started.silent_for(min(step['silence'], 1.0) if kill else step['silence']):
                    raise Failed('the fake sent something during a silence')
            elif 'wait' in step:
                started = time.monotonic()
                self.extra_ping()
                self.call_id += 1
                self.send({'type': 'call', 'id': self.call_id, 'method': 'status'})
                message, _ = self.next_message()
                self.drain()
                compare({'type': 'reply', 'id': self.call_id, 'result': step['status']}, message, set())
                if 'launcherKillsHost' not in self.following(index):
                    time.sleep(max(0.0, step['wait'] - (time.monotonic() - started)))
            elif 'launcherKillsHost' in step:
                if never_kill:
                    return
                self.kill()
            elif 'launcherDraws' in step:
                slot = self.view.slot(self.view.u32(24))
                if (slot['epoch'] == self.epoch) != step['launcherDraws']:
                    raise Failed(f'launcherDraws {step["launcherDraws"]}, but the slot is {slot} in game {self.epoch}')
        if self.started.process.poll() is None and not never_kill:
            if self.session['outcome'] == 'clean':
                if self.status is not None:
                    self.call_id += 1
                    self.send({'type': 'call', 'id': self.call_id, 'method': 'status'})
                    message, _ = self.next_message()
                    compare(dict(self.status, id=self.call_id), message, set())
                self.send({'type': 'shutdown'})
                if self.started.process.wait(10) != 0:
                    raise Failed('the fake did not exit with 0 after shutdown')
            else:
                self.kill()

    def drain(self):
        while self.ours:
            message, _ = self.started.read_message()
            if message.get('type') != 'pong' or message.get('id') not in self.ours:
                raise Failed(f'expected a pong, got {message}')
            self.ours.discard(message['id'])

    def kill(self):
        self.killed = True
        self.started.process.kill()
        self.started.process.wait(10)

    def launcher_step(self, step, wrong):
        message = fill(json.loads(json.dumps(step['message'])), self.paths)
        kind = message['type']
        if kind == 'call':
            if self.welcomed and not self.shut:
                self.extra_ping()
                self.send(INPUT)
            self.call_id += 1
            self.calls[message['id']] = self.call_id
            message['id'] = self.call_id
            self.methods.append(message['method'])
            if wrong:
                message['method'] = 'pause'
                message['args'] = True
        elif kind == 'ping':
            self.ping_id += 1
            self.pings[message['id']] = self.ping_id
            message['id'] = self.ping_id
        elif kind == 'shutdown':
            self.shut = True
        self.send(message)

    def host_step(self, step):
        count = step.get('descriptors', 1 if 'ring' in step else 0)
        if 'bytes' in step:
            wanted = bytes.fromhex(step['bytes'])
            data, descriptors = self.started.read_raw(len(wanted))
            if data != wanted or len(descriptors) != count:
                raise Failed(f'the fake wrote {data.hex()} with {len(descriptors)} descriptors')
            return
        expected = fill(step['message'], self.paths)
        loose = set(step.get('anyValueFor', []))
        message, descriptors = self.next_message()
        kind = expected['type']
        if kind == 'welcome':
            loose.discard('pid')
            pid = self.started.process.pid
            if (message.get('pid') == pid) == bool(step.get('wrongPid')):
                raise Failed(f'the welcome pid is {message.get("pid")}, and the fake host is {pid}')
            expected = dict(expected, pid=message.get('pid'))
            self.welcomed = True
        if kind in ('reply', 'pong') and 'id' in loose:
            loose.discard('id')
            expected = dict(expected, id=(self.calls if kind == 'reply' else self.pings).get(expected['id']))
        compare(expected, message, loose)
        if kind == 'reply' and isinstance(message.get('result'), dict) and 'epoch' in message['result']:
            self.epoch = message['result']['epoch']
        if kind == 'reply' and self.methods and self.methods.pop(0) == 'status':
            self.status = {key: value for key, value in message.items() if key != 'id'}
        if 'ring' in step:
            if WINDOWS:
                if descriptors:
                    raise Failed('a Windows ring came with descriptors')
                self.view = map_windows(self.started, message, step['ring']['memoryBytes'])
            else:
                if len(descriptors) != count:
                    raise Failed(f'a ring came with {len(descriptors)} descriptors, not {count}')
                if not count:
                    return
                self.view = map_posix(descriptors[0], step['ring'])
            self.model = json.loads(json.dumps(step['ring']['header']))
            self.slot_bytes = self.model['slotBytes']
            compare(self.model, self.view.header(), set())
            for index, slot in enumerate(self.model['slots']):
                pixel = self.view.pixel(index, self.slot_bytes)
                if slot['width'] and pixel is not None and pixel != (0x80 if slot['visible'] else 0):
                    raise Failed(f'slot {index} has pixel {pixel}')
            return
        if len(descriptors) != count:
            raise Failed(f'{kind} came with {len(descriptors)} descriptors, not {count}')
        for descriptor in descriptors:
            os.close(descriptor)
        if kind == 'frame' and self.view is not None:
            if 'publish' in step:
                self.model['latest'] = step['publish']['latest']
                self.model['slots'][step['publish']['latest']] = step['publish']['slot']
            compare(self.model, self.view.header(), set())

    def report(self):
        deadline = time.monotonic() + 5
        while True:
            try:
                return json.loads(self.report_path.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                if time.monotonic() > deadline:
                    raise Failed('the fake wrote no report')
                time.sleep(0.05)

    def close(self):
        if self.started.process.poll() is None:
            self.started.process.kill()
            self.started.process.wait()
        self.started.close()
        self.log.close()


def settle(launcher, killed_shim):
    if not killed_shim:
        return launcher.report()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        report = launcher.report()
        if report['killed']:
            return report
        time.sleep(0.05)
    raise Failed('the fake did not notice that its shim was ended')


def run(path, mode='args', **variant):
    work = Path(tempfile.mkdtemp(prefix='fake-host-'))
    launcher = None
    try:
        extra = ['--kill-within', '1'] if variant.get('never_kill') else []
        launcher = Launcher(path, mode, work, extra)
        launcher.play(**variant)
        if variant.get('never_kill') or variant.get('wrong_first_call'):
            code = launcher.started.process.wait(15)
            report = launcher.report()
            if code != 3 or report['passed'] or not report['failures']:
                raise Failed(f'the fake exited with {code} and reported {report} for a wrong launcher')
            return report['failures'][0]
        if variant.get('stop_at') is not None:
            launcher.kill()
            report = launcher.report()
            if report['passed']:
                raise Failed(f'the fake reported a pass when it was ended early: {report}')
            return f'ended at step {report["step"]}, not passed'
        report = settle(launcher, mode == 'shim' and WINDOWS and launcher.killed)
        if not report['passed'] or report['failures']:
            raise Failed(f'the fake reported {report}')
        return 'passed'
    finally:
        if launcher:
            launcher.close()
            if os.environ.get('FAKE_HOST_SELFTEST_LOGS'):
                sys.stdout.write((work / 'fake.log').read_text(errors='replace'))
        shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('names', nargs='*')
    options = parser.parse_args()
    failures = played = skipped = 0
    cases = []
    for path in sorted(SESSIONS.glob('*.json')):
        session = json.loads(path.read_text(encoding='utf-8'))
        if 'launcher' not in session['tests'] or (options.names and path.stem not in options.names):
            continue
        if not runs_here(session):
            skipped += 1
            print(f'skip  {path.stem} (runs only on {session["os"]})')
            continue
        cases.append((path.stem, path, {}))
    if not options.names:
        cases += [
            ('play-and-quit, from environment variables', SESSIONS / 'play-and-quit.json', {'mode': 'env'}),
            ('ring-grows, through a shim', SESSIONS / 'ring-grows.json', {'mode': 'shim'}),
            ('missed-pong, through a shim', SESSIONS / 'missed-pong.json', {'mode': 'shim'}),
            ('play-and-quit, a wrong first call', SESSIONS / 'play-and-quit.json',
             {'wrong_first_call': True, 'stop_at': 3}),
            ('play-and-quit, ended early', SESSIONS / 'play-and-quit.json', {'stop_at': 4}),
            ('welcome-wrong-pid, never ended', SESSIONS / 'welcome-wrong-pid.json', {'never_kill': True}),
        ]
    for name, path, variant in cases:
        played += 1
        try:
            result = run(path, **variant)
            print(f'ok    {name}' + ('' if result == 'passed' else f': {result}'))
        except (Failed, subprocess.TimeoutExpired, OSError) as problem:
            failures += 1
            print(f'FAIL  {name}: {problem}')
    print(f'{played - failures} of {played} fake host cases passed on {SYSTEM}, {skipped} sessions skipped.')
    return 1 if failures or not played else 0


if __name__ == '__main__':
    sys.exit(main())
