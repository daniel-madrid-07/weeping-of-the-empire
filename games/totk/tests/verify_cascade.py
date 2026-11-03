"""Execute the 1.0.0 agl::sdw::DepthShadow::updateSceneMatrix (main+0xa14f7c) with Unicorn on a stand-in DepthShadow:
the cascade boundaries d[0..n] at +0x308, the overlaps o at +0x350, the count at +0x228 / +0x360. Each call it makes
to DepthShadowUnit::updateSceneMatrix (main+0xa15140) is caught: s0 = the cascade's near, s1 = its far. With `cascade`
and the data page's multiplier k (+216) the inner boundaries d[1], d[2] move to max(d, min(k d, 0.75 x the next)),
computed here in f32 the same way, and every near / far must match bit for bit; k = 0 must give the original's calls
bit for bit (the original image is the reference), and so must the original with any k (it does not read the page).
The far, d[0] and d[3] never move, nor the far of a 1- or 2-cascade shadow; callee-saved registers and sp come back.
The receiving pass's copy (main+0xa21f98's loop, main+0xa22020 .. 0xa220d0) runs too: it must copy the same moved
d[0 .. n-1], the matrices and every register and flag as the original."""
import struct

from unicorn import UC_HOOK_CODE, arm64_const
from unicorn.arm64_const import (UC_ARM64_REG_D8, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21,
                                 UC_ARM64_REG_X22, UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26)

from games.totk.patches.sites_v100 import CASCADE_CAP, DP_SHADOW_SPLIT

from .common import STACK, f32, get_s, machine

FN, UNIT = 0xa14f7c, 0xa15140
OBJ = STACK + 0x6000                  # the DepthShadow stand-in (its +0xaa4 stays in the mapping)
D, O, UNITS = STACK + 0x9000, STACK + 0x9100, 0x7000c000
DONE = STACK + 0xf000                 # return address: the run stops there
CALLEE = {UC_ARM64_REG_X19: 0x1919, UC_ARM64_REG_X20: 0x2020, UC_ARM64_REG_X21: 0x2121, UC_ARM64_REG_X22: 0x2222,
          UC_ARM64_REG_X23: 0x2323, UC_ARM64_REG_X24: 0x2424, UC_ARM64_REG_X25: 0x2525, UC_ARM64_REG_X26: 0x2626}
# the Main scene's sun (gsys.bgmsconf: 2 / 32 / 64 / 200 m, far 3000) cut to n cascades, and some overlaps
BOUNDS = (2.0, 32.0, 64.0, 200.0, 3000.0)
OVERLAP = (1.0, 2.0, 4.0, 8.0)


def _page(k):
    page = bytearray(0x200)
    struct.pack_into('<f', page, DP_SHADOW_SPLIT, k)
    return bytes(page)


def run(image, k, n, bounds=None):
    """The (near, far) of each unit call for n cascades (bounds: d[0..n])."""
    d = list(bounds or BOUNDS[:n] + (BOUNDS[-1],))[:n + 1]
    mu = machine(image, _page(k))
    calls = []

    def unit(mu_, address, size, user):
        calls.append((get_s(mu_, 0), get_s(mu_, 1), mu_.reg_read(UC_ARM64_REG_X0)))
        mu_.reg_write(UC_ARM64_REG_PC, mu_.reg_read(UC_ARM64_REG_LR))
    mu.hook_add(UC_HOOK_CODE, unit, begin=UNIT, end=UNIT)
    mu.mem_write(OBJ + 0x228, struct.pack('<I', n))
    mu.mem_write(OBJ + 0x360, struct.pack('<I', 4))
    mu.mem_write(OBJ + 0x288, struct.pack('<I', 0))
    mu.mem_write(OBJ + 0x308, struct.pack('<Q', D))
    mu.mem_write(OBJ + 0x350, struct.pack('<Q', O))
    mu.mem_write(OBJ + 0x368, struct.pack('<Q', UNITS))
    mu.mem_write(OBJ + 0x330, struct.pack('<f', 0.5))
    mu.mem_write(D, struct.pack(f'<{len(d)}f', *d) + struct.pack('<4f', 9e9, 9e9, 9e9, 9e9))   # (past the far: never read)
    mu.mem_write(O, struct.pack('<4f', *OVERLAP))
    for r, v in CALLEE.items():
        mu.reg_write(r, v)
    mu.reg_write(UC_ARM64_REG_D8, 0x3ff8000000000000)
    mu.reg_write(UC_ARM64_REG_X0, OBJ)
    mu.reg_write(UC_ARM64_REG_X1, 0x1111)
    mu.reg_write(UC_ARM64_REG_X2, 0x2222)
    mu.reg_write(UC_ARM64_REG_LR, DONE)
    mu.emu_start(FN, DONE, count=2000)
    assert mu.reg_read(UC_ARM64_REG_PC) == DONE, 'did not return'
    assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, 'sp changed'
    assert mu.reg_read(UC_ARM64_REG_D8) == 0x3ff8000000000000, 'd8 changed'
    for r, v in CALLEE.items():
        assert mu.reg_read(r) == v, f'callee-saved register {r} changed'
    return [(near, far) for near, far, unit_at in calls], [unit_at for near, far, unit_at in calls]


COPY_LOOP, COPY_END = 0xa22020, 0xa220d0   # main+0xa21f98's copy of d[0 .. n-1] and the matrices for the receiver
REGS = [getattr(arm64_const, f'UC_ARM64_REG_X{i}') for i in range(29)] + [arm64_const.UC_ARM64_REG_NZCV,
                                                                         arm64_const.UC_ARM64_REG_SP]


def run_copy(image, k, n):
    """The loop of main+0xa21f98 on the same stand-in (view 0): the d[i] it copies (to sp + 4 i), the rest of what it
    writes (the matrices, sp + 0x10 ..) and the registers and flags it leaves."""
    d = list(BOUNDS[:n] + (BOUNDS[-1],))[:n + 1]
    mu = machine(image, _page(k))
    mu.mem_write(OBJ + 0x228, struct.pack('<I', n))
    mu.mem_write(OBJ + 0x360, struct.pack('<I', 4))
    mu.mem_write(OBJ + 0x308, struct.pack('<Q', D))
    mu.mem_write(OBJ + 0x368, struct.pack('<Q', UNITS))
    mu.mem_write(D, struct.pack(f'<{len(d)}f', *d))
    mu.mem_write(UNITS, bytes(range(256)) * 24)                       # the units' matrices (+0x2a0 .. +0x2e0)
    mu.reg_write(UC_ARM64_REG_X24, OBJ)
    mu.reg_write(arm64_const.UC_ARM64_REG_X25, 0)
    mu.reg_write(arm64_const.UC_ARM64_REG_X13, n)
    mu.emu_start(COPY_LOOP, COPY_END, count=2000)
    assert mu.reg_read(UC_ARM64_REG_PC) == COPY_END, 'copy loop did not finish'
    sp = STACK + 0x4000
    return (list(struct.unpack(f'<{n}f', mu.mem_read(sp, 4 * n))), bytes(mu.mem_read(sp + 0x10, 0x100)),
            [mu.reg_read(r) for r in REGS])


def _move(k, d, cap):
    return max(d, min(f32(k * d), f32(f32(cap) * CASCADE_CAP)))


def moved(k, n):
    """d[0 .. n] with the patch's moved boundaries."""
    d = list(BOUNDS[:n] + (BOUNDS[-1],))[:n + 1]
    if k:
        if n > 2:
            d[2] = _move(k, d[2], d[3])
        if n > 1:
            d[1] = _move(k, d[1], d[2])
    return d


def expect(k, n):
    """The calls the patched code must make: the original's sequence on the moved boundaries."""
    d = moved(k, n)
    out = [(d[0], f32(d[1] + (OVERLAP[0] if n > 1 else 0.0)))]
    for i in range(n - 1):
        last = i == n - 2
        out.append((f32(d[i + 1] - OVERLAP[i + 1]), f32(d[i + 2] + (0.0 if last else OVERLAP[i + 1]))))
    return out


def verify(original, patched):
    cases, failures = [], []
    for n in (1, 2, 3, 4):
        ref, ref_units = run(original, 0.0, n)
        if ref != expect(0.0, n):   # the model of the game's own code (the reference for the moved runs)
            failures.append(f'model: {n} cascades: the original makes {ref}, the model {expect(0.0, n)}')
        for k in (0.0, 1.0, 1.5, 2.0, 4.0, 10.0):
            got, units = run(patched, k, n)
            want = ref if k == 0 else expect(k, n)
            ok = got == want and units == ref_units
            if k:
                b = [got[0][0]] + [got[i + 1][0] + OVERLAP[i + 1] for i in range(n - 1)] + [got[-1][1]]
                ok &= all(x < y for x, y in zip(b, b[1:])) and b[0] == BOUNDS[0] and b[-1] == ref[-1][1]
                ok &= n < 4 or b[3] == BOUNDS[3]
            cases.append(dict(n=n, k=k, calls=got, ok=ok))
            if not ok:
                failures.append(f'{n} cascades, k {k}: {got}, expected {want}')
            orig_k, _ = run(original, k, n)
            if orig_k != ref:
                failures.append(f'negative control: the original reads the page ({n} cascades, k {k})')
            # the receiver's copy: the same moved boundaries as the units got, the rest of the loop untouched
            ref_d, ref_m, ref_r = run_copy(original, k, n)
            got_d, got_m, got_r = run_copy(patched, k, n)
            want_d = ref_d if k == 0 else moved(k, n)[:n]
            ok_copy = got_d == want_d and got_m == ref_m and got_r == ref_r
            ok_copy &= ref_d == list(BOUNDS[:n])                # (the original copies the game's own)
            cases.append(dict(n=n, k=k, copy=got_d, ok=ok_copy))
            if not ok_copy:
                failures.append(f'receiver copy, {n} cascades, k {k}: {got_d}, expected {want_d}'
                                f'{"" if got_m == ref_m else " (matrices differ)"}'
                                f'{"" if got_r == ref_r else " (registers / flags differ)"}')
    return dict(name='cascade', status='fail' if failures else 'pass', checked=len(cases), failures=failures,
                example=[c.get('calls', c.get('copy')) for c in cases if c['n'] == 4 and c['k'] in (0.0, 2.0)])
