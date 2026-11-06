"""Execute the model scene report-row read (1.0.0 main+0x844634, the reportguard feature) with Unicorn, on the original
image and the patched one: one object, from the function's start to its one ret (main+0x8446cc). This is the function
as the runtime's hook runs it AFTER its own check, so a bad value here is the race the patch closes (a row rewritten
between the hook's check and this read).
Checks: good handles (h up to 0x7fff, n up to 0x7fff) give the original's coverage and handle bit for bit; -1 leaves
both functions at once (coverage 0); a negative handle or n >= 0x8000 leaves the patched one with coverage 0 and the
handle untouched (the hook's no-handle path), where the original faults on #52's n (the float 0.05)."""
import struct

from unicorn import UcError
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_W1, UC_ARM64_REG_W2, UC_ARM64_REG_X0, UC_ARM64_REG_X3

from games.totk import patches as P
from games.totk.patches import sites_v100 as S

from .common import machine

FROM, EXIT = 0x844634, S.REPORT_EXIT
ROWS, OBJ = 0x10000000, 0x20000000


def run(image, h, n, ref=0):
    """(coverage bits, the handle after, pc) for one call with handle h on side 1 and row 2h's n, or 'fault'"""
    mu = machine(image)
    mu.mem_map(ROWS - 0x100000, 0x300000)   # 1 MB before the rows, 2 MB from them (n up to 0xffff)
    mu.mem_map(OBJ, 0x1000)
    if h >= 0:
        mu.mem_write(ROWS + h * 32, struct.pack('<I', n))
        mu.mem_write(ROWS + (2 * h + 1) * 16, struct.pack('<4I', 3, 5, 50, 100))
    if 0 <= n < 0x10000:
        mu.mem_write(ROWS + n * 32, struct.pack('<I', ref))
    mu.mem_write(OBJ + 0x90, struct.pack('<hh', 0x1234, h))   # side 0's handle, side 1's
    mu.mem_write(OBJ + 0x94, struct.pack('<f', 0.75))           # (zeroed by the function first)
    mu.reg_write(UC_ARM64_REG_X0, OBJ)
    mu.reg_write(UC_ARM64_REG_W1, 1)
    mu.reg_write(UC_ARM64_REG_W2, 1280 * 720)
    mu.reg_write(UC_ARM64_REG_X3, ROWS)
    try:
        mu.emu_start(FROM, EXIT, count=200)
    except UcError:
        return 'fault'
    cov, = struct.unpack('<I', mu.mem_read(OBJ + 0x94, 4))
    hd, = struct.unpack('<h', mu.mem_read(OBJ + 0x92, 2))
    return cov, hd, mu.reg_read(UC_ARM64_REG_PC)


def _image(original):
    g = P.game_v100()
    img = bytearray(original)
    if g.get('extra_area') and len(img) < g['extra_area'][1]:
        img += bytes(g['extra_area'][1] - len(img))
    P.apply(img, P.read_table(), ['reportguard'])
    return bytes(img)


def verify(original, patched):
    cases, failures = [], []
    mine = _image(original)

    def check(name, ok, detail=''):
        cases.append(dict(case=name, ok=ok, detail=detail))
        if not ok:
            failures.append(f'{name}: {detail}')
    for h, n, ref in ((1, 3, 0), (1, 3, 7), (0, 0, 2), (5, 0x7fff, 0), (5, 0x7fff, 9), (0x7fff, 1, 0)):
        o, m = run(original, h, n, ref), run(mine, h, n, ref)
        check(f'good handle {h} n {n:#x} ref {ref}: as the original', o == m and m != 'fault' and m[2] == EXIT,
              f'original {o} patched {m}')
    for image, name in ((original, 'original'), (mine, 'patched')):
        r = run(image, -1, 0)
        check(f'handle -1, {name}: coverage 0, handle -1', r != 'fault' and r[:2] == (0, -1), str(r))
    for h, n in ((-2, 0), (-0x8000, 0), (1, 0x8000), (1, 0xffff), (1, 0x3d4ccccd), (1, 0xffffffff)):
        r = run(mine, h, n)
        check(f'bad handle {h} n {n:#x}: skipped', r == (0, h, EXIT), str(r))
    o = run(original, 1, 0x3d4ccccd)
    check("#52's n (0.05f): the original faults", o == 'fault', str(o))
    return dict(name='report_guard', status='fail' if failures else 'pass', cases=cases, failures=failures)


if __name__ == '__main__':
    import sys
    data = bytes(P.load_main_image(sys.argv[1]))
    res = verify(data, _image(data))
    for c in res['cases']:
        print(('ok  ' if c['ok'] else 'FAIL'), c['case'], c['detail'])
