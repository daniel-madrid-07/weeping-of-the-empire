"""The compilers and libraries the build needs, fetched into one folder (Setup's <install>/tools), nothing installed
system-wide and no administrator:

  llvm      LLVM (clang-cl, lld-link, llvm-lib), the release PC's version, from the LLVM project's GitHub release
  msvc      only when no Visual Studio / Build Tools with C++ is installed: Microsoft's C/C++ runtime headers and
            libraries and the Windows SDK, downloaded from Microsoft's own servers (the packages Visual Studio's
            installer uses) under Microsoft's license terms, which Setup asks the user to accept first
  vulkan    the Vulkan headers, vulkan-1.lib and shaderc_combined.lib (open source; Setup carries them and unpacks
            them here before builder.py runs)
  python    the packages the build steps import, pinned to the release PC's versions, wheels only

Each part is stamped (<part>/.toolchain.json) once complete; a run cut short redoes only the part it was in, and a
download continues from where it stopped (.part files). ensure() returns the environment the build steps need.

Linux (linux-port, 2026-11-02): two release assets instead, pinned in assets.json like Setup's own:
  llvm      setup-linux-llvm.tar.zst: LLVM 23.1.1 (the same release), cut to what a build runs, runnable on any
            distribution (tools/linux/llvm_asset.sh)
  sdk       setup-linux-sdk.tar.zst: the glibc 2.35 / libstdc++ sysroot the game is built against and its libraries
            (SDL3, Opus, libvpx, the Vulkan SDK's headers and shaderc: tools/linux/sdk.sh)
  python    as above
NXR_LINUX_LLVM_ARCHIVE / NXR_LINUX_SDK_ARCHIVE: a local archive instead of the download (before an asset is published).
"""
import hashlib
import http.client
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import socket
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import kit  # (the download kit: tools/builder/kit.py)

LLVM = dict(version='23.1.1', sha256='c54ac8146b420fe72e11e6fdd56498d6818011ad23267196b6ab37b5ac9264c3', size=901304424,
            url='https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.1/'
                'clang+llvm-23.1.1-x86_64-pc-windows-msvc.tar.xz')
# what the build uses from LLVM: its programs and clang's own headers / runtime libraries (lib/clang/<ver>). The
# archive's other 2/3 (LLVM's static libraries and C++ headers, for building against LLVM) stays packed.
LLVM_KEEP = ('bin/', 'lib/clang/')
VS_CHANNEL = 'https://aka.ms/vs/17/release/channel'
# The MSVC and Windows SDK versions installs are made with (a newer STL can need a newer clang than LLVM above); the
# newest in Microsoft's manifest only when these are gone from it
MSVC_PIN, SDK_PIN = '14.44.17.14', 'win11sdk_10.0.26100'
# The release PC's versions of the packages the steps import (outputs such as the glyph bank depend on them)
PACKAGES = ['zstandard==0.25.0', 'pillow==12.3.0', 'fonttools==4.62.1', 'numpy==2.4.4', 'cryptography==50.0.1',
            'lz4==4.4.5', 'skia-pathops==0.9.2']
# The same packages (and tools/engine/build.py's VENV_PACKAGES) hash-pinned with every dependency, for Setup's Python
# (lock_wheels.py writes them; its '# targets:' line names the Pythons they cover): pip installs them with
# --require-hashes, so a wheel whose bytes are not the pinned ones is refused, from the package index or a download kit
WHEEL_LOCKS = {'setup': 'wheels-setup.txt', 'venv': 'wheels-venv.txt'}
UA = {'User-Agent': 'nxrecomp-setup'}


def say(msg):
    print(f'[Builder] {msg}', flush=True)


class ToolchainError(Exception):
    pass


# ---- downloads -----------------------------------------------------------------------------------------------------

def _mb(n):
    return f'{n / 2**20:.0f}'


def rate_text(done, total, rate):
    """', 2.1 MB/s, 3 min left' (the speed over the last seconds, smoothed); '' before there is one"""
    if rate <= 0:
        return ''
    s = f', {rate / 2**20:.1f} MB/s' if rate >= 2**20 / 10 else f', {rate / 1024:.0f} KB/s'
    if total and done < total:
        left = (total - done) / rate
        s += f', {left / 60:.0f} min left' if left >= 90 else f', {left:.0f} s left'
    return s


def unreachable(name, url, err):
    """the sentence for a download that will not come: offline / blocked said plainly, and that a rerun continues"""
    host = urllib.parse.urlsplit(url).hostname or url
    reason = getattr(err, 'reason', err)
    offline = isinstance(reason, (socket.gaierror, ConnectionRefusedError, TimeoutError, socket.timeout))
    return (f'{name}: {host} could not be reached ({err}). ' +
            ('Check that this PC is connected to the internet; a firewall, proxy or school / work network can block '
             f'{host}. ' if offline else 'Check the internet connection. ') +
            'Then start again: the download continues where it stopped.')


def download(name, url, dest, sha256=None, size=None, tries=6):
    """url -> dest with progress lines ('download <name>: 45% (400/901 MB, 2.1 MB/s, 4 min left)': the window moves its
    bar by them). Resumes dest.part after a cut, with a growing pause (2, 4, 8 ... 60 s) between tries; gives up after
    `tries` failures in a row that gained no byte, so a slow line that keeps dropping still finishes. Never hands back
    a partial file: dest exists only once complete (and its sha256 checked when pinned); a resumed .part whose checksum
    fails (left by another version's file of the same name) is fetched once more from the start."""
    if os.path.isfile(dest) and (not sha256 or file_sha256(dest) == sha256):
        kit.record(dest, url, name, sha256)
        return dest
    if kit.take(url, dest, sha256, name, say):
        return dest
    part = dest + '.part'
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    fails, refetched, err = 0, False, None
    while True:
        have = start = os.path.getsize(part) if os.path.exists(part) else 0
        req = urllib.request.Request(url, headers=dict(UA, **({'Range': f'bytes={have}-'} if have else {})))
        complete = False
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if have and r.status != 206:
                    have = start = 0   # (no range support: from the start)
                total = (int(r.headers['Content-Length']) + have) if r.headers.get('Content-Length') else size
                done, last, rate, mark = have, time.monotonic(), 0.0, (time.monotonic(), have)
                with open(part, 'ab' if have else 'wb') as f:
                    while True:
                        chunk = r.read(1 << 18)
                        if not chunk:
                            break
                        f.write(chunk)
                        done += len(chunk)
                        now = time.monotonic()
                        if now - last >= 2:
                            inst = (done - mark[1]) / max(now - mark[0], 1e-3)
                            rate = inst if not rate else 0.7 * rate + 0.3 * inst
                            last, mark = now, (now, done)
                            say(f'download {name}: {done * 100 // total}% ({_mb(done)}/{_mb(total)} MB{rate_text(done, total, rate)})'
                                if total else f'download {name}: {_mb(done)} MB{rate_text(done, 0, rate)}')
            if total and os.path.getsize(part) < total:
                raise OSError('connection closed early')
            complete = True
        except urllib.error.HTTPError as e:
            if e.code == 416:   # (a stale .part at least as long as the file: check it as it is)
                complete = True
            elif 400 <= e.code < 500 and e.code not in (408, 429):   # (no retry helps: said at once)
                raise ToolchainError(f'{name}: {urllib.parse.urlsplit(url).hostname} answered {e.code} for {url}. '
                                     'Start again later; if it stays like this, report it.')
            else:
                err = e
        except (OSError, http.client.HTTPException) as e:   # (IncompleteRead: a body cut short)
            if isinstance(e, OSError) and (e.errno == 28 or getattr(e, 'winerror', None) in (39, 112)):   # (disk full)
                raise ToolchainError(f'{name}: the disk is full ({os.path.dirname(dest)}). Free some space, then start '
                                     'again: the download continues where it stopped.')
            err = e
        if complete:
            if sha256:
                got = file_sha256(part)
                if got != sha256:
                    os.remove(part)
                    if start and not refetched:
                        refetched = True
                        say(f'download {name}: the resumed file does not match its checksum: from the start')
                        continue
                    raise ToolchainError(f'{name}: the download does not match its checksum ({got}); start again')
            os.replace(part, dest)
            kit.record(dest, url, name, sha256)
            return dest
        gained = os.path.exists(part) and os.path.getsize(part) > start
        fails = 1 if gained else fails + 1
        if fails >= tries:
            raise ToolchainError(unreachable(name, url, err) + kit.missing_note())
        pause = min(60, 2 ** fails)
        say(f'download {name}: {err}; retrying in {pause} s')
        time.sleep(pause)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def fetch_json(url):
    p = kit.find(url, name=url, say=say)   # (a kit holds the manifests it was made with: the channel moves)
    if p:
        with open(p, 'rb') as f:
            return json.load(f)
    for attempt in range(1, 5):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                data = r.read()
            doc = json.loads(data)
            kit.record_bytes(data, url, url)
            return doc
        except (OSError, ValueError, http.client.HTTPException) as e:
            if attempt == 4:
                raise ToolchainError(f'could not read {url} ({e}). Check the internet connection.' + kit.missing_note())
            time.sleep(3 * attempt)


# ---- stamps --------------------------------------------------------------------------------------------------------

def stamped(d, what):
    try:
        with open(os.path.join(d, '.toolchain.json'), encoding='utf-8') as f:
            return json.load(f) == what
    except (OSError, ValueError):
        return False


def stamp(d, what):
    with open(os.path.join(d, '.toolchain.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(what, f, indent=1)


def fresh(d):
    """an empty d (whatever a cut run left there goes)"""
    shutil.rmtree(d, ignore_errors=True)
    if os.path.exists(d):
        raise ToolchainError(f'could not clear {d}: close any program using it and start again')
    os.makedirs(d)


# ---- LLVM ----------------------------------------------------------------------------------------------------------

class _Counting:
    """a file whose reads are counted: the unpack's progress, by compressed bytes"""
    def __init__(self, f, name, total):
        self.f, self.name, self.total, self.n, self.last = f, name, total, 0, 0.0

    def read(self, size=-1):
        b = self.f.read(size)
        self.n += len(b)
        if time.monotonic() - self.last >= 2:
            self.last = time.monotonic()
            say(f'unpack {self.name}: {self.n * 100 // self.total}% ({_mb(self.n)}/{_mb(self.total)} MB)')
        return b


def ensure_llvm(tools):
    d = os.path.join(tools, 'llvm')
    what = {'llvm': LLVM['version'], 'keep': list(LLVM_KEEP)}
    if stamped(d, what):
        return d
    # Setup's own copy (the part of LLVM the build runs, unpacked from the exe, .setup.json): no download
    if os.path.isfile(os.path.join(d, '.setup.json')) and os.path.isfile(os.path.join(d, 'bin', 'clang-cl.exe')):
        return d
    arc = download(f'LLVM {LLVM["version"]}', LLVM['url'], os.path.join(tools, 'downloads', 'llvm.tar.xz'),
                   LLVM['sha256'], LLVM['size'])
    fresh(d)
    with open(arc, 'rb') as raw:
        src = _Counting(raw, f'LLVM {LLVM["version"]}', os.path.getsize(arc))
        with tarfile.open(fileobj=src, mode='r|xz') as tf:
            for m in tf:
                rel = m.name.split('/', 1)[1] if '/' in m.name else ''
                if not rel.startswith(LLVM_KEEP) or not (m.isfile() or m.isdir()):
                    continue
                m.name = rel
                tf.extract(m, d, filter='data')
    if not os.path.isfile(os.path.join(d, 'bin', 'clang-cl.exe')):
        raise ToolchainError('LLVM: clang-cl.exe missing after unpacking')
    stamp(d, what)
    os.remove(arc)
    return d


# ---- MSVC + Windows SDK --------------------------------------------------------------------------------------------

def installed_vs():
    """an installed Visual Studio / Build Tools with the x64 C++ tools (the build's own search), or None"""
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    from pipeline.toolchain import vs_installation
    return vs_installation()


def vs_env():
    """INCLUDE / LIB / ... and the PATH entries of the installed Visual Studio checked above (vswhere's newest with the
    C++ tools, run through its vcvars64.bat), as ensure_msvc returns them for the downloaded tools"""
    from pipeline.toolchain import MSVC_VARS, msvc_environment
    base = {k: v for k, v in os.environ.items() if k.upper() not in ('INCLUDE', 'LIB', 'LIBPATH', 'VCTOOLSINSTALLDIR')}
    env = msvc_environment(base)
    path = lambda e: next((v for k, v in e.items() if k.upper() == 'PATH'), '')
    old = {p.lower() for p in path(base).split(';')}
    out = {k: env[k] for k in MSVC_VARS if k in env}
    out['PATH_PREFIX'] = ';'.join(p for p in path(env).split(';') if p and p.lower() not in old)   # (what vcvars added)
    return out


def vs_msvc_version(vs):
    """the default MSVC toolset of an installed Visual Studio (VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt,
    what its vcvars64.bat picks), e.g. '14.44.35207', or None"""
    try:
        with open(os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'Microsoft.VCToolsVersion.default.txt')) as f:
            v = f.read().strip()
        return v if re.fullmatch(r'\d+(\.\d+)+', v) else None
    except OSError:
        return None


def _vs_manifest():
    ch = fetch_json(VS_CHANNEL)
    items = {i['id']: i for i in ch['channelItems']}
    man = items['Microsoft.VisualStudio.Manifests.VisualStudio']['payloads'][0]['url']
    lic = ''
    for r in items.get('Microsoft.VisualStudio.Product.BuildTools', {}).get('localizedResources', []):
        if r.get('language', '').lower() == 'en-us':
            lic = r.get('license', '')
    return fetch_json(man), lic


def msvc_license():
    """the Build Tools license link (Setup shows it before anything is downloaded)"""
    return _vs_manifest()[1]


def _version_key(v):
    return tuple(int(x) for x in v.split('.'))


def _pick(packages):
    """the MSVC and Windows SDK packages for x64 programs built on x64: {id: package}"""
    by_id = {}
    for p in packages:
        if p.get('language') not in (None, 'neutral', 'en-US'):
            continue
        by_id.setdefault(p['id'].lower(), p)
    pre, post = 'microsoft.vc.', '.tools.hostx64.targetx64.base'
    vers = [i[len(pre):-len(post)] for i in by_id if i.startswith(pre) and i.endswith(post)]
    vers = sorted((v for v in vers if re.fullmatch(r'\d+(\.\d+)+', v)), key=_version_key)
    if not vers:
        raise ToolchainError('MSVC: no compiler package in Microsoft\'s manifest')
    v = MSVC_PIN if MSVC_PIN in vers else vers[-1]
    if v != MSVC_PIN:
        say(f'note: MSVC {MSVC_PIN} is no longer offered by Microsoft; using {v}')
    want = [f'microsoft.vc.{v}.{s}' for s in ('tools.hostx64.targetx64.base', 'tools.hostx64.targetx64.res.base',
                                              'crt.headers.base', 'crt.x64.desktop.base', 'crt.x64.store.base')]
    sdks = sorted((i for i in by_id if re.fullmatch(r'win(10|11)sdk_10\.0\.\d+', i)),
                  key=lambda i: int(i.rsplit('.', 1)[1]))
    if not sdks:
        raise ToolchainError('Windows SDK: no package in Microsoft\'s manifest')
    picked = {i: by_id[i] for i in want if i in by_id}
    missing = [i for i in want[:4] if i not in picked]
    if missing:
        raise ToolchainError('MSVC: packages missing from Microsoft\'s manifest: ' + ', '.join(missing))
    sdk = SDK_PIN if SDK_PIN in sdks else sdks[-1]
    return v, picked, by_id[sdk]


# The Windows SDK installers (MSIs) holding the headers and libraries a desktop x64 program needs
#   (10.0.26100 moved the shared headers, winapifamily.h & co., into the OnecoreUap ones)
SDK_MSIS = ('Windows SDK for Windows Store Apps Headers-x86_en-us.msi',
            'Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi',
            'Windows SDK Desktop Headers x86-x86_en-us.msi', 'Windows SDK Desktop Headers x64-x86_en-us.msi',
            'Windows SDK OnecoreUap Headers x86-x86_en-us.msi', 'Windows SDK OnecoreUap Headers x64-x86_en-us.msi',
            'Windows SDK for Windows Store Apps Libs-x86_en-us.msi', 'Windows SDK Desktop Libs x64-x86_en-us.msi',
            'Universal CRT Headers Libraries and Sources-x86_en-us.msi')
MSVC_LAYOUT = 2   # (in the stamp: a folder made by an older list is fetched again)
# headers the build includes, checked after unpacking (one per include folder)
MSVC_PROBES = (('include', 'vcruntime.h'), ('ucrt', 'stdio.h'), ('shared', 'winapifamily.h'), ('um', 'windows.h'))


def msi_staging(dl):
    """A real (not a junction), plain-named folder for msiexec: <dl>/sdk when the install folder is one (its drive
    keeps the 1-2 GB), else %ProgramData%/nxrecomp-msi-<id of this install> (one per install: two Setups never share)"""
    own = 'nxrecomp-msi-' + hashlib.sha256(os.path.normcase(os.path.abspath(dl)).encode()).hexdigest()[:10]
    for d in (os.path.join(dl, 'sdk'), os.path.join(os.environ.get('ProgramData') or r'C:\ProgramData', own)):
        if re.fullmatch(r'[A-Za-z]:\\[A-Za-z0-9_.\\-]*', d) and \
                os.path.normcase(os.path.realpath(d)) == os.path.normcase(os.path.abspath(d)):
            os.makedirs(d, exist_ok=True)
            return d
    raise ToolchainError('no plain folder for the Windows SDK installers (ProgramData has an unusual name)')


def ensure_msvc(tools, fetch_only=False):
    """Microsoft's C/C++ runtime + Windows SDK into <tools>/msvc; returns the environment variables that point the
    build at them (INCLUDE, LIB, VCToolsInstallDir...: the build's vcvars search is skipped when they are set).
    fetch_only: the downloads alone, nothing unpacked (a download kit records them); returns None."""
    d = os.path.join(tools, 'msvc')
    try:
        with open(os.path.join(d, '.toolchain.json'), encoding='utf-8') as f:
            done = json.load(f)
        if done.get('layout') == MSVC_LAYOUT and all(os.path.isdir(p) for p in done['env']['INCLUDE'].split(';')):
            return done['env']
    except (OSError, ValueError, KeyError):
        pass
    say('fetching the Microsoft C++ libraries and the Windows SDK (Microsoft\'s servers)')
    man, _ = _vs_manifest()
    ver, pkgs, sdk = _pick(man['packages'])
    dl = os.path.join(tools, 'downloads', 'msvc')
    fresh(d)
    for pid, p in pkgs.items():   # VSIX packages: zips whose Contents/ is the tree under the VS root
        for i, pay in enumerate(p.get('payloads', [])):
            z = download(pid.split('.', 6)[-1], pay['url'], os.path.join(dl, f'{pid}.{i}.vsix'), pay.get('sha256', '').lower() or None)
            if fetch_only:
                continue
            with zipfile.ZipFile(z) as zf:
                for m in zf.infolist():
                    name = m.filename.replace('\\', '/')
                    if not name.startswith('Contents/') or name.endswith('/'):
                        continue
                    parts = name[len('Contents/'):].split('/')
                    if any(x in ('', '.', '..') or ':' in x for x in parts):   # (nothing outside <tools>/msvc)
                        raise ToolchainError(f'MSVC: unexpected file name in {pid}: {name}')
                    target = os.path.join(d, *parts)
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with zf.open(m) as s, open(target, 'wb') as o:
                        shutil.copyfileobj(s, o)
    pays = {os.path.basename(p['fileName'].replace('\\', '/')).lower(): p for p in sdk['payloads']}
    msis = [n for n in SDK_MSIS if n.lower() in pays]
    if len(msis) < 4:
        raise ToolchainError(f'Windows SDK: installers missing from {sdk["id"]}')
    # Windows Installer opens neither a package reached through Setup's junction (1620) nor one under a non-ASCII
    # folder (1619): the MSIs, their CABs and the unpacked SDK go through a plain real folder of their own
    sdk_dl = msi_staging(dl)
    tmp = os.path.join(sdk_dl, 'out')
    shutil.rmtree(tmp, ignore_errors=True)   # (what a cut run unpacked)
    for n in msis:
        pay = pays[n.lower()]
        msi = download(n[:-4], pay['url'], os.path.join(sdk_dl, n), pay.get('sha256', '').lower() or None)
        with open(msi, 'rb') as f:   # the CAB files it unpacks from, named in its media table
            cabs = set(c.decode() for c in re.findall(rb'[0-9a-fA-F]{32}\.cab', f.read()))
        for c in sorted(cabs):
            if c.lower() in pays:
                pay = pays[c.lower()]
                download(c, pay['url'], os.path.join(sdk_dl, c), pay.get('sha256', '').lower() or None)
        if fetch_only:
            continue
        for wait in (0, 10, 20, 40, 60, 90):   # 1618: another installation is running (Windows Update...): wait
            time.sleep(wait)
            r = subprocess.run(f'msiexec.exe /a "{msi}" /quiet /qn TARGETDIR="{tmp}"', capture_output=True)
            if r.returncode != 1618:
                break
            say('Windows Installer is busy with another installation; waiting')
        if r.returncode:
            raise ToolchainError(f'Windows SDK: unpacking {n} failed (msiexec exit code {r.returncode})')
        say(f'unpacked {n[:-4]}')
    if fetch_only:
        shutil.rmtree(sdk_dl, ignore_errors=True)
        return None
    shutil.move(os.path.join(tmp, 'Windows Kits', '10'), os.path.join(d, 'kits'))   # (maybe another drive)
    shutil.rmtree(sdk_dl, ignore_errors=True)
    kits = os.path.join(d, 'kits')
    sdkv = sorted(os.listdir(os.path.join(kits, 'Lib')), key=lambda v: _version_key(v) if re.fullmatch(r'[\d.]+', v) else (0,))[-1]
    vc = os.path.join(d, 'VC', 'Tools', 'MSVC')
    vcv = sorted(os.listdir(vc), key=_version_key)[-1]
    vct = os.path.join(vc, vcv)
    inc, lib = os.path.join(kits, 'Include', sdkv), os.path.join(kits, 'Lib', sdkv)
    env = {
        'VCToolsInstallDir': vct + '\\', 'VCToolsVersion': vcv, 'VCINSTALLDIR': os.path.join(d, 'VC') + '\\',
        'WindowsSdkDir': kits + '\\', 'WindowsSDKVersion': sdkv + '\\', 'WindowsSDKLibVersion': sdkv + '\\',
        'UCRTVersion': sdkv, 'UniversalCRTSdkDir': kits + '\\', 'Platform': 'x64',
        'INCLUDE': ';'.join([os.path.join(vct, 'include')] +
                            [os.path.join(inc, s) for s in ('ucrt', 'shared', 'um', 'winrt', 'cppwinrt')]),
        'LIB': ';'.join([os.path.join(vct, 'lib', 'x64'), os.path.join(lib, 'ucrt', 'x64'), os.path.join(lib, 'um', 'x64')]),
        'LIBPATH': os.path.join(vct, 'lib', 'x64'),
        'PATH_PREFIX': ';'.join([os.path.join(vct, 'bin', 'Hostx64', 'x64'), os.path.join(kits, 'bin', sdkv, 'x64')]),
    }
    for p in env['LIB'].split(';'):
        if not os.path.isdir(p):
            raise ToolchainError(f'MSVC / Windows SDK: {p} missing after unpacking')
    for sub, h in MSVC_PROBES:
        if not os.path.isfile(os.path.join(vct, 'include', h) if sub == 'include' else os.path.join(inc, sub, h)):
            raise ToolchainError(f'MSVC / Windows SDK: {h} missing after unpacking')
    stamp(d, {'layout': MSVC_LAYOUT, 'msvc': ver, 'sdk': sdk['id'], 'env': env})
    shutil.rmtree(dl, ignore_errors=True)
    return env


# ---- Vulkan --------------------------------------------------------------------------------------------------------

def ensure_vulkan(tools):
    """Setup unpacks its own copy into <tools>/vulkan-sdk; a Vulkan SDK installed on this PC (VULKAN_SDK) serves too"""
    for d in (os.path.join(tools, 'vulkan-sdk'), os.environ.get('VULKAN_SDK', '')):
        if d and all(os.path.isfile(os.path.join(d, 'Lib', f)) for f in ('vulkan-1.lib', 'shaderc_combined.lib')) \
                and os.path.isfile(os.path.join(d, 'Include', 'vulkan', 'vulkan.h')):
            return d
    raise ToolchainError('the Vulkan headers and libraries are missing from ' + os.path.join(tools, 'vulkan-sdk') +
                         ': start Setup again (it unpacks them)')


# ---- Python packages -----------------------------------------------------------------------------------------------

def ensure_packages(py=sys.executable):
    missing = []
    for spec in PACKAGES:
        name, ver = spec.split('==')
        try:
            from importlib.metadata import version
            if version(name) == ver:
                continue
        except Exception:   # (PackageNotFoundError)
            pass
        missing.append(spec)
    if not missing:
        return
    say('installing the Python packages the steps use: ' + ' '.join(s.split('==')[0] for s in missing))
    # -E -s, as Setup starts us: pip sees only this Python's own site-packages. Without them a user site of the same
    # Python version (a Microsoft Store Python 3.13 with these wheels) made pip say 'already satisfied' and install
    # nothing, and the import that follows failed ('No module named cryptography', a fresh v0.1.6 install, 11-01)
    pyx = pip(py)
    pip_install(pyx, missing, lock='setup')
    import importlib
    importlib.invalidate_caches()   # (this process imports them next: its finders cached site-packages without them)
    say('Python packages installed')


def pip(py=sys.executable):
    """[py, -E, -s] with pip set up"""
    pyx = [py, '-E', '-s']
    if subprocess.run([*pyx, '-m', 'pip', '--version'], capture_output=True).returncode:
        r = subprocess.run([*pyx, '-m', 'ensurepip', '--default-pip'], capture_output=True, text=True)
        if r.returncode:
            raise ToolchainError('pip could not be set up: ' + (r.stdout + r.stderr)[-600:])
    return pyx


def python_target(py=sys.executable):
    """'cp313-win_amd64' / 'cp313-linux_x86_64' for the Python at py (WHEEL_LOCKS' '# targets:' names), or None"""
    if py == sys.executable:
        ver, mach = sys.version_info[:2], platform.machine().lower()
    else:
        r = subprocess.run([py, '-E', '-s', '-c', 'import sys, platform; print(*sys.version_info[:2], platform.machine())'],
                           capture_output=True, text=True)
        try:
            major, minor, mach = r.stdout.split()
        except ValueError:   # (no such Python, or it failed)
            return None
        ver, mach = (int(major), int(minor)), mach.lower()
    if mach not in ('amd64', 'x86_64'):
        return None
    return f'cp{ver[0]}{ver[1]}-' + ('win_amd64' if sys.platform == 'win32' else 'linux_x86_64' if LINUX else '?')


def wheel_lock(which, py=sys.executable, specs=()):
    """WHEEL_LOCKS[which] when it covers the Python at py and pins every one of specs at its version, else None (the
    specs unpinned: a developer's own other Python; or a list changed without lock_wheels.py, said loudly, which
    export_public refuses to ship: the lock would install other versions than the steps check)"""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), WHEEL_LOCKS[which])
    try:
        text = open(p, encoding='utf-8').read()
        targets = next(l.split(':', 1)[1].split() for l in text.splitlines() if l.startswith('# targets:'))
    except (OSError, StopIteration):
        return None
    if python_target(py) not in targets:
        return None
    norm = lambda s: re.sub(r'[-_.]+', '-', s.split('==')[0].lower()) + '==' + s.split('==', 1)[-1]   # (lock's names)
    stale = [s for s in specs if not re.search('^' + re.escape(norm(s)) + ' ', text, re.M)]
    if stale:
        say(f'WARNING: {os.path.basename(p)} does not pin {" ".join(stale)} (run tools/builder/lock_wheels.py): '
            'these packages are installed WITHOUT their hash check')
        return None
    return p


def pip_install(pyx, specs, quiet=False, lock=None):
    """pip install specs, wheels only: from the download kit's wheels alone when it has them (each checked), else (or
    when the kit lacks one) from the package index. lock ('setup' / 'venv'): when its file covers this Python, the
    whole hash-pinned list instead of specs, --require-hashes (every wheel checked against its pinned sha256).
    tools/engine/build.py's venv uses it too."""
    base = [*pyx, '-m', 'pip', 'install', '--disable-pip-version-check', '--no-warn-script-location',
            '--only-binary', ':all:'] + (['--quiet'] if quiet else [])
    lockf = wheel_lock(lock, pyx[0], specs) if lock else None
    reqs = ['--require-hashes', '-r', lockf] if lockf else list(specs)
    wheels, older = kit.wheels(say), ''
    if wheels:
        r = subprocess.run(base + ['--no-index', '--find-links', wheels, *reqs], capture_output=True, text=True)
        if not r.returncode:
            say('Python packages: from the download kit' + (' (hash-pinned)' if lockf else ''))
            return
        if lockf:  # (a kit made by an older Setup holds other wheels than this Setup pins)
            older = ('\nThe download kit is older than this Setup: its Python packages are not the ones this Setup '
                     'pins. Make the kit again with this Setup (Setup --download-kit) on a PC with internet.')
        say('Python packages: not all in the download kit' + (' (it is older than this Setup)' if older else '') +
            '; from the package index')
    r = subprocess.run(base + reqs, capture_output=True, text=True)
    if r.returncode:
        out = r.stdout + r.stderr
        why = ('a downloaded package is not the pinned one (THESE PACKAGES DO NOT MATCH THE HASHES): a proxy or mirror '
               'changed it, or the download broke; try again' if 'DO NOT MATCH THE HASHES' in out else 'internet?')
        raise ToolchainError(f'installing the Python packages failed ({why}):\n' + out[-1500:] + older + kit.missing_note())


def kit_wheels(lists, py=sys.executable):
    """Making a download kit: the wheels of each {lock name: specs} list (and what they need) for this Python into
    <kit>/wheels, listed; a list whose lock covers this Python: exactly its pinned wheels, hashes checked"""
    d = os.path.join(kit.making(), 'wheels')
    os.makedirs(d, exist_ok=True)
    for which, specs in lists.items():
        lockf = wheel_lock(which, py, specs)
        r = subprocess.run([*pip(py), '-m', 'pip', 'download', '--disable-pip-version-check', '--only-binary', ':all:',
                            '--dest', d] + (['--require-hashes', '-r', lockf] if lockf else list(specs)),
                           capture_output=True, text=True)
        if r.returncode:
            raise ToolchainError('downloading the Python packages failed (internet?):\n' + (r.stdout + r.stderr)[-1500:])
    kit.record_wheels(d)
    say(f'Python packages: {sum(n.endswith(".whl") for n in os.listdir(d))} wheels in the kit')


# ---- all of it -----------------------------------------------------------------------------------------------------

LINUX = sys.platform.startswith('linux')
ASSETS_REPO = 'daniel-madrid-07/weeping-of-the-empire'


def linux_asset(used_by):
    """assets.json's entry for used_by, or None"""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets.json'), encoding='utf-8') as f:
        return next((x for x in json.load(f)['assets'] if x['used_by'] == used_by), None)


def ensure_linux_part(tools, part, used_by, env_override, probe):
    """<tools>/<part> from its pinned asset (a .tar.zst of one top folder, unpacked without it), stamped by its sha256"""
    d = os.path.join(tools, part)
    local = os.environ.get(env_override, '')
    pin = linux_asset(used_by)
    if not pin and not local:
        raise ToolchainError(f'no {part} for Linux in this source (assets.json {used_by}): this version cannot build on Linux')
    if local and not os.path.isfile(local):
        raise ToolchainError(f'{env_override}: no file at {local}')
    if local and pin and file_sha256(local) != pin['sha256']:   # (a local copy of a published asset must be that asset)
        raise ToolchainError(f'{env_override}: {local} is not the {pin["name"]} this version pins (sha256 differs)')
    what = {'asset': local and os.path.basename(local) or pin['name'], 'sha256': local and file_sha256(local) or pin['sha256']}
    if stamped(d, what) and os.path.exists(os.path.join(d, probe)):
        return d
    arc = local or download(f'{part} ({pin["name"]})', f'https://github.com/{ASSETS_REPO}/releases/download/{pin["tag"]}/{pin["name"]}',
                            os.path.join(tools, 'downloads', pin['name']), pin['sha256'], pin['size'])
    import zstandard   # (PACKAGES: ensure_packages ran first)
    fresh(d)
    with open(arc, 'rb') as raw:
        src = _Counting(raw, part, os.path.getsize(arc))
        with zstandard.ZstdDecompressor().stream_reader(src) as z, tarfile.open(fileobj=z, mode='r|') as tf:
            for m in tf:
                rel = m.name.split('/', 1)[1] if '/' in m.name else ''
                if not rel:
                    continue
                m.name = rel
                tf.extract(m, d, filter='data')   # (relative symlinks inside the folder: kept; nothing outside it)
    if not os.path.exists(os.path.join(d, probe)):
        raise ToolchainError(f'{part}: {probe} missing after unpacking')
    stamp(d, what)
    if not local:
        os.remove(arc)
    return d


def ensure_linux(tools):
    """Linux: LLVM and the SDK into <tools>; the environment build.py reads (its Linux side)"""
    os.makedirs(tools, exist_ok=True)
    ensure_packages()
    llvm = ensure_linux_part(tools, 'llvm', 'setup:LinuxLlvm', 'NXR_LINUX_LLVM_ARCHIVE', os.path.join('bin', 'clang'))
    sdk = ensure_linux_part(tools, 'linux-sdk', 'setup:LinuxSdk', 'NXR_LINUX_SDK_ARCHIVE', os.path.join('sysroot', 'usr', 'include', 'stdio.h'))
    return {'NXRECOMP_LLVM_DIR': llvm, 'NXR_LINUX_DEPS': os.path.join(sdk, 'deps'), 'NXR_SYSROOT': os.path.join(sdk, 'sysroot')}


def ensure(tools):
    """Everything into <tools>; returns the environment variables for the build steps"""
    if LINUX:
        return ensure_linux(tools)
    os.makedirs(tools, exist_ok=True)
    ensure_packages()
    env = {'NXRECOMP_LLVM_DIR': ensure_llvm(tools), 'VULKAN_SDK': ensure_vulkan(tools)}
    vs = installed_vs()
    have = vs_msvc_version(vs) if vs else None
    if vs and have and _version_key(have)[:2] >= _version_key(MSVC_PIN)[:2]:
        say(f'using the Visual Studio C++ tools installed on this PC (MSVC {have})')
        env.update(vs_env())   # (that instance's own variables: the build's other vcvars search may find another one)
    else:
        if vs:   # (an older STL lacks the helpers the Vulkan libraries' objects call: __std_min_4u & co., issue #6)
            say(f'the Visual Studio C++ tools on this PC are MSVC {have or "of an unknown version"}, older than the '
                f'{MSVC_PIN} the build needs: using the downloaded ones instead (Visual Studio is not changed)')
        env.update(ensure_msvc(tools))
    return env


def apply(env, base):
    """base (an environment) with the toolchain's variables; PATH_PREFIX goes in front of PATH"""
    out = dict(base)
    prefix = env.get('PATH_PREFIX', '')
    for k, v in env.items():
        if k != 'PATH_PREFIX':
            out[k] = v
    key = next((k for k in out if k.upper() == 'PATH'), 'PATH')
    out[key] = os.pathsep.join(p for p in (os.path.join(env['NXRECOMP_LLVM_DIR'], 'bin'), prefix, out.get(key, '')) if p)
    return out


if __name__ == '__main__':   # py tools/builder/toolchain.py <tools dir> [part]: fetch one part (or all) by hand
    t = os.path.abspath(sys.argv[1])
    part = sys.argv[2] if len(sys.argv) > 2 else 'all'
    try:
        r = {'all': ensure, 'llvm': ensure_llvm, 'msvc': ensure_msvc, 'vulkan': ensure_vulkan,
             'license': lambda _t: msvc_license()}[part](t)
    except ToolchainError as e:
        say('ERROR: ' + str(e))
        sys.exit(1)
    print(json.dumps(r, indent=1))
