"""Release folder for playing the engine.

    python engine/launcher/win/package.py [--key KEY] [--exe EXE --commit SHA] [--overlay DIR] [--out DIR] [--env K=V ...]

One folder <out>/TotK-PC-<YYYYMMDD-HHMM>-<commit9>/ (default out: <work>/release) with:
    TotK Launcher.exe   engine/launcher/win/launcher.c (single-build mode: totk.exe next to it)
    totk.exe            the key's own exe (<work>/engine/game/<key>/exe, or game-var/<key> for a build variant such
                        as build.py --package's PGO; built by tools/engine/build.py game)
    player.txt          where the game data is on this PC (img, romfs, overlay: never copied), the commit and key,
                        settings=settings.ini, env.* (F10 overlay on)
    settings.ini        defaults copied to %APPDATA%\\nxrecomp\\TotK-engine\\settings.ini on the first start
    LEEME.txt           the player's guide, in Spanish (LEEME.txt next to this file)
No game data goes in: the NSO images, the RomFS and our romfs overlay (files derived from the game's) stay in the
work folder and player.txt points at them. Key: --key, else the newest built key with an exe and objects done.
"""
import argparse, datetime, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get('NXRECOMP_WORK', r'C:\nxrecomp-work')
ENG = os.path.join(WORK, 'engine')
ROMFS = os.path.join(WORK, 'extracted', 'v100', 'romfs')
# the extraction's RomFS image, served in place when romfs_image_verify.py found it identical to ROMFS (romfs= stays the
# folder: the fallback for a path the image lacks, and what the launcher checks exists); boot.py's romfs_image()
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), 'tools', 'engine'))
from romfs_image_verify import romfs_image  # noqa: E402
OVERLAY = os.path.join(ENG, 'overlay', 'romfs')
SEED = os.path.join(ENG, 'seed')  # the precompile seed: programs and pipelines built before the world
# every program of the game's shader archives (tools/shader_pack.py, 18k programs): the engine's archive warm-up builds
# them in the background, so a shader no session recorded is a cache hit at its first draw (exec.c pa_load)
ARCHIVE = os.path.join(ENG, 'archive_shaders.bin')
DLSS = os.path.join(ENG, 'dlss')
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ENV = {'NXR_PC_OVERLAY': '1',   # the F10 overlay and settings.ini (engine-overlay.md)
       'TOTK_MOUSE_CAMERA': '1',  # the mouse camera
       'TOTK_UNLOCK_CEIL': '240'}  # "unlocked" up to 240 fps
# the RomFS heap / param sets (aspect ratio, texture cache, grass / shadow / fog distance, fast travel); without them
# totk_engine.c hides those six restart rows. Game-derived and built on this PC, so a path, never copied.
PATCH_SETS = os.path.join(WORK, 'overlays')
IMPORT = os.path.join(WORK, 'saves', '100pct')   # offered by the launcher's import (never over an existing save)
SETTINGS = """\
# Valores iniciales del paquete. El juego los cambia desde el menú F10 (pc_settings.cpp).
display.window_mode = borderless  # Pantalla completa sin bordes (F11 o Alt+Intro cambian)
display.fps_cap = 120  # 120 fps; F10 > Gráficos ofrece 30, 60, 90, 120 o Unlocked (hasta la frecuencia del monitor, máx. 240)
graphics.render_scale = auto  # Resolución de render: la altura de la ventana
game.screenshots = 1  # F12 guarda una captura en Imágenes\\Capturas de pantalla
game.crash_reports = 1  # Informe y minivolcado si el juego se cierra por un fallo
"""


def log(*a):
    print('[package]', *a, flush=True)


def newest_key():
    best = None
    for k in os.listdir(os.path.join(ENG, 'game')):
        m = os.path.join(ENG, 'game', k, 'manifest.json')
        exe = os.path.join(ENG, 'game', k, 'exe', 'totk.exe')
        if not (os.path.exists(m) and os.path.exists(exe)):
            continue
        man = json.load(open(m))
        if man.get('objs_done') and (best is None or man.get('objs_built', '') > best[1].get('objs_built', '')):
            best = (k, man)
    if not best:
        sys.exit(f'no built key with an exe under {ENG}\\game')
    return best


def key_dir(key):
    # a build variant (build.py --package / --opt / --pgo) lives under game-var (build.py key_dir)
    for sub in ('game', 'game-var'):
        if os.path.exists(os.path.join(ENG, sub, key, 'manifest.json')):
            return os.path.join(ENG, sub, key)
    sys.exit(f'no key {key} under ' + os.path.join(ENG, 'game') + ' or game-var')


def guide_name():
    """the player's guide: LEEME.txt (Spanish), or README.txt where the tree has that one instead (the public tree)"""
    return next(n for n in ('LEEME.txt', 'README.txt') if os.path.isfile(os.path.join(HERE, n)))


def build_name(man):
    """The game objects' build, for the guide, player.txt's build= and the log: the key's variant (build.py VARIANT),
    else the -O2 base; in the guide's language (release.py's copy is the Spanish one)"""
    v = json.loads((man.get('parts') or {}).get('variant') or '{}')
    es = guide_name() == 'LEEME.txt'
    if v.get('pgo'):
        return f'{"optimizada con perfil" if es else "optimised with a profile"}, PGO {v["pgo"][:8]}'
    if v:
        return ('variante ' if es else 'variant ') + ' '.join(str(x) for x in v.values())
    return 'base -O2'


def archive_pack():
    """ARCHIVE, made (a few seconds) when missing or older than the tool; None if it cannot be"""
    tool = os.path.join(REPO, 'tools', 'shader_pack.py')
    if not os.path.exists(ARCHIVE) or os.path.getmtime(ARCHIVE) < os.path.getmtime(tool):
        r = subprocess.run([sys.executable, '-I', tool, '--romfs', ROMFS, '--out', ARCHIVE])
        if r.returncode:
            log(f'shader_pack.py failed ({r.returncode}): no archive warm-up, first sight of a shader no session recorded will hitch')
            return None
    return ARCHIVE


def cpu_line(man):
    """cpu=<level> for a key built at another processor level (build.py --march x86-64-v2: the Builder's --cpu v2); the
    launcher checks the CPU against it (launcher.c run_package), and no line means x86-64-v3"""
    march = json.loads((man.get('parts') or {}).get('variant') or '{}').get('march')
    return [f'cpu={march}'] if march in ('x86-64', 'x86-64-v2') else []


def commit_of(exe, man, given=None):
    """the commit the exe was built from: --commit, else build.py's source.json next to the exe (every runtime link
    writes it), else the manifest's exe_commit (the key's own exe), else the snapshot the key's objects came from (a
    key built before 10-31, whose runtime may be newer), else 'unknown'"""
    if given:
        return given
    try:
        c = json.load(open(os.path.join(os.path.dirname(exe), 'source.json'), encoding='utf-8')).get('commit')
        if c:
            return c
    except (OSError, ValueError):
        pass
    src = man.get('src', '')
    return man.get('exe_commit') or (os.path.basename(os.path.dirname(src)) if src else 'unknown')


def dirty_of(exe):
    """True when build.py's source.json says the exe was linked from a tree with uncommitted engine/ edits: the folder
    is named -dirty and player.txt says dirty=1, so it cannot pass as its commit"""
    try:
        return bool(json.load(open(os.path.join(os.path.dirname(exe), 'source.json'), encoding='utf-8')).get('dirty'))
    except (OSError, ValueError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--key'); ap.add_argument('--out', default=os.path.join(WORK, 'release'))
    ap.add_argument('--exe', help='a runtime relinked against the key (build.py runtime --key), instead of the exe built with the key')
    ap.add_argument('--commit', help='the commit --exe was built from (player.txt, folder name)')
    ap.add_argument('--env', action='append', default=[], help='K=V added to player.txt (env.K=V)')
    ap.add_argument('--no-patch-sets', action='store_true',
                    help='leave TOTK_PATCH_SETS out of player.txt (the six game-patch restart rows stay hidden)')
    ap.add_argument('--overlay', default=OVERLAY,
                    help='the romfs overlay player.txt points at (default: the shared one; e.g. a copy with a set such as '
                         'mals merged by engine_dir, before the shared one is rebuilt)')
    a = ap.parse_args()
    overlay = os.path.abspath(a.overlay)
    floor = 2 if os.environ.get('NXR_BUILDER_PACKAGE') else 45
    if shutil.disk_usage(WORK).free / 2**30 < floor:
        sys.exit(f'under {floor} GB free on the work drive: not writing a release (disk rule)')
    if a.key:
        key, man = a.key, json.load(open(os.path.join(key_dir(a.key), 'manifest.json')))
    else:
        key, man = newest_key()
    # the key's own exe carries the runtime of the tree the key was built from; a newer runtime relinks against it
    exe = os.path.abspath(a.exe) if a.exe else os.path.join(key_dir(key), 'exe', 'totk.exe')
    commit = commit_of(exe, man, a.commit)
    dirty = dirty_of(exe)
    for p, what in ((exe, 'exe'), (man['img'], 'NSO images'), (ROMFS, 'RomFS'), (overlay, 'romfs overlay')) + \
            (() if a.no_patch_sets else ((PATCH_SETS, 'game-patch sets (TOTK_PATCH_SETS; --no-patch-sets to ship without)'),)):
        if not os.path.exists(p):
            sys.exit(f'missing {what}: {p}')
    name = f'TotK-PC-{datetime.datetime.now():%Y%m%d-%H%M}-{commit[:9]}' + ('-dirty' if dirty else '')
    dst = os.path.join(a.out, name)
    tmp = dst + '.tmp'
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'build.py'), 'launcher', '--out', tmp], check=True)
    for junk in ('TotK Launcher.pdb',):
        if os.path.exists(os.path.join(tmp, junk)):
            os.remove(os.path.join(tmp, junk))
    shutil.copy2(exe, os.path.join(tmp, 'totk.exe'))
    if os.path.exists(os.path.join(SEED, 'precompile.bin')):   # the launcher points TOTK_PRECOMPILE_SEED at it
        shutil.copytree(SEED, os.path.join(tmp, 'cache-seed'))
    else:
        log(f'no shader seed at {SEED}: first sight of each shader will hitch')
    env = dict(ENV)
    if not a.no_patch_sets:
        env['TOTK_PATCH_SETS'] = PATCH_SETS
    # the patch set's runtime_env (games/totk/patches: e.g. TOTK_SPEED_FIX=0, its realtime patches replace the
    # runtime's speed fix); the 21:03 package missed it
    if man.get('patches') and os.path.exists(man['patches']):
        env.update(json.load(open(man['patches'], encoding='utf-8')).get('runtime_env') or {})
    # the console's shared system fonts (only Korean / Chinese text needs them; the runtime falls back to English
    # without): the six <type>-<name>.bfttf the extraction made from the firmware, when it was given
    fonts = os.path.join(os.path.dirname(ROMFS), 'fonts')
    if os.path.isfile(os.path.join(os.path.dirname(ROMFS), 'fonts-report.json')) and os.path.isdir(fonts):
        env['TOTK_SHARED_FONTS'] = fonts
    if romfs_image(ROMFS):   # (only proven equal to the romfs= folder; --env TOTK_ROMFS_PACK= turns it off)
        env['TOTK_ROMFS_PACK'] = romfs_image(ROMFS)
    if os.path.isfile(os.path.join(DLSS, 'nvngx_dlss.dll')):   # NVIDIA's DLSS DLLs, fetched by tools/engine/build.py
        env['TOTK_DLSS_DIR'] = DLSS
    if archive_pack():   # (--env TOTK_PRECOMPILE_ARCHIVE=0 turns the warm-up off)
        env['TOTK_ARCHIVE_SHADERS'] = ARCHIVE
    env.update(kv.split('=', 1) for kv in a.env)
    lines = [f'img={man["img"]}', f'romfs={ROMFS}', f'overlay={overlay}', f'commit={commit}', f'key={key}',
             f'build={build_name(man)}', f'patches={man.get("patches") or "none"}', f'packaged={datetime.datetime.now().isoformat(timespec="seconds")}',
             'settings=settings.ini'] + (['dirty=1'] if dirty else []) + cpu_line(man) + ([f'import={IMPORT}'] if os.path.isdir(IMPORT) else []) + [f'env.{k}={v}' for k, v in env.items()]
    open(os.path.join(tmp, 'player.txt'), 'w', encoding='utf-8', newline='\r\n').write('\n'.join(lines) + '\n')
    open(os.path.join(tmp, 'settings.ini'), 'w', encoding='utf-8', newline='\r\n').write(SETTINGS)
    guide_file = guide_name()
    guide = open(os.path.join(HERE, guide_file), encoding='utf-8').read()
    guide = guide.replace('{commit}', commit[:9]).replace('{key}', key).replace('{build}', build_name(man)).replace('{img}', man['img']) \
                 .replace('{romfs}', ROMFS).replace('{overlay}', overlay)
    open(os.path.join(tmp, guide_file), 'w', encoding='utf-8-sig', newline='\r\n').write(guide)
    os.rename(tmp, dst)
    size = sum(os.path.getsize(os.path.join(dst, f)) for f in os.listdir(dst))
    log(f'{dst} ({size / 2**20:.0f} MB): key {key} ({build_name(man)}), commit {commit[:9]}, patches {man.get("patches") or "none"}')


if __name__ == '__main__':
    main()
