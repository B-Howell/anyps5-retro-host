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
    return ps_exe(b''.join(struct.pack('<I', value) for value in code))


def ps_exe(body):
    """A PS-EXE that loads body at 0x80010000 and starts there, with the stack at the top of RAM."""
    body = body.ljust(-(-len(body) // SECTOR) * SECTOR, b'\0')
    header = bytearray(SECTOR)
    header[0:8] = b'PS-X EXE'
    struct.pack_into('<4I', header, 0x10, 0x80010000, 0, 0x80010000, len(body))
    struct.pack_into('<2I', header, 0x30, 0x801FFFF0, 0)
    marker = b'Sony Computer Entertainment Inc. for North America area'
    header[0x4C:0x4C + len(marker)] = marker
    return bytes(header) + body


class Assembler:
    """Just enough of the R3000A for the test programs: each method appends one instruction, with labels
    for branches, jumps and data. The CPU runs the instruction after a branch or jump (its delay slot), and a
    loaded value is not ready for the next instruction, so callers put a nop in each."""

    REGISTERS = {name: number for number, name in enumerate(
        'zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split())}

    def __init__(self, origin):
        self.origin = origin
        self.items = []
        self.labels = {}

    def here(self):
        return self.origin + sum(self.size(kind, value) for kind, value in self.items)

    @staticmethod
    def size(kind, value):
        return len(value) if kind == 'bytes' else 8 if kind == 'la' else 4

    def label(self, name):
        self.labels[name] = self.here()

    def word(self, value):
        self.items.append(('word', value))

    def data(self, value):
        self.items.append(('bytes', bytes(value)))

    def r(self, name):
        return self.REGISTERS[name]

    def i_type(self, op, rs, rt, imm):
        self.items.append(('i', (op, self.r(rs), self.r(rt), imm)))

    def r_type(self, funct, rs='zero', rt='zero', rd='zero', shift=0):
        self.word(self.r(rs) << 21 | self.r(rt) << 16 | self.r(rd) << 11 | shift << 6 | funct)

    def nop(self):
        self.word(0)

    def li(self, rt, value):
        value &= 0xFFFFFFFF
        if value >> 16:
            self.i_type(0x0F, 'zero', rt, value >> 16)
            if value & 0xFFFF:
                self.i_type(0x0D, rt, rt, value & 0xFFFF)
        else:
            self.i_type(0x0D, 'zero', rt, value)

    def la(self, rt, name):
        self.items.append(('la', (self.r(rt), name)))

    def addiu(self, rt, rs, imm): self.i_type(0x09, rs, rt, imm)
    def andi(self, rt, rs, imm): self.i_type(0x0C, rs, rt, imm)
    def ori(self, rt, rs, imm): self.i_type(0x0D, rs, rt, imm)
    def lw(self, rt, offset, base): self.i_type(0x23, base, rt, offset)
    def lbu(self, rt, offset, base): self.i_type(0x24, base, rt, offset)
    def sb(self, rt, offset, base): self.i_type(0x28, base, rt, offset)
    def sh(self, rt, offset, base): self.i_type(0x29, base, rt, offset)
    def sw(self, rt, offset, base): self.i_type(0x2B, base, rt, offset)
    def sll(self, rd, rt, shift): self.r_type(0x00, rt=rt, rd=rd, shift=shift)
    def srl(self, rd, rt, shift): self.r_type(0x02, rt=rt, rd=rd, shift=shift)
    def or_(self, rd, rs, rt): self.r_type(0x25, rs, rt, rd)
    def nor(self, rd, rs, rt): self.r_type(0x27, rs, rt, rd)
    def addu(self, rd, rs, rt): self.r_type(0x21, rs, rt, rd)
    def jr(self, rs): self.r_type(0x08, rs)
    def mtc0(self, rt, rd): self.word(0x40800000 | self.r(rt) << 16 | rd << 11)

    def beq(self, rs, rt, name): self.items.append(('branch', (0x04, self.r(rs), self.r(rt), name)))
    def bne(self, rs, rt, name): self.items.append(('branch', (0x05, self.r(rs), self.r(rt), name)))
    def j(self, name): self.items.append(('jump', (0x02, name)))
    def jal(self, name): self.items.append(('jump', (0x03, name)))

    def assemble(self):
        out = bytearray()
        for kind, value in self.items:
            at = self.origin + len(out)
            if kind == 'bytes':
                out += value
                continue
            if kind == 'word':
                words = [value]
            elif kind == 'i':
                op, rs, rt, imm = value
                words = [op << 26 | rs << 21 | rt << 16 | imm & 0xFFFF]
            elif kind == 'branch':
                op, rs, rt, name = value
                words = [op << 26 | rs << 21 | rt << 16 | (self.labels[name] - (at + 4)) >> 2 & 0xFFFF]
            elif kind == 'jump':
                op, name = value
                words = [op << 26 | (self.labels[name] >> 2) & 0x3FFFFFF]
            else:
                rt, name = value
                address = self.labels[name]
                words = [0x0F << 26 | rt << 16 | (address + 0x8000) >> 16 & 0xFFFF,
                         0x09 << 26 | rt << 21 | rt << 16 | address & 0xFFFF]
            out += b''.join(struct.pack('<I', word) for word in words)
        return bytes(out)


# The pad program's record in main RAM (pad_program): its magic, then the vertical blanks it counted,
# each port's last reply, how often it locked and unlocked port 1's analog mode, and the SPU control
# value it wrote last.
PAD_RECORD = 0x80100000
PAD_MAGIC = b'PADTEST1'
PAD_PORT = (0x10, 0x20)
PAD_LOCKS = 0x30
PAD_UNLOCKS = 0x34
PAD_SPUCNT = 0x38
# The PS1 buttons the pad program acts on, in the controller's bit order: Triangle locks port 1 in digital
# mode, Square puts it in analog mode and unlocks it, and Circle turns on the SPU's interrupt, which
# changes how often SwanStation runs its sound unit (netplay.md "Determinism").
PAD_TRIANGLE, PAD_CIRCLE, PAD_SQUARE = 1 << 12, 1 << 13, 1 << 15


def pad_program():
    """A PS-EXE that reads both controller ports once a frame and shows what it read, so a test can tell
    that input, analog mode and the game's analog lock reach the console the same way on every machine.

    With interrupts off, it waits for each vertical blank (I_STAT bit 0), then asks each port for its
    buttons (0x01 0x42, nine bytes through the controller port's registers at 0x1F801040) and keeps the
    replies in its record at PAD_RECORD: the ID byte (0x41 digital, 0x73 analog), 0x5A, the two button
    bytes (a pressed button is 0) and, in analog mode, the right and left sticks. When port 1 holds
    Triangle it sends that controller the DualShock's configuration commands to put it in digital mode
    and lock it there (0x43 0x01, 0x44 0x00 0x03, 0x43 0x00); with Square, analog mode and unlocked (0x44
    0x01 0x02). Holding Circle turns the SPU's interrupt on (SPUCNT 0x8040), and letting go turns it off
    (0x8000). Then it draws each port's 16 buttons as squares (white when held), its mode as a bar
    (blue digital, green analog, red for no controller) and its four stick bytes as bars."""
    a = Assembler(0x80010000)
    io = 's0'
    a.li('t0', 0)
    a.mtc0('t0', 12)
    a.li(io, 0x1F801000)
    a.sw('zero', 0x074, io)
    a.li('s1', PAD_RECORD)
    a.li('t0', struct.unpack('<I', PAD_MAGIC[:4])[0])
    a.sw('t0', 0, 's1')
    a.li('t0', struct.unpack('<I', PAD_MAGIC[4:])[0])
    a.sw('t0', 4, 's1')
    for offset in (8, PAD_LOCKS, PAD_UNLOCKS):
        a.sw('zero', offset, 's1')
    a.li('s2', 0)
    for value in (0x00000000, 0x08000001, 0x05000000, 0x06C60260, 0x07040010, 0x03000000):
        a.li('t1', value)
        a.sw('t1', 0x814, io)
    a.li('t0', 0x40)
    a.sh('t0', 0x04A, io)
    a.li('t0', 0x000D)
    a.sh('t0', 0x048, io)
    a.li('t0', 0x0088)
    a.sh('t0', 0x04E, io)
    a.sh('zero', 0x04A, io)
    a.li('t0', 0x8000)
    a.sh('t0', 0xDAA, io)

    a.label('frame')
    a.label('vblank')
    a.lw('t0', 0x070, io)
    a.nop()
    a.andi('t0', 't0', 1)
    a.beq('t0', 'zero', 'vblank')
    a.nop()
    a.li('t0', 0xFFFFFFFE)
    a.sw('t0', 0x070, io)
    a.addiu('s2', 's2', 1)
    a.sw('s2', 8, 's1')

    def transfer(port, command, reply):
        a.li('a0', port)
        a.la('a1', command)
        a.li('a2', reply)
        a.jal('transfer')
        a.nop()

    transfer(0, 'read', PAD_RECORD + PAD_PORT[0])
    transfer(1, 'read', PAD_RECORD + PAD_PORT[1])
    a.lbu('t0', PAD_PORT[0] + 3, 's1')
    a.lbu('t1', PAD_PORT[0] + 4, 's1')
    a.nop()
    a.sll('t1', 't1', 8)
    a.or_('t0', 't0', 't1')
    a.nor('s3', 't0', 'zero')
    for button, mode, counter, skip in ((PAD_TRIANGLE, 'digital_lock', PAD_LOCKS, 'no_lock'),
                                        (PAD_SQUARE, 'analog_unlock', PAD_UNLOCKS, 'no_unlock')):
        a.andi('t0', 's3', button)
        a.beq('t0', 'zero', skip)
        a.nop()
        for command in ('config_on', mode, 'config_off'):
            transfer(0, command, PAD_RECORD + 0x40)
        a.lw('t0', counter, 's1')
        a.nop()
        a.addiu('t0', 't0', 1)
        a.sw('t0', counter, 's1')
        a.label(skip)
    a.andi('t0', 's3', PAD_CIRCLE)
    a.srl('t0', 't0', 7)
    a.ori('t0', 't0', 0x8000)
    a.sh('t0', 0xDAA, io)
    a.sw('t0', PAD_SPUCNT, 's1')

    def fill(colour, x, y, width, height):
        a.li('a0', 0x02000000 | colour)
        a.li('a1', y << 16 | x)
        a.li('a2', height << 16 | width)
        a.jal('fill')
        a.nop()

    fill(0x202020, 0, 0, 320, 240)
    for port, top in ((0, 24), (1, 128)):
        reply = PAD_PORT[port]
        a.lbu('t0', reply + 3, 's1')
        a.lbu('t1', reply + 4, 's1')
        a.nop()
        a.sll('t1', 't1', 8)
        a.or_('t0', 't0', 't1')
        a.nor('s4', 't0', 'zero')
        for bit in range(16):
            a.srl('t0', 's4', bit)
            a.andi('t0', 't0', 1)
            a.li('a0', 0x02404040)
            a.beq('t0', 'zero', f'up_{port}_{bit}')
            a.nop()
            a.li('a0', 0x02FFFFFF)
            a.label(f'up_{port}_{bit}')
            a.li('a1', (top + bit // 8 * 20) << 16 | 32 + bit % 8 * 32)
            a.li('a2', 16 << 16 | 16)
            a.jal('fill')
            a.nop()
        a.lbu('t0', reply + 1, 's1')
        a.nop()
        a.li('a0', 0x020000FF)
        a.li('t1', 0x41)
        a.bne('t0', 't1', f'not_digital_{port}')
        a.nop()
        a.li('a0', 0x02FF0000)
        a.label(f'not_digital_{port}')
        a.li('t1', 0x73)
        a.bne('t0', 't1', f'not_analog_{port}')
        a.nop()
        a.li('a0', 0x0200FF00)
        a.label(f'not_analog_{port}')
        a.li('a1', (top + 44) << 16 | 32)
        a.li('a2', 8 << 16 | 256)
        a.jal('fill')
        a.nop()
        for axis in range(4):
            a.lbu('t0', reply + 5 + axis, 's1')
            a.nop()
            a.addiu('t0', 't0', 16)
            a.li('a2', 6 << 16)
            a.or_('a2', 'a2', 't0')
            a.li('a0', 0x0200C0C0)
            a.li('a1', (top + 56 + axis * 10) << 16 | 32)
            a.jal('fill')
            a.nop()
    a.j('frame')
    a.nop()

    # transfer(a0 port, a1 nine bytes to send, a2 where the nine bytes received go): one exchange with the
    # controller in that port, selected through JOY_CTRL (transmit on, /JOYn out, port 2 by bit 13). Each byte
    # is written to JOY_DATA, and its answer read once JOY_STAT says one has come.
    a.label('transfer')
    a.sll('t0', 'a0', 13)
    a.ori('t0', 't0', 0x0003)
    a.sh('t0', 0x04A, io)
    a.li('t3', 9)
    a.label('transfer_byte')
    a.lbu('t4', 0, 'a1')
    a.nop()
    a.sb('t4', 0x040, io)
    a.label('transfer_wait')
    a.lw('t5', 0x044, io)
    a.nop()
    a.andi('t5', 't5', 2)
    a.beq('t5', 'zero', 'transfer_wait')
    a.nop()
    a.lbu('t4', 0x040, io)
    a.nop()
    a.sb('t4', 0, 'a2')
    a.addiu('a1', 'a1', 1)
    a.addiu('a2', 'a2', 1)
    a.addiu('t3', 't3', -1)
    a.bne('t3', 'zero', 'transfer_byte')
    a.nop()
    a.sh('zero', 0x04A, io)
    a.jr('ra')
    a.nop()

    # fill(a0 GP0 fill command and colour, a1 top-left, a2 size): one GP0 fill rectangle, written once
    # GPUSTAT says the GPU has finished the last command and its FIFO is empty (bit 26).
    a.label('fill')
    a.lw('t0', 0x814, io)
    a.nop()
    a.srl('t0', 't0', 26)
    a.andi('t0', 't0', 1)
    a.beq('t0', 'zero', 'fill')
    a.nop()
    for register in ('a0', 'a1', 'a2'):
        a.sw(register, 0x810, io)
    a.jr('ra')
    a.nop()

    for name, command in (('read', [0x01, 0x42]), ('config_on', [0x01, 0x43, 0x00, 0x01]),
                          ('digital_lock', [0x01, 0x44, 0x00, 0x00, 0x03]),
                          ('analog_unlock', [0x01, 0x44, 0x00, 0x01, 0x02]),
                          ('config_off', [0x01, 0x43, 0x00, 0x00])):
        a.label(name)
        a.data(bytes(command).ljust(9, b'\0'))
    return ps_exe(a.assemble())


def build(program, bin_path, cue_path):
    """program is a PS-EXE file's path or the PS-EXE itself."""
    exe = program if isinstance(program, bytes) else Path(program).read_bytes()
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
    parser.add_argument('program', help='The PS-EXE to put on the disc, or with --pad-program the file to write')
    parser.add_argument('cue', nargs='?', help='The .cue to write; the .bin goes beside it')
    parser.add_argument('--pad-program', action='store_true', help='Write the pad program (pad_program) to program')
    options = parser.parse_args()
    if options.pad_program:
        Path(options.program).write_bytes(pad_program())
    elif options.cue:
        cue = Path(options.cue)
        build(options.program, cue.with_suffix('.bin'), cue)
    else:
        parser.error('a disc needs a .cue to write')
