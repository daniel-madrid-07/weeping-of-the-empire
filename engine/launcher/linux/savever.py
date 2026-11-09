"""Saves of later game versions into 1.0.0, for the Linux launcher's import: engine/launcher/win/savever.c in Python 3.

A port of that file (this repository's code, MIT), step for step: see savever.h beside it for the format and why each
step is there. No UI: launcher_linux.py words the report, test_savever.py checks this against synthetic saves.

The tables are not copied: they are read from savever_data.h, the generated file the Windows launcher compiles, found
beside this file (a game folder: package.py copies it there) or in ../win (a clone), so both launchers convert with the
same tables. The standard library only, except for the caption picture: a 1.4.x picture too big for 1.0.0's room is
scaled and re-encoded with Pillow (WIC on Windows), imported only then; without Pillow such a save is refused
(SV_PICTURE) and nothing changes. 1.1.x and 1.2.x pictures already fit.
"""
import io
import os
import re
from dataclasses import dataclass

MAGIC = 0x01020304
TABLE = 0x20
GROUPS = 33
G_ENUM, G_ENUM_ARRAY, G_BINARY, G_BOOL64 = 6, 7, 18, 32
# (savever.c's REV_11X, not in the generated header: 1.1.x and 1.2.x share one GameDataList revision)
REV_11X = 4710644
PIC_W, PIC_H = 256, 144   # (savever.c: the 1.0.0 caption JPEG)
SV_SAME, SV_CONVERTED = 0, 1
SV_BAD_FILE, SV_UNKNOWN_VERSION, SV_UNKNOWN_KEYS, SV_TOO_BIG, SV_PICTURE, SV_IO = -1, -2, -3, -4, -5, -6
MAX_FILE = 64 << 20   # (savever.c: bigger is not a save file)


@dataclass
class FileSpec:   # (SaveFileSpec: one save file of 1.0.0)
    sfi: int
    name: str
    size: int
    count: int
    digest: int
    drop: frozenset   # {(group, hash)}: the keys later versions added to it


@dataclass
class EnumMap:   # (SaveEnumMap: an Enum / EnumArray key whose later versions added values, which become default)
    sfi: int
    group: int
    key: int
    default: int
    added: frozenset


@dataclass
class Tables:   # (SaveTables)
    target: int        # the revision written: 1.0.0's
    later: tuple       # the revisions converted from
    files: list
    drop_b64: frozenset    # Bool64bitKey flags later versions added (progress.sav)
    enums: list
    bins: dict         # (sfi, key) -> the room bytes of a Binary key in 1.0.0 (the caption picture)
    pic_w: int = PIC_W
    pic_h: int = PIC_H
    latest: int = 0    # SAVEVER_REV_LATEST: the newest list the tables cover (1.4.x)


@dataclass
class Report:   # (SaveConvReport)
    from_rev: int = 0       # the newest revision converted from (0: nothing converted)
    files: int = 0          # files rewritten
    keys_dropped: int = 0   # table keys 1.0.0 does not have
    flags_dropped: int = 0  # Bool64bitKey flags 1.0.0 does not have
    enums_reset: int = 0    # enum values 1.0.0 does not have, set to the default
    pictures_fitted: int = 0    # captions re-encoded smaller
    pictures_blank: int = 0     # captions that could not be decoded, replaced with a black picture
    option_dropped: int = 0     # 1: option.sav could not be converted and was left out (the game makes a new one)
    refused: int = 0            # SV_* < 0 when the save was refused, else 0
    detail: int = 0             # SV_UNKNOWN_VERSION: the revision; SV_UNKNOWN_KEYS: the keys left over 1.0.0's count
    file: str = ''              # the refused file, as slot_0N/progress.sav


def _num(s):
    return int(s.rstrip('ulUL'), 0)


def load_tables(path=None):
    """savever_data.h's tables (tools/saves/savever_gen.py writes it). Each array is checked against the count the
    header gives for it: a header this parser misreads raises ValueError instead of converting with wrong tables."""
    if path is None:
        here = os.path.dirname(os.path.abspath(__file__))
        near = (os.path.join(here, 'savever_data.h'), os.path.join(here, '..', 'win', 'savever_data.h'))
        path = next((p for p in near if os.path.isfile(p)), None)
        if path is None:
            raise FileNotFoundError(f'savever_data.h (beside {here}, or in ../win)')
    with open(path, encoding='utf-8') as f:
        src = re.sub(r'/\*.*?\*/', '', f.read(), flags=re.S)

    def define(name):
        m = re.search(r'#define\s+%s\s+(\w+)' % name, src)
        if not m:
            raise ValueError(f'{path}: no {name}')
        return _num(m.group(1))

    def array(name):
        m = re.search(r'\b%s\[\]\s*=\s*\{(.*?)\};' % re.escape(name), src, re.S)
        if not m:
            raise ValueError(f'{path}: no {name}[]')
        return m.group(1)

    def counted(name, got, want):
        if got != want:
            raise ValueError(f'{path}: {name} has {got} entries, the header says {want}')

    files = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*L"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,'
                         r'\s*(0x[0-9a-fA-F]+)ull\s*,\s*(\w+)\s*,\s*(\d+)\s*\}', array('file_specs')):
        sfi, name, size, count, digest, drop, nd = m.groups()
        pairs = re.findall(r'\{\s*(\w+)\s*,\s*(\w+)\s*\}', array(drop))
        d = frozenset((_num(a), _num(b)) for a, b in pairs[:int(nd)])   # ({0, 0} alone: an empty list's placeholder)
        counted(drop, len(d), int(nd))
        files.append(FileSpec(int(sfi), name, int(size), int(count), int(digest, 16), d))
    enums = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+)u\s*,\s*(0x[0-9a-fA-F]+)u\s*,\s*(\w+)\s*,'
                         r'\s*(\d+)\s*\}', array('enum_maps')):
        sfi, g, key, dflt, added, na = m.groups()
        values = frozenset(_num(x) for x in re.findall(r'0x[0-9a-fA-F]+u?', array(added)))
        counted(added, len(values), int(na))
        enums.append(EnumMap(int(sfi), int(g), _num(key), _num(dflt), values))
    bins = {(int(a), _num(b)): int(c) for a, b, c in
            re.findall(r'\{\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+)u\s*,\s*(\d+)\s*\}', array('bin_rooms'))}
    b64 = frozenset(_num(x) for x in re.findall(r'0x[0-9a-fA-F]+ull', array('drop_b64')))
    for name, got in (('file_specs', len(files)), ('enum_maps', len(enums)), ('bin_rooms', len(bins)),
                      ('drop_b64', len(b64))):
        counted(name, got, len(re.findall(r'\{' if name != 'drop_b64' else r'0x', array(name))))
    latest = define('SAVEVER_REV_LATEST')
    return Tables(define('SAVEVER_REV_100'), (REV_11X, latest), files, b64, enums, bins, latest=latest)


_T = None


def tables():
    """the real tables, read once"""
    global _T
    if _T is None:
        _T = load_tables()
    return _T


def revision_name(rev, t=None):
    """"1.0.0", "1.1.x / 1.2.x", "1.4.x"; None for a revision the tables do not know (savever_name)"""
    t = t or tables()
    return {t.target: '1.0.0', REV_11X: '1.1.x / 1.2.x', t.latest: '1.4.x'}.get(rev)


def _rd32(b, o=0):
    return int.from_bytes(b[o:o + 4], 'little')


def _wr32(b, o, v):
    b[o:o + 4] = (v & 0xFFFFFFFF).to_bytes(4, 'little')


def _inline_group(g):
    return g in (0, 2, 4, 6, 20)


def file_revision(path):
    """the revision word of a .sav file (0: unreadable or not a save)"""
    try:
        with open(path, 'rb') as f:
            b = f.read(8)
    except OSError:
        return 0
    return _rd32(b, 4) if len(b) == 8 and _rd32(b) == MAGIC else 0


def dir_revision(d):
    """the highest revision among <d>/slot_0N/progress.sav (0: none)"""
    return max(file_revision(os.path.join(d, 'slot_%02d' % i, 'progress.sav')) for i in range(6))


def key_digest(keys):
    """FNV-1a 64 over the sorted (group << 32 | hash) values, little-endian (savever_gen.py: digest)"""
    h = 0xcbf29ce484222325
    for k in sorted(keys):
        for b in k.to_bytes(8, 'little'):
            h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class _Refuse(Exception):
    def __init__(self, why, detail=0):
        super().__init__(why)
        self.why, self.detail = why, detail


def convert(t, f, data, r):
    """One file (savever_convert). (SV_SAME, None): already 1.0.0, or not a game save file (as it was: the game
    decides); (SV_CONVERTED, f.size bytes); (SV_* < 0, None) with r.refused and r.detail set."""
    n = len(data)
    if n < 8 or _rd32(data) != MAGIC:
        return SV_SAME, None
    rev = _rd32(data, 4)
    if rev == t.target:
        return SV_SAME, None
    try:
        if rev not in t.later:
            raise _Refuse(SV_UNKNOWN_VERSION, rev)
        out = _convert(t, f, data, rev, r)
    except _Refuse as e:
        r.refused, r.detail = e.why, e.detail
        return e.why, None
    r.from_rev = max(r.from_rev, rev)
    r.files += 1
    return SV_CONVERTED, out


def _convert(t, f, data, rev, r):
    n = len(data)
    start = _rd32(data, 8) if n >= 12 else 0
    bad = _Refuse(SV_BAD_FILE)
    if n < TABLE + 8 or n > MAX_FILE or start < TABLE + 8 or start > n or (start - TABLE) % 8:
        raise bad

    # the table: (0, 0) first, then the markers of groups 1..32 in order
    ent = []      # [group, hash, value]
    blocks = []   # [offset, length, entry]: a data block, the table entry pointing at it
    group = -1
    for i in range((start - TABLE) // 8):
        h, v = _rd32(data, TABLE + 8 * i), _rd32(data, TABLE + 8 * i + 4)
        if not h:
            if v != group + 1:
                raise bad   # every group, in order (all 33 in every file)
            group = v
        elif group < 0:
            raise bad
        ent.append([group, h, v])
        if h and not _inline_group(group) and v:   # 0: no block (an empty Bool64bitKey list)
            if v < start or v >= n:
                raise bad
            blocks.append([v, 0, i])
    if group != GROUPS - 1:
        raise bad
    blocks.sort(key=lambda b: b[0])
    for i, b in enumerate(blocks):
        end = blocks[i + 1][0] if i + 1 < len(blocks) else n
        if end <= b[0]:
            raise bad   # two keys on one block
        b[1] = end - b[0]

    # what stays, with the added enum values reset
    enum_by = {(m.sfi, m.group, m.key): m for m in t.enums}
    keys = []
    for e in ent:
        if not e[1]:
            continue
        if (e[0], e[1]) in f.drop:
            r.keys_dropped += 1
            e[0] = GROUPS   # (dropped)
            continue
        keys.append(e[0] << 32 | e[1])
        m = enum_by.get((f.sfi, G_ENUM, e[1])) if e[0] == G_ENUM else None
        if m and e[2] in m.added:
            e[2] = m.default
            r.enums_reset += 1
    if len(keys) != f.count or key_digest(keys) != f.digest:
        raise _Refuse(SV_UNKNOWN_KEYS, len(keys) - f.count if len(keys) > f.count else 0)

    # the data blocks, in their order; the table shrinks by the dropped keys
    head = TABLE + 8 * sum(1 for e in ent if e[0] != GROUPS)
    out = bytearray(head)
    newoff = {}
    for off, ln, ei in blocks:
        x = ent[ei]
        if x[0] == GROUPS:
            continue
        src = data[off:off + ln]
        newoff[ei] = len(out)
        m = enum_by.get((f.sfi, G_ENUM_ARRAY, x[1])) if x[0] == G_ENUM_ARRAY else None
        room = t.bins.get((f.sfi, x[1])) if x[0] == G_BINARY else None
        if x[0] == G_BOOL64:
            # the set flags up to a zero key; what lies past it is stale (TotkMods/Research, Formats/Sav)
            q = 0
            while q + 8 <= ln and any(src[q:q + 8]):
                if f.sfi == 0 and int.from_bytes(src[q:q + 8], 'little') in t.drop_b64:
                    r.flags_dropped += 1
                else:
                    out += src[q:q + 8]
                q += 8
            if q + 8 > ln:
                raise bad   # no end
            out += bytes(8)
        elif m:
            c = _rd32(src) if ln >= 4 else 0
            if ln < 4 or (ln - 4) // 4 < c:
                raise bad
            blk = bytearray(src)
            for j in range(c):
                if _rd32(src, 4 + 4 * j) in m.added:
                    _wr32(blk, 4 + 4 * j, m.default)
                    r.enums_reset += 1
            out += blk
        elif room is not None:
            sz = _rd32(src) if ln >= 4 else 0
            if ln < 4 or ln - 4 < sz:
                raise bad
            if sz <= room:
                pic = src[4:4 + sz]
            else:
                pic = fit_jpeg(bytes(src[4:4 + sz]), t.pic_w, t.pic_h, room)
                if pic is not None:
                    r.pictures_fitted += 1
                else:
                    pic = fit_jpeg(None, t.pic_w, t.pic_h, room)
                    if pic is None:
                        raise _Refuse(SV_PICTURE)
                    r.pictures_blank += 1
            out += len(pic).to_bytes(4, 'little') + pic + bytes(room - len(pic))   # 1.0.0 keeps the whole room
        else:
            out += src

    # 1.0.0's size: the zero tail (the last block runs to the end of the file) may shrink, nothing else
    out_len = len(out)
    while out_len > f.size and out_len > head and not out[out_len - 1]:
        out_len -= 1
    if out_len > f.size:
        raise _Refuse(SV_TOO_BIG)
    res = bytearray(f.size)
    res[head:out_len] = out[head:out_len]
    _wr32(res, 0, MAGIC)
    _wr32(res, 4, t.target)
    _wr32(res, 8, head)
    row = 0
    for i, e in enumerate(ent):
        if e[0] == GROUPS:
            continue
        _wr32(res, TABLE + 8 * row, e[1])
        _wr32(res, TABLE + 8 * row + 4, newoff[i] if e[1] and not _inline_group(e[0]) and e[2] else e[2])
        row += 1
    return bytes(res)


def _read(p):
    """a file's bytes; None when it cannot be read or is too big to be a save (savever.c read_all)"""
    try:
        if os.path.getsize(p) > MAX_FILE:
            return None
        with open(p, 'rb') as fh:
            return fh.read()
    except OSError:
        return None


def convert_dir(t, d, r):
    """Every file of a save folder (slot_0N/*.sav, option.sav), in place; all files or none (each is written through a
    .tmp). SV_SAME, SV_CONVERTED or SV_* < 0 (nothing changed). An option.sav that cannot be converted is deleted
    instead (r.option_dropped): the game writes its default settings (savever_convert_dir)."""
    jobs = []   # (path, bytes; None: delete)
    for s in range(-1, 6):   # -1: option.sav, beside the slots
        for f in t.files:
            if (s < 0) != (f.sfi == 1) or len(jobs) >= 64:
                continue
            rel = f.name if s < 0 else 'slot_%02d/%s' % (s, f.name)
            p = os.path.join(d, rel)
            if not os.path.lexists(p):
                continue
            one = Report()
            raw = _read(p)
            if raw is None:
                cv, out = SV_BAD_FILE, None
                one.refused = SV_BAD_FILE
            else:
                cv, out = convert(t, f, raw, one)
            r.keys_dropped += one.keys_dropped
            r.flags_dropped += one.flags_dropped
            r.enums_reset += one.enums_reset
            r.pictures_fitted += one.pictures_fitted
            r.pictures_blank += one.pictures_blank
            if cv == SV_SAME:
                continue
            if cv < 0 and f.sfi == 1 and cv != SV_IO:   # settings: leave them out rather than refuse the save
                jobs.append((p, None))
            elif cv < 0:
                r.refused, r.detail, r.file = one.refused, one.detail, rel
                return cv
            else:
                r.from_rev = max(r.from_rev, one.from_rev)
                r.files += 1
                jobs.append((p, out))
    rc = SV_SAME
    for p, out in jobs:
        if out is None:
            try:
                os.remove(p)
            except OSError:
                r.refused = SV_IO
                return SV_IO
            r.option_dropped = 1
            continue
        tmp = p + '.tmp'
        try:
            with open(tmp, 'wb') as fh:
                fh.write(out)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, p)
        except OSError:
            try:
                os.remove(tmp)
            except OSError:
                pass
            r.refused, r.file = SV_IO, os.path.relpath(p, d)
            return SV_IO
        rc = SV_CONVERTED
    return rc


# ---- the picture -----------------------------------------------------------------------------------------------------

def _pil():
    """Pillow's Image module, or None: it is not in the standard library (the AppImage's own Python has none; Setup's
    copy of it in <install>/tools/python gets it with the build's packages)"""
    try:
        from PIL import Image
        return Image
    except ImportError:
        return None


def have_pillow():
    return _pil() is not None


def fit_jpeg(jpg, w, h, room):
    """A JPEG of the picture scaled to w x h, re-encoded at quality 92, 84, ... 20 until it fits in room bytes
    (savever_fit_jpeg); jpg None: a black picture. None when it cannot be done: no Pillow, or a picture it can't
    read."""
    Image = _pil()
    if Image is None:
        return None
    try:
        if jpg is None:
            im = Image.new('RGB', (w, h))
        else:
            im = Image.open(io.BytesIO(jpg))
            im.draft('RGB', (w * 2, h * 2))   # (a JPEG decoded at a smaller scale: faster, the result is scaled anyway)
            im = im.convert('RGB')
        # (WIC's Fant: an area average going down; Pillow's BOX is one, LANCZOS for the rare picture smaller than w x h)
        rs = getattr(Image, 'Resampling', Image)
        im = im.resize((w, h), rs.LANCZOS if im.width < w or im.height < h else rs.BOX)
        for q in range(92, 19, -8):
            buf = io.BytesIO()
            im.save(buf, 'JPEG', quality=q)
            if buf.tell() <= room:
                return buf.getvalue()
    except Exception:   # (any file Pillow cannot read: OSError, SyntaxError, ValueError... as WIC's failed HRESULT)
        return None
    return None
