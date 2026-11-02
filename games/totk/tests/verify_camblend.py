"""Execute the 1.0.0 camera mode-switch blend (FUN_aa80c8 with its progress step ab12f0) with Unicorn, frame by frame at
several frame rates: with `camblend` the old mode's offsets follow the 30 fps curve x0 * (1 - e(t)) at every rate; the
original image is the negative control (x0 * (1 - e)^dt: at 120 fps far above the curve until the end).
powf (PLT main+0x2aa42b0) and sinf (main+0x2aa4640) are imports: they run here in Python on the call's s0 / s1."""
import math
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_W1, UC_ARM64_REG_X0

from .common import STACK, f32, get_s, machine, reg_s

FN, POWF, SINF = 0xaa80c8, 0x2aa42b0, 0x2aa4640
CAM = 0x70009000                      # the PlayerCamera stand-in, in the stack mapping
DONE = STACK + 0xf000                 # return address: the run stops there
COUNT = 30.0                          # the switch's count (30 Hz frames) x scale 1: one second
OFFSETS = {0x5f0: 1.5, 0x5f4: -2.0, 0x5f8: 0.75, 0x608: 40.0, 0x60c: -25.0, 0x614: 0.6, 0x610: 3.0}


def _set_s(mu, n, x):
    mu.reg_write(reg_s(n), struct.unpack('<I', struct.pack('<f', x))[0])


def _imports(mu, address, size, user):
    if address == POWF:
        _set_s(mu, 0, f32(math.pow(get_s(mu, 0), get_s(mu, 1))))
    elif address == SINF:
        _set_s(mu, 0, f32(math.sin(get_s(mu, 0))))
    else:
        return
    mu.reg_write(UC_ARM64_REG_PC, mu.reg_read(UC_ARM64_REG_LR))


def _ease(t):
    p = min(t * 30 / COUNT, 1.0)
    return (math.sin(p * math.pi - math.pi / 2) + 1) / 2


def run(image, hz):
    """Worst |offset - x0 (1 - e(t))| relative to x0 over the switch at `hz` (dt = 30 / hz)."""
    mu = machine(image)
    mu.hook_add(UC_HOOK_CODE, _imports, begin=POWF, end=POWF)
    mu.hook_add(UC_HOOK_CODE, _imports, begin=SINF, end=SINF)
    mu.mem_write(CAM + 0x5d0 + 8, struct.pack('<ffff', COUNT, 1.0, 0.0, 0.0))   # count, scale, 1/(count*scale), p
    mu.mem_write(CAM + 0x5d0 + 0x18, struct.pack('<f', 0.0))                      # e
    for off, x in OFFSETS.items():
        mu.mem_write(CAM + off, struct.pack('<f', x))
    dt, worst, frames = 30 / hz, 0.0, 0
    while frames < hz * 2:
        frames += 1
        mu.reg_write(UC_ARM64_REG_X0, CAM)
        mu.reg_write(UC_ARM64_REG_W1, 1)
        mu.reg_write(UC_ARM64_REG_LR, DONE)
        _set_s(mu, 0, dt)
        mu.emu_start(FN, DONE, count=4000)
        e = _ease(frames / hz)
        for off, x0 in OFFSETS.items():
            x, = struct.unpack('<f', mu.mem_read(CAM + off, 4))
            worst = max(worst, abs(x - x0 * (1 - e)) / abs(x0))
        if e >= 1:
            break
    return worst, frames


def verify(original, patched):
    cases, failures = [], []
    for hz in (30, 60, 120, 144, 240, 360):
        worst, frames = run(patched, hz)
        ok = worst < 1e-4
        cases.append(dict(hz=hz, worst=worst, frames=frames, ok=ok))
        if not ok:
            failures.append(f'{hz} Hz: {worst:.4f} off the 30 fps curve')
    neg30, _ = run(original, 30)
    neg120, _ = run(original, 120)
    if not (neg30 < 1e-4 and neg120 > 0.2):   # the game's own: the curve at 30, far off at 120
        failures.append(f'negative control: 30 Hz {neg30:.4f}, 120 Hz {neg120:.4f}')
    return dict(name='camblend', status='fail' if failures else 'pass', checked=len(cases) + 2, failures=failures,
                cases=cases, negative_control=dict(hz30=neg30, hz120=neg120))
