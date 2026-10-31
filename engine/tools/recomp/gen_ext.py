"""gen.py with extension ranges: same arguments as gen.py; the funcs JSON may carry `ext` = {"0x<function>": [["0x<a>",
"0x<b>"],...]}: code outside the function's FDE that is part of it. TotK's game patches put veneers in the text padding
(no FDE), entered by a `b` from the middle of a function and left by a `b` back into it; gen.py would turn both
branches into call_ind traps. Here the FDE range and its extension ranges are decoded as one body, and branches between
them are labels.

A wrapper, not an edit of gen.py: gen.py is part of every shared game-object key (tools/engine/build.py), so only the
patched keys that need this pay for it. Functions without `ext` go through gen.py's own code (byte-identical output,
checked on 303 functions); the extended copy of Emitter.function below is refused when gen.py's function / tail
change, so it cannot drift silently."""
import hashlib, inspect, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen as G   # noqa: E402

UPSTREAM = {'function': '002a1464061d806f', 'tail': '58cb5091eaf5fad8'}   # sha256[:16] of the methods this copies
for name, want in UPSTREAM.items():
    have = hashlib.sha256(inspect.getsource(getattr(G.Emitter, name)).encode()).hexdigest()[:16]
    if have != want:
        sys.exit(f'gen_ext.py: gen.py Emitter.{name} changed ({have}, expected {want}): port the change into gen_ext.py')


class ExtEmitter(G.Emitter):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.ext = {int(k, 16): [(int(x, 16), int(y, 16)) for x, y in v]
                    for k, v in json.load(open(sys.argv[2])).get('ext', {}).items()}
        self.ranges = []

    def in_function(self, t):
        return any(a <= t < b for (a, b) in self.ranges)

    def tail(self, t, pc):
        if self.ranges[1:] and self.in_function(t) and t in self.labels:
            return f'goto L_{t:x};'
        return super().tail(t, pc)

    def function(self, start):
        self.ranges = [(start, start + self.fde[start])] + self.ext.get(start, [])
        if len(self.ranges) == 1:
            return super().function(start)
        # gen.py's Emitter.function, with the extension ranges decoded after the body
        size = self.fde[start]
        end = start + size
        insns = list(self.md.disasm_lite(self.img[start:end], start))
        ext = [list(self.md.disasm_lite(self.img[a:b], a)) for (a, b) in self.ranges[1:]]
        self.cur_refs = set()
        self.cur_imps = set()
        labels = set()
        jt = {}
        adr_t = sorted({t for (a, sz, m, o) in insns if m == 'adr'
                        for t in [G.imm(G.split_ops(o)[-1])] if start <= t < end})
        for i, (a, sz, m, o) in enumerate(insns):
            if m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                t = G.imm(G.split_ops(o)[-1])
                if self.in_function(t):
                    labels.add(t)
            elif m == 'br':
                tab = self.jump_table(insns, i, start, end)
                if tab:
                    jt[a] = tab
                    labels.update(tab)
                    self.stats['br_jumptable'] += 1
                elif adr_t:
                    jt[a] = adr_t
                    labels.update(adr_t)
                    self.stats['br_adr'] += 1; self.adr_sites.append(a)
                else:
                    self.stats['br_indirect'] += 1
        for part in ext:
            for (a, sz, m, o) in part:
                if m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                    t = G.imm(G.split_ops(o)[-1])
                    if self.in_function(t):
                        labels.add(t)
        self.labels = labels
        self.fstart, self.fend = start, end
        items = []
        for part, (a0, a1), table in [(insns, (start, end), jt)] + [(p, r, {}) for p, r in zip(ext, self.ranges[1:])]:
            for (a, sz, m, o) in part:
                try:
                    code = self.insn(a, m, o, table.get(a))
                except (G.Unhandled, ValueError, KeyError, IndexError) as e:
                    shape = re.sub(r'\b[xw]\d+\b', 'R', o)
                    shape = re.sub(r'#-?[0-9a-fx.e+]+', '#', shape)
                    key = f'{m} {shape}'
                    self.unhandled[key] += 1
                    self.unhandled_ex.setdefault(key, f'{a:#x}: {m} {o} ({e})')
                    code = f'{{ LSO; rt_trap(cx, IMGBASE + {a:#x}ull, "unhandled: {m} {o}"); }}'
                    m = '.byte'
                self.stats['insns'] += 1
                items.append((a, code, m not in ('ret', 'b', 'br', '.byte')))
            last = part[-1][2] if part else ''           # fall-through off the end of the body or of a range
            if last not in ('ret', 'b', 'br'):
                items.append((a1, self.tail(a1, a1), False))
        self.stats['ext_functions'] += 1
        codes = self.lctx_sync([it[0] for it in items], [it[1] for it in items], [it[2] for it in items])
        body = [f'rt_trace(cx, IMGBASE + {start:#x}ull, 0);'] if start in self.trace else []
        for (a, _, _), code in zip(items, codes):
            if a in labels:
                body.append(f'L_{a:x}:;')
            body.append(f'/*{a:x}*/ {code}')
        text = self.prologue(start) + '\n'.join(body)
        FN, ALIAS_FN = G.FN, G.ALIAS_FN
        # --alias, as in gen.py (another game version: the 1.0.0 name f_<old> forwards here)
        old = self.alias.get(start)
        names = ('',) if start in self.hooks else ('', '_orig')
        fwd = ''.join(f'void {ALIAS_FN}{old:x}{sfx}(Ctx* __restrict c) {{ {FN}{start:x}(c); }}\n'
                      for sfx in names) if old is not None else ''
        hname = f'{ALIAS_FN}{old:x}' if old is not None else f'{FN}{start:x}'
        if start in self.hooks:
            return (f'void hook_{hname}(Ctx* __restrict c);\n'
                    f'void {FN}{start:x}(Ctx* __restrict c) {{ hook_{hname}(c); }}\n'
                    f'void {hname}_orig(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd)
        if start in self.timed:
            return (f'void {FN}{start:x}_t(Ctx* __restrict cx);\n'
                    f'void {FN}{start:x}(Ctx* __restrict cx) {{ u64 t_ = __builtin_ia32_rdtsc(); {FN}{start:x}_t(cx); '
                    f'rt_time(IMGBASE + {start:#x}ull, __builtin_ia32_rdtsc() - t_); }}\n'
                    f'__attribute__((noinline)) void {FN}{start:x}_t(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd)
        return f'void {FN}{start:x}(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd


G.Emitter = ExtEmitter

if __name__ == '__main__':
    G.main()
