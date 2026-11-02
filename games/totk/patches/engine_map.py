"""The engine's 1.0.0 main addresses, placed in another game version.

    python -m games.totk.patches engine-map --old <v100 exefs/main> --new <v143 exefs/main> --version 1.4.3
                                            [--report engine-map-report.json]

The engine's own code names main by 1.0.0 offsets in three ways:
  - addresses: every one goes through totk_version.h (totk_main / totk_main_off / totk_global / decomp_bridge::rebase);
  - function names: hook_f_<off> / f_<off>_orig of the GEN_MAIN hooks and direct f_<off> calls;
  - GEN_MAIN's offset lists (--hook, --vlocal) in tools/engine/build.py.
Struct fields (totk_field(Struct, 0x<1.0.0 offset>)) are not the porter's: --fields takes a hand-made map
({"Actor": {"0x220": "0x228"}}, private like the table) and engine/runtime/totk_fields_v<NNN>.inc defines
TF_<Struct>_<off> for every field the sources name and the map places. A field it does not place stays undefined, so
the build stops at its use (--diagnostic: TOTK_FIELD_UNKNOWN instead).
`collect` finds them in the sources: every hex constant in a main offset's range is a
candidate, so nothing is missed by a pattern; constants that are not addresses simply find no place and are left out.
`generate` runs the porter and writes engine/runtime/totk_main_v<NNN>.inc (the address table totk_version.h
includes) and games/totk/patches/v<NNN>.engine.json (gen.py's --hook / --alias / --vlocal for that build):
  - an address is placed only as the porter places patch sites: the same instruction of a confirmed function pair, a
    data address with a clear majority of references (or one reference its content confirms: a vtable's paired
    slots, the same bytes), or unique read-only content (a string, a table);
  - a function name gets an alias when its function pair is confirmed; a hook stays a hook only when the two bodies
    are equal after normalization ("exact": same struct offsets, so the hook's C++ still fits), otherwise it is
    aliased unhooked and the game runs its own recompiled code there;
  - --pins (a hand-made map {"0x<1.0.0 fn>": {"new": "0x<fn>", "body": "<its masked body hash>", "why": "<review
    note>", "checked": "<the hook's C++ read against that build>"}}) places a pair the porter could not confirm and
    keeps a hook whose body changed: someone checked the hook's C++ against that build and routed its moved fields
    through totk_field (--fields). apply_pins refuses a target that is not a function start there, is another
    function's confirmed pair or another pin's, whose masked body hash (pin_body: SHA-256 of version_port's normalized
    words, address-only operands masked) is not the pinned one (another build: never hook other code), and a pin
    without its why or its checked note (the mask ignores call targets: someone read the code). A refused pin pairs
    nothing: its function stays as the porter left it, so its hook is dropped under the usual policy (a required one
    fails a full build, turns its feature off in a beta), the refusal is listed (pins_refused) and logged, never a guess.
    A pin whose 1.0.0 key is not a function start is an anchored address (a return address, one instruction, a data
    slot the engine names): its 'body' is the masked hash of the one function of that build that contains the new
    address (code) or completes an ADRP reference to it (data); it places that address and nothing else.
  - games/totk/patches/pins_v<NNN>.json and fields_v<NNN>.json are those maps for a build, committed and shipped
   : numbers and short notes in our own
    words only, no instruction bytes, disassembly, strings or names from the game (check_pins / check_fields: the
    schema export_public enforces too). engine-map reads them when --pins / --fields are not given (Setup's builder
    runs it so on the player's PC). A shipped field entry {"new": "0x<off>", "for": ["0x<1.0.0 address>", ...]} names
    its evidence: each 1.0.0 load / store (or address add) of Struct+off. It is applied only when every piece holds on
    that image: the instruction's function is a confirmed porter pair or a passing
    pin, and the 1.4.3 instruction aligned with it (version_port's normalized bodies) is the same access with the
    offset 'new'; where several functions reach the field, two pieces are listed and must agree. Any failure leaves the
    field out (its readers on the beta sentinel) with one log line; no partial use. A private --fields map stays flat
    {"Struct": {"0x<off>": "0x<off>"}}.
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
FIELD = re.compile(r'\btotk_field(?:_known)?\(\s*([A-Za-z_]\w*)\s*,\s*(0x[0-9a-f]+)\s*\)')   # totk_version.h
FIELD_UNKNOWN = 0x7FFFFFF0   # totk_version.h TOTK_FIELD_UNKNOWN


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
# A beta build of another version (engine-map --beta, review 11-01) may ship without a REQUIRED hook only when the
# hook belongs to one of these feature groups: the feature is then off by design (the key's beta_runtime_env), never
# half on. A required hook outside them still fails the build.
BETA_GROUPS = {
    0x83e3f8: 'pc_menus', 0x1ccf894: 'pc_virtual_keys',
    0xf6092c: 'options_screen', 0x1a862e4: 'options_screen', 0x1a86420: 'options_screen', 0x1a85b3c: 'options_screen',
    0x1a8604c: 'options_screen', 0x1a85e4c: 'options_screen', 0x1a06840: 'options_screen', 0x1a068cc: 'options_screen',
}
BETA_ENV = {   # what turns each group's feature off (pc_input.cpp scheme, totk_menu_cursor.cpp, totk_options_menu.cpp)
    'pc_menus': {'TOTK_CONTROLS': 'console', 'TOTK_MENU_CURSOR': '0'},
    'pc_virtual_keys': {'TOTK_CONTROLS': 'console'},
    'options_screen': {'TOTK_OPTIONS_MENU': '0'},
}
HOOK_CAP30 = {
    0x7a83e4: 'the frame-delta setter (the speed fix)', 0x8f689c: 'the present-interval minimum',
    0xf15364: 'map scroll', 0xf0fdec: "Hero's Path step", 0x16e4728: 'Walk stop delay', 0x1cd0260: 'stick spin',
    0x16e4610: 'struggle spin', 0x1aa3e44: 'get-item info delays', 0x16e4b80: 'Cucco glide ease',
    0x1d23614: 'tumble-slide sound', 0xa5f010: 'lightning rolls', 0x1d97648: 'event waits', 0x1c17fb0: 'event waits', 0x10bc078: 'event waits', 0x13bd680: 'event waits',
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
             0x11276cc: 'loader thread count (A/B)',
             0x9c2ef0: 'aal flush hold: without it the runtime keeps its 180 fps hold (hle_vi audio_fps_max), under the '
                       '200 where the jam watchdog cuts audio',
             0x9c2b70: "aal jam watchdog in time: without it the game's own 300-call count runs (10 s at 30 fps); with "
                       'the flush hold (or the 180 fps hold) no jam forms above 200',
             0xabb518: 'ambient one-shot settle in game time: without it the game plays, short ambient one-shots just '
                       'come closer together above 30 fps',
             0x91a290: "map temperature sign: without it the sign follows the game's language (the number still "
                       'follows the setting where the tempunit patch is placed)'}


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
                    if n.endswith(SOURCE_EXT) and not n.startswith(('totk_main_v', 'totk_fields_v')):
                        yield Path(dirpath) / n


def gen_main_lists():
    """GEN_MAIN's --hook, --vlocal and --split offsets (tools/engine/build.py)."""
    sys.path.insert(0, str(ROOT / 'tools' / 'engine'))
    import build
    out = {}
    for i, a in enumerate(build.GEN_MAIN[:-1]):
        if a in ('--hook', '--vlocal', '--split'):
            out[a[2:]] = [int(x, 16) for x in build.GEN_MAIN[i + 1].split(',')]
    return out


def collect(root=ROOT):
    """{'offsets': main offsets named as hex constants, 'functions': offsets named by f_<off> symbols,
    'fields': (struct, 1.0.0 offset) pairs named by totk_field}."""
    offsets, functions, fields = set(), set(), set()
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
        if p.name != 'totk_version.h':   # (its comments name an example)
            for m in FIELD.finditer(text):
                fields.add((m.group(1), m.group(2)))
    return dict(offsets=sorted(offsets), functions=sorted(functions), fields=sorted(fields))


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


def fields_inc(fields, field_map, version, diagnostic=False):
    """totk_fields_v<NNN>.inc: TF_<Struct>_<off> for each (struct, off) the sources name and field_map places;
    returns (text, unplaced pairs)"""
    lines = [f'// GENERATED by `python -m games.totk.patches engine-map` for TotK {version}: do not edit.',
             "// TF_<Struct>_<1.0.0 offset> = the field's offset in this build (totk_version.h totk_field).",
             '#pragma once']
    unplaced = []
    for st, off in fields:
        new = (field_map.get(st) or {}).get(off)
        if new is not None:
            lines.append(f'#define TF_{st}_{off} {int(new, 16):#x}')
            continue
        unplaced.append((st, off))
        if diagnostic:
            lines.append(f'#define TF_{st}_{off} {FIELD_UNKNOWN:#x}u')
    return '\n'.join(lines) + '\n', unplaced


SENTINEL = 0xFFFFFFF0   # totk_version.h TOTK_MAIN_UNKNOWN: an offset a diagnostic build could not place


HEX_RE = re.compile(r'^0x[0-9a-f]{1,8}$')
BODY_RE = re.compile(r'^[0-9a-f]{64}$')
NAME_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]{0,47}$')      # a totk_field struct name of our sources
NOTE_MAX = 200
PIN_KEYS = {'new', 'body', 'why', 'checked'}


def _note_problem(v):
    """A pin note: short printable ASCII in our own words, no hex run over 16 digits or 8-digit word, no mangled /
    qualified name (as tools/release/export_public.py NOTE_BAD)."""
    if not isinstance(v, str) or not v.strip():
        return 'empty'
    if len(v) > NOTE_MAX:
        return f'longer than {NOTE_MAX}'
    if any(not (32 <= ord(ch) < 127) for ch in v):
        return 'not printable ASCII'
    if re.search(r'[0-9a-fA-F]{17,}', v):
        return 'a hex run over 16 digits'
    if re.search(r'\b[0-9a-fA-F]{8}\b', v):
        return 'an 8-digit hex word (an instruction?)'
    if re.search(r'_Z[A-Za-z0-9_]|@@|::|\?[A-Za-z_]+@', v):
        return "a game name (mangled or Class::method)"
    return None


def check_pins(d, draft=False):
    """Schema of a shipped pins_v<NNN>.json (export_public enforces it too): problems, [] when it holds. draft: a private
    --pins file, which may leave each body out (engine-map refuses that pin and prints the body to paste)."""
    if not isinstance(d, dict):
        return ['not an object']
    out = []
    for k, v in d.items():
        if not isinstance(k, str) or not HEX_RE.match(k):
            out.append(f'{k!r}: key is not 0x<hex>')
            continue
        if draft and isinstance(v, dict) and 'body' not in v:   # (a draft --pins: engine-map tells each body)
            v = dict(v, body='0' * 64)
        if not isinstance(v, dict) or set(v) != PIN_KEYS:
            out.append(f'{k}: needs exactly {sorted(PIN_KEYS)}')
            continue
        if not isinstance(v['new'], str) or not HEX_RE.match(v['new']):
            out.append(f'{k}: new is not 0x<hex>')
        if not isinstance(v['body'], str) or not BODY_RE.match(v['body']):
            out.append(f'{k}: body is not 64 hex digits')
        for n in ('why', 'checked'):
            why = _note_problem(v[n])
            if why:
                out.append(f'{k}: {n}: {why}')
    return out


def check_fields(d, structs=None):
    """Schema of a shipped fields_v<NNN>.json: {"Struct": {"0x<1.0.0 off>": {"new": "0x<off>", "for": ["0x<fn>"]}}}.
    Struct is one our sources already name in totk_field(Struct,...) (structs; default: collected from them), so the
    file can never bring a new name."""
    if not isinstance(d, dict):
        return ['not an object']
    structs = {st for st, _ in collect()['fields']} if structs is None else set(structs)
    out = []
    for st, offs in d.items():
        if not isinstance(st, str) or not NAME_RE.match(st) or not isinstance(offs, dict):
            out.append(f'{st!r}: not a struct name with an object of offsets')
            continue
        if st not in structs:
            out.append(f"{st}: not a struct our sources name in totk_field (the file may not bring a new name)")
            continue
        for k, v in offs.items():
            if not isinstance(k, str) or not HEX_RE.match(k):
                out.append(f'{st} {k!r}: key is not 0x<hex>')
            elif (not isinstance(v, dict) or set(v) != {'new', 'for'} or not isinstance(v['new'], str)
                  or not HEX_RE.match(v['new']) or not isinstance(v['for'], list) or not v['for']
                  or not all(isinstance(x, str) and HEX_RE.match(x) for x in v['for'])):
                out.append(f'{st} {k}: needs {{"new": "0x<hex>", "for": ["0x<hex>", ...]}}')
    return out


def mem_imm(w):
    """(the instruction with its offset masked, the byte offset) of an LDR / STR (unsigned offset,
    any size, general or SIMD), LDUR / STUR, LDP / STP (signed offset) or ADD (immediate) word; None for anything else"""
    if w & 0x3b000000 == 0x39000000:                       # LDR / STR, unsigned 12-bit offset, scaled
        size, v, opc = w >> 30, (w >> 26) & 1, (w >> 22) & 3
        scale = 4 if v and opc & 2 else size
        return w & ~(0xfff << 10), ((w >> 10) & 0xfff) << scale
    if w & 0x3b200c00 == 0x38000000:                       # LDUR / STUR, signed 9-bit offset
        imm = (w >> 12) & 0x1ff
        return w & ~(0x1ff << 12), imm - 0x200 if imm & 0x100 else imm
    if w & 0x3a000000 == 0x28000000 and (w >> 23) & 3 == 2:   # LDP / STP, signed 7-bit offset, scaled
        opc, v = w >> 30, (w >> 26) & 1
        scale = (2 + opc) if v else (3 if opc & 2 else 2)
        imm = (w >> 15) & 0x7f
        return w & ~(0x7f << 15), (imm - 0x80 if imm & 0x40 else imm) << scale
    if w & 0x7f800000 == 0x11000000:                       # ADD (immediate), optionally << 12
        return w & ~(0xfff << 10) & ~(1 << 22), ((w >> 10) & 0xfff) << (12 if w & (1 << 22) else 0)
    return None


def _word(img, off):
    return int(img.words[(off - img.text_off) // 4])


REG_OFFSET = 0x3b200c00, 0x38200800   # LDR / STR (register offset): [xn, xm{, extend / shift}]


def _writer(img, fs, at, reg, back=8):
    """the last instruction before at (in the function from fs, at most `back` back) that writes reg (rd field, a
    data-processing or load word), as (address, word), or None"""
    for a in range(at - 4, max(fs, at - 4 * back) - 4, -4):
        w = _word(img, a)
        if w & 31 == reg and (w & 0x1c000000 in (0x10000000, 0x08000000, 0x18000000) or w & 0x0a000000 == 0x0a000000 or
                              mem_imm(w) is not None or w & REG_OFFSET[0] == REG_OFFSET[1]):
            return a, w
    return None


def access(img, fs, at):
    """(kind, offset, form) of the access at at: mem_imm's forms ('imm'); a register-offset load / store whose index was
    set by a movz within 8 instructions before ('mov': the literal is the offset, the index register in the kind);
    or a register-offset access whose index is computed ('reg': the field is at +0 of the base, the whole word in the
    kind). None for anything else."""
    w = _word(img, at)
    m = mem_imm(w)
    if m is not None:
        return m[0], m[1], 'imm'
    if w & REG_OFFSET[0] != REG_OFFSET[1]:
        return None
    rm = (w >> 16) & 31
    wr = _writer(img, fs, at, rm)
    if wr and wr[1] & 0x7f800000 == 0x52800000 and not (wr[1] >> 21) & 3:   # movz wN / xN, #imm16 (no shift)
        return w, (wr[1] >> 5) & 0xffff, 'mov'
    return w, 0, 'reg'


def shape(w):
    """the word with its registers masked where an instruction class names them (the constant idiom's comparison: the
    same instructions and immediates, any registers)"""
    if w & 0x1f800000 == 0x12800000:            # MOVZ / MOVN / MOVK
        return w & ~31
    if w & 0x1f000000 == 0x0a000000:            # logical (shifted register): AND / ORR / EOR / ANDS (TST)
        return w & ~((31 << 5) | (31 << 16) | (0 if w & 31 == 31 else 31))
    if w & 0x1f000000 == 0x11000000 or mem_imm(w) is not None:   # ADD / SUB immediate, loads / stores
        return w & ~0x3ff
    return w


IDIOM = 3   # the instructions after an access that must repeat, immediates equal, for the constant-idiom form


def _idiom_matches(img, fs, size, kind_shape, idiom):
    out = []
    for a in range(fs, fs + size - 4 * IDIOM, 4):
        acc = access(img, fs, a)
        if acc is None or acc[2] != 'imm' or acc[0] & ~0x3ff != kind_shape:
            continue
        if [shape(_word(img, a + 4 * (k + 1))) for k in range(IDIOM)] == idiom:
            out.append((a, acc[1]))
    return out


def _rebased(porter, fo, at, fn, at1, m0, m1, new):
    """the re-based form: 1.0.0's access goes through rn = (rb + C) (an add #C, possibly null-checked by a csel of
    xzr), 1.4.3's through rb itself with offset C + field, while its function still builds rb + C before it. None
    when it holds, else why not."""
    def base_add(img, fs, a, reg):
        for _ in range(3):   # (through 'csel reg, xzr, reg' / 'csel reg, reg, xzr' null checks)
            wr = _writer(img, fs, a, reg, 16)
            if wr is None:
                return None
            a, w = wr
            if w & 0x7fe00c00 == 0x1a800000 and ((w >> 5) & 31 in (31, reg)) and ((w >> 16) & 31 in (31, reg)):
                continue
            if w & 0xffc00000 == 0x91000000:   # add xd, xn, #imm (64-bit, no shift)
                return (w >> 5) & 31, (w >> 10) & 0xfff
            return None
        return None
    w0, w1 = _word(porter.old, at), _word(porter.new, at1)
    b0 = base_add(porter.old, fo, at, (w0 >> 5) & 31)
    if b0 is None:
        return f'{at:#x}: its base is not built by an add'
    rb1, c = (w1 >> 5) & 31, b0[1]
    built = any(_word(porter.new, a) & 0xffc00000 == 0x91000000 and (_word(porter.new, a) >> 5) & 31 == rb1 and
                (_word(porter.new, a) >> 10) & 0xfff == c for a in range(max(fn, at1 - 64), at1, 4))
    if not built:
        return f'{at:#x}: 1.4.3 does not build the same base + {c:#x} before the access'
    if m1[1] != c + new:
        return f'{at:#x}: the re-based access has offset {m1[1]:#x}, not {c:#x} + {new:#x}'
    return None


def field_evidence(porter, at, off, new, pinned, kinds=None):
    """None when the 1.0.0 access at `at` (of a field at off) is matched on this image by the same access at offset new
    (its function a confirmed pair or a passing pin, the instruction aligned); else why not"""
    import difflib
    fo = porter.func_of(0, at)
    if fo is None:
        return f'{at:#x} is in no 1.0.0 function'
    if fo not in porter.fwd or not (fo in pinned or porter.confirmed(fo)):
        return f'its function {fo:#x} has no confirmed pair or passing pin'
    m0 = access(porter.old, fo, at)
    if m0 is None or m0[1] != off:
        return f'{at:#x} is not an access at {off:#x}'
    fn = porter.fwd[fo]
    x = (at - fo) // 4
    a, b = porter._norm(0, fo).tolist(), porter._norm(1, fn).tolist()

    def idiom():   # the constant-idiom form: the access + the next IDIOM instructions, unique in both functions
        if m0[2] != 'imm':
            return None
        sh, idi = m0[0] & ~0x3ff, [shape(_word(porter.old, at + 4 * (k + 1))) for k in range(IDIOM)]
        olds = _idiom_matches(porter.old, fo, porter.size_o[fo], sh, idi)
        news = _idiom_matches(porter.new, fn, porter.size_n[fn], sh, idi)
        if [o for o, _ in olds] != [at] or len(news) != 1:
            return f'{at:#x}: the access and its next {IDIOM} instructions are not unique in both functions'
        if news[0][1] != new:
            return f'{at:#x}: the idiom\'s access has offset {news[0][1]:#x}, not {new:#x}'
        if kinds is not None:
            kinds.append('idiom')
        return 'ok'

    for tag, a0, a1, b0, b1 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if a0 <= x < a1:
            if tag == 'equal' or (tag == 'replace' and a1 - a0 == b1 - b0):
                at1 = fn + 4 * (b0 + x - a0)
                m1 = access(porter.new, fn, at1)
                if m1 is not None and m1[2] == m0[2] == 'imm' and m1[0] != m0[0] and \
                        m1[0] | (31 << 5) == m0[0] | (31 << 5):   # only the base register differs: re-based?
                    why = _rebased(porter, fo, at, fn, at1, m0, m1, new)
                    if why is None and kinds is not None:
                        kinds.append('rebased')
                    return why
                if m1 is not None and m1[2] == m0[2] == 'imm' and m1[0] != m0[0] and m1[0] | 31 == m0[0] | 31 and                         _word(porter.old, at) & 0x3a000000 != 0x28000000:   # only the transfer register renamed
                    # (the address is base + offset either way: the same evidence as an exact match; not for a pair)
                    if m1[1] != new:
                        return f'{at:#x}: the aligned access has offset {m1[1]:#x}, not {new:#x}'
                    if kinds is not None:
                        kinds.append('aligned')
                    return None
                if m1 is None or m1[0] != m0[0] or m1[2] != m0[2]:
                    i = idiom()
                    return None if i == 'ok' else f'{at:#x}: the aligned instruction is another one'
                if m1[1] != new:
                    return f'{at:#x}: the aligned access has offset {m1[1]:#x}, not {new:#x}'
                if m0[2] != 'imm':   # a register-offset word proves no base: rn's last writer must match too
                    wb0 = _writer(porter.old, fo, at, (_word(porter.old, at) >> 5) & 31)
                    wb1 = _writer(porter.new, fn, at1, (_word(porter.new, at1) >> 5) & 31)
                    if (wb0 is None) != (wb1 is None) or (wb0 and shape(wb0[1]) != shape(wb1[1])):
                        return f'{at:#x}: its base register is set differently in the two builds'
                if kinds is not None:
                    kinds.append('aligned' if m0[2] == 'imm' else m0[2])
                return None
            if tag == 'replace':   # a rewritten stretch of another length: the one same access in it (opcode, rt, rn)
                same = [i for i in range(b0, b1)
                        if (lambda m: m is not None and m[0] == m0[0])(mem_imm(_word(porter.new, fn + 4 * i)))]
                if len(same) == 1:
                    got = mem_imm(_word(porter.new, fn + 4 * same[0]))[1]
                    if got != new:
                        return f'{at:#x}: the matching access has offset {got:#x}, not {new:#x}'
                    if kinds is not None:
                        kinds.append('stretch')
                    return None
                i = idiom()
                return None if i == 'ok' else (f'{at:#x}: {len(same)} same accesses in its rewritten stretch of '
                                               f'{fn:#x} (none or ambiguous)' + (f'; {i}' if i else ''))
            return f'{at:#x}: not aligned in {fn:#x}'
    return f'{at:#x}: outside its function'


def gated_fields(shipped, porter, pinned, report=None):
    """(the flat field map fields_inc takes, refusals) from a checked shipped map: an entry when all its evidence holds.
    A piece matched only inside a rewritten stretch ('stretch', weaker than an aligned one: a lone same-shape access
    there can be the next member) needs a second agreeing piece, aligned or in another function. report:
    {'Struct+off': [[address, 'aligned' | 'stretch'],...]} for the audit."""
    out, refused = {}, []
    for st, offs in shipped.items():
        for k, v in offs.items():
            bad, kinds = [], []
            for x in v['for']:
                why = field_evidence(porter, int(x, 16), int(k, 16), int(v['new'], 16), pinned, kinds)
                if why:
                    bad.append(why)
            pieces = list(zip([int(x, 16) for x in v['for']], kinds)) if not bad else []
            if report is not None and pieces:
                report[f'{st}+{k}'] = [[hex(a), kd] for a, kd in pieces]
            if not bad and 'stretch' in kinds:
                fns = {porter.func_of(0, a) for a, _ in pieces}
                if not ('aligned' in kinds or len(fns) > 1):
                    bad.append('matched only inside a rewritten stretch: a second agreeing piece (aligned, or in '
                               'another function) is needed')
            if bad:
                refused.append(f'{st}+{k}: {bad[0]}')
            else:
                out.setdefault(st, {})[k] = v['new']
    return out, refused


def shipped_maps(version):
    """(pins, fields, problems) from games/totk/patches/pins_v<NNN>.json / fields_v<NNN>.json ({} where absent). A file
    that breaks the schema is not used at all (its problems are reported: the hooks it would pin stay dropped)."""
    n = version_number(version)
    got, problems = [], []
    for name, check in ((f'pins_v{n}.json', check_pins), (f'fields_v{n}.json', check_fields)):
        p = ROOT / 'games' / 'totk' / 'patches' / name
        d = json.loads(p.read_text(encoding='utf-8')) if p.exists() else {}
        bad = check(d)
        problems += [f'{name}: {b}' for b in bad]
        got.append({} if bad else d)
    return got[0], got[1], problems


def pin_body(porter, fn):
    """A pin's 'body': SHA-256 (64 hex) of the new build's function at fn as version_port normalizes it (B/BL targets,
    ADRP pages, ADR, LDR-literal and the low 12 bits completing an ADRP masked; every other bit as in the image)."""
    import hashlib
    return hashlib.sha256(porter._norm(1, fn).tobytes()).hexdigest()


def _bodies(porter):
    """{masked body hash: [the new build's function starts with it]} (pin_body over every function, once)"""
    if getattr(porter, '_pin_bodies', None) is None:
        porter._pin_bodies = {}
        for s in porter.size_n:
            porter._pin_bodies.setdefault(pin_body(porter, s), []).append(s)
    return porter._pin_bodies


def anchored_address(porter, n, body, version):
    """None when n is an address of the one function with masked body hash body (code) or one it completes an ADRP
    reference to (data); else why not."""
    if n is None:
        return f'no new address'
    fns = _bodies(porter).get(body, [])
    if len(fns) != 1:
        return (f'an address pin needs the masked body hash of the one {version} function that holds or references '
                f'{n:#x}: {len(fns)} function(s) have {body or "none"}' + (
                    f' (the function at {n:#x} has {pin_body(porter, porter.func_of(1, n))})'
                    if porter.func_of(1, n) is not None else ''))
    f = fns[0]
    if f <= n < f + porter.size_n[f]:
        return None if n % 4 == 0 else f'{n:#x} is inside {f:#x} but not on an instruction boundary'
    i, j = porter._slice(porter.new, f, porter.size_n[f])
    if (porter.an.ref_tgt[i:j] == n).any():
        return None
    return f'{n:#x} is neither in nor referenced by the function {f:#x} that has that body hash'


def apply_pins(porter, pins, version):
    """pins: {"0x<1.0.0 fn>": {"new": "0x<fn>", "body": "<hex>", "why": "...", "checked": "..."}} (--pins). Pairs each
    pinned function in porter (how 'pinned'; a different pair it had is replaced, an unconfirmed pair holding the
    target is dropped). Returns ({pinned 1.0.0 start: its pin}, problems): a problem leaves that pin out (its hook
    dropped by the usual policy). A pin without its body hash is refused with the hash the target has (to be read and
    pasted)."""
    pinned, problems, olds = {}, [], {int(k, 16) for k in pins}
    porter.pinned_addrs = {}
    for k, ent in sorted(pins.items(), key=lambda kv: int(kv[0], 16)):
        o = int(k, 16)
        n = int(str(ent.get('new', '')), 16) if str(ent.get('new', '')).strip() else None
        why = str(ent.get('why', '')).strip()
        if not why or not str(ent.get('checked', '')).strip():
            problems.append(f'{k}: a pin needs its why and its checked note (what was read against {version})')
            continue
        if o not in porter.size_o:
            why_not = anchored_address(porter, n, str(ent.get('body', '')).strip().lower(), version)
            if why_not:
                problems.append(f'{k}: {why_not}')
            else:
                porter.pinned_addrs[o] = n
            continue
        if n is None or n not in porter.size_n:
            problems.append(f'{k}: {ent.get("new")} is not a function start in {version}')
            continue
        body = pin_body(porter, n)
        if str(ent.get('body', '')).strip().lower() != body:
            problems.append(f'{k}: {n:#x} has the masked body hash {body}, the pin says '
                            f'{ent.get("body") or "none"} (another build, or the body to check and paste)')
            continue
        held = porter.rev.get(n)
        if held is not None and held != o and (held in olds or porter.confirmed(held)):
            problems.append(f'{k}: {n:#x} is already the pair of {held:#x}')
            continue
        if held is not None and held != o:
            porter.fwd.pop(held, None)
            porter.how.pop(held, None)
        if o in porter.fwd and porter.fwd[o] != n:
            porter.rev.pop(porter.fwd[o], None)
        porter._pair(o, n, 'pinned')
        pinned[o] = ent
    return pinned, problems


def generate(old_nso, new_nso, version, diagnostic=False, beta=False, pins=None, fields=None):
    sys.path.insert(0, str(ROOT / 'tools'))
    import version_port as vp
    from .port import placeable
    porter = vp.Porter(vp.load_image(str(old_nso)), vp.load_image(str(new_nso))).run()
    pinned, pin_problems = apply_pins(porter, pins or {}, version)
    found = collect()
    absent, pins_unused = [d for d in SOURCES if not (ROOT / d).exists()], []
    for o, n in porter.pinned_addrs.items():   # anchored address pins: only an address the engine names
        if o not in found['offsets']:
            if absent:   # named, if at all, by a source this tree lacks: not this build's, said quietly
                pins_unused.append(f'{o:#x} (no source here names it; absent: {", ".join(absent)})')
            else:
                pin_problems.append(f'{o:#x}: an address pin for an address the engine does not name')
    lists = gen_main_lists()
    addrs, report = {}, dict(stats=porter.stats(), addresses={}, functions={}, hooks={})
    for off in found['offsets']:
        if off in porter.pinned_addrs:
            addrs[off] = porter.pinned_addrs[off]
            report['addresses'][hex(off)] = dict(new=hex(porter.pinned_addrs[off]), how='pinned')
            continue
        r = porter.map(off)
        if r.get('how') in DATA_KINDS or placeable(r):
            addrs[off] = int(r['new'], 16)
        report['addresses'][hex(off)] = r
    alias, cannot = {}, {}
    gen_hooks = lists.get('hook', [])
    for off in sorted(set(found['functions']) | set(gen_hooks)):
        fo = off if off in porter.fwd else None
        if fo is None or not (fo in pinned or porter.confirmed(fo)):
            report['functions'][hex(off)] = dict(how=None, why='no confirmed function pair')
            if off in gen_hooks:
                cannot[off] = 'its function has no confirmed place'
            continue
        fn = porter.fwd[fo]
        alias[fn] = off
        addrs.setdefault(off, fn)
        exact = porter.hash_o[fo] == porter.hash_n[fn]
        report['functions'][hex(off)] = dict(new=hex(fn), how=porter.how[fo], exact=exact)
        if fo in pinned:
            report['functions'][hex(off)].update(why=pinned[fo]['why'], checked=pinned[fo].get('checked'))
        if off in gen_hooks and not exact and fo not in pinned:
            cannot[off] = 'the function changed (the hook may assume its 1.0.0 layout)'
    kept, status, safe, reasons = hook_policy(cannot, gen_hooks)
    report['hooks'] = {hex(o): reasons.get(hex(o), 'hooked') for o in gen_hooks}
    vlocal = [porter.fwd[o] for o in lists.get('vlocal', []) if o in porter.fwd]
    # the cold giants gen.py splits into chunks (build.py SPLIT_FUNCS: compile time and memory, not behaviour)
    split = [porter.fwd[o] for o in lists.get('split', []) if o in porter.fwd]
    import build                                  # (gen_main_lists put tools/engine on the path)
    giant_hot = [f'm_{porter.fwd[int(n[2:], 16)]:x}' for n in build.GIANT_HOT if int(n[2:], 16) in porter.fwd]
    engine = dict(version=version, define=version_number(version), fn_prefix='m', status=status, safe_mode=safe,
                  hook=[hex(porter.fwd[o]) for o in sorted(kept)],
                  alias={hex(k): hex(v) for k, v in sorted(alias.items())},
                  vlocal=[hex(v) for v in sorted(vlocal)], split=[hex(v) for v in sorted(split)],
                  giant_hot=sorted(giant_hot),
                  unplaced_hooks=[hex(o) for o in sorted(cannot) if o not in porter.fwd],
                  dropped_hooks=reasons)
    if pinned:   # (absent without --pins: the file as before)
        engine['pinned'] = {hex(o): hex(porter.fwd[o]) for o in sorted(pinned)}
    if porter.pinned_addrs:
        engine['pinned_addresses'] = {hex(o): hex(n) for o, n in sorted(porter.pinned_addrs.items())
                                      if o in found['offsets']}
    if pins_unused:    # (engine.json only: no stderr line, nothing failed)
        engine['pins_unused'] = pins_unused
    if pin_problems:   # each refused pin, said; its hook went the policy's way above (never a guessed pair)
        engine['pins_refused'] = pin_problems
    if diagnostic or beta:   # every engine address and name it could not place gets the sentinel / a trap stub
        for off in found['offsets']:
            addrs.setdefault(off, SENTINEL)
        placed = set(alias.values())
        engine['stubs'] = [hex(o) for o in sorted(set(found['functions']) | set(gen_hooks)) if o not in placed]
    if diagnostic:   # never shipped
        engine['status'], engine['diagnostic'] = 'diagnostic', True
    elif beta:       # shipped with the features of its dropped required hooks off (BETA_GROUPS); else it fails
        lost = sorted(o for o in cannot if o in HOOK_FAIL)
        stray = [hex(o) for o in lost if o not in BETA_GROUPS]
        groups = sorted({BETA_GROUPS[o] for o in lost if o in BETA_GROUPS})
        env = {}
        for g in groups:
            env.update(BETA_ENV[g])
        engine.update(status='fail' if stray else 'beta', diagnostic=False, beta=True, off_features=groups,
                      beta_runtime_env=env)
        if stray:
            engine['beta_refused'] = f'required hooks outside the beta groups: {", ".join(stray)}'
    if fields:   # the shipped field map's entries whose evidence holds here (report['fieldmap'], main_engine_map)
        report['field_evidence'] = {}
        report['fieldmap'], refused = gated_fields(fields, porter, pinned, report['field_evidence'])
        if refused:
            engine['fields_refused'] = refused
    return addrs, engine, report


def stubs_inc(stubs, version):
    """trap stubs for the 1.0.0 function names a diagnostic build could not place (totk_version.cpp includes it)"""
    lines = [f'// GENERATED by `python -m games.totk.patches engine-map --diagnostic` for TotK {version}: do not edit.',
             '// The engine names these 1.0.0 functions; this build has no place for them: a call stops with a message.']
    for o in stubs:
        x = int(o, 16)
        lines += [f'void f_{x:x}(Ctx* __restrict c) {{ (void)c; totk_main_unmapped({x:#x}); }}',
                  f'void f_{x:x}_orig(Ctx* __restrict c) {{ (void)c; totk_main_unmapped({x:#x}); }}']
    return '\n'.join(lines) + '\n'


def main_engine_map(args):
    ship_pins, ship_fields, ship_problems = shipped_maps(args.version)
    pins = json.loads(Path(args.pins).read_text(encoding='utf-8')) if getattr(args, 'pins', None) else ship_pins
    bad = check_pins(pins, draft=True) if getattr(args, 'pins', None) else []
    if bad:   # a private --pins file in the shipped form too (one schema; a draft may leave 'body' out to be told it)
        raise SystemExit('--pins: ' + '; '.join(bad))
    addrs, engine, report = generate(args.old, args.new, args.version, getattr(args, 'diagnostic', False),
                                     getattr(args, 'beta', False), pins, None if getattr(args, 'fields', None) else ship_fields)
    if ship_problems:
        engine['pins_refused'] = ship_problems + engine.get('pins_refused', [])
    for why in engine.get('pins_refused', []):   # (each refused pin: its hook is dropped, its feature off in a beta)
        print(f'[engine-map] pin refused, its hook stays dropped: {why}', file=sys.stderr)
    for why in engine.get('fields_refused', []):   # (each: the field stays unknown, its readers on the beta sentinel)
        print(f'[engine-map] field not applied, its evidence does not hold here: {why}', file=sys.stderr)
    n = version_number(args.version)
    inc = ROOT / 'engine' / 'runtime' / f'totk_main_v{n}.inc'
    js = ROOT / 'games' / 'totk' / 'patches' / f'v{n}.engine.json'
    inc.write_text(table_inc(addrs.items(), args.version), encoding='utf-8', newline='\n')
    if getattr(args, 'fields', None):   # a private flat map
        fmap = json.loads(Path(args.fields).read_text(encoding='utf-8'))
    else:                               # the shipped one: only the entries whose evidence holds on this image
        fmap = report.pop('fieldmap', {})
    ftext, funplaced = fields_inc(collect()['fields'], fmap, args.version, bool(engine.get('diagnostic') or engine.get('beta')))
    # (the fields this build knows: engine.ENGINE_DEFAULT features that need some, FIELDS_NEEDED, read them here)
    engine['fields_known'] = sorted(f'{st}+{o}' for st, offs in fmap.items() for o in offs)
    js.write_text(json.dumps(engine, indent=1) + '\n', encoding='utf-8', newline='\n')
    finc = ROOT / 'engine' / 'runtime' / f'totk_fields_v{n}.inc'
    finc.write_text(ftext, encoding='utf-8', newline='\n')
    stubs = ROOT / 'engine' / 'runtime' / f'totk_main_v{n}_stubs.inc'
    if engine.get('diagnostic') or engine.get('beta'):
        stubs.write_text(stubs_inc(engine['stubs'], args.version), encoding='utf-8', newline='\n')
    elif stubs.exists():
        stubs.unlink()
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(dict(status=engine['status'], safe_mode=engine['safe_mode'], wrote=[str(inc), str(js), str(finc)],
                          unplaced_fields=[f'{st}+{o}' for st, o in funplaced],
                          addresses=len(addrs), aliases=len(engine['alias']), hooks=len(engine['hook']),
                          dropped_hooks=engine['dropped_hooks'], unplaced_hooks=engine['unplaced_hooks'],
                          pinned=engine.get('pinned', {}), pins_refused=engine.get('pins_refused', []))))
    return 0 if engine['status'] in ('pass', 'diagnostic', 'beta') else 1
