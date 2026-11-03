"""The paraglider's velocity ease (ExecutePlayerParasailGlide::updatePhysics) on the 1.0.0 image's own instructions, as
totk_glide_ease.cpp's paraglider path (hook_f_16e4b80 at lr main+0x1d0d16c) feeds it: Unicorn runs main+0x1d0d16c..
0x1d0d1f4 (the caller's clamp of approachTargetSpeed's result by the node's +0x18 bit 1 and the Player component's
+0xb06 bit 3 against the image's 0.4 / 0.74 at main+0x3746b38, the direction at sp + 0x30, ScaleWithGravity(0.1, 0.1)
stubbed as normal gravity, v(+0x24..0x2c) += (min(s, cap) * dir - v) * 0.1) with the hook's s* and pre-set v, and
checks the result is t* + (v - t*) * 0.9^r (t* = min(s*, cap) * dir), the hook's model, for both caps and with the
clamp off. The caller's words around the call are checked first (bl 0x16e4b80 at main+0x1d0d168). The hook does not
patch the image: `original` is the one checked."""
import math
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X20,
                                 UC_ARM64_REG_X21)

from .common import STACK, f32, get_s, machine, reg_s

CALL, AFTER, END = 0x1d0d168, 0x1d0d16c, 0x1d0d1f4
APPROACH, SWG, CAPS = 0x16e4b80, 0x1076be0, 0x3746b38
NODE, PL, SP = STACK + 0x9000, STACK + 0xa000, STACK + 0xb000


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


def _caller(image, s, v, direction, flag18, flagb06):
    """The caller's tail with approachTargetSpeed's result s: the node's velocity after it."""
    mu = machine(image)
    mu.hook_add(UC_HOOK_CODE, _swg, begin=SWG, end=SWG)
    mu.mem_write(NODE + 0x18, bytes([flag18]))
    mu.mem_write(NODE + 0x24, struct.pack('<fff', *v))
    mu.mem_write(PL + 0xb06, bytes([flagb06]))
    mu.mem_write(SP + 0x30, struct.pack('<fff', *direction))
    mu.reg_write(UC_ARM64_REG_SP, SP)
    mu.reg_write(UC_ARM64_REG_X20, NODE)
    mu.reg_write(UC_ARM64_REG_X21, PL)
    _set_s(mu, 9, 0.1)       # (s9: the 0.1 the caller passes ScaleWithGravity)
    _set_s(mu, 0, s)
    mu.emu_start(AFTER, END, count=200)
    return struct.unpack('<fff', mu.mem_read(NODE + 0x24, 12))


def verify(original, patched=None):
    failures, cases = [], []
    if _bl_target(original, CALL) != APPROACH or _bl_target(original, 0x1d0d1b0) != SWG:
        return dict(name='paraglide_ease', status='fail', checked=1,
                    failures=['main+0x1d0d168 / 0x1d0d1b0 are not bl approachTargetSpeed / ScaleWithGravity'])
    caps = struct.unpack_from('<ff', original, CAPS)
    if caps != (f32(0.4), f32(0.74)):
        failures.append(f'the cap table at main+{CAPS:#x} is {caps}, not 0.4 / 0.74')
    direction = (f32(math.sin(0.7)), 0.0, f32(math.cos(0.7)))
    v = (0.05, -0.02, 0.21)
    for r in (0.5, 0.25, 0.125):
        q = (1.0 - 0.1) ** r
        keep = q / 0.9
        for s, flag18, flagb06 in ((0.3, 0, 0), (0.9, 0, 8), (0.9, 0, 0), (0.9, 2, 8)):
            cap = math.inf if flag18 & 2 else caps[0 if flagb06 & 8 else 1]
            s2 = f32(min(s, cap))
            t = [f32(s2 * d) for d in direction]
            pre = [f32(ti + (vi - ti) * keep) for ti, vi in zip(t, v)]   # the hook's pre-set v
            got = _caller(original, s2, pre, direction, flag18, flagb06)
            want = [ti + (vi - ti) * q for ti, vi in zip(t, v)]
            ok = all(abs(g - w) <= 1e-6 + 1e-5 * abs(w) for g, w in zip(got, want))
            cases.append(dict(r=r, s=s, flag18=flag18, flagb06=flagb06, got=got, want=want, ok=ok))
            if not ok:
                failures.append(f'r {r} s {s} flags {flag18}/{flagb06}: {got} against {want}')
    # negative control: the original k at r = 0.25 is one 0.1 step, not 1 - 0.9^0.25
    got = _caller(original, 0.3, v, direction, 0, 0)
    if abs(got[2] - (v[2] + (f32(0.3 * direction[2]) - v[2]) * 0.1)) > 1e-6:
        failures.append(f'negative control: one call moved v.z to {got[2]}, not a 0.1 step')
    return dict(name='paraglide_ease', status='fail' if failures else 'pass', checked=len(cases) + 3, failures=failures,
                cases=cases)
