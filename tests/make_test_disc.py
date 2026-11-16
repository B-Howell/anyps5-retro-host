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


def corner_program(pal=False):
    """A PS-EXE that fills the 320x240 screen blue and the 64x32 block at its top-left corner red,
    then waits forever, so a test can tell which way up a picture is. With pal it sets a PAL video
    mode (GP1(08h) bit 3), so the console runs at PAL's 50 frames a second. Each GPU port write (GP1 at
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
    # GP1: reset, 320x240 NTSC (or PAL) 15-bit, display start 0,0, standard ranges, display on.
    for value in (0x00000000, 0x08000009 if pal else 0x08000001, 0x05000000, 0x06C60260, 0x07040010, 0x03000000):
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
    def sltiu(self, rt, rs, imm): self.i_type(0x0B, rs, rt, imm)
    def sll(self, rd, rt, shift): self.r_type(0x00, rt=rt, rd=rd, shift=shift)
    def srl(self, rd, rt, shift): self.r_type(0x02, rt=rt, rd=rd, shift=shift)
    def and_(self, rd, rs, rt): self.r_type(0x24, rs, rt, rd)
    def or_(self, rd, rs, rt): self.r_type(0x25, rs, rt, rd)
    def xor(self, rd, rs, rt): self.r_type(0x26, rs, rt, rd)
    def nor(self, rd, rs, rt): self.r_type(0x27, rs, rt, rd)
    def addu(self, rd, rs, rt): self.r_type(0x21, rs, rt, rd)
    def subu(self, rd, rs, rt): self.r_type(0x23, rs, rt, rd)
    def sltu(self, rd, rs, rt): self.r_type(0x2B, rs, rt, rd)
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


# The controller commands the programs send, each padded to the nine bytes a transfer exchanges: read the
# buttons, enter and leave the DualShock's configuration mode, and set digital mode locked or analog mode
# unlocked.
PAD_COMMANDS = (('read', [0x01, 0x42]), ('config_on', [0x01, 0x43, 0x00, 0x01]),
                ('digital_lock', [0x01, 0x44, 0x00, 0x00, 0x03]),
                ('analog_unlock', [0x01, 0x44, 0x00, 0x01, 0x02]),
                ('config_off', [0x01, 0x43, 0x00, 0x00]))
PAD_CROSS = 1 << 14


def program_start(a, io, record, magic):
    """Turns interrupts off (COP0 SR 0, I_MASK 0), points io at the I/O ports and s1 at the program's
    record in main RAM, and writes the record's 8-byte magic."""
    a.li('t0', 0)
    a.mtc0('t0', 12)
    a.li(io, 0x1F801000)
    a.sw('zero', 0x074, io)
    a.li('s1', record)
    a.li('t0', struct.unpack('<I', magic[:4])[0])
    a.sw('t0', 0, 's1')
    a.li('t0', struct.unpack('<I', magic[4:])[0])
    a.sw('t0', 4, 's1')


def console_start(a, io):
    """GP1: reset, 320x240 NTSC 15-bit, display start 0,0, standard ranges, display on; and the controller
    port's baud rate, mode and control set for transfers (JOY_BAUD 0x88, JOY_MODE 0x0D)."""
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


def next_frame(a, io):
    """Waits for the next vertical blank (I_STAT bit 0), acknowledges it and counts it in s2 and at the
    record's offset 8."""
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


def call_transfer(a, port, command, reply):
    """Calls transfer: sends the command labelled command to the controller in port (0 or 1) and keeps
    the nine bytes it answers at reply."""
    a.li('a0', port)
    a.la('a1', command)
    a.li('a2', reply)
    a.jal('transfer')
    a.nop()


def pressed(a, register, reply):
    """Sets register to the buttons held in the reply kept at the record's offset reply, one bit each in
    the controller's order (a set bit is a held button)."""
    a.lbu('t0', reply + 3, 's1')
    a.lbu('t1', reply + 4, 's1')
    a.nop()
    a.sll('t1', 't1', 8)
    a.or_('t0', 't0', 't1')
    a.nor(register, 't0', 'zero')


def call_fill(a, colour, x, y, width, height):
    """Calls fill: a GP0 fill rectangle of colour (0xBBGGRR) at x, y."""
    a.li('a0', 0x02000000 | colour)
    a.li('a1', y << 16 | x)
    a.li('a2', height << 16 | width)
    a.jal('fill')
    a.nop()


def gp0(a, io, *words):
    """Writes one GP0 command, each word a number or a register's name, once GPUSTAT says the GPU has
    finished the last command and its FIFO is empty (gp0_ready)."""
    a.jal('gp0_ready')
    a.nop()
    for word in words:
        if isinstance(word, str):
            a.sw(word, 0x810, io)
        else:
            a.li('t1', word)
            a.sw('t1', 0x810, io)


def subroutines(a, io, commands, ready=False):
    """The subroutines and data the programs call: transfer, fill, the controller commands, and with
    ready gp0_ready."""
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

    if ready:
        # gp0_ready: returns once GPUSTAT bit 26 says the GPU takes a command.
        a.label('gp0_ready')
        a.lw('t0', 0x814, io)
        a.nop()
        a.srl('t0', 't0', 26)
        a.andi('t0', 't0', 1)
        a.beq('t0', 'zero', 'gp0_ready')
        a.nop()
        a.jr('ra')
        a.nop()

    for name, command in commands:
        a.label(name)
        a.data(bytes(command).ljust(9, b'\0'))


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
    program_start(a, io, PAD_RECORD, PAD_MAGIC)
    for offset in (8, PAD_LOCKS, PAD_UNLOCKS):
        a.sw('zero', offset, 's1')
    a.li('s2', 0)
    console_start(a, io)
    a.li('t0', 0x8000)
    a.sh('t0', 0xDAA, io)

    a.label('frame')
    next_frame(a, io)

    def transfer(port, command, reply):
        call_transfer(a, port, command, reply)

    transfer(0, 'read', PAD_RECORD + PAD_PORT[0])
    transfer(1, 'read', PAD_RECORD + PAD_PORT[1])
    pressed(a, 's3', PAD_PORT[0])
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
        call_fill(a, colour, x, y, width, height)

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

    subroutines(a, io, PAD_COMMANDS)
    return ps_exe(a.assemble())


# The kept-video-memory program's record in main RAM (vram_program): its magic, the vertical blanks it
# counted, and each port's last reply.
VRAM_RECORD = 0x80100000
VRAM_MAGIC = b'VRAMTST1'
VRAM_PORT = (0x10, 0x20)
# Where the kept-video-memory program keeps its textures in video memory, beside the 320x240 screen: a
# 64x64 texture uploaded once at boot, and to its right a 64x64 area it copies what it drew into.
VRAM_TEXTURE = (512, 0)
VRAM_COPIES = (576, 0)


def vram_program():
    """A PS-EXE whose picture is built up over many frames from video memory it drew once and kept, so a
    test can tell that a rollback brings back all of video memory as it was, not only the screen.

    At boot it uploads a 64x64 texture to video memory beside the screen (GP0(A0h), a pattern of each
    texel's coordinates) and fills the 64x64 area to its right, then never clears the screen. Each frame
    it reads both controllers (as pad_program does) and draws one 16x16 rectangle textured from that
    page (GP0(65h), 15-bit texels): its column comes from the buttons held on both ports and its row from
    the frame, and the texels it samples from both ports' buttons, so a wrong prediction draws somewhere
    else and with other texels. Square held on port 1 sets the mask bit on what it draws, and every draw
    checks the mask bit (GP0(E6h)), so a masked cell is kept until a fill clears it. Then it copies the
    cell it drew into the area beside the texture (GP0(80h)), which later frames sample from, and fills
    another cell with grey, which also clears its mask bits."""
    a = Assembler(0x80010000)
    io = 's0'
    program_start(a, io, VRAM_RECORD, VRAM_MAGIC)
    a.sw('zero', 8, 's1')
    a.li('s2', 0)
    a.li('s6', 0)
    console_start(a, io)
    # GP0: texture page at VRAM_TEXTURE with 15-bit texels, drawing to the displayed area allowed; no
    # texture window; the drawing area all of video memory; no offset; no mask bit set or checked.
    tx, ty = VRAM_TEXTURE
    cx, cy = VRAM_COPIES
    for word in (0xE1000400 | 2 << 7 | ty // 256 << 4 | tx // 64, 0xE2000000, 0xE3000000,
                 0xE4000000 | 511 << 10 | 1023, 0xE5000000, 0xE6000000):
        gp0(a, io, word)
    call_fill(a, 0x202020, 0, 0, 320, 240)
    call_fill(a, 0x800000, cx, cy, 64, 64)
    # GP0(A0h): the 64x64 texture, two texels a word, each word written once GPUSTAT says the GPU takes
    # one (bit 28). Texel (u, v) is u | v << 6 | ((u ^ v) & 7) << 12.
    gp0(a, io, 0xA0000000, ty << 16 | tx, 64 << 16 | 64)
    a.li('t3', 0)
    a.label('upload_row')
    a.li('t4', 0)
    a.label('upload_pair')
    a.lw('t0', 0x814, io)
    a.nop()
    a.srl('t0', 't0', 28)
    a.andi('t0', 't0', 1)
    a.beq('t0', 'zero', 'upload_pair')
    a.nop()
    a.addiu('a1', 't4', 1)
    for texel, u in (('t5', 't4'), ('t6', 'a1')):
        a.sll('t8', 't3', 6)
        a.or_(texel, u, 't8')
        a.xor('t9', u, 't3')
        a.andi('t9', 't9', 7)
        a.sll('t9', 't9', 12)
        a.or_(texel, texel, 't9')
    a.sll('t6', 't6', 16)
    a.or_('t5', 't5', 't6')
    a.sw('t5', 0x810, io)
    a.addiu('t4', 't4', 2)
    a.li('t7', 64)
    a.bne('t4', 't7', 'upload_pair')
    a.nop()
    a.addiu('t3', 't3', 1)
    a.bne('t3', 't7', 'upload_row')
    a.nop()

    a.label('frame')
    next_frame(a, io)
    call_transfer(a, 0, 'read', VRAM_RECORD + VRAM_PORT[0])
    call_transfer(a, 1, 'read', VRAM_RECORD + VRAM_PORT[1])
    pressed(a, 's3', VRAM_PORT[0])
    pressed(a, 's4', VRAM_PORT[1])
    # The cell: column (port 1 ^ port 2 >> 4) & 15, at 32 + 16 * column across; row s6 (0 to 14) down.
    a.srl('t0', 's4', 4)
    a.xor('t0', 't0', 's3')
    a.andi('t0', 't0', 15)
    a.sll('t0', 't0', 4)
    a.addiu('t0', 't0', 32)
    a.sll('t1', 's6', 20)
    a.or_('s5', 't1', 't0')
    # The texels: u from port 1's buttons 4 to 6 and v from port 2's buttons 0 and 1, 16 at a time, so u
    # reaches both the uploaded texture (0 to 63) and the copies beside it (64 to 127).
    a.srl('t0', 's3', 4)
    a.andi('t0', 't0', 7)
    a.sll('t0', 't0', 4)
    a.andi('t1', 's4', 3)
    a.sll('t1', 't1', 12)
    a.or_('a2', 't0', 't1')
    a.andi('t0', 's3', PAD_SQUARE)
    a.li('a0', 0xE6000002)
    a.beq('t0', 'zero', 'unmasked')
    a.nop()
    a.li('a0', 0xE6000003)
    a.label('unmasked')
    gp0(a, io, 'a0')
    gp0(a, io, 0x65808080, 's5', 'a2', 16 << 16 | 16)
    # The copy beside the texture, into one of its 16 cells by the frame count, mask bits neither
    # checked nor set, so the copy keeps the cell's own.
    gp0(a, io, 0xE6000000)
    a.andi('t0', 's2', 3)
    a.sll('t0', 't0', 4)
    a.addiu('t0', 't0', cx)
    a.srl('t1', 's2', 2)
    a.andi('t1', 't1', 3)
    a.sll('t1', 't1', 20)
    a.or_('a3', 't0', 't1')
    gp0(a, io, 0x80000000, 's5', 'a3', 16 << 16 | 16)
    # The cell filled with grey: column 7 * frame & 15, row s6 + 7 (mod 15).
    a.sll('t0', 's2', 3)
    a.subu('t0', 't0', 's2')
    a.andi('t0', 't0', 15)
    a.sll('t0', 't0', 4)
    a.addiu('t0', 't0', 32)
    a.addiu('t1', 's6', 7)
    a.sltiu('t2', 't1', 15)
    a.bne('t2', 'zero', 'clear_row')
    a.nop()
    a.addiu('t1', 't1', -15)
    a.label('clear_row')
    a.sll('t1', 't1', 20)
    a.or_('a1', 't1', 't0')
    a.li('a0', 0x02303030)
    a.li('a2', 16 << 16 | 16)
    a.jal('fill')
    a.nop()
    a.addiu('s6', 's6', 1)
    a.li('t0', 15)
    a.bne('s6', 't0', 'same_row')
    a.nop()
    a.li('s6', 0)
    a.label('same_row')
    a.j('frame')
    a.nop()
    subroutines(a, io, PAD_COMMANDS[:1], ready=True)
    return ps_exe(a.assemble())


# The disc-reading program's record in main RAM (disc_program): its magic, the vertical blanks it counted,
# each port's last reply, the sectors it read, the first word of the last one (its LBA, as disc_data
# writes it), a checksum of every byte read, the CD-ROM errors (INT5) it was told of, the last response
# byte, its phase (0 setting the mode, 1 seeking, 2 starting to read, 3 reading) and the CD-ROM
# interrupts it took. It keeps the last four sectors read at DISC_BUFFER.
DISC_RECORD = 0x80100000
DISC_MAGIC = b'CDTEST01'
DISC_PORT = (0x10, 0x20)
DISC_SECTORS, DISC_LAST_LBA, DISC_CHECKSUM, DISC_ERRORS = 0x30, 0x34, 0x38, 0x3C
DISC_RESPONSE, DISC_PHASE, DISC_INTERRUPTS = 0x40, 0x44, 0x48
DISC_BUFFER = 0x80110000
# Where disc_data's sectors go on the disc (build's data), how many, and the 64 places the program seeks
# to, DISC_STRIDE sectors apart from the first.
DISC_DATA_LBA = 64
DISC_DATA_SECTORS = 256
DISC_STRIDE = 3


def disc_data():
    """DISC_DATA_SECTORS sectors of 2048 bytes, each different: word 0 of sector n is its LBA
    (DISC_DATA_LBA + n) and word i after it (lba * 0x9E3779B1 + i * 0x85EBCA77) mod 2 ** 32."""
    out = bytearray()
    for n in range(DISC_DATA_SECTORS):
        lba = DISC_DATA_LBA + n
        out += struct.pack('<I', lba)
        out += b''.join(struct.pack('<I', (lba * 0x9E3779B1 + i * 0x85EBCA77) & 0xFFFFFFFF) for i in range(1, 512))
    return bytes(out)


def disc_program():
    """A PS-EXE that reads the disc all the time it runs, through the CD-ROM controller's registers at
    0x1F801800 with its interrupts polled, so a test can tell that a rollback puts the drive back where
    it was: its position, the sectors it had read and the commands under way.

    It sets double speed with 2048-byte sectors (Setmode 0x80), seeks to disc_data's first sector
    (Setloc) and reads on (ReadN), one command at a time, each sent once the last was acknowledged
    (INT3). Once a frame, after reading both controllers, it takes up to four CD-ROM interrupts: for each
    sector (INT1) it reads all 2048 bytes from the data FIFO, keeps them, adds them to its checksum and
    keeps the sector's first word, the LBA it was read from. A new press of Cross on port 1 seeks again,
    to one of 64 places port 2's buttons pick, and so does reading near the end of disc_data; an error
    (INT5) seeks again to the same place. It draws a bar as long as the sectors read modulo 16."""
    a = Assembler(0x80010000)
    io = 's0'
    program_start(a, io, DISC_RECORD, DISC_MAGIC)
    for offset in (8, DISC_SECTORS, DISC_LAST_LBA, DISC_CHECKSUM, DISC_ERRORS, DISC_RESPONSE, DISC_PHASE,
                   DISC_INTERRUPTS):
        a.sw('zero', offset, 's1')
    for register in ('s2', 's5', 's6', 's7', 'fp'):
        a.li(register, 0)
    console_start(a, io)
    # Every CD-ROM interrupt flagged and acknowledged (index 1: 0x1F801802 enable, 0x1F801803 flags).
    a.li('t0', 1)
    a.sb('t0', 0x800, io)
    a.li('t0', 0x1F)
    a.sb('t0', 0x802, io)
    a.sb('t0', 0x803, io)

    a.label('frame')
    next_frame(a, io)
    call_transfer(a, 0, 'read', DISC_RECORD + DISC_PORT[0])
    call_transfer(a, 1, 'read', DISC_RECORD + DISC_PORT[1])
    pressed(a, 's3', DISC_PORT[0])
    pressed(a, 's4', DISC_PORT[1])
    # s5 holds port 1's buttons of the frame before; Cross pressed since then seeks, once the mode is set.
    a.nor('t0', 's5', 'zero')
    a.and_('t0', 't0', 's3')
    a.andi('t0', 't0', PAD_CROSS)
    a.or_('s5', 's3', 'zero')
    a.beq('t0', 'zero', 'no_seek')
    a.nop()
    a.beq('s6', 'zero', 'no_seek')
    a.nop()
    a.srl('t0', 's4', 2)
    a.andi('t0', 't0', 63)
    a.sll('fp', 't0', 2)
    a.li('s6', 1)
    a.label('no_seek')
    a.li('t0', 3)
    a.bne('s6', 't0', 'no_wrap')
    a.nop()
    a.lw('t1', DISC_LAST_LBA, 's1')
    a.li('t2', DISC_DATA_LBA + DISC_DATA_SECTORS - 16)
    a.sltu('t2', 't1', 't2')
    a.bne('t2', 'zero', 'no_wrap')
    a.nop()
    a.li('fp', 0)
    a.li('s6', 1)
    a.label('no_wrap')
    for _ in range(4):
        a.jal('cd_poll')
        a.nop()
    # The next command, when none is waiting for its acknowledgement and the controller is not busy
    # (status bit 7): Setmode, Setloc to the place fp names, or ReadN.
    a.bne('s7', 'zero', 'no_command')
    a.nop()
    a.li('t0', 3)
    a.beq('s6', 't0', 'no_command')
    a.nop()
    a.lbu('t0', 0x800, io)
    a.nop()
    a.andi('t0', 't0', 0x80)
    a.bne('t0', 'zero', 'no_command')
    a.nop()
    a.sb('zero', 0x800, io)
    a.beq('s6', 'zero', 'send_mode')
    a.nop()
    a.li('t0', 1)
    a.beq('s6', 't0', 'send_setloc')
    a.nop()
    a.li('t0', 0x06)
    a.sb('t0', 0x801, io)
    a.beq('zero', 'zero', 'sent')
    a.nop()
    a.label('send_mode')
    a.li('t0', 0x80)
    a.sb('t0', 0x802, io)
    a.li('t0', 0x0E)
    a.sb('t0', 0x801, io)
    a.beq('zero', 'zero', 'sent')
    a.nop()
    a.label('send_setloc')
    a.la('t1', 'places')
    a.addu('t1', 't1', 'fp')
    for index in range(3):
        a.lbu('t2', index, 't1')
        a.nop()
        a.sb('t2', 0x802, io)
    a.li('t0', 0x02)
    a.sb('t0', 0x801, io)
    a.label('sent')
    a.li('s7', 1)
    a.label('no_command')
    a.sw('s6', DISC_PHASE, 's1')
    call_fill(a, 0x202020, 0, 0, 320, 240)
    a.lw('t0', DISC_SECTORS, 's1')
    a.nop()
    a.andi('t0', 't0', 15)
    a.addiu('t0', 't0', 1)
    a.sll('t0', 't0', 4)
    a.li('a2', 16 << 16)
    a.or_('a2', 'a2', 't0')
    a.li('a0', 0x0200C000)
    a.li('a1', 112 << 16 | 32)
    a.jal('fill')
    a.nop()
    a.j('frame')
    a.nop()

    # cd_poll: takes one CD-ROM interrupt, if one is flagged (index 1, 0x1F801803 bits 0 to 2). Reads the
    # response FIFO while it holds bytes (status bit 5), then for INT1 reads the sector, and acknowledges
    # the interrupt. INT3 acknowledges the command sent (s7), which moves the phase (s6) on until it
    # reads; INT5 counts an error and seeks again.
    a.label('cd_poll')
    a.li('t0', 1)
    a.sb('t0', 0x800, io)
    a.lbu('t1', 0x803, io)
    a.nop()
    a.andi('t1', 't1', 7)
    a.beq('t1', 'zero', 'poll_done')
    a.nop()
    a.lw('t2', DISC_INTERRUPTS, 's1')
    a.nop()
    a.addiu('t2', 't2', 1)
    a.sw('t2', DISC_INTERRUPTS, 's1')
    a.label('poll_response')
    a.lbu('t2', 0x800, io)
    a.nop()
    a.andi('t2', 't2', 0x20)
    a.beq('t2', 'zero', 'poll_responded')
    a.nop()
    a.lbu('t3', 0x801, io)
    a.nop()
    a.sb('t3', DISC_RESPONSE, 's1')
    a.j('poll_response')
    a.nop()
    a.label('poll_responded')
    a.li('t0', 1)
    a.beq('t1', 't0', 'poll_data')
    a.nop()
    a.li('t0', 1)
    a.sb('t0', 0x800, io)
    a.li('t0', 0x1F)
    a.sb('t0', 0x803, io)
    a.li('t0', 3)
    a.beq('t1', 't0', 'poll_ack')
    a.nop()
    a.li('t0', 5)
    a.beq('t1', 't0', 'poll_error')
    a.nop()
    a.jr('ra')
    a.nop()
    a.label('poll_ack')
    a.beq('s7', 'zero', 'poll_done')
    a.nop()
    a.li('s7', 0)
    a.li('t0', 3)
    a.beq('s6', 't0', 'poll_done')
    a.nop()
    a.addiu('s6', 's6', 1)
    a.jr('ra')
    a.nop()
    a.label('poll_error')
    a.lw('t2', DISC_ERRORS, 's1')
    a.nop()
    a.addiu('t2', 't2', 1)
    a.sw('t2', DISC_ERRORS, 's1')
    a.li('s7', 0)
    a.beq('s6', 'zero', 'poll_done')
    a.nop()
    a.li('s6', 1)
    a.jr('ra')
    a.nop()
    # INT1: ask for the sector (index 0, 0x1F801803 bit 7), wait for the data FIFO (status bit 6), read
    # its 2048 bytes into one of four buffers, then acknowledge.
    a.label('poll_data')
    a.sb('zero', 0x800, io)
    a.li('t0', 0x80)
    a.sb('t0', 0x803, io)
    a.li('t4', 0x1000)
    a.label('poll_drq')
    a.lbu('t2', 0x800, io)
    a.nop()
    a.andi('t2', 't2', 0x40)
    a.bne('t2', 'zero', 'poll_read')
    a.nop()
    a.addiu('t4', 't4', -1)
    a.bne('t4', 'zero', 'poll_drq')
    a.nop()
    a.beq('zero', 'zero', 'poll_acknowledge')
    a.nop()
    a.label('poll_read')
    a.lw('t2', DISC_SECTORS, 's1')
    a.nop()
    a.andi('t3', 't2', 3)
    a.sll('t3', 't3', 11)
    a.li('t5', DISC_BUFFER)
    a.addu('t5', 't5', 't3')
    a.lw('t6', DISC_CHECKSUM, 's1')
    a.li('t4', 2048)
    a.label('poll_byte')
    a.lbu('t3', 0x802, io)
    a.nop()
    a.sb('t3', 0, 't5')
    a.sll('t7', 't6', 5)
    a.addu('t6', 't7', 't6')
    a.addu('t6', 't6', 't3')
    a.addiu('t5', 't5', 1)
    a.addiu('t4', 't4', -1)
    a.bne('t4', 'zero', 'poll_byte')
    a.nop()
    a.sw('t6', DISC_CHECKSUM, 's1')
    a.addiu('t2', 't2', 1)
    a.sw('t2', DISC_SECTORS, 's1')
    a.lw('t3', -2048, 't5')
    a.nop()
    a.sw('t3', DISC_LAST_LBA, 's1')
    a.sb('zero', 0x803, io)
    a.label('poll_acknowledge')
    a.li('t0', 1)
    a.sb('t0', 0x800, io)
    a.li('t0', 0x1F)
    a.sb('t0', 0x803, io)
    a.label('poll_done')
    a.jr('ra')
    a.nop()
    subroutines(a, io, PAD_COMMANDS[:1])
    # Setloc's parameters for each place: minute, second and frame in BCD of its position (LBA + 150),
    # padded to four bytes.
    a.label('places')
    for index in range(64):
        position = DISC_DATA_LBA + index * DISC_STRIDE + PREGAP
        a.data(bytes([bcd(position // 4500), bcd(position // 75 % 60), bcd(position % 75), 0]))
    return ps_exe(a.assemble())


def build(program, bin_path, cue_path, data=None):
    """program is a PS-EXE file's path or the PS-EXE itself. data, when given, goes on the disc as
    DATA.BIN from sector DISC_DATA_LBA on (disc_program reads disc_data there)."""
    exe = program if isinstance(program, bytes) else Path(program).read_bytes()
    if exe[:8] != b'PS-X EXE':
        raise SystemExit(f'{program} is not a PS-EXE')
    config = b'BOOT = cdrom:\\PROGRAM.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'
    root_lba, config_lba, exe_lba = 22, 23, 24
    exe_sectors = -(-len(exe) // SECTOR)
    total = exe_lba + exe_sectors
    files = record(b'PROGRAM.EXE;1', exe_lba, len(exe), False) + record(b'SYSTEM.CNF;1', config_lba, len(config), False)
    if data is not None:
        if total > DISC_DATA_LBA or len(data) % SECTOR:
            raise SystemExit(f'data must be whole sectors, after a program of at most {DISC_DATA_LBA - exe_lba} sectors')
        files = record(b'DATA.BIN;1', DISC_DATA_LBA, len(data), False) + files
        total = DISC_DATA_LBA + len(data) // SECTOR
    root = record(b'\x00', root_lba, SECTOR, True) + record(b'\x01', root_lba, SECTOR, True) + files
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
    for index in range(len(data or b'') // SECTOR):
        last = index == len(data) // SECTOR - 1
        sectors[DISC_DATA_LBA + index] = (data[index * SECTOR:(index + 1) * SECTOR], SUBMODE_END if last else SUBMODE_DATA)
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
    parser.add_argument('--vram-program', action='store_true',
                        help='Write the kept-video-memory program (vram_program) to program')
    parser.add_argument('--disc-program', action='store_true',
                        help='Write the disc-reading program (disc_program) to program; its disc needs --disc-data')
    parser.add_argument('--disc-data', action='store_true', help='Put disc_data on the disc as DATA.BIN')
    options = parser.parse_args()
    if options.pad_program:
        Path(options.program).write_bytes(pad_program())
    elif options.vram_program:
        Path(options.program).write_bytes(vram_program())
    elif options.disc_program:
        Path(options.program).write_bytes(disc_program())
    elif options.cue:
        cue = Path(options.cue)
        build(options.program, cue.with_suffix('.bin'), cue, disc_data() if options.disc_data else None)
    else:
        parser.error('a disc needs a .cue to write')
