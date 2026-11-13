# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Checks the fake launcher's readers against protocol.md "Framing": a read's timeout is for a
message to begin, and the rest of the message, length and body, has one write limit counted from
its first byte. A read that times out before a message begins raises conformance.Idle, after which
reading again is safe; one that times out in the middle of a message does not. Runs without the
host: the POSIX reader on a socketpair, the Windows reader on a pipe."""

import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402

c.WRITE_LIMIT = 0.5
BODY = b'{"type":"frame"}'
MESSAGE = struct.pack('<I', len(BODY)) + BODY


class SlowAfterRead:
    """The reader's socket, made to take longer than the read's timeout to return each read."""

    def __init__(self, inner, delay):
        self.inner = inner
        self.delay = delay

    def fileno(self):
        return self.inner.fileno()

    def recvmsg(self, *args):
        result = self.inner.recvmsg(*args)
        time.sleep(self.delay)
        return result

    def close(self):
        self.inner.close()


def reader_and_writer(parts):
    """A reader on one end of a socketpair (a pipe on Windows); the other end sends each
    (delay, bytes) in turn."""
    if c.WINDOWS:
        near, far = os.pipe()
        host = c.WindowsHost.__new__(c.WindowsHost)
        host.buffer = b''
        host.from_host = near
        host.chunks = []
        host.arrived = threading.Condition()
        host.closed = False
        host.begun = None
        threading.Thread(target=host.pump, daemon=True).start()

        def write(data):
            os.write(far, data)
    else:
        near, far = socket.socketpair()
        host = c.PosixHost.__new__(c.PosixHost)
        host.socket = near
        write = far.sendall

    def send():
        for delay, data in parts:
            time.sleep(delay)
            write(data)

    writer = threading.Thread(target=send)
    writer.start()
    return host, far, writer


def finish(host, far, writer):
    writer.join()
    if c.WINDOWS:
        os.close(far)
        with host.arrived:
            host.arrived.wait_for(lambda: host.closed)
        os.close(host.from_host)
    else:
        far.close()
        host.socket.close()


def read(host, timeout):
    try:
        return host.read(timeout)[0]
    except c.Failed as error:
        return f'{type(error).__name__}: {error}'


failures = []


def check(name, got, want):
    print(f'{name}: {got!r}')
    if got != want:
        failures.append(f'{name}: got {got!r}, want {want!r}')


pair = reader_and_writer([(0, MESSAGE[:4]), (0.3, MESSAGE[4:] + MESSAGE)])
check('a body after the read timeout', read(pair[0], 0.1), {'type': 'frame'})
check('the next message', read(pair[0], 1), {'type': 'frame'})
finish(*pair)

pair = reader_and_writer([(0, MESSAGE[:1]), (0.2, MESSAGE[1:4]), (0.2, MESSAGE[4:])])
check('a split length within the write limit', read(pair[0], 0.1), {'type': 'frame'})
finish(*pair)

pair = reader_and_writer([(0, MESSAGE[:1]), (0.3, MESSAGE[1:4]), (0.4, MESSAGE[4:])])
check('the rest later than the write limit after the first byte', read(pair[0], 0.1), 'Failed: the host did not finish a message within the write limit')
finish(*pair)

pair = reader_and_writer([(0, MESSAGE[:1]), (0.3, MESSAGE[1:4]), (0.4, MESSAGE[4:])])
check('the same with a read timeout longer than the write limit', read(pair[0], 2), 'Failed: the host did not finish a message within the write limit')
finish(*pair)

pair = reader_and_writer([(0.3, MESSAGE)])
check('nothing before the read timeout', read(pair[0], 0.1), 'Idle: the host sent nothing in time')
check('the message after it', read(pair[0], 1), {'type': 'frame'})
finish(*pair)

if not c.WINDOWS:
    pair = reader_and_writer([(0, MESSAGE + MESSAGE)])
    pair[0].socket = SlowAfterRead(pair[0].socket, 0.2)
    check('a read timeout already over when the body is read', read(pair[0], 0.1), {'type': 'frame'})
    check('the message after that', read(pair[0], 1), {'type': 'frame'})
    finish(*pair)

print('FAILED: ' + '; '.join(failures) if failures else 'framing passed')
sys.exit(1 if failures else 0)
