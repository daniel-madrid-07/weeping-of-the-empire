"""abi_scan.py: the call sites and functions that gen.py --lso-abi must leave alone.

usage: python abi_scan.py <img_prefix> <funcs.json> <module> <out> [--findings FILE] [--jobs N]
       (<img_prefix>.bin as gen.py reads it; <module> main | sdk names the out file's section; a FILE already holding
       other sections keeps them; --jobs: processes for the first pass, default min(6, cores); 1 = this one only)

gen.py --lso-abi drops x9-x18 and NZCV ("D") from the registers an LSO stores to the real Ctx before a call or a
return: AAPCS64 makes them dead there (not arguments, not results, not preserved). This binary was built with
interprocedural register allocation, though: some callers keep a D register live across a call to a callee that is
known not to clobber it, and that reliance must keep its store. This scan finds every such place, conservatively:

- per function (every FDE range of funcs.json), a forward dataflow over its instructions with D "poisoned" at the
  entry, at every BL target inside it, and after every BL / BLR (the poison names the call site, and replaces the
  poison of earlier calls: gen.py's lsi_abi does not reload D after a call it does not keep, so a value that lives
  across calls A and B and is read after B depends on B's store only; the entry's poison stays until a write); a
  write clears a register; a read of a poisoned register is a finding;
- the CFG is gen.py's own: its jump tables (Emitter.jump_table, a superset of each table) and its adr continuations;
  any other BR's state flows into every instruction without a straight-line predecessor, and instructions the flow
  never reaches start poisoned (as entries): both only over-approximate;
- register reads and writes come from the operands (capstone's regs_access is wrong for some aliases).

Output, one section per module: `module <m>`, then `site <hex> <regs>` (a call site after which its caller reads those
D registers: its LSO / LSI keep D), `entry <hex> <regs>` (a function that reads those before writing them: every direct
call or tail transfer to it keeps D) and `callee <hex> <regs>` (a function a site calls directly: its returns and tail
calls keep D, since it may be the one that produces what that caller reads, e.g. a MachineOutliner function or an asm
helper returning in x9+ or the flags). gen.py keeps all of D at those points; the register lists are for the compare.
The three are closed under each other to a fixpoint: a callee's exits read what its callers read: otherwise gen.py
would pass it values lsi_abi left stale after an earlier call; off by default, see run()). By the IPRA argument (no
reliance through an indirect call, no arguments in D) those are all the places that matter. A module's section covers
its own code only: an import into another module's entry reader can't be keyed from the caller's section (none exists
in TotK 1.0.0). gen_ext.py's extension ranges are not scanned; gen.py keeps D everywhere in such functions."""
import bisect
import collections
import json
import os
import re
import struct
import sys
import time

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone import arm64_const as A

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen  # noqa: E402   (gen.Emitter.jump_table, gen.split_ops, gen.imm)

D_REGS = list(range(9, 19))
ENTRY_USE = '--entry-use' in sys.argv
EXACT_JT = '--exact-jt' in sys.argv   # jump tables read up to their cmp bound (default: gen.py's superset, sound)
NZCV = 'nzcv'
D_ALL = frozenset(D_REGS + [NZCV])
NZCV_READ = {'csel', 'csinc', 'csinv', 'csneg', 'cset', 'csetm', 'cinc', 'cinv', 'cneg', 'ccmp', 'ccmn', 'fccmp',
             'fccmpe', 'fcsel', 'adc', 'adcs', 'sbc', 'sbcs', 'ngc', 'ngcs'}
NZCV_WRITE = {'adds', 'subs', 'ands', 'bics', 'adcs', 'sbcs', 'negs', 'ngcs', 'cmp', 'cmn', 'tst', 'ccmp', 'ccmn',
              'fcmp', 'fcmpe', 'fccmp', 'fccmpe', 'setf8', 'setf16', 'rmif', 'cfinv'}
ALL_READ = {'cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'cbz', 'cbnz', 'tbz', 'tbnz', 'br', 'blr', 'ret', 'fcmp', 'fcmpe',
            'fccmp', 'fccmpe', 'msr', 'prfm', 'prfum', 'braa', 'brab', 'blraa', 'blrab', 'dc', 'ic', 'at', 'tlbi',
            'sys', 'hint', 'svc'}
OP0_RW = {'movk', 'bfi', 'bfxil', 'bfm', 'bfc', 'ins'}
CALLS = ('bl', 'blr', 'blraa', 'blraaz', 'blrab', 'blrabz')


def reg_index(cs, r):
    n = cs.reg_name(r)
    if n == 'nzcv':
        return NZCV
    if n and n[0] in 'xw' and n[1:].isdigit():
        k = int(n[1:])
        return k if 9 <= k <= 18 else None
    return None


def sem(cs, i):
    """(reads, writes) of D for one instruction (detail mode)"""
    m = i.mnemonic
    reads, writes = set(), set()
    if m.startswith('b.') or m in NZCV_READ or (m == 'mrs' and 'nzcv' in i.op_str):
        reads.add(NZCV)
    if m in NZCV_WRITE or (m == 'msr' and 'nzcv' in i.op_str):
        writes.add(NZCV)
    store = m.startswith('st') and not m.startswith(('stxr', 'stlxr', 'stxp', 'stlxp'))
    excl_store = m.startswith(('stxr', 'stlxr', 'stxp', 'stlxp'))
    lse_cas = m.startswith('cas')
    lse_rmw = m.startswith(('swp', 'ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin'))
    pair_load = m in ('ldp', 'ldpsw', 'ldnp', 'ldxp', 'ldaxp')
    for k, o in enumerate(i.operands):
        if o.type == A.ARM64_OP_MEM:
            for r in (o.mem.base, o.mem.index):
                x = reg_index(cs, r) if r else None
                if x is not None:
                    reads.add(x)
                    if i.writeback and r == o.mem.base:
                        writes.add(x)
            continue
        if o.type != A.ARM64_OP_REG:
            continue
        x = reg_index(cs, o.reg)
        if x is None:
            continue
        if store or m in ALL_READ:
            reads.add(x)
        elif excl_store:
            (writes if k == 0 else reads).add(x)
        elif lse_cas:
            reads.add(x)
            if k == 0 or (m.startswith('casp') and k == 1):
                writes.add(x)
        elif lse_rmw:
            (writes if k == 1 else reads).add(x)
        elif pair_load:
            (writes if k in (0, 1) else reads).add(x)
        elif k == 0:
            writes.add(x)
            if m in OP0_RW:
                reads.add(x)
        else:
            reads.add(x)
    return reads, writes


def exact_table(img, ins, k, cs):
    """the targets of the jump table ending in ins[k] (br), read from the image with its `cmp idx, #N` bound; None when
    the pattern is not the usual adr / ldr{b,h,sw} [table, idx] / add anchor, entry, lsl #s / cmp idx, #N one"""
    import struct
    br = ins[k]
    reg = br.op_str.strip()
    w = ins[max(0, k - 24):k]
    num = lambda r: r[1:] if r[:1] in 'xw' else r
    add = adr = ld = bound = None
    for i in reversed(w):
        o = [x.strip() for x in i.op_str.split(',')]
        if add is None and i.mnemonic == 'add' and num(o[0]) == num(reg) and len(o) >= 3:
            add = o
        elif add and adr is None and i.mnemonic == 'adr' and num(o[0]) == num(add[1]):
            adr = int(o[1].lstrip('#'), 16)
        elif add and ld is None and i.mnemonic in ('ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh') and num(o[0]) == num(add[2]):
            ld = (i.mnemonic, o)
        elif ld and bound is None and i.mnemonic == 'cmp' and len(o) == 2 and o[1].startswith('#'):
            mm = ld[1][1:]
            idx = mm[1].rstrip(']').strip() if len(mm) > 1 else None
            if idx and num(o[0]) == num(idx):
                bound = int(o[1].lstrip('#'), 0) + 1
    if not (add and adr is not None and ld and bound):
        return None
    shift = 0
    if len(add) == 4 and 'lsl' in add[3]:
        shift = int(add[3].split('#')[1], 0)
    tb = ld[1][1].lstrip('[').strip()
    # the table base: adrp tb, page ; add tb, tb, #off within the window
    page = off = None
    for i in w:
        o = [x.strip() for x in i.op_str.split(',')]
        if i.mnemonic == 'adrp' and num(o[0]) == num(tb):
            page, off = int(o[1].lstrip('#'), 16), 0
        elif page is not None and i.mnemonic == 'add' and num(o[0]) == num(tb) and num(o[1]) == num(tb) and o[2].startswith('#'):
            off += int(o[2].lstrip('#'), 0)
    if page is None or bound > 4096:
        return None
    tab = page + off
    esz, fmt = {'ldrb': (1, 'B'), 'ldrsb': (1, 'b'), 'ldrh': (2, 'H'), 'ldrsh': (2, 'h'), 'ldrsw': (4, 'i')}[ld[0]]
    out = []
    for j in range(bound):
        v = struct.unpack_from('<' + fmt, img, tab + j * esz)[0]
        out.append(adr + (v << shift))
    return out


# noreturn imports, by name (their PLT stubs): a call to one has no fall-through
NORET_NAMES = re.compile(r'^(abort|_exit|exit|_Exit|__cxa_throw|__cxa_rethrow|__cxa_bad_cast|__cxa_bad_typeid|'
                         r'__stack_chk_fail|_ZSt9terminatev|__assert_fail)$|'
                         r'^_ZN2nn4diag6detail\d+Abort|^_ZN2nn6detail\d+UnexpectedDefault')


def plt_noreturn(img, info):
    """PLT stub addresses of the noreturn imports (gen.py's own PLT map)"""
    if not info:
        return set()
    dyn = info['dynamic']
    text = next(x for x in info['segments'] if x['name'] == 'text')
    strtab, symtab = dyn['STRTAB'][0], dyn['SYMTAB'][0]
    names, got = [], {}
    for tag_rel, tag_sz in (('JMPREL', 'PLTRELSZ'),):
        if tag_rel not in dyn:
            continue
        r, sz = dyn[tag_rel][0], dyn[tag_sz][0]
        for o in range(r, r + sz, 24):
            off, rinfo, _ = struct.unpack_from('<QQq', img, o)
            if rinfo & 0xFFFFFFFF == 1026:
                no = struct.unpack_from('<I', img, symtab + (rinfo >> 32) * 24)[0]
                got[off] = len(names)
                names.append(img[strtab + no:img.index(b'\0', strtab + no)].decode())
    plt = gen.plt_map(img, info, got, text['mem'] + text['size'], len(got))
    return {a for a, i in plt.items() if NORET_NAMES.search(names[i])}


def noreturn_functions(img, fde, noret_imports, stats):
    """the functions with no way back to their caller (every path ends in a noreturn call, a trap or the end of the
    FDE), to a fixpoint from none: a call to one has no fall-through. Sound: every edge the scan keeps elsewhere is
    kept here, an unresolved br or a tail transfer to anything not known noreturn counts as a return"""
    lite_cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    lite_cs.skipdata = True
    em = gen.Emitter.__new__(gen.Emitter)
    em.img, em.stats = img, collections.Counter()
    info = {}   # start -> (succ lists, call targets by index, exits: [(kind, target)], n)
    callers = collections.defaultdict(set)
    for s in sorted(fde):
        end = s + fde[s]
        lite = list(lite_cs.disasm_lite(img[s:end], s))
        idx = {a: k for k, (a, sz, m, o) in enumerate(lite)}
        adr_t = sorted({t for (a, sz, m, o) in lite if m == 'adr' for t in [gen.imm(gen.split_ops(o)[-1])] if s <= t < end})
        succ, ct, ex = [[] for _ in lite], {}, {}
        for k, (a, sz, m, o) in enumerate(lite):
            nxt = [k + 1] if k + 1 < len(lite) else []
            if m in ('ret', 'retaa', 'retab'):
                ex[k] = ('ret', None)
            elif m in CALLS:
                succ[k] = nxt
                if m == 'bl':
                    ct[k] = gen.imm(o)
                    callers[ct[k]].add(s)
            elif m == 'br':   # (a resolved one still leaves through gen.py's `default: call_ind` arm: an exit too)
                tab = em.jump_table(lite, k, s, end) or adr_t
                succ[k] = [idx[t] for t in tab if t in idx]
                ex[k] = ('br', None)
            elif m in ('braa', 'braaz', 'brab', 'brabz'):
                ex[k] = ('br', None)
            elif m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                t = gen.imm(gen.split_ops(o)[-1])
                succ[k] = ([idx[t]] if t in idx else []) + ([] if m == 'b' else nxt)
                if t not in idx:
                    ex[k] = ('tail', t)
            elif m in ('udf', 'brk', 'hlt', '.byte'):
                pass
            else:
                succ[k] = nxt
        info[s] = (succ, ct, ex, len(lite))
    noret = set(noret_imports)

    def returns(s):
        succ, ct, ex, n = info[s]
        seen, todo = set(), [0] if n else []
        while todo:
            k = todo.pop()
            if k in seen:
                continue
            seen.add(k)
            if k in ex:
                kind, t = ex[k]
                if kind != 'tail' or t not in noret:
                    return True
            if k in ct and ct[k] in noret:
                continue
            todo.extend(succ[k])
        return False
    todo = set(info)
    while todo:
        new = {s for s in todo if not returns(s)} - noret
        noret |= new
        todo = set().union(*(callers[s] for s in new)) if new else set()
        stats['noret_rounds'] += 1
    stats['noreturn_functions'] = len(noret - set(noret_imports))
    stats['noreturn_imports'] = len(noret_imports)
    return noret


_W = {}   # a first-pass worker's inputs (_w_init)


def _w_init(bin_path, fde, mid, noret, flags, findings):
    global ENTRY_USE, EXACT_JT
    ENTRY_USE, EXACT_JT = flags
    _W.update(img=open(bin_path, 'rb').read(), fde=fde, mid=mid, noret=noret, findings=findings)


def _w_part(part):
    """one share of the first pass (a worker process): scan() over `part` only, its raw state back"""
    return scan(_W['img'], _W['fde'], [] if _W.get('findings') else None, _part=(part, _W['mid'], _W['noret']))


def scan(img, fde, findings_out=None, info=None, jobs=1, bin_path=None, _part=None):
    """jobs > 1 (with bin_path, the image file the workers read): the first pass, every function with nothing known
    about the others yet, runs in that many processes; it is order-free (callee_regs is empty then, and without
    --entry-use nothing reads entry_readers), so the result is the same. The closure stays in this process."""
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    cs.skipdata = True
    em = gen.Emitter.__new__(gen.Emitter)   # only jump_table: it needs .img and .stats
    em.img, em.stats = img, collections.Counter()
    starts = sorted(fde)
    fset = set(starts)
    lite_cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    lite_cs.skipdata = True
    stats = collections.Counter()
    if _part is not None:
        _part, mid, noret = _part
    else:
        # BL targets inside a function (an extra entry there), from the functions' own code
        t0 = time.time()
        bl_t = set()
        for s in starts:
            for a, sz, m, o in lite_cs.disasm_lite(img[s:s + fde[s]], s):
                if m == 'bl':
                    bl_t.add(gen.imm(o))
        mid = collections.defaultdict(set)
        for t in bl_t:
            if t not in fset:
                j = bisect.bisect_right(starts, t) - 1
                if j >= 0 and t < starts[j] + fde[starts[j]]:
                    mid[starts[j]].add(t)
        t1 = time.time()
        noret = noreturn_functions(img, fde, plt_noreturn(img, info), stats)
        stats['s_bl_targets'], stats['s_noreturn'] = round(t1 - t0), round(time.time() - t1)
    # per register: the D registers a keep site's caller reads after it / an entry reader reads at its entry / a callee's
    # exits must hand back (a call to an entry reader uses only the registers it reads)
    keep_sites, entry_readers, callee_regs, call_target = {}, {}, {}, {}
    callers_of = collections.defaultdict(set)   # a call / tail-transfer target -> the functions that reach it
    poison = lambda src: frozenset((r, src) for r in D_ALL)
    tail_target = {}
    sem_cache = {}

    def sem_of(word, a):
        """sem() of one instruction word, by its encoding: D's reads and writes depend on the word alone, not on
        where it sits (sem reads the mnemonic, the register operands, writeback and, for mrs / msr, the op text)"""
        e = sem_cache.get(word)
        if e is None:
            i = next(cs.disasm(word, a))
            reads, writes = sem(cs, i)
            e = sem_cache[word] = (frozenset(reads), frozenset(writes))
        return e

    def analyze(s):
        """one function's dataflow. Its returns and tail transfers read the D registers callee_regs[s] lists (it may
        produce what a keep site's caller reads, also through its own callees: the closure), and a direct call
        or tail transfer to an entry reader reads the registers that reader reads at its entry"""
        end = s + fde[s]
        # the lite disassembly (detail mode only per distinct instruction word, in sem_of: it is most of the cost)
        lite = list(lite_cs.disasm_lite(img[s:end], s))
        if not lite:
            return
        n = len(lite)
        ins = list(cs.disasm(img[s:end], s)) if EXACT_JT else None   # (exact_table reads the detail)
        idx = {a: k for k, (a, sz, m, o) in enumerate(lite)}
        adr_t = sorted({t for (a, sz, m, o) in lite if m == 'adr'
                        for t in [gen.imm(gen.split_ops(o)[-1])] if s <= t < end})
        succ = [[] for _ in lite]
        brs, calls, exits, tail_at = [], set(), set(), {}
        for k, (a, sz, m, o) in enumerate(lite):
            nxt = [k + 1] if k + 1 < n else []
            if m == '.byte':
                continue
            if m in ('ret', 'retaa', 'retab'):
                exits.add(k)
                continue
            if m in CALLS:
                calls.add(k)
                succ[k] = nxt
                if m == 'bl':
                    t = gen.imm(o)
                    if t in noret:   # no way back: nothing after it follows this call
                        succ[k] = []
                    call_target[a] = t
                    callers_of[t].add(s)
            elif m in ('br', 'braa', 'braaz', 'brab', 'brabz'):
                tab = ((EXACT_JT and exact_table(img, ins, k, cs)) or em.jump_table(lite, k, s, end)) if m == "br" else None
                tab = tab or adr_t
                exits.add(k)   # (gen.py's switch falls back to call_ind and returns for any other target)
                if tab:
                    succ[k] = [idx[t] for t in tab if t in idx]
                    stats['br_resolved'] += 1
                else:
                    brs.append(k)
            elif m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                t = gen.imm(gen.split_ops(o)[-1])   # (the label: always an immediate operand)
                succ[k] = ([idx[t]] if t in idx else []) + ([] if m == 'b' else nxt)
                if t is not None and t not in idx:   # a tail transfer out of the function
                    exits.add(k)
                    tail_target.setdefault(s, set()).add(t)
                    tail_at[k] = t
                    callers_of[t].add(s)
            else:
                succ[k] = nxt
        has_pred = [False] * n
        for k in range(n):
            for j in succ[k]:
                has_pred[j] = True
        orphans = [k for k in range(1, n) if not has_pred[k]]
        # each instruction's D reads / writes once (a node is visited again whenever its state grows); an exit adds
        # what this function's callers need back (callee_regs changes only between analyses)
        cr = callee_regs.get(s, frozenset())
        rw = []
        for k, (a, sz, m, o) in enumerate(lite):
            if m == '.byte':
                rw.append(None)
                continue
            reads, writes = sem_of(img[a:a + sz], a)
            if k in exits and cr:
                reads = reads | cr
            tgt = (call_target.get(a) if k in calls else tail_at.get(k)) if ENTRY_USE else None
            rw.append((reads, writes, tgt))
        state = [None] * n
        work = collections.deque()

        def push(j, st):
            cur = state[j]
            nst = st if cur is None else (cur | st)
            if nst != cur:
                state[j] = nst
                work.append(j)

        def run():
            br_state = frozenset()
            while work:
                k = work.popleft()
                st, e = state[k], rw[k]
                if e is None:
                    continue
                reads, writes, tgt = e
                # --entry-use: a direct call or tail transfer to an entry reader uses D, so that D holds what the binary
                # would pass, not a value lsi_abi left stale. Off by default: in TotK every entry read is on a path the
                # scan can't rule out but the binary never takes (f_232c0: x10 is set on every feasible path), and the
                # closure then spreads over 65 % of main's calls (looked up at the visit: entry_readers grows during
                # this analysis)
                if tgt is not None and tgt in entry_readers:
                    reads = reads | entry_readers[tgt]
                bad = [x for x in st if x[0] in reads] if reads else ()
                for r, src in bad:
                    if src == 'unreached':   # dead code: no modelled way in (after the noreturn calls, padding)
                        stats['unreached_reads'] += 1
                        continue
                    if src == 'entry':
                        entry_readers.setdefault(s, set()).add(r)
                    else:
                        keep_sites.setdefault(src, set()).add(r)
                    if findings_out is not None:
                        a, sz, m, o = lite[k]
                        findings_out.append((s, a, f'{m} {o}', r, src))
                nst = frozenset(x for x in st if x[0] not in writes) if writes else st
                if k in calls:   # the call clobbers D: earlier calls' poison goes (gen.py's lsi_abi keeps the caller's
                    # own D across a call it does not blame), the entry's stays (a value from the entry still is one)
                    nst = frozenset(x for x in nst if x[1] == 'entry') | poison(lite[k][0])
                if k in brs:
                    nb = br_state | nst
                    if nb != br_state:
                        br_state = nb
                        for o in orphans:
                            push(o, br_state)
                for j in succ[k]:
                    push(j, nst)
        push(0, poison('entry'))
        for e in mid.get(s, ()):
            if e in idx:
                push(idx[e], poison('entry'))
        run()
        # code the flow never reached (no modelled way in): an entry of its own, poisoned (sound, over-approximate)
        for k, (a, sz, m, o) in enumerate(lite):
            if state[k] is None and m not in ('.byte', 'nop', 'udf', 'brk'):
                stats['unreached_entries'] += 1
                push(k, poison('unreached'))
        run()
        stats['functions'] += 1
        stats['insns'] += n
        stats['call_insns'] += len(calls)

    if _part is not None:
        for s in _part:
            analyze(s)
        return keep_sites, entry_readers, call_target, dict(callers_of), tail_target, stats, findings_out
    t0 = time.time()
    if jobs > 1 and bin_path and not ENTRY_USE:
        import multiprocessing
        n = 8 * jobs   # (contiguous shares in address order: their findings join in the one-process order)
        parts = [starts[k * len(starts) // n:(k + 1) * len(starts) // n] for k in range(n)]
        with multiprocessing.Pool(jobs, _w_init, (bin_path, fde, mid, noret, (ENTRY_USE, EXACT_JT),
                                                  findings_out is not None)) as pool:
            for ks, er, ct, co, tt, st, fo in pool.imap(_w_part, parts):
                for a, rr in ks.items():
                    keep_sites.setdefault(a, set()).update(rr)
                for a, rr in er.items():
                    entry_readers.setdefault(a, set()).update(rr)
                call_target.update(ct)
                for t, ff in co.items():
                    callers_of[t] |= ff
                for f, tt_ in tt.items():
                    tail_target.setdefault(f, set()).update(tt_)
                stats.update(st)
                if fo:
                    findings_out.extend(fo)
    else:
        for s in starts:
            analyze(s)
    stats['s_first_pass'], t0 = round(time.time() - t0), time.time()
    # the closure, to a fixpoint:
    # - keep sites' direct callees (and their tail-call targets) are analyzed again with their exits as reads of D;
    # - (--entry-use) the functions that call or tail-transfer to an entry reader are analyzed again with that call as a
    #   read of D;
    # the call sites that poison those reads become keep sites, their targets callees; a function whose D at such a
    # read still comes from its entry becomes an entry reader itself
    seen_callee, seen_entry = {}, {}
    while True:
        regs = collections.defaultdict(set)
        for a, rr in keep_sites.items():
            if a in call_target:
                regs[call_target[a]] |= rr
        grew = True
        while grew:   # a callee's tail transfers hand back what it must
            grew = False
            for f in list(regs):
                for t in tail_target.get(f, ()):
                    if not regs[f] <= regs[t]:
                        regs[t] |= regs[f]
                        grew = True
        callee_regs.clear()
        callee_regs.update(regs)
        todo = {f for f, rr in callee_regs.items() if f in fde and seen_callee.get(f) != rr}
        for e, rr in list(entry_readers.items()) if ENTRY_USE else ():   # (without --entry-use a function's flow
            # depends on its own callee_regs only: its callers would come out the same; that was 172 of main's 459 s)
            if seen_entry.get(e) != rr:
                seen_entry[e] = set(rr)
                todo |= callers_of.get(e, set())
        if not todo:
            break
        for f in sorted(todo):
            if f in callee_regs:
                seen_callee[f] = set(callee_regs[f])
            analyze(f)
        stats['closure_rounds'] += 1
    callees = callee_regs
    stats['s_closure'] = round(time.time() - t0)
    stats['keep_sites'] = len(keep_sites)
    stats['entry_readers'] = len(entry_readers)
    stats['callees'] = len(callees)
    return keep_sites, entry_readers, callees, stats


def regs_txt(rr):
    return ','.join(sorted(f'x{r}' if r != NZCV else NZCV for r in rr))


def main():
    pre, fj, module, out = sys.argv[1:5]
    img = open(pre + '.bin', 'rb').read()
    fde = {int(k, 16): v for k, v in json.load(open(fj))['fde'].items()}
    found = [] if '--findings' in sys.argv else None
    info = json.load(open(pre + '.json')) if os.path.exists(pre + '.json') else None
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else min(6, os.cpu_count() or 1)
    sites, entries, callees, stats = scan(img, fde, found, info, jobs, pre + '.bin')
    keep = []
    if os.path.exists(out):   # other modules' sections stay
        cur = None
        for line in open(out):
            p = line.split()
            if p and p[0] == 'module':
                cur = p[1]
            if cur and cur != module and p and not line.startswith('#'):
                keep.append(line.rstrip('\n'))
    with open(out + '.tmp', 'w', newline='\n') as f:
        f.write('# abi_scan.py: LSO points that keep x9-x18 / NZCV under gen.py --lso-abi\n')
        f.write('\n'.join(keep) + ('\n' if keep else ''))
        f.write(f'module {module}\n')
        f.write(''.join(f'site {a:x} {regs_txt(sites[a])}\n' for a in sorted(sites)))
        f.write(''.join(f'entry {a:x} {regs_txt(entries[a])}\n' for a in sorted(entries)))
        f.write(''.join(f'callee {a:x} {regs_txt(callees[a])}\n' for a in sorted(callees)))
    os.replace(out + '.tmp', out)
    if found is not None:
        with open(sys.argv[sys.argv.index('--findings') + 1], 'w', newline='\n') as f:
            for s, a, txt, r, src in found:
                f.write(f'{s:x}\t{a:x}\t{txt}\t{r}\t{src if src == "entry" else hex(src)}\n')
    print(json.dumps(dict(stats)))


if __name__ == '__main__':
    main()
