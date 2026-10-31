#!/usr/bin/env python3
"""

    py pipeline/build.py --nsp <game.nsp> --keys <prod.keys> [--firmware <dir>] [--work <dir>] [-o <out dir>]

Steps (in order; each is skipped when its manifest entry says the same inputs produced outputs that still verify):
  exefs      NSP -> <work>/extracted/v100/exefs (PFS0 hash table + every block verified, NSO hashes pinned)
  romfs      NSP -> <work>/extracted/v100/romfs.bin (+ index, + unpacked romfs/ tree); IVFC levels 0-5 verified,
             image size + SHA-256 pinned
  fonts      firmware -> <work>/extracted/v100/fonts/*.bfttf (IVFC + BFTTF header verified; pinned hashes compared)
  modules    ExeFS NSOs -> <work>/images/<m>.img, <work>/elf/<m>.elf, <work>/layout.json
  ready      <work>/extracted/v100/READY.json (what other tools poll for)
  patch      python -m games.totk.patches apply --image <work>/images/main.img --out <work>/patched/main.img
             --features <--patch-features> 
  recompile  python -m recomp --layout <work>/layout.json --module <m> --out <staging> --opt <level> --decomp <on|off>
             per module whose generation inputs changed (main from the patched image: --image <work>/patched/main.img
             --patches <...>.patches.json), then only files whose bytes changed are moved into
             <work>/generated[-<tree>]/<m> (unchanged shards keep their mtime)
  configure  CMake (clang-cl, lld-link, Ninja, vcpkg x64-windows-static-md) into <work>/build/cmake[-<tree>]
  build      cmake --build (at most --jobs parallel compiles; big guest compiles through tools/heavy.py): the guest
             libraries first, checked against the tree's generation fingerprint, then TotK.exe

Build trees (variant + decomp setting) never share generated code, stamps or libraries. A generated module records
its generation fingerprint (tree, variant, decomp, codegen level, recomp/simd and recompiler code, replaced.txt,
overrides, layout, patched main) in <gen>/generation.json and compiles it into guest_<m>.lib as the symbol
nx_generation_<m>_<sha256>. The recompile step regenerates a module whose generation.json is not this tree's; the build
step refuses to link (and the package step to ship) a guest library whose fingerprint is not this tree's, and a
failed or refused TotK build removes the tree's packaged TotK.exe so nothing can boot the previous one by mistake.

--decomp on pins one commit at the start of the run (--decomp-rev, default HEAD), so decomp batches committing during a
build land in the next build instead of invalidating this one. The commit is in the step table, build-report.json,
generation.json and nxrecomp-data.json.
  package    <out>/TotK[-<variant>]/: TotK.exe (+ .pdb), config.toml + TotK.cmd, nxrecomp-data.json, fonts/

Boot loop: `py pipeline/build.py --quick` = patch, recompile (changed modules only), build TotK (no explicit
configure: Ninja re-runs CMake itself when a CMakeLists changes), package. Variants: --variant dev|release|profile
(--cpu znver5, --lto for release).

Keys are only passed around as a path and never printed. Heavy work runs at BELOW_NORMAL priority and waits while
free RAM < 6 GB."""
import argparse
import filecmp
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_WORK = Path(r'C:\nxrecomp-work')
VERSION_DIR = 'v100'
GUEST_MODULES = ('main', 'subsdk0', 'sdk')
AREAS = ('runtime', 'gfx/core', 'gfx/shaders', 'gfx/textures', 'platform', 'games/totk')
STEPS = ('exefs', 'romfs', 'fonts', 'modules', 'ready', 'patch', 'recompile', 'configure', 'build', 'package')


def _ensure_python():
    """Re-run under the work folder's venv when this interpreter lacks the pipeline's packages."""
    if all(importlib.util.find_spec(m) for m in ('cryptography', 'lz4')):
        return
    work = DEFAULT_WORK
    for i, arg in enumerate(sys.argv):
        if arg == '--work' and i + 1 < len(sys.argv):
            work = Path(sys.argv[i + 1])
        elif arg.startswith('--work='):
            work = Path(arg.split('=', 1)[1])
    venv = work / '.venv' / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
    if venv.exists() and Path(sys.executable).resolve() != venv.resolve() and not os.environ.get('NXRECOMP_REEXEC'):
        env = dict(os.environ, NXRECOMP_REEXEC='1')
        sys.exit(subprocess.call([str(venv), str(Path(__file__).resolve()), *sys.argv[1:]], env=env))
    sys.exit('build.py needs the packages cryptography and lz4 (the work folder venv has them): '
             f'{venv} not usable')


_ensure_python()
sys.path.insert(0, str(REPO))

from pipeline import extract, modules  # noqa: E402
from pipeline.locks import FileLock  # noqa: E402
from pipeline.manifest import Manifest  # noqa: E402
from pipeline.sync import sync_tree  # noqa: E402
from pipeline.nx.common import (GiB, below_normal_priority, free_disk_bytes, read_json, require,  # noqa: E402
                                sha256_file, wait_for_ram, write_json)

MIN_FREE_DISK = 40 * GiB
HEAVY = REPO / 'tools' / 'heavy.py'
LIGHT_OBJECTS = 24          # up to this many objects to compile is a small build (no slot limiter)
QUICK_STEPS = ('patch', 'recompile', 'configure', 'build', 'package')
SHARED_STEPS = ('exefs', 'romfs', 'fonts', 'modules', 'ready', 'patch')   # run under locks/shared.lock
GENERATION = 'generation.json'   # per generated module: its fingerprint, written once the module is complete
GENERATION_FORMAT = 1
PGO_TAG = {'off': '', 'gen': '-pgogen', 'use': '-pgo'}   # tree name / build dir / package suffix per --pgo


def heavy(cmd, log=print):
    """    started while free RAM < 8 GB, child at BELOW_NORMAL. Without tools/heavy.py, fall back to the RAM gate."""
    if HEAVY.exists():
        return [sys.executable, str(HEAVY), '--', *cmd]
    wait_for_ram(log=log)
    return list(cmd)


class BuildError(Exception):
    pass


def log(msg):
    print(msg, flush=True)


def file_identity(path):
    st = Path(path).stat()
    return dict(path=str(Path(path).resolve()), size=st.st_size, mtime_ns=st.st_mtime_ns)


def tree_files(root, pattern='*'):
    return sorted(p for p in Path(root).glob(pattern) if p.is_file())


def code_identity(*roots, base=REPO):
    """Content hash of the Python/C sources under `roots` (a tool's identity: rerun a step when its code changes).
    Paths are keyed relative to `base` (the repo, or a pinned snapshot of it: same content, same identity)."""
    h = hashlib.sha256()
    for root in roots:
        root = Path(root)
        if not root.exists():
            continue
        for p in sorted(root.rglob('*')):
            if p.is_file() and p.suffix in ('.py', '.h', '.c', '.json') and '__pycache__' not in p.parts:
                h.update(p.relative_to(base).as_posix().encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


def fingerprint_of(inputs):
    """Generation fingerprint: hash of a module's generation inputs (canonical JSON)."""
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def sources_digest(directory):
    """Names and sizes of a generated module's top-level C/H files: what CMake globs and compiles."""
    h = hashlib.sha256()
    count = 0
    for p in sorted(Path(directory).glob('*')):
        if p.is_file() and p.suffix in ('.c', '.h'):
            h.update(f'{p.name}\0{p.stat().st_size}\n'.encode())
            count += 1
    return dict(files=count, digest=h.hexdigest())


def generation_source(module, fingerprint, tree):
    """nx_generation_<module>.c: the fingerprint as a symbol name, so it reaches guest_<module>.lib's symbol table
    whatever the object format (COFF, or bitcode under --lto)."""
    sym = f'nx_generation_{module}_{fingerprint}'
    return (f'/* Written by pipeline/build.py: generation fingerprint of this module (see {GENERATION}). build.py checks\n'
            f'   this symbol in guest_{module}.lib before linking TotK.exe. */\n'
            f'extern const char {sym}[];\n'
            f'const char {sym}[] = "{tree}";\n')


def lib_fingerprints(lib, module):
    """The generation fingerprints guest library `lib` carries (symbols nx_generation_<module>_<sha256>)."""
    import mmap
    prefix = f'nx_generation_{module}_'.encode()
    found = set()
    with open(lib, 'rb') as f:
        if os.fstat(f.fileno()).st_size == 0:
            return found
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            i = mm.find(prefix)
            while i >= 0:
                tail = mm[i + len(prefix):i + len(prefix) + 65]
                if re.fullmatch(rb'[0-9a-f]{64}', tail[:64]) and not re.match(rb'[0-9a-zA-Z_]', tail[64:]):
                    found.add(tail[:64].decode())
                i = mm.find(prefix, i + 1)
    return found


def failing_area(text, areas):
    """The in-tree area whose CMakeLists.txt (or a file under it) a CMake error points at."""
    import re
    root = REPO.as_posix().lower()
    text = text.replace(chr(92), '/')
    # Error locations first, then call-stack frames (vcpkg's find_package wrapper is often the error location).
    paths = re.findall(r'CMake Error at ([^\n]+?):\d+', text)
    paths += re.findall(r'^\s+([^\s][^\n]*?/CMakeLists\.txt|[^\s][^\n]*?\.cmake):\d+ \(', text, flags=re.M)
    for path in paths:
        path = path.strip().lower()
        if path.startswith(root + '/'):
            path = path[len(root) + 1:]
        for area in sorted(areas, key=len, reverse=True):
            if path.startswith(area + '/'):
                return area
    return None


def place(src, dst):
    """Put build output `src` at `dst`: a hard link when possible (instant, no extra disk for a 370 MB exe / 1 GB pdb;
    lld writes each link to a new file, so the packaged copy never changes under a running game), else a copy.
    A `dst` that is in use (TotK.exe running) is left as it is and reported."""
    src, dst = Path(src), Path(dst)
    try:
        if dst.exists():
            if os.path.samefile(src, dst):
                return f'{dst.name} (linked)'
            dst.unlink()
        try:
            os.link(src, dst)
            return f'{dst.name} (linked)'
        except OSError:
            shutil.copy2(src, dst)
            return f'{dst.name} (copied)'
    except PermissionError:
        return f'{dst.name} NOT UPDATED (in use: close TotK.exe and package again)'


def link_dir(link, target):
    """Make `link` a directory junction to `target` (no copy, no admin rights). An existing junction to the same
    target is kept; anything else already at `link` is left alone and reported."""
    link, target = Path(link), Path(target)
    if os.path.isjunction(link) or link.is_symlink():
        if Path(os.path.realpath(link)) == Path(os.path.realpath(target)):
            return f'{link} -> {target} (junction)'
        os.rmdir(link)   # removes the junction itself, never the target's contents
    elif link.exists():
        return f'{link} exists and is not a junction: left alone (expected a link to {target})'
    p = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True, text=True)
    if p.returncode != 0:
        raise BuildError(f'could not create junction {link} -> {target}: {p.stdout.strip()} {p.stderr.strip()}')
    return f'{link} -> {target} (junction created)'


def summarize_failures(text):
    """{target: (failed objects, first compiler error)} from a Ninja log (`FAILED: <path>/CMakeFiles/<t>.dir/...`)."""
    import re
    out = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if not line.startswith('FAILED:'):
            continue
        m = re.search(r'CMakeFiles[\\/]([^\\/]+)\.dir', line)
        target = m.group(1) if m else line[8:60]
        first = next((x.strip()[:200] for x in lines[i + 1:i + 60] if 'error:' in x or ': error ' in x), '?')
        count, prev = out.get(target, (0, first))
        out[target] = (count + 1, prev)
    return out


def first_cmake_error(text):
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith('CMake Error'):
            body = ' '.join(x.strip() for x in lines[i + 1:i + 4] if x.strip())
            return body[:160]
    return 'unknown error'


class Build:
    def __init__(self, args):
        self.a = args
        self.work = args.work.resolve()
        self.ext = self.work / 'extracted' / VERSION_DIR
        self.expected = read_json(args.expected)
        self.manifest = Manifest(self.work)
        self.results = []
        self.changed_sources = {}   # module -> generated C/H files the last recompile changed
        self.shard_set_changed = False
        # Decompiled functions: a persisted switch, on unless asked otherwise.
        if args.decomp == 'both':
            raise BuildError('internal: --decomp both is expanded by main()')
        self.decomp = args.decomp or self.manifest.setting('decomp', 'on')
        if args.decomp:
            self.manifest.set_setting('decomp', args.decomp)
        # Codegen level: passed to the recompiler explicitly, so the environment of a later run cannot change it.
        try:
            from recomp.api import opt_level
            self.opt = opt_level(args.opt)
        except ImportError:   # no recompiler in this checkout: the recompile step is skipped anyway
            self.opt = args.opt if args.opt is not None else int(os.environ.get('NXRECOMP_OPT', '').strip() or 1)
        self.guest_fingerprints = None   # {module: fingerprint} verified in guest_<m>.lib by this run's build step
        self.run_patched = None          # (patched main, patches) this run's recompile read: the tree's snapshot
        self.patch_ran = False           # this run's patch step ran (the shared image may differ from the snapshot)
        self.decomp_commit = None  # decomp on
        self.recomp_commit = None        # the commit this run takes recomp/ + include/ from (pin_recomp)
        self.recomp_root = REPO          # where they are: <work>/build/recomp-pinned/<commit>, or the worktree
        # One build tree per variant + decomp setting: its own generated sources, build dir and package, guarded by
        # its own lock for the whole run (pipeline/locks.py). Shared inputs have a separate, short-held lock.
        self.gen_root = self.work / ('generated' + self.suffix)
        self.tree_lock = FileLock(self.work / 'locks' / f'tree{self.suffix}.lock', log,
                                  what=f'build tree "{self.suffix[1:] or "default"}"')
        self.shared_lock = FileLock(self.work / 'locks' / 'shared.lock', log, what='shared inputs')

    # ---- framework -------------------------------------------------------------------------------------------------
    def run(self, steps):
        below_normal_priority()
        self.work.mkdir(parents=True, exist_ok=True)
        free = free_disk_bytes(self.work)
        if free < MIN_FREE_DISK:
            raise BuildError(f'Only {free / GiB:.0f} GB free on the work drive (< 40 GB): stopping')
        t_all = time.monotonic()
        with self.tree_lock:
            if any(s in QUICK_STEPS for s in steps):   # the extract-only steps (the Builder's) use neither
                self.pin_recomp()
                if self.decomp == 'on':
                    self.pin_decomp()
            self.run_steps(steps, t_all)

    def pin_recomp(self):
        """The commit this whole run takes the recompiler and the guest headers from (recomp/ + include/
        The recompiler runs from there, the generation fingerprints hash it, and CMake compiles the shards against
        its headers and toolchain.py CFLAGS (NXRECOMP_RECOMP_ROOT). So other workers' uncommitted recomp/ edits are
        never built, and a commit or edit during the run cannot make the fingerprint checks fail half-way
        ("generated for another configuration (code, simd)"). --recomp-rev worktree: the working tree, as before."""
        if self.recomp_commit or self.recomp_root != REPO:
            return self.recomp_commit
        rev = getattr(self.a, 'recomp_rev', None) or 'HEAD'
        if rev == 'worktree':
            if not getattr(self, '_worktree_logged', False):
                log('   recomp/ from the working tree (--recomp-rev worktree): uncommitted changes are built')
                self._worktree_logged = True
            return None

        def git(*args):
            p = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True)
            if p.returncode != 0:
                raise BuildError(f"git {' '.join(args)} failed: {p.stderr.decode(errors='replace').strip()[:300]}")
            return p.stdout

        commit = git('rev-parse', '--verify', f'{rev}^{{commit}}').decode().strip()
        pins = self.work / 'build' / 'recomp-pinned'
        root = pins / commit[:16]
        done = root / '.complete'
        if not done.is_file():
            import io
            import tarfile
            tmp = pins / f'{commit[:16]}.tmp{os.getpid()}'
            if tmp.exists():
                shutil.rmtree(tmp)
            tmp.mkdir(parents=True)
            with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', commit, 'recomp', 'include'))) as tar:
                tar.extractall(tmp, filter='data')
            (tmp / '.complete').write_text(commit + '\n', encoding='utf-8')
            try:
                os.replace(tmp, root)
            except OSError:   # another tree's run pinned the same commit first: use its copy
                shutil.rmtree(tmp, ignore_errors=True)
                if not done.is_file():
                    raise
        os.utime(done)   # last use: snapshots unused for 3 days are removed below
        for old in pins.iterdir():
            mark = old / '.complete'
            if old != root and mark.is_file() and time.time() - mark.stat().st_mtime > 3 * 86400:
                shutil.rmtree(old, ignore_errors=True)
        self.recomp_commit, self.recomp_root = commit, root
        dirty = git('status', '--porcelain', '--', 'recomp', 'include').decode().strip()
        log(f'   recomp pinned at {commit[:12]} ({rev})'
            + (f'; {len(dirty.splitlines())} uncommitted recomp/ include/ change(s) are not in this build'
               if dirty else ''))
        return commit

    def pin_decomp(self):
        """decomp on, resolved once. The recompiler gets that
        commit's replaced.txt and decomp_totk is compiled from that commit's sources (NXRECOMP_DECOMP_COMMIT ->
        in the next build."""
        if self.decomp_commit:
            return self.decomp_commit
        rev = getattr(self.a, 'decomp_rev', None) or 'HEAD'

        def git(*args):
            p = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True)
            if p.returncode != 0:
                raise BuildError(f"git {' '.join(args)} failed: {p.stderr.decode(errors='replace').strip()[:300]}")
            return p.stdout

        commit = git('rev-parse', '--verify', f'{rev}^{{commit}}').decode().strip()
        listed = self.work / 'build' / f'replaced-pinned{self.suffix}.txt'
        listed.parent.mkdir(parents=True, exist_ok=True)
        data = git('show', f'{commit}:decomp/replaced.txt')
        if not (listed.is_file() and listed.read_bytes() == data):   # unchanged: keep the file (and its mtime)
            tmp = listed.with_name(listed.name + '.tmp')
            tmp.write_bytes(data)
            os.replace(tmp, listed)
        self.decomp_commit, self.decomp_replaced = commit, listed
        dirty = git('status', '--porcelain', '--', 'decomp/replaced.txt', 'decomp/src', 'decomp/include',
                    'decomp/gen', 'decomp/spec').decode().strip()
        log(f'   decomp pinned at {commit[:12]} ({rev})'
            + (f'; {len(dirty.splitlines())} uncommitted decomp/ change(s) are not in this build' if dirty else ''))
        return commit

    def run_steps(self, steps, t_all):
        for name in steps:
            t0 = time.monotonic()
            log(f'== {name}')
            try:
                if name in SHARED_STEPS:   # writes data every tree reads
                    with self.shared_lock:
                        status = getattr(self, 'step_' + name)() or 'done'
                else:
                    status = getattr(self, 'step_' + name)() or 'done'
            except BuildError:
                raise
            except Exception as exc:
                self.results.append(dict(step=name, status='FAILED', seconds=round(time.monotonic() - t0, 1),
                                         error=str(exc)))
                self.summary(time.monotonic() - t_all)
                raise BuildError(f'{name}: {exc}') from exc
            dt = time.monotonic() - t0
            self.results.append(dict(step=name, status=status, seconds=round(dt, 1)))
            log(f'   {name}: {status} ({dt:.1f}s)')
        self.summary(time.monotonic() - t_all)

    def summary(self, total):
        pin = f', decomp @ {self.decomp_commit[:12]}' if self.decomp_commit else ''
        pin += f', recomp @ {self.recomp_commit[:12]}' if self.recomp_commit else ', recomp from the worktree'
        log(f'\ntree {self.tree} (opt {self.opt}{pin}): {self.gen_root}, {self.build_dir}, {self.package_dir}')
        log('step        status                                   seconds')
        for r in self.results:
            log(f"{r['step']:<11} {r['status'][:40]:<40} {r['seconds']:>8.1f}")
        log(f"{'total':<11} {'':<40} {total:>8.1f}")
        write_json(self.work / 'build-report.json', dict(tree=self.tree, decomp_commit=self.decomp_commit,
                                                         recomp_commit=self.recomp_commit,
                                                         steps=self.results,
                                                         total_seconds=round(total, 1),
                                                         finished=time.strftime('%Y-%m-%dT%H:%M:%S')))

    def need(self, what, value):
        if not value:
            raise BuildError(f'{what} is required for this step')
        return value

    def skip_or_force(self, name, inputs):
        return not (self.a.force and (name in self.a.force or 'all' in self.a.force)) and \
            self.manifest.fresh(name, inputs)

    # ---- extraction ------------------------------------------------------------------------------------------------
    def nsp_inputs(self):
        return dict(nsp=file_identity(self.need('--nsp', self.a.nsp)))

    def step_exefs(self):
        inputs = self.nsp_inputs()
        inputs['expected'] = self.expected['exefs_sha256']
        if self.skip_or_force('exefs', inputs):
            return 'up to date'
        exefs = self.ext / 'exefs'
        pinned = {k: v for k, v in self.expected['exefs_sha256'].items() if k != 'main.npdm'}

        def matches():
            return all((exefs / n).is_file() and sha256_file(exefs / n) == h for n, h in pinned.items())

        status = 'adopted existing (hashes match)'
        if not matches():
            self.need('--keys', self.a.keys)
            extract.extract_exefs(self.a.nsp, self.a.keys, self.ext, log=log)
            status = 'extracted'
        require(matches(), 'ExeFS modules do not match the pinned 1.0.0 hashes (not a TotK 1.0.0 base dump?)')
        self.manifest.record('exefs', inputs, [exefs / n for n in pinned] + [exefs / 'main.npdm'])
        return status + f', {len(pinned)} modules pinned'

    def step_romfs(self):
        inputs = self.nsp_inputs()
        inputs['expected'] = self.expected['romfs']
        inputs['unpack'] = not self.a.no_unpack
        if self.skip_or_force('romfs', inputs):
            return 'up to date'
        report_path = self.ext / 'romfs-report.json'
        image = self.ext / 'romfs.bin'
        exp = self.expected['romfs']
        status = 'adopted existing (verified when written)'
        rep = read_json(report_path) if report_path.exists() else None
        adopt = (rep and rep.get('status') == 'pass' and rep['romfs_sha256'] == exp['sha256']
                 and rep['romfs_size'] == exp['size'] and rep['source']['size'] == inputs['nsp']['size']
                 and image.is_file() and image.stat().st_size == exp['size']
                 and (self.a.no_unpack or rep.get('tree') == 'romfs'))
        if not adopt:
            self.need('--keys', self.a.keys)
            # Heavy job: run the extractor as a child of the slot limiter (keys passed as a path only).
            cmd = [sys.executable, '-m', 'pipeline.extract', 'romfs', '--nsp', str(self.a.nsp), '--keys',
                   str(self.a.keys), '--out', str(self.ext), '--expected', str(self.a.expected)]
            if self.a.no_unpack:
                cmd.append('--no-unpack')
            p = subprocess.run(heavy(cmd, log), cwd=REPO)
            if p.returncode != 0:
                raise BuildError(f'RomFS extraction failed (exit {p.returncode})')
            rep = read_json(report_path)
            status = 'extracted + verified'
        self.manifest.record('romfs', inputs, [image, self.ext / 'romfs-index.json', report_path],
                             report=dict(romfs_sha256=rep['romfs_sha256'], files=rep['file_count']))
        return f"{status}: {rep['file_count']} files, sha256 {rep['romfs_sha256'][:16]}… = pinned"

    def step_fonts(self):
        firmware = Path(self.a.firmware) if self.a.firmware else extract.default_firmware()
        report_path = self.ext / 'fonts-report.json'
        inputs = dict(firmware=str(firmware), expected=self.expected['shared_fonts_sha256'])
        if self.skip_or_force('fonts', inputs):
            return 'up to date'
        rep = read_json(report_path) if report_path.exists() else None
        fonts_dir = self.ext / 'fonts'
        adopt = rep and rep.get('status') == 'pass' and len(rep['fonts']) == 6 and all(
            (fonts_dir / f['path']).is_file() and sha256_file(fonts_dir / f['path']) == f['sha256'] for f in rep['fonts'])
        status = 'adopted existing'
        if not adopt:
            require(firmware.is_dir(), f'Firmware directory not found: {firmware} (pass --firmware)')
            self.need('--keys', self.a.keys)
            rep = extract.extract_fonts(firmware, self.a.keys, self.ext, expected=self.expected['shared_fonts_sha256'],
                                        log=log)
            status = 'extracted'
        self.manifest.record('fonts', inputs, [fonts_dir / f['path'] for f in rep['fonts']] + [report_path])
        matched = sum(1 for f in rep['fonts'] if f['match'])
        return f'{status}: 6 fonts, {matched} byte-identical to pinned, {6 - matched} valid BFTTF (other firmware)'

    def step_modules(self):
        exefs = self.ext / 'exefs'
        inputs = dict(nso={n: sha256_file(exefs / n) for n in GUEST_MODULES + ('rtld',)},
                      code=code_identity(REPO / 'pipeline' / 'modules.py', REPO / 'pipeline' / 'layout.py',
                                         REPO / 'pipeline' / 'nx' / 'nso.py', REPO / 'runtime' / 'layout'))
        if self.skip_or_force('modules', inputs):
            return 'up to date'
        res = modules.prepare_modules(self.work, exefs, self.expected['exefs_sha256'], log=log)
        outs = [self.work / 'layout.json', self.work / 'elf' / 'modules.json']
        for n in GUEST_MODULES + ('rtld',):
            outs += [self.work / 'elf' / f'{n}.elf', self.work / 'elf' / f'{n}-inventory.json']
        outs += [self.work / 'images' / f'{n}.img' for n in GUEST_MODULES]
        self.manifest.record('modules', inputs, outs)
        bases = ', '.join(f"{m['name']}@{m['base']}" for m in res['layout']['modules'])
        return f'images + ELFs + layout.json ({bases})'

    def step_ready(self):
        """READY.json: the extracted data other workers poll for, with sizes and hashes."""
        rom = read_json(self.ext / 'romfs-report.json')
        # the fonts come from the firmware, which the Builder makes optional (only Korean / Chinese text needs them)
        fonts = read_json(self.ext / 'fonts-report.json') if (self.ext / 'fonts-report.json').exists() else None
        layout_path = self.work / 'layout.json'
        layout = read_json(layout_path) if layout_path.exists() else None
        ready = dict(
            status='ready', title_id=self.expected['title_id'], version=self.expected['display_version'],
            romfs=dict(tree=str(self.ext / 'romfs') if rom.get('tree') else None, image=str(self.ext / 'romfs.bin'),
                       index=str(self.ext / 'romfs-index.json'), size=rom['romfs_size'], sha256=rom['romfs_sha256'],
                       sha256_matches_pinned=rom['romfs_sha256'] == self.expected['romfs']['sha256'],
                       file_count=rom['file_count'], total_file_bytes=rom['total_file_bytes']),
            fonts=dict(dir=str(self.ext / 'fonts'),
                       files=[dict(type=f['type'], name=f['name'], path=str(self.ext / 'fonts' / f['path']),
                                   bfttf_bytes=f['bfttf_bytes'], sha256=f['sha256'], matches_pinned=f['match'])
                              for f in fonts['fonts']]) if fonts else None,
            exefs=dict(dir=str(self.ext / 'exefs'),
                       files={n: dict(size=(self.ext / 'exefs' / n).stat().st_size,
                                      sha256=sha256_file(self.ext / 'exefs' / n))
                              for n in ('main', 'subsdk0', 'sdk', 'rtld', 'main.npdm')}),
            layout=str(layout_path) if layout else None,
            images={m['name']: dict(path=str(self.work / m['image']), size=m['image_size'], sha256=m['image_sha256'],
                                    base=m['base']) for m in layout['modules']} if layout else None,
            elf={n: str(self.work / 'elf' / f'{n}.elf') for n in GUEST_MODULES + ('rtld',)} if layout else None,
            written=time.strftime('%Y-%m-%dT%H:%M:%S'))
        write_json(self.ext / 'READY.json', ready)
        return f"READY.json written ({self.ext / 'READY.json'})"

    # ---- recompile + native build ----------------------------------------------------------------------------------
    @property
    def patched_main(self):
        image = self.work / 'patched' / 'main.img'
        return image, image.with_name(image.name + '.patches.json')

    def step_patch(self):
        """the pre-recompile code patches on main (refuses anything but the verified 1.0.0 main)."""
        self.patch_ran = True
        if not (REPO / 'games' / 'totk' / 'patches' / '__main__.py').exists():
            if self.a.strict:
                raise BuildError('games/totk/patches (the patch CLI) missing')
            return 'SKIPPED: games.totk.patches not available'
        image, report = self.patched_main
        if self.a.patch_features == 'none':
            for f in (image, report):
                if f.exists():
                    f.unlink()   # recompile then uses the unpatched image
            return 'disabled (--patch-features none): main recompiled unpatched'
        source = self.work / 'images' / 'main.img'
        inputs = dict(image=sha256_file(source), features=self.a.patch_features,
                      code=code_identity(REPO / 'games' / 'totk' / 'patches'))
        if self.skip_or_force('patch', inputs):
            return 'up to date'
        image.parent.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, '-m', 'games.totk.patches', 'apply', '--image', str(source), '--out', str(image),
               '--features', self.a.patch_features]
        log('   ' + ' '.join(cmd))
        p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        tail = (p.stdout.strip().splitlines() or [''])[-1]
        if p.returncode != 0:
            log(p.stdout[-3000:] + p.stderr[-3000:])
            raise BuildError(f'game patches refused/failed (exit {p.returncode}): {tail[:300]}')
        require(image.is_file() and report.is_file(), f'patch step wrote no {image.name} / {report.name}')
        rep = read_json(report)
        self.manifest.record('patch', inputs, [image, report])
        applied = rep.get('features') or rep.get('applied') or []
        names = ', '.join(x if isinstance(x, str) else str(x.get('name', x)) for x in applied) if isinstance(
            applied, list) else ', '.join(applied)
        return f'patched main ({self.a.patch_features}): {names or "see " + report.name}'

    def step_recompile(self):
        self.pin_recomp()
        if not (self.recomp_root / 'recomp' / '__main__.py').exists():
            if self.a.strict:
                raise BuildError('recomp/__main__.py not found (the recompiler CLI is missing)')
            return 'SKIPPED: recomp CLI (recomp/__main__.py) not available yet'
        layout = self.work / 'layout.json'
        # Snapshot the patched main (shared, rewritten by any tree's patch step) into this tree under the shared lock:
        # the recompiler then reads a copy nobody else writes, for its whole run.
        with self.shared_lock:
            patched, patches = self.shared_main_image()
            use_patched = patched is not None
            if use_patched:
                snap = self.gen_root / '_inputs'
                snap.mkdir(parents=True, exist_ok=True)
                for src in (patched, patches):
                    dst = snap / src.name
                    if not (dst.exists() and filecmp.cmp(src, dst, shallow=False)):
                        shutil.copy2(src, dst)
                patched, patches = snap / patched.name, snap / patches.name
            expected = self.generation_inputs(patched, patches)
        self.run_patched = (patched, patches)
        replaced = self.replaced_list()
        forced = bool(self.a.force) and ('recompile' in self.a.force or 'all' in self.a.force)
        # The child runs the pinned recompiler (cwd and first on the path: `-m recomp` resolves there; the rest of the
        # repo after it) and sees no recomp/simd pin of its own: CMake compiles against the same snapshot's headers.
        env = {k: v for k, v in os.environ.items() if k != 'NXRECOMP_SIMD_ROOT'}
        env['PYTHONPATH'] = os.pathsep.join([str(self.recomp_root), str(REPO)]
                                            + ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
        overrides = REPO / 'runtime' / 'config' / 'overrides.txt'   # the snapshot has no runtime/: pass it
        done, results = [], []
        for m in GUEST_MODULES:
            inputs = expected[m]
            fingerprint = fingerprint_of(inputs)
            # Keyed by tree: a stamp answers only for this tree's generated/<m>.
            name = f'recompile:{self.tree}:{m}'
            out = self.gen_root / m
            if not forced and self.manifest.fresh(name, inputs) and self.generation_problem(m, inputs) is None:
                results.append(f'{m} up to date')
                continue
            # Generate into a staging dir, then move over only the files whose bytes changed: unchanged shards keep
            # their mtime, so Ninja recompiles exactly what the recompiler changed. generation.json goes first and comes
            # back last: an interrupted run leaves a module that matches no configuration.
            marker = out / GENERATION
            if marker.exists():
                marker.unlink()
            stage = self.work / f'{self.gen_root.name}-staging' / m
            if stage.exists():
                shutil.rmtree(stage)
            wait_for_ram(log=log)
            cmd = [sys.executable, '-m', 'recomp', '--layout', str(layout), '--module', m, '--out', str(stage),
                   '--jobs', str(self.a.jobs), '--replaced', str(replaced), '--decomp', self.decomp,
                   '--opt', str(self.opt)]
            if overrides.exists():
                cmd += ['--overrides', str(overrides)]
            if m == 'main' and use_patched:
                cmd += ['--image', str(patched), '--patches', str(patches)]
            log('   ' + ' '.join(cmd))
            t0 = time.monotonic()
            p = subprocess.run(self.recompiler_cmd(cmd), cwd=self.recomp_root, env=env)
            if p.returncode != 0:
                raise BuildError(f'recompiler failed on {m} (exit {p.returncode})')
            t_gen = time.monotonic() - t0
            with open(stage / f'nx_generation_{m}.c', 'w', newline='\n', encoding='utf-8') as f:
                f.write(generation_source(m, fingerprint, self.tree))
            counts = sync_tree(stage, out)
            shutil.rmtree(stage)
            write_json(marker, dict(format=GENERATION_FORMAT, module=m, fingerprint=fingerprint, inputs=inputs,
                                    decomp_commit=self.decomp_commit,   # informational: `replaced` is the input
                                    recomp_commit=self.recomp_commit,   # informational: `code` and `simd` are
                                    sources=sources_digest(out), written=time.strftime('%Y-%m-%dT%H:%M:%S')))
            changed_c = [f for f in counts['changed_files'] if f.endswith(('.c', '.h'))]
            self.changed_sources[m] = changed_c
            if counts['added'] or counts['removed']:
                self.shard_set_changed = True   # the guest glob must be re-run: configure
            log(f"   {m}: generated in {t_gen:.0f}s, synced in {time.monotonic() - t0 - t_gen:.0f}s; {counts['same']} files unchanged, {counts['changed']} changed, "
                f"{counts['added']} added, {counts['removed']} removed ({len(changed_c)} C/H): "
                f"{', '.join(changed_c[:8])}{' ...' if len(changed_c) > 8 else ''}")
            self.manifest.record(name, inputs, [f for f in (out / f'dispatch_{m}.c', out / 'report.json',
                                                            out / f'nx_generation_{m}.c', marker) if f.exists()])
            done.append(m)
            results.append(f'{m} {len(changed_c)}/{len(tree_files(out, "*.c"))} sources changed')
        return ('recompiled ' if done else '') + ('(main patched' if use_patched else '(main UNPATCHED') + \
            f', decomp {self.decomp}, opt {self.opt}) ' + '; '.join(results)

    def recompiler_cmd(self, cmd):
        """The recompiler command as run (tests substitute a stand-in)."""
        return cmd

    @property
    def tree(self):
        """This build tree's name: variant, -pgogen / -pgo for the PGO builds
        linked."""
        return self.a.variant + PGO_TAG[self.a.pgo] + ('-decomp' if self.decomp == 'on' else '')

    def shared_main_image(self):
        """(patched main image, its .patches.json) when the patch step produced both, else (None, None)."""
        patched, patches = self.patched_main
        return (patched, patches) if patched.is_file() and patches.is_file() else (None, None)

    def replaced_list(self):
        """decomp on, compiled from the same commit, defines
        those bodies); off: an empty list, so the recompiler emits every body."""
        if self.decomp == 'on':
            self.pin_decomp()
            return self.decomp_replaced
        replaced = self.work / 'build' / 'replaced-none.txt'
        replaced.parent.mkdir(parents=True, exist_ok=True)
        if not replaced.exists():
            replaced.write_text('# empty: --decomp off, the recompiler emits every body\n', encoding='utf-8')
        return replaced

    def generation_inputs(self, patched, patches):
        """{module: generation inputs}: everything this tree's generated code depends on. The tree, variant, decomp
        setting and codegen level; the recompiler's code (recomp/, incl. simd + config; recomp/simd also on its own:
        it is what CMake compiles the shards against), the guest contract, the runtime's override list, the replaced
        list, the layout (which pins every module image by hash) and, for main, the patched image."""
        overrides = REPO / 'runtime' / 'config' / 'overrides.txt'
        replaced = self.replaced_list()
        src = self.recomp_root   # the pinned snapshot (pin_recomp), or the worktree with --recomp-rev worktree
        common = dict(format=GENERATION_FORMAT, tree=self.tree, variant=self.a.variant, decomp=self.decomp,
                      opt=self.opt, simd=code_identity(src / 'recomp' / 'simd', base=src),
                      layout=sha256_file(self.work / 'layout.json'),
                      code=code_identity(src / 'recomp', src / 'include' / 'guest', base=src),
                      overrides=sha256_file(overrides) if overrides.exists() else None,
                      replaced=sha256_file(replaced) if replaced.exists() else None,
                      codegen=os.environ.get('NXRECOMP_CODEGEN'))   # recomp/features.py switches (None: default)
        inputs = {}
        for m in GUEST_MODULES:
            inputs[m] = dict(common, module=m)
            if m == 'main':
                inputs[m].update(patched_main=sha256_file(patched) if patched else None,
                                 patches=sha256_file(patches) if patches else None)
        return inputs

    def expected_generation(self):
        """generation_inputs this tree's generated code must match, computed now. recomp/simd and recomp +
        include/guest come from this run's pinned snapshot (pin_recomp: what CMake compiles the shards against, and
        nothing edits it); overrides, layout and replaced are live, so an edit to those during the build fails the
        check. Only the patched image is frozen to the copy the code was generated
        from: another tree's patch step may rewrite the shared one at any time. Without a
        recompile in this run: the tree's own snapshot, unless this run's patch step ran (the shared image is then
        what a recompile must use, and a snapshot from older patches is not this tree's code any more)."""
        if self.run_patched is not None:
            return self.generation_inputs(*self.run_patched)
        shared, shared_patches = self.patched_main
        snap = self.gen_root / '_inputs'
        if not self.patch_ran and (snap / shared.name).is_file() and (snap / shared_patches.name).is_file():
            return self.generation_inputs(snap / shared.name, snap / shared_patches.name)
        with self.shared_lock:
            return self.generation_inputs(*self.shared_main_image())

    def generation(self, m):
        marker = self.gen_root / m / GENERATION
        try:
            return read_json(marker) if marker.is_file() else None
        except ValueError:
            return None

    def generation_problem(self, m, inputs):
        """None when <gen_root>/<m> is a complete generation for `inputs`; otherwise what is wrong with it."""
        out = self.gen_root / m
        gen = self.generation(m)
        if gen is None:
            return f'{out} has no {GENERATION} (not generated for this tree yet, or the recompile was interrupted)'
        if gen.get('fingerprint') != fingerprint_of(inputs):
            had = gen.get('inputs') or {}
            diff = sorted(k for k in set(inputs) | set(had) if inputs.get(k) != had.get(k))
            return f'{out} was generated for another configuration (differs in: {", ".join(diff) or "format"})'
        if gen.get('sources') != sources_digest(out):
            return f'{out} changed after it was generated (C/H files added, removed or resized by another writer)'
        root = out / 'simd_root.txt'
        if root.is_file() and root.read_text(encoding='utf-8').strip():
            return f'{out} was emitted against a pinned recomp/simd snapshot; CMake compiles against recomp/simd'
        return None

    def check_generated(self, modules):
        """{module: fingerprint} of this tree; BuildError unless every module's generated code is this tree's."""
        expected = self.expected_generation()
        problems = [f'{m}: {p}' for m in modules for p in [self.generation_problem(m, expected[m])] if p]
        if problems:
            raise BuildError(f'generated guest code is not this tree\'s ({self.tree}, opt {self.opt}); run the '
                             f'recompile step (--quick does): ' + '; '.join(problems))
        return {m: fingerprint_of(expected[m]) for m in modules}

    def verify_guest_libs(self, fingerprints):
        """BuildError unless every guest_<m>.lib of this build dir carries exactly this tree's fingerprint."""
        problems = []
        for m, fp in fingerprints.items():
            lib = self.build_dir / 'lib' / f'guest_{m}.lib'
            if not lib.is_file():
                problems.append(f'{m}: {lib} missing (module not configured or not built)')
                continue
            found = lib_fingerprints(lib, m)
            if found != {fp}:
                has = ', '.join(sorted(x[:12] for x in found)) or 'no generation fingerprint'
                problems.append(f'{m}: {lib.name} carries {has}, this tree ({self.tree}) is {fp[:12]}')
        if problems:
            raise BuildError('guest libraries were not generated for this tree; not linked: ' + '; '.join(problems))

    def native(self):
        if not hasattr(self, '_native'):
            from pipeline.toolchain import native_env
            self._native = native_env(cache=self.work / 'build' / 'msvc-env.json')
        return self._native

    @property
    def build_dir(self):
        """One CMake build dir per variant and decomp setting: dev without decomp keeps <work>/build/cmake."""
        return self.work / 'build' / ('cmake' + self.suffix)

    @property
    def suffix(self):
        return (('' if self.a.variant == 'dev' else f'-{self.a.variant}') + PGO_TAG[self.a.pgo]
                + ('-decomp' if self.decomp == 'on' else ''))

    def variant_cache(self):
        pgo_data = ''
        if self.a.pgo == 'use':
            pgo_data = Path(self.a.pgo_data or self.work / 'pgo' / 'totk.profdata').resolve().as_posix()
        return {'NXRECOMP_VARIANT': self.a.variant, 'NXRECOMP_GUEST_ARCH': self.a.cpu,
                'NXRECOMP_LTO': 'ON' if self.a.lto == 'thin' else 'OFF', 'NXRECOMP_LTO_JOBS': str(self.a.jobs),
                'NXRECOMP_PGO': self.a.pgo, 'NXRECOMP_PGO_DATA': pgo_data,
                'NXRECOMP_PREEMPT_POLL': 'ON',   # guest preemption poll (cmake/NxGuestModule.cmake, runtime.h; C4 fixed)
                'NXRECOMP_WITH_DECOMP': 'ON' if self.decomp == 'on' else 'OFF',
                'NXRECOMP_DECOMP_COMMIT': self.pin_decomp() if self.decomp == 'on' else '',
                'NXRECOMP_RECOMP_ROOT': self.pinned_recomp_root().as_posix(),
                'NXRECOMP_GENERATED_DIR': self.gen_root.as_posix()}

    def pinned_recomp_root(self):
        self.pin_recomp()
        return Path(self.recomp_root).resolve()

    def configured_as_wanted(self):
        cache = self.build_dir / 'CMakeCache.txt'
        if not (self.build_dir / 'build.ninja').exists() or not cache.exists():
            return False
        text = cache.read_text(encoding='utf-8', errors='replace')
        return all(f'\n{k}:' in text and any(line.split('=', 1)[1] == v for line in text.splitlines()
                                             if line.startswith(k + ':'))
                   for k, v in self.variant_cache().items())

    def stale_globs(self):
        """Guest modules whose shard set on disk differs from what the last configure globbed (another writer such as
        a manual recompiler run added or removed shards); a missing stamp counts as stale."""
        stale = []
        for m in GUEST_MODULES:
            stamp = self.build_dir / f'guest_sources_{m}.txt'
            on_disk = {p.name for p in (self.gen_root / m).glob('*.c')}
            seen = set(stamp.read_text(encoding='utf-8').split()) if stamp.exists() else None
            if on_disk and seen != on_disk:
                stale.append(m)
        return stale

    def step_configure(self):
        if self.a.quick and self.configured_as_wanted() and not self.shard_set_changed and not self.stale_globs():
            # Ninja re-runs CMake by itself when a CMakeLists.txt / cmake file changes; the guest globs only change
            # when the recompiler adds or removes shards (then shard_set_changed forces this configure).
            return 'skipped (quick: build dir configured for this variant, shard set unchanged)'
        return self.configure()

    def configure(self):
        env, tools = self.native()
        vcpkg = self.work / 'vcpkg' / 'scripts' / 'buildsystems' / 'vcpkg.cmake'
        cmd = [tools['cmake'], '-S', str(REPO), '-B', str(self.build_dir), '-G', 'Ninja',
               f'-DCMAKE_MAKE_PROGRAM={tools["ninja"]}', '-DCMAKE_BUILD_TYPE=Release',
               f'-DNXRECOMP_WORK={self.work.as_posix()}',
               f'-DNXRECOMP_LLVM_DIR={Path(tools["clang-cl"]).parent.parent.as_posix()}']
        cmd += [f'-D{k}={v}' for k, v in self.variant_cache().items()]
        if vcpkg.exists():
            cmd += [f'-DCMAKE_TOOLCHAIN_FILE={vcpkg.as_posix()}',
                    f'-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={(REPO / "cmake" / "clang-cl.cmake").as_posix()}',
                    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md', '-DVCPKG_MANIFEST_MODE=OFF']
        else:
            cmd += [f'-DCMAKE_TOOLCHAIN_FILE={(REPO / "cmake" / "clang-cl.cmake").as_posix()}']
        # A broken area (typically: its vcpkg packages are not installed yet) must not stop the others: drop the
        # area named in the first CMake error and configure again; report what was dropped.
        areas = list(AREAS)
        dropped = []
        (self.work / 'build').mkdir(exist_ok=True)
        log_path = self.work / 'build' / f'configure{self.suffix}.log'
        while True:
            p = subprocess.run(cmd + ['-DNXRECOMP_AREAS=' + ';'.join(areas)], env=env, capture_output=True, text=True)
            log_path.write_text(p.stdout + p.stderr, encoding='utf-8')
            if p.returncode == 0:
                break
            culprit = failing_area(p.stdout + p.stderr, areas)
            if culprit is None or self.a.strict:
                log(p.stdout[-4000:] + p.stderr[-4000:])
                raise BuildError(f'CMake configure failed (log: {log_path})')
            reason = first_cmake_error(p.stdout + p.stderr)
            log(f'   area {culprit} fails to configure, building without it: {reason}')
            dropped.append(f'{culprit} ({reason})')
            areas.remove(culprit)
            (self.work / 'build' / f'configure{self.suffix}-{culprit.replace("/", "_")}.log').write_text(
                p.stdout + p.stderr, encoding='utf-8')
        for line in p.stdout.splitlines():
            if 'nxrecomp:' in line:
                log('   ' + line.strip())
        self.dropped_areas = dropped
        return 'configured' + (f'; DROPPED {"; ".join(dropped)}' if dropped else '')

    def step_build(self):
        """Build; the guest shard lists are re-checked before (and once after a failed build), because another writer
        may add or remove shards in <work>/generated while we wait for a slot or compile."""
        for attempt in (1, 2):
            if self.replaced_changed():
                # Both come from the commit pinned for this run, so this recompiles at most once.
                log(f'   the generated code was not produced from replaced.txt at {self.decomp_commit[:12]}: '
                    'recompiling first')
                log('   recompile: ' + self.step_recompile())
            if not self.configured_as_wanted():
                # e.g. --steps build after decomp commits: decomp_totk must be configured for the pinned commit
                log('   build dir not configured for this tree / pinned decomp commit: re-configuring')
                self.configure()
            stale = self.stale_globs()
            if stale:
                log(f"   shard set of {', '.join(stale)} changed since configure: re-configuring")
                self.configure()
            started = time.time()
            try:
                return self.build_once()
            except BuildError as exc:
                if attempt == 1 and (self.stale_globs() or self.generated_since(started) or self.replaced_changed()):
                    log('   inputs changed during the build (generated sources or decomp list): building again')
                    continue
                if self.links_exe():
                    self.withdraw_package(str(exc))
                raise

    def build_targets(self):
        return self.a.targets or (['TotK'] if self.a.quick else [])

    def links_exe(self):
        targets = self.build_targets()
        return not targets or 'TotK' in targets

    def withdraw_package(self, why):
        """A failed or refused TotK build leaves no TotK.exe in this tree's package, so a boot loop that ignores
        build.py's exit code cannot run the previous exe as if it were this build. The build dir keeps its
        own TotK.exe; packaging again restores it."""
        gone = []
        for name in ('TotK.exe', 'TotK.pdb'):
            f = self.package_dir / name
            if f.exists():
                try:
                    f.unlink()
                    gone.append(name)
                except PermissionError:
                    log(f'   {f} is in use (TotK running?): NOT withdrawn')
        if gone:
            log(f"   withdrew {', '.join(gone)} from {self.package_dir}: {why[:200]}")

    @property
    def package_dir(self):
        return (self.a.out or self.work / 'out').resolve() / ('TotK' + self.suffix)

    def replaced_changed(self):
        """decomp on: the pinned replaced.txt differs from the list the generated tree was produced with."""
        if self.decomp != 'on':
            return False
        current = sha256_file(self.replaced_list())
        used = {((self.generation(m) or {}).get('inputs') or {}).get('replaced') for m in GUEST_MODULES}
        return used != {current}

    def generated_since(self, t):
        """True when a generated C/H file was written after time `t` (someone else wrote into the tree)."""
        return any(p.stat().st_mtime > t for m in GUEST_MODULES for pat in ('*.c', '*.h')
                   for p in (self.gen_root / m).glob(pat))

    def build_once(self):
        """Guest libraries first, checked against this tree's generation fingerprints (generated code before the
        compile, the libraries' symbols after it), then the rest: TotK.exe never links another tree's guest code."""
        env, tools = self.native()
        targets = self.build_targets()
        links = self.links_exe()
        modules_ = list(GUEST_MODULES) if links else [m for m in GUEST_MODULES if f'guest_{m}' in targets]
        log_path = self.work / 'build' / f'build{self.suffix}.log'
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text('', encoding='utf-8')
        t_build = time.time()
        self.guest_fingerprints = None
        fingerprints = None
        if modules_:
            fingerprints = self.check_generated(modules_)
            self.ninja(env, tools, [f'guest_{m}' for m in modules_], log_path)
            self.verify_guest_libs(fingerprints)
            log(f"   guest libraries verified ({self.tree}): "
                + ', '.join(f'{m} {fp[:12]}' for m, fp in fingerprints.items()))
        if not targets or any(t not in [f'guest_{m}' for m in modules_] for t in targets):
            self.ninja(env, tools, targets, log_path)
        if fingerprints:
            # Another writer may have touched the generated tree while TotK linked: check what was linked.
            self.check_generated(list(fingerprints))
            self.verify_guest_libs(fingerprints)
            self.guest_fingerprints = fingerprints
        text = log_path.read_text(encoding='utf-8', errors='replace')
        libs = sorted(x.name for x in (self.build_dir / 'lib').glob('*.lib')) if (self.build_dir / 'lib').exists() else []
        exe = self.build_dir / 'bin' / 'TotK.exe'
        built = f"{len(libs)} libraries{', TotK.exe' if exe.exists() else ', no TotK.exe'}"
        if fingerprints:
            built += f', guest fingerprints verified ({len(fingerprints)})'
        timing = ''
        try:
            from pipeline.ninjalog import summary
            s = summary(self.build_dir, since=t_build)
            if 'Re-running CMake' in text:
                timing += 'cmake re-run, '
            timing += f"{s['objects']} objects compiled"
            for k in ('guest_main.lib', 'TotK.exe'):
                if k in s:
                    timing += f", {k} {s[k]:.1f}s"
            timing += f", ninja wall {s['wall']:.1f}s"
        except (OSError, ValueError, IndexError):
            pass
        return f'built {built} ({timing})' if timing else 'built ' + built

    def ninja(self, env, tools, targets, log_path):
        """cmake --build `targets` (all when empty), output appended to `log_path`; BuildError when it fails."""
        guest = not targets or any(t.startswith('guest_') or t == 'TotK' for t in targets)
        # How much will Ninja compile? A handful of objects is a small compile.
        pending = self.pending_objects(env, tools, targets)
        heavy_job = guest and (pending is None or pending > LIGHT_OBJECTS)
        log(f"   {' '.join(targets) or 'all'}: {pending if pending is not None else '?'} objects to compile"
            f"{' (heavy: tools/heavy.py)' if heavy_job else ''}")
        cmd = [tools['cmake'], '--build', str(self.build_dir), '--parallel', str(self.a.jobs)]
        if targets:
            cmd += ['--target', *targets]
        if not self.a.stop_on_error:
            cmd += ['--', '-k', '0']   # build every library that can be built; report the rest
        start = log_path.stat().st_size if log_path.exists() else 0
        with open(log_path, 'a', encoding='utf-8', newline='\n') as f:
            p = subprocess.run(heavy(cmd, log) if heavy_job else cmd, env=env, stdout=f, stderr=subprocess.STDOUT,
                               text=True)
        if p.returncode != 0:
            with open(log_path, 'rb') as f:
                f.seek(start)
                failed = summarize_failures(f.read().decode('utf-8', errors='replace'))
            for target, (count, first) in failed.items():
                log(f'   FAILED {target}: {count} object(s); first error: {first}')
            # the next run truncates log_path: keep this one (the newest 5 per tree) so the errors can be read later
            kept = log_path.with_name(f'{log_path.stem}-failed-{time.strftime("%Y%m%d-%H%M%S")}.log')
            try:
                shutil.copyfile(log_path, kept)
                for old in sorted(log_path.parent.glob(f'{log_path.stem}-failed-*.log'))[:-5]:
                    old.unlink()
            except OSError:
                kept = log_path
            raise BuildError(f"building {' '.join(targets) or 'all'} failed; failing targets: "
                             f"{', '.join(f'{t} ({c})' for t, (c, _) in failed.items()) or 'see log'} (log: {kept})")

    def pending_objects(self, env, tools, targets):
        """Objects Ninja would compile for `targets` (dry run), or None when the build dir is not configured."""
        if not (self.build_dir / 'build.ninja').exists():
            return None
        # Bring build.ninja up to date for real first (re-runs CMake only when a CMakeLists changed); a dry run
        # cannot see past a regeneration.
        subprocess.run([tools['ninja'], '-C', str(self.build_dir), 'build.ninja'], env=env, capture_output=True,
                       text=True)
        p = subprocess.run([tools['ninja'], '-C', str(self.build_dir), '-n', *(targets or [])], env=env,
                           capture_output=True, text=True)
        if p.returncode != 0:
            return None
        if 'Re-running CMake' in p.stdout:
            # A CONFIGURE_DEPENDS glob elsewhere (runtime/) makes every dry run stop at the CMake re-run: size the
            # guest part ourselves, which is what decides whether this is a heavy job.
            return self.stale_guest_objects()
        return sum(1 for line in p.stdout.splitlines() if 'Building C' in line and '.obj' in line)

    def stale_guest_objects(self):
        """Guest objects Ninja will rebuild: source newer than its object (or no object); every shard of a module
        when a header the precompiled prelude pulls in is newer than the module's PCH object."""
        headers = [p for d in (REPO / 'include' / 'guest', REPO / 'recomp' / 'simd') if d.exists()
                   for p in d.rglob('*.h')]
        newest_shared = max((p.stat().st_mtime for p in headers), default=0)
        total = 0
        for m in GUEST_MODULES:
            gen = self.gen_root / m
            sources = sorted(gen.glob('*.c'))
            objdir = self.build_dir / 'CMakeFiles' / f'guest_{m}.dir'
            pch = objdir / 'cmake_pch.c.obj'
            prelude = gen / 'recomp_prelude.h'
            newest = max(newest_shared, prelude.stat().st_mtime if prelude.exists() else 0)
            if not pch.exists() or pch.stat().st_mtime < newest:
                total += len(sources)
                continue
            mangled = objdir / gen.drive.replace(':', '_') / gen.relative_to(gen.anchor)
            for src in sources:
                obj = mangled / (src.name + '.obj')
                if not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime:
                    total += 1
        return total

    def step_package(self):
        out = self.package_dir
        out.mkdir(parents=True, exist_ok=True)
        exe = self.build_dir / 'bin' / 'TotK.exe'
        copied = []
        fingerprints = self.guest_fingerprints
        if exe.exists() and fingerprints is None:
            # No TotK build verified in this run (--steps package): check what the exe was linked from.
            try:
                fingerprints = self.check_generated(GUEST_MODULES)
                self.verify_guest_libs(fingerprints)
                newest = max((self.build_dir / 'lib' / f'guest_{m}.lib').stat().st_mtime for m in GUEST_MODULES)
                if exe.stat().st_mtime < newest:
                    raise BuildError(f'{exe} is older than the guest libraries: build TotK first')
            except BuildError as exc:
                self.withdraw_package(str(exc))
                raise
        if exe.exists():
            for f in (exe, exe.with_suffix('.pdb')):
                if f.exists():
                    copied.append(place(f, out / f.name))
        fonts_out = out / 'fonts'
        fonts_out.mkdir(exist_ok=True)
        fonts = read_json(self.ext / 'fonts-report.json')['fonts']
        for f in fonts:
            shutil.copy2(self.ext / 'fonts' / f['path'], fonts_out / f['path'])
        data = dict(
            format=1, title_id=self.expected['title_id'], version=self.expected['display_version'],
            tree=self.tree, opt=self.opt, guest=fingerprints if exe.exists() else None,
            decomp_commit=self.decomp_commit, recomp_commit=self.recomp_commit, cpu=self.a.cpu, lto=self.a.lto, pgo=self.a.pgo,
            work=str(self.work), layout=str(self.work / 'layout.json'),
            romfs=dict(tree=str(self.ext / 'romfs'), image=str(self.ext / 'romfs.bin'),
                       index=str(self.ext / 'romfs-index.json')),
            fonts=[dict(type=f['type'], name=f['name'], path=f'fonts/{f["path"]}') for f in fonts],
            images={m: str(self.work / 'images' / f'{m}.img') for m in GUEST_MODULES},
            exefs=str(self.ext / 'exefs'),
            overlays=str(self.work / 'overlays'), saves=str(self.work / 'saves'),
            shader_cache=str(self.work / 'shader-cache'))
        # Shaders: the translator is copied next to the exe (<exe>\xlate\, the default); the 1.7 GB seed is
        # referenced by path, never copied: a config key when app/ has one, else a directory junction at the default
        # location <exe>\shader-seed.
        shaders = self.package_shaders(out)
        data['shader_seed'] = shaders['seed']
        data['translator'] = shaders['translator']
        write_json(out / 'nxrecomp-data.json', data)
        # app/ (TotK.exe) reads settings from TOML: point it at this work folder. Forward slashes need no escaping.
        toml = ('# Written by pipeline/build.py (package step). Settings reference: TotK.exe --help\n'
                '[paths]\n'
                f'work = "{self.work.as_posix()}"\n'
                f'romfs = "{(self.ext / "romfs").as_posix()}"\n')
        for key, value in shaders['config'].items():
            toml += f'{key} = "{Path(value).as_posix()}"\n'
        with open(out / 'config.toml', 'w', newline='\n', encoding='utf-8') as f:
            f.write(toml)
        with open(out / 'TotK.cmd', 'w', newline='\r\n', encoding='utf-8') as f:
            f.write('@echo off\n"%~dp0TotK.exe" --config "%~dp0config.toml" %*\n')
        return (f"{out}: {', '.join(copied) or 'no exe yet'}, config.toml + TotK.cmd, nxrecomp-data.json, "
                f"{len(fonts)} fonts, translator {shaders['translator_state']}, seed {shaders['seed_state']}")

    def package_shaders(self, out):
        """Translator into <out>/xlate/ (copied when changed); seed by path. Returns paths, states and the
        config.toml keys to write (only keys app/ knows: an unknown key is a start-up error there)."""
        # Which settings does the exe being packaged accept? Ask the binary, not app/'s source: a key added to the
        # source but not yet compiled in would make this TotK.exe refuse to start.
        exe = out / 'TotK.exe'
        known = ''
        if exe.exists():
            import mmap
            with open(exe, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
                known = ' '.join(f'"{k}"' for k in ('paths.shader_seed', 'paths.translator')
                                 if mm.find(k.encode() + b'\0') >= 0)
        res = dict(config={}, seed=None, translator=None, seed_state='missing', translator_state='missing')
        publish = self.work / 'build' / 'xlate' / 'publish'
        if not (publish / 'nxrecomp-xlate.exe').exists():
            publish = self.work / 'tools' / 'xlate'
        if (publish / 'nxrecomp-xlate.exe').exists():
            dest = out / 'xlate'
            dest.mkdir(exist_ok=True)
            copied = 0
            for f in publish.iterdir():
                if f.is_file() and f.suffix.lower() in ('.exe', '.dll', '.pdb', '.json'):
                    target = dest / f.name
                    if not target.exists() or target.stat().st_size != f.stat().st_size or \
                            target.stat().st_mtime < f.stat().st_mtime:
                        shutil.copy2(f, target)
                        copied += 1
            res['translator'] = str(dest / 'nxrecomp-xlate.exe')
            res['translator_state'] = f'{dest} ({copied} files updated from {publish})'
            if '"paths.translator"' in known:
                res['config']['translator'] = res['translator']
        seed, why = pick_seed(self.work)
        if (seed / 'seed-manifest.json').exists():
            res['seed'] = str(seed)
            if '"paths.shader_seed"' in known:
                res['config']['shader_seed'] = str(seed)
                res['seed_state'] = f'{seed} (config key paths.shader_seed; {why})'
            else:
                res['seed_state'] = f'{link_dir(out / "shader-seed", seed)} ({why})'
        return res


SEED_IDENTITY = ('schema', 'opts', 'options', 'translator_sha256', 'translator_build', 'retranslated')


def pick_seed(work):
    """The shader seed to package: shader-seed.opt (tools/xlate/optimize_seed.py: the same variants with optimised
    SPIR-V, a tenth of the size) when it was made from the current shader-seed, else shader-seed itself. Made from it:
    the same translator identity in both manifests, and written after the raw seed last changed (a re-seed or a
    re-translation rewrites seed-manifest.json and variants.jsonl)."""
    raw, opt = work / 'shader-seed', work / 'shader-seed.opt'
    rm, om = raw / 'seed-manifest.json', opt / 'seed-manifest.json'
    if not om.is_file():
        return raw, 'raw seed: no shader-seed.opt'
    if not rm.is_file():
        return opt, 'optimised seed (no raw seed to compare)'
    try:
        a, b = json.loads(rm.read_text(encoding='utf-8')), json.loads(om.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        return raw, f'raw seed: manifest unreadable ({e})'
    if 'optimisation' not in b or any(a.get(k) != b.get(k) for k in SEED_IDENTITY):
        return raw, 'raw seed: shader-seed.opt is from another translation (re-run tools/xlate/optimize_seed.py)'
    newest = max(rm.stat().st_mtime, (raw / 'variants.jsonl').stat().st_mtime if (raw / 'variants.jsonl').is_file() else 0)
    if om.stat().st_mtime < newest:
        return raw, 'raw seed: shader-seed.opt is older than shader-seed (re-run tools/xlate/optimize_seed.py)'
    return opt, 'optimised seed'


def parse(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--nsp', type=Path, help='base game 1.0.0 (.nsp or .xci)')
    p.add_argument('--keys', type=Path, help='prod.keys (path only; never printed)')
    p.add_argument('--firmware', type=Path, help='firmware NCA dir (default: %%APPDATA%%/Ryujinx/bis/system/Contents/registered)')
    p.add_argument('--work', type=Path, default=DEFAULT_WORK)
    p.add_argument('-o', '--out', type=Path, help='output folder (default: <work>/out); TotK/ is created inside')
    p.add_argument('--expected', type=Path, default=REPO / 'pipeline' / 'expected' / 'totk-1.0.0.json')
    p.add_argument('--steps', default='all', help=f'comma list or "all": {",".join(STEPS)}; "from:<step>" also works')
    p.add_argument('--force', type=lambda s: s.split(','), default=[], help='steps to redo even when up to date')
    p.add_argument('--no-unpack', action='store_true', help='keep only romfs.bin + index (no unpacked tree)')
    p.add_argument('--jobs', type=int, default=6, help='parallel compiles (shared machine: default 6)')
    p.add_argument('--patch-features', default='default',
                   help='the patch feature set passed to `games.totk.patches apply --features` ("none" = unpatched)')
    p.add_argument('--targets', type=lambda s: [t for t in s.split(',') if t], default=[],
                   help='build only these CMake targets (e.g. runtime,gfx_core); default: everything')
    p.add_argument('--stop-on-error', action='store_true', help='stop at the first compile error (default: build '
                   'everything that can be built, then report failures per target)')
    p.add_argument('--strict', action='store_true', help='fail when an optional stage (recompiler) is missing')
    p.add_argument('--quick', action='store_true',
                   help='boot loop: patch, recompile only modules whose inputs changed (unchanged shards keep their '
                        'mtime), no explicit configure, build TotK only, package')
    p.add_argument('--variant', choices=('dev', 'release', 'profile'), default='dev',
                   help='dev: fast relink; release: /OPT:REF,ICF and ThinLTO by default; profile: /Z7 + /DEBUG:FULL '
                        '(TotK.pdb for nxprof). Separate build dir and <out>/TotK-<variant>')
    p.add_argument('--cpu', default='x86-64-v3',
                   help='-march for everything (guest and host): x86-64-v3 = portable (the minimum recomp/simd '
                        'accepts, the default until native is verified on a boot), native = this PC (znver5 on a Zen 5), or '
                        'any clang -march value')
    p.add_argument('--lto', nargs='?', const='thin', choices=('off', 'thin'), default=None,
                   help='ThinLTO for everything (guest functions inline across shards; lld runs --jobs backends at '
                        'link, incremental cache in the build dir); default: thin for release, off otherwise')
    p.add_argument('--pgo', choices=('off', 'gen', 'use'), default='off',
                   help='gen: instrumented build (tree -pgogen; the exe writes pgo-raw/*.profraw next to itself), '
                        'then `python -m pipeline.pgo merge`; use: optimised with --pgo-data (tree -pgo)')
    p.add_argument('--pgo-data', type=Path, default=None,
                   help='--pgo use: the merged profile (default <work>/pgo/totk.profdata)')
    p.add_argument('--decomp-rev', default='HEAD',
                   help='decomp on: the commit decomp/ is taken from (replaced.txt for the recompiler, sources for '
                        'decomp_totk), resolved once at the start of the run; default HEAD. Uncommitted decomp/ '
                        'changes are never built')
    p.add_argument('--recomp-rev', default='HEAD',
                   help='the commit recomp/ and include/ are taken from (the recompiler, and the guest headers and '
                        'CFLAGS CMake compiles the shards with), resolved once at the start of the run; default HEAD, '
                        'so uncommitted recomp/ changes are not built. "worktree": the working tree as it is')
    p.add_argument('--opt', type=int, choices=(0, 1), default=None,
                   help='recompiler codegen level (default NXRECOMP_OPT, else 1); part of the generation '
                        'fingerprint, so changing it regenerates the tree')
    p.add_argument('--decomp', choices=('on', 'off', 'both'),
                   help='decompiled functions (decomp/replaced.txt -> decomp_totk): off = the recompiler emits every '
                        'body, decomp_totk not linked; on = those bodies come from decomp_totk; both = build off then '
                        'on in one run (A/B). on/off are remembered in the build manifest; default on. on uses '
                        '<work>/generated-decomp, '
                        'build/cmake[-variant]-decomp and out/TotK[-variant]-decomp')
    a = p.parse_args(argv)
    if a.lto is None:
        a.lto = 'thin' if a.variant == 'release' else 'off'
    if a.quick:
        if a.steps != 'all':
            p.error('--quick picks its own steps')
        return a, list(QUICK_STEPS)
    if a.steps == 'all':
        steps = list(STEPS)
    elif a.steps.startswith('from:'):
        steps = list(STEPS[STEPS.index(a.steps[5:]):])
    else:
        steps = [s for s in a.steps.split(',') if s]
        bad = [s for s in steps if s not in STEPS]
        if bad:
            p.error(f'unknown steps: {bad}')
    return a, steps


def main(argv=None):
    a, steps = parse(argv)
    try:
        if a.decomp == 'both':
            # A/B pair in one invocation, each tree under its own lock; the remembered setting (what a plain run
            # builds) is left as it was.
            import copy
            manifest = Manifest(a.work.resolve())
            remembered = manifest.setting('decomp')
            try:
                for setting in ('off', 'on'):
                    log(f'######## decomp {setting}')
                    one = copy.copy(a)
                    one.decomp = setting
                    Build(one).run(steps)
            finally:
                manifest.set_setting('decomp', remembered)
            return 0
        Build(a).run(steps)
    except BuildError as exc:
        print(f'BUILD FAILED: {exc}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
