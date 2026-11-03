"""Execute Havok's step controller (1.0.0 main+0x274a3c) with Unicorn at several dt, on the original image and with each
of the v0.1.9 A/B features alone. setTauAndDamping (main+0x10d9b88) and setStepInfo (main+0x2588cc) are caught at
entry: tau (s0) / damping (s1), and dt (s0) / the second dt (s1) / numSteps (w2) / microsteps (w3, the solver info's
+0x80 passed through). Original: n = clamp(ceil(120 dt), 1, 8), tau = min(1, 0.6 (120 dt / n)^2), dt = min(dt, max
dt). havok_dt: the same of max(dt, 1/240). havok_tau: tau >= its 1/240 value 0.15, dt and n the original's. Both:
unchanged from 1/240 up."""
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_W2, UC_ARM64_REG_W3, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2)

from games.totk import patches as P

from .common import STACK, f32, get_s, machine, reg_s

FN, TAU_FN, STEP_FN = 0x274a3c, 0x10d9b88, 0x2588cc
CTRL, GRAV, INFO = STACK + 0xa000, STACK + 0xb000, STACK + 0xc000
DONE = STACK + 0xf000
MAX_DT = 0.0861
DAMPING, MICRO, DT2_SCALE = 1.0, 3, 0.5   # (the second dt: dt x the world's time scale; 0.5 tells it from dt)
DTS = (1 / 480, 1 / 360, 1 / 300, 1 / 250, 1 / 240, 1 / 200, 1 / 144, 1 / 120, 1 / 100, 1 / 60, 1 / 30, 1 / 15, 0.1)


def _set_s(mu, n, x):
    mu.reg_write(reg_s(n), struct.unpack('<I', struct.pack('<f', x))[0])


def run(image, dt):
    """(dt, numSteps, tau) the controller hands on for a step of dt."""
    mu = machine(image)
    got = {}

    def catch(mu, address, size, user):
        if address == TAU_FN:
            got['tau'], got['damping'] = get_s(mu, 0), get_s(mu, 1)
        elif address == STEP_FN:
            got['dt'], got['n'] = get_s(mu, 0), mu.reg_read(UC_ARM64_REG_W2)
            got['dt2'], got['micro'] = get_s(mu, 1), mu.reg_read(UC_ARM64_REG_W3)
        else:
            return
        mu.reg_write(UC_ARM64_REG_PC, mu.reg_read(UC_ARM64_REG_LR))
    mu.hook_add(UC_HOOK_CODE, catch, begin=TAU_FN, end=TAU_FN)
    mu.hook_add(UC_HOOK_CODE, catch, begin=STEP_FN, end=STEP_FN)
    # the defaults main+0x274994 writes: nominal 1/30, 0.6, damping 1, 4 steps per nominal, 1..8 steps, max dt
    mu.mem_write(CTRL + 0x18, struct.pack('<fffiiif', 1 / 30, 0.6, DAMPING, 4, 1, 8, MAX_DT))
    mu.mem_write(INFO + 0x80, struct.pack('<i', MICRO))   # (the controller passes the solver info's microsteps on)
    mu.reg_write(UC_ARM64_REG_X0, CTRL)
    mu.reg_write(UC_ARM64_REG_X1, GRAV)
    mu.reg_write(UC_ARM64_REG_X2, INFO)
    mu.reg_write(UC_ARM64_REG_LR, DONE)
    _set_s(mu, 0, dt)
    _set_s(mu, 1, dt * DT2_SCALE)
    mu.emu_start(FN, DONE, count=400)
    if not (got['damping'] == f32(DAMPING) and got['micro'] == MICRO and got['dt2'] == min(f32(f32(dt) * DT2_SCALE), f32(MAX_DT))):
        raise AssertionError(f"damping {got['damping']}, microsteps {got['micro']}, second dt {got['dt2']} not passed on")
    return got['dt'], got['n'], got['tau']


def expect(dt, dt_floor=False, tau_floor=False):
    d = min(f32(dt), MAX_DT)
    if dt_floor:
        d = max(d, f32(1 / 240))
    n = min(max(int(120 * d + 0.99999), 1), 8)
    x = n / (120 * d)
    if tau_floor:
        x = min(x, 2.0)
    return d, n, min(1.0, 0.6 / (x * x))


def _image(original, feature):
    g = P.game_v100()
    img = bytearray(original)
    if g.get('extra_area') and len(img) < g['extra_area'][1]:
        img += bytes(g['extra_area'][1] - len(img))
    P.apply(img, P.read_table(), [feature])
    return bytes(img)


def verify(original, patched):
    cases, failures = [], []
    images = {'original': (original, False, False), 'havok_dt': (_image(original, 'havok_dt'), True, False),
              'havok_tau': (_image(original, 'havok_tau'), False, True), 'all': (patched, True, True)}
    for name, (img, dfl, tfl) in images.items():
        for dt in DTS:
            try:
                d, n, tau = run(img, dt)
            except AssertionError as e:
                failures.append(f'{name} {1 / dt:.0f} Hz: {e}')
                continue
            wd, wn, wt = expect(dt, dfl, tfl)
            ok = abs(d - wd) <= 1e-6 * wd and n == wn and abs(tau - wt) <= 2e-6 * max(wt, 1e-3)
            cases.append(dict(image=name, hz=round(1 / dt, 1), dt=d, n=n, tau=tau, ok=ok))
            if not ok:
                failures.append(f'{name} {1 / dt:.0f} Hz: dt {d:.6f} n {n} tau {tau:.5f}, want {wd:.6f} {wn} {wt:.5f}')
    # the features change nothing from 1/240 up, and the original goes under both floors below it (the A/B's point)
    base = {c['hz']: c for c in cases if c['image'] == 'original'}
    for c in cases:
        if c['image'] != 'original' and c['hz'] <= 240.0 and (c['dt'], c['n'], c['tau']) != (
                base[c['hz']]['dt'], base[c['hz']]['n'], base[c['hz']]['tau']):
            failures.append(f"{c['image']} changes {c['hz']} Hz (from 1/240 up it must not)")
    o360 = base[360.0]
    if not (o360['dt'] < 1 / 240 and o360['tau'] < 0.1):
        failures.append(f"negative control: the original at 360 Hz gives dt {o360['dt']}, tau {o360['tau']}")
    return dict(name='havok_step', status='fail' if failures else 'pass', checked=len(cases) + 2, failures=failures,
                cases=cases)
