"""abi_scan.py incremental() on a synthetic image (hand-assembled AArch64, no game data): from a full scan's keep and
state, a patched image's scan holds every line a full scan of it has (here: equals it), rescans only what changed (a
new BL into the middle of a function rescans that one), and falls back to a full scan when the base cannot bound the
change (bytes outside every function, a word turned 0, a noreturn function that returns, a base function gone, another
scan's state). Seconds; the engine venv (capstone).

    <engine venv python> -I engine/tools/recomp/test_abi_incremental.py
"""
import json, os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abi_scan as A  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


RET, NOP, BRK, MOV_X9 = 0xd65f03c0, 0xd503201f, 0xd4200000, 0xd2800029
ADD_X9, ADD_X10 = 0x91000520, 0x91000540   # add x0, x9, #1 / add x0, x10, #1: read x9 / x10
BL = lambda t, p: 0x94000000 | (((t - p) >> 2) & 0x3ffffff)   # noqa: E731


def image(funcs):
    """{start: [words]} -> (image, fde)"""
    img = bytearray(0x1000)
    for s, words in funcs.items():
        for k, w in enumerate(words):
            struct.pack_into('<I', img, s + 4 * k, w)
    return bytes(img), {s: 4 * len(w) for s, w in funcs.items()}


BASE = {0x100: [BL(0x200, 0x100), ADD_X9, RET],   # reads x9 after the call: a keep site, 0x200 a callee
        0x200: [MOV_X9, RET],
        0x300: [BL(0x400, 0x300), ADD_X10, RET],  # after a noreturn call: nothing follows
        0x400: [BRK],
        0x500: [NOP, RET]}
tmp = tempfile.mkdtemp(prefix='abiinc-')


def full(img, fde, name, state=True):
    st = {} if state else None
    sites, entries, callees, _ = A.scan(img, fde, state_out=st)
    keep = os.path.join(tmp, name + '.txt')
    with open(keep, 'w') as f:
        f.write('module main\n' + ''.join(f'site {a:x} {A.regs_txt(r)}\n' for a, r in sorted(sites.items())) +
                ''.join(f'entry {a:x} {A.regs_txt(r)}\n' for a, r in sorted(entries.items())) +
                ''.join(f'callee {a:x} {A.regs_txt(r)}\n' for a, r in sorted(callees.items())))
    if state:
        A.write_state(keep + '.state', img, fde, st)
    open(os.path.join(tmp, name + '.bin'), 'wb').write(img)
    return keep, (sites, entries, callees)


bimg, bfde = image(BASE)
bkeep, bres = full(bimg, bfde, 'base')
check('the base: site 0x100 and callee 0x200 keep x9; nothing after the noreturn call',
      bres[0] == {0x100: {9}} and bres[2] == {0x200: {9}}, str(bres))


def inc(funcs, ext=None, keep=None):
    img, fde = image(funcs)
    base, why = A.incremental(img, fde, ext or {}, os.path.join(tmp, 'base'), keep or bkeep, 'main')
    if base is None:
        return None, why, None
    got = A.scan(img, fde, base=base)[:3]
    want = A.scan(img, fde)[:3]
    sup = all(set(want[i]) <= set(got[i]) and all(want[i][a] <= set(got[i][a]) for a in want[i]) for i in range(3))
    return base, (got, want), sup


p = dict(BASE)
p[0x500] = [BL(0x200, 0x500), ADD_X10, RET]   # a new caller reading x10 after its call
base, res, sup = inc(p)
check('a patched function: only it again', base is not None and base['todo'] == {0x500}, str(base and base['todo']))
check('...its keep holds the full scan\'s (here: equals it)', sup and res[0] == res[1], str(res))
check('...callee 0x200 now keeps x9 and x10', res[0][2].get(0x200) == {9, 10}, str(res[0][2]))
p = dict(BASE)
p[0x500] = [BL(0x104, 0x500), RET]            # a BL into the middle of 0x100: an entry there
base, res, sup = inc(p)
check('a new BL into a function\'s middle: that function again too', base and base['todo'] == {0x500, 0x100}, str(base and base['todo']))
check('...still a superset', sup, str(res))
img, fde = image(BASE)
img = bytearray(img)
img[0x800] = 7
r = A.incremental(bytes(img), fde, {}, os.path.join(tmp, 'base'), bkeep, 'main')
check('a byte outside every function: a full scan', r[0] is None and 'outside every function' in r[1], str(r[1]))
r = A.incremental(bytes(img), fde, {0x500: [(0x800, 0x804)]}, os.path.join(tmp, 'base'), bkeep, 'main')
check('...unless it is extension code', r[0] is not None, str(r[1]))
p = dict(BASE)
p[0x500] = [0, RET]
r = inc(p)
check('a word turned 0: a full scan', r[0] is None and 'turned 0' in r[1], str(r[1]))
p = dict(BASE)
p[0x400] = [RET]
r = inc(p)
check('a noreturn function that returns now: a full scan', r[0] is None and 'noreturn' in r[1], str(r[1]))
p = dict(BASE)
del p[0x500]
r = inc(p)
check('a base function gone: a full scan', r[0] is None and 'gone' in r[1], str(r[1]))
st = json.load(open(bkeep + '.state'))
st['tools'] = 'other'
json.dump(st, open(os.path.join(tmp, 'other.txt.state'), 'w'))
open(os.path.join(tmp, 'other.txt'), 'w').write(open(bkeep).read())
r = inc(BASE, keep=os.path.join(tmp, 'other.txt'))
check('a state of another abi_scan.py: a full scan', r[0] is None and 'another scan' in r[1], str(r[1]))
r = inc(BASE, keep=os.path.join(tmp, 'nostate.txt'))
check('no state: a full scan', r[0] is None and 'no state' in r[1], str(r[1]))

import shutil  # noqa: E402
shutil.rmtree(tmp, ignore_errors=True)
print('all passed' if not fails else f'FAILED: {fails}')
sys.exit(1 if fails else 0)
