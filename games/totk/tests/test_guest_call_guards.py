"""Every call the runtime makes INTO main must have a place in another game version's table, or be guarded (public #72's
class: on 1.4.3 an unplaced address is the sentinel main+0xfffffff0, and a call to it traps at start or in play).

A call into main is a DECOMP_GUEST_FN's call, or a totk_main(<off>) (directly, or through a constant bound to one) passed
as the target of guest_call / call_ind or of a file's own helper that forwards its argument to them. For each one in
the player's tree the target must be PLACED on 1.4.3, which
means pinned in pins_v143.json or in PLACED_V143 (the reference engine-map's placed addresses), or every call site
must be guarded:
  - a totk_main_known(<off>) / totk_main_known_rt(<off>) earlier in the same function, or
  - the call, or its declaration, inside a 1.0.0-only preprocessor branch (TOTK_GAME_VERSION == 100).
Source text only, no mains (seconds). A new guest function fails here until it is pinned, guarded, or its reference
placement is added to PLACED_V143 (refresh: python -m games.totk.tests.test_guest_call_guards --refresh
<engine-map --report JSON> prints the dict for the current sources; refresh it whenever engine_map's placement logic
changes). A call target must rest on aligned / exact evidence, a pin, or be the start of a porter-confirmed function
pair ('pair start'); a 'fuzzy' placement counts as unplaced (a wrong one calls the wrong function, worse than a trap).
Limits: a helper's forwarding is seen only in the first 400 characters of its body; macros taking an offset (MAIN_U64
style) are not followed (they guard themselves); a guard on another branch of the same function counts (it is constant
per build, so a gate there is valid, but the call may sit outside it); data READS of unplaced totk_main constants are not
checked here (a follow-up).
    python -m games.totk.tests.test_guest_call_guards"""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TREES = ('engine', 'games/totk/runtime', 'games/totk/engine', 'games/totk/include')
EXTS = ('.c', '.cc', '.cpp', '.h', '.hpp', '.inc')

# 1.4.3 placements of the call targets the sources name, from the reference engine-map --beta report at the v0.1.9.1
# cut: offset -> how the porter placed it. Pinned functions are not listed (pins_v143.json is read). A target missing
# here and from the pins counts as UNPLACED.
PLACED_V143 = {
    0xa206b0: 'aligned', 0xb60880: 'aligned', 0xb906ec: 'exact', 0xb907a0: 'aligned', 0xb90810: 'aligned',
    0xde5ce0: 'aligned', 0x17333b0: 'aligned', 0x19eaaf8: 'aligned', 0x19ebf28: 'pair start', 0x1a829fc: 'exact',
    0x1a85d24: 'aligned', 0x1a85d64: 'exact', 0x1a85d84: 'aligned', 0x1a861f8: 'pair start', 0x1a90b2c: 'aligned',
    0x1cc9510: 'aligned', 0x1ccadcc: 'exact', 0x1cf6334: 'fuzzy', 0x21225a8: 'fuzzy', 0x370c21f: 'string',
    0x4376e68: 'vtable', 0x43822a8: 'vtable', 0x44fc2c8: 'data1', 0x44fd0b0: 'vtable',
}
SOLID = {'aligned', 'exact', 'pair start', 'string', 'vtable', 'data1'}   # (not 'fuzzy': see the module doc)

DECL = re.compile(r'DECOMP_GUEST_FN\(\s*(\w+)\s*,\s*([0-9a-fA-F]{10})')
BIND = re.compile(r'\b(k\w+|\w+)\s*=\s*totk_main\(\s*(0x[0-9a-fA-F]+)\s*\)')
KNOWN = r'totk_main_known(?:_rt)?\(\s*{off}\s*\)'
HEAD = re.compile(r'^[A-Za-z_][\w:<>*& ]{0,80}?\b(\w+)\(([^()]{0,200})\)\s*(?:const\s*)?\{')
V100_IF = re.compile(r'#\s*if\b.*TOTK_GAME_VERSION\s*==\s*100')


def sources():
    out = {}
    for t in TREES:
        for p in sorted((ROOT / t).rglob('*')):
            if p.suffix in EXTS and p.is_file():
                out[p.relative_to(ROOT).as_posix()] = p.read_text(encoding='utf-8', errors='replace')
    return out


def sources_rev(rev):
    """sources() as committed in rev (git ls-tree + one cat-file --batch: the hook checks the commit main moves to,
    not a checkout)"""
    import subprocess
    names = subprocess.run(['git', 'ls-tree', '-r', '--name-only', rev, '--', *TREES], capture_output=True, text=True,
                           check=True).stdout.split('\n')
    names = [n for n in names if n.endswith(EXTS)]
    req = ''.join(f'{rev}:{n}\n' for n in names).encode()
    raw = subprocess.run(['git', 'cat-file', '--batch'], input=req, capture_output=True, check=True).stdout
    out, pos = {}, 0
    for n in names:
        head_end = raw.index(b'\n', pos)
        size = int(raw[pos:head_end].split()[2])
        out[n] = raw[head_end + 1:head_end + 1 + size].decode('utf-8', 'replace')
        pos = head_end + 1 + size + 1
    return out


def pins_rev(rev):
    import subprocess
    d = json.loads(subprocess.run(['git', 'show', f'{rev}:games/totk/patches/pins_v143.json'], capture_output=True,
                                  text=True, check=True).stdout)
    return {int(k, 16) for k, v in d.items() if k.startswith('0x') and isinstance(v, dict) and 'new' in v}


def strip_comments(text):
    """the text with // and /* */ comments blanked (line numbers kept)"""
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def v100_only(lines, i):
    """line i sits inside the 1.0.0 branch of an #if on TOTK_GAME_VERSION == 100 (not its #else)"""
    depth = 0
    for j in range(i - 1, -1, -1):
        l = lines[j].strip()
        if l.startswith('#endif'):
            depth += 1
        elif l.startswith('#if'):
            if depth == 0:
                return bool(V100_IF.search(l)) and '!=' not in l
            depth -= 1
        elif l.startswith(('#else', '#elif')) and depth == 0:
            return False   # (the other branch of whatever #if this is)
    return False


def func_start(lines, i):
    """the first line of the top-level definition holding line i (a line at column 0 that is not a preprocessor line,
    a brace, a namespace or an extern block)"""
    for j in range(i, -1, -1):
        l = lines[j]
        if l and not l[0].isspace() and not l.startswith(('#', '}', '{', 'namespace', 'extern "C" {', 'using ')) \
                and not re.match(r'^(static\s+)?(constexpr|const)\b', l):
            return j
    return 0


def guarded(lines, i, off):
    s = func_start(lines, i)
    pat = re.compile(KNOWN.format(off=hex(off)), re.I)
    return any(pat.search(lines[k]) for k in range(s, i + 1))


def placed(off, pins):
    return PLACED_V143.get(off) in SOLID or off in pins


def shipped_pins():
    d = json.loads((ROOT / 'games/totk/patches/pins_v143.json').read_text())
    return {int(k, 16) for k, v in d.items() if k.startswith('0x') and isinstance(v, dict) and 'new' in v}


def problems(srcs, pins):
    """[(path:line, target offset, what)] for every unguarded call of an unplaced target"""
    out = []
    for path, raw in srcs.items():
        text = strip_comments(raw)
        lines = text.split('\n')
        # the targets this file names: guest functions and totk_main constants
        decls = {}
        for m in DECL.finditer(text):
            i = text.count('\n', 0, m.start())
            decls[m.group(1)] = (int(m.group(2), 16) - 0x1000000000, i, v100_only(lines, i))
        binds = {m.group(1): int(m.group(2), 16) for m in BIND.finditer(text)}
        # helpers that forward a parameter into call_ind / guest_call: name -> that parameter's index
        helpers = {'call_ind': 1, 'guest_call': 1}
        offs = [0]
        for ln in lines:
            offs.append(offs[-1] + len(ln) + 1)
        for li, ln in enumerate(lines):   # (one line at a time: a definition's head 'type name(params) {')
            m = HEAD.match(ln)
            if not m:
                continue
            params = [p.strip().split()[-1].lstrip('*&') if p.strip() else '' for p in m.group(2).split(',')]
            body = text[offs[li + 1]:offs[li + 1] + 400]
            for k, p in enumerate(params):
                if p and re.search(rf'\b(call_ind|guest_call)\(\s*\w+\s*,\s*{re.escape(p)}\s*\)', body):
                    helpers.setdefault(m.group(1), k)
        for name, (off, di, dv100) in decls.items():
            if placed(off, pins) or dv100:
                continue
            for m in re.finditer(rf'\b{name}\s*\(', text):
                i = text.count('\n', 0, m.start())
                if i == di or v100_only(lines, i) or guarded(lines, i, off):
                    continue
                out.append((f'{path}:{i + 1}', hex(off), f'{name}() (DECOMP_GUEST_FN)'))
        for h, k in helpers.items():
            for m in re.finditer(rf'\b{h}\s*\(', text):
                # the k-th argument (top-level commas only)
                j, depth, args, cur = m.end(), 1, [], ''
                while j < len(text) and depth:
                    ch = text[j]
                    depth += ch == '('
                    depth -= ch == ')'
                    if depth and ch == ',' and depth == 1:
                        args.append(cur.strip())
                        cur = ''
                    elif depth:
                        cur += ch
                    j += 1
                args.append(cur.strip())
                if k >= len(args):
                    continue
                a = args[k]
                mm = re.fullmatch(r'totk_main\(\s*(0x[0-9a-fA-F]+)\s*\)', a)
                off = int(mm.group(1), 16) if mm else binds.get(a)
                if off is None or placed(off, pins):
                    continue
                i = text.count('\n', 0, m.start())
                if v100_only(lines, i) or guarded(lines, i, off):
                    continue
                out.append((f'{path}:{i + 1}', hex(off), f'{h}(..., {a})'))
    return sorted(set(out))


class GuestCallGuardTest(unittest.TestCase):
    def test_no_unguarded_unplaced_call(self):
        bad = problems(sources(), shipped_pins())
        self.assertEqual(bad, [], 'calls into main with no place in 1.4.3 and no totk_main_known guard (#72):\n' +
                         '\n'.join(f'  {w}: {o} {t}' for w, o, t in bad))

    def test_catches_the_jump_option_shape(self):
        """the 11-04 case: a bridge task calling an unplaced getter with no guard, then the same with one"""
        decl = 'namespace {\nDECOMP_GUEST_FN(Getter, 1001a3e83c, bool());\n}\n'
        bad = decl + 'static void task(void*, bool ran) {\n    if (ran) set(Getter() ? 1 : 0);\n}\n'
        good = decl + 'static void task(void*, bool ran) {\n    if (ran && totk_main_known(0x1a3e83c)) set(Getter());\n}\n'
        self.assertEqual([p[1] for p in problems({'x.cpp': bad}, set())], ['0x1a3e83c'])
        self.assertEqual(problems({'x.cpp': good}, set()), [])
        self.assertEqual(problems({'x.cpp': bad}, {0x1a3e83c}), [])   # (pinned: placed)

    def test_fuzzy_is_not_placed(self):
        """a call target placed only 'fuzzy' (not a confirmed pair's start) needs a guard or a pin"""
        src = 'constexpr u64 kHit = totk_main(0x1cf6334);\nvoid f(Ctx *c) {\n    call_ind(c, kHit);\n}\n'
        self.assertEqual([p[1] for p in problems({'z.cpp': src}, set())], ['0x1cf6334'])

    def test_helpers_constants_and_branches(self):
        src = ('constexpr u64 kFn = totk_main(0x123450);\n'
               'u64 call(Ctx *c, u64 fn, u64 a0) {\n    c->x[0] = a0;\n    call_ind(c, fn);\n    return c->x[0];\n}\n'
               'void a(Ctx *c) {\n    call(c, kFn, 1);\n}\n'
               'void b(Ctx *c) {\n    if (!totk_main_known(0x123450)) return;\n    call_ind(c, kFn);\n}\n'
               '#if !defined(TOTK_GAME_VERSION) || TOTK_GAME_VERSION == 100\n'
               'void d(Ctx *c) {\n    guest_call(c, totk_main(0x123450));\n}\n#endif\n')
        self.assertEqual([p[0] for p in problems({'y.cpp': src}, set())], ['y.cpp:8'])


def refresh(report):
    """the PLACED_V143 dict for the current sources from an engine-map --report JSON"""
    addrs = json.load(open(report))['addresses']
    pins = shipped_pins()
    want = set()
    for path, raw in sources().items():
        text = strip_comments(raw)
        want |= {int(m.group(2), 16) - 0x1000000000 for m in DECL.finditer(text)}
        want |= {int(m.group(2), 16) for m in BIND.finditer(text)}
        want |= {int(x, 16) for x in re.findall(r'(?:call_ind|guest_call|call)\(\s*\w+\s*,\s*totk_main\(\s*(0x[0-9a-fA-F]+)', text)}
    got = {}
    for o in sorted(want - pins):
        e = addrs.get(hex(o), {})
        if e.get('how'):   # (a fuzzy place that is the start of a confirmed pair: the pair's start, as solid as a pin)
            start = (e['how'] == 'fuzzy' and e.get('func_old') == e.get('old') and e.get('func_new') == e.get('new')
                     and e.get('confirmed'))
            got[o] = 'pair start' if start else e['how']
    print('PLACED_V143 = {' + ', '.join(f'{o:#x}: {h!r}' for o, h in got.items()) + '}')


def check_rev(rev):
    """tools/git-hooks/reference-transaction's call: 0 none, 3 problems (printed), 2 the check itself failed (3, not 1:
    a Python error such as a SyntaxError in this file exits 1, which the hook must not read as problems)"""
    try:
        bad = problems(sources_rev(rev), pins_rev(rev))
    except Exception as e:   # (never block a landing on the checker's own failure: the hook lets it through)
        print(f'test_guest_call_guards: the check failed ({type(e).__name__}: {e})', file=sys.stderr)
        return 2
    for w, o, t in bad:
        print(f'  {w}: {o} {t}', file=sys.stderr)
    return 3 if bad else 0


if __name__ == '__main__':
    if sys.argv[1:2] == ['--refresh']:
        refresh(sys.argv[2])
    elif sys.argv[1:2] == ['--rev']:
        sys.exit(check_rev(sys.argv[2]))
    else:
        unittest.main()
