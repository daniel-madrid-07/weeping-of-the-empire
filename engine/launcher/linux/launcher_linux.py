#!/usr/bin/env python3
"""The Linux launcher (linux-port, 2026-11-02): engine/launcher/win/launcher.c's run_package / play_package for a
Linux game folder, in Python 3 (the standard library only: it runs from the AppImage's Python and from the system's).

    python3 launcher_linux.py [<game folder>]      play (default: the folder this file is in)
    python3 launcher_linux.py --env <game folder>  print the environment the game gets, run nothing
    python3 launcher_linux.py --saves [--look-in <folder>] [--json] [<game folder>]   the saves it can import
    python3 launcher_linux.py --import-save <save folder> [<game folder>]             make that save the game's

What the game gets, as on Windows (the folders follow the XDG base directories instead of %APPDATA% / %LOCALAPPDATA%):
    <data> = $XDG_DATA_HOME/nxrecomp/TotK-engine (~/.local/share/...): settings.ini, userdata (saves), mods, amiibo
    <cache> = $XDG_CACHE_HOME/nxrecomp/TotK-engine (~/.cache/...): run (the working folder: shader and pipeline caches),
             crashes (crash-<time>-<pid>.txt: the game's crash report, out of its log), logs (the last 8 game logs,
             totk-<time>.log)
    TOTK_USERDATA (userdata-v<n> for another game version, seeded once from 1.0.0's saves), TOTK_IMG / TOTK_ROMFS /
    TOTK_OVERLAY from player.txt, TOTK_PRIORITY=normal, TOTK_CRASH_DIR, TOTK_MODS_DIR (<game folder>/mods;<data>/mods),
    TOTK_AMIIBO_DIR, NXR_LAUNCHER_RESTART (the game's restart request), player.txt's env.* lines, TOTK_PRECOMPILE_SEED
    (cache-seed/ unless player.txt set it); the package's settings.ini copied to <data> when the player has none. The
    shell's TOTK_* / LARECOMP_* / NX_* / NXR_* / NXRECOMP_* variables are cleared first (launcher.c clean_environment).
The game's restart (exit code RESTART_CODE: F10's restart, the in-game skip) starts it again, at most 5 times a minute.
As launcher.c also: the save copied to <data>/backups before each start (save-<time>-<nn>-auto, the newest 10 automatic
ones kept) and once more as -before-update when a new package (another commit=) starts first; one game at a time (a
lock in <cache>); a folder that is incomplete or whose game fails within its first 20 s falls back once to the
newest other folder beside it (lastgood); errors go to <cache>/launcher.log and, without a terminal (a file manager,
a desktop entry), to a desktop notice (notify-send, zenity or kdialog, whichever there is).
Saves, as launcher.c's saves menu (Shift / --saves) and saves.c: --saves lists the current save and the saves it can
import (player.txt's import=, our other game versions' saves, yuzu and its forks, Ryujinx, their Flatpak folders), or
those under --look-in <folder> (a JKSV backup from the console: 5 levels down); --json is that list for the AppImage's
window, whose Saves button imports one. --import-save backs up the current save (-before-import) and, for a save of a
later game version, the original (-original), then makes it the save, converted to 1.0.0 on the way (savever.py);
never while the game runs.
"""
import datetime
import glob
import json
import os
import shutil
import stat
import subprocess
import sys
import time

# launcher.c's exit codes, as a Linux exit status keeps them: their low byte (the game exits with the full value)
RESTART_CODE = 0x52535452 & 0xFF          # the game asks to be started again (pc_restart.cpp)
GPU_UNSUPPORTED_CODE = 0x47505530 & 0xFF  # (the game said why)
DUMP_LOST_CODE = 0x444D5030 & 0xFF        # (the dump went away during play; the game said so)
KEEP_GAME_LOGS = 8
KEEP_AUTO_BACKUPS = 10
KEEP_CRASHES = 10    # (pc_crash.cpp's KEEP: the newest crash-*.txt)
CRASH_CONTEXT = 40   # (the log lines before a crash report that go into its crash-*.txt: what the game was doing)
EARLY_S = 20   # (launcher.c EARLY_MS: a failure this soon is the build's, and the previous folder is tried)
RESTART_MAX, RESTART_WINDOW_S = 5, 60
CLEARED = ('TOTK_', 'LARECOMP_', 'NX_', 'NXR_', 'NXRECOMP_')


def user_dirs(environ=None):
    """(<data>, <cache>): Windows' %APPDATA% and %LOCALAPPDATA% nxrecomp\\TotK-engine folders, the XDG way"""
    e = os.environ if environ is None else environ
    home = e.get('HOME') or os.path.expanduser('~')
    data = e.get('XDG_DATA_HOME') or os.path.join(home, '.local', 'share')
    cache = e.get('XDG_CACHE_HOME') or os.path.join(home, '.cache')
    return os.path.join(data, 'nxrecomp', 'TotK-engine'), os.path.join(cache, 'nxrecomp', 'TotK-engine')


_LOG = None
_ECHO = True   # (False for --saves / --import-save: their answer is stdout, and what is said goes to the log alone)


def say(msg):
    if _ECHO:
        print(f'[launcher] {msg}', flush=True)
    if _LOG:
        try:
            with open(_LOG, 'a', encoding='utf-8') as f:
                f.write(f'{datetime.datetime.now():%Y-%m-%d %H:%M:%S} {msg}\n')
        except OSError:
            pass


def tell(msg):
    """an error the player must see: said, and without a terminal shown as a desktop notice"""
    say(msg)
    if sys.stdout.isatty():
        return
    for cmd in (['notify-send', '-a', 'Weeping of the Empire', 'Weeping of the Empire', msg],
                ['zenity', '--error', '--no-wrap', '--text', msg], ['kdialog', '--error', msg]):
        if shutil.which(cmd[0]):
            subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return


def backup_from(data, src, why):
    """the save folder src copied to <data>/backups/save-<time>-<nn>-<why> through a .tmp folder (launcher.c /
    saves.c's names: saves_backup_from); the automatic ones beyond the newest KEEP_AUTO_BACKUPS go. (1, the backup's
    path); (0, None) when src is not a save; (-1, None) when the copy failed"""
    if not is_save(src):
        return 0, None
    bdir = os.path.join(data, 'backups')
    stamp = f'{datetime.datetime.now():%Y%m%d-%H%M%S}'
    # <nn>: after the highest of this second's, so the names keep sorting by age (a pruned low number is not reused)
    same = [os.path.basename(p)[len(f'save-{stamp}-'):][:2]
            for p in glob.glob(os.path.join(glob.escape(bdir), f'save-{stamp}-*'))]
    k = min(99, max([int(x) for x in same if x.isdigit()], default=-1) + 1)
    dst = os.path.join(bdir, f'save-{stamp}-{k:02d}-{why}')
    try:
        os.makedirs(bdir, exist_ok=True)
        delete_tree(dst + '.tmp')
        copy_tree(src, dst + '.tmp')
        os.rename(dst + '.tmp', dst)
    except OSError as e:
        delete_tree(dst + '.tmp')
        say(f'save backup failed ({e})')
        return -1, None
    autos = sorted(glob.glob(os.path.join(glob.escape(bdir), 'save-*-auto')))
    for old in autos[:max(0, len(autos) - KEEP_AUTO_BACKUPS)]:
        delete_tree(old)
    return 1, dst


def backup_save(data, userdata, why):
    """the save (<userdata>/save) backed up (backup_from): the backup's path; None when there is no save or the copy
    failed"""
    return backup_from(data, os.path.join(userdata, 'save'), why)[1]


# ---- saves: engine/launcher/win/saves.c for Linux, and launcher.c's import around it. No UI: --saves / --import-save
# (main) and the AppImage's window word it. The import always goes to <data>/userdata/save, 1.0.0's save (another game
# version's userdata-v<n> is never touched), as on Windows.

TOTK_TITLE_ID = 0x0100F2C0115B6000
YUZU_FORKS = ('yuzu', 'eden', 'citron', 'sudachi', 'suyu', 'torzu')   # (saves.c saves_find's list)
MAX_SAVES = 16    # (launcher.c MAX_CANDIDATES: what the menu lists)
LOOK_DEPTH = 5    # (launcher.c import_save: "Look in another folder" searches 5 levels down)
FAIL_STEP = 0     # (tests: make step k of replace_save fail, as saves.c g_saves_fail_step)


def save_info(d):
    """(slots, newest): d's slot_0N/progress.sav files and the newest one's time; (0, 0) when d is not a save. A TotK
    save folder is recognised that way wherever it comes from (saves.c saves_is_save)."""
    n, newest = 0, 0
    for i in range(6):
        try:
            st = os.stat(os.path.join(d, 'slot_%02d' % i, 'progress.sav'))
        except OSError:
            continue
        if stat.S_ISREG(st.st_mode):
            n, newest = n + 1, max(newest, st.st_mtime)
    return n, newest


def is_save(d):
    return save_info(d)[0] > 0


def _subdirs(d):
    """d's folders by name; a link to one is left out, never followed (saves.c each_subdir / saves_is_link)"""
    try:
        with os.scandir(d) as it:
            return sorted(e.name for e in it if e.is_dir(follow_symlinks=False))
    except OSError:
        return []


def _found(source, path, user=''):
    """a save the menu can offer, or None when path is not one (saves.c add)"""
    slots, newest = save_info(path)
    return dict(source=source, path=path, user=user, slots=slots, newest=newest) if slots else None


def _newest_first(found):
    return sorted((c for c in found if c), key=lambda c: -c['newest'])


def _ryujinx(root, index):
    """Ryujinx's <root>/<index>/0 (the committed copy; 1 is the working one), when ExtraData0, if there is one, starts
    with TotK's program id"""
    try:
        with open(os.path.join(root, index, 'ExtraData0'), 'rb') as f:
            head = f.read(8)
        if len(head) == 8 and int.from_bytes(head, 'little') != TOTK_TITLE_ID:
            return None
    except OSError:
        pass
    p = os.path.join(root, index, '0')
    return _found('Ryujinx', p if is_save(p) else os.path.join(root, index, '1'))


def find_emulator_saves(environ=None):
    """the emulators' TotK saves and our old runtime's (saves.c saves_find, the Linux folders), newest first:
        yuzu and its forks  $XDG_DATA_HOME/<name>/nand/user/save/0000000000000000/<user>/0100F2C0115B6000
        Ryujinx             $XDG_CONFIG_HOME/Ryujinx/bis/user/save/<index>/0
        nxrecomp            $XDG_DATA_HOME/nxrecomp/TotK/profile/save/<user>
    and the first two in each Flatpak app's own folders, whatever its id (~/.var/app/<id>/data and config: a Flatpak
    emulator's $XDG_DATA_HOME and $XDG_CONFIG_HOME). A link on the way to these folders is followed (a layout like
    EmuDeck's, nand/user/save linked into ~/Emulation); a link met while listing one is not (saves.c each_subdir)."""
    e = os.environ if environ is None else environ
    home = e.get('HOME') or os.path.expanduser('~')
    data = e.get('XDG_DATA_HOME') or os.path.join(home, '.local', 'share')
    config = e.get('XDG_CONFIG_HOME') or os.path.join(home, '.config')
    flatpak = os.path.join(home, '.var', 'app')
    found = []
    for d, c in [(data, config)] + [(os.path.join(flatpak, a, 'data'), os.path.join(flatpak, a, 'config'))
                                    for a in _subdirs(flatpak)]:
        for name in YUZU_FORKS:
            root = os.path.join(d, name, 'nand', 'user', 'save', '0000000000000000')
            found += [_found(name, os.path.join(root, u, f'{TOTK_TITLE_ID:016X}'), u) for u in _subdirs(root)]
        root = os.path.join(c, 'Ryujinx', 'bis', 'user', 'save')
        found += [_ryujinx(root, i) for i in _subdirs(root)]
    root = os.path.join(data, 'nxrecomp', 'TotK', 'profile', 'save')
    found += [_found('nxrecomp', os.path.join(root, u), u) for u in _subdirs(root)]
    return _newest_first(found)


def find_saves_under(root, depth=LOOK_DEPTH):
    """every save folder under root, at most depth levels down (a JKSV backup on an SD card, a portable emulator),
    newest first; a save folder's own folders are not searched (saves.c saves_find_under)"""
    found = []

    def walk(d, depth):
        c = _found('folder', d)
        if c:
            found.append(c)
            return
        for s in (_subdirs(d) if depth > 0 else ()):
            if len(found) < MAX_SAVES:
                walk(os.path.join(d, s), depth - 1)
    walk(root, depth)
    return _newest_first(found)


def find_saves(pkg=None, environ=None):
    """what the menu offers, at most MAX_SAVES (launcher.c find_candidates): the game folder's import=<save folder>
    (player.txt) first, then our other game versions' saves (<data>/userdata-v<n>/save, 'nxrecomp v143': into 1.0.0
    through the converter), then the emulators'"""
    found = []
    if pkg:
        p = path_of(pkg, player_values(pkg).get('import'))
        found.append(p and _found('save on this PC', p))
    data = user_dirs(environ)[0]
    for d in sorted(glob.glob(os.path.join(glob.escape(data), 'userdata-v*'))):
        found.append(_found('nxrecomp ' + os.path.basename(d)[len('userdata-'):], os.path.join(d, 'save')))
    return ([c for c in found if c] + find_emulator_saves(environ))[:MAX_SAVES]


def copy_tree(src, dst):
    """src's files and folders copied into dst (saves.c saves_copy_tree): links and special files left out; each copy
    keeps its time (the menus show a save's newest) and is writable, so a read-only file from an SD card can be replaced
    and deleted later"""
    os.makedirs(dst, exist_ok=True)
    with os.scandir(src) as it:
        for e in it:
            d = os.path.join(dst, e.name)
            if e.is_dir(follow_symlinks=False):
                copy_tree(e.path, d)
            elif e.is_file(follow_symlinks=False):
                shutil.copy2(e.path, d)
                os.chmod(d, stat.S_IMODE(os.stat(d).st_mode) | stat.S_IWUSR)


def delete_tree(d):
    """d and all it holds, read-only folders too; a link in it goes, never what it points at (saves.c
    saves_delete_tree). True when nothing is left."""
    def open_up(p):   # (a folder without write permission keeps its files: given it first; a link left alone)
        try:
            if not os.path.islink(p):
                os.chmod(p, stat.S_IMODE(os.lstat(p).st_mode) | stat.S_IRWXU)
        except OSError:
            pass
    if os.path.isdir(d) and not os.path.islink(d):
        open_up(d)
        for top, dirs, _ in os.walk(d):
            for x in dirs:
                open_up(os.path.join(top, x))
        shutil.rmtree(d, ignore_errors=True)
    elif os.path.lexists(d):
        try:
            os.remove(d)
        except OSError:
            pass
    return not os.path.lexists(d)


def recover_save(data):
    """an interrupted replace_save (a power cut) repaired: a leftover save.new goes, a save.old with no save is put
    back (saves.c saves_recover: before every start and every import)"""
    save = os.path.join(data, 'userdata', 'save')
    delete_tree(save + '.new')
    if os.path.lexists(save + '.old'):
        if os.path.lexists(save):
            delete_tree(save + '.old')
        else:
            try:
                os.rename(save + '.old', save)
            except OSError as e:
                say(f'save: {save}.old could not be put back ({e})')


def replace_save(data, src, rep):
    """makes the save folder src the save (saves.c saves_replace_report): src copied to save.new and its files of later
    game versions converted there (savever.py; src itself is never written), save renamed to save.old, save.new to
    save, save.old deleted. A failure at any step leaves a complete save in place, the old one or the new one. 0 done,
    else the step that failed: 1 the copy (or src is not a save), 2 moving the old save away, 3 moving the new one in,
    4 the converter refused the save (rep says why; nothing changed). The caller makes the backup first."""
    savever = _savever()
    userdata = os.path.join(data, 'userdata')
    save, fresh, old = (os.path.join(userdata, n) for n in ('save', 'save.new', 'save.old'))
    try:
        os.makedirs(userdata, exist_ok=True)
        delete_tree(fresh)
        if FAIL_STEP == 1:
            raise OSError('step 1 made to fail')
        copy_tree(src, fresh)
    except OSError as e:
        say(f'save: copying {src} failed ({e})')
        delete_tree(fresh)
        return 1
    if not is_save(fresh):
        delete_tree(fresh)
        return 1
    if savever.convert_dir(savever.tables(), fresh, rep) < 0:   # (a later version's save, if it is one)
        delete_tree(fresh)
        return 4
    had = os.path.lexists(save)
    if had:
        delete_tree(old)
        try:
            if FAIL_STEP == 2:
                raise OSError('step 2 made to fail')
            os.rename(save, old)
        except OSError:
            delete_tree(fresh)
            return 2
    try:
        if FAIL_STEP == 3:
            raise OSError('step 3 made to fail')
        os.rename(fresh, save)
    except OSError:
        if had:
            try:
                os.rename(old, save)
            except OSError:
                pass   # (recover_save puts it back at the next start)
        delete_tree(fresh)
        return 3
    if had:
        delete_tree(old)
    return 0


def _savever():
    """savever.py beside this file (savever.c in Python), imported only when a save is converted or named, so playing
    and the builder's import of this file never load it; its folder goes on sys.path (python -I puts none there)"""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import savever
    return savever


def import_save(src, environ=None):
    """launcher.c replace_with for an import: the current save backed up first (-before-import), then the save to
    import as it is when it is a later game version's (-original: the copy is what gets converted), then replace_save.
    (code, report, backup, original): code 0 done, 1-4 replace_save's, 5 the current save could not be backed up, 6 nor
    the save to import, 7 the converter's tables could not be read (nothing changed in these three); backup and
    original are the backups' paths, or None."""
    savever = _savever()
    rep = savever.Report()
    data = user_dirs(environ)[0]
    try:
        target = savever.tables().target
    except (OSError, ValueError) as e:   # (a game folder without savever_data.h, or with a damaged one)
        say(f'save import: the converter\'s tables could not be read ({e})')
        return 7, rep, None, None
    recover_save(data)
    rc, bk = backup_from(data, os.path.join(data, 'userdata', 'save'), 'before-import')
    if rc < 0:
        return 5, rep, None, None
    rev, orig = savever.dir_revision(src), None
    if rev and rev != target:
        rc, orig = backup_from(data, src, 'original')
        if rc < 0:
            return 6, rep, bk, None
    code = replace_save(data, src, rep)
    say(f'import from {src} (revision {rev}): {"failed" if code else "done"} (backup {bk or "none: no save"}; original '
        f'{orig or "-"}); converted {rep.files} files from {rep.from_rev}: keys -{rep.keys_dropped} flags '
        f'-{rep.flags_dropped} enums {rep.enums_reset} pictures {rep.pictures_fitted}+{rep.pictures_blank} '
        f'option-dropped {rep.option_dropped}; refused {rep.refused} {rep.file} {rep.detail}')
    return code, rep, bk, orig


def save_version(path):
    """a save folder's game version as the list shows it (launcher.c import_save): '1.1.x / 1.2.x'..., 'unknown
    version', '' when its files are not TotK save files"""
    savever = _savever()
    rev = savever.dir_revision(path)
    try:
        return (savever.revision_name(rev) or 'unknown version') if rev else ''
    except (OSError, ValueError):   # (no tables)
        return 'unknown version'


def when(t):
    """a file time as the Windows menus show it (launcher.c when): DD/MM/YYYY HH:MM, local; '?' for none"""
    return f'{datetime.datetime.fromtimestamp(t):%d/%m/%Y %H:%M}' if t else '?'


def refusal_text(rep):
    """why the converter refused a save, for the player (launcher.c refusal_text)"""
    sv, f = _savever(), rep.file
    if rep.refused == sv.SV_UNKNOWN_VERSION:
        return (f"{f} comes from a game version the converter doesn't know (revision {rep.detail}). It converts saves "
                "of 1.1.x, 1.2.x and 1.4.x.")
    if rep.refused == sv.SV_UNKNOWN_KEYS:
        return (f"{f} has data that version 1.0.0 doesn't have and that the converter doesn't know ({rep.detail}; a "
                "modified save, or one from a version after 1.4.x). Converted, the save would be broken.")
    if rep.refused == sv.SV_TOO_BIG:
        return f'{f} holds more than a 1.0.0 save has room for.'
    if rep.refused == sv.SV_PICTURE:   # (on Linux also: no Pillow for this Python, which Setup's own Python has)
        return f"The picture in {f} couldn't be converted." + ('' if sv.have_pillow() else
                " That needs Pillow, which this Python doesn't have: import the save with the Saves button in the "
                "Setup window instead.")
    if rep.refused == sv.SV_IO:
        return f"{f} couldn't be read or written."
    return f'{f} is damaged or incomplete.'


def conversion_text(rep):
    """what the conversion changed, a line each; '' when nothing was converted (launcher.c conversion_text)"""
    if not rep.files:
        return ''
    lines = [f'The save was from version {_savever().revision_name(rep.from_rev) or "?"} and is now a 1.0.0 save:']
    if rep.keys_dropped + rep.flags_dropped:
        lines.append(f"- {rep.keys_dropped + rep.flags_dropped} entries from later updates that 1.0.0 doesn't have "
                     "were left out.")
    if rep.enums_reset:
        lines.append(f'- {rep.enums_reset} values that exist only in later versions were set back to their default.')
    if rep.pictures_fitted + rep.pictures_blank:
        lines.append(f'- {rep.pictures_fitted + rep.pictures_blank} slot pictures were made smaller '
                     f"({rep.pictures_blank} couldn't be read and are now black).")
    if rep.option_dropped:
        lines.append("- The game settings (option.sav) couldn't be converted: the game starts with its defaults.")
    return '\n'.join(lines)


def import_text(code, rep, bk, orig, src):
    """import_save's outcome in words (launcher.c replace_with's messages)"""
    if code == 5:
        return "Couldn't back up the current save; nothing was changed."
    if code == 6:
        return "Couldn't keep a copy of the save you wanted to import; nothing was changed."
    if code == 7:
        return ("This game folder's save converter is missing or damaged: press Install in Setup to repair it. Nothing "
                "was changed.")
    if code == 4:
        return f"The save wasn't imported; your previous save is unchanged.\n\n{refusal_text(rep)}\n\n{src}"
    if code:
        return f"Couldn't copy the save (step {code}). Your previous save is unchanged.\n\n{src}"
    parts = ['Done.', conversion_text(rep)]
    if orig and rep.files:
        parts.append(f'The original save, not converted, is kept in:\n{orig}')
    if bk:
        parts.append(f'Your previous save is backed up in:\n{bk}')
    return '\n\n'.join(p for p in parts if p)


def player_values(pkg):
    """player.txt's key=value lines (the first of each key), as launcher.c's package_value reads them"""
    out = {}
    try:
        with open(os.path.join(pkg, 'player.txt'), encoding='utf-8-sig') as f:
            for ln in f:
                k, eq, v = ln.rstrip('\r\n').partition('=')
                if eq and k and k not in out:
                    out[k] = v
    except OSError:
        pass
    return out


def path_of(pkg, v):
    v = (v or '').rstrip(' ')
    return v if not v or v.startswith('/') else os.path.abspath(os.path.join(pkg, v))


def cpu_missing(level):
    """what this CPU lacks for the build's level ('' when nothing): v3 = AVX2 + FMA + BMI2 (launcher.c cpu_missing)"""
    try:
        flags = next((ln.split(':', 1)[1].split() for ln in open('/proc/cpuinfo') if ln.startswith('flags')), [])
    except OSError:
        return ''
    need = ['ssse3', 'sse4_1', 'sse4_2', 'popcnt'] + (['avx2', 'fma', 'bmi2', 'f16c'] if level == 3 else [])
    return ', '.join(f.upper() for f in need if f not in flags)


def game_env(pkg, environ=None):
    """(run folder, log folder, environment) for the game in pkg, as run_package sets them; makes the folders"""
    base = dict(os.environ if environ is None else environ)
    data, cache = user_dirs(base)
    env = {k: v for k, v in base.items() if not k.upper().startswith(CLEARED)}
    vals = player_values(pkg)
    ver = vals.get('version', '')
    tag = ''.join(c for c in ver if c.isdigit()) if ver and ver != '1.0.0' else ''
    userdata = os.path.join(data, f'userdata-v{tag}' if tag else 'userdata')
    if tag:   # (another game version: its own saves, a copy of 1.0.0's once)
        src, dst = os.path.join(data, 'userdata', 'save'), os.path.join(userdata, 'save')
        if not os.path.exists(dst) and os.path.isdir(src):
            shutil.copytree(src, dst)
            open(os.path.join(userdata, 'saves-copied-from-1.0.0'), 'w').close()
            say(f'version {ver}: its own saves in {userdata}; the 1.0.0 saves copied there')
    run, crashes, logs = (os.path.join(cache, d) for d in ('run', 'crashes', 'logs'))
    for d in (userdata, run, crashes, logs):
        os.makedirs(d, exist_ok=True)
    env.update(TOTK_USERDATA=userdata, TOTK_IMG=path_of(pkg, vals.get('img')), TOTK_PRIORITY='normal',
               TOTK_CRASH_DIR=crashes)
    for k in ('romfs', 'overlay'):
        if vals.get(k, '').strip():
            env['TOTK_' + k.upper()] = path_of(pkg, vals[k])
    mods = []   # (the game folder's mods, then the player's: launcher.c mods_env)
    for d in (os.path.join(pkg, f'mods-v{tag}' if tag else 'mods'), os.path.join(data, f'mods-v{tag}' if tag else 'mods')):
        os.makedirs(d, exist_ok=True)
        if d not in mods:
            mods.append(d)
    # (':' between them: the runtime's Linux list reader, mods.cpp env_list; ';' made one folder 'a;b' that does not
    #  exist and the game found no mods: linux-test D#83, justelesis. The amiibo list below is ';' on both systems)
    env['TOTK_MODS_DIR'] = ':'.join(mods)
    amiibo = [d for d in (os.path.join(pkg, 'amiibo'), os.path.join(data, 'amiibo')) if os.path.isdir(d) or d.startswith(pkg)]
    os.makedirs(amiibo[0], exist_ok=True)
    env['TOTK_AMIIBO_DIR'] = ';'.join(amiibo)
    env['NXR_LAUNCHER_RESTART'] = os.path.join(cache, 'restart.env')
    env.update((k[4:], v) for k, v in vals.items() if k.startswith('env.') and len(k) > 4)
    if 'TOTK_PRECOMPILE_SEED' not in env and os.path.isfile(os.path.join(pkg, 'cache-seed', 'precompile.bin')):
        env['TOTK_PRECOMPILE_SEED'] = os.path.join(pkg, 'cache-seed')
    defs, dst = path_of(pkg, vals.get('settings')), os.path.join(data, 'settings.ini')
    if defs and os.path.isfile(defs) and not os.path.exists(dst):
        os.makedirs(data, exist_ok=True)
        shutil.copyfile(defs, dst)
        say(f'settings: {defs} -> {dst}')
    return run, logs, env


def start(pkg, exe):
    """one run: the game with its log, waited for; (exit code, log, seconds it ran, its crash-*.txt or None)"""
    run, logs, env = game_env(pkg)
    data = user_dirs(env)[0]
    commit = player_values(pkg).get('commit', '')
    mark = os.path.join(data, 'last-package.txt')
    last = open(mark).read().strip() if os.path.isfile(mark) else ''
    if commit and commit != last:   # (a new package: its first start keeps a copy from before it)
        if last and backup_save(data, env['TOTK_USERDATA'], 'before-update'):
            say(f'a new package ({commit[:9]}): the save copied first (before-update)')
        with open(mark, 'w') as f:
            f.write(commit + '\n')
    bk = backup_save(data, env['TOTK_USERDATA'], 'auto')
    if bk:
        say(f'save backed up: {bk}')
    old = sorted(glob.glob(os.path.join(logs, 'totk-*.log')))
    for p in old[:max(0, len(old) - (KEEP_GAME_LOGS - 1))]:
        os.remove(p)
    logp = os.path.join(logs, f'totk-{datetime.datetime.now():%Y%m%d-%H%M%S}.log')
    try:
        os.remove(env['NXR_LAUNCHER_RESTART'])
    except OSError:
        pass
    with open(logp, 'wb') as log:
        say(f'starting {exe}, log {logp}')
        t0 = time.monotonic()
        p = subprocess.Popen([exe], cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        code = p.wait()   # (negative: the signal that ended it)
    secs = time.monotonic() - t0
    rep = None
    if code not in (0, RESTART_CODE, GPU_UNSUPPORTED_CODE, DUMP_LOST_CODE):
        rep = crash_report(logp, env['TOTK_CRASH_DIR'], code, p.pid, env, commit)
        if rep:
            say(f'crash report: {rep}')
    return code, logp, secs, rep


def last_fatal(logp):
    """the game's last '[fatal] ...' line in its log (the reason it stopped by itself: the dump unreadable, no game
    data, the GPU unsupported), without the tag; '' when there is none"""
    try:
        with open(logp, encoding='utf-8', errors='replace') as f:
            lines = [ln.strip() for ln in f if ln.startswith('[fatal] ')]
    except OSError:
        return ''
    return lines[-1][len('[fatal] '):] if lines else ''


def crash_report(logp, crashes, code, pid, env, commit=''):
    """the game's crash report as a file of its own, crash-<time>-<pid>.txt in <cache>/crashes (TOTK_CRASH_DIR): the
    name pc_crash.cpp gives it on Windows, so a player attaches the same file on both. The game writes the report to
    stderr, which is its log here (rt_main.cpp on_crash_signal: '[crash] signal ...' to the end, then exit 3); this takes
    it from there with the CRASH_CONTEXT lines before it. Nothing else: no core dump, no memory (the key is not in the
    log). The newest KEEP_CRASHES kept; TOTK_PC_CRASH=0 (player.txt's env.TOTK_PC_CRASH=0, as on Windows): none. The
    file's path, or None (no report in the log: killed by a signal, closed with an error)."""
    if env.get('TOTK_PC_CRASH', '').strip() == '0':
        return None
    try:
        with open(logp, encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
    except OSError:
        return None
    at = next((i for i, ln in enumerate(lines) if ln.startswith('[crash] signal ')), None)
    if at is None:
        return None
    os.makedirs(crashes, exist_ok=True)
    out = os.path.join(crashes, f'crash-{datetime.datetime.now():%Y%m%d-%H%M%S}-{pid}.txt')
    try:
        with open(out, 'w', encoding='utf-8') as f:
            f.write(f'Weeping of the Empire crash report (Linux)\ncommit: {commit or "?"}\nexit code: {code}\nlog: {logp}\n\n'
                    f'--- the log before it (last {CRASH_CONTEXT} lines)\n')
            f.writelines(lines[max(0, at - CRASH_CONTEXT):at])
            f.write('\n--- the report\n')
            f.writelines(lines[at:])
    except OSError as e:
        say(f'crash report not written ({e})')
        return None
    old = sorted(glob.glob(os.path.join(crashes, 'crash-*.txt')))
    for p in old[:max(0, len(old) - KEEP_CRASHES)]:
        try:
            os.remove(p)
        except OSError:
            pass
    return out


def incomplete(pkg):
    """why pkg cannot start ('' when it can): its files, the processor level it was built for"""
    exe, vals = os.path.join(pkg, 'totk'), player_values(pkg)
    img = path_of(pkg, vals.get('img'))
    if not (os.path.isfile(exe) and img and os.path.isfile(os.path.join(img, 'main.bin'))):
        return f'{pkg}: incomplete (totk, player.txt or img/main.bin is missing): run Setup again'
    miss = cpu_missing(2 if vals.get('cpu') == 'x86-64-v2' else 3)
    if miss:
        return (f'this build of the game was made for processors with AVX2, and this one lacks {miss}: run Setup again on '
                'this PC (it picks the build for processors without AVX2 by itself)')
    return ''


def lastgood(pkg):
    """the newest other game folder beside pkg that could start, or None"""
    parent = os.path.dirname(pkg)
    others = [os.path.join(parent, d) for d in os.listdir(parent)] if os.path.isdir(parent) else []
    others = [d for d in others if d != pkg and os.path.isfile(os.path.join(d, 'player.txt')) and not incomplete(d)]
    return max(others, key=os.path.getmtime) if others else None


def run_package(pkg):
    """play_package: the game, its restarts; 'early' when it failed in its first EARLY_S seconds"""
    times = []
    while True:
        code, logp, secs, rep = start(pkg, os.path.join(pkg, 'totk'))
        if code == RESTART_CODE:
            now = time.monotonic()
            times = [t for t in times if now - t < RESTART_WINDOW_S] + [now]
            if len(times) > RESTART_MAX:
                tell(f'the game asked to restart {RESTART_MAX} times within a minute: stopped (a setting that restarts '
                     'it at every start?)')
                return 1
            say('the game asked for a restart')
            continue
        if code in (GPU_UNSUPPORTED_CODE, DUMP_LOST_CODE):   # (the same low byte, 48: the game said why in its log;
            why = last_fatal(logp)                            #  Windows shows a box, here the notice carries it)
            if why:
                tell(f'{why} (log: {logp})')
            return 0
        if code == 0:
            return 0
        tell((f'the game was ended by signal {-code}' if code < 0 else f'the game closed with code {code}') + f'; log: {logp}' +
             (f'; crash report: {rep}' if rep else ''))
        return 'early' if secs < EARLY_S else 1


def game_lock(cache):
    """<cache>/launcher.lock taken, held while the returned file stays open; None when a game (or an import) holds it:
    one game at a time, two would share the save and the caches, and an import never runs under a game"""
    import fcntl   # (Linux only; the tests import this file on Windows too)
    os.makedirs(cache, exist_ok=True)
    lock = open(os.path.join(cache, 'launcher.lock'), 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock.close()
        return None
    return lock


def play(pkg):
    global _LOG
    pkg = os.path.abspath(pkg)
    data, cache = user_dirs()
    os.makedirs(cache, exist_ok=True)
    _LOG = os.path.join(cache, 'launcher.log')
    lock = game_lock(cache)
    if lock is None:
        tell('the game is already running')
        return 1
    recover_save(data)   # (an import cut short leaves a complete save in place: launcher.c's saves_recover at start)
    why = incomplete(pkg)
    tried = [pkg]
    if why:
        fallback = lastgood(pkg)
        if not fallback:
            tell(why)
            return 2
        say(f'{why}; starting the previous folder {fallback}')
        pkg = fallback
        tried.append(pkg)
    r = run_package(pkg)
    if r == 'early' and len(tried) == 1:   # (a build that fails at once: the previous one, once)
        fallback = lastgood(pkg)
        if fallback:
            say(f'{pkg} failed while starting: the previous folder {fallback}')
            r = run_package(fallback)
    return 1 if r == 'early' else r


def list_saves(pkg, look_in=None, environ=None):
    """the current save and the saves found (find_saves; with look_in, those under it), as --saves --json prints them"""
    data = user_dirs(environ)[0]
    save = os.path.join(data, 'userdata', 'save')
    slots, newest = save_info(save)
    found = find_saves_under(look_in) if look_in else find_saves(pkg, environ)
    return dict(data=data, save=save, slots=slots, saved=when(newest) if slots else '',
                saves=[dict(source=c['source'], user=c['user'], slots=c['slots'], saved=when(c['newest']),
                            version=save_version(c['path']), path=c['path']) for c in found])


SAVES_USAGE = ('usage: launcher_linux.py --saves [--look-in <folder>] [--json] [<game folder>]\n'
               '       launcher_linux.py --import-save <save folder: the one with slot_00, slot_01...> [<game folder>]')
IMPORT_NOTE = ('The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; '
               'the original is kept, untouched, in the backups.')


def saves_main(a):
    """--saves / --import-save: the Windows launcher's saves menu, its import, in a terminal (and --json for the
    AppImage's window). The flags go anywhere: 'TotK Launcher' puts its own folder first. --look-in or --json alone
    mean --saves."""
    global _LOG, _ECHO
    opts, rest, i = {}, [], 0
    while i < len(a):
        if a[i] in ('--import-save', '--look-in'):
            if i + 1 == len(a):
                print(SAVES_USAGE)
                return 2
            opts[a[i]] = os.path.abspath(a[i + 1])
            i += 2
        else:
            if a[i] in ('--saves', '--json'):
                opts[a[i]] = True
            else:
                rest.append(a[i])
            i += 1
    if len(rest) > 1:
        print(SAVES_USAGE)
        return 2
    pkg = os.path.abspath(rest[0] if rest else os.path.dirname(os.path.abspath(__file__)))
    cache = user_dirs()[1]
    os.makedirs(cache, exist_ok=True)
    _LOG, _ECHO = os.path.join(cache, 'launcher.log'), False
    src = opts.get('--import-save')
    if src:
        if not is_save(src):
            found = find_saves_under(src)
            print(f'{src} is not a save folder (one with slot_00, slot_01...).' + (' Saves under it:' if found else ''))
            for c in found:
                print(f'    {c["path"]}')
            return 2
        lock = game_lock(cache)
        if lock is None:
            print('The game is running: close it, then import the save. Your save is unchanged.')
            return 1
        try:
            code, rep, bk, orig = import_save(src)
        finally:
            lock.close()
        print(import_text(code, rep, bk, orig, src))
        return 1 if code else 0
    look_in = opts.get('--look-in')
    s = list_saves(pkg, look_in)
    if opts.get('--json'):
        print(json.dumps(s, indent=1))
        return 0
    print((f'Current save: {s["slots"]} slot(s), saved on {s["saved"]}.' if s['slots'] else 'No save yet.') +
          f'\n    {s["save"]}\n')
    if not s['saves']:
        print(f'No TotK saves found{" under " + look_in if look_in else ""}. To look in another folder (for example, a '
              'JKSV backup from the console): --saves --look-in <folder>')
        return 0
    print('Saves you can import, newest first:')
    for n, c in enumerate(s['saves'], 1):
        print(f'{n:3}. {c["source"]}{" (" + c["user"][:8] + ")" if c["user"] else ""}, {c["slots"]} slot(s), '
              f'saved {c["saved"]}{", " + c["version"] if c["version"] else ""}\n     {c["path"]}')
    print(f'\nTo import one: --import-save <its folder>. Your current save is backed up first.\n{IMPORT_NOTE}')
    return 0


def main(argv=None):
    a = list(sys.argv[1:] if argv is None else argv)
    if a and a[0] == '--env':
        pkg = os.path.abspath(a[1] if len(a) > 1 else os.path.dirname(os.path.abspath(__file__)))
        for k, v in sorted(game_env(pkg)[2].items()):
            if k.startswith(('TOTK_', 'NXR_', 'LARECOMP_')):
                print(f'{k}={v}')
        return 0
    if any(f in a for f in ('--saves', '--import-save', '--look-in', '--json')):
        return saves_main(a)
    return play(a[0] if a else os.path.dirname(os.path.abspath(__file__)))


if __name__ == '__main__':
    sys.exit(main())
