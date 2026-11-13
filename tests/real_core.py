# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Runs the real SwanStation core through the host with a freely licensed BIOS (OpenBIOS) and
program, put on a disc image by make_test_disc.py, and checks that it starts, draws a picture that is not blank, stops and exits cleanly.
The renderer is whichever the machine allows: with no OpenGL context the host must say why a scale
above 1x does not apply."""

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402
import make_test_disc  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('--host', required=True)
parser.add_argument('--core', required=True)
parser.add_argument('--bios', required=True, help='OpenBIOS, 512 KiB')
parser.add_argument('--program', required=True, help='A PS-EXE that draws to the screen')
parser.add_argument('--log', required=True)
options = parser.parse_args()

host_path = Path(options.host).resolve()
c.load_atomics(host_path)
work = Path(tempfile.mkdtemp(prefix='real-core-'))
system = work / 'system'
system.mkdir()
shutil.copy(options.bios, system / 'openbios.bin')
disc = work / 'program.cue'
make_test_disc.build(options.program, disc.with_suffix('.bin'), disc)
log = open(options.log, 'wb')
host = (c.WindowsHost if c.WINDOWS else c.PosixHost)(host_path, log)
ids = iter(range(1, 1 << 30))
state = {'ring': None, 'frames': 0, 'pinged': time.monotonic(), 'pings': iter(range(1, 1 << 30))}


def pump(until=None, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if time.monotonic() - state['pinged'] >= 2:
            host.send_message({'type': 'ping', 'id': next(state['pings'])})
            state['pinged'] = time.monotonic()
        message, descriptors = host.read(max(0.1, min(1.0, deadline - time.monotonic())))
        if message.get('type') == 'ring':
            state['ring'] = host.ring(message, descriptors)
        elif message.get('type') == 'frame':
            state['frames'] += 1
        if until and until(message):
            return message
    raise c.Failed('the host sent nothing that was waited for')


def drain(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            pump(timeout=min(1.0, end - time.monotonic()))
        except c.Failed:
            pass


def call(method, args=None):
    ident = next(ids)
    message = {'type': 'call', 'id': ident, 'method': method}
    if args is not None:
        message['args'] = args
    host.send_message(message)
    reply = pump(lambda m: m.get('type') == 'reply' and m.get('id') == ident, 5)
    if 'error' in reply:
        raise c.Failed(f'{method}: {reply["error"]}')
    return reply.get('result')


def wait_status(check, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        status = call('status')
        if check(status):
            return status
        drain(0.5)
    raise c.Failed(f'{what} within {seconds} s; last status {status}')


def picture():
    """The pinned frame's size and how many colours it has, read as protocol.md "Frame ring" says
    a launcher reads, or None when there is no frame to read now."""
    ring = state['ring']
    if ring is None:
        raise c.Failed('no frame ring was sent')
    for _ in range(3):
        index = ring.load(24)
        if index == c.NONE:
            return None
        if index > 2:
            raise c.Failed(f'latest is {index}')
        ring.store(28, index)
        if ring.load(24) == index:
            break
    else:
        return None
    at = 40 + 24 * index
    width, height = ring.u32(at), ring.u32(at + 4)
    if not (1 <= width <= 8192 and 1 <= height <= 8192) or width * height * 4 > ring.slot_bytes:
        raise c.Failed(f'slot {index} is {width}x{height}')
    offset = 4096 + index * ring.slot_bytes
    data = bytes(ring.memory[offset:offset + width * height * 4])
    return width, height, len({data[i:i + 3] for i in range(0, len(data), 4)})


failed = False
try:
    host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['bgra8', 'rgba8']})
    pump(lambda m: m.get('type') == 'welcome')
    call('volume', {'level': 0.0, 'muted': True})
    call('start', {'core': str(Path(options.core).resolve()), 'system': str(system), 'saves': str(work / 'saves'),
                   'disc': str(disc),
                   'options': {'swanstation_BIOS_PathNTSCU': 'openbios.bin', 'swanstation_BIOS_PathNTSCJ': 'openbios.bin',
                               'swanstation_BIOS_PathPAL': 'openbios.bin', 'swanstation_GPU_ResolutionScale': '2'}})
    status = wait_status(lambda s: s['ready'] or s['ended'], 60, 'the game was not ready')
    if status['ended']:
        raise c.Failed(f'the game ended at start: {status["errorCode"]!r} {status.get("error")!r}')
    print(f'ready: hardware={status["hardware"]} resolutionError={status["resolutionError"]!r}')
    if not status['hardware'] and not status['resolutionError']:
        raise c.Failed('software drawing at 2x must explain itself in resolutionError')
    if status['hardware'] and status['resolutionError']:
        raise c.Failed('GPU drawing must leave resolutionError empty')
    before = state['frames']
    drain(10)
    shown = state['frames'] - before
    width, height, colours = 0, 0, 0
    deadline = time.monotonic() + 30
    while colours < 2 and time.monotonic() < deadline:
        drain(1)
        seen = picture()
        if seen:
            width, height, colours = seen
    print(f'{shown} frames in 10 s; picture {width}x{height} with {colours} colours')
    if shown < 100:
        raise c.Failed('fewer than 100 frames in 10 s')
    if colours < 2:
        raise c.Failed('the picture stayed one colour')
    if status['hardware'] and (width < 640 or height < 480):
        raise c.Failed('a 2x GPU picture is at least 640x480')
    call('stop')
    status = wait_status(lambda s: s['ended'], 10, 'the game did not end after stop')
    if status['errorCode']:
        raise c.Failed(f'stop ended with {status["errorCode"]!r}')
    call('release')
    host.send_message({'type': 'shutdown'})
    code = host.exits(15)
    if code != 0:
        raise c.Failed(f'the host exited with {code}')
    print('real core passed')
except c.Failed as error:
    failed = True
    print('FAILED:', error)
finally:
    host.close()
    log.close()
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
