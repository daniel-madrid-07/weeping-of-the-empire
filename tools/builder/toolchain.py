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
"""
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile

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
UA = {'User-Agent': 'nxrecomp-setup'}


def say(msg):
    print(f'[Builder] {msg}', flush=True)


class ToolchainError(Exception):
    pass


# ---- downloads -----------------------------------------------------------------------------------------------------

def _mb(n):
    return f'{n / 2**20:.0f}'


def download(name, url, dest, sha256=None, size=None, tries=6):
    """url -> dest with progress lines ('download <name>: 45% (400/901 MB)': the window moves its bar by them).
    Resumes dest.part after a cut; retries with a growing pause; checks the sha256 when one is pinned."""
    if os.path.isfile(dest) and (not sha256 or file_sha256(dest) == sha256):
        return dest
    part = dest + '.part'
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for attempt in range(1, tries + 1):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        req = urllib.request.Request(url, headers=dict(UA, **({'Range': f'bytes={have}-'} if have else {})))
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if have and r.status != 206:
                    have = 0   # (no range support: from the start)
                total = (int(r.headers['Content-Length']) + have) if r.headers.get('Content-Length') else size
                done, last, t0 = have, 0.0, time.monotonic()
                with open(part, 'ab' if have else 'wb') as f:
                    while True:
                        chunk = r.read(1 << 20)
                        if not chunk:
                            break
                        f.write(chunk)
                        done += len(chunk)
                        if time.monotonic() - last >= 2:
                            last = time.monotonic()
                            say(f'download {name}: {done * 100 // total}% ({_mb(done)}/{_mb(total)} MB)' if total
                                else f'download {name}: {_mb(done)} MB')
            if total and os.path.getsize(part) < total:
                raise OSError('connection closed early')
            break
        except urllib.error.HTTPError as e:
            if e.code == 416:   # (a stale .part at least as long as the file: check it as it is)
                break
            err = e
        except (OSError, http.client.HTTPException) as e:   # (IncompleteRead: a body cut short)
            err = e
        if attempt == tries:
            raise ToolchainError(f'{name}: download failed ({err}). Check the internet connection and start again: '
                                 'it continues where it stopped.')
        say(f'download {name}: {err}; retrying')
        time.sleep(3 * attempt)
    if sha256:
        got = file_sha256(part)
        if got != sha256:
            os.remove(part)
            raise ToolchainError(f'{name}: the download does not match its checksum ({got}); start again')
    os.replace(part, dest)
    return dest


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def fetch_json(url):
    for attempt in range(1, 5):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
                return json.load(r)
        except (OSError, ValueError, http.client.HTTPException) as e:
            if attempt == 4:
                raise ToolchainError(f'could not read {url} ({e}). Check the internet connection.')
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


def ensure_msvc(tools):
    """Microsoft's C/C++ runtime + Windows SDK into <tools>/msvc; returns the environment variables that point the
    build at them (INCLUDE, LIB, VCToolsInstallDir...: the build's vcvars search is skipped when they are set)."""
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
        for wait in (0, 10, 20, 40, 60, 90):   # 1618: another installation is running (Windows Update...): wait
            time.sleep(wait)
            r = subprocess.run(f'msiexec.exe /a "{msi}" /quiet /qn TARGETDIR="{tmp}"', capture_output=True)
            if r.returncode != 1618:
                break
            say('Windows Installer is busy with another installation; waiting')
        if r.returncode:
            raise ToolchainError(f'Windows SDK: unpacking {n} failed (msiexec exit code {r.returncode})')
        say(f'unpacked {n[:-4]}')
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
    if subprocess.run([py, '-m', 'pip', '--version'], capture_output=True).returncode:
        r = subprocess.run([py, '-m', 'ensurepip', '--default-pip'], capture_output=True, text=True)
        if r.returncode:
            raise ToolchainError('pip could not be set up: ' + (r.stdout + r.stderr)[-600:])
    r = subprocess.run([py, '-m', 'pip', 'install', '--disable-pip-version-check', '--no-warn-script-location',
                        '--only-binary', ':all:', *missing], capture_output=True, text=True)
    if r.returncode:
        raise ToolchainError('installing the Python packages failed (internet?):\n' + (r.stdout + r.stderr)[-1500:])
    import importlib
    importlib.invalidate_caches()   # (this process imports them next: its finders cached site-packages without them)
    say('Python packages installed')


# ---- all of it -----------------------------------------------------------------------------------------------------

def ensure(tools):
    """Everything into <tools>; returns the environment variables for the build steps"""
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
    out[key] = ';'.join(p for p in (os.path.join(env['NXRECOMP_LLVM_DIR'], 'bin'), prefix, out.get(key, '')) if p)
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
