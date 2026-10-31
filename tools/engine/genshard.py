"""One share of gen.py's functions (build.py `game --split N` runs several at once; gen.py is single-threaded).

usage: python genshard.py <gen.py|gen_ext.py> <i> <n> <img_prefix> <funcs.json> <outdir> [gen args...]
       python genshard.py --merge <outdir> <shard dir>...

Share i of n is a contiguous range of the functions in address order, cut at equal instruction counts. gen.py runs
in this process with `--only <that range>` (a list too long for a command line), unchanged: each function's text
depends only on the image, funcs.json and the arguments, not on which others are emitted with it (no state carries
between functions), and tusplit.py re-cuts the TUs anyway, so the split output of n shares is the split output of
one run, byte for byte.
--merge writes what the shares hold for the whole module, into <outdir>: their tu_*.cpp renumbered, the ftab.cpp
of all the functions (each share's lists the functions it emitted, in address order, so the concatenation is
gen.py's whole-run ftab.cpp), imports.cpp (the same in every share: all of the module's imports) and a report.json
with the counts summed.
"""
import collections, json, os, re, runpy, shutil, sys


def share(order, size, i, n):
    total = sum(size[s] for s in order)
    lo, hi = total * i // n, total * (i + 1) // n
    acc, out = 0, []
    for s in order:
        if lo <= acc < hi:
            out.append(s)
        acc += size[s]
    return out


def run(tool, i, n, img, fj, outdir, args):
    fde = {int(k, 16): v for k, v in json.load(open(fj))['fde'].items()}
    mine = share(sorted(fde), {s: v // 4 for s, v in fde.items()}, i, n)
    if not mine:
        os.makedirs(outdir, exist_ok=True); return
    sys.argv = [tool, img, fj, outdir] + args + ['--only', ','.join(f'{s:x}' for s in mine)]
    sys.path.insert(0, os.path.dirname(os.path.abspath(tool)))
    runpy.run_path(tool, run_name='__main__')


def ftab_parts(text):
    """gen.py's ftab.cpp -> (head, declarations, table head, entries, tail)"""
    m = re.fullmatch(r'(// generated\n#include "rt.h"\n)((?:void .*\n)*)(struct FtabEnt .*\n.*= \{\n)((?:\{.*\},\n)*)(\};\n.*\n)',
                     text)
    if not m:
        raise ValueError('ftab.cpp: not in the form genshard.py knows (gen.py changed?)')
    return m.groups()


def merge(outdir, shards):
    os.makedirs(outdir, exist_ok=True)
    k = 0
    ftabs, imports, reports = [], set(), []
    for d in shards:
        for fn in sorted(os.listdir(d)):
            p = os.path.join(d, fn)
            if re.fullmatch(r'tu_\d+\.cpp', fn):
                shutil.move(p, os.path.join(outdir, f'tu_{k:05d}.cpp')); k += 1
            elif fn == 'ftab.cpp':
                ftabs.append(ftab_parts(open(p).read()))
            elif fn == 'imports.cpp':
                imports.add(open(p).read())
            elif fn == 'report.json':
                reports.append(json.load(open(p)))
    if len(imports) != 1:
        raise ValueError(f'imports.cpp differs between shares ({len(imports)} versions)')
    if len({(f[0], f[2], f[4]) for f in ftabs}) != 1:
        raise ValueError('ftab.cpp head or tail differs between shares')
    f0 = ftabs[0]
    # (gen.py's Out writes in text mode; so do these)
    with open(os.path.join(outdir, 'ftab.cpp'), 'w') as f:
        f.write(f0[0] + ''.join(f[1] for f in ftabs) + f0[2] + ''.join(f[3] for f in ftabs) + f0[4])
    with open(os.path.join(outdir, 'imports.cpp'), 'w') as f:
        f.write(imports.pop())
    rep = dict(reports[0])
    for key in ('functions', 'insns', 'tus', 'unhandled_total'):
        rep[key] = sum(r[key] for r in reports)
    rep['stats'] = dict(sum((collections.Counter(r['stats']) for r in reports), collections.Counter()))
    unh = collections.Counter()
    for r in reports:
        for key, n in r['unhandled']:
            unh[json.dumps(key)] += n
    rep['unhandled'] = [[json.loads(key), n] for key, n in unh.most_common()]
    rep['adr_sites'] = [a for r in reports for a in r['adr_sites']]
    rep['shares'] = len(reports)
    json.dump(rep, open(os.path.join(outdir, 'report.json'), 'w'), indent=1)


if __name__ == '__main__':
    if sys.argv[1] == '--merge':
        merge(sys.argv[2], sys.argv[3:])
    else:
        run(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), *sys.argv[4:7], sys.argv[7:])
