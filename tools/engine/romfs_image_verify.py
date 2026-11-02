"""Lossless check of serving the RomFS from the extraction's image: every file of romfs.bin, at the offset and size
engine/runtime/romfs_image.cpp's index gives (identical to the extraction's romfs-index.json: test_romfs_image.py),
compared byte for byte with the extracted folder, and the folder holds no file the image lacks. On a pass it writes
<image>.verified.json (size, mtime), which boot.py and package.py require before they serve the image
(TOTK_ROMFS_PACK). 8 threads (a first open of a loose file costs Defender's scan). Disk-heavy (2 x 17 GB): run it
outside the measurement windows.
--from-report: an extraction without a folder (pipeline --no-unpack, the Builder's: a player's install keeps no 17 GB
copy): nothing to compare, so the record is written from the extraction's own proof instead: romfs-report.json passed
(all six IVFC levels verified while romfs.bin was written, its sha256 the pinned version's), romfs.bin still that size,
every indexed file inside it. Seconds; the runtime serves the image through its own index (test_romfs_image.py).
Since v0.1.8 the image may stay in the player's dump (romfs-source.txt, pipeline/nx/romsource.py: extract --in-dump):
--from-report records it the same way, from the extraction's proof, and checks the dump is still there at its size.
Run: python tools/engine/romfs_image_verify.py [--image romfs.bin] [--index romfs-index.json] [--romfs DIR] [--from-report]"""
import argparse
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

V100 = os.path.join(os.environ.get('NXRECOMP_WORK') or r'C:\nxrecomp-work', 'extracted', 'v100')
CHUNK = 8 << 20
ROMFS_IMAGE = os.path.join(V100, 'romfs.bin')   # served in place (TOTK_ROMFS_PACK), the folder behind it
ROMFS_SOURCE = os.path.join(V100, 'romfs-source.txt')   # or the image in the player's dump (v0.1.8)


def romfs_image(romfs_dir):
    """The extraction's RomFS image when this check found it byte-identical to romfs_dir, the RomFS folder the boot /
    package really uses (its .verified.json names that folder and the image's size and mtime), else None: another
    folder (a package's romfs=, another version's extraction) never gets v100's image as its pack. One handle instead
    of thousands of first opens per load (boot.py and package.py). romfs.bin first, then romfs-source.txt (the image
    in the player's dump: the runtime reads it the same way)."""
    for image in (ROMFS_IMAGE, ROMFS_SOURCE):
        try:
            v = json.load(open(image + '.verified.json', encoding='utf-8'))
            st = os.stat(image)
            same = lambda p: os.path.normcase(os.path.abspath(p))
            if (v.get('size'), v.get('mtime_ns')) != (st.st_size, st.st_mtime_ns) or not romfs_dir or                     same(v.get('romfs') or '') != same(romfs_dir):
                continue
            return image
        except (OSError, ValueError):
            continue
    return None


def open_image(path):
    """romfs.bin unbuffered, or romfs-source.txt as pipeline/nx/romsource.py's DumpImage"""
    if os.path.basename(path).lower() != 'romfs-source.txt':
        return open(path, 'rb', buffering=0)
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if repo not in sys.path:
        sys.path.insert(0, repo)
    from pipeline.nx.romsource import DumpImage
    return DumpImage(path)


def write_record(a, files, nbytes, **extra):
    st = os.stat(a.image)
    json.dump(dict({'image': a.image, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'romfs': a.romfs, 'files': files,
                    'bytes': nbytes, 'checked': time.strftime('%Y-%m-%d %H:%M:%S')}, **extra),
              open(a.image + '.verified.json', 'w', encoding='utf-8'), indent=1)


def from_report(a, files):
    try:
        rep = json.load(open(os.path.join(os.path.dirname(a.image), 'romfs-report.json'), encoding='utf-8'))
        with open_image(a.image) as img:   # (the image in the dump: also checks the dump is there at its size)
            size = img.seek(0, 2)
    except (OSError, ValueError) as e:
        print(f'romfs_image_verify: FAIL (no extraction report or image: {e})')
        return 1
    bad = [m for m, ok in (
        ('the extraction did not pass', rep.get('status') == 'pass'),
        ('its sha256 was not checked against the pinned version', rep.get('expected_match')),
        (f'the image is {size} bytes, the extraction wrote {rep.get("romfs_size")}', rep.get('romfs_size') == size),
        (f'the extraction wrote {rep.get("image")}, not {os.path.basename(a.image)}',
         rep.get('image', 'romfs.bin') == os.path.basename(a.image)),
        (f'the index lists {len(files)} files, the extraction {rep.get("file_count")}', rep.get('file_count') == len(files)),
        ('an indexed file lies outside the image', all(0 <= f['offset'] and f['offset'] + f['size'] <= size
                                                        for f in files))) if not ok]
    if os.path.isdir(a.romfs) and any(True for _ in os.scandir(a.romfs)):
        bad.append(f'{a.romfs} holds files: compare them (run without --from-report)')
    for m in bad:
        print('  ' + m)
    if bad:
        print('romfs_image_verify: FAIL')
        return 1
    write_record(a, len(files), sum(f['size'] for f in files), proof='extraction report: IVFC levels 0-5, pinned '
                 'sha256 ' + rep['romfs_sha256'][:16])
    print(f'romfs_image_verify: pass from the extraction report ({len(files)} files; {a.image}.verified.json written)')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', help='default: the image romfs-index.json names (romfs.bin or romfs-source.txt)')
    ap.add_argument('--index', default=os.path.join(V100, 'romfs-index.json'))
    ap.add_argument('--romfs', default=os.path.join(V100, 'romfs'))
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--from-report', action='store_true', help='no folder: record the image from the extraction report')
    a = ap.parse_args()
    index = json.load(open(a.index, encoding='utf-8'))
    files = index['files']
    a.image = a.image or os.path.join(os.path.dirname(os.path.abspath(a.index)), index.get('image', 'romfs.bin'))
    if a.from_report:
        return from_report(a, files)
    local = threading.local()
    bad, done, lock = [], [0, 0], threading.Lock()

    def check(f):
        img = getattr(local, 'img', None)
        if img is None:
            img = local.img = open_image(a.image)
        path = os.path.join(a.romfs, *f['path'].split('/'))
        try:
            with open(path, 'rb', buffering=0) as src:
                if os.fstat(src.fileno()).st_size != f['size']:
                    return 'size differs: ' + f['path']
                img.seek(f['offset'])
                left = f['size']
                while left:
                    n = min(left, CHUNK)
                    if img.read(n) != src.read(n):
                        return 'bytes differ: ' + f['path']
                    left -= n
        except OSError as e:
            return 'unreadable: %s (%s)' % (f['path'], e)
        with lock:
            done[0] += 1
            done[1] += f['size']
        return None

    t0 = time.time()
    with ThreadPoolExecutor(a.threads) as ex:
        for i, r in enumerate(ex.map(check, sorted(files, key=lambda f: f['offset']), chunksize=256)):
            if r:
                bad.append(r)
            if i % 50000 == 0:
                print('%6d / %d files, %.1f GB, %.0f s' % (i, len(files), done[1] / 1e9, time.time() - t0), flush=True)
    # the other way: a folder file the image lacks would be served by the folder fallback, but say so
    names = {f['path'] for f in files}
    extra = []
    for root, _, fs in os.walk(a.romfs):
        for n in fs:
            rel = os.path.relpath(os.path.join(root, n), a.romfs).replace(os.sep, '/')
            if rel not in names:
                extra.append(rel)
    print('checked %d files, %.2f GB in %.0f s: %d differ, %d folder files not in the image' %
          (done[0], done[1] / 1e9, time.time() - t0, len(bad), len(extra)))
    for r in bad[:20] + ['extra: ' + e for e in extra[:20]]:
        print('  ' + r)
    ok = not bad and not extra and done[0] == len(files)
    if ok:   # boot.py and package.py serve the image only with this record of it (same size and mtime)
        write_record(a, done[0], done[1])
    print('romfs_image_verify: %s' % ('pass (%s.verified.json written)' % a.image if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
