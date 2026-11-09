"""savever.py on synthetic saves (no game files): test_savever.c's cases one for one (a later-version file down to
1.0.0's layout, each refusal, the caption picture, a folder all or nothing), the real tables read from savever_data.h,
and this port's own constants against savever.c / savever.h. Seconds. The picture cases need Pillow (Setup's
<install>/tools/python has it) and say SKIP without it; the refusal without Pillow is checked either way.

    python3 -I engine/launcher/linux/test_savever.py                 the tests
    python3 -I engine/launcher/linux/test_savever.py convert <dir>   convert a save folder in place with the real tables
                                                                     and print the report (test_savever.exe convert)
"""
import io
import os
import random
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.path.join(os.path.dirname(HERE), 'win')
sys.path.insert(0, HERE)
import savever as sv  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + ('' if ok else f' {detail}'), flush=True)
    if not ok:
        fails.append(name)


# ---- synthetic tables (test_savever.c's)

REV_OLD, REV_NEW, FSIZE, ROOM, PIC_W, PIC_H = 100, 200, 4096, 1000, 16, 9
DROP64 = 0x0bad0000000064
LATER_ENUM = frozenset((0x6f, 0x7f))
FILES = [
    # 1.0.0 keys: Bool 0x11 0x12, Int 0x21, Enum 0x61, EnumArray 0x71, Binary 0x181, Bool64bitKey 0x320 (savever_gen.py
    # digest: 0x6c6d471dd086cd69)
    sv.FileSpec(0, 'progress.sav', FSIZE, 7, 0x6c6d471dd086cd69, frozenset({(0, 0x13), (1, 0x1a), (22, 0x2a)})),
    sv.FileSpec(1, 'option.sav', 512, 2, 0x98f778fadd8596c5, frozenset({(0, 0x14)})),   # Bool 0x11, Bool64bitKey 0x320
]
T = sv.Tables(REV_OLD, (REV_NEW,), FILES, frozenset({DROP64}),
              [sv.EnumMap(0, 6, 0x61, 0x60, LATER_ENUM), sv.EnumMap(0, 7, 0x71, 0x70, LATER_ENUM)], {(0, 0x181): ROOM},
              PIC_W, PIC_H, latest=REV_NEW)


# ---- synthetic saves

def u32(*v):
    return b''.join((x & 0xFFFFFFFF).to_bytes(4, 'little') for x in v)


def build(size, rev, items):
    """A save of size bytes from items (group, hash, value, block or None): the table (every group's marker, the items
    in their group), then the items' blocks in item order."""
    buf = bytearray(size)
    start = 0x20 + 8 * (33 + len(items))
    pos, offs = start, []
    for g, h, v, blob in items:
        offs.append(pos if blob is not None else v)
        if blob is not None:
            buf[pos:pos + len(blob)] = blob
            pos += len(blob)
    row = 0
    for g in range(33):
        buf[0x20 + 8 * row:0x28 + 8 * row] = u32(0, g)
        row += 1
        for i, it in enumerate(items):
            if it[0] == g:
                buf[0x20 + 8 * row:0x28 + 8 * row] = u32(it[1], offs[i])
                row += 1
    buf[0:12] = u32(sv.MAGIC, rev, start)
    return bytes(buf)


def find(s, group, h):
    """the value (or block offset) of (group, hash) in a save; -1 when absent"""
    start, g = sv._rd32(s, 8), 0
    for o in range(0x20, start, 8):
        k, v = sv._rd32(s, o), sv._rd32(s, o + 4)
        if not k:
            g = v
        elif g == group and k == h:
            return v
    return -1


BARR = u32(30) + b'\xff\xff\xff\x3f'   # BoolArray of 30
EARR = u32(3, 0x70, 0x7f, 0x72)        # EnumArray: 0x70 0x7f 0x72
I64 = bytes(range(1, 9))
BIN = u32(40) + bytes(range(1, 41))
B64 = b''.join(k.to_bytes(8, 'little') for k in (0x101, DROP64, 0x102, 0, 0x103))   # (later), end, a stale key


def later_save(size=FSIZE + 1024, extra_key=False, rev=REV_NEW):
    items = [(0, 0x11, 1, None), (0, 0x13, 1, None), (0, 0x12, 0, None), (1, 0x1a, 0, BARR),
             (2, 0x21, 0xfffffff6, None), (6, 0x61, 0x6f, None), (7, 0x71, 0, EARR),
             (22, 0x2a, 0, I64), (18, 0x181, 0, BIN), (32, 0x320, 0, B64), (0, 0x55, 1, None)]
    return build(size, rev, items if extra_key else items[:-1])


def conv(f, data, t=T):
    r = sv.Report()
    rc, out = sv.convert(t, f, data, r)
    return rc, out, r


def test_convert():
    rc, out, r = conv(FILES[0], later_save())
    check('convert: done', rc == sv.SV_CONVERTED and sv._rd32(out) == sv.MAGIC and sv._rd32(out, 4) == REV_OLD, rc)
    check('convert: the report', (r.keys_dropped, r.flags_dropped, r.enums_reset, r.from_rev, r.files) ==
          (3, 1, 2, REV_NEW, 1), r)
    if rc != sv.SV_CONVERTED:
        return
    check('convert: the table lost the 3 dropped rows', sv._rd32(out, 8) == 0x20 + 8 * (33 + 7))
    check('convert: kept and dropped keys', (find(out, 0, 0x11), find(out, 0, 0x12), find(out, 0, 0x13),
          find(out, 1, 0x1a), find(out, 2, 0x21), find(out, 22, 0x2a)) == (1, 0, -1, -1, 0xfffffff6, -1))
    check('convert: a later enum value is the default', find(out, 6, 0x61) == 0x60)
    ea, b, b64 = find(out, 7, 0x71), find(out, 18, 0x181), find(out, 32, 0x320)
    check('convert: the blocks follow the table without gaps, in their order', (ea, b, b64) ==
          (sv._rd32(out, 8), ea + 16, ea + 16 + 4 + ROOM))
    check('convert: EnumArray later value reset', [sv._rd32(out, ea + 4 * i) for i in range(4)] ==
          [3, 0x70, 0x70, 0x72])
    check('convert: Binary within its room, the room kept', sv._rd32(out, b) == 40 and out[b + 4:b + 44] == BIN[4:] and
          not any(out[b + 44:b + 4 + ROOM]))
    check('convert: the later flag dropped, the stale key past the end gone',
          [sv._rd32(out, b64 + 8 * i) for i in range(4)] == [0x101, 0x102, 0, 0] and not any(out[b64 + 24:]))
    check('convert: 1.0.0\'s size', len(out) == FSIZE)
    check('convert: the result again is 1.0.0', conv(FILES[0], out)[0] == sv.SV_SAME)
    # option.sav: a dropped key, and a Bool64bitKey at offset 0 (the file has no list) stays 0
    rc, oout, r2 = conv(FILES[1], build(700, REV_NEW, [(0, 0x11, 1, None), (0, 0x14, 1, None), (32, 0x320, 0, None)]))
    check('convert option.sav: a dropped key, an empty flag list', rc == sv.SV_CONVERTED and r2.keys_dropped == 1 and
          (find(oout, 0, 0x11), find(oout, 0, 0x14), find(oout, 32, 0x320), sv._rd32(oout, 8), len(oout)) ==
          (1, -1, 0, 0x20 + 8 * 35, 512), r2)


def test_refusals():
    rc, _, r = conv(FILES[0], later_save(extra_key=True))
    check('refused: a key neither 1.0.0 nor the tables know', (rc, r.refused, r.detail) ==
          (sv.SV_UNKNOWN_KEYS, sv.SV_UNKNOWN_KEYS, 1), r)
    rc, _, r = conv(FILES[0], later_save(rev=300))
    check('refused: an unknown revision', (rc, r.detail) == (sv.SV_UNKNOWN_VERSION, 300), r)
    check('not a save at all: left alone', conv(FILES[0], b'hello world, not a save')[0] == sv.SV_SAME)
    good = later_save()
    start, ea, b64 = sv._rd32(good, 8), find(good, 7, 0x71), find(good, 32, 0x320)

    def damaged(edit):
        d = bytearray(good)
        edit(d)
        return conv(FILES[0], bytes(d))[0]

    def rows(d, test, value):
        for o in range(0x20, start, 8):
            if test(sv._rd32(d, o), sv._rd32(d, o + 4)):
                sv._wr32(d, o + 4, value)
    check('refused: group markers out of order', damaged(lambda d: rows(d, lambda k, v: not k and v == 2, 1)) ==
          sv.SV_BAD_FILE)
    check('refused: a block past the end', damaged(lambda d: rows(d, lambda k, v: k == 0x71, len(good))) ==
          sv.SV_BAD_FILE)
    check('refused: two keys on one block', damaged(lambda d: rows(d, lambda k, v: k == 0x181, ea)) == sv.SV_BAD_FILE)

    def no_end(d):
        d[b64:] = b'\xab' * (len(d) - b64)   # flags to the end of the file, no zero key
    check('refused: a flag list without its end', damaged(no_end) == sv.SV_BAD_FILE)

    def too_long(d):
        d[b64 + 8:len(d) - 16] = b'\x11' * (len(d) - 16 - b64 - 8)   # a flag list longer than the 1.0.0 file
    check('refused: more than 1.0.0\'s file has room for', damaged(too_long) == sv.SV_TOO_BIG)


def noisy_jpeg(Image, w, h):
    rnd = random.Random(1)
    buf = io.BytesIO()
    Image.frombytes('RGB', (w, h), bytes(rnd.randrange(256) for _ in range(w * h * 3))).save(buf, 'JPEG', quality=90)
    return buf.getvalue()


def picture_save(blob):
    items = [(0, 0x11, 1, None), (0, 0x12, 0, None), (2, 0x21, 0, None), (6, 0x61, 0x60, None), (7, 0x71, 0, EARR),
             (18, 0x181, 0, u32(len(blob)) + blob), (32, 0x320, 0, bytes(8))]
    return build(FSIZE + len(blob) + 8, REV_NEW, items)


def test_picture():
    """a picture over the room re-encoded at PIC_W x PIC_H to fit; one Pillow cannot read becomes black; without Pillow
    the file is refused (SV_PICTURE), as WIC's failure on Windows"""
    real = sv._pil
    sv._pil = lambda: None
    try:
        rc, _, r = conv(FILES[0], picture_save(b'\xff\xd8' + bytes(ROOM + 500)))
        check('picture without Pillow: refused, nothing guessed', (rc, r.refused, sv.have_pillow()) ==
              (sv.SV_PICTURE, sv.SV_PICTURE, False), r)
    finally:
        sv._pil = real
    Image = sv._pil()
    if Image is None:
        print('SKIP picture re-encoding (no Pillow in this Python; Setup\'s <install>/tools/python/bin/python3 has it)')
        return
    big = noisy_jpeg(Image, 320, 180)
    fitted = sv.fit_jpeg(big, PIC_W, PIC_H, ROOM)
    check('picture: a big JPEG fitted into the room', len(big) > ROOM and fitted is not None and
          0 < len(fitted) <= ROOM and fitted[:2] == b'\xff\xd8' and
          Image.open(io.BytesIO(fitted)).size == (PIC_W, PIC_H))
    black = sv.fit_jpeg(None, 1920, 1080, 1 << 16)
    check('picture: a black one', black is not None and black[:2] == b'\xff\xd8')
    rc, out, r = conv(FILES[0], picture_save(big))
    b = find(out, 18, 0x181) if out else -1
    check('picture in a save: fitted', rc == sv.SV_CONVERTED and r.pictures_fitted == 1 and b > 0 and
          sv._rd32(out, b) <= ROOM and out[b + 4:b + 6] == b'\xff\xd8', r)
    rc, out, r = conv(FILES[0], picture_save(b'\x5a' * len(big)))   # garbage where the JPEG should be
    check('picture in a save: unreadable, black', rc == sv.SV_CONVERTED and (r.pictures_blank, r.pictures_fitted) ==
          (1, 0), r)


def test_dir(root):
    a, b = later_save(), later_save(extra_key=True)   # slot_01: a key nobody knows
    opt = build(600, REV_NEW, [(0, 0x11, 1, None), (0, 0x14, 1, None), (0, 0x99, 1, None), (32, 0x320, 0, bytes(8))])

    def put(rel, data):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)

    def get(rel):
        try:
            with open(os.path.join(root, rel), 'rb') as f:
                return f.read()
        except OSError:
            return None
    put('slot_00/progress.sav', a)
    put('slot_01/progress.sav', b)
    put('option.sav', opt)   # (a key nobody knows: left out, not refused)
    put('slot_00/notes.txt', b'x')
    r = sv.Report()
    check('folder: one refused file refuses all', sv.convert_dir(T, root, r) == sv.SV_UNKNOWN_KEYS and
          r.file == 'slot_01/progress.sav', r)
    check('folder: none written', get('slot_00/progress.sav') == a and get('option.sav') == opt and
          not [n for n in os.listdir(os.path.join(root, 'slot_00')) if n.endswith('.tmp')])
    check('folder: its revision', sv.dir_revision(root) == REV_NEW)
    put('slot_01/progress.sav', a)
    r = sv.Report()
    check('folder: converted, option.sav left out', sv.convert_dir(T, root, r) == sv.SV_CONVERTED and
          (r.files, r.option_dropped, r.refused, r.from_rev) == (2, 1, 0, REV_NEW), r)
    s1 = get('slot_01/progress.sav') or b''
    check('folder: 1.0.0 files', len(s1) == FSIZE and sv._rd32(s1, 4) == REV_OLD and get('option.sav') is None and
          sv.dir_revision(root) == REV_OLD and get('slot_00/notes.txt') == b'x')
    r = sv.Report()
    check('folder: again, nothing to do', sv.convert_dir(T, root, r) == sv.SV_SAME and r.files == 0, r)
    # a file that cannot be written (its folder read-only): SV_IO, that file named, the original kept, no .tmp left
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        print('SKIP a file that cannot be written (root writes in a read-only folder)')
        return
    put('ro/slot_00/progress.sav', a)
    ro = os.path.join(root, 'ro', 'slot_00')
    os.chmod(ro, 0o555)
    r = sv.Report()
    try:
        rc = sv.convert_dir(T, os.path.join(root, 'ro'), r)
        left = sorted(os.listdir(ro))
    finally:
        os.chmod(ro, 0o755)
    check('folder: a file that cannot be written', (rc, r.refused, r.file, left, get('ro/slot_00/progress.sav') == a) ==
          (sv.SV_IO, sv.SV_IO, 'slot_00/progress.sav', ['progress.sav'], True), (rc, r, left))


def test_real_tables():
    t = sv.tables()
    check('real tables: the files', [(f.sfi, f.name, len(f.drop), f.count) for f in t.files][:2] ==
          [(0, 'progress.sav', 29, 30693), (1, 'option.sav', 9, 33)] and len(t.files) == 5 and
          t.files[0].size == 2307552)
    check('real tables: flags, enums, the picture room', (len(t.drop_b64), [len(e.added) for e in t.enums], t.bins) ==
          (6, [5, 5], {(2, 0x63696a32): 10240}))
    check('real tables: the revisions have names', sv.revision_name(t.target) == '1.0.0' and
          all(sv.revision_name(rev) for rev in t.later) and sv.revision_name(12345) is None)
    # this port's own constants are savever.c's / savever.h's (a clone has them; a game folder has only the header)
    c_src = os.path.join(WIN, 'savever.c')
    if os.path.isfile(c_src):
        with open(c_src, encoding='utf-8') as f:
            c = f.read()
        with open(os.path.join(WIN, 'savever.h'), encoding='utf-8') as f:
            h = f.read()
        rev = re.search(r'#define\s+REV_11X\s+(\d+)u', c)
        pic = re.search(r'(\d+),\s*(\d+),\s*/\*\s*the 1\.0\.0 caption JPEG', c)
        codes = {k: int(v) for k, v in re.findall(r'\b(SV_[A-Z_]+)\s*=\s*(-?\d+)', h)}
        check('savever.c: REV_11X and the picture size', (int(rev.group(1)) if rev else None,
              tuple(map(int, pic.groups())) if pic else None) == (sv.REV_11X, (sv.PIC_W, sv.PIC_H)))
        check('savever.h: the SV_ codes', codes == {k: getattr(sv, k) for k in codes} and len(codes) == 8, codes)
    else:
        print('SKIP savever.c / savever.h: not beside this folder')
    # a header this parser misreads is refused, not half read
    with open(os.path.join(WIN, 'savever_data.h') if os.path.isfile(os.path.join(WIN, 'savever_data.h')) else
              os.path.join(HERE, 'savever_data.h'), encoding='utf-8') as f:
        header = f.read()
    tmp = tempfile.mkdtemp(prefix='nxr_test_savever_h_')
    try:
        p = os.path.join(tmp, 'savever_data.h')
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(re.sub(r'(drop_0,\s*)(\d+)', lambda m: m.group(1) + str(int(m.group(2)) + 1), header))
        try:
            sv.load_tables(p)
            check('a header with a count that does not match: refused', False)
        except ValueError:
            check('a header with a count that does not match: refused', True)
        try:
            sv.load_tables(os.path.join(tmp, 'none.h'))
            check('no header: refused', False)
        except FileNotFoundError:
            check('no header: refused', True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    if len(argv) == 2 and argv[0] == 'convert':
        r = sv.Report()
        rc = sv.convert_dir(sv.tables(), argv[1], r)
        print(f'rc {rc} {r}')
        return 1 if rc < 0 else 0
    test_convert()
    test_refusals()
    test_picture()
    root = tempfile.mkdtemp(prefix='nxr_test_savever_')
    try:
        test_dir(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    test_real_tables()
    print(f'test_savever: {len(fails)} FAILED' if fails else 'test_savever: all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
