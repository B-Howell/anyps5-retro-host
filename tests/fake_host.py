# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""The fake host of protocol.md "Fixtures": started by a launcher under test as if it were the host,
it plays one session whose `tests` includes `launcher` and writes a JSON report of how far it got."""

import argparse
import array
import collections
import copy
import json
import math
import mmap
import os
import queue
import random
import re
import socket
import string
import struct
import sys
import threading
import time
import traceback
from pathlib import Path

WINDOWS = os.name == 'nt'
SYSTEM = 'windows' if WINDOWS else ('macos' if sys.platform == 'darwin' else 'linux')
NONE = 0xFFFFFFFF
LARGEST_ID = 2147483647
FAILED_EXIT = 3
ENVIRONMENT = {'session': 'ANYPS5_FAKE_HOST_SESSION', 'report': 'ANYPS5_FAKE_HOST_REPORT',
               'paths': 'ANYPS5_FAKE_HOST_PATHS', 'timeout': 'ANYPS5_FAKE_HOST_TIMEOUT',
               'kill_within': 'ANYPS5_FAKE_HOST_KILL_WITHIN', 'hold': 'ANYPS5_FAKE_HOST_HOLD',
               'resume': 'ANYPS5_FAKE_HOST_RESUME'}


class Failed(Exception):
    pass


def runs_here(session):
    wanted = session.get('os')
    return wanted is None or wanted == SYSTEM or (wanted == 'posix' and not WINDOWS)


def fill(value, paths):
    if isinstance(value, str) and value.startswith('${') and value.endswith('}'):
        name = value[2:-1]
        if name not in paths:
            raise Failed(f'the session needs the path {value}, and --paths does not give it')
        return paths[name]
    if isinstance(value, list):
        return [fill(item, paths) for item in value]
    if isinstance(value, dict):
        return {key: fill(item, paths) for key, item in value.items()}
    return value


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def integer(value):
    if isinstance(value, bool) or not is_number(value):
        return None
    if isinstance(value, float):
        return int(value) if value.is_integer() else None
    return value


def same_type(expected, actual):
    if isinstance(expected, bool) or isinstance(actual, bool):
        return isinstance(expected, bool) and isinstance(actual, bool)
    if is_number(expected):
        return is_number(actual)
    return type(expected) is type(actual)


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
                raise Failed(f'{where}.{key} is missing'.lstrip('.'))
            compare(value, actual[key], loose, f'{where}.{key}'.lstrip('.'))
        return
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(actual) != len(expected):
            raise Failed(f'{where}: expected {expected!r}, got {actual!r}')
        for at, (want, got) in enumerate(zip(expected, actual)):
            compare(want, got, loose, f'{where}[{at}]')
        return
    if not same_type(expected, actual) or expected != actual:
        raise Failed(f'{where or "message"}: expected {expected!r}, got {actual!r}')


def check_values(value):
    if isinstance(value, str):
        try:
            value.encode('utf-8')
        except UnicodeEncodeError:
            raise Failed('a string holds a lone surrogate')
    elif isinstance(value, float) and not math.isfinite(value):
        raise Failed('a number is out of range')
    elif isinstance(value, list):
        for item in value:
            check_values(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            check_values(key)
            check_values(item)


def decode(body):
    try:
        text = body.decode('utf-8')
    except UnicodeDecodeError:
        raise Failed('a body is not UTF-8')

    def pairs(items):
        result = {}
        for key, item in items:
            if key in result:
                raise Failed(f'the key {key!r} appears twice in one object')
            result[key] = item
        return result

    def constant(name):
        raise Failed(f'{name} is not JSON')

    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except ValueError as problem:
        raise Failed(f'a body is not JSON ({problem})')
    check_values(value)
    if not isinstance(value, dict):
        raise Failed('a body is not a JSON object')
    if not isinstance(value.get('type'), str):
        raise Failed('a message has no string type')
    return value


def encode(message):
    body = json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    return struct.pack('<I', len(body)) + body


class Report:
    def __init__(self, path, session):
        self.path = Path(path)
        self.data = {'session': session, 'passed': False, 'failures': [], 'step': 0, 'steps': 0, 'done': False,
                     'awaitingKill': False, 'killed': False, 'reached': [], 'stepStartedAt': time.time()}

    def at(self, index, awaiting_kill):
        self.data['step'] = index
        self.data['awaitingKill'] = awaiting_kill
        self.data['stepStartedAt'] = time.time()
        self.write()

    def fail(self, problem):
        self.data['failures'].append(f'step {self.data["step"]}: {problem}')
        self.data['awaitingKill'] = False
        self.write()

    def write(self):
        data = self.data
        data['passed'] = not data['failures'] and (data['done'] or data['awaitingKill'])
        text = json.dumps(data, indent=1) + '\n'
        spare = self.path.with_name(self.path.name + '.tmp')
        with open(spare, 'w', encoding='utf-8') as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        for _ in range(50):
            try:
                os.replace(spare, self.path)
                return
            except PermissionError:
                time.sleep(0.02)
        self.path.write_text(text, encoding='utf-8')


class Link:
    def __init__(self):
        self.events = queue.Queue()

    def start(self):
        threading.Thread(target=self.pump, daemon=True).start()

    def pump(self):
        try:
            while True:
                header = self.read_exact(4, True)
                if header is None:
                    self.events.put(('eof',))
                    return
                size = struct.unpack('<I', header)[0]
                if not 2 <= size <= 1 << 20:
                    raise Failed(f'a length of {size} is outside 2 to 1 MiB')
                body = self.read_exact(size, False)
                if body is None:
                    raise Failed('the stream ended inside a message')
                self.events.put(('message', decode(body)))
        except Failed as problem:
            self.events.put(('bad', str(problem)))

    def send_message(self, message, descriptors=()):
        self.send(encode(message), descriptors)


class PosixLink(Link):
    def __init__(self, descriptor):
        super().__init__()
        try:
            self.socket = socket.socket(fileno=descriptor)
        except OSError as problem:
            raise Failed(f'descriptor {descriptor} is not a socket ({problem})')
        if self.socket.family != socket.AF_UNIX or self.socket.type != socket.SOCK_STREAM:
            raise Failed(f'descriptor {descriptor} is not an AF_UNIX stream socket')
        self.start()

    def read_exact(self, size, first):
        data = b''
        while len(data) < size:
            try:
                chunk, control, flags, _ = self.socket.recvmsg(size - len(data), socket.CMSG_SPACE(4 * 4),
                                                               getattr(socket, 'MSG_CMSG_CLOEXEC', 0))
            except (ConnectionResetError, OSError):
                chunk, control, flags = b'', [], 0
            for level, kind, payload in control:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    for descriptor in array.array('i', payload[:len(payload) - len(payload) % 4]):
                        os.close(descriptor)
                    raise Failed('the launcher sent a descriptor; only a host ring carries one')
                raise Failed('the launcher sent control data other than SCM_RIGHTS')
            if flags & socket.MSG_CTRUNC:
                raise Failed('the launcher sent truncated ancillary data')
            if not chunk:
                if first and not data:
                    return None
                raise Failed('the stream ended inside a message')
            data += chunk
        return data

    def send(self, data, descriptors=()):
        flags = getattr(socket, 'MSG_NOSIGNAL', 0)
        try:
            if descriptors:
                sent = self.socket.sendmsg([data[:1]], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                         array.array('i', descriptors))], flags)
                data = data[sent:]
            if data:
                self.socket.sendall(data, flags)
        except OSError as problem:
            raise Failed(f'writing to the launcher failed ({problem})')


class WindowsLink(Link):
    def __init__(self, read_handle, write_handle):
        import msvcrt
        super().__init__()
        try:
            self.reader = msvcrt.open_osfhandle(read_handle, os.O_RDONLY | os.O_BINARY)
            self.writer = msvcrt.open_osfhandle(write_handle, os.O_WRONLY | os.O_BINARY)
        except OSError as problem:
            raise Failed(f'the --ipc-read or --ipc-write handle cannot be used ({problem})')
        self.start()

    def read_exact(self, size, first):
        data = b''
        while len(data) < size:
            try:
                chunk = os.read(self.reader, size - len(data))
            except OSError:
                chunk = b''
            if not chunk:
                if first and not data:
                    return None
                raise Failed('the stream ended inside a message')
            data += chunk
        return data

    def send(self, data, descriptors=()):
        if descriptors:
            raise Failed('descriptors are not sent on Windows')
        try:
            while data:
                data = data[os.write(self.writer, data):]
        except OSError as problem:
            raise Failed(f'writing to the launcher failed ({problem})')


class Windows:
    PROCESS_DUP_HANDLE = 0x0040
    SYNCHRONIZE = 0x00100000
    PAGE_READWRITE = 0x04
    SEC_COMMIT = 0x08000000
    FILE_MAP_WRITE = 0x0002
    FILE_MAP_READ = 0x0004
    DUPLICATE_SAME_ACCESS = 0x0002

    def __init__(self):
        import ctypes
        import ctypes.wintypes as w
        self.ctypes = ctypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileMappingW.restype = w.HANDLE
        kernel.CreateFileMappingW.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD, w.DWORD, w.DWORD, w.LPCWSTR]
        kernel.MapViewOfFile.restype = ctypes.c_void_p
        kernel.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
        kernel.OpenProcess.restype = w.HANDLE
        kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        kernel.WaitForSingleObject.restype = w.DWORD
        kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        kernel.GetCurrentProcess.restype = w.HANDLE
        kernel.DuplicateHandle.restype = w.BOOL
        kernel.DuplicateHandle.argtypes = [w.HANDLE, w.HANDLE, w.HANDLE, ctypes.POINTER(w.HANDLE), w.DWORD, w.BOOL,
                                           w.DWORD]
        kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
        kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]

        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ProcessID', w.DWORD),
                        ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', w.DWORD), ('cntThreads', w.DWORD),
                        ('th32ParentProcessID', w.DWORD), ('pcPriClassBase', w.LONG), ('dwFlags', w.DWORD),
                        ('szExeFile', w.WCHAR * 260)]

        self.Entry = Entry
        kernel.Process32FirstW.argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
        kernel.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
        kernel.CloseHandle.argtypes = [w.HANDLE]
        self.kernel = kernel

    def parents(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(0x2, 0)
        if not snapshot or snapshot == self.ctypes.c_void_p(-1).value:
            return {}
        table = {}
        entry = self.Entry()
        entry.dwSize = self.ctypes.sizeof(entry)
        more = self.kernel.Process32FirstW(snapshot, self.ctypes.byref(entry))
        while more:
            table[entry.th32ProcessID] = entry.th32ParentProcessID
            more = self.kernel.Process32NextW(snapshot, self.ctypes.byref(entry))
        self.kernel.CloseHandle(snapshot)
        return table

    def open_process(self, pid, access):
        return self.kernel.OpenProcess(access, False, pid) or None

    def exited(self, handle):
        return self.kernel.WaitForSingleObject(handle, 0) == 0

    def section(self, size):
        section = self.kernel.CreateFileMappingW(self.ctypes.c_void_p(-1).value, None,
                                                 self.PAGE_READWRITE | self.SEC_COMMIT, size >> 32,
                                                 size & 0xFFFFFFFF, None)
        if not section:
            raise Failed(f'CreateFileMappingW failed ({self.ctypes.get_last_error()})')
        view = self.kernel.MapViewOfFile(section, self.FILE_MAP_READ | self.FILE_MAP_WRITE, 0, 0, size)
        if not view:
            raise Failed(f'MapViewOfFile failed ({self.ctypes.get_last_error()})')
        return section, view

    def duplicate_into(self, handle, process):
        target = self.ctypes.wintypes.HANDLE()
        if not self.kernel.DuplicateHandle(self.kernel.GetCurrentProcess(), handle, process,
                                           self.ctypes.byref(target), 0, False, self.DUPLICATE_SAME_ACCESS):
            raise Failed(f'DuplicateHandle into the started process failed ({self.ctypes.get_last_error()})')
        return target.value


def linux_parent(pid):
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
    except OSError:
        return None
    return int(stat.rsplit(')', 1)[1].split()[1])


def started_process(parent_pid, windows):
    me = os.getpid()
    if windows:
        table = windows.parents()
        parent_of = table.get
    elif SYSTEM == 'linux':
        parent_of = linux_parent
    else:
        return me if os.getppid() == parent_pid else None
    current = me
    for _ in range(4):
        parent = parent_of(current)
        if parent is None:
            return None
        if parent == parent_pid:
            return current
        current = parent
    return None


class Memory:
    def __init__(self, size, windows, started_handle):
        self.size = size
        self.extra = {}
        self.descriptor = None
        if windows:
            import ctypes
            self.section, self.view = windows.section(size)
            self.buffer = (ctypes.c_ubyte * size).from_address(self.view)
            self.bytes = memoryview(self.buffer).cast('B')
            handle = self.section if started_handle is None else windows.duplicate_into(self.section, started_handle)
            self.extra = {'handle': handle}
        else:
            self.descriptor = self.create(size)
            self.buffer = mmap.mmap(self.descriptor, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
            self.bytes = self.buffer

    def create(self, size):
        if SYSTEM == 'linux':
            descriptor = os.memfd_create('anyps5-fake-host-ring', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
            os.ftruncate(descriptor, size)
            return descriptor
        import _posixshmem
        name = '/a5r-' + ''.join(random.choice(string.ascii_letters) for _ in range(12))
        descriptor = _posixshmem.shm_open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL, mode=0o600)
        _posixshmem.shm_unlink(name)
        os.set_inheritable(descriptor, False)
        os.ftruncate(descriptor, size)
        return descriptor

    def seal(self):
        import fcntl
        fcntl.fcntl(self.descriptor, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL)

    def u32(self, offset, value):
        if offset + 4 <= self.size:
            struct.pack_into('<I', self.buffer, offset, value & 0xFFFFFFFF)

    def u64(self, offset, value):
        if offset + 8 <= self.size:
            struct.pack_into('<Q', self.buffer, offset, value & 0xFFFFFFFFFFFFFFFF)

    def fill(self, offset, count, byte):
        count = min(count, self.size - offset)
        if count > 0:
            self.bytes[offset:offset + count] = bytes([byte]) * count


class Ring:
    def __init__(self, memory, header):
        self.memory = memory
        self.slot_bytes = header.get('slotBytes', 0)
        m = memory
        m.u32(0, header.get('magic', 0))
        m.u32(4, header.get('version', 0))
        m.u32(8, header.get('slotCount', 0))
        m.u32(12, header.get('headerBytes', 0))
        m.u64(16, self.slot_bytes)
        m.u32(28, header.get('reading', NONE))
        m.u32(32, header.get('pixelFormat', 0))
        m.u32(36, 0)
        for index, slot in enumerate(header.get('slots', [])):
            self.slot(index, slot)
        m.u32(24, header.get('latest', NONE))

    def slot(self, index, slot):
        width, height = slot.get('width', 0), slot.get('height', 0)
        start = 4096 + index * self.slot_bytes
        self.memory.fill(start, min(width * height * 4, self.slot_bytes), 0x80 if slot.get('visible') else 0)
        at = 40 + 24 * index
        self.memory.u32(at, width)
        self.memory.u32(at + 4, height)
        self.memory.u32(at + 8, slot.get('visible', 0))
        self.memory.u32(at + 12, slot.get('epoch', 0))
        self.memory.u64(at + 16, slot.get('sequence', 0))

    def publish(self, publish):
        self.slot(publish['latest'], publish['slot'])
        self.memory.u32(24, publish['latest'])


class Fake:
    def __init__(self, session, link, report, paths, options, pid, windows, started_handle):
        self.session = session
        self.steps = session['steps']
        self.link = link
        self.events = link.events
        self.report = report
        self.paths = paths
        self.options = options
        self.pid = pid
        self.windows = windows
        self.started_handle = started_handle
        self.backlog = collections.deque()
        self.calls = {}
        self.pings = {}
        self.last_call = 0
        self.last_ping = 0
        self.hello = False
        self.welcomed = False
        self.unsupported = False
        self.waiting = collections.deque()
        self.replies = {}
        self.rings = []
        self.ring = None
        self.index = 0
        self.awaiting_kill = False
        self.tail = False

    def run(self):
        self.report.data['steps'] = len(self.steps)
        for index, step in enumerate(self.steps):
            self.index = index
            self.awaiting_kill = self.kill_follows(index)
            self.report.at(index, self.awaiting_kill)
            if 'from' in step:
                if step['from'] == 'host':
                    self.host_step(step)
                else:
                    self.launcher_step(step)
            elif 'hostExits' in step:
                self.exit(step['hostExits'])
            elif 'silence' in step:
                self.quiet(time.monotonic() + step['silence'])
            elif 'wait' in step:
                self.wait(time.monotonic() + step['wait'], step['status'])
            elif 'launcherKillsHost' in step:
                self.await_kill()
            elif 'launcherDraws' in step or 'launcherReports' in step:
                self.report.data['reached'].append(index)
                self.report.write()
                self.hold(index)
            elif 'pollWithin' in step:
                pass
            else:
                raise Failed(f'the fake host does not know the step {step!r}')
        self.finish()

    def kill_follows(self, index):
        step = self.steps[index]
        if 'launcherKillsHost' in step:
            return True
        if 'silence' not in step and 'wait' not in step and step.get('from') != 'host':
            return False
        later = self.steps[index + 1:]
        if any(set(each) & {'from', 'hostExits', 'silence', 'wait', 'pollWithin'} for each in later):
            return False
        return any('launcherKillsHost' in each for each in later) or self.session['outcome'] != 'clean'

    def finish(self):
        self.index = len(self.steps)
        if self.session['outcome'] == 'clean':
            self.tail = True
            self.report.at(self.index, False)
            deadline = time.monotonic() + self.options.timeout
            while True:
                message = self.take(deadline)
                if message is None:
                    raise Failed(f'the launcher neither sent shutdown nor closed the stream within '
                                 f'{self.options.timeout} s of the last step')
                if message['type'] == 'shutdown':
                    self.exit(0)
                self.extra(message)
        self.awaiting_kill = True
        self.report.at(self.index, True)
        self.await_kill()

    def exit(self, code):
        self.report.data['done'] = True
        self.report.write()
        os._exit(code)

    def next_event(self, deadline):
        if self.backlog:
            return self.backlog.popleft()
        try:
            return self.events.get(timeout=max(0.0, deadline - time.monotonic()))
        except queue.Empty:
            return None

    def control(self, event):
        kind = event[0]
        if kind in ('killed', 'launcher-gone'):
            if self.awaiting_kill:
                self.report.data['killed'] = True
                self.exit(0)
            if kind == 'killed':
                raise Failed('the launcher ended the host before the session expected it')
            raise Failed('the launcher exited')
        if kind == 'eof':
            if self.tail:
                self.exit(0)
            if self.awaiting_kill:
                return
            raise Failed('the launcher closed the control stream')
        raise Failed(f'the launcher sent a protocol error: {event[1]}')

    def take(self, deadline, wanted=None):
        while True:
            event = self.next_event(deadline)
            if event is None:
                return None
            if event[0] != 'message':
                self.control(event)
                continue
            message = event[1]
            self.check_order(message)
            kind = message['type']
            if kind == wanted:
                return message
            if kind == 'ping':
                self.send({'type': 'pong', 'id': message['id']})
                continue
            if kind in ('pong', 'input'):
                continue
            return message

    def check_order(self, message):
        kind = message['type']
        if self.unsupported:
            raise Failed(f'the launcher sent {kind} after unsupported')
        if kind == 'hello':
            if self.hello:
                raise Failed('the launcher sent a second hello')
            self.hello = True
            return
        if not self.hello:
            raise Failed(f'the launcher sent {kind} before hello')
        if not self.welcomed:
            raise Failed(f'the launcher sent {kind} before welcome')
        if kind in ('call', 'ping'):
            number = integer(message.get('id'))
            last = self.last_call if kind == 'call' else self.last_ping
            if number is None or not last < number <= LARGEST_ID:
                raise Failed(f'the {kind} id {message.get("id")!r} does not count up from {last}')
            message['id'] = number
            if kind == 'call':
                if not isinstance(message.get('method'), str):
                    raise Failed('a call has no string method')
                message.setdefault('args', None)
                self.last_call = number
            else:
                self.last_ping = number

    def send(self, message, descriptors=()):
        self.link.send_message(message, descriptors)

    def spare_descriptors(self, count):
        if count and WINDOWS:
            raise Failed('descriptors are not sent on Windows')
        return [os.open(os.devnull, os.O_RDONLY) for _ in range(count)]

    def host_step(self, step):
        count = step.get('descriptors', 1 if 'ring' in step else 0)
        if WINDOWS and 'ring' in step:
            count = 0
        if 'bytes' in step:
            spare = self.spare_descriptors(count)
            self.link.send(bytes.fromhex(step['bytes']), spare)
            for descriptor in spare:
                os.close(descriptor)
            return
        message = fill(copy.deepcopy(step['message']), self.paths)
        loose = set(step.get('anyValueFor', []))
        kind = message.get('type')
        attach = []
        if kind == 'welcome':
            message['pid'] = self.pid
            if step.get('wrongPid'):
                written = step['message'].get('pid')
                message['pid'] = written if isinstance(written, int) and written != self.pid else self.pid + 1
            self.welcomed = True
        elif kind == 'unsupported':
            self.unsupported = True
        elif kind == 'reply':
            if 'id' in loose:
                message['id'] = self.calls.get(message.get('id'), message.get('id'))
            if self.waiting:
                self.replies[self.waiting.popleft()] = {key: value for key, value in message.items() if key != 'id'}
        elif kind == 'pong' and 'id' in loose:
            message['id'] = self.pings.get(message.get('id'), message.get('id'))
        if 'ring' in step:
            memory = Memory(step['ring']['memoryBytes'], self.windows, self.started_handle)
            if SYSTEM == 'linux' and step['ring'].get('sealed', True):
                memory.seal()
            self.ring = Ring(memory, step['ring']['header'])
            self.rings.append(self.ring)
            message.update(memory.extra)
            if not WINDOWS and count:
                attach = [memory.descriptor]
                count -= 1
        if kind == 'frame' and 'publish' in step:
            if self.ring is None:
                raise Failed('the session publishes a frame before any ring')
            self.ring.publish(step['publish'])
        spare = self.spare_descriptors(count)
        try:
            self.send(message, attach + spare)
        finally:
            for descriptor in spare:
                os.close(descriptor)

    def launcher_step(self, step):
        expected = fill(step['message'], self.paths)
        loose = set(step.get('anyValueFor', []))
        kind = expected.get('type')
        what = f'call {expected.get("method")!r}' if kind == 'call' else kind
        timeout = self.options.timeout
        message = self.take(time.monotonic() + timeout, kind)
        if message is None:
            raise Failed(f'the launcher sent no {what} within {timeout} s')
        try:
            compare(expected, message, loose)
        except Failed as problem:
            raise Failed(f'expected {what}, got {json.dumps(message)}: {problem}')
        if kind == 'call':
            self.calls[expected.get('id')] = message['id']
            self.waiting.append(message['method'])
        elif kind == 'ping':
            self.pings[expected.get('id')] = message['id']

    def quiet(self, deadline):
        while True:
            try:
                event = self.events.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                return
            if event[0] in ('killed', 'launcher-gone'):
                self.control(event)
            else:
                self.backlog.append(event)

    def wait(self, deadline, status):
        while True:
            message = self.take(deadline)
            if message is None:
                return
            if message['type'] == 'call' and message.get('method') == 'status':
                self.send({'type': 'reply', 'id': message['id'], 'result': status})
                self.replies['status'] = {'type': 'reply', 'result': status}
            else:
                raise Failed(f'the launcher sent {json.dumps(message)} during a wait, where only status calls '
                             f'and pings may come')

    def await_kill(self):
        self.awaiting_kill = True
        self.report.write()
        deadline = time.monotonic() + self.options.kill_within
        while time.monotonic() < deadline:
            event = self.next_event(deadline)
            if event is not None and event[0] in ('killed', 'launcher-gone'):
                self.control(event)
        self.awaiting_kill = False
        raise Failed(f'the launcher did not end the host within {self.options.kill_within} s')

    def resumed(self, index):
        try:
            return int(Path(self.options.resume).read_text(encoding='utf-8').strip() or -1) >= index
        except (OSError, ValueError):
            return False

    def hold(self, index):
        if self.options.resume:
            deadline = time.monotonic() + self.options.timeout
            while not self.resumed(index):
                if time.monotonic() > deadline:
                    raise Failed(f'{self.options.resume} did not reach step {index} within {self.options.timeout} s')
                self.serve(time.monotonic() + 0.05)
        elif self.options.hold > 0:
            deadline = time.monotonic() + self.options.hold
            while time.monotonic() < deadline:
                self.serve(deadline)

    def serve(self, deadline):
        message = self.take(deadline)
        if message is not None:
            self.extra(message)

    def extra(self, message):
        if message['type'] == 'call' and message.get('method') in self.replies:
            self.send(dict(self.replies[message['method']], id=message['id']))
            return
        raise Failed(f'the launcher sent {json.dumps(message)}, which the session does not expect here')


def watch(events, parent_pid, windows, started_handle):
    launcher = windows.open_process(parent_pid, Windows.SYNCHRONIZE) if windows else None
    parent = os.getppid()
    while True:
        time.sleep(0.1)
        if windows:
            if started_handle is not None and windows.exited(started_handle):
                events.put(('killed',))
                return
            if launcher is not None and windows.exited(launcher):
                events.put(('launcher-gone',))
                return
        elif os.getppid() != parent:
            events.put(('launcher-gone',))
            return


class Arguments(argparse.ArgumentParser):
    def error(self, message):
        raise Failed(f'the command line is wrong: {message}')


def parse(argv):
    parser = Arguments(description=__doc__, allow_abbrev=False)
    parser.add_argument('--session', help='the session file (or ANYPS5_FAKE_HOST_SESSION)')
    parser.add_argument('--report', help='where the JSON report is written (or ANYPS5_FAKE_HOST_REPORT)')
    parser.add_argument('--paths', help='a JSON file of placeholder names to strings (or ANYPS5_FAKE_HOST_PATHS)')
    parser.add_argument('--timeout', type=float, help='seconds to wait for each launcher message (default 30)')
    parser.add_argument('--kill-within', type=float, help='seconds to wait to be ended by the launcher (default 10)')
    parser.add_argument('--hold', type=float, help='seconds to pause at each launcherDraws and launcherReports step')
    parser.add_argument('--resume', help='at those steps, wait until this file holds a step index at least theirs')
    parser.add_argument('--ipc-fd')
    parser.add_argument('--ipc-read')
    parser.add_argument('--ipc-write')
    parser.add_argument('--parent-pid')
    return parser.parse_known_args(argv)


def decimal(options, name):
    value = getattr(options, name.replace('-', '_'))
    if value is None or not re.fullmatch('[0-9]+', value):
        raise Failed(f'--{name} is {value!r}, not a decimal number')
    return int(value)


def main(argv):
    try:
        options, unknown = parse(argv)
    except Failed as problem:
        options, unknown = None, [str(problem)]
    values = {}
    for name, variable in ENVIRONMENT.items():
        given = getattr(options, name, None) if options else None
        values[name] = given if given is not None else os.environ.get(variable)
    if not values['report']:
        print('fake_host: no --report or ANYPS5_FAKE_HOST_REPORT', file=sys.stderr)
        return FAILED_EXIT
    name = Path(values['session']).stem if values['session'] else ''
    report = Report(values['report'], name)
    try:
        if options is None or unknown:
            raise Failed(f'the command line is wrong: {" ".join(unknown)}')
        options.timeout = float(values['timeout'] or 30)
        options.kill_within = float(values['kill_within'] or 10)
        options.hold = float(values['hold'] or 0)
        options.resume = values['resume']
        if not values['session']:
            raise Failed('no --session or ANYPS5_FAKE_HOST_SESSION')
        session = json.loads(Path(values['session']).read_text(encoding='utf-8'))
        paths = json.loads(Path(values['paths']).read_text(encoding='utf-8')) if values['paths'] else {}
        if 'launcher' not in session.get('tests', []):
            raise Failed('the session does not test a launcher')
        if not runs_here(session):
            raise Failed(f'the session runs only on {session["os"]}, not {SYSTEM}')
        parent_pid = decimal(options, 'parent-pid')
        if WINDOWS:
            if options.ipc_fd is not None:
                raise Failed('--ipc-fd is not used on Windows')
            read_handle, write_handle = decimal(options, 'ipc-read'), decimal(options, 'ipc-write')
        elif options.ipc_read is not None or options.ipc_write is not None:
            raise Failed('--ipc-read and --ipc-write are used only on Windows')
        elif decimal(options, 'ipc-fd') != 3:
            raise Failed(f'--ipc-fd is {options.ipc_fd}, not 3')
        windows = Windows() if WINDOWS else None
        started = started_process(parent_pid, windows)
        if started is None:
            raise Failed(f'--parent-pid {parent_pid} did not start this process')
        started_handle = None
        if windows and started != os.getpid():
            started_handle = windows.open_process(started, Windows.PROCESS_DUP_HANDLE | Windows.SYNCHRONIZE)
            if started_handle is None:
                raise Failed(f'the started process {started} cannot be opened')
        link = WindowsLink(read_handle, write_handle) if WINDOWS else PosixLink(3)
        threading.Thread(target=watch, args=(link.events, parent_pid, windows, started_handle), daemon=True).start()
        Fake(session, link, report, paths, options, started, windows, started_handle).run()
    except Failed as problem:
        report.fail(str(problem))
    except Exception:
        report.fail(f'the fake host broke: {traceback.format_exc()}')
    try:
        sys.stderr.write(f'fake_host: {"; ".join(report.data["failures"])}\n')
        sys.stderr.flush()
    except (OSError, ValueError, AttributeError):
        pass
    os._exit(FAILED_EXIT)


if __name__ == '__main__':
    main(sys.argv[1:])
