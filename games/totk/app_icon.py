"""The app icon (window, taskbar, Alt+Tab, Explorer, desktop shortcut) from the PLAYER's own game files: the Zonai ring
of the title screen. Nintendo's artwork: never committed, shipped or downloaded; the Builder makes the icon on the
player's PC from their RomFS, like the glyph sets (games/totk/glyphs).

python -m games.totk.app_icon --romfs <romfs-index.json + romfs.bin, or an unpacked RomFS> --out <dir> [--preview]
python -m games.totk.app_icon --neutral --out <dir>      (our own drawing: no game file read)

Writes <out>/totk.ico (16 .. 256 px: 32-bit BMP entries, the 256 one PNG) and <out>/totk.res (the same as ICON 1, a
link input of totk.exe: TOTK_APP_ICON_RES, engine/tools/recomp/mkninja_win.py); --preview also <out>/totk_icon.png
(512 px). Prints one JSON line: the source ("romfs" or "neutral") and the sizes.

The ring: timg/__Combined.bntx of the Common layout archive holds it as two BC4 masks, the ring itself
(Logo_Element_L^s, 427 px) and its shadow / glow (Logo_ElementSh_GameTitle_00^s, 547 px, the same centre). The title
layout colours them; here the ring is mint on the shadow in teal, turned 44 degrees clockwise as on the title
screen. Without the archive, a texture of another size or format (a different game version, a damaged dump), the
neutral icon is made instead and the line says so."""
import argparse
import io
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

BNTX_MEMBER = 'timg/__Combined.bntx'
RING = 'Logo_Element_L^s'
SHADOW = 'Logo_ElementSh_GameTitle_00^s'
BC4_UNORM = 0x1d01
TURN = -44                       # degrees (PIL: counter-clockwise), the title screen's angle
MINT = (74, 213, 159)  # the ring
TEAL = (34, 117, 129)            # the shadow under it
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


# ------------------------------------------------------------------------------------------------------------- source

def bc4_decode(raw, w, h, block_h):
    """BC4 (one channel) in Tegra block-linear order -> (h, w) uint8."""
    from games.totk.glyphs import bl_offsets
    bw, bh = -(-w // 4), -(-h // 4)
    offs = bl_offsets(bw, bh, 8, block_h)   # one 8-byte block per "texel" of a bw x bh image
    blocks = np.frombuffer(bytes(raw), np.uint8)[(offs[..., None] + np.arange(8)).reshape(-1, 8)]
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    pal = np.zeros((len(blocks), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    six = a0 > a1   # 6 interpolated values, else 4 and 0 / 255
    for i in range(1, 7):
        pal[:, i + 1] = ((7 - i) * a0 + i * a1) // 7
    for i in range(1, 5):
        pal[:, i + 1] = np.where(six, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(six, pal[:, 6], 0)
    pal[:, 7] = np.where(six, pal[:, 7], 255)
    bits = np.zeros(len(blocks), np.uint64)
    for k in range(6):
        bits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    idx = np.stack([(bits >> np.uint64(3 * j)) & np.uint64(7) for j in range(16)], 1).astype(np.int64)
    texels = np.take_along_axis(pal, idx, 1).astype(np.uint8).reshape(bh, bw, 4, 4)
    return texels.transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)[:h, :w]


def ring_masks(romfs_path):
    """(ring, shadow) masks as L images from the player's RomFS; raises ValueError when they are not the expected ones."""
    from games.totk.glyphs import bntx_textures
    from games.totk.overlays import COMMON_ARCHIVE, RomFS, Zstd, sarc_members
    romfs = RomFS(romfs_path)
    name = common_archive(romfs, COMMON_ARCHIVE)
    archive, _ = Zstd(romfs).decompress(romfs.read(name))
    members = sarc_members(archive)
    if BNTX_MEMBER not in members:
        raise ValueError(f'{BNTX_MEMBER} not in {name}')
    lo, hi = members[BNTX_MEMBER]
    bntx = archive[lo:hi]
    tex = bntx_textures(bntx)
    out = []
    for name, size in ((RING, 427), (SHADOW, 547)):
        t = tex.get(name)
        if not t or t['fmt'] != BC4_UNORM or (t['w'], t['h']) != (size, size):
            raise ValueError(f'{name}: missing or not a {size} px BC4 texture')
        out.append(Image.fromarray(bc4_decode(bntx[t['start']:t['start'] + t['size']], t['w'], t['h'], t['block_h']), 'L'))
    if not 0.05 < np.asarray(out[0]).mean() / 255 < 0.6:   # (a ring covers part of its square, not none or all of it)
        raise ValueError(f'{RING}: does not look like the ring')
    return out


def common_archive(romfs, v100_name):
    """The Common layout archive of this RomFS: 1.0.0's name, else the one of its version (Common.Product.<v>...)."""
    if romfs.files is not None:
        names = list(romfs.files)
    else:
        names = [f'UI/LayoutArchive/{p.name}' for p in (romfs.tree / 'UI' / 'LayoutArchive').glob('Common.Product.*')]
    if v100_name in names:
        return v100_name
    other = sorted(n for n in names if re.fullmatch(r'UI/LayoutArchive/Common\.Product\.\d+\.Nin_NX_NVN\.blarc\.zs', n))
    if not other:
        raise ValueError('no Common layout archive in this RomFS')
    return other[-1]


def compose(ring, shadow, size=1024):
    """The icon at size x size RGBA, cropped tight to what is visible (the shadow's glow included)."""
    n = shadow.width
    r = Image.new('L', (n, n))
    r.paste(ring, ((n - ring.width) // 2, (n - ring.height) // 2))
    r = np.asarray(r.rotate(TURN, resample=Image.BICUBIC)).astype(np.float32) / 255
    s = np.asarray(shadow.rotate(TURN, resample=Image.BICUBIC)).astype(np.float32) / 255
    rgb = np.array(TEAL, np.float32) * (1 - r[..., None]) + np.array(MINT, np.float32) * r[..., None]
    alpha = np.maximum(s, r)
    img = Image.fromarray(np.dstack([rgb, alpha * 255]).round().astype(np.uint8), 'RGBA')
    return square_crop(img, size)


def neutral(size=1024):
    """Our own icon (no game file): a mint ring with notches on a teal halo, for a dump the ring cannot be read from."""
    k = 4
    big = size * k
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    halo = Image.new('L', (big, big), 0)
    d = ImageDraw.Draw(halo)
    c, ro, ri = big / 2, big * 0.46, big * 0.27
    d.ellipse([c - ro, c - ro, c + ro, c + ro], fill=255)
    d.ellipse([c - ri, c - ri, c + ri, c + ri], fill=0)
    halo = halo.filter(ImageFilter.GaussianBlur(big * 0.012))
    img.paste(Image.new('RGBA', (big, big), TEAL + (255,)), mask=halo)
    ring = Image.new('L', (big, big), 0)
    d = ImageDraw.Draw(ring)
    ro, ri = big * 0.42, big * 0.31
    d.ellipse([c - ro, c - ro, c + ro, c + ro], fill=255)
    d.ellipse([c - ri, c - ri, c + ri, c + ri], fill=0)
    for i in range(24):   # notches round the outside
        a = np.radians(i * 15)
        x, y = c + np.cos(a) * big * 0.405, c + np.sin(a) * big * 0.405
        d.ellipse([x - big * 0.012, y - big * 0.012, x + big * 0.012, y + big * 0.012], fill=0)
    img.paste(Image.new('RGBA', (big, big), MINT + (255,)), mask=ring)
    return square_crop(img.resize((size, size), Image.LANCZOS), size)


def square_crop(img, size):
    a = np.asarray(img)[..., 3]
    ys, xs = np.nonzero(a > 6)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    side = max(x1 - x0, y1 - y0)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    box = (int(round(cx - side / 2)), int(round(cy - side / 2)))
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(img.crop((box[0], box[1], box[0] + side, box[1] + side)), (0, 0))
    return sq.resize((size, size), Image.LANCZOS)


# --------------------------------------------------------------------------------------------------------------- .ico

def scaled(img, px):
    """img at px x px; small sizes lose the outer glow's faint edge, which only blurs them."""
    out = img.resize((px, px), Image.LANCZOS)
    if px <= 32:
        a = np.asarray(out).copy()
        a[..., 3] = np.clip((a[..., 3].astype(np.int32) - 24) * 255 // 231, 0, 255)
        out = Image.fromarray(a, 'RGBA')
    return out


def bmp_entry(img):
    """A 32-bit BGRA DIB with its AND mask, as an .ico entry wants it (bottom-up, height doubled)."""
    w, h = img.size
    a = np.asarray(img)
    bgra = a[::-1, :, [2, 1, 0, 3]].tobytes()
    row = ((w + 31) // 32) * 4
    mask = bytearray(row * h)
    for y in range(h):
        for x in range(w):
            if a[h - 1 - y, x, 3] == 0:
                mask[y * row + x // 8] |= 0x80 >> (x % 8)
    head = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(bgra) + len(mask), 0, 0, 0, 0)
    return head + bgra + bytes(mask)


def ico_entries(img, sizes=SIZES):
    """[(px, image bytes)]: 32-bit BMP entries, PNG from 256 px up (as Windows' own icons)."""
    entries = []
    for px in sizes:
        im = scaled(img, px)
        if px >= 256:
            buf = io.BytesIO()
            im.save(buf, 'PNG', optimize=True)
            entries.append((px, buf.getvalue()))
        else:
            entries.append((px, bmp_entry(im)))
    return entries


def write_ico(entries, path):
    out = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
    off = 6 + 16 * len(entries)
    for px, data in entries:
        out += struct.pack('<BBBBHHII', px % 256, px % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    for _, data in entries:
        out += data
    write_if_changed(path, bytes(out))


def res_entry(rtype, name, data, flags, lang=0x0409):
    """One resource of a .res file (numeric type and name, US English), padded to 4 bytes."""
    head = struct.pack('<IIHHHHIHHII', len(data), 32, 0xffff, rtype, 0xffff, name, 0, flags, lang, 0, 0)
    return head + data + b'\0' * (-len(data) % 4)


def write_res(entries, path):
    """A .res holding the icon as ICON 1 (RT_GROUP_ICON 1 over RT_ICON 1..n): lld-link takes it as a link input, the
    same as llvm-rc's output of '1 ICON "totk.ico"' (no resource compiler needed on the player's PC)."""
    out = bytearray(res_entry(0, 0, b'', 0, 0))   # (a .res starts with an empty entry)
    group = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
    for i, (px, data) in enumerate(entries, 1):
        out += res_entry(3, i, data, 0x1010)    # RT_ICON, moveable | discardable
        group += struct.pack('<BBBBHHIH', px % 256, px % 256, 0, 0, 1, 32, len(data), i)
    out += res_entry(14, 1, bytes(group), 0x1030)   # RT_GROUP_ICON, + pure
    write_if_changed(path, bytes(out))


def write_if_changed(path, data):
    """Kept as it is when the same: totk.exe relinks when the .res is newer (ninja), so only once per new icon."""
    path = Path(path)
    if not (path.is_file() and path.read_bytes() == data):
        path.write_bytes(data)


def main(argv=None):
    p = argparse.ArgumentParser(prog='python -m games.totk.app_icon', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', type=Path, help='romfs-index.json + romfs.bin, or an unpacked RomFS tree')
    p.add_argument('--neutral', action='store_true', help='our own icon, no game file read')
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--preview', action='store_true', help='also <out>/totk_icon.png (512 px)')
    a = p.parse_args(argv)
    if not a.neutral and not a.romfs:
        p.error('--romfs or --neutral')
    source, why = 'neutral', 'asked' if a.neutral else ''
    img = None
    if not a.neutral:
        try:
            img = compose(*ring_masks(a.romfs))
            source = 'romfs'
        except Exception as e:  # (whatever the dump holds: the Setup never fails over an icon)
            why = f'{type(e).__name__}: {e}'
    if img is None:
        img = neutral()
    a.out.mkdir(parents=True, exist_ok=True)
    entries = ico_entries(img)
    write_ico(entries, a.out / 'totk.ico')
    write_res(entries, a.out / 'totk.res')
    if a.preview:
        img.resize((512, 512), Image.LANCZOS).save(a.out / 'totk_icon.png')
    print(json.dumps(dict(source=source, why=why, ico=str(a.out / 'totk.ico'), res=str(a.out / 'totk.res'),
                     sizes=list(SIZES))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
