"""RomFS overlays for TotK 1.0.0.

Each set is a folder holding replacement files under their RomFS paths plus manifest.tsv in MacNativeGames' mod
manifest format (runtime/romfs.c there): a '#' header line, then one line per file,
    <RomFS path> \\t <size> \\t <SHA-256 of the replacement> \\t <SHA-256 of the original, or '-' for an added file>
The runtime serves a listed file in place of the original after checking both hashes; two sets may not replace the
same file. Sets written here (`python -m games.totk.overlays`):

  base/                       quick-menu capture timing + game-over retry (needed with the 60 FPS / real-time patches)
  aspect-<w>x<h>[-center]/    the same two edits plus the UI laid out for a w:h canvas (ultrawide or 16:10);
                              use it instead of base/ (both replace the Common archive)
  output-<w>x<h>/             HeapDef with the texture heap grown for a w x h output (+ overlay.json); any size whose
                              pixel count is above 1920x1080 needs it, together with totk_output_extra_bytes()

Ported from MacNativeGames (MIT): games/totk/scripts/make_romfs_patches.py and make_output_res_overlay.py. Every edit
checks the value the game ships before writing it, and the original file's SHA-256 is pinned where offsets come from it.
"""
import hashlib
import json
import math
import struct
from pathlib import Path

import zstandard

# ------------------------------------------------------------------------------------------------------------ RomFS


class RomFS:
    """Read files from the extracted RomFS: pipeline's <dir>/romfs-index.json + romfs.bin (pipeline/extract.py),
    MacNativeGames' <dir>/index.json + romfs.bin, or an unpacked tree (<dir>/<path>)."""

    def __init__(self, root):
        self.root = Path(root)
        self.files, self.image = None, None
        for index in ('romfs-index.json', 'index.json'):
            p = self.root / index
            if p.exists():
                meta = json.loads(p.read_text(encoding='utf-8'))
                if meta.get('status', 'pass') != 'pass':
                    raise ValueError(f'{p}: status {meta.get("status")}')
                self.files = {f['path'].lstrip('/'): f for f in meta['files']}
                self.image = self.root / meta.get('image', 'romfs.bin')
                break
        if self.files is None:
            tree = self.root / 'romfs' if (self.root / 'romfs').is_dir() else self.root
            if not (tree / 'Pack' / 'ZsDic.pack.zs').exists():
                raise FileNotFoundError(f'{root}: no romfs-index.json, index.json or unpacked RomFS tree')
            self.tree = tree

    def read(self, path):
        if self.files is None:
            return (self.tree / path).read_bytes()
        f = self.files[path]
        with self.image.open('rb') as image:
            image.seek(f['offset'])
            data = image.read(f['size'])
        if len(data) != f['size']:
            raise ValueError(f'{path}: short read')
        return data


class Zstd:
    """The game's zstd dictionaries (Pack/ZsDic.pack.zs) and frame round trips with them."""

    def __init__(self, romfs):
        pack = self._whole(zstandard.ZstdDecompressor(), romfs.read('Pack/ZsDic.pack.zs'))
        self.dicts = {}
        for lo, hi in sarc_members(pack).values():
            d = zstandard.ZstdCompressionDict(pack[lo:hi])
            self.dicts[d.dict_id()] = d

    @staticmethod
    def _whole(dctx, raw):
        """Decompress one frame whether or not its header records the content size."""
        obj = dctx.decompressobj()
        out = obj.decompress(raw)
        if not obj.eof:
            raise ValueError('truncated zstd frame')
        return out

    def decompress(self, raw):
        info = zstandard.get_frame_parameters(raw)
        d = self.dicts[info.dict_id] if info.dict_id else None
        out = self._whole(zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor(), raw)
        if info.content_size not in (zstandard.CONTENTSIZE_UNKNOWN, len(out)):
            raise ValueError('zstd frame content size mismatch')
        return bytearray(out), info.dict_id

    def compress(self, data, dict_id):
        d = self.dicts[dict_id] if dict_id else None
        kw = dict(level=19, write_content_size=True, write_checksum=False)
        c = zstandard.ZstdCompressor(dict_data=d, write_dict_id=True, **kw) if d else zstandard.ZstdCompressor(**kw)
        out = c.compress(bytes(data))
        info = zstandard.get_frame_parameters(out)
        back, _ = self.decompress(out)
        if info.dict_id != dict_id or info.content_size != len(data) or back != data:
            raise ValueError('zstd round trip failed')
        return out


# ------------------------------------------------------------------------------------------------------------ formats


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'
    names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[bytes(data[offset:data.index(0, offset, start)]).decode()] = (start + lo, start + hi)
    return out


def flvi_keys(data, base):
    """(pane name, absolute key offset, keys) of every FLVI (visibility) step track in the BFLAN at data[base:]."""
    assert data[base:base + 4] == b'FLAN' and data[base + 4:base + 6] == b'\xff\xfe'
    pos, out = base + 0x14, []
    for _ in range(struct.unpack_from('<H', data, base + 0x10)[0]):
        magic, size = bytes(data[pos:pos + 4]), struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'pai1':
            count, table = struct.unpack_from('<H', data, pos + 14)[0], pos + struct.unpack_from('<I', data, pos + 16)[0]
            for e in range(count):
                eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
                name = bytes(data[eo:eo + 28]).split(b'\0')[0].decode()
                for t in range(data[eo + 28]):
                    to = eo + struct.unpack_from('<I', data, eo + 32 + 4 * t)[0]
                    if data[to:to + 4] != b'FLVI':
                        continue
                    for g in range(data[to + 4]):
                        go = to + struct.unpack_from('<I', data, to + 8 + 4 * g)[0]
                        assert data[go + 2] == 1, 'visibility tracks are step curves'
                        ko = go + struct.unpack_from('<I', data, go + 8)[0]
                        keys = [struct.unpack_from('<fH', data, ko + 8 * k) for k in range(struct.unpack_from('<H', data, go + 4)[0])]
                        out.append((name, ko, keys))
        pos += size
    return out


# ------------------------------------------------------------------------------------------------------------ edits

COMMON_ARCHIVE = 'UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs'
# The other layout archives: the title screen's own copies of the system menus and loading screen (43 layouts), the
# boot cursors (2). An aspect set narrows them like Common's, or the title comes out stretched on a wide canvas.
OTHER_ARCHIVES = ('UI/LayoutArchive/Title.Product.100.Nin_NX_NVN.blarc.zs',
                  'UI/LayoutArchive/Boot.Product.100.Nin_NX_NVN.blarc.zs')
# The weapon / shield / material quick menus capture the game picture for their background when these panes become
# visible at animation frame 1.0. UI animations advance 0.5 frames per game frame at 60 fps, so the capture lands after
# the world stopped drawing and the background can be black: the visibility keys move to frame 0.0.
CAPTURE_MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}


def patch_capture(blarc, members, frame=0.0):
    """Move the capture panes' visibility keys from frame 1.0 to `frame` (each must hold the shipping (0.0, 0) (1.0, 1)).
    Returns member -> panes moved."""
    patched = {}
    for member, panes in CAPTURE_MEMBERS.items():
        lo, _ = members[member]
        changed = []
        for name, ko, keys in flvi_keys(blarc, lo):
            if name not in panes:
                continue
            if [(f, v) for f, v in keys] != [(0.0, 0), (1.0, 1)]:
                raise ValueError(f'{member}: pane {name} has the keys {keys}, not the shipping (0.0, 0) (1.0, 1)')
            struct.pack_into('<f', blarc, ko + 8, frame)
            changed.append(name)
        if sorted(changed) != sorted(panes):
            raise ValueError(f'{member}: found the panes {changed}, expected {list(panes)}')
        patched[member] = changed
    return patched


GAMEOVER_FLOW = 'Event/EventFlow/DmF_SY_GameOver.bfevfl.zs'
GAMEOVER_SHA256 = 'c59975494fb30063b7a339fd66bbde8b456b11c73a5b4332bc070d63ece66b7d'   # the decompressed 1.0.0 flow


def patch_gameover(flow):
    """After retry / quit the game-over flow requests GameOverPause (Event14), then waits one frame (Event25, EventWait
    Frames 1). With the real-time step a frame is a fraction of the shipping one, the pause freezes the wait after its
    first frame and the menu never goes away. Event14 ends the branch itself: its next-event index goes from Event25 to
    none. BFEVFL events are 0x28 bytes from 0x1f0 (name ref at +0, type at +8, next at +0xa); the SHA-256 pins that."""
    if hashlib.sha256(flow).hexdigest() != GAMEOVER_SHA256:
        raise ValueError('DmF_SY_GameOver.bfevfl is not the analysed 1.0.0 file')

    def event(i):
        e = 0x1f0 + 0x28 * i
        ref = struct.unpack_from('<Q', flow, e)[0]
        n = struct.unpack_from('<H', flow, ref)[0]
        return e, bytes(flow[ref + 2:ref + 2 + n]).decode(), flow[e + 8], struct.unpack_from('<H', flow, e + 0xa)[0]

    e14, name14, type14, next14 = event(7)
    _, name25, type25, next25 = event(25)
    if (name14, type14, next14) != ('Event14', 0, 25) or (name25, type25, next25) != ('Event25', 0, 0xffff):
        raise ValueError('game-over flow events are not where the analysed file has them')
    struct.pack_into('<H', flow, e14 + 0xa, 0xffff)


OPENING_FLOW = 'Event/EventFlow/OpeningEvent.bfevfl.zs'
OPENING_SHA256 = 'b8a814de028b2d6289001407b5016f0ce48da7dc229769a8be01f11ad98d92e0'   # the decompressed 1.0.0 flow


def patch_skip_opening(flow):
    """A new game enters OpeningEvent at IntroductionOpening (event 112, Event116: the underground prologue). The
    prologue's own end, reached from CheckedMummy, is the chain Event168 (event 28: fade out) -> clear pouch -> wait for
    the equipment -> max life 12 -> max stamina 1000 -> IsMasterSwordTrueForm off -> belts hidden -> Event77
    (ReserveBancChange: MainField, StartPos_StartIsland_WakeupAltar, boot event SageOfSky_WakeUpOnStart) -> Event123
    (Transition to DmT_OP_GanonWakeUp_PreRender). The entry point's main event goes from 112 to 28, so a new game plays
    that chain and wakes up on the Great Sky Island. BFEVFL: the flowchart's entry points are 0x20 bytes, main event at
    +0x1c; events are 0x28 bytes (name ref at +0, type at +8, next at +0xa); the SHA-256 pins the layout."""
    if hashlib.sha256(flow).hexdigest() != OPENING_SHA256:
        raise ValueError('OpeningEvent.bfevfl is not the analysed 1.0.0 file')
    fc = struct.unpack_from('<Q', flow, struct.unpack_from('<Q', flow, 0x28)[0])[0]
    events, entries = (struct.unpack_from('<Q', flow, fc + o)[0] for o in (0x30, 0x40))

    def event(i):
        e = events + 0x28 * i
        ref = struct.unpack_from('<Q', flow, e)[0]
        n = struct.unpack_from('<H', flow, ref)[0]
        return bytes(flow[ref + 2:ref + 2 + n]).decode(), flow[e + 8], struct.unpack_from('<H', flow, e + 0xa)[0]

    intro = entries + 0x1c   # entry point 0: IntroductionOpening (the dictionary's first name)
    if (struct.unpack_from('<H', flow, intro)[0], event(112)[:2], event(28), event(27)[:2]) != \
            (112, ('Event116', 0), ('Event168', 0, 4), ('Event77', 0)):
        raise ValueError('opening flow events are not where the analysed file has them')
    struct.pack_into('<H', flow, intro, 28)


WARPIN_FLOW = 'Event/EventFlow/DmF_SY_WarpIn.bfevfl.zs'
WARPIN_SHA256 = '753f913c66286c6dff189f5ba32eb17d56ead9fd64fa8fc3f3ec035b100b3c4f'   # the decompressed 1.0.0 flow
# The fast-travel departure (DmF_SY_WarpIn; it ends in EventReserveBancChange, the arrival is DmF_SY_WarpOut). Its time
# values, in game frames (EventWait counts the game's frame delta, so they are game time at any frame rate): (event,
# parameter, value offset, shipping value). Standing on the ground it runs camera 20, waits 20 + 10 + 70 + 5 + 35, with
# Link's dissolve (FadeFrame 120) starting 120 frames before the bank change, so the cut comes as he has gone.
WARPIN_TIMES = (
    ('Event10', 'Frames', 0x1630, 10), ('Event0', 'Frames', 0x16b8, 70), ('Event19', 'Frames', 0x1870, 10),
    ('Event24', 'FadeFrame', 0x18e0, 120.0), ('Event25', 'FadeFrame', 0x1968, 120.0), ('Event31', 'Frames', 0x1dd8, 20),
    ('Event29', 'MoveTime', 0x2530, 20), ('Event34', 'MoveTime', 0x2a08, 20), ('Event36', 'MoveTime', 0x30a8, 60),
    ('Event37', 'MoveTime', 0x35b8, 60), ('Event38', 'Frames', 0x3738, 35), ('Event53', 'Frames', 0x3ba0, 5),
    ('Event54', 'Frames', 0x3c28, 5), ('Event82', 'Frames', 0x4918, 10),
)
# 'waits': the long waits only (70 -> 35, 35 -> 18, 20 -> 10, camera 20 -> 10); the dissolve keeps its 120 frames and is
# cut at about 60 %. 'whole': every time value halved, dissolve included, so it completes as on the Switch, twice as fast.
# Both shorten the departure from ~6.7 s to ~4 s (ft7, 2026-10-25, shots every 15 frames); 'waits' shows Link's dissolve
# at the Switch's pace (tinted, blue, gone, as in ft4-ship), 'whole' at double speed. 'waits' is the shipped one.
FAST_TRAVEL = {'waits': {'Event0': 35, 'Event38': 18, 'Event31': 10, 'Event29': 10, 'Event34': 10},
               'whole': None}


def patch_fast_travel(flow, variant):
    """Shorter fast-travel departure: the time values of WARPIN_TIMES per FAST_TRAVEL[variant] (None = all halved,
    rounded up; 1 stays 1). In place: integer and float parameters keep their size. Returns {event: (old, new)}."""
    if hashlib.sha256(flow).hexdigest() != WARPIN_SHA256:
        raise ValueError('DmF_SY_WarpIn.bfevfl is not the analysed 1.0.0 file')
    table = FAST_TRAVEL[variant]
    changed = {}
    for event, param, at, value in WARPIN_TIMES:
        fmt = '<f' if isinstance(value, float) else '<i'
        if struct.unpack_from(fmt, flow, at)[0] != value:
            raise ValueError(f'{event} {param} is not {value} at {at:#x}')
        new = table.get(event, value) if table is not None else type(value)(max(1, -(-value // 2)))
        if new != value:
            struct.pack_into(fmt, flow, at, new)
            changed[f'{event}.{param}'] = (value, new)
    return changed


HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
DTA_KIB, GRAPHICS_KIB = 113664, 201728      # shipped DynamicTextureAllocatorMem and Graphics sizes (KiB)
TEXTURE_KIB, GAME_GFX_KIB = 501760, 552416  # shipped TextureHeap and its parent ゲームグラフィックス (KiB)
TEXTURE_MAX = 3.0                           # largest texture cache multiplier (+980 MiB)
BASE_W, BASE_H = 1920, 1080
SCENE_W, SCENE_H = 1600, 900


def extra_kib(w, h, sw=0, sh=0):
    """KiB added to DynamicTextureAllocatorMem, Graphics and the application pool for a w x h output and an optional
    sw x sh scene: the texture heap grows by the output's pixel ratio above 1920x1080, a bigger scene with a 1.6 margin.
    Same formula as totk_output_extra_bytes() (games/totk/runtime/totk_patches.c): keep the two in step."""
    scene = sw * sh > SCENE_W * SCENE_H
    grow = max(0, w * h / (BASE_W * BASE_H) - 1) + (sw * sh / (SCENE_W * SCENE_H) - 1 if scene else 0)
    if grow <= 0:
        return 0
    if scene:
        grow *= 1.6
    return math.ceil(DTA_KIB * (1 + grow) / 1024) * 1024 - DTA_KIB


def tex_extra_kib(tex):
    """KiB the texture cache multiplier `tex` adds to TextureHeap, its parent and the application pool (whole MiB).
    Same formula as totk_output_extra_bytes() (games/totk/runtime/totk_patches.c): keep the two in step."""
    if not 1.0 <= tex <= TEXTURE_MAX:
        raise ValueError(f'texture cache x{tex} out of range (1..{TEXTURE_MAX:g})')
    return math.ceil(TEXTURE_KIB * (tex - 1) / 1024) * 1024


def patch_heapdef(byml, extra, tex_extra=0):
    """Grow DynamicTextureAllocatorMem and Graphics by `extra` KiB, and TextureHeap (agl::TextureDataMgr's texture
    cache, GameGraphicsModule::prepare_) and its parent ゲームグラフィックス by `tex_extra` KiB, in the HeapDef BYML (v7,
    little endian). Returns the (old, new) sizes. Only those 4-byte values change."""
    assert byml[:2] == b'YB' and struct.unpack_from('<H', byml, 2)[0] == 7
    key_table, _, root_node = struct.unpack_from('<III', byml, 4)
    n = struct.unpack_from('<I', byml, key_table)[0] >> 8
    offs = struct.unpack_from(f'<{n + 1}I', byml, key_table + 4)
    keys = [bytes(byml[key_table + o: byml.index(0, key_table + o)]).decode() for o in offs[:n]]

    def field(node, name, want_type):
        assert byml[node] == 0xc1
        for i in range(struct.unpack_from('<I', byml, node)[0] >> 8):
            e = node + 4 + 8 * i
            if keys[struct.unpack_from('<I', byml, e)[0] & 0xffffff] == name:
                if byml[e + 3] != want_type:
                    raise ValueError(f'HeapDef {name}: type {byml[e + 3]:#x}, expected {want_type:#x}')
                return e + 4
        raise KeyError(name)

    def child(node, name):
        return struct.unpack_from('<I', byml, field(node, name, 0xc1))[0]

    heaps = child(root_node, 'Heaps')
    sizes = dict(DynamicTextureAllocatorMem=(DTA_KIB, extra), Graphics=(GRAPHICS_KIB, extra),
                 TextureHeap=(TEXTURE_KIB, tex_extra), ゲームグラフィックス=(GAME_GFX_KIB, tex_extra))
    at = {name: field(child(heaps, name), 'Size', 0xd1) for name in sizes}
    for name, (shipped, _) in sizes.items():
        held = struct.unpack_from('<i', byml, at[name])[0]
        if held != shipped:
            raise ValueError(f'HeapDef {name} holds {held} KiB, not the shipped {shipped}')
    base = bytes(byml)
    out = {}
    for name, (shipped, grow) in sizes.items():
        if grow:
            struct.pack_into('<i', byml, at[name], shipped + grow)
            out[f'{name}_kib'] = [shipped, shipped + grow]
    diff = [i for i in range(len(base)) if base[i] != byml[i]]
    assert diff and all(any(a <= i < a + 4 for a in at.values()) for i in diff), diff
    return out


# ------------------------------------------------------------------------------------------------------------ sets

MANIFEST_HEADER = '# path\tsize\tsha256\toriginal_sha256 (generated by python -m games.totk.overlays)\n'


def _write_set(out_dir, entries):
    """entries: [(RomFS path, new bytes, original bytes)] -> files + manifest.tsv. Returns the manifest lines."""
    out_dir = Path(out_dir)
    lines = []
    for path, data, raw in entries:
        target = out_dir / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()]))
    with open(out_dir / 'manifest.tsv', 'w', newline='\n', encoding='utf-8') as f:
        f.write(MANIFEST_HEADER + '\n'.join(lines) + '\n')
    return lines


def parse_size(text):
    w, h = (int(v) for v in text.lower().split('x'))
    return w, h


def build_base(romfs, zstd, out_root, aspect=None, hud='corner', capture_frame=0.0):
    """base/ or aspect-<w>x<h>[-center]/: the Common archive (capture keys, plus the UI layout for `aspect`) and the
    game-over flow."""
    assert 0 <= capture_frame <= 0.5
    raw = romfs.read(COMMON_ARCHIVE)
    blarc, dict_id = zstd.decompress(raw)
    original = bytes(blarc)
    members = sarc_members(blarc)
    moved = patch_capture(blarc, members, capture_frame)
    layout = []
    name = 'base'
    if aspect:
        w, h = parse_size(aspect)
        if w * 9 == h * 16:
            raise ValueError(f'{aspect} is 16:9: use base/')
        from . import tall_layout, ultrawide_layout
        module = ultrawide_layout if w * 9 > h * 16 else tall_layout
        layout = module.apply(blarc, members, w, h, hud_corner=hud == 'corner')
        name = f'aspect-{w}x{h}' + ('-center' if hud == 'center' else '')
    diff = [i for i in range(len(original)) if original[i] != blarc[i]]
    allowed = [members[m] for m in [*CAPTURE_MEMBERS, *layout]]
    assert diff and all(any(lo <= i < hi for lo, hi in allowed) for i in diff), diff[:20]
    archive = zstd.compress(blarc, dict_id)

    flow_raw = romfs.read(GAMEOVER_FLOW)
    flow, flow_dict = zstd.decompress(flow_raw)
    patch_gameover(flow)
    flow_data = zstd.compress(flow, flow_dict)

    others = []
    if aspect:
        for path in OTHER_ARCHIVES:
            o_raw = romfs.read(path)
            o_blarc, o_dict = zstd.decompress(o_raw)
            o_orig = bytes(o_blarc)
            o_members = sarc_members(o_blarc)
            o_changed = module.apply(o_blarc, o_members, w, h, hud_corner=hud == 'corner', common=False)
            o_diff = [i for i in range(len(o_orig)) if o_orig[i] != o_blarc[i]]
            o_allowed = [o_members[m] for m in o_changed]
            assert o_diff and all(any(lo <= i < hi for lo, hi in o_allowed) for i in o_diff), o_diff[:20]
            others.append((path, zstd.compress(o_blarc, o_dict), o_raw, len(o_changed)))
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(COMMON_ARCHIVE, archive, raw), (GAMEOVER_FLOW, flow_data, flow_raw),
                         *[(path, data, o_raw) for path, data, o_raw, _ in others]])
    return dict(set=name, dir=str(out_dir), capture=moved, capture_frame=f'1.0 -> {capture_frame}',
                layout=dict(aspect=aspect, hud=hud, members=len(layout),
                            other_archives={p.split('/')[-1]: n for p, _, _, n in others}) if aspect else None,
                common_changed_bytes=len(diff), gameover='Event14 next: Event25 -> end')


def build_skip_opening(romfs, zstd, out_root):
    """skip_opening/: the opening flow that starts a new game at the Great Sky Island wake-up. The engine serves it before
    the RomFS while "Skip the opening" is on: TOTK_SKIP_OPENING=1 and TOTK_SKIP_DIR=<out_root>/skip_opening (its
    default is C:/totk/overlay/skip_opening/romfs). Same decompressed size, so the RSTB entry stands."""
    raw = romfs.read(OPENING_FLOW)
    flow, dict_id = zstd.decompress(raw)
    size = len(flow)
    patch_skip_opening(flow)
    assert len(flow) == size
    out_dir = Path(out_root) / 'skip_opening'
    _write_set(out_dir, [(OPENING_FLOW, zstd.compress(flow, dict_id), raw)])
    return dict(set='skip_opening', dir=str(out_dir), opening='IntroductionOpening: Event116 -> Event168 (end-of-prologue chain)')


def build_fast_travel(romfs, zstd, out_root, variant='waits'):
    """fasttravel/ (fasttravel-<variant>/ for the others): the shorter fast-travel departure (patch_fast_travel). Same
    decompressed size, so the RSTB entry stands."""
    raw = romfs.read(WARPIN_FLOW)
    flow, dict_id = zstd.decompress(raw)
    size = len(flow)
    changed = patch_fast_travel(flow, variant)
    assert len(flow) == size and changed
    name = 'fasttravel' if variant == 'waits' else f'fasttravel-{variant}'
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(WARPIN_FLOW, zstd.compress(flow, dict_id), raw)])
    return dict(set=name, dir=str(out_dir), changed={k: f'{a} -> {b}' for k, (a, b) in changed.items()})


def build_output(romfs, zstd, out_root, size, scene=None, tex=1.0):
    """output-<w>x<h>[-scene-<w>x<h>][-tex<percent>]/: the HeapDef for a bigger output (and scene), and a bigger
    texture cache (tex > 1; the same set, since two sets may not replace one file)."""
    w, h = parse_size(size)
    sw, sh = parse_size(scene) if scene else (0, 0)
    if not (640 <= w <= 7680 and 360 <= h <= 4320):
        raise ValueError(f'output {size} out of range (640x360 .. 7680x4320)')
    extra = extra_kib(w, h, sw, sh)
    tex_extra = tex_extra_kib(tex)
    if extra <= 0 and tex_extra <= 0:
        raise ValueError(f'{size}: not bigger than 1920x1080 and no texture cache growth, no overlay needed')
    raw = romfs.read(HEAPDEF)
    byml, dict_id = zstd.decompress(raw)
    sizes = patch_heapdef(byml, extra, tex_extra)
    data = zstd.compress(byml, dict_id)
    name = (f'output-{w}x{h}' + (f'-scene-{sw}x{sh}' if sw * sh > SCENE_W * SCENE_H else '') +
            (f'-tex{round(tex * 100)}' if tex_extra else ''))
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(HEAPDEF, data, raw)])
    with open(out_dir / 'overlay.json', 'w', newline='\n', encoding='utf-8') as f:
        json.dump(dict(output=f'{w}x{h}', scene=scene, texture_cache=tex, extra_kib=extra + tex_extra, **sizes), f,
                  indent=2, ensure_ascii=False)
        f.write('\n')
    return dict(set=name, dir=str(out_dir), extra_kib=extra + tex_extra, **sizes)


GRASS_PACK = 'Pack/Grass.pack.zs'
GRASS_DENSITY_KINDS = ('ResVegeSet', 'ResLeavesSet', 'ResStoneSet', 'ResChipsSet')


def build_grass(romfs, zstd, out_root, density):
    """grass-<percent>/: Pack/Grass.pack with every DensityMin / DensityMax of the vegetation, leaves, stone and chips
    sets multiplied by `density` (capped at 1.0, the highest shipped value), edited in place in the archive (sizes and
    layout unchanged; each original float is read from the shipped file). Grass draw distance is code, not data."""
    import struct as _struct
    from . import byml
    if not 0.25 <= density <= 4:
        raise ValueError(f'grass density {density} out of range (0.25 .. 4)')
    raw = romfs.read(GRASS_PACK)
    pack, dict_id = zstd.decompress(raw)
    edits = 0
    for name, (start, end) in sarc_members(bytes(pack)).items():
        if name.split('/')[1] not in GRASS_DENSITY_KINDS:
            continue
        for path, t, value, off in byml.walk(bytes(pack[start:end])):
            if t == byml.FLOAT and path and path[-1] in ('DensityMin', 'DensityMax') and value > 0:
                _struct.pack_into('<f', pack, start + off, min(1.0, value * density))
                edits += 1
    if not edits:
        raise ValueError('grass pack: no density values found (not the 1.0.0 RomFS?)')
    data = zstd.compress(pack, dict_id)
    name = f'grass-{round(density * 100)}'
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(GRASS_PACK, data, raw)])
    return dict(set=name, dir=str(out_dir), density=density, values=edits)


SHADER_PACKAGE = 'Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs'
SHADOW_CONF = 'gsys.bgmsconf'


def build_shadow(romfs, zstd, out_root, distance):
    """shadow-<percent>/: the shader package's gsys.bgmsconf with the cascade split distances of every cascaded depth
    shadow (depth_shadow_cascade_num_0 >= 2: the Main scene's 3 cascades, splits 32 / 64 / 200 m from 2 m, far 3000 m)
    multiplied by `distance`, so the sharp near cascades reach further out (each covers more ground with the same map:
    pair with the shadowres patch). The first cascade's start and the far distance stay; splits are capped below far.
    Edited in place (sizes and layout unchanged); a value slot another parameter shares is refused."""
    import struct as _struct
    from . import aamp
    if not 0.5 <= distance <= 4:
        raise ValueError(f'shadow distance {distance} out of range (0.5 .. 4)')
    raw = romfs.read(SHADER_PACKAGE)
    pack, dict_id = zstd.decompress(raw)
    start, end = sarc_members(bytes(pack))[SHADOW_CONF]
    conf = bytes(pack[start:end])
    num, far = aamp.name_hash('depth_shadow_cascade_num_0'), aamp.name_hash('depth_shadow_far_0')
    splits = {aamp.name_hash(f'depth_shadow_near_0_{j}'): j for j in range(1, 4)}
    objects = {}
    for path, h, t, v, off in aamp.walk(conf):
        objects.setdefault(path, {})[h] = (t, v, off)
    edits = []
    for params in objects.values():
        if num not in params or params[num][1] < 2 or far not in params:
            continue
        limit = params[far][1]
        for h, j in splits.items():
            if h not in params:
                continue
            t, v, off = params[h]
            if t != aamp.F32 or not 0 < v < limit:
                raise ValueError(f'{SHADOW_CONF}: depth_shadow_near_0_{j} = {v} (expected a float below far {limit})')
            if aamp.shared(conf, off) != 1:
                raise ValueError(f'{SHADOW_CONF}: depth_shadow_near_0_{j} shares its value slot')
            edits.append((off, min(v * distance, limit * 0.95)))
    if not edits:
        raise ValueError(f'{SHADOW_CONF}: no cascaded depth shadow found (not the 1.0.0 RomFS?)')
    for off, v in edits:
        _struct.pack_into('<f', pack, start + off, v)
    data = zstd.compress(pack, dict_id)
    name = f'shadow-{round(distance * 100)}'
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(SHADER_PACKAGE, data, raw)])
    return dict(set=name, dir=str(out_dir), distance=distance, values=len(edits))


ENV_PALETTE_PACK = 'Pack/EnvPalette.pack.zs'
FOG_KEYS = ('FogStart', 'FogEnd')


def build_fog(romfs, zstd, out_root, distance):
    """fog-<percent>/: Pack/EnvPalette.pack with the distance fog of every world palette (game::wm::ResEnvPalette
    FogStart / FogEnd: weather, area and time of day; the agl env defaults in Env/*.genvb are overridden by them)
    multiplied by `distance`, so the haze begins and saturates k times further (1.0.0: 91 starts, -200..20 m, and 89
    ends, 12..5000 m). Height fog (YFog*), the sky's scattering, caves (CavePalette) and cutscenes (DemoPalette) stay."""
    import struct as _struct
    from . import byml
    if not 0.5 <= distance <= 4:
        raise ValueError(f'fog distance {distance} out of range (0.5 .. 4)')
    raw = romfs.read(ENV_PALETTE_PACK)
    pack, dict_id = zstd.decompress(raw)
    edits = {k: 0 for k in FOG_KEYS}
    for name, (start, end) in sarc_members(bytes(pack)).items():
        if not name.endswith('.game__wm__ResEnvPalette.bgyml'):
            continue
        for path, t, value, off in byml.walk(bytes(pack[start:end])):
            if t == byml.FLOAT and len(path) == 1 and path[0] in FOG_KEYS:
                _struct.pack_into('<f', pack, start + off, value * distance)
                edits[path[0]] += 1
    if not all(edits.values()):
        raise ValueError(f'env palettes: fog distances not found ({edits}; not the 1.0.0 RomFS?)')
    data = zstd.compress(pack, dict_id)
    name = f'fog-{round(distance * 100)}'
    out_dir = Path(out_root) / name
    _write_set(out_dir, [(ENV_PALETTE_PACK, data, raw)])
    return dict(set=name, dir=str(out_dir), distance=distance, **{k.lower(): v for k, v in edits.items()})
