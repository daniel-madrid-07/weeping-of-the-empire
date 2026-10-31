"""PC button prompts for TotK's HUD: RomFS overlay sets that redraw the game's button icons as Xbox, PlayStation or
keyboard-and-mouse prompts.

What the game draws: the action guide (blyt/DoCommand_00.bflyt in UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc)
composes each prompt from a base and one of seven symbol masks in timg/__Combined.bntx, Nt_KeyTex{A,B,X,Y,L,ZL,LSt}
(R8_UNORM, 72x72, 1 level, block-linear with 8-GOB blocks): white button shape, symbol cut out, coloured by the layout.
Kenney's "Input Prompts" (CC0, www.kenney.nl) draws its filled icons the same way (opaque shape, transparent symbol), so a
replacement mask is the icon's alpha, fitted into the original mask's bounding box and swizzled back in place. The
texture keeps its size, format and offset: the BNTX and the SARC are untouched around it.

Sets: the Common archive is already replaced by the layout sets (base, aspect-<w>x<h>[-center]) and a RomFS path may have
only one overlay, so each style is built as a variant of each layout set: <set>+glyphs-<style>, a full copy of <set>
with the glyphs on top of its archive. The runtime picks the variant when TOTK_GLYPHS=<style> (see README.md).

Not covered yet (no texture for them in this archive; they are font glyphs or other layouts): R, ZR, +, -, the d-pad and
the right stick. The styles follow controls.face_buttons = label (Xbox A / PlayStation Cross = Switch A) and, for
kbm, the pc keyboard preset."""
import hashlib
import struct
from pathlib import Path

import numpy as np
from PIL import Image

from games.totk.overlays import COMMON_ARCHIVE, MANIFEST_HEADER, sarc_members

BNTX_MEMBER = 'timg/__Combined.bntx'
R8_UNORM = 0x0201
# Switch texture -> Kenney icon (Double = 128 px), per style. Missing keys keep the game's own mask (the Xbox face
# letters are the Switch's: with label mapping Xbox A is Switch A).
STYLES = {
    'xbox': {
        'Nt_KeyTexL_00^d': 'Xbox Series/Double/xbox_lb.png',
        'Nt_KeyTexZL_00^d': 'Xbox Series/Double/xbox_lt.png',
        'Nt_KeyTexLSt_00^d': 'Xbox Series/Double/xbox_stick_l_up.png',
        'Nt_KeyTexLSt_01^d': 'Xbox Series/Double/xbox_stick_l.png',
    },
    'ps': {
        'Nt_KeyTexA_00^d': 'PlayStation Series/Double/playstation_button_cross.png',
        'Nt_KeyTexB_00^d': 'PlayStation Series/Double/playstation_button_circle.png',
        'Nt_KeyTexX_00^d': 'PlayStation Series/Double/playstation_button_square.png',
        'Nt_KeyTexY_00^d': 'PlayStation Series/Double/playstation_button_triangle.png',
        'Nt_KeyTexL_00^d': 'PlayStation Series/Double/playstation_trigger_l1.png',
        'Nt_KeyTexZL_00^d': 'PlayStation Series/Double/playstation_trigger_l2.png',
        'Nt_KeyTexLSt_00^d': 'PlayStation Series/Double/playstation_stick_l_up.png',
        'Nt_KeyTexLSt_01^d': 'PlayStation Series/Double/playstation_stick_l.png',
    },
    # the pc keyboard preset (platform/src/input.c pcPreset): A = E, B = Shift, X = Space, Y = left mouse, L = F,
    # ZL = Q, the left stick = W (forward) / Ctrl (crouch is the stick press)
    'kbm': {
        'Nt_KeyTexA_00^d': 'Keyboard & Mouse/Double/keyboard_e.png',
        'Nt_KeyTexB_00^d': 'Keyboard & Mouse/Double/keyboard_shift.png',
        'Nt_KeyTexX_00^d': 'Keyboard & Mouse/Double/keyboard_space.png',
        'Nt_KeyTexY_00^d': 'Keyboard & Mouse/Double/mouse_left.png',
        'Nt_KeyTexL_00^d': 'Keyboard & Mouse/Double/keyboard_f.png',
        'Nt_KeyTexZL_00^d': 'Keyboard & Mouse/Double/keyboard_q.png',
        'Nt_KeyTexLSt_00^d': 'Keyboard & Mouse/Double/keyboard_w.png',
        'Nt_KeyTexLSt_01^d': 'Keyboard & Mouse/Double/keyboard_ctrl.png',
    },
}
KENNEY_ZIP_SHA256 = 'ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1'   # kenney_input-prompts_1.5.zip


# ------------------------------------------------------------------------------------------------------- textures

def gob_off(x, y):
    """Byte offset of (x bytes, y rows) inside a 64x8 GOB (Tegra block-linear)."""
    return (x % 64 // 32) * 256 + (y % 8 // 2) * 64 + (x % 32 // 16) * 32 + (y % 2) * 16 + x % 16


def bl_offsets(w, h, bpp, block_h):
    """offsets[y, x]: the block-linear byte offset of texel (x, y) (bpp bytes each; block_h GOBs per block)."""
    gx = -(-w * bpp // 64)
    out = np.zeros((h, w), np.int64)
    for y in range(h):
        for x in range(w):
            xb = x * bpp
            out[y, x] = ((y // (8 * block_h)) * gx + xb // 64) * 512 * block_h + ((y % (8 * block_h)) // 8) * 512 + gob_off(xb, y)
    return out


def bntx_textures(bntx):
    """name -> dict(fmt, w, h, levels, block_h, start, size) for every BRTI (offsets relative to the BNTX)."""
    if bntx[:4] != b'BNTX':
        raise ValueError('not a BNTX')
    count, info_ptr = struct.unpack_from('<IQ', bntx, 0x24)
    out = {}
    for i in range(count):
        off = struct.unpack_from('<Q', bntx, info_ptr + i * 8)[0]
        if bntx[off:off + 4] != b'BRTI':
            continue
        (_, dim, tile_mode, _, levels, _, fmt, _, w, h, d, layers, layout) = struct.unpack_from('<BBHHHIIIIIIII', bntx, off + 0x10)
        size = struct.unpack_from('<I', bntx, off + 0x50)[0]
        name_off = struct.unpack_from('<Q', bntx, off + 0x60)[0]
        n = struct.unpack_from('<H', bntx, name_off)[0]
        name = bntx[name_off + 2:name_off + 2 + n].decode('utf-8')
        mip_ptr = struct.unpack_from('<Q', bntx, off + 0x70)[0]
        start = struct.unpack_from('<Q', bntx, mip_ptr)[0]
        out[name] = dict(fmt=fmt, w=w, h=h, levels=levels, layers=layers, tile_mode=tile_mode, block_h=1 << (layout & 7),
                         start=start, size=size)
    return out


def read_r8(bntx, t):
    offs = bl_offsets(t['w'], t['h'], 1, t['block_h'])
    flat = np.frombuffer(bytes(bntx[t['start']:t['start'] + t['size']]), np.uint8)
    return flat[offs]


def write_r8(bntx, t, img):
    offs = bl_offsets(t['w'], t['h'], 1, t['block_h'])
    region = np.frombuffer(bytes(bntx[t['start']:t['start'] + t['size']]), np.uint8).copy()
    region[offs] = img
    bntx[t['start']:t['start'] + t['size']] = region.tobytes()


def bbox(mask, threshold=24):
    ys, xs = np.nonzero(mask > threshold)
    if not len(xs):
        raise ValueError('empty mask')
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def kenney_mask(path, like):
    """The icon's alpha (shape opaque, symbol cut out) fitted into the bounding box of `like` (the game's mask), centred,
    aspect kept, as an R8 image of like's size."""
    icon = np.asarray(Image.open(path).convert('RGBA'))[:, :, 3]
    x0, y0, x1, y1 = bbox(icon)
    icon = Image.fromarray(np.ascontiguousarray(icon[y0:y1, x0:x1]))
    bx0, by0, bx1, by1 = bbox(like)
    bw, bh = bx1 - bx0, by1 - by0
    k = min(bw / icon.width, bh / icon.height)
    w, h = max(1, round(icon.width * k)), max(1, round(icon.height * k))
    icon = icon.resize((w, h), Image.LANCZOS)
    out = Image.new('L', (like.shape[1], like.shape[0]), 0)
    out.paste(icon, (bx0 + (bw - w) // 2, by0 + (bh - h) // 2))
    return np.asarray(out, np.uint8)


def apply_style(blarc, members, art_dir, style, preview_dir=None):
    """Redraws the style's masks inside the decompressed Common archive (in place). Returns the textures changed."""
    lo, hi = members[BNTX_MEMBER]
    bntx = bytearray(blarc[lo:hi])
    textures = bntx_textures(bntx)
    changed = []
    for name, rel in STYLES[style].items():
        t = textures.get(name)
        if not t:
            raise ValueError(f'{name} not in {BNTX_MEMBER}')
        if t['fmt'] != R8_UNORM or t['levels'] != 1 or t['layers'] != 1 or t['tile_mode'] != 0:
            raise ValueError(f'{name}: format {t["fmt"]:#x}, {t["levels"]} levels, {t["layers"]} layers, tile mode {t["tile_mode"]}: not the 1-level R8 block-linear texture expected')
        expected = -(-t['w'] // 64) * -(-t['h'] // (8 * t['block_h'])) * 512 * t['block_h']
        if expected != t['size']:
            raise ValueError(f'{name}: {t["size"]} bytes, block-linear layout gives {expected}')
        original = read_r8(bntx, t)
        mask = kenney_mask(Path(art_dir) / rel, original)
        write_r8(bntx, t, mask)
        if not np.array_equal(read_r8(bntx, t), mask):
            raise AssertionError(f'{name}: swizzle round trip')
        if preview_dir:
            Path(preview_dir).mkdir(parents=True, exist_ok=True)
            pair = np.concatenate([original, np.full((t['h'], 4), 80, np.uint8), mask], axis=1)
            Image.fromarray(pair).resize((pair.shape[1] * 3, pair.shape[0] * 3), Image.NEAREST).save(
                Path(preview_dir) / f'{style}-{name.replace("^", "_")}.png')
        changed.append(name)
    blarc[lo:hi] = bntx
    return changed


# ------------------------------------------------------------------------------------------------------------ sets

def read_manifest(set_dir):
    rows = []
    for line in (Path(set_dir) / 'manifest.tsv').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            path, size, sha, orig = line.split('\t')
            rows.append((path, int(size), sha, orig))
    return rows


def build_variant(romfs, zstd, overlays, base_set, style, art_dir, preview_dir=None):
    """<overlays>/<base_set>+glyphs-<style>/: base_set's files, its Common archive with the style's glyphs."""
    src = Path(overlays) / base_set
    rows = read_manifest(src)
    if not any(p == COMMON_ARCHIVE for p, *_ in rows):
        raise ValueError(f'{base_set} does not carry {COMMON_ARCHIVE}')
    name = f'{base_set}+glyphs-{style}'
    out_dir = Path(overlays) / name
    lines, files = [], []
    for path, size, sha, orig in rows:
        data = (src / path).read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            raise ValueError(f'{base_set}/{path} does not match its manifest')
        if path == COMMON_ARCHIVE:
            blarc, dict_id = zstd.decompress(data)
            before = bytes(blarc)
            changed = apply_style(blarc, sarc_members(blarc), art_dir, style, preview_dir)
            lo, hi = sarc_members(blarc)[BNTX_MEMBER]
            diff = np.nonzero(np.frombuffer(before, np.uint8) != np.frombuffer(bytes(blarc), np.uint8))[0]
            if not len(diff) or diff.min() < lo or diff.max() >= hi:
                raise AssertionError('glyph edit outside the texture container')
            data = zstd.compress(blarc, dict_id)
        files.append((path, data))
        lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(), orig]))
    for path, data in files:   # written only after every check passed
        target = out_dir / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    with open(out_dir / 'manifest.tsv', 'w', newline='\n', encoding='utf-8') as f:
        f.write(MANIFEST_HEADER.replace('python -m games.totk.overlays', 'python -m games.totk.glyphs') + '\n'.join(lines) + '\n')
    return dict(set=name, dir=str(out_dir), textures=changed, changed_bytes=int(len(diff)))


def layout_sets(overlays):
    """The sets that carry the Common archive (base, aspect-*), so the ones a glyph variant is built from."""
    out = []
    for d in sorted(Path(overlays).iterdir()):
        if d.is_dir() and '+glyphs-' not in d.name and (d / 'manifest.tsv').exists():
            if any(p == COMMON_ARCHIVE for p, *_ in read_manifest(d)):
                out.append(d.name)
    return out
