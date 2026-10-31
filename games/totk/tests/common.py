"""Shared helpers for the TotK patch verifiers: module images are mapped at offset 0 (main+X = address X) with the
data page at main+DATA_PAGE_OFFSET; ADRP is page relative, so the words behave as at the real 2 MiB-aligned base."""
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, arm64_const

from games.totk.patches import DATA_PAGE_OFFSET, MAIN_SPAN

DATA = DATA_PAGE_OFFSET
STACK, FRAME = 0x70000000, 0x70008000


def adrp_ldr(image, pc, ldr_pc):
    """The address an adrp + ldr x pair at pc / ldr_pc loads from."""
    a = struct.unpack_from('<I', image, pc)[0]
    b = struct.unpack_from('<I', image, ldr_pc)[0]
    assert a & 0x9f000000 == 0x90000000 and b & 0xffc00000 == 0xf9400000 and a & 31 == (b >> 5) & 31
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    return (pc & ~4095) + (imm << 12) + ((b >> 10) & 4095) * 8


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reg_s(n):
    return getattr(arm64_const, f'UC_ARM64_REG_S{n}')


def get_s(mu, n):
    return struct.unpack('<f', struct.pack('<I', mu.reg_read(reg_s(n)) & 0xffffffff))[0]


def machine(image, page=b''):
    """The whole image (span, .bss zero) plus the data page holding `page`, a stack and a frame area."""
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, MAIN_SPAN)
    mu.mem_write(0, bytes(image))
    mu.mem_map(DATA, 0x4000)
    if page:
        mu.mem_write(DATA, page)
    mu.mem_map(STACK, 0x10000)
    mu.reg_write(arm64_const.UC_ARM64_REG_SP, STACK + 0x4000)
    mu.reg_write(arm64_const.UC_ARM64_REG_X29, FRAME)
    return mu
