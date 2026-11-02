"""After an install (builder.py --install, the last step): what a later update does not need goes, so the install
leaves less on the disk (a v0.1.6 install measured 51 GB, 11.1 GB of it objects of earlier versions' builds nobody
links any more: ask <= 25 GB, leave <= 12 GB).

Kept, because an update or the game needs it:
  - the key just built (<work>/engine/game[-var]/<key>): its exe/ (the next relink is incremental), obj/ (an update
    that changes only the runtime relinks these; a game-code update reuses each unchanged one through objcache, the
    same files: hard links) and gen/ (the runtime relink reads its file names), manifest.json;
  - the newest key of every OTHER game version (a 1.4.3 build next to the 1.0.0 one: its own next update);
  - objcache entries those keys link (the same files as their obj/: nothing extra);
  - the source snapshot the key was built from (<work>/engine/src/<head12>).
Removed: every other key dir (earlier versions, a mods build the launcher made: it is made again at the next start
with mods, from the cache), objcache entries no key links, older source snapshots, <work>/engine/tmp.
Then NTFS compression (compact /c) of what is only read again at an update: the kept key's gen/ (generated C++).
From extracted/ only the v100 RomFS folder an older install unpacked (17 GB), once romfs.bin is proven to serve the
game instead (the passing pinned extraction report, romfs.bin's size, its verified record); the empty folder stays as
the game's mount root. Never touched: the rest of extracted/, img/, overlay/, overlays/, glyphs/, seed/, pgo/, dlss/,
venv/: the game or the next build reads them. Everything is listed in the log before it goes, and nothing outside
<work> (no link out of it) is ever removed. Cached objects go by file identity (not by link count): an install copied
with hard links shares them with other folders.
"""
import json
import os
import shutil
import subprocess


def on_disk(path, st):
    """the bytes a file takes on the disk (an NTFS-compressed one: fewer than its size)"""
    if os.name != 'nt':
        return st.st_size
    import ctypes
    f = ctypes.windll.kernel32.GetCompressedFileSizeW
    f.restype, hi = ctypes.c_ulong, ctypes.c_ulong(0)
    lo = f(path, ctypes.byref(hi))
    return st.st_size if lo == 0xFFFFFFFF and ctypes.GetLastError() else (hi.value << 32) + lo


def tree_bytes(path):
    """a folder's bytes on the disk, each hard-linked file once"""
    total, stack, seen = 0, [path], set()
    while stack:
        try:
            with os.scandir(stack.pop()) as it:
                for e in it:
                    if e.is_symlink() or (hasattr(e, 'is_junction') and e.is_junction()):
                        continue
                    if e.is_dir(follow_symlinks=False):
                        stack.append(e.path)
                    else:
                        st = os.stat(e.path, follow_symlinks=False)   # (DirEntry.stat has no st_nlink / st_ino on Windows)
                        if st.st_nlink > 1:   # (a hard-linked file counts once)
                            if (st.st_ino, st.st_dev) in seen:
                                continue
                            seen.add((st.st_ino, st.st_dev))
                        total += on_disk(e.path, st)
        except OSError:
            pass
    return total


def key_dirs(eng):
    """[(dir, manifest)] of every key dir under <eng>/game and <eng>/game-var"""
    out = []
    for parent in ('game', 'game-var'):
        root = os.path.join(eng, parent)
        for name in sorted(os.listdir(root)) if os.path.isdir(root) else []:
            d = os.path.join(root, name)
            if not os.path.isdir(d):
                continue
            try:
                with open(os.path.join(d, 'manifest.json'), encoding='utf-8') as f:
                    man = json.load(f)
            except (OSError, ValueError):
                man = {}
            out.append((d, man))
    return out


def built_at(d):
    for p in (os.path.join(d, 'exe', 'totk.exe'), os.path.join(d, 'manifest.json'), d):
        if os.path.exists(p):
            return os.path.getmtime(p)
    return 0


def compress(path, run=subprocess.run):
    """NTFS compression of a folder's files (compact /c /s); False when Windows refused (another file system)"""
    if os.name != 'nt' or not os.path.isdir(path):
        return False
    r = run(['compact', '/c', '/s', '/q', '/i', os.path.join(path, '*')], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return r.returncode == 0


def inside(p, work):
    """p really lies in the work folder (through no link out of it)"""
    w = os.path.normcase(os.path.realpath(work))
    return os.path.normcase(os.path.realpath(p)).startswith(w + os.sep)


def romfs_folder_done(work):
    """<work>/extracted/v100/romfs, an older install's unpacked copy of the game's files, when romfs.bin serves the game
    instead: the extraction passed with the pinned hash, romfs.bin is that size, and its verified record matches it (the
    game's package reads the image only with that record). Else None (no folder, or the image not proven: kept)"""
    v100 = os.path.join(work, 'extracted', 'v100')
    folder, img = os.path.join(v100, 'romfs'), os.path.join(v100, 'romfs.bin')
    try:
        if not os.path.isdir(folder) or not any(True for _ in os.scandir(folder)) or not inside(folder, work):
            return None
        with open(os.path.join(v100, 'romfs-report.json'), encoding='utf-8') as f:
            rep = json.load(f)
        with open(img + '.verified.json', encoding='utf-8') as f:
            ver = json.load(f)
        st = os.stat(img)
    except (OSError, ValueError):
        return None
    ok = (rep.get('status') == 'pass' and rep.get('expected_match') and rep.get('romfs_size') == st.st_size and
          (ver.get('size'), ver.get('mtime_ns')) == (st.st_size, st.st_mtime_ns))
    return folder if ok else None


def tidy(work, key, head, log=print, run=subprocess.run):
    """Returns (bytes freed, what was done). key: the key just built; head: the source commit it was built from. What
    it will remove is listed in the log first (setup.log), and nothing outside <work> is ever removed."""
    eng = os.path.join(work, 'engine')
    before = tree_bytes(eng)
    keys = key_dirs(eng)
    mine = [(d, m) for d, m in keys if os.path.basename(d) == key]
    if not mine:   # (never guess: without the key just built nothing is removed)
        log(f'tidy: the key {key} is not in {eng}: nothing removed')
        return 0, []
    version = lambda m: m.get('game_version') or '1.0.0'
    keep = {os.path.normcase(d) for d, _ in mine}
    newest = {}
    for d, m in keys:
        v = version(m)
        if v != version(mine[0][1]) and (v not in newest or built_at(d) > built_at(newest[v])):
            newest[v] = d
    keep |= {os.path.normcase(d) for d in newest.values()}
    # the plan: dirs to remove, then the cached objects no kept key links (by file identity, not by link count: an install
    # copied with hard links shares them with other folders, where removing them here still empties this install)
    dirs = [d for d, _ in keys if os.path.normcase(d) not in keep]
    src = os.path.join(eng, 'src')
    dirs += [os.path.join(src, n) for n in (os.listdir(src) if os.path.isdir(src) else [])
             if head and n != head[:12] and os.path.isdir(os.path.join(src, n))]
    tmp = os.path.join(eng, 'tmp')
    dirs += [os.path.join(tmp, n) for n in (os.listdir(tmp) if os.path.isdir(tmp) else [])]
    unpacked = romfs_folder_done(work)
    folder_bytes = 0
    if unpacked:   # (its size from the extraction's report: walking 304k files only to count them costs a minute)
        try:
            with open(os.path.join(os.path.dirname(unpacked), 'romfs-report.json'), encoding='utf-8') as f:
                folder_bytes = int(json.load(f).get('total_file_bytes') or 0)
        except (OSError, ValueError):
            pass
    if unpacked:
        dirs += [os.path.join(unpacked, n) for n in os.listdir(unpacked)]   # (the empty folder stays: the game's mount root)
    linked = set()
    for d in keep:
        for r, _, fs in os.walk(d):
            for f in fs:
                try:
                    st = os.stat(os.path.join(r, f))
                    linked.add((st.st_ino, st.st_dev))
                except OSError:
                    pass
    stale, stale_bytes = [], 0
    for r, _, fs in os.walk(os.path.join(eng, 'objcache')):
        for f in fs:
            p = os.path.join(r, f)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if (st.st_ino, st.st_dev) not in linked:
                stale.append(p)
                stale_bytes += st.st_size
    dirs = [d for d in dirs if inside(d, work)]
    stale = [p for p in stale if inside(p, work)]
    for d in dirs:
        log(f'tidy: removing {d}')
    if stale:
        log(f'tidy: removing {len(stale)} cached objects no kept build uses ({stale_bytes / 1e9:.1f} GB)')
    done = []
    for d in dirs:
        try:
            shutil.rmtree(d, ignore_errors=True) if os.path.isdir(d) and not os.path.islink(d) else os.remove(d)
        except OSError:
            pass
        rel = os.path.relpath(d, work)
        done.append(rel if not rel.startswith(os.path.join('extracted', 'v100', 'romfs')) else None)
    done = [x for x in done if x]
    if unpacked:
        done.append('the unpacked game files (romfs.bin serves the game)')
    for p in stale:
        try:
            os.remove(p)
        except OSError:
            pass
    if stale:
        done.append(f'{len(stale)} cached objects no build uses')
    if compress(os.path.join(mine[0][0], 'gen'), run):
        done.append('generated code compressed')
    freed = max(0, before - tree_bytes(eng)) + folder_bytes
    log(f'tidy: {freed / 1e9:.1f} GB freed (' + (', '.join(done[:6]) + (', ...' if len(done) > 6 else '') if done else
                                                           'nothing to remove') + ')')
    return freed, done
