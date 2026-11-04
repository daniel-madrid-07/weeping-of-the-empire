#!/usr/bin/env python3
"""The Linux launcher (linux-port, 2026-11-02): engine/launcher/win/launcher.c's run_package / play_package for a
Linux game folder, in Python 3 (the standard library only: it runs from the AppImage's Python and from the system's).

    python3 launcher_linux.py [<game folder>]      play (default: the folder this file is in)
    python3 launcher_linux.py --env <game folder>  print the environment the game gets, run nothing

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
"""
import datetime
import glob
import os
import shutil
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


def say(msg):
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


def backup_save(data, userdata, why):
    """the save (<userdata>/save) copied to <data>/backups/save-<time>-<nn>-<why> (launcher.c / saves.c's names);
    the automatic ones beyond the newest KEEP_AUTO_BACKUPS go"""
    save = os.path.join(userdata, 'save')
    if not os.path.isdir(save) or not any(os.scandir(save)):
        return None
    bdir = os.path.join(data, 'backups')
    os.makedirs(bdir, exist_ok=True)
    stamp = f'{datetime.datetime.now():%Y%m%d-%H%M%S}'
    # <nn>: after the highest of this second's, so the names keep sorting by age (a pruned low number is not reused)
    same = [os.path.basename(p)[len(f'save-{stamp}-'):][:2] for p in glob.glob(os.path.join(bdir, f'save-{stamp}-*'))]
    k = min(99, max([int(x) for x in same if x.isdigit()], default=-1) + 1)
    dst = os.path.join(bdir, f'save-{stamp}-{k:02d}-{why}')
    shutil.rmtree(dst + '.tmp', ignore_errors=True)
    try:
        shutil.copytree(save, dst + '.tmp')
        os.rename(dst + '.tmp', dst)
    except OSError as e:
        shutil.rmtree(dst + '.tmp', ignore_errors=True)
        say(f'save backup failed ({e})')
        return None
    autos = sorted(glob.glob(os.path.join(bdir, 'save-*-auto')))
    for old in autos[:max(0, len(autos) - KEEP_AUTO_BACKUPS)]:
        shutil.rmtree(old, ignore_errors=True)
    return dst


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
    env['TOTK_MODS_DIR'] = ';'.join(mods)
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


def play(pkg):
    global _LOG
    pkg = os.path.abspath(pkg)
    data, cache = user_dirs()
    os.makedirs(cache, exist_ok=True)
    _LOG = os.path.join(cache, 'launcher.log')
    import fcntl   # (Linux only; the tests import this file on Windows too)
    lock = open(os.path.join(cache, 'launcher.lock'), 'w')
    try:   # (one game at a time: two would share the save and the caches)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        tell('the game is already running')
        return 1
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


def main(argv=None):
    a = list(sys.argv[1:] if argv is None else argv)
    if a and a[0] == '--env':
        pkg = os.path.abspath(a[1] if len(a) > 1 else os.path.dirname(os.path.abspath(__file__)))
        for k, v in sorted(game_env(pkg)[2].items()):
            if k.startswith(('TOTK_', 'NXR_', 'LARECOMP_')):
                print(f'{k}={v}')
        return 0
    return play(a[0] if a else os.path.dirname(os.path.abspath(__file__)))


if __name__ == '__main__':
    sys.exit(main())
