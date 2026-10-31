"""Execute the 1.0.0 frame-step sites of the patched image with Unicorn: the engine object's dt / ratio (main+0x7a83e4)
and the independent global-ratio writer (main+0x8f68b8) must follow the data page for several frame rates; the original
image is the negative control (the interval-derived 60 Hz step whatever the page says). Ported from MacNativeGames
(MIT) games/totk/scripts/verify_realtime_patch.py, run on the image `apply` wrote."""
import struct

from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_W1, UC_ARM64_REG_X0, UC_ARM64_REG_X19

from .common import adrp_ldr, machine

FN = 0x7a83f4 - 16
GLOBAL_FN = 0x8f68b8
OBJ, GLOBAL_OBJ, GLOBAL_RATIO = 0x7000a000, 0x7000b000, 0x7000c000   # in the stack mapping, above sp and x29


def execute(image, seconds, global_got):
    mu = machine(image, struct.pack('<ff', seconds * 30, seconds))
    mu.mem_write(global_got, struct.pack('<Q', GLOBAL_RATIO))   # the global-ratio writer resolves *(GOT) to this float
    mu.mem_write(GLOBAL_OBJ + 0x25c, struct.pack('<I', 1))       # shipping: present interval 1
    mu.reg_write(UC_ARM64_REG_X0, OBJ)
    mu.reg_write(UC_ARM64_REG_W1, 1)
    mu.emu_start(FN, FN + 0x34, count=40)
    assert mu.reg_read(UC_ARM64_REG_PC) == FN + 0x34
    ratio, dt = struct.unpack('<ff', mu.mem_read(OBJ + 8, 8))
    mu.reg_write(UC_ARM64_REG_X19, GLOBAL_OBJ)
    mu.emu_start(GLOBAL_FN, GLOBAL_FN + 0x20, count=20)
    assert mu.reg_read(UC_ARM64_REG_PC) == GLOBAL_FN + 0x20
    global_ratio, = struct.unpack('<f', mu.mem_read(GLOBAL_RATIO, 4))
    return ratio, dt, global_ratio


def verify(original, patched):
    global_got = adrp_ldr(original, GLOBAL_FN + 8, GLOBAL_FN + 12)
    cases, failures = [], []
    for hz in (30, 50, 60, 90, 100, 120, 144):
        seconds = 1 / hz
        ratio, dt, global_ratio = execute(patched, seconds, global_got)
        ok = abs(ratio - seconds * 30) < 1e-6 and abs(dt - seconds) < 1e-7 and abs(global_ratio - ratio) < 1e-7
        cases.append(dict(hz=hz, ratio=ratio, seconds=dt, global_ratio=global_ratio, ok=ok))
        if not ok:
            failures.append(f'{hz} Hz')
    neg = execute(original, 0.01, global_got)
    if not (abs(neg[0] - 0.5) < 1e-6 and abs(neg[1] - 1 / 60) < 1e-6 and abs(neg[2] - 0.5) < 1e-6):
        failures.append(f'negative control {neg}')
    return dict(name='realtime', status='fail' if failures else 'pass', checked=len(cases) + 1, failures=failures,
                negative_control=dict(ratio=neg[0], seconds=neg[1], global_ratio=neg[2]))
