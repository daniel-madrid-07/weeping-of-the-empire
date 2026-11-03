"""Execute the altitude effect's two frame-counted spots (1.0.0 main+0xb5b7bc, the effectease feature) with Unicorn, on
the original image and the patched one:
- the ease: from main+0xb5b85c (x8 = the literal's page, s0 = the target, x19 = the state) to the cbz after the store
  at main+0xb5b87c: +0x3bc = x + (t - x) k with k the game's 0.01, or the page's DP_EASE_K001 when it is non-zero.
- the burst count: from main+0xb5ba60 (ldr w8, [x19, #0x3b4]) until the 4x call (bl main+0x731c9c at 0xb5ba78) or the
  way out (main+0xb5b8a8): with the page's DP_TICKS30 at 0 the game's subs #1 / b.lt, else the count steps by the
  ticks and runs while it is >= 1.
Checks: page 0 = the original bit for bit; k' = 1 - 0.99^r settles like 30 fps (8 calls at 240 leave the 0.99 of one
30 fps call); the burst lasts 150 ticks of game time at 60 / 120 / 240 fps, as 150 frames at 30."""
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X8, UC_ARM64_REG_X19, UC_ARM64_REG_X21

from games.totk import patches as P
from games.totk.patches import sites_v100 as S

from .common import STACK, bits, f32, machine, reg_s

EASE_FROM, EASE_TO = 0xb5b85c, 0xb5b87c
BURST_FROM, BURST_CALL, BURST_OUT = 0xb5ba60, 0xb5ba78, 0xb5b8a8
STATE = STACK + 0xa000
K = f32(0.01)


def _page(k=0.0, ticks_m1=0):
    page = bytearray(0x200)
    struct.pack_into('<f', page, S.DP_EASE_K001, k)
    struct.pack_into('<i', page, S.DP_TICKS30, ticks_m1)
    return bytes(page)


def ease(image, x, t, k=0.0):
    """+0x3bc after one call of the ease from x toward t, page k (0: the game's)"""
    mu = machine(image, _page(k))
    mu.mem_write(STATE + 0x3bc, struct.pack('<f', x))
    mu.mem_write(STATE + 0x3f0, bytes(8))
    mu.reg_write(UC_ARM64_REG_X8, 0x3776000)
    mu.reg_write(UC_ARM64_REG_X19, STATE)
    mu.reg_write(reg_s(0), bits(t))
    mu.emu_start(EASE_FROM, EASE_TO, count=40)
    return struct.unpack('<f', mu.mem_read(STATE + 0x3bc, 4))[0]


def burst(image, count, ticks_m1=0):
    """(called the 4x step, the count stored after) for one pass with the count at +0x3b4"""
    mu = machine(image, _page(0.0, ticks_m1))
    mu.mem_write(STATE + 0x3b4, struct.pack('<i', count))
    mu.reg_write(UC_ARM64_REG_X19, STATE)
    mu.reg_write(UC_ARM64_REG_X21, STATE + 0x800)
    got = {}

    def stop(mu, address, size, user):
        got['at'] = address
        mu.emu_stop()
    mu.hook_add(UC_HOOK_CODE, stop, begin=BURST_CALL, end=BURST_CALL)
    mu.hook_add(UC_HOOK_CODE, stop, begin=BURST_OUT, end=BURST_OUT)
    mu.emu_start(BURST_FROM, BURST_OUT + 4, count=60)
    if 'at' not in got:
        raise AssertionError(f'burst pass from {count} reached neither the call nor the way out')
    return got['at'] == BURST_CALL, struct.unpack('<i', mu.mem_read(STATE + 0x3b4, 4))[0]


def k_of(r):
    return f32(1.0 - (1.0 - K) ** r)


def burst_frames(image, fps):
    """frames the 4x step runs from a fresh 150 at fps (ticks from the runtime's accumulator, put_eases)"""
    r, acc, count, frames = 30.0 / fps, 0.0, 150, 0
    for _ in range(int(fps * 20)):
        if r < 1.0:
            acc += r
            ticks = int(acc)
            acc -= ticks
            tm1 = ticks - 1
        else:
            tm1 = 0
        called, count = burst(image, count, tm1)
        if not called:
            break
        frames += 1
    return frames


def _image(original):
    g = P.game_v100()
    img = bytearray(original)
    if g.get('extra_area') and len(img) < g['extra_area'][1]:
        img += bytes(g['extra_area'][1] - len(img))
    P.apply(img, P.read_table(), ['effectease'])
    return bytes(img)


def verify(original, patched):
    cases, failures = [], []
    mine = _image(original)

    def check(name, ok, detail=''):
        cases.append(dict(case=name, ok=ok, detail=detail))
        if not ok:
            failures.append(f'{name}: {detail}')
    # page 0: the original's ease bit for bit
    for x, t in ((0.3, 1.0), (1.0, 0.3), (0.55, 0.61), (0.0, 0.0)):
        o, m = ease(original, x, t), ease(mine, x, t)
        check(f'ease page 0 x {x} t {t}', bits(o) == bits(m), f'original {o!r} patched {m!r}')
    # the page's k: the formula, and 8 calls at 240 = one 30 fps call
    for fps in (60, 120, 144, 240):
        k = k_of(30 / fps)
        x, n = 0.3, round(fps / 30)
        for _ in range(n):
            x = ease(mine, x, 1.0, k)
        want = 1.0 - 0.7 * (1.0 - K) ** (n * 30 / fps)   # (n calls = n r of game time: one 30 fps call at 60 / 120 / 240)
        check(f'ease {fps} fps: {n} calls', abs(x - want) < 2e-5, f'{x:.6f} want {want:.6f} (k {k:.6f})')
    one = ease(mine, 0.3, 1.0, k_of(0.5))
    check('ease page k: x + (t - x) k', bits(one) == bits(f32(f32(f32(f32(1.0) - f32(0.3)) * k_of(0.5)) + f32(0.3))),
          f'{one!r}')
    # the burst: page 0 = the original's pass for every count
    for c in (150, 2, 1, 0, -1):
        o, m = burst(original, c), burst(mine, c)
        check(f'burst page 0 count {c}', o == m, f'original {o} patched {m}')
    # no tick this frame: the count holds and the step still runs while it is >= 1; 0 never runs
    check('burst no tick, count 5', burst(mine, 5, -1) == (True, 5), str(burst(mine, 5, -1)))
    check('burst no tick, count 0', burst(mine, 0, -1)[0] is False, str(burst(mine, 0, -1)))
    o30 = burst_frames(original, 30)
    check('burst 30 fps original: 150 frames', o30 == 150, str(o30))
    check('burst 30 fps patched = original', burst_frames(mine, 30) == o30, '')
    for fps in (60, 120, 240):
        n, o = burst_frames(mine, fps), burst_frames(original, fps)
        want = 150 * fps // 30
        check(f'burst {fps} fps: 150 ticks of game time', abs(n - want) <= 1, f'{n} frames, want {want} (original {o})')
    return dict(name='effect_ease', status='fail' if failures else 'pass', cases=cases, failures=failures)


if __name__ == '__main__':
    import sys
    data = bytes(P.load_main_image(sys.argv[1]))
    res = verify(data, _image(data))
    for c in res['cases']:
        print(('ok  ' if c['ok'] else 'FAIL'), c['case'], c['detail'])
    print(res['status'], f"{len(res['failures'])} failed" if res['failures'] else '')
    sys.exit(0 if res['status'] == 'pass' else 1)
