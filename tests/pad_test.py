# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Two real SwanStation hosts in lockstep run make_test_disc.py's pad program, which reads both controller
ports every frame and keeps what it read in main RAM (make_test_disc.pad_program). After each frame of a
script, each host saves its state to a file, and the test reads the program's record from the state's
copy of main RAM: both consoles must have read exactly the buttons, sticks and controller mode the script
gave that frame, and act alike on them. The script presses buttons on both ports, moves the sticks, holds
the analog-mode combination (L1 + R1 + Select) to switch modes, has the program lock port 1's mode in
digital (Triangle), holds the combination again while locked (no change), unlocks it in analog mode
(Square), turns the SPU's interrupt on and off (Circle), and unplugs port 2 for a while. The frames'
sync hashes must match too."""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as c  # noqa: E402
import lockstep_test as ls  # noqa: E402
import make_test_disc as disc  # noqa: E402

# libretro's joypad bits (RETRO_DEVICE_ID_JOYPAD_*) and the PS1 button each is in SwanStation's
# AnalogController, whose bit in the controller's reply is the button's number.
LIBRETRO_TO_PS1 = {0: 14, 1: 15, 2: 0, 3: 3, 4: 4, 5: 6, 6: 7, 7: 5, 8: 13, 9: 12, 10: 10, 11: 11,
                   12: 8, 13: 9, 14: 1, 15: 2}
B, Y, SELECT, START, UP, A, X, L, R = 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 8, 1 << 9, 1 << 10, 1 << 11
DIGITAL, ANALOG, NONE = 0x41, 0x73, 0xFF


def ps1_buttons(buttons):
    return sum(1 << LIBRETRO_TO_PS1[bit] for bit in range(16) if buttons >> bit & 1)


def stick(value):
    """The byte SwanStation reports for a libretro axis value at the extremes and the middle used here."""
    return {-32768: 0x00, 0: 0x80, 32767: 0xFF}[value]


def record(peer, work, name):
    path = work / f'{name}.state'
    saved = peer.call('saveState', {'path': str(path)})
    if not saved:
        raise c.Failed(f'{peer.name}: saveState gave {saved}')
    data = path.read_bytes()[48:]
    path.unlink()
    at = data.find(disc.PAD_MAGIC)
    if at < 0 or data.find(disc.PAD_MAGIC, at + 1) >= 0:
        raise c.Failed(f'{peer.name}: the pad program\'s record is not in the state once')
    raw = data[at:at + 0x40]
    ports = []
    for offset in disc.PAD_PORT:
        reply = raw[offset:offset + 9]
        ports.append({'id': reply[1], 'buttons': (reply[3] | reply[4] << 8) ^ 0xFFFF,
                      'sticks': tuple(reply[5:9])})
    return {'ports': ports, 'locks': struct.unpack_from('<I', raw, disc.PAD_LOCKS)[0],
            'unlocks': struct.unpack_from('<I', raw, disc.PAD_UNLOCKS)[0],
            'spucnt': struct.unpack_from('<I', raw, disc.PAD_SPUCNT)[0], 'raw': raw}


def expected_port(ports, index, mode):
    if index >= len(ports):
        return {'id': NONE, 'buttons': 0, 'sticks': (0xFF,) * 4}
    state = ports[index]
    sticks = (0xFF,) * 4
    if mode == ANALOG:
        sticks = (stick(state['rightX']), stick(state['rightY']), stick(state['leftX']), stick(state['leftY']))
    return {'id': mode, 'buttons': ps1_buttons(state['buttons']), 'sticks': sticks}


def scenario():
    """(ports, port 1's mode the frame reads, port 2's mode, what the frame shows) for each frame. A mode
    change by the combination shows in the frame it is held; one the program makes shows in the next."""
    p = ls.port
    return [
        ([p(0), p(0)], DIGITAL, DIGITAL, 'nothing held'),
        ([p(B | UP), p(START)], DIGITAL, DIGITAL, 'buttons on both ports, digital mode'),
        ([p(L | R | SELECT, 32767), p(0)], ANALOG, DIGITAL, 'the combination puts port 1 in analog mode'),
        ([p(L | R | SELECT, -32768), p(0)], ANALOG, DIGITAL, 'held, it does not switch back'),
        ([p(0, -32768), p(L | R | SELECT)], ANALOG, ANALOG, 'port 2 switches on its own'),
        ([p(X), p(0)], ANALOG, ANALOG, 'Triangle: the program locks port 1 in digital mode'),
        ([p(0), p(0)], DIGITAL, ANALOG, 'locked in digital mode'),
        ([p(L | R | SELECT), p(0)], DIGITAL, ANALOG, 'the combination does nothing while locked'),
        ([p(0), p(0)], DIGITAL, ANALOG, 'still digital'),
        ([p(Y, 0), p(0)], DIGITAL, ANALOG, 'Square: the program unlocks it in analog mode'),
        ([p(A, 32767), p(0)], ANALOG, ANALOG, 'analog again; Circle turns the SPU interrupt on'),
        ([p(0)], ANALOG, None, 'port 2 unplugged; Circle let go'),
        ([p(L | R | SELECT)], DIGITAL, None, 'unlocked, the combination switches port 1 to digital'),
        ([p(0), p(0)], DIGITAL, DIGITAL, 'port 2 plugged in again: a new controller, digital'),
        ([p(X), p(0)], DIGITAL, DIGITAL, 'Triangle: port 1 locked in digital mode again'),
        ([p(L | R | SELECT)], DIGITAL, None, 'port 2 unplugged; port 1 stays locked'),
        ([p(0), p(0)], DIGITAL, DIGITAL, 'port 2 plugged in again'),
        ([p(L | R | SELECT), p(0)], DIGITAL, DIGITAL, 'the lock outlasted both: the combination does nothing'),
    ]


def play(options):
    match = ls.Match(options)
    try:
        peers = [match.peer('host'), match.peer('guest')]
        for peer in peers:
            peer.call('start', match.start_args(peer.name, lockstep=True))
        ls.advance(peers, 0, options.boot, lambda peer, frame: [ls.port(0), ls.port(0)])
        locks = unlocks = 0
        for at, (ports, mode_1, mode_2, what) in enumerate(scenario()):
            frame = options.boot + at
            synced = ls.advance(peers, frame, 1, lambda peer, number: ports)
            ls.same(f'frame {frame}', synced['host'], synced['guest'])
            records = [record(peer, match.work, f'{peer.name}-{frame}') for peer in peers]
            if records[0]['raw'] != records[1]['raw']:
                raise c.Failed(f'frame {frame} ({what}): the consoles read differently: {records}')
            seen = records[0]
            want = [expected_port(ports, 0, mode_1), expected_port(ports, 1, mode_2)]
            for index in range(2):
                if seen['ports'][index] != want[index]:
                    raise c.Failed(f'frame {frame} ({what}): port {index + 1} read {seen["ports"][index]}, '
                                   f'not {want[index]}')
            pressed = ps1_buttons(ports[0]['buttons'])
            locks += bool(pressed & disc.PAD_TRIANGLE)
            unlocks += bool(pressed & disc.PAD_SQUARE)
            spucnt = 0x8040 if pressed & disc.PAD_CIRCLE else 0x8000
            if (seen['locks'], seen['unlocks'], seen['spucnt']) != (locks, unlocks, spucnt):
                raise c.Failed(f'frame {frame} ({what}): the program counted {seen["locks"]} locks, '
                               f'{seen["unlocks"]} unlocks and wrote SPUCNT {seen["spucnt"]:#x}')
            print(f'frame {frame}: {what}: both consoles read it')
        for peer in peers:
            peer.host.send_message({'type': 'shutdown'})
            if peer.host.exits(15) != 0:
                raise c.Failed(f'{peer.name}: the host did not exit cleanly')
        print('pad passed')
        return 0
    except c.Failed as error:
        print('FAILED:', error)
        return 1
    finally:
        match.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--core', required=True)
    parser.add_argument('--bios', required=True, help='OpenBIOS, 512 KiB')
    parser.add_argument('--log')
    parser.add_argument('--boot', type=int, default=300, help='Frames to run before the script')
    options = parser.parse_args()
    options.program = disc.pad_program()
    return play(options)

if __name__ == '__main__':
    sys.exit(main())
