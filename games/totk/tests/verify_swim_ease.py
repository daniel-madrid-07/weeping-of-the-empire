"""The swim velocity ease (the swim-move physics, an ExecutePlayerSwimMove method at main+0x1d259f4) on the 1.0.0
image's own instructions, as totk_swim_ease.cpp (hook_f_16e4f90 at lr main+0x1d25ac0) feeds it: Unicorn runs
main+0x1d25ac0..0x1d25b20 (s = s9 * the getter's s0, t = dir (sp + 0x30) * s, v(x21 + 0x24..0x2c) += (t - v) * the
image's 0.175 at main+0x377671c) with the hook's pre-set v, and checks the result is t + (v - t) * (1 - k)^r, the
hook's model, at r 0.5 / 0.25 / 0.125 and with the stick released. The call word (bl 0x16e4f90 at main+0x1d25abc) and
the factor are checked first; the original v at r = 0.25 is one 0.175 step (negative control). The hook does not patch
the image: `original` is the one checked."""
import math
import struct

from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X21

from .common import STACK, f32, machine, reg_s

CALL, AFTER, END = 0x1d25abc, 0x1d25ac0, 0x1d25b20
GETTER, KADDR = 0x16e4f90, 0x377671c
NODE, SP = STACK + 0x9000, STACK + 0xb000


def _set_s(mu, n, x):
    mu.reg_write(reg_s(n), struct.unpack('<I', struct.pack('<f', x))[0])


def _bl_target(image, at):
    w = struct.unpack_from('<I', image, at)[0]
    if w >> 26 != 0b100101:
        return None
    imm = w & 0x3ffffff
    return at + ((imm ^ (1 << 25)) - (1 << 25)) * 4


def _caller(image, s9, got, v, direction):
    mu = machine(image)
    mu.mem_write(NODE + 0x20, struct.pack('<I', 0))
    mu.mem_write(NODE + 0x24, struct.pack('<fff', *v))
    mu.mem_write(SP + 0x30, struct.pack('<fff', *direction))
    mu.reg_write(UC_ARM64_REG_SP, SP)
    mu.reg_write(UC_ARM64_REG_X21, NODE)
    _set_s(mu, 9, s9)
    _set_s(mu, 0, got)
    mu.emu_start(AFTER, END, count=64)
    return struct.unpack('<fff', mu.mem_read(NODE + 0x24, 12))


def verify(original, patched=None):
    failures, cases = [], []
    if _bl_target(original, CALL) != GETTER:
        return dict(name='swim_ease', status='fail', checked=1, failures=['main+0x1d25abc is not bl 0x16e4f90'])
    k, = struct.unpack_from('<f', original, KADDR)
    if k != f32(0.175):
        failures.append(f'the factor at main+{KADDR:#x} is {k}, not 0.175')
    direction = (f32(math.sin(0.7)), 0.0, f32(math.cos(0.7)))
    v = (0.05, -0.01, 0.12)
    for r in (0.5, 0.25, 0.125):
        q = (1.0 - k) ** r
        keep = q / (1.0 - k)
        for s9, got in ((1.0, 0.2), (0.6, 0.2), (0.0, 0.2)):
            s = f32(s9 * got)
            t = [f32(d * s) for d in direction]
            pre = [f32(ti + (vi - ti) * keep) for ti, vi in zip(t, v)]   # the hook's pre-set v
            out = _caller(original, s9, got, pre, direction)
            want = [ti + (vi - ti) * q for ti, vi in zip(t, v)]
            ok = all(abs(o - w) <= 1e-6 + 1e-5 * abs(w) for o, w in zip(out, want))
            cases.append(dict(r=r, s9=s9, got=out, want=want, ok=ok))
            if not ok:
                failures.append(f'r {r} s9 {s9}: {out} against {want}')
    out = _caller(original, 1.0, 0.2, v, direction)
    t2 = f32(direction[2] * f32(0.2))
    if abs(out[2] - (v[2] + (t2 - v[2]) * k)) > 1e-6:
        failures.append(f'negative control: one call moved v.z to {out[2]}, not a 0.175 step')
    return dict(name='swim_ease', status='fail' if failures else 'pass', checked=len(cases) + 3, failures=failures,
                cases=cases)
