# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Runs the real SwanStation core through the host with a freely licensed BIOS (OpenBIOS) and
program, and checks that it starts, draws a picture that is not blank, stops and exits cleanly.
The renderer is whichever the machine allows: with no OpenGL context the host must say why a scale
above 1x does not apply."""

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

import conformance as c

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
program = work / 'program.exe'
shutil.copy(options.program, program)
log = open(options.log, 'wb')
host = (c.WindowsHost if c.WINDOWS else c.PosixHost)(host_path, log)
ids = iter(range(1, 1 << 30))
state = {'ring': None, 'frames': 0}


def pump(until=None, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message, descriptors = host.read(max(0.1, deadline - time.monotonic()))
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
    reply = pump(lambda m: m.get('type') == 'reply' and m.get('id') == ident, 10)
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
    ring = state['ring']
    if ring is None:
        raise c.Failed('no frame ring was sent')
    slot = ring.latest()
    offset = 4096 + ring.load(24) * ring.slot_bytes
    data = bytes(ring.memory[offset:offset + slot['width'] * slot['height'] * 4])
    return slot['width'], slot['height'], len({data[i:i + 3] for i in range(0, len(data), 4)})


failed = False
try:
    host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['bgra8', 'rgba8']})
    pump(lambda m: m.get('type') == 'welcome')
    call('volume', {'level': 0.0, 'muted': True})
    call('start', {'core': str(Path(options.core).resolve()), 'system': str(system), 'saves': str(work / 'saves'),
                   'disc': str(program),
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
        width, height, colours = picture()
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
