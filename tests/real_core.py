# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Runs the real SwanStation core through the host with a freely licensed BIOS (OpenBIOS) and
program, put on a disc image by make_test_disc.py, and checks that it starts, draws a picture
that is not black and keeps changing, stops and exits cleanly. With --expect-core-version it also
checks the core build identity in status.core (docs/netplay.md "Core build identity") and can write
it to a file, so CI can compare the identity each system's build reports. It checks status.timing (the
frame rate the host paces the game to and the console's region) on an auto-detected console and on a
PAL console running a program that sets a PAL video mode, and that the game publishes no more frames a
second than that rate.
The renderer is whichever the machine allows: with no OpenGL context the host must say why a scale
above 1x does not apply."""

import argparse
import json
import shutil
import struct
import sys
import tempfile
import time
import zlib
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
parser.add_argument('--picture', help='Where to save the last picture read, as a PNG')
parser.add_argument('--expect-opengl', action='store_true',
                    help='Fail unless the host gives the core an OpenGL context (status.hardware)')
parser.add_argument('--expect-core-version',
                    help='Fail unless status.core is SwanStation with this library_version')
parser.add_argument('--core-identity-out', help='Where to write status.core as JSON')
options = parser.parse_args()

host_path = Path(options.host).resolve()
c.load_atomics(host_path)
work = Path(tempfile.mkdtemp(prefix='real-core-'))
system = work / 'system'
system.mkdir()
shutil.copy(options.bios, system / 'openbios.bin')
disc = work / 'program.cue'
make_test_disc.build(options.program, disc.with_suffix('.bin'), disc)
corner = work / 'corner.exe'
corner.write_bytes(make_test_disc.corner_program())
corner_disc = work / 'corner.cue'
make_test_disc.build(corner, corner_disc.with_suffix('.bin'), corner_disc)
pal_corner = work / 'pal-corner.exe'
pal_corner.write_bytes(make_test_disc.corner_program(pal=True))
pal_disc = work / 'pal-corner.cue'
make_test_disc.build(pal_corner, pal_disc.with_suffix('.bin'), pal_disc)
log = open(options.log, 'wb')
host = (c.WindowsHost if c.WINDOWS else c.PosixHost)(host_path, log)
ids = iter(range(1, 1 << 30))
state = {'ring': None, 'frames': 0, 'epoch': None, 'pinged': time.monotonic(), 'pings': iter(range(1, 1 << 30))}


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
    raise c.Idle('the host sent nothing that was waited for')


def drain(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            pump(timeout=min(1.0, end - time.monotonic()))
        except c.Idle:
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
    """The pinned frame's size and pixels, read as protocol.md "Frame ring" says a launcher reads,
    or None when there is no frame to read now."""
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
    width, height, epoch = ring.u32(at), ring.u32(at + 4), ring.u32(at + 12)
    if epoch != state['epoch']:
        return None
    if not (1 <= width <= 8192 and 1 <= height <= 8192) or width * height * 4 > ring.slot_bytes:
        raise c.Failed(f'slot {index} is {width}x{height}')
    offset = 4096 + index * ring.slot_bytes
    data = bytes(ring.memory[offset:offset + width * height * 4])
    if options.picture:
        save_png(width, height, data, ring.snapshot['pixelFormat'] == 2)
    return width, height, data


def start(game, region=None):
    """Starts game at 2x, on a console of region when given, and waits for its first frame; returns the
    status then."""
    core_options = {'swanstation_BIOS_PathNTSCU': 'openbios.bin', 'swanstation_BIOS_PathNTSCJ': 'openbios.bin',
                    'swanstation_BIOS_PathPAL': 'openbios.bin', 'swanstation_GPU_ResolutionScale': '2'}
    if region:
        core_options['swanstation_Console_Region'] = region
    result = call('start', {'core': str(Path(options.core).resolve()), 'system': str(system),
                            'saves': str(work / 'saves'), 'disc': str(game), 'options': core_options})
    state['epoch'] = result['epoch']
    status = wait_status(lambda s: s['ready'] or s['ended'], 60, 'the game was not ready')
    if status['ended']:
        raise c.Failed(f'the game ended at start: {status["errorCode"]!r} {status.get("error")!r}')
    print(f'ready: hardware={status["hardware"]} resolutionError={status["resolutionError"]!r}')
    if not status['hardware'] and not status['resolutionError']:
        raise c.Failed('software drawing at 2x must explain itself in resolutionError')
    if status['hardware'] and status['resolutionError']:
        raise c.Failed('GPU drawing must leave resolutionError empty')
    core = status.get('core')
    print(f'core: {core}')
    if options.expect_core_version and core != {'name': 'SwanStation', 'version': options.expect_core_version}:
        raise c.Failed(f'status.core is {core!r}, not SwanStation {options.expect_core_version}')
    if options.core_identity_out:
        Path(options.core_identity_out).write_text(json.dumps(core, sort_keys=True) + '\n', encoding='utf-8')
    if options.expect_opengl and not status['hardware']:
        raise c.Failed('this machine has an OpenGL renderer, but the host made no context for the core')
    check_timing(status, 'pal' if region == 'PAL' else None)
    scales = status['options']['swanstation_GPU_ResolutionScale']['values']
    if status['hardware'] and max(int(value) for value in scales) > 8:
        raise c.Failed(f'scales whose largest geometry, 1024 x 512 times the scale, passes 8192 are offered: {scales}')
    return status


def check_timing(status, region=None):
    """status.timing is the console's region (region, when the console was set to one) and a frame rate of
    one of the two video standards: SwanStation gives the vertical frequency of the video mode the game set."""
    timing = status.get('timing')
    print(f'timing: {timing}')
    if not isinstance(timing, dict) or timing.get('region') not in ('ntsc', 'pal'):
        raise c.Failed(f'status.timing is {timing!r}, not an NTSC or PAL region and a frame rate')
    if region and timing['region'] != region:
        raise c.Failed(f'a {region} console gave region {timing["region"]!r}')
    if not (48 < timing["fps"] < 52 or 58 < timing["fps"] < 62):
        raise c.Failed(f'status.timing.fps is {timing["fps"]!r}, neither about 50 nor about 60')
    return timing


def paced(seconds, what, pal=False):
    """Counts the frames published over seconds and checks they are no more than the frame rate in
    status.timing allows; with pal, that the rate is PAL's, about 50."""
    before = state['frames']
    drain(seconds)
    shown = state['frames'] - before
    fps = check_timing(call('status'), 'pal' if pal else None)['fps']
    if pal and not 48 < fps < 52:
        raise c.Failed(f'a PAL video mode gave status.timing.fps {fps!r}, not about 50')
    print(f'{what}: {shown} frames in {seconds} s at {fps:.3f} frames a second')
    if shown > fps * seconds * 1.05:
        raise c.Failed(f'{shown} frames in {seconds} s is faster than {fps:.3f} frames a second')
    return shown


def stop():
    call('stop')
    status = wait_status(lambda s: s['ended'], 10, 'the game did not end after stop')
    if status['errorCode']:
        raise c.Failed(f'stop ended with {status["errorCode"]!r}')
    call('release')


def colour(seen, x, y):
    """The red, green and blue of the pixel at x, y of a picture from picture()."""
    width, _, data = seen
    at = (y * width + x) * 4
    red, green, blue = data[at], data[at + 1], data[at + 2]
    return (blue, green, red) if state['ring'].snapshot['pixelFormat'] == 2 else (red, green, blue)


def is_corner_picture(seen):
    """The corner program's picture the right way up: red only near the top-left corner."""
    width, height, _ = seen
    red, blue = (255, 0, 0), (0, 0, 255)
    left, right, top, bottom = width // 20, width - 1 - width // 20, height // 20, height - 1 - height // 20
    return (near(colour(seen, left, top), red) and near(colour(seen, right, top), blue)
            and near(colour(seen, left, bottom), blue) and near(colour(seen, right, bottom), blue))


def near(got, want):
    return all(abs(a - b) < 48 for a, b in zip(got, want))


def save_png(width, height, data, bgra):
    if bgra:
        swapped = bytearray(data)
        swapped[0::4], swapped[2::4] = data[2::4], data[0::4]
        data = bytes(swapped)
    rows = b''.join(b'\0' + data[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    Path(options.picture).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                                      + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


failed = False
try:
    host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['bgra8', 'rgba8']})
    welcome = pump(lambda m: m.get('type') == 'welcome')
    if options.expect_core_version and 'coreIdentity' not in welcome['capabilities']:
        raise c.Failed(f'the host does not list coreIdentity: {welcome["capabilities"]}')
    call('volume', {'level': 0.0, 'muted': True})
    status = start(disc)
    shown = paced(10, 'auto-detected console')
    pictures = []
    deadline = time.monotonic() + 30
    while len(set(p[2] for p in pictures)) < 2 and time.monotonic() < deadline:
        drain(1)
        seen = picture()
        if seen and any(seen[2][i] or seen[2][i + 1] or seen[2][i + 2] for i in range(0, len(seen[2]), 4)):
            pictures.append(seen)
    width, height = pictures[-1][:2] if pictures else (0, 0)
    print(f'{shown} frames in 10 s; {len(pictures)} pictures that are not black, last {width}x{height}')
    if shown < 100:
        raise c.Failed('fewer than 100 frames in 10 s')
    if len(set(p[2] for p in pictures)) < 2:
        raise c.Failed('no two pictures that are not black differ: the game is not running')
    if status['hardware'] and (width < 640 or height < 480):
        raise c.Failed('a 2x GPU picture is at least 640x480')
    stop()
    # A second game, on a new context where the host has the GPU: the corner program shows whether
    # pictures reach the ring the right way up and in the right colours.
    start(corner_disc)
    seen = None
    deadline = time.monotonic() + 15
    while not (seen and is_corner_picture(seen)) and time.monotonic() < deadline:
        drain(1)
        seen = picture() or seen
    if not seen:
        raise c.Failed('the corner program showed no picture')
    print(f'corner picture {seen[0]}x{seen[1]}: top-left {colour(seen, seen[0] // 20, seen[1] // 20)}, '
          f'bottom-right {colour(seen, seen[0] - 1 - seen[0] // 20, seen[1] - 1 - seen[1] // 20)}')
    if not is_corner_picture(seen):
        raise c.Failed('the corner program is not red at the top-left only: the picture is flipped or its colours are wrong')
    stop()
    # A PAL console whose program sets a PAL video mode: SwanStation gives the new rate after a frame.
    start(pal_disc, 'PAL')
    drain(1)
    if paced(5, 'PAL console in a PAL mode', pal=True) < 50:
        raise c.Failed('fewer than 50 frames in 5 s on a PAL console')
    stop()
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
