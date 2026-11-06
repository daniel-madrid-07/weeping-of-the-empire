"""A scripted paraglide's decay on the 1.0.0 image's own instructions, as totk_glide_ease.cpp's event path (hook_f_16e4b80 at
lr main+0x1c0cff8) feeds it: the caller keeps its node in x21 (`mov x21, x0` at main+0x1c0cf8c) and a zero in s11 (`movi
d11, #0` at 0x1c0cfb8), calls approachTargetSpeed at 0x1c0cff4 and ScaleWithGravity(0.1, 0.1) at 0x1c0d004. Unicorn then
runs main+0x1c0cff8..0x1c0d048: s8 = s x 0 and v(+0x24..0x2c) += (s8 - v) x k, with ScaleWithGravity stubbed as normal
gravity (k = 0.1). With v pre-scaled by the hook's 0.9^(r - 1), the result must be v x 0.9^r, the hook's model, for
several ratios and any s. The hook does not patch the image: `original` is the one checked."""
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X21

from .common import STACK, machine, reg_s

CALL, AFTER, END = 0x1c0cff4, 0x1c0cff8, 0x1c0d048
APPROACH, SWG = 0x16e4b80, 0x1076be0
NODE, SP = STACK + 0x9000, STACK + 0xb000


def _set_s(mu, n, x):
    mu.reg_write(reg_s(n), struct.unpack('<I', struct.pack('<f', x))[0])


def _swg(mu, address, size, user):   # ScaleWithGravity(s0, s1) -> s0 at normal gravity
    mu.reg_write(UC_ARM64_REG_PC, mu.reg_read(UC_ARM64_REG_LR))


def _bl_target(image, at):
    w = struct.unpack_from('<I', image, at)[0]
    if w >> 26 != 0b100101:
        return None
    imm = w & 0x3ffffff
    return at + ((imm ^ (1 << 25)) - (1 << 25)) * 4


def _word(image, at):
    return struct.unpack_from('<I', image, at)[0]


def _caller(image, s, v):
    mu = machine(image)
    mu.hook_add(UC_HOOK_CODE, _swg, begin=SWG, end=SWG)
    mu.mem_write(NODE + 0x24, struct.pack('<fff', *v))
    mu.reg_write(UC_ARM64_REG_SP, SP)
    mu.reg_write(UC_ARM64_REG_X21, NODE)
    _set_s(mu, 9, 0.1)    # (s9: the 0.1 the caller passes ScaleWithGravity)
    _set_s(mu, 11, 0.0)   # (s11: the caller's zero direction)
    _set_s(mu, 0, s)
    mu.emu_start(AFTER, END, count=200)
    return struct.unpack('<fff', mu.mem_read(NODE + 0x24, 12))


def verify(original, patched=None):
    failures = []
    if _bl_target(original, CALL) != APPROACH or _bl_target(original, 0x1c0d004) != SWG:
        failures.append('main+0x1c0cff4 / 0x1c0d004 are not bl approachTargetSpeed / ScaleWithGravity')
    if _word(original, 0x1c0cf8c) != 0xaa0003f5:   # mov x21, x0
        failures.append('main+0x1c0cf8c is not mov x21, x0 (the node in x21)')
    if _word(original, 0x1c0cfb8) != 0x2f00e40b:   # movi d11, #0
        failures.append('main+0x1c0cfb8 is not movi d11, #0 (the zero direction)')
    if not failures:
        for r in (0.5, 0.25, 0.125):
            keep = 0.9 ** r / 0.9
            v0 = (1.0, 0.5, -0.25)
            got = _caller(original, 0.33, tuple(x * keep for x in v0))
            want = tuple(x * 0.9 ** r for x in v0)
            if any(abs(g - w) > 1e-6 for g, w in zip(got, want)):
                failures.append(f'ratio {r}: v {got}, want {want}')
    return dict(name='event_glide', status='fail' if failures else 'pass', checked=4, failures=failures)
