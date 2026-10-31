"""The engine's 1.0.0 main addresses, placed in another game version.

    python -m games.totk.patches engine-map --old <v100 exefs/main> --new <v143 exefs/main> --version 1.4.3
                                            [--report engine-map-report.json]

The engine's own code names main by 1.0.0 offsets in three ways:
  - addresses: every one goes through totk_version.h (totk_main / totk_main_off / totk_global / decomp_bridge::rebase);
  - function names: hook_f_<off> / f_<off>_orig of the GEN_MAIN hooks and direct f_<off> calls;
  - GEN_MAIN's offset lists (--hook, --vlocal) in tools/engine/build.py.
`collect` finds them in the sources: every hex constant in a main offset's range is a
candidate, so nothing is missed by a pattern; constants that are not addresses simply find no place and are left out.
`generate` runs the porter and writes engine/runtime/totk_main_v<NNN>.inc (the address table totk_version.h
includes) and games/totk/patches/v<NNN>.engine.json (gen.py's --hook / --alias / --vlocal for that build):
  - an address is placed only as the porter places patch sites: the same instruction of a confirmed function pair, a
    data address with a clear majority of references (or one reference its content confirms: a vtable's paired
    slots, the same bytes), or unique read-only content (a string, a table);
  - a function name gets an alias when its function pair is confirmed; a hook stays a hook only when the two bodies
    are equal after normalization ("exact": same struct offsets, so the hook's C++ still fits), otherwise it is
    aliased unhooked and the game runs its own recompiled code there.
An engine address missing from the table is a build error (constant uses) or a logged stop (run-time uses), never a
1.0.0 address in the other build.
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCES = ('engine/runtime', 'games/totk/engine', 'games/totk/runtime', 'decomp/src/game/options_menu.cpp')
SOURCE_EXT = ('.c', '.cpp', '.h', '.inc')
MAIN_SPAN_V100 = 0x4666000
DECOMP_MAIN = 0x1000000000
DATA_KINDS = ('data', 'data1', 'vtable', 'bound', 'string', 'blob')   # tools/version_port.py map() answers
MIN_OFFSET = 0x10000             # below this a hex constant is a size, a mask or an encoding, not a main address
HEX = re.compile(r'\b0x([0-9a-fA-F]+)(?:ull|ULL|u|U|ll|LL)?\b')
GUEST_FN = re.compile(r'DECOMP_GUEST_FN\(\s*\w+\s*,\s*([0-9a-fA-F]+)\s*,')  # bare hex
FNAME = re.compile(r'\b(?:hook_)?f_([0-9a-f]{5,8})(?:_orig)?\b')


# What a build for another version may do when the porter cannot keep one of GEN_MAIN's hooks. Every GEN_MAIN hook
# is in exactly one of these (games/totk/tests/test_engine_map.py); a new hook needs its line here.
#   fail   the build refuses: nothing safe plays without it
#   cap30  the build must run at the Switch's 30 fps, where the hook changes nothing: its patch set may not carry
#          fps60 (tools/engine/build.py refuses one that does)
#   group  all or none: a memo kept without one of its invalidating mutators returns stale rows
#   free   speed or diagnostics only: dropped, the game's own code runs
HOOK_FAIL = {
    0x83e3f8: 'eui::UIController::updateState: menus on the PC scheme (pc-controls)',
    0x1ccf894: "ExecutePlayerCalcKey: the player's virtual keys (pc-controls; vk_guard needs it)",
    0xf6092c: 'OptionsMgr::initialize: the options screen', 0x1a862e4: 'options row controller',
    0x1a86420: 'options row controller', 0x1a85b3c: 'options row controller', 0x1a8604c: 'options row controller',
    0x1a85e4c: 'options row controller', 0x1a06840: "options: shown entries' count",
    0x1a068cc: 'options: the n-th shown entry',
}
HOOK_CAP30 = {
    0x7a83e4: 'the frame-delta setter (the speed fix)', 0x8f689c: 'the present-interval minimum',
    0xf15364: 'map scroll', 0xf0fdec: "Hero's Path step", 0x16e4728: 'Walk stop delay', 0x1cd0260: 'stick spin',
    0x16e4610: 'struggle spin', 0x1aa3e44: 'get-item info delays', 0x16e4b80: 'Cucco glide ease',
    0x1d23614: 'tumble-slide sound', 0xa5f010: 'lightning rolls', 0x1d97648: 'event waits', 0x1c17fb0: 'event waits',
    0x120ec94: 'Recall velocity step', 0x9d8d00: 'Autobuild settle', 0x1b3ebac: 'cooking pot settle',
    0x1a4f0ac: 'extra-stamina pause', 0x1c73fbc: 'tar Like Like spots', 0x1dbdcf0: "flying enemies' path probes",
    0x1aa6a94: 'horse gauge tween', 0x1b7ba58: "followers' ring", 0x212b740: "inventory Link's tab debounce",
    0x1f61d6c: "inventory Link's rotation", 0x2833b60: 'AI-program integrators', 0x200f61c: 'ChaseF32 easer',
    0x200f8b0: 'ChaseVec3f easer', 0x20147f4: 'LerpF32 easer', 0x27dcb98: 'AddF32 adder',
}
HOOK_GROUPS = (
    ('PouchActorInfoTable::getRow memo and its mutators',
     (0xb6afc8, 0x75718c, 0x18fb7d0, 0x18fbf18, 0x18fbf68, 0x18fc0ac, 0x279a77c, 0x2798a1c)),
)
HOOK_FREE = {0x704b7c: 'submesh cull (speed)', 0x27b6e64: 'load lap labels (diagnostic)',
             0x11276cc: 'loader thread count (A/B)'}


def hook_policy(kept, hooks):
    """kept: {1.0.0 hook offset: why it cannot stay (absent: it stays)} for every GEN_MAIN hook. Returns (hooked
    1.0.0 offsets, status 'pass' | 'fail', safe_mode list, reasons {hex: text})."""
    drop, reasons = dict(kept), {}
    for name, members in HOOK_GROUPS:
        lost = [m for m in members if m in drop]
        for m in members:
            if lost and m not in drop:
                drop[m] = f'its group ({name}) lost {", ".join(hex(x) for x in lost)}'
    status, safe = 'pass', []
    for off, why in sorted(drop.items()):
        if off in HOOK_FAIL:
            status = 'fail'
            reasons[hex(off)] = f'REQUIRED ({HOOK_FAIL[off]}): {why}'
        elif off in HOOK_CAP30:
            safe = ['cap30']
            reasons[hex(off)] = f'cap30 ({HOOK_CAP30[off]}): {why}'
        else:
            reasons[hex(off)] = f'dropped: {why}'
    return [h for h in hooks if h not in drop], status, safe, reasons


def source_files(root=ROOT):
    for s in SOURCES:
        p = root / s
        if p.is_file():
            yield p
        else:
            for dirpath, _, names in os.walk(p):
                for n in sorted(names):
                    if n.endswith(SOURCE_EXT) and not n.startswith('totk_main_v'):
                        yield Path(dirpath) / n


def gen_main_lists():
    """GEN_MAIN's --hook and --vlocal offsets (tools/engine/build.py)."""
    sys.path.insert(0, str(ROOT / 'tools' / 'engine'))
    import build
    out = {}
    for i, a in enumerate(build.GEN_MAIN[:-1]):
        if a in ('--hook', '--vlocal'):
            out[a[2:]] = [int(x, 16) for x in build.GEN_MAIN[i + 1].split(',')]
    return out


def collect(root=ROOT):
    """{'offsets': main offsets named as hex constants, 'functions': offsets named by f_<off> symbols}."""
    offsets, functions = set(), set()
    for p in source_files(root):
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in [*HEX.finditer(text), *GUEST_FN.finditer(text)]:
            v = int(m.group(1), 16)
            if DECOMP_MAIN <= v < DECOMP_MAIN + MAIN_SPAN_V100:
                v -= DECOMP_MAIN
            if MIN_OFFSET <= v < MAIN_SPAN_V100:
                offsets.add(v)
        for m in FNAME.finditer(text):
            functions.add(int(m.group(1), 16))
    return dict(offsets=sorted(offsets), functions=sorted(functions))


def version_number(version):
    """'1.4.3' -> 143 (the TOTK_GAME_VERSION define)."""
    return int(version.replace('.', ''))


def table_inc(pairs, version):
    lines = [f'// GENERATED by `python -m games.totk.patches engine-map` for TotK {version}: do not edit.',
             '// {1.0.0 main offset, offset in this build}, sorted; totk_version.h looks offsets up here.',
             '#pragma once',
             f'static constexpr uint64_t kTotkMainMapCount = {len(pairs)};',
             f'static constexpr uint64_t kTotkMainMap[{max(1, len(pairs))}][2] = {{']
    lines += [f'    {{{a:#x}, {b:#x}}},' for a, b in sorted(pairs)] or ['    {0, 0},']
    return '\n'.join(lines + ['};', ''])


def generate(old_nso, new_nso, version):
    sys.path.insert(0, str(ROOT / 'tools'))
    import version_port as vp
    from .port import placeable
    porter = vp.Porter(vp.load_image(str(old_nso)), vp.load_image(str(new_nso))).run()
    found = collect()
    lists = gen_main_lists()
    addrs, report = {}, dict(stats=porter.stats(), addresses={}, functions={}, hooks={})
    for off in found['offsets']:
        r = porter.map(off)
        if r.get('how') in DATA_KINDS or placeable(r):
            addrs[off] = int(r['new'], 16)
        report['addresses'][hex(off)] = r
    alias, cannot = {}, {}
    gen_hooks = lists.get('hook', [])
    for off in sorted(set(found['functions']) | set(gen_hooks)):
        fo = off if off in porter.fwd else None
        if fo is None or not porter.confirmed(fo):
            report['functions'][hex(off)] = dict(how=None, why='no confirmed function pair')
            if off in gen_hooks:
                cannot[off] = 'its function has no confirmed place'
            continue
        fn = porter.fwd[fo]
        alias[fn] = off
        addrs.setdefault(off, fn)
        exact = porter.hash_o[fo] == porter.hash_n[fn]
        report['functions'][hex(off)] = dict(new=hex(fn), how=porter.how[fo], exact=exact)
        if off in gen_hooks and not exact:
            cannot[off] = 'the function changed (the hook may assume its 1.0.0 layout)'
    kept, status, safe, reasons = hook_policy(cannot, gen_hooks)
    report['hooks'] = {hex(o): reasons.get(hex(o), 'hooked') for o in gen_hooks}
    vlocal = [porter.fwd[o] for o in lists.get('vlocal', []) if o in porter.fwd]
    import build                                  # (gen_main_lists put tools/engine on the path)
    giant_hot = [f'm_{porter.fwd[int(n[2:], 16)]:x}' for n in build.GIANT_HOT if int(n[2:], 16) in porter.fwd]
    engine = dict(version=version, define=version_number(version), fn_prefix='m', status=status, safe_mode=safe,
                  hook=[hex(porter.fwd[o]) for o in sorted(kept)],
                  alias={hex(k): hex(v) for k, v in sorted(alias.items())},
                  vlocal=[hex(v) for v in sorted(vlocal)], giant_hot=sorted(giant_hot),
                  unplaced_hooks=[hex(o) for o in sorted(cannot) if o not in porter.fwd],
                  dropped_hooks=reasons)
    return addrs, engine, report


def main_engine_map(args):
    addrs, engine, report = generate(args.old, args.new, args.version)
    n = version_number(args.version)
    inc = ROOT / 'engine' / 'runtime' / f'totk_main_v{n}.inc'
    js = ROOT / 'games' / 'totk' / 'patches' / f'v{n}.engine.json'
    inc.write_text(table_inc(addrs.items(), args.version), encoding='utf-8', newline='\n')
    js.write_text(json.dumps(engine, indent=1) + '\n', encoding='utf-8', newline='\n')
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(status=engine['status'], safe_mode=engine['safe_mode'], wrote=[str(inc), str(js)],
                          addresses=len(addrs), aliases=len(engine['alias']), hooks=len(engine['hook']),
                          dropped_hooks=engine['dropped_hooks'], unplaced_hooks=engine['unplaced_hooks'])))
    return 0 if engine['status'] == 'pass' else 1
