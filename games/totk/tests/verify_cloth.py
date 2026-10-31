"""Check the hair / cloth clock veneers of the patched 1.0.0 image with Unicorn. Ported from MacNativeGames (MIT)
games/totk/scripts/verify_cloth_clock.py; the image is the one `apply` wrote instead of a C fixture's.

Per inline place: the fmul veneer leaves measured seconds * factor in the result register, the ratio-store veneer
writes measured 30 Hz ratio * factor into the clock's ratio slot (sp- or x29-relative), and both restore x9, q31, sp
and x29. The call veneer scales s2 / s3 by the measured ratio and enters the shared update with the caller's return
address. The unpatched image must give factor / 30 (4x at 120 FPS). Words near each place other than the sites must
be unchanged."""
import struct

from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_Q31, UC_ARM64_REG_SP, UC_ARM64_REG_X9, UC_ARM64_REG_X29

from games.totk.patches.sites_v100 import CLOTH_CALL, CLOTH_INLINE, CLOTH_UPDATE

from .common import FRAME, STACK, bits, f32, get_s, machine, reg_s

SENTINEL_X9, SENTINEL_Q31 = 0x1122334455667788, 0x0123456789abcdeffedcba9876543210


def _machine(image, hz):
    mu = machine(image, struct.pack('<ff', 30 / hz, 1 / hz))
    mu.reg_write(UC_ARM64_REG_X9, SENTINEL_X9)
    mu.reg_write(UC_ARM64_REG_Q31, SENTINEL_Q31)
    return mu


def _preserved(mu, what):
    assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{what}: sp changed'
    assert mu.reg_read(UC_ARM64_REG_X29) == FRAME, f'{what}: x29 changed'
    assert mu.reg_read(UC_ARM64_REG_X9) == SENTINEL_X9, f'{what}: x9 changed'
    assert mu.reg_read(UC_ARM64_REG_Q31) == SENTINEL_Q31, f'{what}: q31 changed'


def verify(original, patched):
    cases, failures = 0, []
    try:
        for hz in (20, 30, 60, 90, 120, 144):
            for factor in (0.0, 0.25, 1.0, 2.0):
                for fmul, store, r, f, off, pair in CLOTH_INLINE:
                    mu = _machine(patched, hz)   # fmul: s<r> held 1/30
                    mu.reg_write(reg_s(f), bits(factor))
                    mu.reg_write(reg_s(r), bits(f32(1 / 30)))
                    mu.emu_start(fmul, fmul + 4, count=20)
                    assert mu.reg_read(UC_ARM64_REG_PC) == fmul + 4, f'{fmul:#x}: did not return'
                    assert abs(get_s(mu, r) - factor / hz) < 1e-6, (hex(fmul), hz, factor, get_s(mu, r))
                    assert abs(get_s(mu, f) - factor) < 1e-6, f'{fmul:#x}: factor changed'
                    _preserved(mu, f'{fmul:#x}')
                    mu = _machine(patched, hz)   # the ratio store
                    seconds = f32(factor / hz)
                    mu.reg_write(reg_s(f), bits(factor))
                    mu.reg_write(reg_s(r), bits(seconds))
                    mu.emu_start(store, store + 4, count=20)
                    assert mu.reg_read(UC_ARM64_REG_PC) == store + 4, f'{store:#x}: did not return'
                    slot = FRAME + off if pair else STACK + 0x4000 + off
                    got = struct.unpack('<f', mu.mem_read(slot + (4 if pair else 0), 4))[0]
                    assert abs(got - factor * 30 / hz) < 1e-5, (hex(store), hz, factor, got)
                    if pair:
                        assert abs(struct.unpack('<f', mu.mem_read(slot, 4))[0] - seconds) < 1e-7, f'{store:#x}: seconds slot'
                    assert abs(get_s(mu, f) - factor) < 1e-6, f'{store:#x}: factor changed'
                    _preserved(mu, f'{store:#x}')
                    cases += 2
                mu = _machine(patched, hz)   # the call into the shared update: s2 = factor / 30, s3 = factor
                mu.reg_write(reg_s(2), bits(f32(factor / 30)))
                mu.reg_write(reg_s(3), bits(factor))
                mu.emu_start(CLOTH_CALL, CLOTH_UPDATE, count=20)
                assert mu.reg_read(UC_ARM64_REG_PC) == CLOTH_UPDATE, 'call did not reach the update'
                assert mu.reg_read(UC_ARM64_REG_LR) == CLOTH_CALL + 4, 'call: return address changed'
                assert abs(get_s(mu, 2) - factor / hz) < 1e-6 and abs(get_s(mu, 3) - factor * 30 / hz) < 1e-5, (hz, factor)
                _preserved(mu, 'call')
                cases += 1
        fmul, _, r, f, _, _ = CLOTH_INLINE[0]   # negative control: shipping code advances factor / 30 per frame
        mu = _machine(original, 120)
        mu.reg_write(reg_s(f), bits(1.0))
        mu.reg_write(reg_s(r), bits(f32(1 / 30)))
        mu.emu_start(fmul, fmul + 4, count=1)
        assert abs(get_s(mu, r) * 120 - 4) < 1e-4, 'negative control must reproduce the 4x clock at 120 FPS'
        cases += 1
        for fmul, store, *_ in CLOTH_INLINE:
            for o in range(min(fmul, store) - 32, max(fmul, store) + 36, 4):
                if o not in (fmul, store):
                    assert original[o:o + 4] == patched[o:o + 4], f'unexpected change at {o:#x}'
    except AssertionError as e:
        failures.append(str(e))
    return dict(name='cloth', status='fail' if failures else 'pass', checked=cases, failures=failures)
