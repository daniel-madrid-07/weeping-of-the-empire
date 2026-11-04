"""Builder: drop your own dump and keys file (and the firmware, for Korean / Chinese text), get the playable folder.

    Setup.exe / Builder.exe (drag the three onto its window), or:
    py tools/builder/builder.py <dump> <keys file> [<firmware folder|.zip>]   (any order) [--work DIR] [--out DIR] [--dry-run]
    py tools/builder/builder.py <the three> --install DIR    (Setup's layout: fetches the compilers itself, see below)
    py tools/builder/builder.py <the three> --update <the 1.4.3 update dump>   (builds version 1.4.3, see UPDATE below)
                                [--cpu auto|v3|v2]

Everything is made here, on your PC, from files you dumped from your own console: none of it is in this repository.
The keys file is only passed on as a path and is never printed or copied.

--install DIR (what Setup.exe runs): DIR/tools gets the compilers and libraries (tools/builder/toolchain.py: LLVM,
Microsoft's C++ libraries + Windows SDK unless Visual Studio is installed, the Python packages), DIR/work the work
folder, DIR/game the playable folder; older playable folders there are removed once a new one is made (saves and
settings live in %APPDATA%, not in it). Step 0 'tools' runs first.

Steps (each one is skipped when its output already exists and is still current, so a second run continues where the
first stopped, and a run after a `git pull` redoes only what changed; <work>/builder-steps.json stamps each finished
step with its inputs - dump, keys, Setup version, key - and a reused one says 'reusing step N (done at ...)'):
  1. extract   dump + keys -> <work>/extracted/v100 (code, and the data: since v0.1.8 it stays in the dump, which the
               game reads in place through romfs-source.txt, so no 17 GB copy; --copy-romfs writes it out as romfs.bin,
               for a player who will move or delete the dump), every hash verified; with the firmware also its shared
               system fonts, which only Korean and Chinese text needs (required when Windows is in either)
               (pipeline/build.py --steps exefs,romfs,fonts,ready --no-unpack [--romfs-in-dump]). An install made
               the other way is switched over (a second full pass over the dump; romfs.bin removed or written)
  2. romfs     the game's data recorded (from the extraction's proof; an older install's unpacked folder is compared
               byte for byte instead), so the game reads it in place
  3. images    the code modules decompressed -> <work>/engine/img/<id> (checks the supported version, 1.0.0)
  4. patches   our patches (high frame rates, real-time speed, PC controls...) exported for that image
  5. build     the recompiled code + engine -> the executable (the long step: about 25-45 min the first time); for
               this PC's processor: --cpu auto (default) builds for x86-64-v3 (AVX2) when the processor has AVX2, else
               for x86-64-v2 (Sandy / Ivy Bridge, AMD FX, Pentium / Celeron: runs there, slower: no FMA instructions)
  6. sets      the game-patch sets the Options menu switches between -> <work>/overlays
  7. overlay   the engine's data overlay, its aspect-ratio copies and the PC key glyphs
  8. package   the playable folder with its launcher -> <out>/

Needs: Python 3.11+ (with zstandard, pillow, fonttools, numpy, cryptography, lz4, skia-pathops), git (a clone of this
repository, not a ZIP), LLVM, CMake, Ninja, Visual Studio Build Tools (C++), the Vulkan SDK, and the internet once
(build.py's own venv installs capstone, lz4 and ninja)."""
import argparse
import glob
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Linux (linux-port, 2026-11-02): the same steps natively; what Windows answers through kernel32 / the registry comes
# from host_linux.py (each such function starts with its Linux line), the toolchain is toolchain.py's Linux assets, the
# package engine/launcher/win/package.py's Linux layout, the run folders the XDG ones (launcher_linux.py)
LINUX = sys.platform.startswith('linux')
if LINUX:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import host_linux
EXE = 'totk' if LINUX else 'totk.exe'
GAME_EXT = ('.nsp', '.xci', '.nsz', '.xcz', '.nup')   # (.nsz / .xcz: nsz-compressed, pipeline/nx/ncz.py; .nup: nup.py)
RELEASE_TOOLS = {'clang': '23.1.1', 'vulkan_sdk': '1.4.357.0'}   # what this repo's releases are built with
# free space the work folder needs, from its measured parts (setup-speed, 2026-11-01: cold install 2b7c4cb2 on this
# repo's Builder, sized every 60 s): a first run's peak was romfs.bin 17.1 GB + the code and fonts 0.1 + the build's
# work folder ~2.8 (generated C++ 0.94, objects without line tables ~0.3, the cache = the same files, exe 0.25, images,
# overlays, glyphs, venv, profile, DLSS ~1.1, the package 0.25) = ~20 GB, + 5 for the compiles' temp files and a
# pagefile that grows on a small-memory PC. Since v0.1.8 the game's data stays in the dump (romsource.py): romfs.bin's
# 17 GB only with --copy-romfs (COPY_ROMFS_GB). A resumed run (the game extracted): a new key beside the old one until
# the end's tidy (~2 + ~2 GB) + the same 5. (TOOLS_GB on top when the tools are not there yet.)
FIRST_RUN_GB, RESUME_GB = 8, 9
COPY_ROMFS_GB = 17   # --copy-romfs: romfs.bin (the dump's data written out)
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
    if LINUX:
        return host_linux.cjk()
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
EXTRACT_GB = 3       # --until extract / romfs: the game's files alone (the code; + COPY_ROMFS_GB with --copy-romfs)
UPDATE_UNPACK_GB = 18   # --update: 1.4.3's extraction still unpacks its RomFS folder (package.py points that version
                        # at it): 304k files, 17.0 GB + their cluster slack, on top of a first run's or --until's need
# an "8 GB" PC reports 7.3-7.8 GB usable (an iGPU's reserve: ram-diet; 7.5 refused such laptops, though the README says
# 8 GB).
WARN_RAM_GB = 7.5
SMALL_RAM_GB = 8.0          # (build.py's SMALL_RAM_GB / HOT_GIANT_PEAK_GB: one giant at a time and what fits beside it)
HOT_GIANT_PEAK_GB = 3.2
# The release PC's game-patch sets: every value the Options sliders offer (totk_engine.c: texture cache 1..3 by 0.5,
# grass 0.25..4 by 0.25, shadow and fog 0.5..4 by 0.5; 1 is the game's own, no set); outputs, aspects, scenes and fast
# travel at games.totk.overlays' defaults
OVERLAY_SETS = ['--texture-cache', '1.5,2,2.5,3',
                '--grass', '0.25,0.5,0.75,1.25,1.5,1.75,2,2.25,2.5,2.75,3,3.25,3.5,3.75,4',
                '--shadow', '0.25,0.5,0.75,1.25,1.5,1.75,2,2.25,2.5,2.75,3,3.25,3.5,3.75,4',
                '--fog', '0.5,1.5,2,2.5,3,3.5,4',
                '--hud', 'corner,center']   # (the ultrawide sets with the HUD at the edges and in the middle 16:9)
HEARTBEAT_S = 60     # a step silent this long prints a 'still working' line (step 4 can be quiet for minutes)


NESTED = []   # (non-empty while an --update install also brings its 1.0.0 game up to date: main_100)


def say(msg):
    if NESTED:   # (that pass's lines: '1.0.0: ...', its steps as 'step N of M', so Setup's progress (StepLine 'step N/M')
        msg = '1.0.0: ' + re.sub(r'\bstep (\d+)/(\d+)', r'step \1 of \2', msg)   # does not go back)
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
            if game:
                fail(f'two game files: {os.path.basename(game)} and {os.path.basename(p)}. Give the base game as the dump '
                     'and the update with --update (Setup: its Update row)')
            game = p
        elif os.path.isfile(p) and low.endswith('.keys'):
            if os.path.basename(low) == 'title.keys':
                continue                     # (not needed: the NSP's own ticket is used)
            keys = p
        elif os.path.isdir(p) or (os.path.isfile(p) and low.endswith('.zip')):
            firmware = p
        else:
            fail(f'unknown input: {p} (expected the dump, the keys file and the firmware: folder or .zip)')
    # (a NUP holds the game unencrypted: no keys for it; an update NSP or a firmware still needs them, said in main)
    missing = [n for n, v in (('the dump', game), ('the keys file', keys or (game and nup_dump(game)))) if not v]
    if missing:
        fail('missing: ' + ', '.join(missing) + '. Drop them onto the window.')
    return game, keys, firmware


def unpacked(v):
    """the extraction has an unpacked RomFS folder with files in it (an install from before --no-unpack; the runtime
    creates an empty mount folder by itself)"""
    try:
        with os.scandir(os.path.join(v, 'romfs')) as it:
            return any(True for _ in it)
    except OSError:
        return False


def drop_partial_folder(v):
    """An unpacked RomFS folder no passing report describes (an older Setup's extraction stopped while unpacking) is
    removed before the extraction runs again without one: left there, the build tools would read it as the game's
    files and step 2 would compare romfs.bin against a cut copy. A complete one (its report says tree 'romfs') stays."""
    try:
        with open(os.path.join(v, 'romfs-report.json'), encoding='utf-8') as f:
            rep = json.load(f)
    except (OSError, ValueError):
        rep = {}
    if unpacked(v) and not (rep.get('status') == 'pass' and rep.get('tree') == 'romfs'):
        say('removing an unfinished copy of the game files (an earlier extraction stopped while unpacking them)')
        shutil.rmtree(os.path.join(v, 'romfs'), ignore_errors=True)


def check_extraction(v100, version='1.0.0'):
    """The extraction's own report says the RomFS matched the supported version's hash, and the folder holds every
    file it lists, or, without a folder, romfs.bin is still the size it was written at (a run that ends 'ok' with an
    empty or cut RomFS would build a game that cannot start)."""
    try:
        with open(os.path.join(v100, 'romfs-report.json'), encoding='utf-8') as f:
            rep = json.load(f)
    except (OSError, ValueError):
        fail('the extraction left no RomFS report: delete the work folder\'s extracted folder and start again')
    if rep.get('status') != 'pass' or not rep.get('expected_match'):
        fail(f'the RomFS does not match the supported version ({version}): use a dump of version 1.0.0' +
             (f' and its {version} update' if version != '1.0.0' else ''))
    if not unpacked(v100):
        if rep.get('image') == 'romfs-source.txt':   # (the data in the dump: it must still be where it was read)
            try:
                with open(os.path.join(v100, 'romfs-source.txt'), encoding='utf-8') as f:
                    src = dict(line.rstrip('\n').split('=', 1) for line in f if '=' in line and not line.startswith('#'))
                there = os.path.getsize(src['dump']) == int(src['dump_size'])
            except (OSError, ValueError, KeyError):
                src, there = {}, False
            if not there:
                fail(f'the game dump this install reads ({src.get("dump", "?")}) is not there or changed: pick it '
                     'again, or choose "copy the game files" if you will move or delete it')
            return
        img = os.path.join(v100, 'romfs.bin')
        if not os.path.isfile(img) or os.path.getsize(img) != rep.get('romfs_size'):
            fail('the game data file (romfs.bin) is missing or cut: delete the work folder\'s extracted folder and '
                 'start again')
        return
    n = sum(len(files) for _, _, files in os.walk(os.path.join(v100, 'romfs')))
    if n != rep.get('file_count'):
        fail(f'the RomFS folder holds {n} of its {rep.get("file_count")} files: delete the work folder\'s extracted '
             'folder and start again')


def check_pc():
    """What the built game needs from this PC (problems, not notes: building would waste an hour). A processor without
    AVX2 is not one: --cpu auto builds x86-64-v2 for it. Below v2 (the launcher's start-up floor: SSSE3, SSE4.1, SSE4.2
    and with it POPCNT) the game cannot run at all, so say so before the build."""
    if LINUX:
        return host_linux.check_pc()
    import ctypes
    problems = []
    if not all(ctypes.windll.kernel32.IsProcessorFeaturePresent(f) for f in (36, 37, 38)):   # PF_SSSE3, PF_SSE4_1, _2
        problems.append('this processor lacks SSE4.2 (or SSSE3 / SSE4.1), which the game needs at the least '
                        '(x86-64-v2: Intel 2008+ Nehalem, AMD 2011+ Bulldozer)')
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


KEYS_FIX = ('dump prod.keys again on the console the game was dumped from, with a current key dumper (one that knows '
            'that console\'s firmware), and choose that file')


def dump_problem(what, msg):
    """The pipeline's reason a dump / update could not be opened, as one sentence with what to do (what: 'the dump' or
    'the update dump')"""
    if 'prod.keys' in msg or 'NCA3' in msg or 'titlekek' in msg or 'key_area_key' in msg:
        return f'Your prod.keys cannot open {what} ({msg}): {KEYS_FIX}.'
    if 'FS header hash' in msg or 'NCA key area' in msg:   # (headers decrypted to nonsense: other keys, or damage)
        return (f'{what[0].upper() + what[1:]} could not be read correctly ({msg}): either your prod.keys is not the one '
                f'from the console it was dumped on ({KEYS_FIX}), or the file is damaged (copy or dump it again).')
    if 'Personalized' in msg:
        return (f'{what[0].upper() + what[1:]} has a personalized ticket (tied to one console account), which Setup cannot '
                'use: dump the game again so that it carries a common ticket.')
    if 'ticket' in msg.lower():   # (missing, of an unknown signature type, or two that disagree)
        return (f'{what[0].upper() + what[1:]} has no usable ticket ({msg}): dump the game again with its ticket '
                '(a common ticket).')
    if 'this Setup reads' in msg:   # (a NUP of a format this Setup does not read yet)
        return (f'{what[0].upper() + what[1:]} is a NUP this Setup cannot read ({msg}): choose the '
                f"{'game' if what == 'the dump' else 'update'} as an .nsp / .xci.")
    if ('Expected an XCI or NSP' in msg or 'Truncated' in msg or 'does not match' in msg or 'out of bounds' in msg or
            'NCZ' in msg or ('NUP' in msg and not msg.startswith(('The NUP', 'Expected base game')))):
        # (".nsp / .xci" for an NSZ / XCZ / NUP too: Setup's sentence is translated as it stands; the cause says which)
        return (f'{what[0].upper() + what[1:]} is not a complete .nsp / .xci file ({msg}): copy or dump it again '
                '(an interrupted copy or download leaves it short).')
    if what == 'the dump':
        return ('The dump is not Tears of the Kingdom\'s base game, version 1.0.0 (title 0100F2C0115B6000): choose the '
                f'base game\'s .nsp / .xci. The {UPDATE_VERSION} update goes in the Update row, and DLC is not used '
                f'({msg}).')
    return (f'The update dump is not Tears of the Kingdom\'s update: choose its own .nsp, or leave the Update row empty '
            f'({msg}).')


def check_dump(game, keys, update=None):
    """What stops step 1 found in seconds: the dump's and the update's container and NCA headers opened with these keys
    through the pipeline's own functions (the ones step 1 runs); a NUP's superblock, table of contents and base game
    (no keys). '' when they open; else one plain sentence. Without the pipeline's packages (a source checkout before
    step 0) nothing is checked here: step 1 says it."""
    try:
        if REPO not in sys.path:
            sys.path.insert(0, REPO)
        from pipeline.nx import container, nup
    except ImportError:
        return ''
    keyset = {}
    if keys:
        try:
            keyset = container.load_keys(keys)
        except (OSError, ValueError) as e:
            return f'{os.path.basename(keys)} cannot be read as a keys file ({e}): {KEYS_FIX}.'

    def nup_base(f, keyset, size):
        nup.base_package(nup.Nup(f, size), container.TOTK_TITLE_ID)

    def nup_update(f, keyset, size):   # (extract.py's choice: the 1.4.3 package, else the newest)
        nup.update_package(nup.Nup(f, size), container.TOTK_UPDATE_TITLE_ID, 720896)
    for path, what, opener in ((game, 'the dump', container.program_sections),
                               (update, 'the update dump', container.update_program)):
        if not path:
            continue
        if nup_dump(path):
            opener = nup_base if what == 'the dump' else nup_update
        try:
            with open(path, 'rb') as f:
                opener(f, keyset, os.path.getsize(path))
        except ValueError as e:
            return dump_problem(what, str(e))
        except OSError as e:
            return f'{what[0].upper() + what[1:]} cannot be read ({e.strerror or e}): {path}'
    return ''


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


MSVC_MIN = (14, 44)   # (toolchain.py's MSVC_PIN: VS 2022 17.14)


def msvc_too_old(env):
    """Public issue #6: the Vulkan SDK's shaderc_combined.lib calls STL helpers (__std_min_4u, __std_search_1...) an
    older MSVC lacks, and the link fails at the end of the long build. The problem, said before it starts, or None."""
    v = next((x for k, x in env.items() if k.upper() == 'VCTOOLSVERSION'), '').strip()
    m = re.match(r'(\d+)\.(\d+)', v)
    if not m or (int(m[1]), int(m[2])) >= MSVC_MIN:
        return None
    return (f'Visual Studio C++ tools: this build would use MSVC {v}, older than the 14.44 the game\'s libraries need '
            '(the link would fail). Update Visual Studio 2022 / Build Tools to 17.14 or newer (Visual Studio Installer, '
            'Update) and start again')


def check_tools(managed=False):
    """What a fresh PC needs installed; a clear list instead of a failure halfway through the build. Returns
    (missing, notes, env): notes are differences from the PC the releases are built on, env what the build must be
    told about this PC (where LLVM is). managed (--install): toolchain.py fetches the compilers and packages, so only
    git and the clone are checked here."""
    missing, notes, env = [], [], {}
    sys.path.insert(0, REPO)
    if LINUX and not managed:   # (a developer's run: the LLVM and SDK of tools/linux/env.sh, named by the environment)
        for var, what in (('NXRECOMP_LLVM_DIR', 'LLVM 23.1.1 (tools/linux/llvm_asset.sh)'),
                          ('NXR_LINUX_DEPS', 'the Linux SDK\'s deps (tools/linux/sdk.sh)')):
            if not os.path.isdir(os.environ.get(var, '')):
                missing.append(f'{var}: the folder of {what}, or use --install (Setup fetches both)')
        if not shutil.which('git'):
            missing.append('git (your distribution\'s package)')
        return missing, notes, env
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
        old = msvc_too_old(native_env()[0])
        if old:
            missing.append(old)
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
    (Builder.exe's kill-on-close job does not reach every Python: the Microsoft Store one starts outside it.)
    Linux: the Setup window (tools/linux/appimage/setup_gui.py) passes its pid too, and starts this in a process group
    of its own: a watcher thread sees that pid go and sends SIGTERM to the group (this process and the build's
    children)."""
    if LINUX:
        def watch_linux():
            while os.path.exists(f'/proc/{pid}'):
                time.sleep(1)
            os.killpg(os.getpgid(0), 15)
        threading.Thread(target=watch_linux, daemon=True).start()
        return
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
    if LINUX:
        return host_linux.memory_gb()
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong)] + \
                   [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'tpage', 'apage', 'tvirt', 'avirt', 'aext')]
    m = Status()
    m.dwLength = ctypes.sizeof(Status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return 16.0, 8.0
    return m.total / 2**30, m.avail / 2**30


def ram_warning(total_gb):
    """the warning the build goes on with for this much usable RAM, or None"""
    if total_gb < WARN_RAM_GB:
        return (f'warning: this PC has {total_gb:.1f} GB of memory usable. Below 8 GB the game is untested: the build can '
                'take hours, or stop for lack of memory and go on when Setup runs again; the game itself may not run, or '
                'may crash on fast travel. Close other apps while it builds.')
    return None


def jobs_for(total_gb, cores):
    """(game, runtime) parallel compiles this PC's memory holds. Sized by RAM, not cores: a many-core PC with little
    memory otherwise runs out of it (other ports' first-build failures). A game unit peaks near 0.5 GB and a few giant
    ones near 2.2 GB (build.py, which also holds back when free commit runs low), a runtime C++ unit near 1.5 GB;
    4 GB stay for Windows and the rest of the PC. Under SMALL_RAM_GB the game's compiles are build.py's: one -O2 giant
    (HOT_GIANT_PEAK_GB) and what fits beside it, so 1 at 7.0-7.6 GB, 2 at 7.8."""
    usable = max(1.0, total_gb - 4)
    if total_gb < SMALL_RAM_GB:
        game = max(1, min(cores, int((total_gb - 4 - HOT_GIANT_PEAK_GB) / 0.5) + 1))
    else:
        game = max(2, min(cores, int(usable)))
    return game, max(1, min(cores, 6, int(usable / 1.5)))


def on_wine():
    """Windows' Python under Wine / Proton (Linux, Steam Deck): its ntdll exports wine_get_version (Setup's Wine class)"""
    if LINUX:   # (native: not Wine)
        return False
    import ctypes
    try:
        return hasattr(ctypes.windll.ntdll, 'wine_get_version')
    except (AttributeError, OSError):
        return False


def filesystem(path):
    """the file system of the drive path really is on (through Setup's junction): 'NTFS', 'FAT32', 'exFAT'...
    Under Wine a drive letter is a Linux folder (Z: is /), so the folder itself is asked: Wine answers with the statfs()
    of the file system holding it (a FAT32 SD card mounted below /run/media), the drive root would answer for /"""
    if LINUX:
        return host_linux.filesystem(path)
    import ctypes
    real = os.path.realpath(path)
    name = ctypes.create_unicode_buffer(64)
    k32 = ctypes.windll.kernel32
    if on_wine():
        at = real
        while at and not os.path.isdir(at) and os.path.dirname(at) != at:
            at = os.path.dirname(at)
        if k32.GetVolumeInformationW(at.rstrip(os.sep) + os.sep, None, 0, None, None, None, name, 64):
            return name.value
    root = os.path.splitdrive(real)[0] + os.sep
    ok = k32.GetVolumeInformationW(root, None, 0, None, None, None, name, 64)
    return name.value if ok else ''


def nup_dump(path):
    """A NUP (pipeline/nx/nup.py: an unencrypted dump, no keys needed): by its name or its magic"""
    if path.lower().endswith('.nup'):
        return True
    try:
        with open(path, 'rb') as f:
            return f.read(3) == b'NUP'
    except OSError:
        return False


def compressed_dump(path):
    """An NSZ / XCZ (its NCAs zstd-compressed by nsz, pipeline/nx/ncz.py) or a NUP (its files zstd one by one): by its
    name, else by its files (a renamed one). The game cannot read its data in place, so it is copied out (romfs.bin)."""
    if path.lower().endswith(('.nsz', '.xcz')) or nup_dump(path):
        return True
    try:
        if REPO not in sys.path:
            sys.path.insert(0, REPO)
        from pipeline.nx import container, ncz
        with open(path, 'rb') as f:
            return any(ncz.is_ncz(e[0]) for e in container.container_entries(f, os.path.getsize(path))[1])
    except (ImportError, OSError, ValueError):   # (before step 0's packages: by its name only)
        return False


def ncz_plain_gb(path):
    """GB the .ncz NCAs of an NSZ / XCZ decompress to (their headers: pipeline/nx/ncz.py), rounded up; twice the file
    when they cannot be read here (before step 0's packages)"""
    try:
        if REPO not in sys.path:
            sys.path.insert(0, REPO)
        from pipeline.nx import container, ncz
        with open(path, 'rb') as f:
            total = sum(ncz.NczBody(f, off, size).nca_size for name, off, size in
                        container.container_entries(f, os.path.getsize(path))[1] if ncz.is_ncz(name))
    except (ImportError, OSError, ValueError):
        total = 2 * os.path.getsize(path)
    return -(-total // 2**30)


def in_onedrive(path):
    """the path is in one of this account's OneDrive folders (Setup's Preflight.InOneDrive)"""
    p = os.path.abspath(path).rstrip('\\').lower() + '\\'
    return any(od and p.startswith(od.rstrip('\\').lower() + '\\')
               for od in (os.environ.get(v) for v in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial')))


def dump_drive(path):
    """what kind of drive the dump is on: 'fixed' (an internal disk), or 'removable', 'usb', 'network', 'cd' (a drive
    that can go away: the game's data is copied out by default then), 'OneDrive' (it frees space by leaving an
    online-only stub of a file: copied too), 'other' (unknown: treated as fixed)"""
    if LINUX:
        return host_linux.dump_drive(path)
    import ctypes
    from ctypes import wintypes
    p = os.path.abspath(path)
    if p.startswith('\\\\'):
        return 'network'
    if in_onedrive(p):
        return 'OneDrive'
    root = os.path.splitdrive(p)[0]
    kind = {2: 'removable', 3: 'fixed', 4: 'network', 5: 'cd'}.get(ctypes.windll.kernel32.GetDriveTypeW(root + '\\'), 'other')
    if kind != 'fixed':
        return kind
    # an external disk on USB reports itself fixed: the storage bus says what it is (no admin rights needed)
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    h = k32.CreateFileW('\\\\.\\' + root, 0, 3, None, 3, 0, None)   # (FILE_SHARE_READ | WRITE, OPEN_EXISTING)
    if h in (None, wintypes.HANDLE(-1).value):
        return kind
    try:
        query = (ctypes.c_uint32 * 3)(0, 0, 0)   # STORAGE_PROPERTY_QUERY: StorageDeviceProperty, PropertyStandardQuery
        out = ctypes.create_string_buffer(1024)
        got = wintypes.DWORD()
        if k32.DeviceIoControl(h, 0x2D1400, query, ctypes.sizeof(query), out, 1024, ctypes.byref(got), None) and got.value >= 32:
            bus = int.from_bytes(out.raw[28:32], 'little')   # STORAGE_DEVICE_DESCRIPTOR.BusType
            if bus in (7, 0xC, 0xD):   # BusTypeUsb, BusTypeSd, BusTypeMmc
                return 'usb'
    finally:
        k32.CloseHandle(h)
    return kind


def free_gb(path):
    os.makedirs(path, exist_ok=True)
    return shutil.disk_usage(path).free / 2**30


class Growth:
    """The file a step is writing (step 1: romfs.bin, written as romfs.bin.partial): 'NN% (x.x of y.y GB written)'
    every `every` s while it grows (the window moves its bar by 'NN% (' lines), and once per stall a plain sentence
    when it has not grown for `stall` s (a player saw only 'still working' for many minutes and took it for stuck).
    poll() returns the lines to say; total None: GB alone. Done (silent) once the file reaches total."""

    def __init__(self, label, paths, total, every=10, stall=300, clock=time.monotonic):
        self.label, self.paths, self.total, self.every, self.stall, self.clock = label, paths, total, every, stall, clock
        self.size, self.grew = self.now_size(), clock()
        self.said, self.warned = self.grew, False

    def now_size(self):
        sizes = [os.path.getsize(p) for p in self.paths if os.path.isfile(p)]
        return max(sizes) if sizes else 0

    def poll(self):
        t, size = self.clock(), self.now_size()
        if self.total and size >= self.total:
            return []
        out = []
        if size > self.size:
            self.size, self.grew, self.warned = size, t, False
            if t - self.said >= self.every:
                self.said = t
                gb = f'{size / 1e9:.1f} of {self.total / 1e9:.1f} GB' if self.total else f'{size / 1e9:.1f} GB'
                out.append(f'{self.label}: {size * 100 // self.total}% ({gb} written)' if self.total else
                           f'{self.label}: {gb} written')
        elif not self.warned and t - self.grew >= self.stall:
            self.warned = True
            # (nothing yet: still reading the dump's headers and code, slow from a slow drive, not necessarily stuck)
            what = (f'nothing written for {self.stall // 60} minutes' if size else
                    f'still reading the dump after {self.stall // 60} minutes, the game data not started yet')
            out.append(f'{self.label}: {what}. Check that the drive the dump is on (USB or network) is still connected '
                       f'and that your antivirus is not blocking {sys.executable}; if it stays like this, send setup.log')
        return out


def romfs_size(version):
    """the RomFS image's size the version pin expects (pipeline/expected/totk-<version>.json), None if unknown"""
    try:
        with open(os.path.join(REPO, 'pipeline', 'expected', f'totk-{version}.json'), encoding='utf-8') as f:
            return int(json.load(f)['romfs']['size'])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def run(step, cmd, env, dry, beat=HEARTBEAT_S, quiet=False, watch=None, tolerate=False):
    """Run one step, its output streamed line by line (the window shows the last lines), with a 'still working'
    line after `beat` seconds of silence. Returns the whole stdout+stderr. quiet: a step whose output is a JSON
    result for us (build.py key), shown only when it fails. watch: a Growth, polled every few seconds. tolerate:
    an optional part (give quiet too): returns (ok, output) and a failure does not stop the build."""
    say(f'step {step}: ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    if dry:
        return (True, '') if tolerate else ''
    p = subprocess.Popen(cmd, cwd=REPO, env=env, text=True, encoding='utf-8', errors='replace',
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: ([lines.put(ln) for ln in p.stdout], lines.put(None)), daemon=True).start()
    out, t0, last = [], time.monotonic(), time.monotonic()
    while True:
        for m in watch.poll() if watch else []:
            say(m)
            last = time.monotonic()
        try:
            ln = lines.get(timeout=5 if not watch else max(0.2, min(5, watch.every)))
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
    if tolerate:
        return p.wait() == 0, ''.join(out)
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


def key_built(kdir):
    """build.py's key manifest says its objects are done (the exe is checked by the step's stamp)"""
    try:
        with open(os.path.join(kdir, 'manifest.json'), encoding='utf-8') as f:
            return bool(json.load(f).get('objs_done'))
    except (OSError, ValueError):
        return False


def player_txt(folder):
    """a game folder's player.txt as {name: value} (package.py writes key= and commit=)"""
    try:
        with open(os.path.join(folder, 'player.txt'), encoding='utf-8') as f:
            return dict(ln.rstrip('\n').split('=', 1) for ln in f if '=' in ln)
    except OSError:
        return {}


def gpus():
    """this PC's display adapters and driver versions, sorted (the display class in the registry, no process): a new
    GPU or driver makes the shader cache step 9 prepared stale, so it joins that step's inputs"""
    if LINUX:
        return sorted(host_linux.gpus())
    try:
        import winreg
        found = []
        cls = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, cls) as k:
            for i in range(winreg.QueryInfoKey(k)[0]):
                try:
                    with winreg.OpenKey(k, winreg.EnumKey(k, i)) as d:
                        found.append([winreg.QueryValueEx(d, 'DriverDesc')[0], winreg.QueryValueEx(d, 'DriverVersion')[0]])
                except OSError:   # (Properties and other subkeys without a driver)
                    pass
        return sorted(found)
    except (ImportError, OSError):
        return []


def done_at(path):
    """a file's time as the stamps write theirs (a step done before builder-steps.json existed)"""
    try:
        return time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(path)))
    except OSError:
        return 'an earlier run'


def identity(path):
    """what tells a dump / keys file / firmware folder from another without reading 17 GB: its full path, size and time"""
    if not path:
        return None
    try:
        st = os.stat(path)
        return [os.path.normcase(os.path.abspath(path)), st.st_size, st.st_mtime_ns]
    except OSError:
        return [os.path.normcase(os.path.abspath(path)), None, None]


class Stamps:
    """<work>/builder-steps.json: each step that finished, with a digest of the inputs it was done from (the dump, the
    keys, the Setup's source = its version, the key, its own flags) and when. Setup run again on the same folder after a
    failure reuses every step whose inputs are the same and whose outputs are all there ('reusing step N (done at
    ...)') and goes on from the one that failed; a step is stamped only once it succeeded, so a failed or stopped one
    runs again. The steps' own checks stay (an extraction is reused only with its READY.json, objects by content)."""

    def __init__(self, work, total, dry=False, log=None):
        self.path, self.total, self.dry, self.log = os.path.join(work, 'builder-steps.json'), total, dry, log or say
        try:
            with open(self.path, encoding='utf-8') as f:
                self.data = json.load(f)
            if not isinstance(self.data, dict):
                self.data = {}
        except (OSError, ValueError):
            self.data = {}

    @staticmethod
    def digest(inputs):
        return hashlib.sha256(json.dumps(inputs, sort_keys=True, default=str).encode()).hexdigest()[:20]

    def reuse(self, num, name, inputs, outputs=()):
        """True (and the line said) when this step was done from these inputs and its outputs are there"""
        rec = self.data.get(f'{num} {name}')
        if self.dry or not isinstance(rec, dict) or rec.get('inputs') != self.digest(inputs) or \
                not all(os.path.exists(o) for o in outputs):
            return False
        self.reused(num, name, rec.get('done', '?'))
        return True

    def reused(self, num, name, when):
        self.log(f'step {num}/{self.total} {name}: reusing step {num} (done at {when})')

    def done(self, num, name, inputs):
        if self.dry:
            return
        self.data[f'{num} {name}'] = dict(inputs=self.digest(inputs), done=time.strftime('%Y-%m-%d %H:%M'))
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(self.path + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
                json.dump(self.data, f, indent=1, sort_keys=True)
            os.replace(self.path + '.tmp', self.path)
        except OSError:   # (a stamp that cannot be written only costs the reuse next time)
            pass


# Every release asset this builder or the Setup downloads is pinned in assets.json next to this file (name, the tag of
# the release that carries it, sha256, size): the one list, which the release script checks a draft against.
def pinned_asset(used_by):
    """assets.json's entry for 'builder:<NAME>' as dict(tag, name, size, sha256), or None (this source pins none)"""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets.json'), encoding='utf-8') as f:
        for x in json.load(f)['assets']:
            if x['used_by'] == used_by:
                return {k: x[k] for k in ('tag', 'name', 'size', 'sha256')}
    return None


# The release's profile (PGO): made on the release PC by playing the game (tools/engine/pgo_train.py), a release asset,
# never in the repository. The compile uses it (-fprofile-use) where the code still matches the training; without it
# (--no-pgo, no network, a bad checksum) the build is the plain -O2 one. Pinned per source version: the profile belongs
# to the code it was trained on. None: this source has no published profile (no download, no log line): -O2.
PGO = pinned_asset('builder:PGO')
# its training record: the hashes of the parts it was trained on (build.py's log says how many have changed since) and
# "runtime": true, which makes build.py compile the runtime with the profile too (pgo_rt: it joins the key). Without
# the record the game objects still get the profile, the runtime does not
PGO_RECORD = pinned_asset('builder:PGO_RECORD')
PGO_URL = 'https://github.com/daniel-madrid-07/weeping-of-the-empire/releases/download/{tag}/{name}'


HOT_NAME = re.compile(r'^\s+\?([A-Za-z_][A-Za-z0-9_]*)@@')   # ('  ?f_1b4d760@@YAX...:' in llvm-profdata show)
HOT_NAME_LINUX = re.compile(r'^\s+_Z\d+([A-Za-z_][A-Za-z0-9_]*)P3Ctx:')   # (the Linux profile's: '  _Z9f_1b4d760P3Ctx:')


def hot_list(profile, out, llvm_dir, run=subprocess.Popen):
    """<out>: the lifted functions the profile saw run (any block count above 0), one name a line, from llvm-profdata
    show --all-functions --counts (the player's toolchain carries it). build.py --hot re-splits the TUs into hot ones
    (-O2 with the profile, as today) and cold ones (COLD_OPT, -O1). setup-speed, 2026-11-02 (profile 855fd396):
    73.5k of 283k functions ran in its training, 45 % of main's code; a cold TU compiles 31 % faster at -O1 (11.6 ->
    8.0 s), the whole build ~13 % less CPU. Deterministic: the pinned profile and the pinned llvm-profdata."""
    tool = os.path.join(llvm_dir, 'bin', 'llvm-profdata.exe' if os.name == 'nt' else 'llvm-profdata')
    p = run([tool, 'show', '--all-functions', '--counts', profile], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding='utf-8', errors='replace')
    hot, name, seen = [], None, 0
    for ln in p.stdout:
        m = HOT_NAME.match(ln) or (HOT_NAME_LINUX.match(ln) if LINUX else None)
        if m:
            name = m.group(1)
        elif name and ln.lstrip().startswith('Block counts: ['):
            seen += 1
            if any(x.strip() not in ('', '0') for x in ln.split('[', 1)[1].rstrip().rstrip(']').split(',')):
                hot.append(name)
            name = None
    if p.wait() or not seen or not hot:
        raise RuntimeError(f'llvm-profdata gave no counts (exit {p.returncode}, {seen} functions)')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(sorted(set(hot))) + '\n')
    os.replace(out + '.tmp', out)
    return len(set(hot)), seen


def hot_args(work, pgo, env, dry):
    """['--hot', <the release profile's hot list>] with the profile, [] without it (all -O2, said)"""
    if not pgo:
        return []
    out = os.path.join(work, 'engine', 'hot', os.path.splitext(os.path.basename(pgo[-1]))[0] + '.hot.txt')
    if dry:
        return ['--hot', out]
    if not os.path.isfile(out):
        try:
            # (Setup: <install>/tools/llvm, which toolchain.apply puts in NXRECOMP_LLVM_DIR; else the PATH's LLVM)
            on_path = shutil.which('llvm-profdata')
            llvm = env.get('NXRECOMP_LLVM_DIR') or (os.path.dirname(os.path.dirname(on_path)) if on_path else
                                                    r'C:\Program Files\LLVM')
            n, total = hot_list(pgo[-1], out, llvm)
            say(f'profile (PGO): {n} of {total} functions ran in its training: the rest compile at -O1 (faster)')
        except Exception as e:   # (never stops the build: all at -O2 as before)
            say(f'profile (PGO): no hot list ({e}): every function at -O2')
            return []
    return ['--hot', out]


def pgo_args(work, off, dry, llvm_dir=None):
    """['--pgo', <the profile>] for build.py game / key, [] to build without one (said in the log). Linux: the release's
    profile with the generated functions' records renamed to the Linux build's names (tools/linux/pgo_remap.py; their
    bodies are the same source, so clang takes the counts: 1239 of 1246 functions matched in a check, the others a
    patch set's differences), the runtime's dropped (no pgo_rt there)."""
    if not PGO:
        return []
    if off:
        say('profile (PGO): off (--no-pgo): plain -O2')
        return []
    dest = os.path.join(work, 'engine', 'pgo', PGO['name'])   # (build.py's own name for it: <sha16>.profdata)
    remap = os.path.join(REPO, 'tools', 'linux', 'pgo_remap.py')
    rev = ''   # (Linux: the remap's own hash in the name, so a changed pgo_remap.py makes a new file, never reuses one)
    if LINUX:
        with open(remap, 'rb') as f:
            rev = '-' + hashlib.sha256(f.read().replace(b'\r\n', b'\n')).hexdigest()[:8]
    linux = os.path.join(work, 'engine', 'pgo', os.path.splitext(PGO['name'])[0] + f'-linux{rev}.profdata')
    if dry:
        return ['--pgo', linux if LINUX else dest]
    import toolchain
    try:
        toolchain.download('the release\'s profile (PGO)', PGO_URL.format(**PGO), dest, PGO['sha256'], PGO['size'])
    except Exception as e:   # (any failure: a network error, a full disk... never stops the build)
        say(f'profile (PGO): not used ({e}); the build goes on without it, plain -O2 (a little slower in play)')
        return []
    if LINUX:
        if not os.path.isfile(linux):
            r = subprocess.run([sys.executable, '-I', remap, dest, linux,
                                '--llvm', llvm_dir or ''], capture_output=True, text=True)
            if r.returncode:
                say(f'profile (PGO): not used (the Linux names could not be made: {(r.stdout + r.stderr).strip()[-300:]}); '
                    'plain -O2')
                return []
            say(r.stdout.strip())
        return ['--pgo', linux]
    if not PGO_RECORD:
        return ['--pgo', dest]
    try:
        toolchain.download('the profile\'s training record', PGO_URL.format(**PGO_RECORD),
                           os.path.join(os.path.dirname(dest), PGO_RECORD['name']), PGO_RECORD['sha256'],
                           PGO_RECORD['size'])
    except Exception as e:
        say(f'profile (PGO): no training record ({e}): the game code gets the profile, the runtime does not (it '
            'builds at plain -O2)')
    return ['--pgo', dest]


# The release's shader seed: cache-seed/precompile.bin alone, the records of the programs and pipelines the game
# compiles (code hashes, sizes, shader keys, pipeline state; no shader code). The runtime takes each program's code
# from the player's own archive_shaders.bin by its hash, so first sight of a shader no longer hitches. None: this
# source has no published seed (nothing fetched, nothing said). A release asset, pinned like PGO.
SEED = pinned_asset('builder:SEED')


def shader_seed(work, dry):
    """<work>/engine/seed/precompile.bin from the pinned release asset (package.py copies that folder as cache-seed);
    any failure: no seed, said, the build goes on. The package's seed is always this source's pin or nothing: a file
    left by an earlier install (another pin) goes when this one has none or its fetch fails."""
    if dry:
        return
    dest = os.path.join(work, 'engine', 'seed', 'precompile.bin')
    mine = dest + '.builder'   # (the Builder put it there: never remove a seed someone else made in a shared work folder)
    if SEED:
        import toolchain
        try:
            toolchain.download('the shader seed', PGO_URL.format(**SEED), dest, SEED['sha256'], SEED['size'])
            open(mine, 'w').write(SEED['sha256'])
            return
        except Exception as e:
            say(f'shader seed: not used ({e}); the game compiles each shader at its first sight (short hitches)')
    if os.path.exists(mine):
        for p in (dest, mine):
            if os.path.exists(p):
                os.remove(p)


# AMD's FidelityFX runtime for FSR 3.1: amd_fidelityfx_vk.dll + its MIT LICENSE.txt (FidelityFX SDK v1.1.4), mirrored as
# our release asset setup-ffx.zip so an AMD release moving cannot break a Setup; unpacked into <work>/engine/ffx, where
# build.py finds them (its own fetch from AMD then has nothing to do) and package.py points TOTK_FFX_DIR. Optional: any
# failure is one line and FSR is not available this time; the build goes on. None: this source pins none (nothing fetched,
# nothing said).
FFX = pinned_asset('builder:FFX')


def fsr_runtime(work, dry):
    if not FFX or dry:
        return
    import toolchain
    import zipfile
    d = os.path.join(work, 'engine', 'ffx')
    z = os.path.join(d, FFX['name'])
    try:
        toolchain.download('the FSR runtime (AMD FidelityFX)', PGO_URL.format(**FFX), z, FFX['sha256'], FFX['size'])
        with zipfile.ZipFile(z) as zf:
            for n in ('amd_fidelityfx_vk.dll', 'LICENSE.txt'):
                with zf.open(n) as src, open(os.path.join(d, n + '.tmp'), 'wb') as out:
                    shutil.copyfileobj(src, out)
                os.replace(os.path.join(d, n + '.tmp'), os.path.join(d, n))
    except Exception as e:   # (never stops the build)
        say(f'FSR not available this time ({e}): the game runs without it (its rows say so)')


# Step 9: totk.exe --prepare builds every program and pipeline the package's records-only list (cache-seed) names, from
# the player's own shader archive, into the cache play reads (%LOCALAPPDATA%\nxrecomp\TotK-engine\run\cache: driver
# pipeline cache, SPIR-V, the precompile_cold.key stamp): no window, no guest code, minutes, for this GPU and driver.
# Without it (a failure) each shader compiles the first time it is seen (short hitches the first play). False: this
# source's runtime has no prepare mode (an older totk.exe would start the game instead).
PREPARE_SHADERS = True
PREPARE_COUNTS = re.compile(r'(\d+)/(\d+)')   # ('[prepare] programs a/b, pipelines c/d, compute e/f': summed)
PREPARE_FIRST_LINE_S = 120   # (no '[prepare]' line by then: not a prepare mode; stopped, skipped)
PREPARE_MAX_S = 45 * 60


def player_values(pkg):
    """player.txt's key=value lines, as the launcher reads them (launcher.c package_value / package_env)"""
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


def player_path(pkg, v):
    if LINUX:   # (player.txt's paths: absolute, or relative to the package)
        v = v.rstrip(' ')
        return v if not v or v.startswith('/') else os.path.abspath(os.path.join(pkg, v))
    v = v.rstrip(' ').replace('/', '\\')
    return v if not v or v[1:2] == ':' or v.startswith('\\\\') else os.path.abspath(os.path.join(pkg, v))


def prepare_env(pkg, environ=None):
    """(run dir, environment) as the launcher gives the game (launcher.c run_package): the per-user folders, the
    package's img / romfs / overlay, cache-seed as TOTK_PRECOMPILE_SEED, player.txt's env.* lines. settings.ini: the
    package's defaults copied where the game reads them when the player has none (as the launcher's first start)."""
    environ = dict(os.environ if environ is None else environ)
    roam = os.path.join(environ.get('APPDATA', ''), 'nxrecomp', 'TotK-engine')
    local = os.path.join(environ.get('LOCALAPPDATA', ''), 'nxrecomp', 'TotK-engine')
    if LINUX:   # (the XDG folders the Linux launcher gives the game: engine/launcher/linux/launcher_linux.py)
        sys.path.insert(0, os.path.join(REPO, 'engine', 'launcher', 'linux'))
        import launcher_linux
        roam, local = launcher_linux.user_dirs(environ)
    vals, run = player_values(pkg), os.path.join(local, 'run')
    env = {k: v for k, v in environ.items() if not k.upper().startswith(('TOTK_', 'NXR_'))}
    env.update(TOTK_USERDATA=os.path.join(roam, 'userdata'), TOTK_IMG=player_path(pkg, vals.get('img', '')),
               TOTK_PRIORITY='normal', TOTK_CRASH_DIR=os.path.join(local, 'crashes'))
    for k in ('romfs', 'overlay'):
        if vals.get(k, '').strip():
            env['TOTK_' + k.upper()] = player_path(pkg, vals[k])
    if os.path.isfile(os.path.join(pkg, 'cache-seed', 'precompile.bin')):
        env['TOTK_PRECOMPILE_SEED'] = os.path.join(pkg, 'cache-seed')
    env.update((k[4:], v) for k, v in vals.items() if k.startswith('env.') and len(k) > 4)
    return run, env, roam, player_path(pkg, vals.get('settings', ''))


def shader_inputs(pkg):
    """what step 9's result depends on besides the dump, the GPUs and their drivers: the translator (xlate_id.py, the
    renderer's sources, which the game's own shader-cache keys use), the package's recorded programs (cache-seed), and
    the player.txt env lines and default settings the game translates with. Not the Setup commit or the package folder:
    a relink-only update (v0.1.8.1) prepared everything again, 184 of its 335 s."""
    sys.path.insert(0, os.path.join(REPO, 'tools', 'engine'))
    import xlate_id

    def digest(path):
        try:
            with open(path, 'rb') as f:
                return hashlib.sha256(f.read()).hexdigest()[:16]
        except OSError:
            return None
    vals = player_values(pkg)
    return dict(xlate=f'{xlate_id.xlate_id(os.path.join(REPO, "engine")):016x}',
                seed=digest(os.path.join(pkg, 'cache-seed', 'precompile.bin')),
                env=sorted(f'{k}={v}' for k, v in vals.items() if k.startswith('env.')),
                settings=digest(player_path(pkg, vals.get('settings', ''))) if vals.get('settings', '').strip() else None)


def prepare_shaders(pkg, n, dry, cmd=None):
    if not PREPARE_SHADERS:
        return
    exe = os.path.join(pkg, EXE)
    cmd = cmd or [exe, '--prepare']   # (the tests: a fake)
    say(f'step 9/{n} shaders: "{exe}" --prepare')
    if dry:
        return
    run, env, roam, defaults = prepare_env(pkg)
    for d in (run, env['TOTK_USERDATA'], env['TOTK_CRASH_DIR']):
        os.makedirs(d, exist_ok=True)
    if defaults and os.path.isfile(defaults) and not os.path.exists(os.path.join(roam, 'settings.ini')):
        shutil.copyfile(defaults, os.path.join(roam, 'settings.ini'))   # (settings change some translations)
    t0, last, rc, tail, done_line = time.monotonic(), 0.0, None, [], None
    try:
        p = subprocess.Popen(cmd, cwd=run, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace', bufsize=1,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except OSError as e:
        p, rc, tail = None, -1, [str(e)]
    if p:
        lines, seen = queue.Queue(), False
        threading.Thread(target=lambda: ([lines.put(ln) for ln in p.stdout], lines.put(None)), daemon=True).start()
        while True:
            limit = PREPARE_MAX_S if seen else PREPARE_FIRST_LINE_S
            try:
                ln = lines.get(timeout=max(1.0, t0 + limit - time.monotonic()))
            except queue.Empty:
                p.kill()   # (this run's own child)
                tail.append(f'stopped after {round(time.monotonic() - t0)} s' if seen else 'no prepare mode')
                p.wait()
                rc = -2
                break
            if ln is None:
                rc = p.wait()
                break
            ln = ln.rstrip()
            seen = seen or ln.startswith('[prepare]')
            tail = (tail + [ln])[-6:]
            counts = PREPARE_COUNTS.findall(ln) if ln.startswith('[prepare]') else []
            if ln.startswith('[prepare] done'):
                done_line = ln
            elif counts and time.monotonic() - last >= 2:   # (the window moves its bar by 'NN% (' lines)
                last = time.monotonic()
                done, total = sum(int(a) for a, _ in counts), sum(int(b) for _, b in counts)
                say(f'shaders: {done * 100 // max(total, 1)}% ({done}/{total} programs and pipelines)')
    secs = round(time.monotonic() - t0)
    if done_line:
        say(f'shaders: {done_line[len("[prepare] "):]}')
    if rc == 0:
        say(f'shaders: prepared in {secs} s')
    elif rc == 1:
        say(f'shaders: prepared in {secs} s, some builds failed: the game builds those the first time it sees them')
    elif rc == 2:
        say('shaders: nothing to prepare (no shader list or shader archive); the game builds each one the first time '
            'it is seen')
    else:
        say(f'shaders: not prepared (exit {rc}, {secs} s: {" | ".join(tail[-3:])}); the game builds each one the first '
            'time it is seen (short hitches the first time you play an area)')
    return rc   # (0 / 1 / 2: done, a later Setup run reuses it; anything else runs again next time)


def release_of(repo):
    """when the source is exactly a release tag cloned from GitHub (Setup's clone), else None: the launcher's automatic
    update compares the tag with GitHub's latest; a build from anything else never updates itself"""
    tag = subprocess.run(['git', '-C', repo, 'describe', '--tags', '--exact-match', 'HEAD'], capture_output=True,
                         text=True).stdout.strip()
    url = subprocess.run(['git', '-C', repo, 'remote', 'get-url', 'origin'], capture_output=True, text=True).stdout.strip()
    m = re.match(r'https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?$', url)
    return (tag, m.group(1)) if tag and m else None


def newest_package(out, tag=None):
    """the newest playable folder in out; with tag ('' for 1.0.0, 'v143'), the newest of that game version (a 1.4.3
    folder beside a 1.0.0 one is neither's reuse check nor its shaders' folder)"""
    folders = sorted((d for d in glob.glob(os.path.join(out, 'TotK-PC-*')) if os.path.isdir(d) and not d.endswith('.tmp')
                      and (tag is None or version_tag(d) == tag)), key=os.path.getmtime)
    return folders[-1] if folders else None


def prune(out, keep):
    """--install: the playable folders an update replaced (each holds only the exe and the launcher; the game's data
    and the player's saves and settings are elsewhere)"""
    for d in glob.glob(os.path.join(out, 'TotK-PC-*')):   # (a 1.4.3 folder, ...-v143, never replaces a 1.0.0 one)
        if os.path.isdir(d) and os.path.normcase(d) != os.path.normcase(keep) and \
                version_tag(d) == version_tag(keep):
            if not carry_mods(d, keep):   # (a mod still there: the folder stays, the next update tries again)
                say(f'mods left in {os.path.join(d, "mods")} (a mod of the same name is already in the new game '
                    f'folder, or it could not be moved): move them yourself; that old folder is kept until then')
                continue
            shutil.rmtree(d, ignore_errors=True)   # (one in use, the game still open: stays until the next update)


def carry_mods(old, new):
    """The game folder's mods\\ (the launcher reads it first, as other ports keep it) moves into the new folder before
    the old one goes: every mod the player put there that the new folder has no entry of the same name for. True when
    nothing of the player's is left in it (the readme aside), so the old folder may go"""
    src, dst = os.path.join(old, 'mods'), os.path.join(new, 'mods')
    if not os.path.isdir(src):
        return True
    os.makedirs(dst, exist_ok=True)
    left = False
    for name in os.listdir(src):
        if name.upper().startswith('LEEME'):
            continue   # (the readme: the new one is current)
        if os.path.exists(os.path.join(dst, name)):
            left = True   # (a name already there: neither copy is deleted, the player chooses)
            continue
        try:
            shutil.move(os.path.join(src, name), os.path.join(dst, name))
            say(f'mods: {name} kept (moved into the new game folder)')
        except OSError as e:
            left = True
            say(f'mods: {name} could not be moved ({e})')
    return not left


def update_romfs_there(work):
    """Setup unpacked the 1.4.3 update's game files in this work folder (build.py's UPDATE_ROMFS, the same test)"""
    d = os.path.join(work, 'extracted', UPDATE_DIR)
    return os.path.isfile(os.path.join(d, 'romfs-index.json')) or \
        os.path.isfile(os.path.join(d, 'romfs', 'Pack', 'ZsDic.pack.zs'))


def same_100_package(out, head):
    """the newest 1.0.0 game folder in `out` was packaged from this Setup's commit (its overlay can be made again by
    this Setup's tools without leaving the exe's Options rows and the texts out of step)"""
    folders = [d for d in glob.glob(os.path.join(out, 'TotK-PC-*'))
               if os.path.isdir(d) and not d.endswith('.tmp') and not version_tag(d)]
    if not folders or not head:
        return False
    c = player_txt(max(folders, key=os.path.getmtime)).get('commit', '')
    return bool(c) and (head.startswith(c) or c.startswith(head))


def version_tag(folder):
    """'v143' for a package of another game version (package.py names it ...-v143 or ...-v143-beta), '' for 1.0.0"""
    m = re.search(r'-(v\d{3})(-beta)?$', os.path.basename(folder.rstrip('\\/')))
    return m.group(1) if m else ''


# UPDATE: version 1.4.3: the base dump AND the player's 1.4.3 update dump. The update's code is the 1.0.0 work moved
# onto its main by games.totk.patches port / engine-map, which run here on the player's two mains (their tables
# derive from the game: git-ignored, never shipped); no PGO profile (it was trained on 1.0.0), no Options sets / PC
# overlay / shader seed (made from 1.0.0's files: package.py points a 1.4.3 folder at an empty overlay and its own
# RomFS); saves in userdata-v143, seeded once from the 1.0.0 ones.
UPDATE_VERSION = '1.4.3'
UPDATE_DIR = 'v' + UPDATE_VERSION.replace('.', '')
SAVES_NOTE = (f'saves: {UPDATE_VERSION} keeps its own. The first time it starts, it copies your 1.0.0 saves once; '
              f'progress you make later in 1.0.0 does not appear in {UPDATE_VERSION}, and {UPDATE_VERSION} saves never '
              'go back to 1.0.0')


def engine_patches(work, img, py, env, label, dry, table=None):
    """['--patches', <engine-patches.json>]: funcs.py on the clean image (kept per image and funcs.py version), then
    the default patch set for the engine (seconds): 1.0.0's, or `table`'s (the update's, version_tables)"""
    pdir = os.path.join(work, 'engine', 'patches', 'builder')
    funcs = os.path.join(pdir, f'funcs_{os.path.basename(img)}_{git_id("engine/tools/funcs.py")}.json')
    vpy = os.path.join(work, 'engine', 'venv', *(('bin', 'python') if LINUX else ('Scripts', 'python.exe')))
    os.makedirs(pdir, exist_ok=True)
    if dry or not os.path.exists(funcs):
        run(label, [vpy if os.path.exists(vpy) else py, '-I', os.path.join(REPO, 'engine', 'tools', 'funcs.py'),
                    os.path.join(img, 'main'), funcs + '.new'], env, dry)   # (funcs.py takes the prefix: main.json + main.bin)
        if not dry:
            os.replace(funcs + '.new', funcs)
    run(label, [py, '-m', 'games.totk.patches', 'engine', '--image', os.path.join(img, 'main.bin'), '--funcs', funcs,
                '--out', pdir, '--features', 'default'] + (['--table', table] if table else []), env, dry)
    return ['--patches', os.path.join(pdir, 'engine-patches.json')]


def file_id(p):
    st = os.stat(p)
    return f'{st.st_size}:{st.st_mtime_ns}'


def app_icon(work, vdir, py, env, label, dry):
    """The app icon: the title screen's ring, from the player's own game files of the version built (vdir; games/totk/
    app_icon.py: Nintendo's artwork, never shipped; a neutral one of ours if the ring cannot be read), linked into
    totk.exe and the launcher (the desktop shortcut's icon). Returns env with TOTK_APP_ICON_RES for the build and the
    package. Optional, as every such step: a failure says so and returns env as it was (Windows' own icon).
    Linux: no resource to link (the launcher's .desktop icon is the AppImage's own): nothing to do."""
    if LINUX:
        return env
    out = os.path.join(work, 'engine', 'icon')
    cmd = [py, '-m', 'games.totk.app_icon', '--romfs', vdir, '--out', out]
    say(f'step {label}: ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    res = os.path.join(out, 'totk.res')
    if dry:
        return dict(env, TOTK_APP_ICON_RES=res)
    r = subprocess.run(cmd, cwd=REPO, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode or not os.path.isfile(res):
        say('no app icon this time (the game keeps the Windows icon): ' + (r.stdout + r.stderr).strip()[-300:])
        return env
    say(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'app icon made')
    return dict(env, TOTK_APP_ICON_RES=res)


def version_table_files():
    """what version_tables() makes, all game-derived and untracked: games/totk/patches/<v>.toml (port) and <v>.engine.json
    (engine-map), engine/runtime/totk_main_<v>.inc and totk_fields_<v>.inc (engine-map), and with a beta engine map
    totk_main_<v>_stubs.inc. A step marked done with one of them gone (a clean-up) runs again: v0.1.8's 1.4.3 check
    died at step 5 on a missing.inc that build.py's snapshot needs."""
    pat = os.path.join(REPO, 'games', 'totk', 'patches')
    rt = os.path.join(REPO, 'engine', 'runtime')
    out = [os.path.join(pat, UPDATE_DIR + '.toml'), os.path.join(pat, UPDATE_DIR + '.engine.json'),
           os.path.join(rt, f'totk_main_{UPDATE_DIR}.inc'), os.path.join(rt, f'totk_fields_{UPDATE_DIR}.inc')]
    try:   # (engine_map.py writes the stubs for a beta or diagnostic map, and removes them otherwise)
        eng = json.load(open(out[1], encoding='utf-8'))
        stubs = bool(eng.get('beta') or eng.get('diagnostic'))
    except (OSError, ValueError):
        stubs = False
    if stubs:
        out.append(os.path.join(rt, f'totk_main_{UPDATE_DIR}_stubs.inc'))
    return out


def version_tables(work, v100, vdir, py, env, label, dry):
    """games/totk/patches/v143.toml + v143.engine.json from the player's 1.0.0 and 1.4.3 mains; again when the porter's
    code, the engine or either main changed (a git pull, a new dump)"""
    old, new = os.path.join(v100, 'exefs', 'main'), os.path.join(vdir, 'exefs', 'main')
    pat = os.path.join(REPO, 'games', 'totk', 'patches')
    rep = os.path.join(work, 'engine', 'patches')
    marker = os.path.join(rep, f'{UPDATE_DIR}-tables.json')
    want = None if dry else {'code': git_id('games/totk/patches'), 'engine': git_id('engine'),   # (engine-map reads
                             'old': file_id(old), 'new': file_id(new)}                       #  the engine's addresses)
    if not dry and done_with(marker, want) and all(os.path.isfile(p) for p in version_table_files()):
        return
    os.makedirs(rep, exist_ok=True)
    common = ['--old', old, '--new', new, '--version', UPDATE_VERSION]
    run(label, [py, '-m', 'games.totk.patches', 'port', *common, '--out', os.path.join(pat, UPDATE_DIR + '.toml'),
                '--report', os.path.join(rep, f'port-{UPDATE_DIR}.json')], env, dry)   # (1.4.3's own sites, sites_v143.py)
    run(label, [py, '-m', 'games.totk.patches', 'engine-map', *common, '--beta',
                '--report', os.path.join(rep, f'engine-{UPDATE_DIR}.json')], env, dry)
    if not dry:
        mark(marker, want)


def host_cpu():
    """'v3' when this processor has AVX2 (and with it FMA, BMI2, F16C on every AVX2 CPU), else 'v2'"""
    if os.name == 'nt':
        import ctypes
        return 'v3' if ctypes.windll.kernel32.IsProcessorFeaturePresent(40) else 'v2'   # PF_AVX2_INSTRUCTIONS_AVAILABLE
    try:
        flags = next((ln for ln in open('/proc/cpuinfo') if ln.startswith('flags')), '').split()
    except OSError:
        return 'v3'
    return 'v3' if 'avx2' in flags else 'v2'


def download_kit(argv):
    """builder.py --download-kit <nxr-kit> [--setup-items FILE] [--tag TAG] [--gui-pid N]: the rest of a download kit
    (tools/builder/kit.py), after Setup put its own downloads and the source in: the Python packages (the steps' and
    the engine venv's), the release's profile, shader seed and FSR runtime, Microsoft's C++ libraries and Windows SDK
    (a PC without Visual Studio C++ needs them; Setup asked for their terms) and the DLSS / FidelityFX files build.py
    fetches. A kit made before keeps its files: only what it lacks is downloaded. Ends with the kit marked complete."""
    ap = argparse.ArgumentParser(prog='builder.py --download-kit')
    ap.add_argument('kit')
    ap.add_argument('--setup-items', help='Setup\'s own downloads, already in <kit>/files: sha256 TAB size TAB name TAB url')
    ap.add_argument('--tag', help='the release the kit is for (Setup\'s)')
    ap.add_argument('--gui-pid', type=int, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if a.gui_pid:
        follow_gui(a.gui_pid)
    import toolchain
    import kit
    d = os.path.abspath(a.kit)
    os.environ['NXR_KIT_MAKE'] = os.environ['NXR_KIT'] = d
    doc = kit.load(d)
    doc.update(format=kit.FORMAT, complete=False, setup=a.tag or doc.get('setup'))
    if os.path.isdir(os.path.join(d, 'src.git')):
        doc['source'] = {'path': 'src.git', 'tag': a.tag}
    if a.setup_items:
        with open(a.setup_items, encoding='utf-8') as f:
            for ln in f:
                s, size, name, url = ln.rstrip('\n').split('\t')
                if os.path.isfile(os.path.join(d, 'files', s)):
                    kit._put(doc, {'name': name, 'url': url, 'sha256': s, 'size': int(size), 'path': 'files/' + s})
    kit.save(d, doc)
    scratch = d + '.work'
    sys.path.insert(0, os.path.join(REPO, 'tools', 'engine'))
    import build
    say('download kit: the Python packages')
    toolchain.kit_wheels({'setup': toolchain.PACKAGES, 'venv': build.VENV_PACKAGES})
    for what, asset in (('the release\'s profile (PGO)', PGO), ('the profile\'s training record', PGO_RECORD),
                        ('the shader seed', SEED), ('the FSR runtime (AMD FidelityFX)', FFX)):
        if asset:
            toolchain.download(what, PGO_URL.format(**asset), os.path.join(scratch, 'assets', asset['name']),
                               asset['sha256'], asset['size'])
    toolchain.ensure_msvc(os.path.join(scratch, 'tools'), fetch_only=True)
    build.dlss_fetch(build.DLSS_SDK, os.path.join(scratch, 'dlss-sdk'), flat=False)
    build.dlss_fetch(build.DLSS_DLLS, os.path.join(scratch, 'dlss'), flat=True)
    build.dlss_fetch(build.FFX_DLLS, os.path.join(scratch, 'ffx'), flat=True, url=build.FFX_URL, what='FidelityFX SDK')
    doc = kit.load(d)
    doc['complete'] = True
    kit.save(d, doc)
    size = sum(e.get('size', 0) for e in doc['files'])
    say(f'download kit complete: {len(doc["files"])} files, {size / 2**30:.1f} GB')
    say(f'KIT: {d}')


def main_100(argv, out, head):
    """An --update install builds only UPDATE_VERSION, and Setup offers the update dump again at every run (Builder.cs
    RestoreChoices): a 1.0.0 game folder from an older Setup would stay on its old code for good. So when this install
    has a 1.0.0 folder that another commit made, the same run builds 1.0.0 too (main() again without --update: its
    steps reuse what is there, minutes with the object cache); a failure there leaves the UPDATE_VERSION build done and
    says how to update 1.0.0. No 1.0.0 folder: none is made (a player who chose UPDATE_VERSION only), only the line
    saying how."""
    old = newest_package(out, '')
    if not old:
        say(f'1.0.0: not built (this run builds {UPDATE_VERSION} only): to play 1.0.0 too, run Setup again without the '
            f'{UPDATE_VERSION} update')
        return
    if same_100_package(out, head):
        return
    # (minutes when its build reuses the earlier objects; tens of minutes when it can't: exFAT, a cleared cache)
    say(f'1.0.0: your 1.0.0 game ({os.path.basename(old)}) is from an older Setup: updating it too (usually a few '
        'minutes; up to 45 minutes when its earlier build files are gone or the drive is exFAT)')
    rest, skip = [], 0
    for x in argv:   # (the same inputs and folders, without the update and without the window's pid: this run follows it)
        if skip:
            skip -= 1
        elif x in ('--update', '--gui-pid'):
            skip = 1
        elif not x.startswith(('--update=', '--gui-pid=')):
            rest.append(x)
    NESTED.append(1)
    ok = False
    try:
        main(rest)
        ok = True
    except SystemExit as e:   # (fail(): its ERROR line is above)
        ok = not e.code
    except Exception as e:
        say(f'ERROR: {type(e).__name__}: {e}')
    finally:
        NESTED.clear()
    if not ok:
        say(f'1.0.0: not updated (see the 1.0.0 lines above); your {UPDATE_VERSION} game is ready. To update 1.0.0, run '
            f'Setup again without the {UPDATE_VERSION} update')


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['--download-kit']:
        import toolchain
        try:
            return download_kit(argv[1:])
        except (toolchain.ToolchainError, OSError) as e:
            say(f'ERROR: the download kit is not complete: {e}. Start it again: what it has is kept.')
            sys.exit(1)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='the dump, the keys file and, optional, the firmware (folder or .zip)')
    ap.add_argument('--update', help=f'the player\'s {UPDATE_VERSION} update dump (.nsp): builds version '
                    f'{UPDATE_VERSION} (needs the 1.0.0 base dump too)')
    ap.add_argument('--work', default=os.path.join(os.path.dirname(REPO), 'build-work'),
                    help=f'where everything generated goes (about {FIRST_RUN_GB} GB the first time)')
    ap.add_argument('--out', help='where the playable folder goes (default: <work>/release)')
    ap.add_argument('--install', help='Setup\'s layout: DIR/tools (compilers, fetched), DIR/work, DIR/game')
    ap.add_argument('--until', choices=('tools', 'extract', 'romfs'),
                    help=f'stop after this step (romfs: only the game files, about {EXTRACT_GB} GB)')
    ap.add_argument('--dry-run', action='store_true', help='check the inputs and print the steps, build nothing')
    ap.add_argument('--no-pgo', action='store_true',
                    help='build without the release\'s profile (PGO): plain -O2, a little slower in play')
    ap.add_argument('--cpu', choices=['auto', 'v3', 'v2'], default='auto',
                    help='the processor level to build for: auto = this PC (v3 with AVX2, else v2); v2 also runs on '
                         'processors without AVX2, slower')
    ap.add_argument('--copy-romfs', action='store_true', default=None,
                    help=f'copy the game\'s data out of the dump (romfs.bin, {COPY_ROMFS_GB} GB) instead of reading it '
                         'from the dump: for a player who will move or delete the dump (default: only when the dump '
                         'is on a removable, USB, network or disc drive)')
    ap.add_argument('--no-copy-romfs', dest='copy_romfs', action='store_false',
                    help='read the game\'s data from the dump in place whatever drive it is on')
    ap.add_argument('--gui-pid', type=int, help=argparse.SUPPRESS)   # (Builder.exe: stop with it)
    a = ap.parse_args(argv)
    if a.gui_pid:
        follow_gui(a.gui_pid)

    game, keys, firmware = classify(a.inputs)
    if keys:
        check_keys(keys)
    install = os.path.abspath(a.install) if a.install else None
    work = os.path.join(install, 'work') if install else os.path.abspath(a.work)
    out = os.path.join(install, 'game') if install else os.path.abspath(a.out or os.path.join(work, 'release'))
    tools = os.path.join(install, 'tools') if install else None
    update = os.path.abspath(a.update.strip('"')) if a.update else None
    if update and not (os.path.isfile(update) and update.lower().endswith(GAME_EXT)):
        fail(f'the update dump: not a .nsp / .xci file: {update}')
    # an update NUP (the same .nup as the game, or one of its own) holds its whole RomFS tree: no base RomFS and no keys;
    # an update NSP / XCI patches the base game's NCA section, which a NUP base has not
    update_nup = bool(update) and nup_dump(update)
    if update and not update_nup and nup_dump(game):
        fail(f'a NUP base game cannot take an update .nsp / .xci: choose the {UPDATE_VERSION} update as a NUP (the same .nup '
             'when it holds the update), or the base game as an .nsp / .xci')
    if update and not update_nup and not keys:   # (a NUP needs none; an update NSP / XCI does)
        fail('missing: the keys file (the update dump is encrypted: prod.keys from your console). Drop it onto the window.')
    if firmware and not keys:   # (its fonts are in encrypted system NCAs)
        say('firmware: its fonts need the keys file (prod.keys from your console); without it the game is built '
            'without them: Korean and Chinese text uses a Windows font')
        firmware = None
    keyarg = ['--keys', keys] if keys else []   # (a NUP alone: no keys file at all)
    fw = None if not firmware else firmware_dir(firmware, work) if not a.dry_run or os.path.isdir(firmware) else firmware
    say(f'dump: {os.path.basename(game)}' + (f' + update {os.path.basename(update)} (builds {UPDATE_VERSION})'
                                               if update else '') +
        f' | keys: {os.path.basename(keys) if keys else "none (a NUP needs none)"} | firmware: {fw or "none"}')
    vdir = os.path.join(work, 'extracted', UPDATE_DIR if update else 'v100')   # (the version built: its game files)
    if update:
        say(SAVES_NOTE)
    if not fw and not os.path.isfile(os.path.join(vdir, 'fonts-report.json')):
        say(NO_FIRMWARE_CJK if cjk_windows() else NO_FIRMWARE)
    say(f'work folder: {work} | output: {out}')

    v100 = os.path.join(work, 'extracted', 'v100')
    extracted = os.path.exists(os.path.join(vdir, 'READY.json'))
    # the data in the dump (romfs-source.txt) or copied out (romfs.bin): --copy-romfs / --no-copy-romfs, else an install
    # that has romfs.bin keeps it (v0.1.4-v0.1.7 told players the install stands alone: many deleted the dump; installer),
    # else by the dump's drive (one that can go away, removable / USB / network / disc: copied, so the game runs without
    # it). An install made the other way switches over only on the player's flag (romfs step again: a full pass over
    # the dump, then the other one removed)
    drive = dump_drive(game)
    had_copy = os.path.isfile(os.path.join(v100, 'romfs.bin'))
    packed = compressed_dump(game)
    copy = bool(update) or packed or (a.copy_romfs if a.copy_romfs is not None else
                                      had_copy or drive not in ('fixed', 'other'))
    if not update:
        say('game data: ' + ('copied out of the dump (romfs.bin, 17 GB)' + (': the dump is a NUP' if packed and nup_dump(game) else
                                                                             ': the dump is compressed (NSZ / XCZ)' if packed else
                                                                             '' if a.copy_romfs else
                                                                             ': kept from the earlier install' if had_copy else
                                                                             ': the dump is in OneDrive' if drive == 'OneDrive' else
                                                                             f': the dump is on a {drive} drive')
                             if copy else 'read from the dump in place (keep the dump where it is)'))
    switch = bool(extracted and not update and
                  os.path.isfile(os.path.join(v100, 'romfs-source.txt' if copy else 'romfs.bin')) and
                  not os.path.isfile(os.path.join(v100, 'romfs.bin' if copy else 'romfs-source.txt')))
    problems, notes, tool_env = check_tools(managed=bool(install))
    need = RESUME_GB if extracted else FIRST_RUN_GB   # (a resumed run: the game's files are already out)
    if a.until:
        need = 0 if (extracted and not switch) or a.until == 'tools' else EXTRACT_GB
    if copy and (not extracted or switch) and a.until != 'tools':   # (1.4.3: its merged image is written out too)
        need += COPY_ROMFS_GB
    if update and not extracted and a.until != 'tools':
        need += UPDATE_UNPACK_GB
        # (an NSZ / XCZ base or update the merge reads out of order: its plaintext spilled once beside the
        # extraction, pipeline/nx/ncz.py, then removed: the worst case counted)
        if compressed_dump(game) and not nup_dump(game):
            need += COPY_ROMFS_GB
        if compressed_dump(update) and not nup_dump(update):
            need += ncz_plain_gb(update)
    if install and not os.path.isfile(os.path.join(tools, 'llvm', '.toolchain.json')):
        need += TOOLS_GB
    if free_gb(work) < need:
        problems.append(f'disk space: about {need} GB free needed in {work} ({free_gb(work):.0f} GB free)')
    fs = filesystem(work)
    if fs.upper().startswith('FAT') and a.until != 'tools':
        problems.append(f'{os.path.realpath(work)[:2]} is formatted {fs}, which cannot hold files over 4 GB (the game '
                        'data has one of 17 GB): choose a folder on ' + ('an ext4, btrfs, xfs or exFAT drive' if LINUX
                                                                           else 'an NTFS drive'))
    problems += check_pc()
    ram, _ = memory_gb()   # (no floor: under WARN_RAM_GB said, and Setup asked 'Continue anyway?' before this)
    cores = host_linux.cpu_count() if LINUX else os.cpu_count()   # (Linux: affinity and the cgroup's quota)
    game_jobs, rt_jobs = jobs_for(ram, cores or 2)
    say(f'{cores} cores, {ram:.0f} GB RAM: up to {game_jobs} compiles at once')
    if ram_warning(ram):
        say(ram_warning(ram))
    for note in notes:
        say('note: ' + note)
    if problems:
        say('install these first:')
        for p in problems:
            say('  - ' + p)
        if not a.dry_run:
            sys.exit(1)

    env = dict(os.environ, **tool_env, NXRECOMP_WORK=work, NXR_SOLO_PC='1', PYTHONIOENCODING='utf-8',
               NXR_GAME_JOBS_MAX=str(game_jobs), NXR_BUILDER_PACKAGE='1',
               # (pipeline.build's floor: what extraction itself writes; the whole install's sum is checked above)
               NXR_MIN_FREE_GB=str(EXTRACT_GB), PYTHONNOUSERSITE='1')
    py = sys.executable
    build = os.path.join(REPO, 'tools', 'engine', 'build.py')
    n = 9
    # (--commit for package.py, and the Setup's version for the steps' stamps: a step done by another Setup is redone)
    head = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    stamps = Stamps(work, n, a.dry_run)
    base = dict(dump=identity(game), keys=identity(keys), firmware=identity(fw), update=identity(update),
                setup=head or 'unknown', cpu=a.cpu, no_pgo=a.no_pgo, copy=copy)

    # the dump, the keys and the update read in their headers first (seconds): a wrong file, or keys that cannot open
    # it, is said in plain words with the fix before step 0 fetches the compilers and step 1 reads 17 GB (the pipeline's
    # own checks, so they agree; their Python packages are the first thing step 0 installs, so those go first)
    fonts = os.path.isfile(os.path.join(vdir, 'fonts-report.json'))
    steps = ['exefs', 'romfs'] if a.dry_run or not extracted else ['romfs'] if switch else []
    if not a.dry_run and (steps or (update and not os.path.isfile(os.path.join(v100, 'exefs', 'main')))):
        if install:
            import toolchain
            try:
                toolchain.ensure_packages()
            except toolchain.ToolchainError as e:
                fail(str(e))
        bad = check_dump(game, keys, update)
        if bad:
            fail(bad)

    # 0. tools (--install): the compilers and libraries, fetched into <install>/tools (always checked: they set the
    #    environment; done, that is seconds)
    if install:
        if not stamps.reuse(0, 'tools', {'setup': base['setup']}, [tools]):
            say(f'step 0/{n} tools')
        if a.dry_run:
            say('(--dry-run: nothing fetched)')
        else:
            import toolchain
            try:
                env = toolchain.apply(toolchain.ensure(tools), env)
            except toolchain.ToolchainError as e:
                fail(str(e))
            old = None if LINUX else msvc_too_old(env)   # (ensure() takes the pinned tools over an older VS: a guard)
            if old:
                fail(old)
            stamps.done(0, 'tools', {'setup': base['setup']})
        if a.until == 'tools':
            say('DONE (--until tools)')
            return

    # 1. extract (pipeline/build.py verifies every hash and pins the supported version's)
    #    (the fonts only with the firmware; given on a later run, they are added to an extraction made without)
    if update and (a.dry_run or not os.path.isfile(os.path.join(v100, 'exefs', 'main'))):
        # (1.4.3: the 1.0.0 main too, which the port moves from; its RomFS is not needed)
        run(f'1/{n} extract', [py, '-m', 'pipeline.build', '--nsp', game] + keyarg + ['--work', work,
                               '--steps', 'exefs'], env, a.dry_run)
    if steps and not update and not a.dry_run and 'romfs' in steps:
        drop_partial_folder(v100)
    common = (['--update', update] if update else ['--no-unpack'] + ([] if copy else ['--romfs-in-dump']))
    if steps:
        run(f'1/{n} extract', [py, '-m', 'pipeline.build', '--nsp', game] + keyarg + ['--work', work,
                               '--steps', ','.join(steps + ['ready'])] + common,
            env, a.dry_run,
            watch=None if a.dry_run or 'romfs' not in steps or not copy else   # (in the dump: no file grows)
            Growth('extract', [os.path.join(vdir, 'romfs.bin.partial'), os.path.join(vdir, 'romfs.bin')],
                   romfs_size(UPDATE_VERSION if update else '1.0.0')))
        # (1.0.0: romfs.bin alone, the game reads it in place: no 17 GB, 304,207-file copy (5-11 min, a Defender scan
        #  each). 1.4.3 keeps its folder: package.py points that version at it)
        stamps.done(1, 'extract', base)
    elif not a.dry_run:   # (READY.json decides, as before; an install from before the stamps says its date)
        stamps.reused(1, 'extract', (stamps.data.get('1 extract') or {}).get('done') or
                      done_at(os.path.join(vdir, 'READY.json')))
    # the firmware's fonts on their own, after the game's files: optional (only Korean and Chinese text uses them, a
    # Windows font otherwise), so a firmware these keys cannot open is said and the build goes on (#26: keys dumped by
    # a tool older than the firmware lack its key_area_key_*_NN and the whole install stopped after the game's files)
    if fw and not fonts:
        ok, out = run(f'1/{n} extract', [py, '-m', 'pipeline.build', '--nsp', game] + keyarg + ['--work', work,
                                         '--steps', 'fonts,ready', '--firmware', fw] + common,
                      env, a.dry_run, quiet=True, tolerate=True)
        if not ok:
            why = next((ln.strip() for ln in reversed(out.splitlines()) if 'FAILED' in ln or 'Missing' in ln),
                       (out.strip().splitlines() or ['no output'])[-1].strip())
            say(f'firmware: its fonts could not be read ({why}); the game is built without them: Korean and Chinese '
                f'text uses a Windows font. With keys dumped from a console on that firmware (a key dumper that '
                f'knows it), or another firmware, the next Install adds them')
    if a.until == 'extract':
        say('DONE (--until extract)')
        return
    # 2. the RomFS as one file: once proven equal to the folder the game reads romfs.bin (faster loads; package.py
    #    points the launcher at it only then)
    if update:   # (package.py points a 1.4.3 folder at its RomFS folder)
        if not a.dry_run:
            check_extraction(vdir, UPDATE_VERSION)
    elif a.dry_run or not any(os.path.exists(os.path.join(v100, f + '.verified.json'))
                              for f in ('romfs.bin', 'romfs-source.txt')):
        if not a.dry_run:
            check_extraction(v100)
        run(f'2/{n} romfs', [py, '-I', os.path.join(REPO, 'tools', 'engine', 'romfs_image_verify.py')] +
            ([] if not a.dry_run and unpacked(v100) else ['--from-report']), env, a.dry_run)
    else:   # (its .verified.json decides, as before)
        stamps.reused(2, 'romfs', done_at(next(p for p in (os.path.join(v100, f + '.verified.json') for f in
                                                        ('romfs.bin', 'romfs-source.txt')) if os.path.exists(p))))
    if a.until == 'romfs':
        say('DONE (--until romfs)')
        return
    # 3. images (build.py key computes them; it refuses an unsupported version). 1.4.3: first its tables, then every
    #    build.py run is that version's
    if update:
        version_tables(work, v100, vdir, py, env, f'3/{n} images', a.dry_run)
        env.update(TOTK_GAME_VERSION=UPDATE_VERSION, TOTK_VERSION_BETA='1')
    k = run(f'3/{n} images', [py, build, 'key'], env, a.dry_run, quiet=True)
    img = '<work>/engine/img/<id>' if a.dry_run else result(k)['img']
    # 4. patches: funcs.py on the clean image (kept per image and funcs.py version), then the default patch set,
    #    exported every run (seconds; a git pull may have changed the patches). 1.4.3: its own table's (sites_v143.py:
    #    the real-time frame step, hair and cloth, trampolines, fades; under cap30 the set keeps 30 fps)
    pargs = engine_patches(work, img, py, env, f'4/{n} patches', a.dry_run,
                           table=os.path.join(REPO, 'games', 'totk', 'patches', UPDATE_DIR + '.toml') if update else None)
    # the app icon (optional: a failure leaves the Windows icon, said, and the build goes on)
    env = app_icon(work, vdir, py, env, f'4/{n} icon', a.dry_run)
    # 5. build (resumable: build.py reuses every object already compiled)
    cpu =host_cpu() if a.cpu == 'auto' else a.cpu
    march = ['--march', 'x86-64-v2'] if cpu == 'v2' else []   # (the same flag to game and key: it joins the key)
    say(f'processor level: {cpu}' + (' (no AVX2 here: the build runs on this processor, slower than an AVX2 one)'
                                    if cpu == 'v2' and a.cpu == 'auto' else ''))
    if update:
        say(f'profile (PGO): none for {UPDATE_VERSION} (the release\'s profile was trained on 1.0.0): plain -O2')
    variant = march + ([] if update else pgo_args(work, a.no_pgo, a.dry_run, env.get('NXRECOMP_LLVM_DIR')))   # (both join the key)
    variant += hot_args(work, variant[len(march):], env, a.dry_run)   # (the hot list joins the key too)
    # no line tables in the game objects, as the release package (build.py --package): the exe links without a PDB, so
    # they serve nothing (crash reports name guest code through the function table); the same code, ~11% less compile
    # CPU, a quarter of the object bytes (setup-speed). Joins the key: one cold build at the update that brings it
    variant += ['--no-line-tables']
    # (the key first: one built from the same inputs, its objects done and its exe there, is reused whole)
    kinfo = None if a.dry_run else result(run(f'5/{n} build', [py, build, 'key'] + pargs + variant, env, a.dry_run,
                                              quiet=True))
    if a.dry_run or not (kinfo.get('built') and key_built(kinfo['dir']) and
                         stamps.reuse(5, 'build', dict(base, key=kinfo['key']),
                                      [os.path.join(kinfo['dir'], 'exe', EXE)])):
        say('step 5 is the long one: about 25-45 min the first time, minutes after that')
        run(f'5/{n} build', [py, build, 'game'] + pargs + ['--jobs', str(game_jobs), '--jobs-rt', str(rt_jobs)]
            + variant, env, a.dry_run)
        k = run(f'5/{n} build', [py, build, 'key'] + pargs + variant, env, a.dry_run, quiet=True)
        kinfo = None if a.dry_run else result(k)
        if kinfo:
            stamps.done(5, 'build', dict(base, key=kinfo['key']))
    key = '<key>' if a.dry_run else kinfo['key']
    # 6. the game-patch sets the Options menu switches between (grass / shadow / fog distance, texture cache, output
    #    and aspect sizes, fast travel): every value its sliders offer, as on the release PC
    sets = os.path.join(work, 'overlays')
    # (the screen shapes' UI layout sets: this PC's monitors' and those built on demand since, tools/engine/aspect_sets.py,
    #  not every shape: each is ~30 s and ~150 MB; another shape picked in the game is built at its next start)
    sys.path.insert(0, os.path.join(REPO, 'tools', 'engine'))
    import aspect_sets
    shapes = aspect_sets.setup_sets(work, write=not a.dry_run)
    say('screen shapes with their own layouts: ' + (', '.join(aspect_sets.label(s) for s in shapes) or 'none (16:9)'))
    if not update and not a.dry_run:   # (an earlier Setup built every shape: the others' layout sets go, ~150 MB each)
        gone = aspect_sets.drop_unwanted(work, shapes, say)
        if gone:
            say('layout sets of shapes not used here, removed: ' + ', '.join(gone))
    set_args = OVERLAY_SETS + ['--aspects', aspect_sets.sizes(shapes)]
    sets_in = {'args': set_args, 'code': git_id('games/totk/overlays')}
    if update:
        say(f'sets and overlay (steps 6 and 7): not for {UPDATE_VERSION} yet (made from 1.0.0\'s files: the game '
            'runs with its own menus and settings, no PC key glyphs)')
    elif a.dry_run or not done_with(os.path.join(sets, 'builder.json'), sets_in):
        run(f'6/{n} sets', [py, '-m', 'games.totk.overlays', '--romfs', v100, '--out', sets, *set_args], env,
            a.dry_run)
        if not a.dry_run:
            mark(os.path.join(sets, 'builder.json'), sets_in)
    else:   # (its builder.json decides, as before)
        stamps.reused(6, 'sets', done_at(os.path.join(sets, 'builder.json')))
    # 7. the engine's data overlay, its aspect copies and their PC key glyph banks (build.py overlay makes them all):
    #    ~1.5 min, again only for another dump or Setup version (a git pull may change it)
    #    (only the shapes' copies of step 6's sets: NXR_ASPECT_SETS; a shape's failed on-demand build is tried again)
    #    (ptbr: the 1.4.3 update unpacked here, whose Portuguese texts the 1.0.0 overlay serves (build.py, mals.py):
    #    one given since the last overlay makes it again)
    t7, bank = time.time(), os.path.join(work, 'engine', 'glyphs', 'engine+glyphs')
    ptbr = update_romfs_there(work)
    in7 = dict(base, aspects=shapes, ptbr=ptbr)
    pt100 = os.path.join(work, 'engine', 'overlay', 'romfs', 'Mals', 'USpt.Product.100.sarc.zs')
    if update and ptbr and not os.path.isfile(pt100) and same_100_package(out, head):
        # (the 1.0.0 game of this same Setup next to it: its overlay gets the Portuguese now, from this update; a 1.0.0
        #  game of another Setup version gets it at its next Setup, whose overlay matches its exe. Optional: a failure
        #  leaves the 1.0.0 overlay as it was)
        ok, _ = run(f'7/{n} overlay (1.0.0: Brazilian Portuguese texts from the update)', [py, build, 'overlay', '--force'],
                    dict(env, NXR_ASPECT_SETS=','.join(shapes)), a.dry_run, quiet=True, tolerate=True)
        if not a.dry_run:
            say('1.0.0 game: Portuguese (Brasil) texts ' + ('ready (F10 > Game > Language)' if ok and os.path.isfile(pt100)
                                                            else 'not made this time (above); the next Setup tries again'))
    # (the skip-opening set too: an install whose step 7 ran before the set existed, or whose set was deleted, would
    #  otherwise reuse step 7 without it, and 'Skip the opening' finds nothing; public #65, rival-034)
    skipset = os.path.join(work, 'engine', 'overlay', 'skip_opening', 'manifest.tsv')   # (a file the set always has: an
    #  empty folder left by a broken run doesn't count)
    if not update and not stamps.reuse(7, 'overlay', in7, [bank, skipset]):
        run(f'7/{n} overlay', [py, build, 'overlay', '--force'], dict(env, NXR_ASPECT_SETS=','.join(shapes)), a.dry_run)
        if not a.dry_run and not (os.path.isdir(bank) and os.path.getmtime(bank) >= t7 - 2):   # (made by this step)
            fail('no PC key glyph bank was made: the overlay step says why above')
        if not a.dry_run:
            for f in glob.glob(os.path.join(work, 'engine', 'overlay', 'romfs-aspect-*.failed')):
                os.remove(f)
        stamps.done(7, 'overlay', in7)
    # DLSS's runtime DLLs (~66 MB): an NVIDIA RTX GPU only (build.py dlss_dlls), at every run, so an RTX card added
    # later gets them at the next Setup; never stops Setup (offline: DLSS unavailable until a run that is online).
    # Before step 8: package.py writes TOTK_DLSS_DIR only when the DLLs are there, and in8 says whether they are, so
    # the run that fetches them packages again
    ok, out_dlss = run('DLSS (NVIDIA RTX only)', [py, build, 'dlss'], env, a.dry_run, quiet=True, tolerate=True)
    if not a.dry_run:
        said = [ln.split('] ', 1)[-1] for ln in out_dlss.splitlines() if 'DLSS' in ln]
        say(said[-1] if ok and said else 'DLSS: not checked this time (above); the next Setup tries again')
    # 8. package (package.py points the launcher at <work>/overlays itself, and copies <work>/engine/seed as cache-seed)
    if not update:   # (1.4.3: package.py ships no seed)
        shader_seed(work, a.dry_run)
        fsr_runtime(work, a.dry_run)
    # (--commit: the source the exe was just built from. Without it package.py names the folder after the commit the
    #  key was first made from, so an update that changed only the runtime kept the older version's name)
    rel = release_of(REPO)   # (the launcher's automatic update: this build's release tag and where it comes from)
    # (TOTK_EXEFS_BUILD: the game builds itself again with its mods' code patches, tools/engine/mods_exefs.py, with
    #  this build's tools and flags)
    exefs = (f'"{py}" "{os.path.join(REPO, "tools", "engine", "mods_exefs.py")}" '
             + (f'--install "{install}"' if install else f'--work "{work}"') + f' --cpu {cpu}'
             + (' --no-pgo' if a.no_pgo else ''))
    # (TOTK_ASPECT_BUILD: a screen shape picked in the game whose layouts step 7 did not make, built at its next start:
    #  tools/engine/aspect_sets.py)
    aspect = (f'"{py}" "{os.path.join(REPO, "tools", "engine", "aspect_sets.py")}" '
              + (f'--install "{install}"' if install else f'--work "{work}"'))
    # (reused: the newest game folder is this key's, from this source, and the stamp says it was packaged so)
    # (and whether the firmware's fonts are there: a failed read is optional now (#26), a later success must repackage;
    #  the same for DLSS's DLLs: player.txt names TOTK_DLSS_DIR only when they are there)
    in8 = dict(base, key=key, release=rel, out=os.path.normcase(out), exefs=exefs, aspect=aspect,
               fonts=os.path.isfile(os.path.join(vdir, 'fonts-report.json')),
               dlss=os.path.isfile(os.path.join(work, 'engine', 'dlss', 'nvngx_dlss.dll')))   # (TOTK_DLSS_DIR)
    tag = UPDATE_DIR if update else ''
    newest = None if a.dry_run else newest_package(out, tag)
    pt = player_txt(newest) if newest else {}
    c = pt.get('commit', '')
    if a.dry_run or not (pt.get('key') == key and head and c and (head.startswith(c) or c.startswith(head)) and
                         stamps.reuse(8, 'package', dict(in8, folder=os.path.basename(newest)),
                                      [os.path.join(newest, EXE)])):
        run(f'8/{n} package', [py, os.path.join(REPO, 'engine', 'launcher', 'win', 'package.py'), '--key', key,
                               '--out', out, '--env', f'TOTK_EXEFS_BUILD={exefs}', '--env', f'TOTK_ASPECT_BUILD={aspect}']
            + (['--commit', head] if head else []) + (['--release', rel[0], '--release-repo', rel[1]] if rel else []),
            env, a.dry_run)
        newest = None if a.dry_run else newest_package(out, tag)
        if not a.dry_run and not newest:
            fail(f'no playable folder in {out} after packaging')
        if newest:
            stamps.done(8, 'package', dict(in8, folder=os.path.basename(newest)))
    # 9. the shaders the game would compile the first time it sees them, built now into the player's own cache
    #    (reused for the same game folder, GPU and driver while the cache it made is still there: it lives in the
    #    player's profile, %LOCALAPPDATA%, which an uninstall or the player may clear; a stopped or failed one runs
    #    again); keyed on what the shaders are made from (shader_inputs), not on the Setup commit or the folder
    in9 = dict({k: v for k, v in base.items() if k != 'setup'}, gpus=gpus(),
               **(shader_inputs(newest or out) if not a.dry_run else {}))
    cold = None if a.dry_run else os.path.join(prepare_env(newest or out)[0], 'cache', 'precompile_cold.key')
    if not update and not stamps.reuse(9, 'shaders', in9, [cold] if cold else []):   # (1.4.3: none recorded)
        if prepare_shaders(newest or out, n, a.dry_run) in (0, 1, 2):
            stamps.done(9, 'shaders', in9)
    if a.dry_run:
        say('check done (--dry-run): nothing was built')
        return
    if NESTED:   # (the 1.0.0 pass of an --update install: the --update run says DONE, with its own folder)
        if install:
            prune(out, newest)
        say(f'updated: {newest}')
        return
    if update and install:
        main_100(argv, out, head)
    if install:
        prune(out, newest)
        try:   # (what a later update does not need: earlier builds, unused cached objects; tools/builder/tidy.py)
            import tidy
            tidy.tidy(work, key, head, log=say)
        except Exception as e:   # (never fails an install that worked)
            say(f'tidy: skipped ({e})')
    say('DONE: ' + (newest if LINUX else os.path.join(newest, 'TotK Launcher.exe')))


if __name__ == '__main__':
    main()
