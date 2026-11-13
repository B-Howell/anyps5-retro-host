# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Checks the POSIX fake launcher's reader against protocol.md "Framing": a read's timeout is for a
message to begin, and the rest of the message, length and body, has one write limit counted from
its first byte. Runs without the host, on a socketpair."""

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


def reader_and_writer(parts):
    """A reader on one end of a socketpair; the other end sends each (delay, bytes) in turn."""
    near, far = socket.socketpair()
    host = c.PosixHost.__new__(c.PosixHost)
    host.socket = near

    def send():
        for delay, data in parts:
            time.sleep(delay)
            far.sendall(data)

    writer = threading.Thread(target=send)
    writer.start()
    return host, far, writer


def finish(host, far, writer):
    writer.join()
    far.close()
    host.socket.close()


def read(host, timeout):
    try:
        return host.read(timeout)[0]
    except c.Failed as error:
        return str(error)


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
check('the rest later than the write limit after the first byte', read(pair[0], 0.1), 'the host sent nothing in time')
finish(*pair)

pair = reader_and_writer([(0, MESSAGE[:1]), (0.3, MESSAGE[1:4]), (0.4, MESSAGE[4:])])
check('the same with a read timeout longer than the write limit', read(pair[0], 2), 'the host sent nothing in time')
finish(*pair)

print('FAILED: ' + '; '.join(failures) if failures else 'framing passed')
sys.exit(1 if failures else 0)
