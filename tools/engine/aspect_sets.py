"""The UI layout sets of the screen shapes a player uses, not of every shape (each costs ~30 s and ~150 MB): Setup builds
those of the PC's monitors (16:9 needs none) and of the shapes built here before; another shape picked in Options / F10
(graphics.aspect_ratio, or graphics.hud_layout = center) is built at the next start. The game finds its layout overlay
missing (games/totk/engine/totk_engine.c build_shape), starts TOTK_ASPECT_BUILD (this, with the Builder's tools: player.txt)
and exits before its window opens; this builds the shape, adds it to <work>/overlays/aspects.txt (the next Setup keeps
it) and starts the launcher again.

    <python> tools/engine/aspect_sets.py (--install DIR | --work DIR) --set <w>x<h>[-center] [--set ...]
        --exe-dir <the playable folder> [--pid N]

A set is aspect-<w>x<h>[-center] in <work>/overlays (python -m games.totk.overlays --only-aspects) and the engine
overlay's romfs-aspect-<...> with its glyph bank (tools/engine/build.py overlay-aspect). The game marks each set
<overlay>-aspect-<set>.failed ("building") before it starts this: a build that ends well removes the marks, one that
fails writes why into them, and one that never ends (this does not start, its console is closed) leaves them; while a
mark is there the game keeps 16:9 for that shape and does not ask again (the next Setup clears it). The output goes to
<work>/overlays/aspect-build.log.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# (name, ratio range, the size of its layout set): the ranges are totk_engine.c wide_canvas's k_shapes, the sizes the
# sets totk_patches.c totk_layout_set gives every canvas of that shape
SHAPES = (('16:10', 1.55, 1.65, '2560x1600'), ('21:9', 2.2, 2.6, '3440x1440'), ('32:9', 3.3, 3.8, '5120x1440'),
          ('48:9', 4.8, 5.9, '7680x1440'))
WANTED = 'aspects.txt'   # in <work>/overlays: the sets Setup builds besides the monitors' (one per line)


def size_of(w, h):
    """the layout set size of a w x h screen's shape, None for 16:9 and shapes the game has no layouts for"""
    a = w / h if h else 0
    return next((size for _, lo, hi, size in SHAPES if lo <= a < hi), None)


def sets_of(size):
    """a shape's sets: its own and, for a wide one, the one with the HUD in the middle 16:9 (graphics.hud_layout)"""
    w, h = (int(x) for x in size.split('x'))
    return [size] + ([size + '-center'] if w * 9 > h * 16 else [])


def known(name):
    """a set of SHAPES: <size>, or <size>-center for a wide one"""
    m = re.fullmatch(r'(\d+x\d+)(-center)?', name)
    return bool(m) and m.group(1) in [s for *_, s in SHAPES] and name in sets_of(m.group(1))


def drop_unwanted(work, names, log=print):
    """<work>/overlays/aspect-* layout sets of no wanted shape (an earlier Setup built every shape): removed, as
    build.py overlay drops their engine copies (they hold an older version's layouts)"""
    root = os.path.join(work, 'overlays')
    try:
        dirs = [d for d in os.listdir(root) if d.startswith('aspect-') and os.path.isdir(os.path.join(root, d))]
    except OSError:
        return []
    gone = []
    for d in dirs:
        if not known(d[len('aspect-'):]) or d[len('aspect-'):] in names:
            continue
        p = os.path.join(root, d)
        try:
            os.rename(p, p + '.old')   # (whole, first: a running game holding a file of it keeps it, said)
            shutil.rmtree(p + '.old', ignore_errors=True)
            gone.append(d)
        except OSError as e:
            log(f'{p}: no shape wanted it, but it could not be removed ({e})')
    return gone


def label(name):
    size, _, center = name.partition('-')
    w, h = (int(x) for x in size.split('x'))
    shape = next((n for n, lo, hi, _ in SHAPES if lo <= w / h < hi), size)
    return shape + (' (HUD in the middle)' if center == 'center' else '')


def monitors():
    """(width, height) of every monitor attached (Windows; none elsewhere or when they cannot be read)"""
    if os.name != 'nt':
        return []
    import ctypes
    from ctypes import wintypes
    out = []
    proc = ctypes.WINFUNCTYPE(ctypes.c_int, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    def one(hm, dc, r, p):
        out.append((r.contents.right - r.contents.left, r.contents.bottom - r.contents.top))
        return 1
    try:
        ctypes.windll.user32.EnumDisplayMonitors(None, None, proc(one), 0)
    except OSError:
        return []
    return out


def wanted(work):
    try:
        with open(os.path.join(work, 'overlays', WANTED), encoding='utf-8') as f:
            return [ln.strip() for ln in f if ln.strip()]
    except OSError:
        return []


def add_wanted(work, names):
    have = wanted(work)
    new = have + [n for n in names if n not in have]
    if new != have:
        os.makedirs(os.path.join(work, 'overlays'), exist_ok=True)
        with open(os.path.join(work, 'overlays', WANTED), 'w', encoding='utf-8', newline='\n') as f:
            f.write(''.join(n + '\n' for n in new))


def setup_sets(work, screens=None, write=True):
    """the sets Setup builds (tools/builder/builder.py steps 6 and 7): every monitor's shape (screens: [(w, h)], else
    monitors()) and those built on demand before (aspects.txt, which gets the monitors' too), in that order"""
    names = []
    for w, h in monitors() if screens is None else screens:
        size = size_of(w, h)
        names += [n for n in sets_of(size) if n not in names] if size else []
    names += [n for n in wanted(work) if n not in names]
    if write:
        add_wanted(work, names)
    return names


def sizes(names):
    """the --aspects list of games.totk.overlays for these sets (each size once)"""
    return ','.join(dict.fromkeys(n.split('-')[0] for n in names))


def player_txt(exe_dir):
    out = {}
    try:
        with open(os.path.join(exe_dir, 'player.txt'), encoding='utf-8') as f:
            for ln in f:
                k, sep, v = ln.rstrip('\r\n').partition('=')
                if sep:
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def failed_marker(exe_dir, work, name):
    """<the engine overlay>-aspect-<name>.failed: the path totk_engine.c build_shape checks (TOTK_OVERLAY = overlay=)"""
    ov = player_txt(exe_dir).get('overlay') or os.path.join(work, 'engine', 'overlay', 'romfs')
    return f'{ov}-aspect-{name}.failed'


def build(work, install, names, record):
    """the sets, with the Builder's tools: the layout sets, then each engine overlay copy and its glyph bank (the steps'
    output, which B.run shows, to record too)"""
    sys.path.insert(0, HERE)
    from mods_exefs import builder_env
    B, env = builder_env(install, work)
    py = sys.executable
    v100 = os.path.join(work, 'extracted', 'v100')
    huds = ','.join(dict.fromkeys('center' if n.endswith('-center') else 'corner' for n in names))
    out = B.run('layouts', [py, '-m', 'games.totk.overlays', '--romfs', v100, '--out', os.path.join(work, 'overlays'),
                            '--aspects', sizes(names), '--hud', huds, '--only-aspects'], env, False)
    record(out)
    for n in names:
        record(B.run('overlay', [py, os.path.join(HERE, 'build.py'), 'overlay-aspect', '--set', n], env, False))


def wait_pid(pid, log):
    if not pid or os.name != 'nt':
        return
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
    if h:
        log(f'waiting for the game (pid {pid}) to close')
        ctypes.windll.kernel32.WaitForSingleObject(h, 120000)
        ctypes.windll.kernel32.CloseHandle(h)


def relaunch(exe_dir, log):
    launcher = os.path.join(exe_dir, 'TotK Launcher.exe')
    if os.name == 'nt' and os.path.isfile(launcher):
        log('starting the game again')
        subprocess.Popen([launcher], cwd=exe_dir, creationflags=0x00000008 | 0x00000200)   # DETACHED, NEW_PROCESS_GROUP


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--install')
    ap.add_argument('--work')
    ap.add_argument('--set', action='append', required=True, help='<w>x<h>[-center], once per set')
    ap.add_argument('--exe-dir', required=True)
    ap.add_argument('--pid', type=int)
    ap.add_argument('--no-start', action='store_true', help='do not start the launcher afterwards (tests)')
    a = ap.parse_args(argv)
    install = os.path.abspath(a.install) if a.install else None
    if not install and not a.work:   # (not a Builder install: nothing to build into, nothing to write)
        print('aspect_sets.py: no --install / --work: this folder was not made by the Builder', file=sys.stderr)
        if not a.no_start:
            relaunch(a.exe_dir, print)
        return 0
    work = os.path.join(install, 'work') if install else os.path.abspath(a.work)
    # (only the sets of SHAPES go any further, into paths and commands: anything else is refused, unmarked)
    refused = [n for n in dict.fromkeys(a.set) if not known(n)]
    names = [n for n in dict.fromkeys(a.set) if known(n)]
    os.makedirs(os.path.join(work, 'overlays'), exist_ok=True)
    logf = open(os.path.join(work, 'overlays', 'aspect-build.log'), 'a', encoding='utf-8')

    def log(msg):
        line = msg if msg.endswith('\n') else msg + '\n'
        if sys.stdout:   # (None under pythonw: no console)
            print(time.strftime('%H:%M:%S ') + line, end='', flush=True)
        logf.write(time.strftime('%H:%M:%S ') + line)
        logf.flush()

    def mark(n, why):
        try:
            with open(failed_marker(a.exe_dir, work, n), 'w', encoding='utf-8') as f:
                f.write(f'{why}\n')
        except OSError as e2:
            log(f'the failure could not be marked ({e2}): the next start asks again')

    # a refused set is marked too (the game's own mark, written before it started this, may be gone): else a shape the
    # game knows and this does not (k_shapes and SHAPES apart) would have the two start each other at every start; a
    # name that is no set name at all goes into no path, and the game is not started again
    unsafe = [n for n in refused if not re.fullmatch(r'\d+x\d+(-center)?', n)]
    if refused:
        log(f'not a shape the game has layouts for: {", ".join(repr(n) for n in refused)} (refused)')
    for n in refused:
        if n not in unsafe:
            mark(n, 'not a shape this Setup has layouts for')
    shapes = ' and '.join(dict.fromkeys(label(n) for n in names))
    if names:
        log(f'Preparing the {shapes} screen layout (about {20 + 10 * len(names)} s, once): the game starts again by itself.')
    try:
        wait_pid(a.pid, log)   # (before the launcher starts again in any case: it refuses while the game runs)
        if not names:
            raise SystemExit('no shape to build')
        build(work, install, names, lambda out: (logf.write(out), logf.flush()))
        add_wanted(work, names)
        for n in names:
            try:
                os.remove(failed_marker(a.exe_dir, work, n))
            except OSError:
                pass
        log('done')
    except BaseException as e:   # (anything: the marker and the restart always happen, or every start would exit again)
        if isinstance(e, KeyboardInterrupt):
            raise
        log(f'not built: {type(e).__name__}: {e}; the game keeps 16:9 for it (the next Setup tries again)')
        for n in names:
            mark(n, e)
    if unsafe:
        log('the game is not started again: it asked for something that is no screen shape (start it from the launcher)')
    elif not a.no_start:
        relaunch(a.exe_dir, log)
    logf.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
