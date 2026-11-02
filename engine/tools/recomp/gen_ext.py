"""gen.py with extension ranges: same arguments as gen.py; the funcs JSON may carry `ext` = {"0x<function>": [["0x<a>",
"0x<b>"],...]}: code outside the function's FDE that is part of it. TotK's game patches put veneers in the text padding
(no FDE), entered by a `b` from the middle of a function and left by a `b` back into it; gen.py would turn both
branches into call_ind traps. Here the FDE range and its extension ranges are decoded as one body, and branches between
them are labels. It may also carry `switch` (see ExtEmitter): switch sites, words chosen at run time (the mods' code
cheats, switched live in the F10 Cheats tab).

A wrapper, not an edit of gen.py: gen.py is part of every shared game-object key (tools/engine/build.py), so only the
patched keys that need this pay for it. Functions without `ext` go through gen.py's own code (byte-identical output,
checked on 303 functions); the extended copy of Emitter.function below is refused when gen.py's function / tail
change, so it cannot drift silently."""
import ast, hashlib, inspect, json, os, re, sys, textwrap

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen as G   # noqa: E402


def shape(fn):
    """sha256[:16] of a method's code, not its text: comments, blank lines, layout and docstrings do not count
    (tools/release/export_public.py rewrites comments inside gen.py; a hash of the text refused every public build)."""
    tree = ast.parse(textwrap.dedent(inspect.getsource(fn)))
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.body and \
                isinstance(node.body[0], ast.Expr) and isinstance(getattr(node.body[0], 'value', None), ast.Constant) \
                and isinstance(node.body[0].value.value, str):
            node.body = node.body[1:] or [ast.Pass()]
    return hashlib.sha256(ast.dump(tree).encode()).hexdigest()[:16]


UPSTREAM = {'function': 'b97cdeca74c6b20e', 'tail': 'b06f536b1ecf4910'}   # shape() of the gen.py methods this copies
for name, want in UPSTREAM.items():
    have = shape(getattr(G.Emitter, name))
    if have != want:
        sys.exit(f'gen_ext.py: gen.py Emitter.{name} changed ({have}, expected {want}): port the change into gen_ext.py')


class ExtEmitter(G.Emitter):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        funcs = json.load(open(sys.argv[2]))
        self.ext = {int(k, 16): [(int(x, 16), int(y, 16)) for x, y in v]
                    for k, v in funcs.get('ext', {}).items()}
        # `switch` = {"0x<address>": [[<slot>, "<word, hex bytes>"], ...]}: switch sites (the mods' code cheats,
        # tools/engine/mods_exefs.py): the word of the first alternative whose run-time flag is on
        # (nxr_code_switch(<slot>): engine/runtime/mods_exefs.cpp, the F10 Cheats tab), else the image's own
        self.switch = {int(k, 16): [(int(s), bytes.fromhex(w)) for s, w in v] for k, v in funcs.get('switch', {}).items()}
        self.has_switch = False   # (the function being emitted has switch sites)
        self.ranges = []

    def in_function(self, t):
        return any(a <= t < b for (a, b) in self.ranges)

    def abi_dead_ok(self, a, part):
        """--lso-abi: a function with switch sites keeps x9-x18 and NZCV at every LSO, as one with extension ranges does
        (abi_scan.py reads the image's words, never an alternative's: a cheat's call may rely on them)"""
        return not self.has_switch and super().abi_dead_ok(a, part)

    def switch_alts(self, a):
        """(slot, mnemonic, operands) of each alternative at switch site a"""
        out = []
        for slot, word in self.switch[a]:
            d = list(self.md.disasm_lite(word, a))
            out.append((slot, d[0][2], d[0][3]) if d else (slot, '.byte', ''))
        return out

    def switched(self, a, m, o, jt):
        """a switch site's code: each alternative under its flag, the image's word last. A fused compare (--fcfuse)
        never reaches across it. An alternative that cannot be lifted stops the build (its cheat would trap)."""
        parts = []
        for slot, m2, o2 in self.switch_alts(a):
            self.fc = None
            try:
                code = self.insn(a, m2, o2, None)
            except (G.Unhandled, ValueError, KeyError, IndexError) as e:
                sys.exit(f'gen_ext.py: switch site {a:#x}: {m2} {o2} (slot {slot}) cannot be lifted ({e})')
            parts.append(f'if (nxr_code_switch({slot})) {{ {code} }}')
        self.fc = None
        code = self.insn(a, m, o, jt)
        self.fc = None
        self.stats['switch_sites'] += 1
        return ' else '.join(parts) + f' else {{ {code} }}'

    def tail(self, t, pc):
        if self.ranges[1:] and self.in_function(t) and t in self.labels:
            return f'goto L_{t:x};'
        return super().tail(t, pc)

    def function(self, start):
        self.ranges = [(start, start + self.fde[start])] + self.ext.get(start, [])
        sites = sorted(s for s in self.switch if self.in_function(s))
        self.has_switch = bool(sites)
        if len(self.ranges) == 1 and not sites:
            return super().function(start)
        # gen.py's Emitter.function, with the extension ranges decoded after the body (never --split)
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
        for s in sites:   # (an alternative's branch into the function: a label too)
            for _, m, o in self.switch_alts(s):
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
                    if a in self.switch:   # (falls through when any of its words does)
                        code = self.switched(a, m, o, table.get(a))
                        if any(m2 not in ('ret', 'b', 'br', '.byte') for _, m2, _ in self.switch_alts(a)):
                            m = 'nop'
                    else:
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
            if part and part[-1][0] in self.switch and \
                    any(m2 not in ('ret', 'b', 'br', '.byte') for _, m2, _ in self.switch_alts(part[-1][0])):
                last = 'nop'
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
        # (the switch sites' flags, engine/runtime/mods_exefs.cpp: a declaration line in the function's block, the
        # form tools/engine/tusplit.py moves to its TU's declarations)
        decl = 'int nxr_code_switch(unsigned slot);\n' if sites else ''
        # --alias, as in gen.py (another game version: the 1.0.0 name f_<old> forwards here)
        old = self.alias.get(start)
        names = ('',) if start in self.hooks else ('', '_orig')
        # each forwarder is a block of its own (a blank line, then up to a `}` line), as tools/engine/tusplit.py
        # parses a TU
        fwd = ''.join(f'\nvoid {ALIAS_FN}{old:x}{sfx}(Ctx* __restrict c) {{\n{FN}{start:x}(c);\n}}\n'
                      for sfx in names) if old is not None else ''
        hname = f'{ALIAS_FN}{old:x}' if old is not None else f'{FN}{start:x}'
        if start in self.hooks:
            return decl + (f'void hook_{hname}(Ctx* __restrict c);\n'
                    f'void {FN}{start:x}(Ctx* __restrict c) {{ hook_{hname}(c); }}\n'
                    f'void {hname}_orig(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd)
        if start in self.timed:
            return decl + (f'void {FN}{start:x}_t(Ctx* __restrict cx);\n'
                    f'void {FN}{start:x}(Ctx* __restrict cx) {{ u64 t_ = __builtin_ia32_rdtsc(); {FN}{start:x}_t(cx); '
                    f'rt_time(IMGBASE + {start:#x}ull, __builtin_ia32_rdtsc() - t_); }}\n'
                    f'__attribute__((noinline)) void {FN}{start:x}_t(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd)
        return decl + f'void {FN}{start:x}(Ctx* __restrict cx) {{\n' + text + '\n}\n' + fwd


G.Emitter = ExtEmitter

if __name__ == '__main__':
    G.main()
