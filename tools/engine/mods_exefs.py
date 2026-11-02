"""Builds the game with the code patches of the player's mods (.ips / .pchtxt writes and code cheats into main's .text) and
swaps the new totk.exe into the playable folder. The code cheats, on or off, are built in as switch sites (gen_ext.py:
the cheat's words or the game's, by a flag the F10 Cheats tab sets live). The game starts it (engine/runtime/mods_exefs.cpp, TOTK_EXEFS_BUILD,
which package.py writes for a Builder install) when the mods' code patches differ from the ones its exe holds, and exits;
this waits for it, builds, and starts the launcher again.

    <python> tools/engine/mods_exefs.py (--install DIR | --work DIR) [--cpu v3|v2] [--no-pgo]
        --request <exe dir>/exefs/<set>/request.json --exe-dir <the playable folder> [--pid N]

The build is a patched key (tools/engine/build.py game --patches): the playable folder's own patch set (player.txt
patches=) plus the mods' writes, named mod:<file>, with the code caves they branch into declared as the port's veneers
are (games/totk/patches/engine.py): a cave entered by bl becomes a function of its own (fde_add), one entered by b or a
conditional branch an extension of the function the branch is in (ext). Only the functions the writes touch compile again
(the object cache has the rest): about 3 minutes warm. An empty request (the mods' code patches all off) builds the
folder's own set again, which the object cache and the key folder already hold.

The request folder gets build.log; a failed build leaves failed.txt there (the game then runs without those patches and
does not ask again for the same set) and the old exe in place.

Rules after totk-pc-recomp's installer (rebuild for ExeFS patches; MIT License, Copyright (c) 2026 Valiantsin Dzerakh).
"""
import argparse
import bisect
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)
from games.totk.patches.engine import branch  # noqa: E402  (the veneer rules' branch decoder)


class Refused(Exception):
    pass


def player_txt(exe_dir):
    out = {}
    with open(os.path.join(exe_dir, 'player.txt'), encoding='utf-8') as f:
        for ln in f:
            k, sep, v = ln.rstrip('\r\n').partition('=')
            if sep:
                out[k.strip()] = v.strip()
    return out


def key_dir(work, key):
    for sub in ('game', 'game-var'):
        d = os.path.join(work, 'engine', sub, key)
        if os.path.isdir(d):
            return d
    raise Refused(f'the build {key} is not in {work}\\engine (a Builder update removed it?): run Setup again')


def fde_map(funcs_json):
    with open(funcs_json) as f:
        fde = json.load(f)['fde']
    return {int(k, 16) if not k.startswith('0x') else int(k, 0): v for k, v in fde.items()}


def words_of(image, patches):
    """{word offset: new word} of every word the patches touch, the bytes they leave as the image has them"""
    buf = {}
    for p in patches:
        off, new = int(str(p['offset']), 0), bytes.fromhex(p['bytes'])
        for i, b in enumerate(new):
            buf[off + i] = b
    out = {}
    for a in sorted({o - o % 4 for o in buf}):
        w = bytes(buf.get(a + i, image[a + i]) for i in range(4))
        out[a] = int.from_bytes(w, 'little')
    return out


def check_build_id(request, img_dir):
    """the request's offsets are for the build the game ran (exefs_scan.h request_json): refuses another version's image"""
    want = (request.get('build_id') or '').lower()
    with open(os.path.join(img_dir, 'main.json')) as f:
        have = (json.load(f).get('build_id') or '').lower()
    if want and have and not have.startswith(want[:40]):
        raise Refused(f'the request is for main {want[:16]}, this folder plays main {have[:16]} (another game version)')


def text_end(img_dir):
    """where main's .text (and its padding) ends: the start of .rodata (img/main.json)"""
    with open(os.path.join(img_dir, 'main.json')) as f:
        return next(s['mem'] for s in json.load(f)['segments'] if s['name'] == 'ro')


def caves(words, fde, end):
    """(fde_add, ext) for the code the writes put outside every function and branch into (see the docstring); end:
    text_end"""
    starts = sorted(fde)

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 and a < starts[i] + fde[starts[i]] else None

    def block(t):
        e = t
        while e in words and owner(e) is None:
            e += 4
        return t, e

    fde_add, ext, seen = {}, {}, {}
    todo = [(a, w, owner(a)) for a, w in sorted(words.items()) if a < end and owner(a) is not None]
    while todo:
        a, w, fn = todo.pop(0)
        br = branch(w, a)
        if not br or owner(br[1]) is not None or br[1] in fde_add:
            continue
        kind, t = br
        if t not in words:
            raise Refused(f'main+{a:#x} branches to main+{t:#x}, outside every function, where the patch writes nothing')
        if t in seen:
            if seen[t] != (kind == 'bl', fn):
                raise Refused(f'the code at main+{t:#x} is entered from two places of different kinds')
            continue
        s, e = block(t)
        seen[t] = (kind == 'bl', fn)
        if kind == 'bl':
            fde_add[s] = e - s
            inner = s
        else:
            ext.setdefault(fn, []).append((s, e))
            inner = fn
        todo += [(x, words[x], inner) for x in range(s, e, 4)]
    return fde_add, ext


def switch_sites(cheats, image, fde):
    """the code cheats' writes (request patches with a `switch` slot, exefs_scan.h request_json), each cheat whole:
    ({"0x<word>": [[slot, word hex], ...]} for its words inside a function, chosen at run time by the cheat's flag
    (gen_ext.py), [patch] for the rest (a cave in the padding it branches into, written as any patch), {slot: words})"""
    starts = sorted(fde)

    def inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and a < starts[i] + fde[starts[i]]

    by_slot = {}
    for p in cheats:
        off, new = p['offset'], bytes.fromhex(p['bytes'])
        if image[off:off + len(new)].hex() != p['expect']:
            raise Refused(f"main+{off:#x} is {image[off:off + len(new)].hex()}, the cheat ({p.get('from', '?')}) expects "
                          f"{p['expect']}")
        by_slot.setdefault(int(p['switch']), []).append(p)
    sites, baked, words, cave_owner = {}, [], {}, {}
    for slot, ps in sorted(by_slot.items()):
        words[slot] = words_of(image, ps)
        for a, w in sorted(words[slot].items()):
            new = w.to_bytes(4, 'little').hex()
            if inside(a):
                sites.setdefault(hex(a), []).append([slot, new])
                continue
            if cave_owner.setdefault(a, (slot, new))[1] != new:
                raise Refused(f'main+{a:#x}: two code cheats write different code there, outside every function')
            if cave_owner[a][0] == slot:
                baked.append({'offset': a, 'bytes': new, 'expect': image[a:a + 4].hex(), 'from': ps[0].get('from', '?')})
    return sites, baked, words


def patch_set(base, request, image, fde, end):
    """the folder's patch set plus the request's writes and their caves; the code cheats' as switch sites"""
    pset = json.loads(json.dumps(base))
    mods = request.get('patches') or []
    if not mods:
        return pset
    files = [p for p in mods if 'switch' not in p]
    sites, baked, cheat_words = switch_sites([p for p in mods if 'switch' in p], image, fde)
    for p in files + baked:
        pset['patches'].append({'module': 'main', 'offset': hex(p['offset']), 'bytes': p['bytes'], 'expect': p['expect'],
                                'name': 'mod:' + p.get('from', '?')})
    if sites:
        pset['switch'] = sites
    file_words = words_of(image, files)
    fde_add, ext = caves(file_words, fde, end)
    for words in cheat_words.values():   # (each cheat's branches with the files' code: its caves)
        fa, ex = caves({**file_words, **words}, fde, end)
        fde_add.update(fa)
        for fn, ranges in ex.items():
            ext.setdefault(fn, []).extend(r for r in ranges if r not in ext.get(fn, []))
    fa = pset.setdefault('fde_add', {})
    for s, n in fde_add.items():
        fa[hex(s)] = n
    ex = pset.setdefault('ext', {})
    for fn, ranges in ext.items():
        ex.setdefault(hex(fn), []).extend([hex(a), hex(b)] for a, b in ranges)
    pset['name'] = f"{pset.get('name', 'patches')}+mods-{request.get('set', '')[:8]}"
    return pset


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


def version_env(player):
    """the game version the folder plays (package.py's version= line, absent for 1.0.0): build.py game compiles that
    version's NSO, as the Builder's step 3 does for an update install"""
    v = player.get('version', '1.0.0')
    return {} if v == '1.0.0' else {'TOTK_GAME_VERSION': v, 'TOTK_VERSION_BETA': '1'}


def icon_env(work):
    """the app icon the Builder made (builder.py app_icon), linked into this totk.exe too; none: Windows' own"""
    res = os.path.join(work, 'engine', 'icon', 'totk.res')
    return {'TOTK_APP_ICON_RES': res} if os.path.isfile(res) else {}


def builder_env(install, work, **extra):
    """(the Builder module, the environment its steps run in: its tools, this work folder)"""
    sys.path.insert(0, os.path.join(REPO, 'tools', 'builder'))
    import builder as B
    _, _, tool_env = B.check_tools(managed=bool(install))
    env = dict(os.environ, **tool_env, NXRECOMP_WORK=work, NXR_SOLO_PC='1', PYTHONIOENCODING='utf-8', **extra)
    if install:
        import toolchain
        env = toolchain.apply(toolchain.ensure(os.path.join(install, 'tools')), env)
    return B, env


def build(a, pset_path, log, player=None):
    """build.py game with the Builder's tools and flags; returns its result (key, exe)"""
    install = os.path.abspath(a.install) if a.install else None
    work = os.path.join(install, 'work') if install else os.path.abspath(a.work)
    sys.path.insert(0, os.path.join(REPO, 'tools', 'builder'))
    import builder as B
    ram, _ = B.memory_gb()
    game_jobs, rt_jobs = B.jobs_for(ram, os.cpu_count() or 2)
    B, env = builder_env(install, work, NXR_GAME_JOBS_MAX=str(game_jobs), NXR_BUILDER_PACKAGE='1',
                         **version_env(player or {}), **icon_env(work))
    cpu = B.host_cpu() if a.cpu == 'auto' else a.cpu
    variant = (['--march', 'x86-64-v2'] if cpu == 'v2' else []) + B.pgo_args(work, a.no_pgo, False)
    cmd = [sys.executable, os.path.join(REPO, 'tools', 'engine', 'build.py'), 'game', '--patches', pset_path,
           '--jobs', str(game_jobs), '--jobs-rt', str(rt_jobs)] + variant
    log('build: ' + ' '.join(cmd))
    out = B.run('mods build', cmd, env, False)
    return B.result(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--install')
    ap.add_argument('--work')
    ap.add_argument('--cpu', choices=['auto', 'v3', 'v2'], default='auto')
    ap.add_argument('--no-pgo', action='store_true')
    ap.add_argument('--request', required=True)
    ap.add_argument('--exe-dir', required=True)
    ap.add_argument('--pid', type=int)
    ap.add_argument('--no-start', action='store_true', help='do not start the launcher afterwards (tests)')
    a = ap.parse_args(argv)
    req_dir = os.path.dirname(os.path.abspath(a.request))
    logf = open(os.path.join(req_dir, 'build.log'), 'a', encoding='utf-8')

    def log(msg):   # (to the console and build.log: Tee below)
        print(time.strftime('%H:%M:%S ') + msg, flush=True)

    class Tee:   # the build's output into build.log too
        def __init__(self, out):
            self.out = out

        def write(self, t):
            if self.out:   # (None under pythonw: no console)
                self.out.write(t)
            logf.write(t)
            return len(t)

        def flush(self):
            if self.out:
                self.out.flush()
            logf.flush()

    sys.stdout = Tee(sys.stdout)
    log('Applying the code patches of your mods: the game is built again with them (a few minutes); it starts again '
        'by itself.')
    try:
        wait_pid(a.pid, log)
        with open(a.request, encoding='utf-8') as f:
            request = json.load(f)
        player = player_txt(a.exe_dir)
        if not a.install and not a.work:
            raise Refused('no --install / --work: this folder was not made by the Builder')
        work = os.path.join(os.path.abspath(a.install), 'work') if a.install else os.path.abspath(a.work)
        if player.get('patches', 'none') == 'none':   # (package.py's line for a key built without a patch set)
            base = {'name': 'patches', 'patches': []}
        else:
            with open(player['patches'], encoding='utf-8') as f:
                base = json.load(f)
        kd = key_dir(work, player['key'])
        check_build_id(request, player['img'])
        with open(os.path.join(player['img'], 'main.bin'), 'rb') as f:
            image = f.read()
        pset = patch_set(base, request, image, fde_map(os.path.join(kd, 'funcs_main.json')), text_end(player['img']))
        pset_path = os.path.join(req_dir, 'engine-patches.json')
        with open(pset_path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(pset, f, indent=1)
        log(f"{len(request.get('patches') or [])} patched ranges, {len(pset.get('fde_add', {})) - len(base.get('fde_add', {}))} "
            f"cave functions, set {request.get('set') or 'none'}")
        try:
            res = build(a, pset_path, log, player)
        except SystemExit as e:
            raise Refused(f'the build failed ({e}): the end of its output is in {logf.name}')
        tmp = os.path.join(a.exe_dir, 'totk.exe.new')
        shutil.copyfile(res['exe'], tmp)
        os.replace(tmp, os.path.join(a.exe_dir, 'totk.exe'))
        sets = os.path.join(a.exe_dir, 'exefs-set.txt')
        src = os.path.join(req_dir, 'exefs-set.txt')
        if request.get('set') and os.path.isfile(src):
            # (the exe it describes: mods_exefs.cpp exe_stamp; any other exe in its place holds none of it)
            st = os.stat(os.path.join(a.exe_dir, 'totk.exe'))
            with open(src, encoding='utf-8') as f:
                body = f.read()
            with open(sets + '.new', 'w', encoding='utf-8', newline='\n') as f:
                f.write(f'exe {st.st_size} {st.st_mtime_ns}\n' + body)
            os.replace(sets + '.new', sets)
        elif os.path.exists(sets):
            os.remove(sets)
        log(f"done: totk.exe is build {res.get('key')}")
    except BaseException as e:   # (anything: failed.txt and the restart always happen, or every start would exit again)
        if isinstance(e, KeyboardInterrupt):
            raise
        log(f'not applied: {type(e).__name__}: {e}')
        with open(os.path.join(req_dir, 'failed.txt'), 'w', encoding='utf-8') as f:
            f.write(str(e) + '\n')
    if not a.no_start:
        relaunch(a.exe_dir, log)
    sys.stdout = sys.stdout.out
    logf.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
