"""Builder: drop your own dump and keys file (and the firmware, for Korean / Chinese text), get the playable folder.

    Setup.exe / Builder.exe (drag the three onto its window), or:
    py tools/builder/builder.py <dump> <keys file> [<firmware folder|.zip>]   (any order) [--work DIR] [--out DIR] [--dry-run]
    py tools/builder/builder.py <the three> --install DIR    (Setup's layout: fetches the compilers itself, see below)

Everything is made here, on your PC, from files you dumped from your own console: none of it is in this repository.
The keys file is only passed on as a path and is never printed or copied.

--install DIR (what Setup.exe runs): DIR/tools gets the compilers and libraries (tools/builder/toolchain.py: LLVM,
Microsoft's C++ libraries + Windows SDK unless Visual Studio is installed, the Python packages), DIR/work the work
folder, DIR/game the playable folder; older playable folders there are removed once a new one is made (saves and
settings live in %APPDATA%, not in it). Step 0 'tools' runs first.

Steps (each one is skipped when its output already exists and is still current, so a second run continues where the
first stopped, and a run after a `git pull` redoes only what changed):
  1. extract   dump + keys -> <work>/extracted/v100 (code and data), every hash verified; with the firmware also its
               shared system fonts, which only Korean and Chinese text needs (required when Windows is in either)
               (pipeline/build.py --steps exefs,romfs,fonts,ready)
  2. romfs     romfs.bin proven equal to the extracted folder, so the game reads its data from one file (faster loads)
  3. images    the code modules decompressed -> <work>/engine/img/<id> (checks the supported version, 1.0.0)
  4. patches   our patches (high frame rates, real-time speed, PC controls...) exported for that image
  5. build     the recompiled code + engine -> the executable (the long step: about 25-45 min the first time)
  6. sets      the game-patch sets the Options menu switches between -> <work>/overlays
  7. overlay   the engine's data overlay, its aspect-ratio copies and the PC key glyphs
  8. package   the playable folder with its launcher -> <out>/

Needs: Python 3.11+ (with zstandard, pillow, fonttools, numpy, cryptography, lz4, skia-pathops), git (a clone of this
repository, not a ZIP), LLVM, CMake, Ninja, Visual Studio Build Tools (C++), the Vulkan SDK, and the internet once
(build.py's own venv installs capstone, lz4 and ninja)."""
import argparse
import glob
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME_EXT = ('.nsp', '.xci')
RELEASE_TOOLS = {'clang': '23.1.1', 'vulkan_sdk': '1.4.357.0'}   # what this repo's releases are built with
FIRST_RUN_GB, RESUME_GB = 60, 30   # free space the work folder needs: a first run; the game already extracted
# Without the firmware there are no shared system fonts, which Korean and Chinese text needs: the runtime (hle_pl) then
# shows Korean / Chinese text in a Windows font for the language (English only on a Windows without one). The game's
# language follows settings.ini's game.language, else Windows' display language.
NO_FIRMWARE = ('no firmware: fine for every language but Korean and Chinese, which need your console firmware\'s fonts '
               '(without them a Windows font stands in). Add the firmware and build again for them.')
NO_FIRMWARE_CJK = ('Without the firmware, Korean and Chinese text uses a Windows font instead of your console\'s own '
                   'fonts. Add the firmware (its folder or .zip) and build again for the right fonts.')


def cjk_windows():
    """The game's language is Korean or Chinese: settings.ini's game.language (ko, zh-Hans, zh-Hant), or, empty or
    absent, Windows' display language, as the runtime picks it (pc_lang / hle_pl)"""
    import ctypes
    ini = os.path.join(os.environ.get('APPDATA', ''), 'nxrecomp', 'TotK-engine', 'settings.ini')
    try:
        with open(ini, encoding='utf-8', errors='replace') as f:
            for ln in f:
                k, _, v = ln.split('#', 1)[0].partition('=')
                if k.strip() == 'game.language' and v.strip():
                    return v.strip().lower().startswith(('ko', 'zh'))
    except OSError:
        pass
    return ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff in (0x04, 0x12)   # LANG_CHINESE, LANG_KOREAN


TOOLS_GB = 5         # --install: LLVM's part, Python, git, Microsoft's C++ libraries (~3 GB) and their downloads
EXTRACT_GB = 40      # --until extract / romfs: the game's files alone (RomFS folder + romfs.bin, ~34 GB)
MIN_RAM_GB = 7.5     # an "8 GB" PC reports ~7.8
# The release PC's game-patch sets: every value the Options sliders offer (totk_engine.c: texture cache 1..3 by 0.5,
# grass 0.25..4 by 0.25, shadow and fog 0.5..4 by 0.5; 1 is the game's own, no set); outputs, aspects, scenes and fast
# travel at games.totk.overlays' defaults
OVERLAY_SETS = ['--texture-cache', '1.5,2,2.5,3',
                '--grass', '0.25,0.5,0.75,1.25,1.5,1.75,2,2.25,2.5,2.75,3,3.25,3.5,3.75,4',
                '--shadow', '0.5,1.5,2,2.5,3,3.5,4', '--fog', '0.5,1.5,2,2.5,3,3.5,4']
HEARTBEAT_S = 60     # a step silent this long prints a 'still working' line (step 4 can be quiet for minutes)


def say(msg):
    print(f'[Builder] {msg}', flush=True)


def fail(msg):
    say('ERROR: ' + msg)
    sys.exit(1)


def classify(paths):
    """The dropped files, by what they are: the game dump, the keys file and, optional, the firmware (folder or
    .zip)."""
    game = keys = firmware = None
    for p in paths:
        p = os.path.abspath(p.strip('"'))
        low = p.lower()
        if not os.path.exists(p):
            fail(f'not found: {p}')
        if os.path.isfile(p) and low.endswith(GAME_EXT):
            game = p
        elif os.path.isfile(p) and low.endswith('.keys'):
            if os.path.basename(low) == 'title.keys':
                continue                     # (not needed: the NSP's own ticket is used)
            keys = p
        elif os.path.isdir(p) or (os.path.isfile(p) and low.endswith('.zip')):
            firmware = p
        else:
            fail(f'unknown input: {p} (expected the dump, the keys file and the firmware: folder or .zip)')
    missing = [n for n, v in (('the dump', game), ('the keys file', keys)) if not v]
    if missing:
        fail('missing: ' + ', '.join(missing) + '. Drop them onto the window.')
    return game, keys, firmware


def check_extraction(v100):
    """The extraction's own report says the RomFS matched the supported version's hash, and the folder holds every
    file it lists (a run that ends 'ok' with an empty or cut RomFS would build a game that cannot start)."""
    try:
        with open(os.path.join(v100, 'romfs-report.json'), encoding='utf-8') as f:
            rep = json.load(f)
    except (OSError, ValueError):
        fail('the extraction left no RomFS report: delete the work folder\'s extracted folder and start again')
    if rep.get('status') != 'pass' or not rep.get('expected_match'):
        fail('the RomFS does not match the supported version (1.0.0): use a dump of version 1.0.0')
    n = sum(len(files) for _, _, files in os.walk(os.path.join(v100, 'romfs')))
    if n != rep.get('file_count'):
        fail(f'the RomFS folder holds {n} of its {rep.get("file_count")} files: delete the work folder\'s extracted '
             'folder and start again')


def check_pc():
    """What the built game needs from this PC (problems, not notes: building would waste an hour)"""
    import ctypes
    problems = []
    if not ctypes.windll.kernel32.IsProcessorFeaturePresent(40):   # PF_AVX2_INSTRUCTIONS_AVAILABLE
        problems.append('this processor has no AVX2, which the game code is compiled for (x86-64-v3: Intel 2013+ '
                        'Haswell, AMD 2015+ Excavator / Ryzen)')
    if not os.path.isfile(os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'vulkan-1.dll')):
        problems.append('no Vulkan driver (vulkan-1.dll missing): install the current driver for your graphics card')
    return problems


def check_keys(keys):
    """The keys file must hold the expected entries; only names are looked at, values are never read out."""
    names = set()
    with open(keys, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '=' in line:
                names.add(line.split('=', 1)[0].strip().lower())
    need = ['header_key', 'key_area_key_application_00', 'titlekek_00']
    lacking = [n for n in need if n not in names]
    if lacking:
        fail(f'{os.path.basename(keys)} does not look like a complete keys file. '
             'Use the one from your own console.')


def firmware_dir(firmware, work):
    """A firmware folder must hold .nca files; a .zip is unpacked once into <work>/firmware."""
    if os.path.isfile(firmware):
        dest = os.path.join(work, 'firmware')
        if not glob.glob(os.path.join(dest, '*.nca')) + glob.glob(os.path.join(dest, '*', '*.nca')):   # (a zip may hold one folder)
            say(f'unpacking the firmware into {dest} ...')
            shutil.rmtree(dest, ignore_errors=True)
            with zipfile.ZipFile(firmware) as z:
                z.extractall(dest)
        firmware = dest
    ncas = glob.glob(os.path.join(firmware, '*.nca')) + glob.glob(os.path.join(firmware, '*', '*.nca'))
    if not ncas:
        fail(f'{firmware} does not look like a firmware: use its folder (or its .zip) as is')
    return os.path.dirname(ncas[0])


def check_tools(managed=False):
    """What a fresh PC needs installed; a clear list instead of a failure halfway through the build. Returns
    (missing, notes, env): notes are differences from the PC the releases are built on, env what the build must be
    told about this PC (where LLVM is). managed (--install): toolchain.py fetches the compilers and packages, so only
    git and the clone are checked here."""
    missing, notes, env = [], [], {}
    sys.path.insert(0, REPO)
    if managed:
        if not shutil.which('git') or not os.path.exists(os.path.join(REPO, '.git')):
            missing.append(f'git and a clone of the repository in {REPO} (start Setup again: it fetches both)')
        return missing, notes, env
    llvm = r'C:\Program Files\LLVM'
    if not os.path.isfile(os.path.join(llvm, 'bin', 'clang-cl.exe')) and shutil.which('clang-cl'):
        llvm = os.path.dirname(os.path.dirname(shutil.which('clang-cl')))   # (installed elsewhere: tell build.py)
        env['NXRECOMP_LLVM_DIR'] = os.environ['NXRECOMP_LLVM_DIR'] = llvm
    try:   # the build's own search (Visual Studio env + LLVM + PATH), not just PATH
        from pipeline.toolchain import native_env
        native_env()
    except ImportError:
        pass   # (a missing Python package: the list below names it)
    except BaseException as e:   # (require() exits with the tool's name and the winget id to install it)
        missing.append(f'compilers: {e} (winget install LLVM.LLVM Kitware.CMake Ninja-build.Ninja, '
                       'and Visual Studio Build Tools with C++)')
    else:
        try:
            ver = subprocess.run([os.path.join(llvm, 'bin', 'clang-cl.exe'), '--version'], capture_output=True,
                                 text=True).stdout.split('version ')[1].split()[0]
        except (OSError, IndexError):
            ver = '?'
        if ver != RELEASE_TOOLS['clang']:
            notes.append(f'clang {ver}: the releases are built with {RELEASE_TOOLS["clang"]}; it builds, but the code '
                         f'is not byte for byte theirs (winget install LLVM.LLVM --version {RELEASE_TOOLS["clang"]})')
    sdk = os.environ.get('VULKAN_SDK', '')
    sdk_ver = os.path.basename(sdk.rstrip('\\/'))
    if not all(os.path.isfile(os.path.join(sdk, 'Lib', f)) for f in ('vulkan-1.lib', 'shaderc_combined.lib')):
        missing.append('the Vulkan SDK (VULKAN_SDK not set, or no Lib\\shaderc_combined.lib in it): winget install '
                       'KhronosGroup.VulkanSDK, then start Builder again')
    elif sdk_ver != RELEASE_TOOLS['vulkan_sdk']:
        notes.append(f'Vulkan SDK {sdk_ver}: the releases use {RELEASE_TOOLS["vulkan_sdk"]} (its shader compiler is '
                     'linked in)')
    if not shutil.which('git'):
        missing.append('git: winget install Git.Git')
    elif not os.path.exists(os.path.join(REPO, '.git')):
        missing.append(f'{REPO} is not a git clone (a downloaded ZIP): the build exports its sources with git; '
                       'clone the repository instead')
    if sys.version_info < (3, 11):
        missing.append('Python 3.11 or newer')
    for mod, pkg in (('zstandard', 'zstandard'), ('PIL', 'pillow'), ('fontTools', 'fonttools'), ('numpy', 'numpy'),
                     ('cryptography', 'cryptography'), ('lz4', 'lz4'), ('pathops', 'skia-pathops')):
        try:
            __import__(mod)
        except ImportError:
            missing.append(f'the Python package {pkg}: py -m pip install {pkg}')
    return missing, notes, env


def follow_gui(pid):
    """When Builder.exe goes (closed, crashed, killed), this build goes with it: its whole tree, compilers included.
    (Builder.exe's kill-on-close job does not reach every Python: the Microsoft Store one starts outside it.)"""
    import ctypes
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
    if not h:
        return

    def watch():
        k32.WaitForSingleObject(h, 0xFFFFFFFF)
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(os.getpid())], capture_output=True)
    threading.Thread(target=watch, daemon=True).start()


def memory_gb():
    """(total, available) physical memory in GB"""
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong)] + \
                   [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'tpage', 'apage', 'tvirt', 'avirt', 'aext')]
    m = Status()
    m.dwLength = ctypes.sizeof(Status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return 16.0, 8.0
    return m.total / 2**30, m.avail / 2**30


def jobs_for(total_gb, cores):
    """(game, runtime) parallel compiles this PC's memory holds. Sized by RAM, not cores: a many-core PC with little
    memory otherwise runs out of it (other ports' first-build failures). A game unit peaks near 0.5 GB and a few giant
    ones near 2.2 GB (build.py, which also holds back when free commit runs low), a runtime C++ unit near 1.5 GB;
    4 GB stay for Windows and the rest of the PC."""
    usable = max(1.0, total_gb - 4)
    return max(2, min(cores, int(usable))), max(1, min(cores, 6, int(usable / 1.5)))


def filesystem(path):
    """the file system of the drive path really is on (through Setup's junction): 'NTFS', 'FAT32', 'exFAT'..."""
    import ctypes
    root = os.path.splitdrive(os.path.realpath(path))[0] + os.sep
    name = ctypes.create_unicode_buffer(64)
    ok = ctypes.windll.kernel32.GetVolumeInformationW(root, None, 0, None, None, None, name, 64)
    return name.value if ok else ''


def free_gb(path):
    os.makedirs(path, exist_ok=True)
    return shutil.disk_usage(path).free / 2**30


def run(step, cmd, env, dry, beat=HEARTBEAT_S, quiet=False):
    """Run one step, its output streamed line by line (the window shows the last lines), with a 'still working'
    line after `beat` seconds of silence. Returns the whole stdout+stderr. quiet: a step whose output is a JSON
    result for us (build.py key), shown only when it fails."""
    say(f'step {step}: ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    if dry:
        return ''
    p = subprocess.Popen(cmd, cwd=REPO, env=env, text=True, encoding='utf-8', errors='replace',
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: ([lines.put(ln) for ln in p.stdout], lines.put(None)), daemon=True).start()
    out, t0, last = [], time.monotonic(), time.monotonic()
    while True:
        try:
            ln = lines.get(timeout=5)
        except queue.Empty:
            if time.monotonic() - last >= beat:   # (not 'step N/M': the window takes those for a new step)
                say(f'{step.split()[-1]}: still working ({(time.monotonic() - t0) / 60:.0f} min)')
                last = time.monotonic()
            continue
        if ln is None:
            break
        out.append(ln)
        if ln.strip() and not quiet:
            sys.stdout.write(ln if ln.endswith('\n') else ln + '\n')
            sys.stdout.flush()
            last = time.monotonic()
    if p.wait():
        if quiet:
            sys.stdout.write(''.join(out)[-4000:])
        fail(f'step {step} failed (exit code {p.returncode}); the end of its output is above.')
    return ''.join(out)


def result(text):
    """A step's JSON result: the last object that starts a line and runs to the end of the output (one line or
    indented; log lines before it may hold braces too)."""
    lines = text.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].startswith('{'):
            try:
                return json.loads('\n'.join(lines[i:]))
            except ValueError:
                pass
    sys.stdout.write(text[-4000:])
    fail('a step gave no result (JSON); the end of its output is above')


def git_id(spec):
    """the git object id of a path at HEAD (a step's inputs: redo the step when they change after a git pull)"""
    r = subprocess.run(['git', '-C', REPO, 'rev-parse', f'HEAD:{spec}'], capture_output=True, text=True)
    return r.stdout.strip()[:12] or 'unknown'


def done_with(marker, inputs):
    """a step's output is there and was made from these inputs"""
    try:
        with open(marker, encoding='utf-8') as f:
            return json.load(f) == inputs
    except (OSError, ValueError):
        return False


def mark(marker, inputs):
    with open(marker, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(inputs, f, indent=1)


def newest_package(out):
    folders = sorted((d for d in glob.glob(os.path.join(out, 'TotK-PC-*')) if os.path.isdir(d) and not d.endswith('.tmp')),
                     key=os.path.getmtime)
    return folders[-1] if folders else None


def prune(out, keep):
    """--install: the playable folders an update replaced (each holds only the exe and the launcher; the game's data
    and the player's saves and settings are elsewhere)"""
    for d in glob.glob(os.path.join(out, 'TotK-PC-*')):
        if os.path.isdir(d) and os.path.normcase(d) != os.path.normcase(keep):
            shutil.rmtree(d, ignore_errors=True)   # (one in use, the game still open: stays until the next update)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='the dump, the keys file and, optional, the firmware (folder or .zip)')
    ap.add_argument('--work', default=os.path.join(os.path.dirname(REPO), 'build-work'),
                    help=f'where everything generated goes (about {FIRST_RUN_GB} GB the first time)')
    ap.add_argument('--out', help='where the playable folder goes (default: <work>/release)')
    ap.add_argument('--install', help='Setup\'s layout: DIR/tools (compilers, fetched), DIR/work, DIR/game')
    ap.add_argument('--until', choices=('tools', 'extract', 'romfs'),
                    help=f'stop after this step (romfs: only the game files, about {EXTRACT_GB} GB)')
    ap.add_argument('--dry-run', action='store_true', help='check the inputs and print the steps, build nothing')
    ap.add_argument('--gui-pid', type=int, help=argparse.SUPPRESS)   # (Builder.exe: stop with it)
    a = ap.parse_args(argv)
    if a.gui_pid:
        follow_gui(a.gui_pid)

    game, keys, firmware = classify(a.inputs)
    check_keys(keys)
    install = os.path.abspath(a.install) if a.install else None
    work = os.path.join(install, 'work') if install else os.path.abspath(a.work)
    out = os.path.join(install, 'game') if install else os.path.abspath(a.out or os.path.join(work, 'release'))
    tools = os.path.join(install, 'tools') if install else None
    fw = None if not firmware else firmware_dir(firmware, work) if not a.dry_run or os.path.isdir(firmware) else firmware
    say(f'dump: {os.path.basename(game)} | keys: {os.path.basename(keys)} | firmware: {fw or "none"}')
    if not fw and not os.path.isfile(os.path.join(work, 'extracted', 'v100', 'fonts-report.json')):
        say(NO_FIRMWARE_CJK if cjk_windows() else NO_FIRMWARE)
    say(f'work folder: {work} | output: {out}')

    v100 = os.path.join(work, 'extracted', 'v100')
    extracted = os.path.exists(os.path.join(v100, 'READY.json'))
    problems, notes, tool_env = check_tools(managed=bool(install))
    need = RESUME_GB if extracted else FIRST_RUN_GB   # (a resumed run: the game's files are already out)
    if a.until:
        need = 0 if extracted or a.until == 'tools' else EXTRACT_GB
    if install and not os.path.isfile(os.path.join(tools, 'llvm', '.toolchain.json')):
        need += TOOLS_GB
    if free_gb(work) < need:
        problems.append(f'disk space: about {need} GB free needed in {work} ({free_gb(work):.0f} GB free)')
    fs = filesystem(work)
    if fs.upper().startswith('FAT') and a.until != 'tools':
        problems.append(f'{os.path.realpath(work)[:2]} is formatted {fs}, which cannot hold files over 4 GB (the game '
                        'data has one of 17 GB): choose a folder on an NTFS drive')
    problems += check_pc()
    ram, _ = memory_gb()
    if ram < MIN_RAM_GB:
        problems.append(f'memory: the build needs a PC with at least 8 GB of RAM ({ram:.1f} GB here)')
    game_jobs, rt_jobs = jobs_for(ram, os.cpu_count() or 2)
    say(f'{os.cpu_count()} cores, {ram:.0f} GB RAM: up to {game_jobs} compiles at once')
    for note in notes:
        say('note: ' + note)
    if problems:
        say('install these first:')
        for p in problems:
            say('  - ' + p)
        if not a.dry_run:
            sys.exit(1)

    env = dict(os.environ, **tool_env, NXRECOMP_WORK=work, NXR_SOLO_PC='1', PYTHONIOENCODING='utf-8',
               NXR_GAME_JOBS_MAX=str(game_jobs), NXR_BUILDER_PACKAGE='1')
    py = sys.executable
    build = os.path.join(REPO, 'tools', 'engine', 'build.py')
    n = 8

    # 0. tools (--install): the compilers and libraries, fetched into <install>/tools
    if install:
        say(f'step 0/{n} tools')
        if a.dry_run:
            say('(--dry-run: nothing fetched)')
        else:
            import toolchain
            try:
                env = toolchain.apply(toolchain.ensure(tools), env)
            except toolchain.ToolchainError as e:
                fail(str(e))
        if a.until == 'tools':
            say('DONE (--until tools)')
            return

    # 1. extract (pipeline/build.py verifies every hash and pins the supported version's)
    #    (the fonts only with the firmware; given on a later run, they are added to an extraction made without)
    fonts = os.path.isfile(os.path.join(v100, 'fonts-report.json'))
    steps = ['exefs', 'romfs'] if a.dry_run or not extracted else []
    if fw and not fonts:
        steps.append('fonts')
    if steps:
        run(f'1/{n} extract', [py, '-m', 'pipeline.build', '--nsp', game, '--keys', keys, '--work', work,
                               '--steps', ','.join(steps + ['ready'])] + (['--firmware', fw] if fw else []), env,
            a.dry_run)
    if a.until == 'extract':
        say('DONE (--until extract)')
        return
    # 2. the RomFS as one file: once proven equal to the folder the game reads romfs.bin (faster loads; package.py
    #    points the launcher at it only then)
    if a.dry_run or not os.path.exists(os.path.join(v100, 'romfs.bin.verified.json')):
        if not a.dry_run:
            check_extraction(v100)
        run(f'2/{n} romfs', [py, '-I', os.path.join(REPO, 'tools', 'engine', 'romfs_image_verify.py')], env, a.dry_run)
    if a.until == 'romfs':
        say('DONE (--until romfs)')
        return
    # 3. images (build.py key computes them; it refuses an unsupported version)
    k = run(f'3/{n} images', [py, build, 'key'], env, a.dry_run, quiet=True)
    img = '<work>/engine/img/<id>' if a.dry_run else result(k)['img']
    # 4. patches: funcs.py on the clean image (kept per image and funcs.py version), then the default patch set,
    #    exported every run (seconds; a git pull may have changed the patches)
    pdir = os.path.join(work, 'engine', 'patches', 'builder')
    funcs = os.path.join(pdir, f'funcs_{os.path.basename(img)}_{git_id("engine/tools/funcs.py")}.json')
    vpy = os.path.join(work, 'engine', 'venv', 'Scripts', 'python.exe')
    os.makedirs(pdir, exist_ok=True)
    if a.dry_run or not os.path.exists(funcs):
        run(f'4/{n} patches', [vpy if os.path.exists(vpy) else py, '-I',
                               os.path.join(REPO, 'engine', 'tools', 'funcs.py'), os.path.join(img, 'main'),
                               funcs + '.new'], env, a.dry_run)   # (funcs.py takes the prefix: main.json + main.bin)
        if not a.dry_run:
            os.replace(funcs + '.new', funcs)
    run(f'4/{n} patches', [py, '-m', 'games.totk.patches', 'engine', '--image', os.path.join(img, 'main.bin'),
                           '--funcs', funcs, '--out', pdir, '--features', 'default'], env, a.dry_run)
    patches = os.path.join(pdir, 'engine-patches.json')
    # 5. build (resumable: build.py reuses every object already compiled)
    say('step 5 is the long one: about 25-45 min the first time, minutes after that')
    run(f'5/{n} build', [py, build, 'game', '--patches', patches, '--jobs', str(game_jobs), '--jobs-rt', str(rt_jobs)],
        env, a.dry_run)
    k = run(f'5/{n} build', [py, build, 'key', '--patches', patches], env, a.dry_run, quiet=True)
    key = '<key>' if a.dry_run else result(k)['key']
    # 6. the game-patch sets the Options menu switches between (grass / shadow / fog distance, texture cache, output
    #    and aspect sizes, fast travel): every value its sliders offer, as on the release PC
    sets = os.path.join(work, 'overlays')
    sets_in = {'args': OVERLAY_SETS, 'code': git_id('games/totk/overlays')}
    if a.dry_run or not done_with(os.path.join(sets, 'builder.json'), sets_in):
        run(f'6/{n} sets', [py, '-m', 'games.totk.overlays', '--romfs', v100, '--out', sets, *OVERLAY_SETS], env,
            a.dry_run)
        if not a.dry_run:
            mark(os.path.join(sets, 'builder.json'), sets_in)
    # 7. the engine's data overlay, its aspect copies and their PC key glyph banks (build.py overlay makes them all)
    t7, bank = time.time(), os.path.join(work, 'engine', 'glyphs', 'engine+glyphs')
    run(f'7/{n} overlay', [py, build, 'overlay', '--force'], env, a.dry_run)   # (~1.5 min; after a git pull it changes)
    if not a.dry_run and not (os.path.isdir(bank) and os.path.getmtime(bank) >= t7 - 2):   # (made by this step)
        fail('no PC key glyph bank was made: the overlay step says why above')
    # 8. package (package.py points the launcher at <work>/overlays itself)
    run(f'8/{n} package', [py, os.path.join(REPO, 'engine', 'launcher', 'win', 'package.py'), '--key', key,
                           '--out', out], env, a.dry_run)
    if a.dry_run:
        say('check done (--dry-run): nothing was built')
        return
    newest = newest_package(out)
    if not newest:
        fail(f'no playable folder in {out} after packaging')
    if install:
        prune(out, newest)
    say('DONE: ' + os.path.join(newest, 'TotK Launcher.exe'))


if __name__ == '__main__':
    main()
