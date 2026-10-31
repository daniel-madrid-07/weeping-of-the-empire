"""Re-partition gen.py's TUs (build.py `game --split N`): stable boundaries and a hot/cold split.

gen.py cuts a TU every 60000 guest instructions in address order, so a function that grows, shrinks or is added
(a patch, a veneer) moves every later boundary and the whole module recompiles. Here the functions of a module are
cut where the function's own name says so (content-defined chunking: a cut before f when hash(f) % M == 0 and the
TU holds at least N/2 instructions, or when f would take it past 2N), so a change moves boundaries only up to the
next anchor and a TU's text changes only when one of its functions (or the declarations they need) changes. A
function bigger than 2N is a TU of its own, which also spreads the few giant functions over several jobs.

With a hot set (function names, one per line), hot functions are chunked apart into h_<addr>.cpp and the rest into
c_<addr>.cpp, which build.py compiles at -O1; without one every TU is t_<addr>.cpp.

The text of every function is gen.py's, byte for byte: each input TU is checked to parse back to itself, and every
input function lands in exactly one output TU. Only the TU boundaries and the forward declarations change.
"""
import hashlib, os, re, shutil

ANCHOR_INSNS = 160        # assumed mean function size: anchors every ~N/2 instructions (a constant, never measured,
                          # so the anchors depend on N alone)
DEF = re.compile(r'^void (\w+)\(.*\) \{')
DECL = re.compile(r'^(?:[A-Za-z_]\w*[\s*]+)+(\w+)\s*\(.*\);$')
IDENT = re.compile(r'\b[A-Za-z_]\w*\b')


class Module:
    """one gen.py output directory: the common header, the declarations and the function texts"""

    def __init__(self, d):
        self.pre = None
        self.decl = {}            # name -> declaration line
        self.funcs = {}           # name -> its block: from `void ...{` to `}` (no trailing newline)
        self.defs = {}            # name -> the functions its block defines (itself, or a hook's wrapper and _orig)
        self.other = []           # files passed through unchanged (ftab.cpp, imports.cpp, report.json, ...)
        self.eol = None           # gen.py's line ending (CRLF on Windows), kept
        for fn in sorted(os.listdir(d)):
            if not re.fullmatch(r'tu_\d+\.cpp', fn):
                self.other.append(fn)
                continue
            self.parse(os.path.join(d, fn))

    def parse(self, path):
        text = open(path, newline='').read()
        eol = '\r\n' if '\r\n' in text else '\n'
        if self.eol is None:
            self.eol = eol
        elif eol != self.eol:
            raise ValueError(f'{path}: line endings differ from the other TUs of the module')
        lines = text.split(eol)
        i = 0
        while i < len(lines) and lines[i].startswith(('//', '#')):
            i += 1
        pre = lines[:i]
        if self.pre is None:
            self.pre = pre
        elif pre != self.pre:
            raise ValueError(f'{path}: header differs from the other TUs of the module')
        decls = []
        while i < len(lines) and lines[i] != '':
            self.declare(lines[i], f'{path}:{i + 1}')
            decls.append(lines[i]); i += 1
        names, leads = [], []
        while i < len(lines):
            if lines[i] == '' and i == len(lines) - 1:
                break
            if lines[i] != '':
                raise ValueError(f'{path}:{i + 1}: expected a blank line before a function')
            i += 1
            # a block: from the blank line to the next `}` line. Usually one function; a --hook function is three
            # lines and its _orig (`void hook_f_X(..);` `void f_X(..) { hook_f_X(c); }` `void f_X_orig(..) {` .. `}`)
            j = i
            while j < len(lines) and lines[j] != '}':
                j += 1
            block = lines[i:j + 1]
            defs = [m.group(1) for m in map(DEF.match, block) if m]
            if j == len(lines) or not defs:
                raise ValueError(f'{path}:{i + 1}: expected a function definition')
            # declarations ahead of the first definition move to the declarations: gen.py puts the rt_tpidrro /
            # rt_cntpct / rt_trace ones in front of the first function of a TU that needs them (where the TU starts,
            # so not part of any function), and a hook's `void hook_f_X(..);` is needed only by its own wrapper
            k = 0
            while not DEF.match(block[k]):
                self.declare(block[k], f'{path}:{i + k + 1}'); k += 1
            name = defs[0]
            if name in self.funcs:
                raise ValueError(f'{path}: {name} defined twice')
            self.funcs[name] = eol.join(block[k:])
            self.defs[name] = defs
            names.append(name); leads.append(block[:k])
            i = j + 1
        if render(pre, decls, [eol.join(ld + [self.funcs[n]]) for ld, n in zip(leads, names)], eol) != text:
            raise ValueError(f'{path}: does not parse back to itself')

    def declare(self, line, where):
        m = DECL.match(line)
        if not m:
            raise ValueError(f'{where}: not a declaration: {line[:80]}')
        if self.decl.setdefault(m.group(1), line) != line:
            raise ValueError(f'{where}: {m.group(1)} declared differently elsewhere')


def render(pre, decls, bodies, eol):
    return eol.join(pre + decls) + eol + ''.join(eol + b + eol for b in bodies)


def ref_pattern(names):
    """a regex for the names (prefix + hex, as gen.py makes them, or literal): ~5x faster than every identifier"""
    pre, lit = set(), set()
    for n in names:
        m = re.fullmatch(r'(.*?_)([0-9a-f]+)', n)
        pre.add(m.group(1)) if m else lit.add(n)
    alts = [re.escape(p) + '[0-9a-f]+' for p in sorted(pre, key=len, reverse=True)]
    alts += [re.escape(n) for n in sorted(lit, key=len, reverse=True)]
    return re.compile(r'\b(?:' + '|'.join(alts) + r')\b')


REF_PROCS = 6  # refs_of's processes; 1 = in this process


def _refs_init(pattern):
    global _REF
    _REF = re.compile(pattern)


def _refs(bodies):
    return [_REF.findall(b) for b in bodies]


def refs_of(funcs, pattern, procs=None):
    """{name: the names its text references}: the regex over every function is ~60% of a split (21 of 38 s on main,
    2026-10-30), so it runs in REF_PROCS processes on slices of the functions; same regex, same matches."""
    names = list(funcs)
    procs = REF_PROCS if procs is None else procs
    if procs <= 1 or len(names) < 1000:
        ref = re.compile(pattern)
        return {n: ref.findall(funcs[n]) for n in names}
    from concurrent.futures import ProcessPoolExecutor
    step = -(-len(names) // (procs * 4))
    slices = [names[i:i + step] for i in range(0, len(names), step)]
    out = {}
    with ProcessPoolExecutor(procs, initializer=_refs_init, initargs=(pattern,)) as ex:
        for part, found in zip(slices, ex.map(_refs, [[funcs[n] for n in sl] for sl in slices])):
            out.update(zip(part, found))
    return out


def addr(name):
    return int(re.search(r'_([0-9a-f]+)', name).group(1), 16)


def weight(body):
    """guest instructions: gen.py starts each one's line with /*<address>*/"""
    return body.count('\n/*')        # (with CRLF too: the \r comes before the \n)


def anchor(name, mod):
    return int.from_bytes(hashlib.sha256(name.encode()).digest()[:8], 'little') % mod == 0


def chunks(names, w, target):
    lo, hi, mod = target // 2, target * 2, max(1, target // 2 // ANCHOR_INSNS)
    cur, acc = [], 0
    for n in names:
        if cur and ((acc >= lo and anchor(n, mod)) or acc + w[n] > hi):
            yield cur
            cur, acc = [], 0
        cur.append(n); acc += w[n]
    if cur:
        yield cur


def split(src, dst, target, hot=None):
    """src: gen.py's output for one module -> dst: the same functions in stable TUs. Returns a summary dict."""
    m = Module(src)
    os.makedirs(dst, exist_ok=True)
    for fn in m.other:
        shutil.copy2(os.path.join(src, fn), os.path.join(dst, fn))
    w = {n: weight(b) for n, b in m.funcs.items()}
    order = sorted(m.funcs, key=addr)
    classes = {'t': order} if hot is None else {'h': [n for n in order if n in hot], 'c': [n for n in order if n not in hot]}
    # every function of the module a TU names gets a declaration (gen.py's line, or one made from the definition for
    # a function gen.py never declared because its callers shared its TU), its own functions included: a TU may now
    # call a function it defines further down
    decl = dict(m.decl)
    for n, b in m.funcs.items():
        for line in b.split(m.eol):
            d = DEF.match(line)
            if d and d.group(1) not in decl:
                decl[d.group(1)] = line[:line.index(') {') + 1] + ';'
    found = refs_of(m.funcs, ref_pattern(decl).pattern)
    summary = {'functions': len(order), 'tus': {}}
    for cls, names in classes.items():
        count = 0
        for group in chunks(names, w, target):
            refs = set()
            for n in group:
                refs.update(t for t in found[n] if t in decl)
            decls = sorted(decl[r] for r in refs)
            text = render(m.pre, decls, [m.funcs[n] for n in group], m.eol)
            # zero-padded: mkninja_win.py links in sorted name order, and that must be address order (as gen.py's
            # tu_NNN), or the exe's code is laid out out of guest order (t_1e07e44 sorted before t_d3a724)
            with open(os.path.join(dst, f'{cls}_{addr(group[0]):010x}.cpp'), 'w', newline='') as f:
                f.write(text)
            count += 1
        summary['tus'][cls] = count
    return summary
