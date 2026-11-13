# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Makes a one-track PS1 data disc (.cue and a MODE2/2352 .bin) from a PS-EXE: an ISO 9660 file
system holding SYSTEM.CNF and the program, in Mode 2 Form 1 sectors with their EDC and ECC, as a
real PS1 disc has them. A BIOS boots it through SYSTEM.CNF, so real-core tests can start a freely
licensed program the way the protocol starts a game (protocol.md, "start")."""

import argparse
import struct
from pathlib import Path

SECTOR = 2048
RAW = 2352
PREGAP = 150
SUBMODE_DATA = 0x08
SUBMODE_END = 0x89

EDC_TABLE = []
ECC_F = [0] * 256
ECC_B = [0] * 256
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    ECC_F[i] = j
    ECC_B[i ^ j] = i
    edc = i
    for _ in range(8):
        edc = (edc >> 1) ^ (0xD8018001 if edc & 1 else 0)
    EDC_TABLE.append(edc)


def edc(data):
    value = 0
    for byte in data:
        value = (value >> 8) ^ EDC_TABLE[(value ^ byte) & 0xFF]
    return value


def ecc_block(sector, major_count, minor_count, major_mult, minor_inc, out):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            value = sector[0x0C + index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= value
            b ^= value
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        sector[out + major] = a
        sector[out + major + major_count] = a ^ b


def bcd(value):
    return (value // 10) << 4 | value % 10


def raw_sector(lba, data, submode):
    sector = bytearray(RAW)
    sector[0:12] = b'\x00' + b'\xff' * 10 + b'\x00'
    position = lba + PREGAP
    sector[12:16] = bytes([bcd(position // 4500), bcd(position // 75 % 60), bcd(position % 75), 2])
    sector[16:24] = bytes([0, 0, submode, 0]) * 2
    sector[24:24 + SECTOR] = data.ljust(SECTOR, b'\0')
    struct.pack_into('<I', sector, 0x818, edc(sector[16:0x818]))
    header = bytes(sector[12:16])
    sector[12:16] = b'\0\0\0\0'
    ecc_block(sector, 86, 24, 2, 86, 0x81C)
    ecc_block(sector, 52, 43, 86, 88, 0x8C8)
    sector[12:16] = header
    return bytes(sector)


def both16(value):
    return struct.pack('<H', value) + struct.pack('>H', value)


def both32(value):
    return struct.pack('<I', value) + struct.pack('>I', value)


def record(name, lba, size, directory):
    date = bytes([126, 1, 1, 0, 0, 0, 0])
    body = both32(lba) + both32(size) + date + bytes([2 if directory else 0, 0, 0]) + both16(1)
    body += bytes([len(name)]) + name
    if len(name) % 2 == 0:
        body += b'\0'
    return bytes([len(body) + 2, 0]) + body


def text(value, length):
    return value.encode('ascii').ljust(length, b' ')


def corner_program():
    """A PS-EXE that fills the 320x240 screen blue and the 64x32 block at its top-left corner red,
    then waits forever, so a test can tell which way up a picture is. Each GPU port write (GP1 at
    0x1F801814, GP0 at 0x1F801810) is followed by a short wait, as the GPU takes one command at a
    time."""
    code = []

    def word(op, rs, rt, imm):
        code.append(op << 26 | rs << 21 | rt << 16 | imm & 0xFFFF)

    def write(value, port):
        word(0x0F, 0, 9, value >> 16)  # lui $t1
        word(0x0D, 9, 9, value)  # ori $t1, $t1
        word(0x2B, 8, 9, port)  # sw $t1, port($t0)
        word(0x09, 0, 10, 0x4000)  # addiu $t2, $zero, 0x4000
        word(0x09, 10, 10, -1)  # addiu $t2, $t2, -1
        word(0x05, 10, 0, -2)  # bne $t2, $zero, back one
        code.append(0)

    word(0x0F, 0, 8, 0x1F80)  # lui $t0, 0x1F80
    word(0x0D, 8, 8, 0x1810)  # ori $t0, $t0, 0x1810
    # GP1: reset, 320x240 NTSC 15-bit, display start 0,0, standard ranges, display on.
    for value in (0x00000000, 0x08000001, 0x05000000, 0x06C60260, 0x07040010, 0x03000000):
        write(value, 4)
    # GP0 fill rectangle: colour (0x02BBGGRR), top-left (y << 16 | x), size (h << 16 | w).
    for value in (0x02FF0000, 0x00000000, 0x00F00140, 0x020000FF, 0x00000000, 0x00200040):
        write(value, 0)
    code += [0x1000FFFF, 0]  # b self
    body = b''.join(struct.pack('<I', value) for value in code)
    body = body.ljust(-(-len(body) // SECTOR) * SECTOR, b'\0')
    header = bytearray(SECTOR)
    header[0:8] = b'PS-X EXE'
    struct.pack_into('<4I', header, 0x10, 0x80010000, 0, 0x80010000, len(body))
    struct.pack_into('<2I', header, 0x30, 0x801FFFF0, 0)
    marker = b'Sony Computer Entertainment Inc. for North America area'
    header[0x4C:0x4C + len(marker)] = marker
    return bytes(header) + body


def build(program, bin_path, cue_path):
    exe = Path(program).read_bytes()
    if exe[:8] != b'PS-X EXE':
        raise SystemExit(f'{program} is not a PS-EXE')
    config = b'BOOT = cdrom:\\PROGRAM.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'
    root_lba, config_lba, exe_lba = 22, 23, 24
    exe_sectors = -(-len(exe) // SECTOR)
    total = exe_lba + exe_sectors
    root = (record(b'\x00', root_lba, SECTOR, True) + record(b'\x01', root_lba, SECTOR, True)
            + record(b'PROGRAM.EXE;1', exe_lba, len(exe), False) + record(b'SYSTEM.CNF;1', config_lba, len(config), False))
    path_l = bytes([1, 0]) + struct.pack('<I', root_lba) + struct.pack('<H', 1) + b'\x00\x00'
    path_m = bytes([1, 0]) + struct.pack('>I', root_lba) + struct.pack('>H', 1) + b'\x00\x00'
    pvd = bytearray(SECTOR)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = text('PLAYSTATION', 32)
    pvd[40:72] = text('ANYPS5_TEST', 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(path_l))
    struct.pack_into('<I', pvd, 140, 18)
    struct.pack_into('>I', pvd, 148, 19)
    pvd[156:190] = record(b'\x00', root_lba, SECTOR, True)
    pvd[190:318] = text('', 128)
    pvd[318:446] = text('', 128)
    pvd[446:574] = text('', 128)
    pvd[574:702] = text('', 128)
    for at in (813, 830, 847, 864):
        pvd[at:at + 17] = b'0' * 16 + b'\0'
    pvd[881] = 1
    sectors = {16: (bytes(pvd), SUBMODE_END), 17: (b'\xffCD001\x01', SUBMODE_END),
               18: (path_l, SUBMODE_END), 19: (path_m, SUBMODE_END), root_lba: (root, SUBMODE_END),
               config_lba: (config, SUBMODE_END)}
    for index in range(exe_sectors):
        last = index == exe_sectors - 1
        sectors[exe_lba + index] = (exe[index * SECTOR:(index + 1) * SECTOR], SUBMODE_END if last else SUBMODE_DATA)
    with open(bin_path, 'wb') as out:
        for lba in range(total):
            data, submode = sectors.get(lba, (b'', SUBMODE_DATA))
            out.write(raw_sector(lba, data, submode))
    Path(cue_path).write_text(f'FILE "{Path(bin_path).name}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n',
                              encoding='ascii')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('program')
    parser.add_argument('cue')
    options = parser.parse_args()
    cue = Path(options.cue)
    build(options.program, cue.with_suffix('.bin'), cue)
