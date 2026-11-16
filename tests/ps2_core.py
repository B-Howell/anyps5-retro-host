# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Runs the real LRPS2 core through the host as a PS2 game (capability ps2, docs/ps2.md) with no
BIOS, since LRPS2 boots nothing without one (docs/ps2.md "The BIOS": there is no freely licensed PS2
BIOS, and LRPS2 has no built-in one). It checks that the host lists ps2, loads the core, reports
its name and core build identity in status.core (with --expect-core-version, and writes it with
--core-identity-out so CI can compare systems), and ends the game with errorCode bios both when
the system directory has no BIOS folder and when that folder holds only a file of a BIOS's size
that is not one, that the core loads again for a later game, and that the host exits cleanly.

The game is a small PS2 program in an ELF file this test writes itself (a loop at its entry
point); LRPS2 refuses the game before it reads the file, so it only has to be a well-formed ELF."""

import argparse
import json
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--host', required=True)
parser.add_argument('--core', required=True, help='LRPS2 (pcsx2_libretro)')
parser.add_argument('--log', required=True)
parser.add_argument('--expect-core-version', help='Fail unless status.core is LRPS2 with this library_version')
parser.add_argument('--core-identity-out', help='Where to write status.core as JSON')
options = parser.parse_args()

# LRPS2's library_name (libretro/main.c, retro_get_system_info).
CORE_NAME = 'ParaLLEl PS2'
# The smallest and largest BIOS image LRPS2 looks at (pcsx2/ps2/BiosTools.c, MIN_BIOS_SIZE).
BIOS_BYTES = 4 * 1024 * 1024


def loop_elf():
    """An ELF32 little-endian MIPS executable of one loadable segment at 0x00100000 whose entry
    point jumps to itself, as a PS2 program's ELF is laid out (EE main RAM)."""
    entry = 0x00100000
    code = struct.pack('<II', 0x08000000 | ((entry >> 2) & 0x03FFFFFF), 0)
    header_bytes, segment_bytes = 52, 32
    offset = header_bytes + segment_bytes
    ident = b'\x7fELF' + bytes([1, 1, 1]) + bytes(9)
    header = ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, entry, header_bytes, 0, 0, header_bytes,
                                 segment_bytes, 1, 40, 0, 0)
    segment = struct.pack('<IIIIIIII', 1, offset, entry, entry, len(code), len(code), 5, 16)
    return header + segment + code


host_path = Path(options.host).resolve()
core_path = Path(options.core).resolve()
c.load_atomics(host_path)
work = Path(tempfile.mkdtemp(prefix='ps2-core-'))
program = work / 'loop.elf'
program.write_bytes(loop_elf())
log = open(options.log, 'wb')
host = (c.WindowsHost if c.WINDOWS else c.PosixHost)(host_path, log)
ids = iter(range(1, 1 << 30))
pings = iter(range(1, 1 << 30))
state = {'pinged': time.monotonic()}


def pump(until, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if time.monotonic() - state['pinged'] >= 2:
            host.send_message({'type': 'ping', 'id': next(pings)})
            state['pinged'] = time.monotonic()
        message, _ = host.read(max(0.1, min(1.0, deadline - time.monotonic())))
        if until(message):
            return message
    raise c.Idle('the host sent nothing that was waited for')


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


def ended(system, what):
    """Starts the program as a PS2 game with system as the system directory, and returns the status of
    the game once it has ended, which it must within 60 seconds and before its first frame."""
    call('start', {'console': 'ps2', 'core': str(core_path), 'disc': str(program), 'system': str(system),
                   'saves': str(work / 'saves')})
    deadline = time.monotonic() + 60
    status = None
    while time.monotonic() < deadline:
        status = call('status')
        if status['ended'] or status['ready']:
            break
        time.sleep(0.2)
    shown = {key: status.get(key) for key in ('ready', 'ended', 'errorCode', 'error', 'console', 'core', 'timing')}
    print(f'{what}: {json.dumps(shown, sort_keys=True)}, {len(status.get("options", {}))} options listed')
    if not status or not status['ended'] or status['ready']:
        raise c.Failed(f'{what}: the game did not end before its first frame; last status {status}')
    if status['errorCode'] != 'bios' or status['error'] != 'No PS2 BIOS was found.':
        raise c.Failed(f'{what}: the game ended with {status["errorCode"]!r} {status["error"]!r}, not the missing BIOS')
    if status.get('console') != 'ps2' or status.get('timing', 'missing') is not None or status['hardware']:
        raise c.Failed(f'{what}: status gives console {status.get("console")!r}, timing {status.get("timing", "missing")!r}'
                       f' and hardware {status["hardware"]!r}; a PS2 game that never loaded has ps2, null and false')
    core = status.get('core')
    if not core or core.get('name') != CORE_NAME:
        raise c.Failed(f'{what}: status.core is {core!r}, not {CORE_NAME}')
    if options.expect_core_version and core.get('version') != options.expect_core_version:
        raise c.Failed(f'{what}: status.core.version is {core.get("version")!r}, not {options.expect_core_version}')
    if call('release') is not None or call('status') is not None:
        raise c.Failed(f'{what}: the ended game was not released')
    return status


failed = False
try:
    host.send_message({'type': 'hello', 'minVersion': 1, 'maxVersion': 1, 'pixelFormats': ['rgba8']})
    welcome = pump(lambda m: m.get('type') == 'welcome')
    print(f'capabilities: {welcome["capabilities"]}')
    for capability in ('ps2', 'coreIdentity'):
        if capability not in welcome['capabilities']:
            raise c.Failed(f'the host does not list {capability}')
    call('volume', {'level': 0.0, 'muted': True})

    bare = work / 'system-without-bios'
    bare.mkdir()
    status = ended(bare, 'no BIOS folder')
    if options.core_identity_out:
        Path(options.core_identity_out).write_text(json.dumps(status['core'], sort_keys=True) + '\n', encoding='utf-8')

    not_bios = work / 'system-with-another-file'
    (not_bios / 'pcsx2' / 'bios').mkdir(parents=True)
    (not_bios / 'pcsx2' / 'bios' / 'scph-not-a-bios.bin').write_bytes(bytes(BIOS_BYTES))
    ended(not_bios, 'a file of a BIOS\'s size that is not one')

    ended(bare, 'the core loaded again')

    host.send_message({'type': 'shutdown'})
    code = host.exits(15)
    if code != 0:
        raise c.Failed(f'the host exited with {code}')
    print('ps2 core passed')
except c.Failed as error:
    failed = True
    print('FAILED:', error)
finally:
    host.close()
    log.close()
    shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failed else 0)
