"""gen.py Emitter.split_text with --split-loops (build.py SPLIT_LOOPS): a hot function is cut only where no loop
crosses, so every chunk switch goes forward (at most chunks - 1 a call, none per iteration); --split's count cuts
stay as they were. Synthetic items, no image. A second or two.

    python -I engine/tools/recomp/test_split_loops.py
"""
import collections, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


START = 0x1000


def addr(i):
    return START + 4 * i


def emitter(loops):
    em = gen.Emitter.__new__(gen.Emitter)   # (split_text's own state only: no image)
    em.split_chunk, em.split_loops = 100, ({START} if loops else set())
    em.stats = collections.Counter()
    em.lctx_sync = lambda addrs, codes, falls: codes
    em.prologue = lambda start: ''
    return em


# 1000 instructions; loop A: item 450 branches back to item 10, loop B: item 900 back to item 520; forward branches
# from item 5 to item 700 and from item 460 to item 950
back = {450: 10, 900: 520}
fwd = {5: 700, 460: 950}
items, labels = [], set()
for i in range(1000):
    t = back.get(i, fwd.get(i))
    code = f'if (c->x[0]) goto L_{addr(t):x};' if t is not None else '{ c->x[1] += 1; }'
    if t is not None:
        labels.add(addr(t))
    items.append((addr(i), code, True))


def chunks(text):
    """{chunk: (its first instruction's index, the chunks it tail-calls, its gotos' target indexes, its indexes)}"""
    out = {}
    for m in re.finditer(r'\nvoid f_1000_c(\d+)\(Ctx\* __restrict cx\) \{(.*?)\n\}', text, re.S):
        k, body = int(m.group(1)), m.group(2)
        own = [(int(a, 16) - START) // 4 for a in re.findall(r'/\*([0-9a-f]+)\*/', body)]
        gotos = [(int(t, 16) - START) // 4 for t in re.findall(r'goto L_([0-9a-f]+);', body)]
        out[k] = (own[0], sorted({int(j) for j in re.findall(r'return f_1000_c(\d+)\(cx\)', body)}), gotos, set(own))
    return out


text = emitter(True).split_text(START, items, labels)
# tools/engine/tusplit.py reads a function as the lines up to a bare `}` and names it by its first definition: the
# entry and all its chunks must be ONE such block (a --hot function's chunks then share its h_ TU, -O2), so only the
# last chunk may end on a bare `}`, the others on `} // f_<off>_c<k>`
lines = text.split('\n')
bare = [i for i, ln in enumerate(lines) if ln == '}']
ends = re.findall(r'^\} // (.+)$', text, re.M)
check('one block for tusplit: a single bare } (the last line of the last chunk), the other chunks end on } // name',
      len(bare) == 1 and all(ln == '' for ln in lines[bare[0] + 1:]) and
      ends == ['3 chunks', 'f_1000_c0', 'f_1000_c1'], f'bare at {bare} of {len(lines)}; {ends}')
c = chunks(text)
check('loops: cut only at loop-free points (after loop A at 451, after loop B at 901): 3 chunks',
      sorted(v[0] for v in c.values()) == [0, 451, 901], f'{sorted(v[0] for v in c.values())}')
check('loops: every chunk switch goes forward', all(all(j > k for j in v[1]) for k, v in c.items()),
      f'{ {k: v[1] for k, v in c.items()} }')
check('loops: each goto stays in its chunk (the loops whole: A in chunk 0, B in chunk 1)',
      all(set(v[2]) <= v[3] for v in c.values()) and 10 in c[0][2] and 520 in c[1][2],
      f'{ {k: v[2] for k, v in c.items()} }')
text = emitter(False).split_text(START, items, labels)
c = chunks(text)
check('--split (count cuts) unchanged: every C + C // 4 (no instruction ends a run here), loops cut through',
      sorted(v[0] for v in c.values()) == [0, 125, 250, 375, 500, 625, 750, 875], f'{sorted(v[0] for v in c.values())}')
check('--split: a loop crossing chunks switches backward', any(any(j < k for j in v[1]) for k, v in c.items()),
      f'{ {k: v[1] for k, v in c.items()} }')
# a function whose loops leave no free point past C: not split (None), as a function too small for --split
one = [(addr(i), f'if (c->x[0]) goto L_{addr(0):x};' if i == 999 else '{ }', True) for i in range(1000)]
check('loops: one loop around the whole function: not split', emitter(True).split_text(START, one, {addr(0)}) is None)
if fails:
    sys.exit(f'{len(fails)} failed: {fails}')
print('all passed')
