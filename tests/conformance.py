# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""The fake launcher of protocol.md "Fixtures": plays every session that tests the host."""

import argparse
import array
import json
import mmap
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SESSIONS = ROOT / 'fixtures' / 'v1' / 'sessions'
WINDOWS = os.name == 'nt'
SYSTEM = 'windows' if WINDOWS else ('macos' if sys.platform == 'darwin' else 'linux')
IGNORED = {'frame', 'rumble'}
NONE = 0xFFFFFFFF


class Failed(Exception):
    pass


def runs_here(session):
    wanted = session.get('os')
    return wanted is None or wanted == SYSTEM or (wanted == 'posix' and not WINDOWS)


def place(root, core):
    discs = root / 'discs'
    discs.mkdir()
    for name in ('game.cue', 'disc1.cue', 'disc2.cue'):
        (discs / name).write_text('FILE "game.bin" BINARY\n', encoding='utf-8')
    (discs / 'game.m3u').write_text('disc1.cue\ndisc2.cue\n', encoding='utf-8')
    system = root / 'system'
    system.mkdir()
    (system / 'fake-bios.bin').write_bytes(b'\0' * 16)
    return {'core': str(core), 'disc': str(discs / 'game.cue'), 'playlist': str(discs / 'game.m3u'),
            'disc1': str(discs / 'disc1.cue'), 'disc2': str(discs / 'disc2.cue'), 'system': str(system),
            'saves': str(root / 'saves')}


def fill(value, paths):
    if isinstance(value, str) and value.startswith('${') and value.endswith('}'):
        return paths[value[2:-1]]
    if isinstance(value, list):
        return [fill(item, paths) for item in value]
    if isinstance(value, dict):
        return {key: fill(item, paths) for key, item in value.items()}
    return value


def same_type(expected, actual):
    if isinstance(expected, bool) or isinstance(actual, bool):
        return isinstance(expected, bool) and isinstance(actual, bool)
    if isinstance(expected, (int, float)):
        return isinstance(actual, (int, float))
    return type(expected) is type(actual) or expected is None or actual is None


def compare(expected, actual, loose, where=''):
    if where in loose:
        if not same_type(expected, actual):
            raise Failed(f'{where or "message"}: {actual!r} is not the type of {expected!r}')
        return
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            raise Failed(f'{where}: expected an object, got {actual!r}')
        for key, value in expected.items():
            if key not in actual:
                raise Failed(f'{where}.{key} is missing')
            compare(value, actual[key], loose, f'{where}.{key}'.lstrip('.'))
        return
    if expected != actual and not (isinstance(expected, (int, float)) and isinstance(actual, (int, float))
                                   and not isinstance(expected, bool) and expected == actual):
        raise Failed(f'{where or "message"}: expected {expected!r}, got {actual!r}')


class PosixHost:
    trace = None

    def __init__(self, program, log):
        self.buffer = b''
        self.descriptors = []
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket = ours
        child = theirs.fileno()

        def to_three():
            os.dup2(child, 3)

        command = [str(program), '--ipc-fd', '3', '--parent-pid', str(os.getpid())]
        if self.trace:
            # strace takes the shell's process id with exec, so the host's parent is the pid it is given.
            command = ['sh', '-c', 'exec strace -f -qq -o "$1" -e trace=%network -- "$0" --ipc-fd 3 --parent-pid $$',
                       str(program), str(self.trace)]
        self.process = subprocess.Popen(command,
                                        pass_fds=tuple(sorted({child, 3})), preexec_fn=to_three, stdout=log,
                                        stderr=log)
        theirs.close()

    def send(self, data, descriptors=0):
        if descriptors:
            spare = [os.open(os.devnull, os.O_RDONLY) for _ in range(descriptors)]
            try:
                self.socket.sendmsg([data[:1]], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', spare))])
            finally:
                for fd in spare:
                    os.close(fd)
            data = data[1:]
        try:
            self.socket.sendall(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_message(self, message, descriptors=0):
        body = json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
        self.send(struct.pack('<I', len(body)) + body, descriptors)

    def read(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack('<I', self.buffer[:4])[0]
                if len(self.buffer) >= 4 + size:
                    body, self.buffer = self.buffer[4:4 + size], self.buffer[4 + size:]
                    descriptors, self.descriptors = self.descriptors, []
                    return json.loads(body.decode('utf-8')), descriptors
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.socket], [], [], left)[0]:
                raise Failed('the host sent nothing in time')
            try:
                data, control, _, _ = self.socket.recvmsg(65536, socket.CMSG_SPACE(4 * 4))
            except ConnectionResetError:
                data, control = b'', []
            for level, kind, payload in control:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    self.descriptors += array.array('i', payload[:len(payload) - len(payload) % 4]).tolist()
            if not data:
                raise Failed('the host closed the connection')
            self.buffer += data

    def exits(self, timeout):
        try:
            return self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            raise Failed(f'the host did not exit within {timeout} s')

    def close(self):
        self.socket.close()
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


    def ring(self, message, descriptors):
        if len(descriptors) != 1:
            raise Failed(f'a ring came with {len(descriptors)} descriptors')
        descriptor = descriptors[0]
        if os.fstat(descriptor).st_size < message['bytes']:
            raise Failed('the ring is smaller than its bytes')
        memory = mmap.mmap(descriptor, message['bytes'], mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        os.close(descriptor)
        return Ring(memory)


class WindowsHost:
    def __init__(self, program, log):
        import msvcrt
        import threading
        self.buffer = b''
        to_host_read, self.to_host = os.pipe()
        self.from_host, from_host_write = os.pipe()
        handles = [msvcrt.get_osfhandle(to_host_read), msvcrt.get_osfhandle(from_host_write)]
        for handle in handles:
            os.set_handle_inheritable(handle, True)
        startup = subprocess.STARTUPINFO(lpAttributeList={'handle_list': handles})
        self.process = subprocess.Popen(
            [str(program), '--ipc-read', str(handles[0]), '--ipc-write', str(handles[1]),
             '--parent-pid', str(os.getpid())], close_fds=True, startupinfo=startup, stdout=log, stderr=log)
        os.close(to_host_read)
        os.close(from_host_write)
        self.chunks = []
        self.arrived = threading.Condition()
        self.closed = False
        threading.Thread(target=self.pump, daemon=True).start()

    def pump(self):
        while True:
            try:
                data = os.read(self.from_host, 65536)
            except OSError:
                data = b''
            with self.arrived:
                if data:
                    self.chunks.append(data)
                else:
                    self.closed = True
                self.arrived.notify_all()
            if not data:
                return

    def send(self, data, descriptors=0):
        if descriptors:
            raise Failed('descriptors are not sent on Windows')
        try:
            os.write(self.to_host, data)
        except OSError:
            pass

    def send_message(self, message, descriptors=0):
        body = json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
        self.send(struct.pack('<I', len(body)) + body, descriptors)

    def read(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack('<I', self.buffer[:4])[0]
                if len(self.buffer) >= 4 + size:
                    body, self.buffer = self.buffer[4:4 + size], self.buffer[4 + size:]
                    return json.loads(body.decode('utf-8')), []
            with self.arrived:
                while not self.chunks and not self.closed:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise Failed('the host sent nothing in time')
                    self.arrived.wait(left)
                if self.chunks:
                    self.buffer += b''.join(self.chunks)
                    self.chunks.clear()
                elif self.closed:
                    raise Failed('the host closed the connection')

    def ring(self, message, descriptors):
        import ctypes
        import ctypes.wintypes
        import _winapi
        if 'handle' not in message:
            raise Failed('a Windows ring came without a handle')
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.MapViewOfFile.restype = ctypes.c_void_p
        kernel.MapViewOfFile.argtypes = [ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
                                         ctypes.wintypes.DWORD, ctypes.c_size_t]
        section = _winapi.DuplicateHandle(int(self.process._handle), message['handle'], _winapi.GetCurrentProcess(),
                                          0x0004 | 0x0002, False, 0)
        view = kernel.MapViewOfFile(section, 0x0004 | 0x0002, 0, 0, message['bytes'])
        if not view:
            raise Failed(f'MapViewOfFile failed ({ctypes.get_last_error()})')
        return Ring((ctypes.c_ubyte * message['bytes']).from_address(view))

    def exits(self, timeout):
        try:
            return self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            raise Failed(f'the host did not exit within {timeout} s')

    def close(self):
        for fd in (self.to_host, self.from_host):
            try:
                os.close(fd)
            except OSError:
                pass
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


class Ring:
    def __init__(self, memory):
        self.memory = memory

    def u32(self, offset):
        return struct.unpack_from('<I', self.memory, offset)[0]

    def u64(self, offset):
        return struct.unpack_from('<Q', self.memory, offset)[0]

    def header(self):
        return {'magic': self.u32(0), 'version': self.u32(4), 'slotCount': self.u32(8), 'headerBytes': self.u32(12),
                'slotBytes': self.u64(16), 'pixelFormat': self.u32(32)}

    def latest(self):
        index = self.u32(24)
        if index == NONE:
            return None
        if index > 2:
            raise Failed(f'latest is {index}')
        at = 40 + 24 * index
        return {'width': self.u32(at), 'height': self.u32(at + 4), 'visible': self.u32(at + 8),
                'epoch': self.u32(at + 12), 'sequence': self.u64(at + 16)}


def play(session, program, core, log):
    work = Path(tempfile.mkdtemp(prefix='retro-host-'))
    paths = place(work, core)
    host = (WindowsHost if WINDOWS else PosixHost)(program, log)
    ring = None
    steps = session['steps']
    poll_id = 1000
    last_call = None
    try:
        index = 0
        while index < len(steps):
            step = steps[index]
            index += 1
            if 'from' not in step:
                if 'hostExits' in step:
                    code = host.exits(15)
                    if code != step['hostExits']:
                        raise Failed(f'the host exited with {code}, not {step["hostExits"]}')
                elif 'pollWithin' in step:
                    expected = steps[index]
                    index += 1
                    deadline = time.monotonic() + step['pollWithin']
                    while True:
                        message, _ = next_from_host(host, 5)
                        try:
                            compare(fill(expected['message'], paths), message, set(expected.get('anyValueFor', [])))
                            break
                        except Failed:
                            if time.monotonic() > deadline:
                                raise
                        poll_id += 1
                        time.sleep(0.1)
                        host.send_message(dict(last_call, id=poll_id))
                continue
            if step['from'] == 'launcher':
                if 'bytes' in step:
                    host.send(bytes.fromhex(step['bytes']), step.get('descriptors', 0))
                else:
                    message = fill(step['message'], paths)
                    if message.get('type') == 'call':
                        last_call = message
                    host.send_message(message, step.get('descriptors', 0))
                continue
            expected = fill(step['message'], paths)
            loose = set(step.get('anyValueFor', []))
            message, descriptors = next_from_host(host, 10, expected.get('type'))
            if message.get('type') == 'ring':
                if WINDOWS:
                    expected = dict(expected, handle=0)
                    loose.add('handle')
                compare(expected, message, loose)
                ring = host.ring(message, descriptors)
                compare({key: value for key, value in step['ring']['header'].items()
                         if key in ('magic', 'version', 'slotCount', 'headerBytes', 'slotBytes', 'pixelFormat')},
                        ring.header(), set())
                continue
            for fd in descriptors:
                os.close(fd)
                raise Failed('a descriptor came with a message other than ring')
            compare(expected, message, loose)
            if message.get('type') == 'frame':
                if ring is None:
                    raise Failed('a frame came before any ring')
                slot = ring.latest()
                if slot is None or not (1 <= slot['width'] <= 8192 and 1 <= slot['height'] <= 8192):
                    raise Failed(f'the latest slot is {slot}')
        outcome = session['outcome']
        if outcome == 'protocolError':
            code = host.exits(10)
            if code == 0:
                raise Failed('the host exited with 0 after a protocol error')
        elif outcome in ('clean', 'unsupported') and host.process.poll() is None:
            host.send_message({'type': 'shutdown'})
            if host.exits(10) != 0:
                raise Failed('the host did not exit cleanly')
    finally:
        host.close()
        shutil.rmtree(work, ignore_errors=True)


def next_from_host(host, timeout, wanted=None):
    while True:
        message, descriptors = host.read(timeout)
        if message.get('type') in IGNORED and message.get('type') != wanted:
            for fd in descriptors:
                os.close(fd)
            continue
        return message, descriptors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=Path)
    parser.add_argument('--core', required=True, type=Path)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--sessions', type=Path, default=SESSIONS)
    parser.add_argument('--strace', type=Path, help='Linux: trace the network calls of the host into this file')
    parser.add_argument('names', nargs='*')
    options = parser.parse_args()
    options.host = options.host.resolve()
    options.core = options.core.resolve()
    log = open(options.log, 'ab') if options.log else subprocess.DEVNULL
    if options.strace:
        PosixHost.trace = options.strace
    failures = 0
    played = 0
    for path in sorted(options.sessions.glob('*.json')):
        session = json.loads(path.read_text(encoding='utf-8'))
        if 'host' not in session['tests'] or not runs_here(session):
            continue
        if options.names and path.stem not in options.names:
            continue
        played += 1
        try:
            play(session, options.host, options.core, log)
            print(f'ok    {path.stem}')
        except Failed as problem:
            failures += 1
            print(f'FAIL  {path.stem}: {problem}')
    print(f'{played - failures} of {played} host sessions passed.')
    return 1 if failures or not played else 0


if __name__ == '__main__':
    sys.exit(main())
