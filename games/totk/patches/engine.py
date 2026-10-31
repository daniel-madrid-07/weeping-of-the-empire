"""The code patches for engine (totk-recomp, `engine/`): its gen.py lifts one C++ function per.eh_frame FDE and traps on
any branch to an address that is neither inside the current function nor an FDE start. Our veneers live in the text
padding (no FDE) and are entered by a `b` from the middle of a function and leave by a `b` back into it, so for engine
each veneer block becomes an extension range of the function that owns its site (gen.py decodes the FDE range plus
these ranges as one body: `ext` in the funcs JSON), and a block entered by `bl` becomes a function of its own (an
added FDE entry).

Everything is derived from the table and checked: every block is entered from exactly one site, the site lies inside
an FDE, the block leaves only to that function (or, for a `bl` block, to an FDE start), no block overlaps an FDE and
no branch crosses between blocks."""
import struct

SIGN = lambda v, bits: v - (1 << bits) if v & (1 << (bits - 1)) else v


def branch(word, at):
    """(kind, target) of a direct branch word at `at`, or None. kind: 'b', 'bl' or 'cond' (b.cc, cbz/cbnz, tbz/tbnz)."""
    if word & 0x7c000000 == 0x14000000:
        return ('bl' if word & 0x80000000 else 'b'), at + SIGN(word & 0x3ffffff, 26) * 4
    if word & 0xff000010 == 0x54000000:
        return 'cond', at + SIGN((word >> 5) & 0x7ffff, 19) * 4
    if word & 0x7e000000 == 0x34000000:
        return 'cond', at + SIGN((word >> 5) & 0x7ffff, 19) * 4
    if word & 0x7e000000 == 0x36000000:
        return 'cond', at + SIGN((word >> 5) & 0x3fff, 14) * 4
    return None


class EngineError(Exception):
    pass


def layout(table, features, fde):
    """fde: {start: size} (the engine's funcs JSON). Returns {'ext': {fn: [(a, b), ...]}, 'fde_add': {start: size},
    'blocks': [...]} for the selected features' veneers."""
    g = table['game']
    text_end = g['text_end']
    rows = [p for p in table['patch'] if p['feature'] in features]
    words = {p['offset']: struct.unpack('<I', p['new'])[0] for p in rows}
    starts = sorted(fde)
    import bisect

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 and a < starts[i] + fde[starts[i]] else None

    entries = {}                                   # block start -> (kind, site)
    for off, w in words.items():
        if off >= text_end:
            continue
        br = branch(w, off)
        if br and br[1] >= text_end:
            if br[0] == 'cond':
                raise EngineError(f'{off:#x}: conditional branch into the padding')
            if br[1] in entries:
                raise EngineError(f'{br[1]:#x}: veneer entered from {entries[br[1]][1]:#x} and {off:#x}')
            entries[br[1]] = (br[0], off)
    pad = sorted(o for o in words if o >= text_end)
    if not pad:
        return dict(ext={}, fde_add={}, blocks=[])
    # A veneer range ([[veneer]]) that no site enters is a part of another veneer, entered by a `b` from it (the mouse
    # input veneer's pitch tail): a block of the same function.
    chained = {v['start']: None for v in table.get('veneer', []) if v['feature'] in features and v['start'] not in entries}
    for off in pad:
        br = branch(words[off], off)
        if br and br[0] == 'b' and br[1] in chained:
            if chained[br[1]] is not None:
                raise EngineError(f'{br[1]:#x}: veneer part entered from {chained[br[1]]:#x} and {off:#x}')
            chained[br[1]] = off
    for t, frm in chained.items():
        if frm is None:
            raise EngineError(f'veneer range {t:#x}: entered from no site and no other veneer')
    bounds = sorted(set(entries) | set(chained))
    if bounds[0] != pad[0]:
        raise EngineError(f'padding word {pad[0]:#x} before the first veneer entry {bounds[0]:#x}')
    end = pad[-1] + 4
    spans = []
    for i, s in enumerate(bounds):
        nxt = bounds[i + 1] if i + 1 < len(bounds) else end
        e = s
        while e < nxt and e in words:
            e += 4
        if any(o in words for o in range(e, nxt, 4)):     # alignment between veneers stays unpatched (zero)
            raise EngineError(f'veneer block {s:#x}..{e:#x}: patched words after its end, before {nxt:#x}')
        if owner(s) is not None or owner(e - 4) is not None:
            raise EngineError(f'veneer block {s:#x}..{e:#x} overlaps an FDE')
        spans.append((s, e))

    def span_of(a):
        return next(((s, e) for s, e in spans if s <= a < e), None)
    fn_of, kind_of, site_of = {}, {}, {}
    for s, e in spans:
        if s in entries:
            kind_of[s], site_of[s] = entries[s]
            fn = owner(site_of[s])
            if fn is None:
                raise EngineError(f'site {site_of[s]:#x} (veneer {s:#x}) is not inside an FDE')
            fn_of[s] = fn
    for _ in spans:                                    # chained parts take their entering block's function
        for s, e in spans:
            if s in chained and s not in fn_of:
                parent = span_of(chained[s])
                if parent and parent[0] in fn_of:
                    fn_of[s], kind_of[s], site_of[s] = fn_of[parent[0]], kind_of[parent[0]], chained[s]
    blocks = []
    for s, e in spans:
        if s not in fn_of:
            raise EngineError(f'veneer part {s:#x}..{e:#x}: no function reaches it')
        kind, site, fn = kind_of[s], site_of[s], fn_of[s]
        last = branch(words[e - 4], e - 4)
        if words[e - 4] != 0xd65f03c0 and (not last or last[0] != 'b'):
            raise EngineError(f'veneer block {s:#x}..{e:#x} does not end with b or ret')
        for o in range(s, e, 4):
            br = branch(words[o], o)
            if not br or s <= br[1] < e:
                continue
            t = br[1]
            if kind == 'b' and br[0] != 'bl' and (owner(t) == fn or fn_of.get((span_of(t) or (None,))[0]) == fn):
                continue                           # back into the owning function, or another part of its veneer
            if br[0] in ('b', 'bl') and t in fde:
                continue                           # a call or tail call to a function
            raise EngineError(f'{o:#x}: branch to {t:#x} leaves veneer {s:#x}..{e:#x} (owner {fn:#x})')
        blocks.append(dict(start=s, end=e, entered_by=kind, site=site, function=fn))
    ext, fde_add = {}, {}
    for b in blocks:
        if b['entered_by'] == 'bl':
            fde_add[b['start']] = b['end'] - b['start']
        else:
            ext.setdefault(b['function'], []).append((b['start'], b['end']))
    return dict(ext=ext, fde_add=fde_add, blocks=blocks)


def funcs_json(funcs, lay):
    """the engine's funcs JSON with the veneer functions added to `fde` and the extension ranges under `ext`."""
    out = dict(funcs)
    fde = dict(funcs['fde'])
    for s, n in lay['fde_add'].items():
        fde[hex(s)] = n
    out['fde'] = {k: fde[k] for k in sorted(fde, key=lambda k: int(k, 16))}
    out['ext'] = {hex(fn): [[hex(a), hex(b)] for a, b in rs] for fn, rs in sorted(lay['ext'].items())}
    return out


def build_patch_set(table, features, lay, safe=()):
    """tools/engine/build.py's --patches JSON: one entry per changed word (original checked), plus the veneer layout."""
    rows = [p for p in table['patch'] if p['feature'] in features and p['original'] != p['new']]
    hooks = sorted({h for f in features for h in HOOKS_NEEDED.get(f, ())})
    return {
        **({'hooks': hooks} if hooks else {}),   # only when needed: an unchanged set keeps its JSON (and its key)
        'name': 'totk-' + '+'.join(features) if features else 'totk-none',
        'features': list(features),
        'patches': [dict(module='main', offset=hex(p['offset']), bytes=p['new'].hex(), expect=p['original'].hex(),
                         name=p['feature']) for p in rows],
        'fde_add': {hex(s): n for s, n in sorted(lay['fde_add'].items())},
        'ext': {hex(f): [[hex(a), hex(b)] for a, b in rs] for f, rs in sorted(lay['ext'].items())},
        'runtime_env': runtime_env(features, safe),
    }


# Features whose patched code only works with an engine hook (gen.py --hook <off>, GEN_MAIN in tools/engine/build.py):
# vk_guard's guard reads virtual key 27, which only hook_f_1ccf894 (engine/runtime/totk_pc_vk.cpp) sets from the pad.
# build.py refuses a patch set whose hooks are not all in GEN_MAIN.
HOOKS_NEEDED = {'vk_guard': ('1ccf894',)}
# Features `default` means here on top of the table's defaults: they need an engine hook, so the generic `apply` path
# (no hooks) leaves them off. `-name` after `default` still drops one.
ENGINE_DEFAULT = ('vk_guard',)


def engine_spec(spec):
    """--features for the engine: each `default` also selects ENGINE_DEFAULT."""
    items = [s.strip() for s in (spec or 'default').split(',') if s.strip()]
    return ','.join(x for s in items for x in ([s, *ENGINE_DEFAULT] if s == 'default' else [s]))


def runtime_env(features, safe=()):
    """What the engine's runtime needs with these features: `realtime` rewrites the frame step inside f_7a83e4, which
    the engine's TOTK_SPEED_FIX hook rescales again after the call, so the two must not both run. `safe` (another game
    version's safe_mode, safe_mode()): with cap30 nothing paces above 30 fps, not the loading screens nor the title's
    burst either (without the frame-time hooks every frame steps game time by 1/30)."""
    env = {}
    if 'realtime' in features:
        env['TOTK_SPEED_FIX'] = '0'
    if 'cap30' in safe:
        env.update(TOTK_LOAD_FPS='0', TOTK_TITLE_FPS='0', TOTK_FPS='30', TOTK_UNLOCK_FPS='0')   # (the runtime holds
        # them too, over settings.ini: build.py compiles it with -DTOTK_CAP30)
    return env


def safe_mode(table_path, table):
    """Another game version's safe_mode, from the engine map next to its table (v143.toml -> v143.engine.json,
    engine_map.py): what the hooks its build lost force. 1.0.0: none."""
    from pathlib import Path
    if table['game']['version'] == '1.0.0':
        return []
    eng = Path(table_path).with_name(Path(table_path).stem + '.engine.json')
    if not eng.exists():
        raise EngineError(f'{eng} is missing (python -m games.totk.patches engine-map)')
    import json
    return list(json.loads(eng.read_text(encoding='utf-8')).get('safe_mode', []))
