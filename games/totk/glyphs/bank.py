"""The glyph bank: every PC button prompt (keyboard keys, mouse, Xbox, PlayStation) as glyphs the game can draw anywhere,
switched by the engine at run time to the inputs actually bound.

Where TotK draws button prompts:
  - message and UI text: the characters U+E040..U+E068 of Font/Font.Nin_NX_NVN.bfarc -> scft/External.bfotf (A B X Y L
    R ZL ZR, + -, the d-pad and both sticks with their directions; Mals/*/GuideKeyIcon.msbt names them). Menus, the map,
    shops, tutorials, loading tips, the pause menu and every action prompt in text use them.
  - the action cluster (blyt/DoCommand_00.bflyt in the Common layout archive): seven R8 masks Nt_KeyTex*, and the small
    letters of the other three face buttons, static 'A' / 'B' / 'X' / 'Y' text panes in the Normal font.

What this builds (one set, <out>/engine+glyphs/, the engine picks the glyphs when it serves the files):
  - Font/Font.Nin_NX_NVN.bfarc.zs: External.bfotf with the bank glyphs added (one glyph per key, mouse button, pad
    button; Kenney "Input Prompts" CC0 vectors, filled shape with the symbol cut out, like the game's own) at U+F100+i,
    and a cmap whose U+E040.. entries are all glyphIdArray slots: the engine rewrites those slots so U+E040 draws the key
    bound to A, and so on.
  - UI/LayoutArchive/Common...blarc.zs: the engine overlay's archive with DoCommand's small letters turned into U+E040..
    U+E043 in the External font (so they follow the slots too); the masks stay the game's and are rewritten by the engine
    from masks.bin (each bank glyph rasterised into each mask's box).
  - Both archives are zstd frames of raw blocks, so a byte of the archive sits at a known file offset (zstd_offset)
    and the engine patches it while serving the read.
  - glyphs.txt: the slots, the bank (name, glyph id, code point), the masks and where they are; masks.bin."""
import hashlib
import io
import re
import struct
import zlib
from pathlib import Path

import numpy as np
import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.svgLib.path import parse_path
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from PIL import Image, ImageDraw, ImageFont

from games.totk.overlays import COMMON_ARCHIVE, sarc_members
from games.totk.overlays.engine_dir import RSTB, rstb_entries, rstb_key
from . import BNTX_MEMBER, R8_UNORM, bbox, bntx_textures, read_r8

FONT_ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'
EXTERNAL = 'scft/External.bfotf'
BANK_BASE = 0xF100
GAME_BASE = 0xF000              # U+E0xx's own glyph also at U+F0xx (the live switch back to a Nintendo pad)
BFOTF_MAGIC = 0x7F9A0218        # the first word of a decrypted bfotf header; the XOR key is word0 ^ this
ZBLOCK = 128 * 1024
ZHEADER = 13                    # magic, descriptor (single segment, 8-byte content size), content size

# The game's button code points (GuideKeyIcon.msbt) -> the control they stand for (the engine's names).
SLOTS = {
    0xE040: 'A', 0xE041: 'B', 0xE042: 'X', 0xE043: 'Y', 0xE044: 'L', 0xE045: 'R', 0xE046: 'ZL', 0xE047: 'ZR',
    0xE049: 'DPAD', 0xE04B: 'PLUS', 0xE04C: 'MINUS',
    0xE050: 'LS_DOWN', 0xE051: 'RS_DOWN', 0xE052: 'LS_UP', 0xE053: 'RS_UP', 0xE054: 'LS_LEFTRIGHT', 0xE055: 'RS_LEFTRIGHT',
    0xE056: 'LS_PUSH', 0xE057: 'RS_PUSH', 0xE058: 'LS_RIGHT', 0xE059: 'RS_RIGHT', 0xE05A: 'LS_LEFT', 0xE05B: 'RS_LEFT',
    0xE05C: 'LS_UPDOWN', 0xE05D: 'RS_UPDOWN', 0xE05E: 'LS', 0xE05F: 'RS',
    0xE060: 'DPAD_UP', 0xE061: 'DPAD_DOWN', 0xE062: 'DPAD_LEFT', 0xE063: 'DPAD_RIGHT', 0xE064: 'DPAD_UPDOWN',
    0xE065: 'DPAD_LEFTRIGHT', 0xE068: 'RS_SHAKE',
}
# DoCommand's masks -> the control each one shows (LSt_00: the stick tilted, LSt_01: the stick pressed).
MASKS = {'Nt_KeyTexA_00^d': 'A', 'Nt_KeyTexB_00^d': 'B', 'Nt_KeyTexX_00^d': 'X', 'Nt_KeyTexY_00^d': 'Y',
         'Nt_KeyTexL_00^d': 'L', 'Nt_KeyTexZL_00^d': 'ZL', 'Nt_KeyTexLSt_00^d': 'LS', 'Nt_KeyTexLSt_01^d': 'LS_PUSH'}
# The d-pad picture of the item shortcuts (PaShortCutGuide_00, top left): BC4, 64x64, the picture and its blurred shadow.
BC4_MASKS = {'ButtonIconL46NN_00^s': ('DPAD', False), 'ButtonIconL46NN_01^s': ('DPAD', True)}
BC4_UNORM = 0x1D01

KM, XB, PS = 'Keyboard & Mouse/Vector/', 'Xbox Series/Vector/', 'PlayStation Series/Vector/'
_KEYS = ([c for c in 'abcdefghijklmnopqrstuvwxyz0123456789'] + [f'f{i}' for i in range(1, 13)] +
         'arrow_left arrow_up arrow_right arrow_down minus equals plus comma period slash_forward slash_back semicolon '
         'apostrophe quote bracket_open bracket_close tilde win'.split())
# Named keys: Kenney writes their names small inside a square key, unreadable at text size. In text they are a key at
# most 1.2 keys wide with the name cut out in the game's own bold face (Rodin B), or Kenney's symbol key (ICONS): the
# game's guides right-align each prompt in a box sized for a square button, and a wider glyph ran into the text before
# it. The action cluster's masks have room: there the name is written out at full size (MASK_BASE glyphs, used for
# masks.bin only).
ICONS = {'shift': 'keyboard_shift_icon', 'enter': 'keyboard_return',   # (Space's symbol key was unreadable small)
         'backspace': 'keyboard_backspace_icon', 'tab': 'keyboard_tab_icon', 'capslock': 'keyboard_capslock_icon'}
MASK_BASE = 0xF400
WIDE = {'space': 'SPACE', 'enter': 'ENTER', 'escape': 'ESC', 'tab': 'TAB', 'backspace': 'BKSP', 'shift': 'SHIFT',
        'ctrl': 'CTRL', 'alt': 'ALT', 'page_up': 'PG UP', 'page_down': 'PG DN', 'home': 'HOME', 'end': 'END',
        'insert': 'INS', 'delete': 'DEL', 'capslock': 'CAPS', 'numlock': 'NUM', 'printscreen': 'PRT SC',
        'pause': 'PAUSE', 'scroll_lock': 'SCR LK'}
LABEL_FONT = 'scft/ninP_RodinNTLG-B_DH_003_subset.bfotf'
_MOUSE = ('mouse_left_outline mouse_right_outline mouse_scroll_outline mouse_side_back_outline mouse_side_forward_outline '
          'mouse_scroll_up_outline mouse_scroll_down_outline mouse_scroll_vertical_outline mouse_move mouse_horizontal '
          'mouse_vertical').split()
_XBOX = ('xbox_button_a xbox_button_b xbox_button_x xbox_button_y xbox_lb xbox_rb xbox_lt xbox_rt xbox_button_view '
         'xbox_button_menu xbox_button_share xbox_dpad xbox_dpad_up_outline xbox_dpad_down_outline xbox_dpad_left_outline '
         'xbox_dpad_right_outline xbox_dpad_vertical_outline xbox_dpad_horizontal_outline').split() + [
    f'xbox_stick_{s}{d}' for s in 'lr' for d in ('', '_up', '_down', '_left', '_right', '_vertical', '_horizontal', '_press')]
_PS = ('playstation_button_cross playstation_button_circle playstation_button_square playstation_button_triangle '
       'playstation_trigger_l1 playstation_trigger_r1 playstation_trigger_l2 playstation_trigger_r2 '
       'playstation5_button_create playstation5_button_options playstation5_touchpad playstation_dpad '
       'playstation_dpad_up_outline playstation_dpad_down_outline playstation_dpad_left_outline '
       'playstation_dpad_right_outline playstation_dpad_vertical_outline playstation_dpad_horizontal_outline').split() + [
    f'playstation_stick_{s}{d}' for s in 'lr' for d in ('', '_up', '_down', '_left', '_right', '_vertical', '_horizontal', '_press')]
# Keys drawn side by side at half size (the keyboard's stick: W A S D and its two axes; any keys bound to the stick
# that have no composite show the key of the direction asked, or the up key for the whole stick).
COMPOSITES = {
    'kbm_wasd': [('w', 1, 0), ('a', 0, 1), ('s', 1, 1), ('d', 2, 1)],
    'kbm_ws': [('w', 0, 0), ('s', 0, 1)],
    'kbm_ad': [('a', 0, 0), ('d', 1, 0)],
    'kbm_arrows': [('arrow_up', 1, 0), ('arrow_left', 0, 1), ('arrow_down', 1, 1), ('arrow_right', 2, 1)],
    'kbm_arrows_ud': [('arrow_up', 0, 0), ('arrow_down', 0, 1)],
    'kbm_arrows_lr': [('arrow_left', 0, 0), ('arrow_right', 1, 0)],
    # the d-pad's keys (up on top; left, down, right below): the pc preset's and the classic one's
    'kbm_dpad_pc': [('1', 1, 0), ('2', 0, 1), ('x', 1, 1), ('3', 2, 1)],   # (down X: one key per button, 4.22)
    'kbm_dpad_classic': [('2', 1, 0), ('1', 0, 1), ('4', 1, 1), ('3', 2, 1)],
}
# Added after the pad glyphs, so the earlier glyphs keep their code points (U+F100 + the bank index):
COMPOSITES_2 = {
    # the PC scheme's own d-pad keys on foot (pc_scheme_gameplay.inc: C materials up, 2 shields left, X whistle down,
    # 1 weapons right) and every set's up-down / left-right pairs (a prompt on half the d-pad: kbm_glyph matches them)
    'kbm_dpad_scheme': [('c', 1, 0), ('2', 0, 1), ('x', 1, 1), ('1', 2, 1)],
    'kbm_dpad_scheme_ud': [('c', 0, 0), ('x', 0, 1)],
    'kbm_dpad_scheme_lr': [('2', 0, 0), ('1', 1, 0)],
    'kbm_dpad_pc_ud': [('1', 0, 0), ('x', 0, 1)],
    'kbm_dpad_pc_lr': [('2', 0, 0), ('3', 1, 0)],
    'kbm_dpad_classic_ud': [('2', 0, 0), ('4', 0, 1)],
    'kbm_dpad_classic_lr': [('1', 0, 0), ('3', 1, 0)],
}
# One key cap for a pair of wide keys (two half-size labels would not read): the map's Switch, PageUp / PageDown
PAIR_LABELS = {'kbm_pgupdn': 'PGUP/DN'}
# Every bindable key draws its own glyph after a rebind: the numpad's * (/ and. reuse the main keys'), the ISO key
# left of Z (<>), a generic key for anything else, and labels for Menu and F13..F24. Last, so every earlier glyph
# keeps its code point
EXTRA_KEYS = ['asterisk', 'bracket_less', 'any']
EXTRA_LABELS = {'menu': 'MENU', **{f'f{i}': f'F{i}' for i in range(13, 25)}}


def bank_names():
    """The bank in code point order: (name, source); source is an SVG path under the Kenney folder or a composite."""
    out = [(f'keyboard_{k}', KM + f'keyboard_{k}.svg') for k in _KEYS]
    out += [(f'keyboard_{k}', KM + ICONS[k] + '.svg' if k in ICONS else ('label', text, True)) for k, text in WIDE.items()]
    out += [(m.replace('_outline', ''), KM + m + '.svg') for m in _MOUSE]
    out += [(n, COMPOSITES[n]) for n in COMPOSITES]
    out += [(x.replace('_outline', ''), XB + x + '.svg') for x in _XBOX]
    out += [(p.replace('_outline', ''), PS + p + '.svg') for p in _PS]
    out += [(n, COMPOSITES_2[n]) for n in COMPOSITES_2]
    out += [(n, ('label', text, True)) for n, text in PAIR_LABELS.items()]
    out += [(f'keyboard_{k}', KM + f'keyboard_{k}.svg') for k in EXTRA_KEYS]
    out += [(f'keyboard_{k}', ('label', text, True)) for k, text in EXTRA_LABELS.items()]
    return out


# ------------------------------------------------------------------------------------------------------- outlines

def svg_path(path):
    """Union of every filled <path> of a Kenney SVG (64x64 canvas, y down) as one pathops path: the shape with its
    symbol cut out (the outline variants: the device outlined, the button meant filled)."""
    text = Path(path).read_text(encoding='utf-8')
    if 'transform=' in text:
        raise ValueError(f'{path}: transforms are not handled')
    out = pathops.Path()
    for d in re.findall(r'<path[^>]*\sd="([^"]+)"', text):
        p = pathops.Path()
        parse_path(d, p.getPen())
        p.simplify(fix_winding=True)
        out = pathops.op(out, p, pathops.PathOp.UNION)
    if not list(out.contours):
        raise ValueError(f'{path}: no path')
    return out


def composite_path(art, parts):
    """Keys at half size on a grid (column, row), 26 units apart."""
    out = pathops.Path()
    for key, col, row in parts:
        src = svg_path(Path(art) / (KM + f'keyboard_{key}.svg'))
        moved = pathops.Path()
        src.draw(TransformPen(moved.getPen(), (0.5, 0, 0, 0.5, col * 26, row * 26)))
        out = pathops.op(out, moved, pathops.PathOp.UNION)
    return out


def label_path(label_font, text, compact=False):
    """A key 48 units tall (Kenney's y 8..56), as wide as its name plus 9 units a side, corners of 8, the name (22 units
    of cap height) cut out; compact: 18 units of cap height, the name narrowed so the key is at most 58 units wide."""
    gs, cmap = label_font.getGlyphSet(), label_font.getBestCmap()
    k = (18 if compact else 22) / 700.0   # Rodin's cap height is ~700 of 1000
    name = pathops.Path()
    x = 0.0
    for ch in text:
        g = gs[cmap[ord(ch)]]
        if ch != ' ':
            part = pathops.Path()
            g.draw(TransformPen(part.getPen(), (k, 0, 0, -k, x, 32 + 11)))
            name = pathops.op(name, part, pathops.PathOp.UNION)
        x += g.width * k * (0.6 if ch == ' ' else 1.0)
    x0, _, x1, _ = name.bounds
    if compact and x1 - x0 > 46:
        narrowed = pathops.Path()
        name.draw(TransformPen(narrowed.getPen(), (46 / (x1 - x0), 0, 0, 1, 0, 0)))
        name = narrowed
        x0, _, x1, _ = name.bounds
    w = max(48.0, (x1 - x0) + (12 if compact else 18))
    key = pathops.Path()
    pen = key.getPen()
    r, l, t, b = 8, 8, 8, 56
    rr = l + w
    pen.moveTo((l + r, t)); pen.lineTo((rr - r, t)); pen.qCurveTo((rr, t), (rr, t + r)); pen.lineTo((rr, b - r))
    pen.qCurveTo((rr, b), (rr - r, b)); pen.lineTo((l + r, b)); pen.qCurveTo((l, b), (l, b - r)); pen.lineTo((l, t + r))
    pen.qCurveTo((l, t), (l + r, t)); pen.closePath()
    moved = pathops.Path()
    name.draw(TransformPen(moved.getPen(), (1, 0, 0, 1, l + (w - (x1 - x0)) / 2 - x0, 0)))
    return pathops.op(key, moved, pathops.PathOp.DIFFERENCE)


def bounds(path):
    return path.bounds   # (xmin, ymin, xmax, ymax)


# ---------------------------------------------------------------------------------------------------------- font

def decrypt_bfotf(data):
    key = struct.unpack_from('>I', data, 0)[0] ^ BFOTF_MAGIC
    size = struct.unpack_from('>I', data, 4)[0] ^ key
    words = np.frombuffer(bytes(data[8:8 + (len(data) - 8) // 4 * 4]), '>u4') ^ np.uint32(key)
    return words.astype('>u4').tobytes()[:size], key


def encrypt_bfotf(otf, key):
    body = otf + b'\0' * (-len(otf) % 4)
    words = np.frombuffer(body, '>u4') ^ np.uint32(key)
    return struct.pack('>II', BFOTF_MAGIC ^ key, len(otf) ^ key) + words.astype('>u4').tobytes()


def cmap_format4(mapping):
    """A cmap (format 4 under (0, 3) and (3, 1)) where every code point has its own glyphIdArray slot (idRangeOffset,
    never idDelta), so the engine can point one at another glyph. Returns (table bytes, {code point: slot offset})."""
    segs = []
    for cp in sorted(mapping):
        if segs and segs[-1][1] == cp - 1:
            segs[-1][1] = cp
        else:
            segs.append([cp, cp])
    segs.append([0xFFFF, 0xFFFF])
    n = len(segs)
    ends, starts, deltas, ranges, gia, slot_index = [], [], [], [], [], {}
    for i, (s, e) in enumerate(segs):
        ends.append(e); starts.append(s)
        if s == 0xFFFF:
            deltas.append(1); ranges.append(0)
            continue
        deltas.append(0); ranges.append(2 * (n - i) + 2 * len(gia))
        for cp in range(s, e + 1):
            slot_index[cp] = len(gia)
            gia.append(mapping[cp])
    sr = 2 * (1 << (n.bit_length() - 1))
    sub = struct.pack('>HHHHHHH', 4, 0, 0, 2 * n, sr, (n.bit_length() - 1), 2 * n - sr)
    sub += struct.pack(f'>{n}H', *ends) + b'\0\0' + struct.pack(f'>{n}H', *starts)
    sub += struct.pack(f'>{n}h', *[d if d < 0x8000 else d - 0x10000 for d in deltas]) + struct.pack(f'>{n}H', *ranges)
    sub += struct.pack(f'>{len(gia)}H', *gia)
    if len(sub) > 0xFFFF:
        raise ValueError('cmap format 4 subtable too large')
    sub = sub[:2] + struct.pack('>H', len(sub)) + sub[4:]
    table = struct.pack('>HH', 0, 2) + struct.pack('>HHI', 0, 3, 20) + struct.pack('>HHI', 3, 1, 20) + sub
    gia_at = 20 + 16 + 8 * n
    return table, {cp: gia_at + 2 * k for cp, k in slot_index.items()}


def table_offset(otf, tag):
    num = struct.unpack_from('>H', otf, 4)[0]
    for i in range(num):
        t, _, off, length = struct.unpack_from('>4sIII', otf, 12 + 16 * i)
        if t == tag.encode():
            return off, length
    raise KeyError(tag)


def build_font(external_otf, art, label_otf):
    """External with the bank. Returns (otf, {code point: slot offset in otf}, {cp: original glyph id},
    [(name, glyph id, code point)], {bank name: code point of its mask variant})."""
    src = TTFont(io.BytesIO(external_otf))
    gs = src.getGlyphSet()
    order = src.getGlyphOrder()
    cmap = src.getBestCmap()
    upm = src['head'].unitsPerEm
    charstrings, metrics = {}, {}
    for g in order:
        pen = T2CharStringPen(gs[g].width, gs)
        gs[g].draw(pen)
        charstrings[g] = pen.getCharString()
        metrics[g] = (gs[g].width, src['hmtx'][g][1])
    # scale and place: a Kenney key (48 units tall) as tall as the game's A (583 - 19), from its top
    s = (583 - 19) / 48.0
    side = 38

    def add(g, p, top):
        x0, y0, x1, y1 = bounds(p)
        pen = T2CharStringPen(round((x1 - x0) * s + 2 * side), None)
        p.draw(TransformPen(pen, (s, 0, 0, -s, side - x0 * s, 583 + min(top, y0) * s)))
        charstrings[g] = pen.getCharString()
        metrics[g] = (round((x1 - x0) * s + 2 * side), side)
        order.append(g)
        return len(order) - 1

    label_font = TTFont(io.BytesIO(label_otf))
    bank = []
    for i, (name, source) in enumerate(bank_names()):
        if isinstance(source, tuple):
            p = label_path(label_font, source[1], source[2])
        elif isinstance(source, list):
            p = composite_path(art, source)
        else:
            p = svg_path(Path(art) / source)
        bank.append((name, add(f'nxr_{name}', p, 8 if not isinstance(source, list) else 99), BANK_BASE + i))
    # the named keys written out at full size, for the masks only (no code point the game is given)
    mask_cps = {}
    for i, (key, text) in enumerate(WIDE.items()):
        add(f'nxrw_keyboard_{key}', label_path(label_font, text), 8)
        mask_cps[f'keyboard_{key}'] = MASK_BASE + i
    fb = FontBuilder(upm, isTTF=False)
    fb.setupGlyphOrder(order)
    mapping = {cp: g for cp, g in cmap.items() if cp <= 0xFFFF}
    mapping.update({cp: f'nxr_{n}' for n, _, cp in bank})
    mapping.update({cp - 0xE000 + GAME_BASE: cmap[cp] for cp in SLOTS})   # the game's own, at code points no slot moves
    mapping.update({cp: f'nxrw_{n}' for n, cp in mask_cps.items()})
    fb.setupCharacterMap(mapping)
    fb.setupCFF('NxrExternal', {'FullName': 'NxrExternal'}, charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=src['hhea'].ascent, descent=src['hhea'].descent)
    fb.setupNameTable({'familyName': 'NxrExternal', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=src['OS/2'].sTypoAscender, sTypoDescender=src['OS/2'].sTypoDescender,
                usWinAscent=src['OS/2'].usWinAscent, usWinDescent=src['OS/2'].usWinDescent)
    fb.setupPost()
    gid = {g: i for i, g in enumerate(order)}
    raw, slots = cmap_format4({cp: gid[g] for cp, g in mapping.items()})
    t = DefaultTable('cmap')
    t.data = raw
    fb.font['cmap'] = t
    # the source font's dates, not now: the same inputs make the same bank byte for byte (tools/builder: a build on
    # another PC can be compared with the release's)
    fb.font['head'].created, fb.font['head'].modified = src['head'].created, src['head'].modified
    fb.font.recalcTimestamp = False
    out = io.BytesIO()
    fb.font.save(out)
    otf = out.getvalue()
    coff, clen = table_offset(otf, 'cmap')
    if otf[coff:coff + clen] != raw:
        raise AssertionError('cmap not written as built')
    orig = {cp: gid[cmap[cp]] for cp in SLOTS}
    return otf, {cp: coff + off for cp, off in slots.items() if cp in SLOTS}, orig, bank, mask_cps


# ---------------------------------------------------------------------------------------------------- containers

def sarc_replace(sarc, member, data, align=0x1000):
    """The SARC with member's data appended at the end (aligned) and its entry pointed there; the old bytes stay."""
    sarc = bytearray(sarc)
    header, size, start = struct.unpack_from('<HxxII', sarc, 4)
    count = struct.unpack_from('<H', sarc, header + 6)[0]
    names = header + 12 + count * 16 + 8
    for i in range(count):
        e = header + 12 + i * 16
        attribute, lo, hi = struct.unpack_from('<3I', sarc, e + 4)
        off = names + (attribute & 0xffffff) * 4
        if bytes(sarc[off:sarc.index(0, off)]).decode() == member:
            pad = -len(sarc) % align
            new_lo = len(sarc) + pad - start
            sarc += b'\0' * pad + data
            struct.pack_into('<II', sarc, e + 8, new_lo, new_lo + len(data))
            struct.pack_into('<I', sarc, 8, len(sarc))
            return bytes(sarc)
    raise KeyError(member)


def raw_zstd(data):
    """A zstd frame of raw blocks (no dictionary, no checksum): byte i of data is at zstd_offset(i) of the file."""
    out = bytearray(struct.pack('<IB', 0xFD2FB528, 0xE0) + struct.pack('<Q', len(data)))
    for i in range(0, max(len(data), 1), ZBLOCK):
        chunk = data[i:i + ZBLOCK]
        last = i + ZBLOCK >= len(data)
        h = (len(chunk) << 3) | int(last)
        out += struct.pack('<I', h)[:3] + chunk
    return bytes(out)


def zstd_offset(i):
    return ZHEADER + (i // ZBLOCK) * (ZBLOCK + 3) + 3 + i % ZBLOCK


# ------------------------------------------------------------------------------------------------------- layouts

def bflyt_sections(b):
    off = struct.unpack_from('<H', b, 6)[0]
    for _ in range(struct.unpack_from('<H', b, 0x10)[0]):
        magic, size = bytes(b[off:off + 4]), struct.unpack_from('<I', b, off + 4)[0]
        yield off, magic, size
        off += size


# DoCommand's small face-button diamond (P_Base*: the diamond, T_<letter>_NN: the other three buttons' letters) in one copy
# per style, each copy a pane of its own (name + suffix); the engine shows the copy of the style in use and hides the rest,
# at serve time and again in memory on a live switch (the font engine keeps the glyph it drew for a code point, so one pane
# cannot change its glyph live). Letters: '@s' the game's own (Normal font), '@x' / '@p' a bank glyph in External_00.fcpx
# (the engine writes the code point of the button bound); the diamond '@b' shows with any pad style and with none for keys
# and mouse (the key masks say it all). The original panes stay, hidden.
LETTER_GROUPS = ('@s', '@x', '@p')
BASE_GROUP = '@b'
PANE_HEAD = 0x54          # a pane section's common part: header, flags, name, user data, translate .. size


def _pane_record(b, off, group, control, text_at):
    """(name, group, control, flag offset, text offset or 0, flag, alpha, translate..size as hex) of the pane at off."""
    name = bytes(b[off + 12:off + 36]).split(b'\0')[0].decode()
    return (name, group, control, off + 8, text_at, b[off + 8], b[off + 10], bytes(b[off + 44:off + 84]).hex())


def docommand_letters(bflyt):
    """DoCommand_00 with its diamond in one copy per style (LETTER_GROUPS, BASE_GROUP; External_00.fcpx added to the
    font list for the bank glyphs). Returns (bflyt, the copies: _pane_record tuples, offsets in the returned bflyt)."""
    b = bytearray(bflyt)
    fonts_at = None
    for off, magic, size in bflyt_sections(b):
        if magic == b'fnl1':
            fonts_at = (off, size)
    off, size = fonts_at
    n = struct.unpack_from('<H', b, off + 8)[0]
    offs = struct.unpack_from(f'<{n}I', b, off + 12)
    names = [bytes(b[off + 12 + o:b.index(0, off + 12 + o)]).decode() for o in offs]
    if 'External_00.fcpx' in names:
        raise ValueError('DoCommand already lists External_00.fcpx')
    names.append('External_00.fcpx')
    ext = len(names) - 1
    table = b''
    strs = b''
    for nm in names:
        table += struct.pack('<I', 4 * len(names) + len(strs))
        strs += nm.encode() + b'\0'
    body = struct.pack('<HH', len(names), 0) + table + strs
    body += b'\0' * (-(8 + len(body)) % 4)
    sec = b'fnl1' + struct.pack('<I', 8 + len(body)) + body
    b[off:off + size] = sec
    letters = bases = 0
    out = bytearray(b[:struct.unpack_from('<H', b, 6)[0]])
    sections = list(bflyt_sections(b))
    count = len(sections)
    i = 0
    while i < len(sections):
        off, magic, size = sections[i]
        j = i + 1
        while j < len(sections) and sections[j][1] == b'usd1':   # (a pane's user data follows it)
            j += 1
        own = b''.join(bytes(b[o:o + s]) for o, _, s in sections[i:j])
        name = bytes(b[off + 12:off + 36]).split(b'\0')[0].decode() if magic in (b'txt1', b'pic1') else ''
        letter = re.fullmatch(r'T_([ABXY])_\d+', name) if magic == b'txt1' else None
        base = magic == b'pic1' and re.fullmatch(r'P_Base[ABXY]_\d+', name)
        if not letter and not base:
            out += own
            i = j
            continue
        if letter:
            text = off + struct.unpack_from('<I', b, off + PANE_HEAD + 0x10)[0]
            if bytes(b[text:text + 4]) != letter[1].encode('utf-16-le') + b'\0\0':
                raise ValueError(f'{name}: text is not "{letter[1]}"')
        hidden = bytearray(own)
        hidden[8] &= 0xFE
        out += hidden
        for group in (LETTER_GROUPS if letter else (BASE_GROUP,)):
            c = bytearray(hidden)
            if len(name + group) > 23:
                raise ValueError(f'{name}{group}: pane name too long')
            c[12:36] = (name + group).encode().ljust(24, b'\0')
            if letter and group != '@s':
                struct.pack_into('<H', c, PANE_HEAD + 6, ext)                       # External_00.fcpx
                struct.pack_into('<H', c, text - off, GAME_BASE + 0x40 + 'ABXY'.index(letter[1]))   # (the engine's)
            out += c
            count += j - i
        letters += bool(letter)
        bases += bool(base)
        i = j
    struct.pack_into('<H', out, 0x10, count)
    struct.pack_into('<I', out, 0xc, len(out))
    if letters < 12 or bases < 4:
        raise ValueError(f'DoCommand: {letters} letter panes, {bases} diamonds')
    records = []
    for off, magic, size in bflyt_sections(out):
        name = bytes(out[off + 12:off + 36]).split(b'\0')[0].decode() if magic in (b'txt1', b'pic1') else ''
        if '@' not in name:
            continue
        group = name[name.index('@'):]
        control = name[2] if magic == b'txt1' else '-'
        text = off + struct.unpack_from('<I', out, off + PANE_HEAD + 0x10)[0] if magic == b'txt1' and group != '@s' else 0
        records.append(_pane_record(out, off, group, control, text))
    return bytes(out), records


# --------------------------------------------------------------------------------------------------------- masks

def render_glyph(font, cp, size=288):
    im = Image.new('L', (size * 3, size * 2), 0)
    ImageDraw.Draw(im).text((size // 4, size // 4), chr(cp), font=font, fill=255)
    a = np.asarray(im)
    x0, y0, x1, y1 = bbox(a, 8)
    return Image.fromarray(np.ascontiguousarray(a[y0:y1, x0:x1]))


def bc4_decode(data, t):
    from . import bl_offsets
    bw, bh = t['w'] // 4, t['h'] // 4
    offs = bl_offsets(bw, bh, 8, t['block_h'])
    img = np.zeros((t['h'], t['w']), np.uint8)
    for y in range(bh):
        for x in range(bw):
            o = t['start'] + int(offs[y, x])
            r0, r1 = data[o], data[o + 1]
            bits = int.from_bytes(bytes(data[o + 2:o + 8]), 'little')
            if r0 > r1:
                pal = [r0, r1] + [((6 - i) * r0 + (1 + i) * r1) // 7 for i in range(6)]
            else:
                pal = [r0, r1] + [((4 - i) * r0 + (1 + i) * r1) // 5 for i in range(4)] + [0, 255]
            img[y * 4:y * 4 + 4, x * 4:x * 4 + 4] = np.array([pal[(bits >> (3 * k)) & 7] for k in range(16)]).reshape(4, 4)
    return img


def bc4_encode(img):
    """BC4 blocks in row-major block order (8 bytes each; the 8-level mode, ends = the block's max and min)."""
    h, w = img.shape
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            v = img[by:by + 4, bx:bx + 4].astype(np.int32).reshape(16)
            r0, r1 = int(v.max()), int(v.min())
            if r0 == r1:
                out += bytes([r0, r1]) + bytes(6)
                continue
            pal = np.array([r0, r1] + [((6 - i) * r0 + (1 + i) * r1) // 7 for i in range(6)])
            idx = np.abs(v[:, None] - pal[None, :]).argmin(1)
            bits = 0
            for k, i in enumerate(idx):
                bits |= int(i) << (3 * k)
            out += bytes([r0, r1]) + bits.to_bytes(6, 'little')
    return bytes(out)


def fit(icon, box, w, h):
    bx0, by0, bx1, by1 = box
    bw, bh = bx1 - bx0, by1 - by0
    k = min(bw / icon.width, bh / icon.height)
    iw, ih = max(1, round(icon.width * k)), max(1, round(icon.height * k))
    out = Image.new('L', (w, h), 0)
    out.paste(icon.resize((iw, ih), Image.LANCZOS), (bx0 + (bw - iw) // 2, by0 + (bh - ih) // 2))
    return np.asarray(out, np.uint8)


# ------------------------------------------------------------------------------------------------- GuideKeyIcon

GUIDE_KEY_ICON = 'StaticMsg/GuideKeyIcon.msbt'
_DPAD_LABELS = {'Up': 'DPAD_UP', 'Down': 'DPAD_DOWN', 'Left': 'DPAD_LEFT', 'Right': 'DPAD_RIGHT', 'UpDown': 'DPAD_UPDOWN',
                'LeftRight': 'DPAD_LEFTRIGHT', 'UpDownLeftRight': 'DPAD'}


def guide_key_icon(romfs, zstd, lang='USen'):
    """GuideKeyIcon.msbt (the same in every language: tools/engine/check_texts.py compares the 14): its TXT2 section's
    body (the in-memory signature) and, per entry that is one button character, (byte offset of the character in that
    body, control). The game turns a message's button tag into the character of the entry, so rewriting these in
    memory switches every prompt shown after. `lang`: the Mals archive read (the bank's is USen)."""
    data, _ = zstd.decompress(romfs.read(f'Mals/{lang}.Product.100.sarc.zs'))
    data = bytes(data)
    a, e = sarc_members(data)[GUIDE_KEY_ICON]
    b = data[a:e]
    if b[:8] != b'MsgStdBn':
        raise ValueError(f'{GUIDE_KEY_ICON}: not an MSBT')
    labels, body = {}, None
    off = 0x20
    for _ in range(struct.unpack_from('<H', b, 0xe)[0]):
        magic, size = b[off:off + 4], struct.unpack_from('<I', b, off + 4)[0]
        s = off + 0x10
        if magic == b'LBL1':
            for i in range(struct.unpack_from('<I', b, s)[0]):
                cnt, o = struct.unpack_from('<II', b, s + 4 + 8 * i)
                p = s + o
                for _ in range(cnt):
                    n = b[p]
                    labels[struct.unpack_from('<I', b, p + 1 + n)[0]] = b[p + 1:p + 1 + n].decode()
                    p += 1 + n + 4
        elif magic == b'TXT2':
            body = b[s:s + size]
        off += (size + 0x10 + 15) // 16 * 16
    n = struct.unpack_from('<I', body, 0)[0]
    entries = []
    for i in range(n):
        o = struct.unpack_from('<I', body, 4 + 4 * i)[0]
        ch = struct.unpack_from('<H', body, o)[0]
        if ch not in SLOTS or struct.unpack_from('<H', body, o + 2)[0] != 0:
            continue
        label = labels[i]
        control = _DPAD_LABELS.get(label) or label.upper()
        if control != SLOTS[ch]:
            raise ValueError(f'{GUIDE_KEY_ICON}: {label} is U+{ch:04X} ({SLOTS[ch]})')
        entries.append((o, control))
    if len(entries) < 30:
        raise ValueError(f'{GUIDE_KEY_ICON}: only {len(entries)} button entries')
    return body, entries


# ----------------------------------------------------------------------------------------------------------- set

def raise_rstb(zstd, rstb_zs, grow):
    """The resource size table with grow[path] bytes more for each path: the game sizes a resource's buffer from it, and
    both archives here are larger than the game's (a font archive 55 KB larger overran its buffer at boot)."""
    d, dict_id = zstd.decompress(rstb_zs)
    d = bytearray(d)
    entries = rstb_entries(d)
    for path, extra in grow.items():
        k = rstb_key(path)
        at = entries.get(zlib.crc32(k.encode())) or entries.get(k)
        if at is None:
            raise KeyError(f'{k}: not in the resource size table')
        struct.pack_into('<I', d, at, struct.unpack_from('<I', d, at)[0] + max(0, extra))
    return zstd.compress(bytes(d), dict_id)


def fnv64(data):
    h = 0xcbf29ce484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def build_bank(romfs, zstd, common_zs, art, out_dir, preview=None, rstb_zs=None):
    """Writes <out_dir>/ (the engine+glyphs set). common_zs: the engine overlay's Common archive (zstd) it builds on;
    rstb_zs: the engine overlay's resource size table (default: the game's), served raised for the two archives."""
    out_dir = Path(out_dir)
    fonts, _ = zstd.decompress(romfs.read(FONT_ARCHIVE))
    fonts = bytes(fonts)
    lo, hi = sarc_members(fonts)[EXTERNAL]
    external, key = decrypt_bfotf(fonts[lo:hi])
    llo, lhi = sarc_members(fonts)[LABEL_FONT]
    otf, slots, orig, bank, mask_cps = build_font(external, art, decrypt_bfotf(fonts[llo:lhi])[0])
    bfotf = encrypt_bfotf(otf, key)
    fonts2 = sarc_replace(fonts, EXTERNAL, bfotf)
    new_lo = sarc_members(fonts2)[EXTERNAL][0]
    if decrypt_bfotf(fonts2[new_lo:new_lo + len(bfotf)])[0] != otf:
        raise AssertionError('bfotf round trip')
    kb = struct.pack('>I', key)

    blarc, _ = zstd.decompress(common_zs)
    blarc = bytes(blarc)
    members = sarc_members(blarc)
    a, e = members['blyt/DoCommand_00.bflyt']
    lay, letters = docommand_letters(blarc[a:e])
    blarc2 = sarc_replace(blarc, 'blyt/DoCommand_00.bflyt', lay, 0x100)
    lay_at = sarc_members(blarc2)['blyt/DoCommand_00.bflyt'][0]
    blo, bhi = sarc_members(blarc2)[BNTX_MEMBER]
    textures = bntx_textures(blarc2[blo:bhi])

    font_img = ImageFont.truetype(io.BytesIO(otf), 256)
    groups, mask_rows = [], []
    for tex, control in MASKS.items():
        t = textures[tex]
        if t['fmt'] != R8_UNORM or t['levels'] != 1 or t['tile_mode'] != 0 or (t['w'], t['h']) != (72, 72):
            raise ValueError(f'{tex}: not the 72x72 R8 block-linear mask expected')
        box = tuple(int(v) for v in bbox(read_r8(blarc2[blo:bhi], t)))
        if box not in groups:
            groups.append(box)
        mask_rows.append((tex, control, blo + t['start'], t['w'], t['h'], t['block_h'], groups.index(box)))
    bc4_rows, bc4 = [], []
    from PIL import ImageFilter
    for j, (tex, (control, shadow)) in enumerate(BC4_MASKS.items()):
        t = textures[tex]
        if t['fmt'] != BC4_UNORM or t['levels'] != 1 or t['tile_mode'] != 0 or t['w'] % 4 or t['h'] % 4:
            raise ValueError(f'{tex}: not the 1-level BC4 block-linear texture expected')
        own = bc4_decode(blarc2[blo:bhi], t)
        bc4_rows.append((tex, control, blo + t['start'], t['w'], t['h'], t['block_h'], j))
        bc4.append((t, own, shadow))
    masks = np.zeros((len(groups), len(bank), 72, 72), np.uint8)
    for j, (name, _, cp) in enumerate(bank):
        icon = render_glyph(font_img, mask_cps.get(name, cp))
        for g, box in enumerate(groups):
            masks[g, j] = fit(icon, box, 72, 72)
    # the d-pad picture: each glyph in the picture's box; its shadow the same, widened and blurred to the game's peak
    pic = next(own for _, own, shadow in bc4 if not shadow)
    box = tuple(int(v) for v in bbox(pic, 64))
    bc4_bank = bytearray()
    for _, own, shadow in bc4:
        peak = int(own.max())
        for j, (name, _, cp) in enumerate(bank):
            img = fit(render_glyph(font_img, cp), box, own.shape[1], own.shape[0])
            if shadow:
                im = Image.fromarray(img).filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(3))
                a = np.asarray(im, np.float32)
                img = np.clip(a * (peak / max(1.0, a.max())), 0, 255).astype(np.uint8)
            bc4_bank += bc4_encode(img)

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / 'Font').mkdir(exist_ok=True)
    (out_dir / 'UI/LayoutArchive').mkdir(parents=True, exist_ok=True)
    (out_dir / 'System/Resource').mkdir(parents=True, exist_ok=True)
    rstb_zs = rstb_zs or romfs.read(RSTB)
    rstb_out = raise_rstb(zstd, rstb_zs, {FONT_ARCHIVE: len(fonts2) - len(fonts), COMMON_ARCHIVE: len(blarc2) - len(blarc)})
    font_zs = raw_zstd(fonts2)
    common_out = raw_zstd(blarc2)
    for path, data, dec in ((FONT_ARCHIVE, font_zs, fonts2), (COMMON_ARCHIVE, common_out, blarc2)):
        if zstd.decompress(data)[0] != dec:
            raise AssertionError(f'{path}: raw zstd round trip')
    coff, clen = table_offset(otf, 'cmap')
    lines = ['# nxrecomp glyph bank v1 (games/totk/glyphs/bank.py): the engine reads this to switch button glyphs',
             f'zstd {ZHEADER} {ZBLOCK}',
             f'font {FONT_ARCHIVE}']
    for cp, off in sorted(slots.items()):
        o = new_lo + 8 + off   # the u16 (big endian) inside the decrypted otf -> archive -> file
        lines.append(f'slot {cp:04X} {SLOTS[cp]} {zstd_offset(o)} {kb[off % 4]:d} {zstd_offset(o + 1)} '
                     f'{kb[(off + 1) % 4]:d} {orig[cp]} {off - coff}')
    for name, gid, cp in bank:
        lines.append(f'glyph {name} {gid} {cp:04X}')
    lines.append(f'cmaptable {otf[coff:coff + clen].hex()}')   # (the slots' table, for the live rewrite in memory)
    lines.append(f'common {COMMON_ARCHIVE}')
    for tex, control, start, w, h, bh, g in mask_rows:
        lines.append(f'mask {tex} {control} {start} {w} {h} {bh} {g}')
    lines.append(f'maskbank masks.bin {len(groups)} {len(bank)} 72 72')
    for tex, control, start, w, h, bh, j in bc4_rows:
        lines.append(f'mask4 {tex} {control} {start} {w} {h} {bh} {j}')
    lines.append(f'maskbank4 masks_bc4.bin {len(bc4_rows)} {len(bank)} {len(bc4_bank) // max(1, len(bc4_rows) * len(bank))}')
    # DoCommand's diamond copies: name, group, control, the archive offsets of the pane's flag byte and text (0: none),
    # the flag and alpha as built, and translate .. size (the engine finds the pane objects by name and checks these)
    for name, group, control, flag_at, text_at, flag, alpha, place in letters:
        lines.append(f'pane {name} {group} {control} {lay_at + flag_at} {lay_at + text_at if text_at else 0} {flag} {alpha} {place}')
    gki_body, gki_entries = guide_key_icon(romfs, zstd)
    lines.append(f'guidekeyicon {gki_body.hex()}')
    lines += [f'gki {o} {control}' for o, control in gki_entries]
    (out_dir / 'glyphs.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    (out_dir / 'masks.bin').write_bytes(masks.tobytes())
    (out_dir / 'masks_bc4.bin').write_bytes(bytes(bc4_bank))
    (out_dir / FONT_ARCHIVE).write_bytes(font_zs)
    (out_dir / COMMON_ARCHIVE).write_bytes(common_out)
    (out_dir / RSTB).write_bytes(rstb_out)
    # the bases the engine checks: the overlay's Common archive and resource size table this was built on
    (out_dir / 'base.fnv').write_text(f'{len(common_zs)} {fnv64(common_zs):x}' + chr(10) +
                                      f'rstb {len(rstb_zs)} {fnv64(rstb_zs):x}' + chr(10))
    if preview:
        sheet_preview(otf, bank, masks, preview)
    return dict(dir=str(out_dir), bank=len(bank), slots=len(slots), letters=len(letters), mask_groups=len(groups),
                font_bytes=len(otf), common_sha=hashlib.sha256(common_out).hexdigest()[:16])


def sheet_preview(otf, bank, masks, out):
    f = ImageFont.truetype(io.BytesIO(otf), 40)
    s = ImageFont.load_default()
    cols = 12
    rows = -(-len(bank) // cols)
    im = Image.new('L', (cols * 100, rows * 70), 40)
    d = ImageDraw.Draw(im)
    for i, (name, _, cp) in enumerate(bank):
        x, y = i % cols * 100, i // cols * 70
        d.text((x + 6, y + 2), chr(cp), font=f, fill=255)
        d.text((x + 2, y + 56), name.replace('keyboard_', 'k_').replace('playstation', 'ps')[:18], font=s, fill=200)
    Path(out).mkdir(parents=True, exist_ok=True)
    im.save(Path(out) / 'bank.png')
    g = masks.shape[0]
    strip = np.concatenate([np.concatenate(list(masks[k, :24]), axis=1) for k in range(g)], axis=0)
    Image.fromarray(strip).save(Path(out) / 'masks.png')
