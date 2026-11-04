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

On Linux (linux-port, 2026-11-02) the same folder holds totk (ELF) instead of totk.exe, lib/libSDL3.so.0 (the exe's
RUNPATH is $ORIGIN/lib; from build_deps.sh's prefix, NXR_LINUX_DEPS) and the launcher: launcher_linux.py with 'TotK
Launcher', a shell script that runs it (python3); text files with LF line ends."""
import argparse, datetime, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get('NXRECOMP_WORK', r'C:\nxrecomp-work')
LINUX = sys.platform.startswith('linux')
EXE = 'totk' if LINUX else 'totk.exe'
NL = '\n' if LINUX else '\r\n'   # (player.txt, settings.ini and the guide: each system's own line ends)
ENG = os.path.join(WORK, 'engine')
ROMFS = os.path.join(WORK, 'extracted', 'v100', 'romfs')
# the extraction's RomFS image, served in place when romfs_image_verify.py found it identical to ROMFS (romfs= stays the
# folder: the fallback for a path the image lacks, and what the launcher checks exists); boot.py's romfs_image()
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), 'tools', 'engine'))
from romfs_image_verify import romfs_image  # noqa: E402
import exekey  # noqa: E402
OVERLAY = os.path.join(ENG, 'overlay', 'romfs')
SEED = os.path.join(ENG, 'seed')  # the precompile seed: programs and pipelines built before the world
# every program of the game's shader archives (tools/shader_pack.py, 18k programs): the engine's archive warm-up builds
# them in the background, so a shader no session recorded is a cache hit at its first draw (exec.c pa_load)
ARCHIVE = os.path.join(ENG, 'archive_shaders.bin')
DLSS = os.path.join(ENG, 'dlss')
FFX = os.path.join(ENG, 'ffx')   # (AMD's FidelityFX runtime for FSR, fetched by tools/engine/build.py)
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
display.fps_cap = 120  # 120 fps; F10 > Gráficos ofrece 30 a 240 (40, 45, 60, 75, 90, 120, 144, 165...) o Unlocked (hasta la frecuencia del monitor, máx. 240)
graphics.render_scale = auto  # Resolución de render: la altura de la ventana
game.screenshots = 1  # F12 guarda una captura en Imágenes\\Capturas de pantalla
game.crash_reports = 1  # Informe y minivolcado si el juego se cierra por un fallo
"""


def log(*a):
    print('[package]', *a, flush=True)


SUPPORTED_VERSIONS = ('1.0.0', '1.4.3')   # game versions a release may be (1.4.3: built from the player's own update)


def shippable(key, man):
    """None, or why the key is never packaged: a diagnostic build (sentinel addresses, trap stubs: tools/engine/
    build.py DIAGNOSTIC) or a game version the releases do not support"""
    if man.get('diagnostic'):
        return f'key {key} is a diagnostic build of TotK {man.get("game_version", "?")}: never packaged'
    if man.get('game_version', '1.0.0') not in SUPPORTED_VERSIONS:
        return f'key {key} is TotK {man["game_version"]}: the releases are {" / ".join(SUPPORTED_VERSIONS)}'
    return None


def newest_key():
    best = None
    for k in os.listdir(os.path.join(ENG, 'game')):
        m = os.path.join(ENG, 'game', k, 'manifest.json')
        exe = os.path.join(ENG, 'game', k, 'exe', EXE)
        if not (os.path.exists(m) and os.path.exists(exe)):
            continue
        man = json.load(open(m))
        if man.get('objs_done') and not shippable(k, man) and (best is None or man.get('objs_built', '') > best[1].get('objs_built', '')):
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


def linux_launcher(tmp):
    """the Linux folder's launcher (engine/launcher/linux/launcher_linux.py and its 'TotK Launcher' script) and the
    SDL3 library the exe loads from lib/ (build_deps.sh's prefix: NXR_LINUX_DEPS, else <work>/linux-deps)"""
    src = os.path.join(os.path.dirname(HERE), 'linux')
    shutil.copy2(os.path.join(src, 'launcher_linux.py'), os.path.join(tmp, 'launcher_linux.py'))
    script = os.path.join(tmp, 'TotK Launcher')
    with open(script, 'w', newline='\n') as f:
        f.write('#!/bin/sh\n# Tears of the Kingdom (nxrecomp): starts the game in this folder (launcher_linux.py)\n'
                'here=$(cd "$(dirname "$0")" && pwd)\nexec python3 "$here/launcher_linux.py" "$here" "$@"\n')
    os.chmod(script, 0o755)
    deps = os.environ.get('NXR_LINUX_DEPS') or os.path.join(WORK, 'linux-deps')
    sdl = os.path.join(deps, 'sdl3-install', 'lib', 'libSDL3.so.0')
    if not os.path.isfile(sdl):
        sys.exit(f'missing {sdl} (the SDL3 library the game loads; NXR_LINUX_DEPS: the Linux SDK\'s deps folder)')
    os.makedirs(os.path.join(tmp, 'lib'))
    shutil.copy2(os.path.realpath(sdl), os.path.join(tmp, 'lib', 'libSDL3.so.0'))   # (the file, not the symlink chain)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--key'); ap.add_argument('--out', default=os.path.join(WORK, 'release'))
    ap.add_argument('--allow-untagged', action='store_true',
                    help='package an exe with no key tag (linked before 11-03); a tag naming another key is refused anyway')
    ap.add_argument('--exe', help='a runtime relinked against the key (build.py runtime --key), instead of the exe built with the key')
    ap.add_argument('--commit', help='the commit --exe was built from (player.txt, folder name)')
    ap.add_argument('--release', help='the release tag this source is (player.txt release=: the launcher\'s '
                    'automatic update compares it with GitHub\'s latest; none = this build never updates itself)')
    ap.add_argument('--release-repo', help='owner/repo on GitHub the release comes from (player.txt release_repo=)')
    ap.add_argument('--env', action='append', default=[], help='K=V added to player.txt (env.K=V)')
    ap.add_argument('--no-patch-sets', action='store_true',
                    help='leave TOTK_PATCH_SETS out of player.txt (the six game-patch restart rows stay hidden)')
    ap.add_argument('--overlay', default=OVERLAY,
                    help='the romfs overlay player.txt points at (default: the shared one; e.g. a copy with a set such as '
                         'mals merged by engine_dir, before the shared one is rebuilt)')
    a = ap.parse_args()
    floor = 2 if os.environ.get('NXR_BUILDER_PACKAGE') else 45
    if shutil.disk_usage(WORK).free / 2**30 < floor:
        sys.exit(f'under {floor} GB free on the work drive: not writing a release (disk rule)')
    if a.key:
        key, man = a.key, json.load(open(os.path.join(key_dir(a.key), 'manifest.json')))
    else:
        key, man = newest_key()
    if shippable(key, man):
        sys.exit(shippable(key, man))
    # Another game version (1.4.3): its own extraction, no overlay (ours is built from 1.0.0's files), no 1.0.0 game-patch
    # sets, shader seed or shader archive (all made from 1.0.0), version= for the launcher (its own save folder) and
    # -v<digits> in the folder's name. 1.0.0: every line as before.
    global ROMFS
    version = man.get('game_version', '1.0.0')
    vtag = 'v' + version.replace('.', '')
    if version != '1.0.0':
        ROMFS = os.path.join(WORK, 'extracted', vtag, 'romfs')
        if a.overlay == OVERLAY:
            a.overlay = os.path.join(ENG, 'overlay', f'empty-{vtag}')
            os.makedirs(a.overlay, exist_ok=True)
        a.no_patch_sets = True
    overlay = os.path.abspath(a.overlay)
    # the key's own exe carries the runtime of the tree the key was built from; a newer runtime relinks against it
    exe = os.path.abspath(a.exe) if a.exe else os.path.join(key_dir(key), 'exe', EXE)
    if not a.exe and exekey.read(exe) is None:
        # the key's own exe (game-var/<key>/exe) is that key's by construction; one linked before the tag existed (a
        # player's step 5 reused on an update) gets it now instead of a refusal, which would stop his Setup at step 8
        exekey.tag(exe, key)
    why = '' if a.allow_untagged and exekey.read(exe) is None else exekey.refuse(exe, key)
    if why:   # (an exe linked against another key than the one player.txt will name: 11-03's slot exe)
        sys.exit(f'refused: {why}')
    commit = commit_of(exe, man, a.commit)
    dirty = dirty_of(exe)
    if not os.path.isdir(ROMFS) and romfs_image(ROMFS):
        # an extraction without the unpacked folder (the Builder's --no-unpack): romfs.bin serves every file; the folder
        # stays the game's (empty) mount root, which romfs= names and the launcher checks
        os.makedirs(ROMFS)
    for p, what in ((exe, 'exe'), (man['img'], 'NSO images'), (ROMFS, 'RomFS'), (overlay, 'romfs overlay')) + \
            (() if a.no_patch_sets else ((PATCH_SETS, 'game-patch sets (TOTK_PATCH_SETS; --no-patch-sets to ship without)'),)):
        if not os.path.exists(p):
            sys.exit(f'missing {what}: {p}')
    beta = bool(man.get('beta'))   # (a beta of another version: build.py TOTK_VERSION_BETA, features off by design)
    name = f'TotK-PC-{datetime.datetime.now():%Y%m%d-%H%M}-{commit[:9]}' + ('-dirty' if dirty else '') +         ('' if version == '1.0.0' else f'-{vtag}') + ('-beta' if beta else '')
    dst = os.path.join(a.out, name)
    tmp = dst + '.tmp'
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    os.makedirs(tmp)
    if LINUX:
        linux_launcher(tmp)
    else:
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'build.py'), 'launcher', '--out', tmp], check=True)
        for junk in ('TotK Launcher.pdb',):
            if os.path.exists(os.path.join(tmp, junk)):
                os.remove(os.path.join(tmp, junk))
    shutil.copy2(exe, os.path.join(tmp, EXE))
    if version == '1.0.0' and os.path.exists(os.path.join(SEED, 'precompile.bin')):   # the launcher points TOTK_PRECOMPILE_SEED at it
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
    env.update(man.get('beta_runtime_env') or {})   # (a beta's features off: also compiled in, build.py)
    # the console's shared system fonts (only Korean / Chinese text needs them; the runtime falls back to English
    # without): the six <type>-<name>.bfttf the extraction made from the firmware, when it was given
    fonts = os.path.join(os.path.dirname(ROMFS), 'fonts')
    if os.path.isfile(os.path.join(os.path.dirname(ROMFS), 'fonts-report.json')) and os.path.isdir(fonts):
        env['TOTK_SHARED_FONTS'] = fonts
    if romfs_image(ROMFS):   # (only proven equal to the romfs= folder; --env TOTK_ROMFS_PACK= turns it off)
        env['TOTK_ROMFS_PACK'] = romfs_image(ROMFS)
    if os.path.isfile(os.path.join(DLSS, 'nvngx_dlss.dll')):   # NVIDIA's DLSS DLLs, fetched by tools/engine/build.py
        env['TOTK_DLSS_DIR'] = DLSS
    if os.path.isfile(os.path.join(FFX, 'amd_fidelityfx_vk.dll')):
        env['TOTK_FFX_DIR'] = FFX
    if version == '1.0.0' and archive_pack():   # (--env TOTK_PRECOMPILE_ARCHIVE=0 turns the warm-up off)
        env['TOTK_ARCHIVE_SHADERS'] = ARCHIVE
    env.update(kv.split('=', 1) for kv in a.env)
    lines = [f'img={man["img"]}', f'romfs={ROMFS}', f'overlay={overlay}', f'commit={commit}', f'key={key}',
             f'build={build_name(man)}', f'patches={man.get("patches") or "none"}', f'packaged={datetime.datetime.now().isoformat(timespec="seconds")}',
             'settings=settings.ini'] + ([] if version == '1.0.0' else [f'version={version}']) + (['beta=1'] if beta else []) + (['dirty=1'] if dirty else []) + ([f'release={a.release}'] if a.release else []) + ([f'release_repo={a.release_repo}'] if a.release_repo else []) + cpu_line(man) + ([f'import={IMPORT}'] if os.path.isdir(IMPORT) else []) + [f'env.{k}={v}' for k, v in env.items()]
    open(os.path.join(tmp, 'player.txt'), 'w', encoding='utf-8', newline=NL).write('\n'.join(lines) + '\n')
    open(os.path.join(tmp, 'settings.ini'), 'w', encoding='utf-8', newline=NL).write(SETTINGS)
    guide_file = guide_name()
    guide = open(os.path.join(HERE, guide_file), encoding='utf-8').read()
    guide = guide.replace('{commit}', commit[:9]).replace('{key}', key).replace('{build}', build_name(man)).replace('{img}', man['img']) \
                 .replace('{romfs}', ROMFS).replace('{overlay}', overlay)
    open(os.path.join(tmp, guide_file), 'w', encoding='utf-8-sig', newline=NL).write(guide)
    os.rename(tmp, dst)
    size = sum(os.path.getsize(os.path.join(dst, f)) for f in os.listdir(dst))
    log(f'{dst} ({size / 2**20:.0f} MB): key {key} ({build_name(man)}), commit {commit[:9]}, patches {man.get("patches") or "none"}')


if __name__ == '__main__':
    main()
