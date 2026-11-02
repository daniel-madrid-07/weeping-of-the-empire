"""Checks of tools/builder/toolchain.py without the internet: downloads from a local server (progress, a cut download
continued, a bad checksum, a server without ranges), the MSVC / SDK package choice on a stand-in manifest, the
environment it hands the build, the part stamps. A few seconds.

    python -I tools/builder/test_toolchain.py
"""
import contextlib, hashlib, http.server, io, os, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import toolchain  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


BLOB = os.urandom(3 * 2**20 + 123)
SHA = hashlib.sha256(BLOB).hexdigest()
served = {'ranges': True, 'cut': None, 'cut_every': None, 'hits': []}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith('/missing'):
            served['hits'].append('404')
            self.send_response(404)
            self.end_headers()
            return
        rng = self.headers.get('Range')
        served['hits'].append(rng)
        start = int(rng.split('=')[1].split('-')[0]) if rng and served['ranges'] else 0
        body = BLOB[start:]
        self.send_response(206 if start else 200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if served['cut']:   # (the connection drops partway: the client keeps a .part)
            body, served['cut'] = body[:served['cut']], None
        elif served['cut_every']:   # (a flaky line: every response drops partway)
            body = body[:served['cut_every']]
        try:
            self.wfile.write(body)
        except OSError:
            pass


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
url = f'http://127.0.0.1:{srv.server_address[1]}/blob'
toolchain.time.sleep = lambda s: None   # (no pauses between retries here)

with tempfile.TemporaryDirectory() as t:
    dest = os.path.join(t, 'dl', 'blob.bin')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        toolchain.download('blob', url, dest, SHA)
    check('download: whole file, checksum ok', open(dest, 'rb').read() == BLOB)

    # a cut download continues from its .part (the second request asks for the rest)
    dest2 = os.path.join(t, 'dl', 'blob2.bin')
    served.update(cut=2**20, hits=[])
    with contextlib.redirect_stdout(io.StringIO()):
        toolchain.download('blob', url, dest2, SHA)
    check('download: a cut download resumes with a range', open(dest2, 'rb').read() == BLOB and
          served['hits'][0] is None and served['hits'][-1] == f'bytes={2**20}-', str(served['hits']))

    # a server that ignores ranges: starts over instead of appending a second copy
    dest3 = os.path.join(t, 'dl', 'blob3.bin')
    with open(dest3 + '.part', 'wb') as f:
        f.write(BLOB[:1000])
    served.update(ranges=False)
    with contextlib.redirect_stdout(io.StringIO()):
        toolchain.download('blob', url, dest3, SHA)
    check('download: no range support -> from the start', open(dest3, 'rb').read() == BLOB)
    served.update(ranges=True)

    # a wrong checksum: an error, nothing left behind as if done
    dest4 = os.path.join(t, 'dl', 'blob4.bin')
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            toolchain.download('blob', url, dest4, '0' * 64)
        bad = None
    except toolchain.ToolchainError as e:
        bad = str(e)
    check('download: a bad checksum fails and keeps nothing', bad and 'checksum' in bad and
          not os.path.exists(dest4) and not os.path.exists(dest4 + '.part'), str(bad))
    # an existing file with the right checksum: no request at all
    served.update(hits=[])
    toolchain.download('blob', url, dest, SHA)
    check('download: done before -> not fetched again', served['hits'] == [])
    # a flaky line: every response drops after 512 KB; each try gains bytes, so 3 tries in a row is enough
    dest5 = os.path.join(t, 'dl', 'blob5.bin')
    served.update(cut_every=512 * 1024, hits=[])
    with contextlib.redirect_stdout(io.StringIO()):
        toolchain.download('blob', url, dest5, SHA, tries=3)
    served.update(cut_every=None)
    check('download: a line that keeps dropping still finishes (failures counted only without progress)',
          open(dest5, 'rb').read() == BLOB and len(served['hits']) >= 7, str(len(served['hits'])))
    # a .part left by another file of the same name: its checksum fails, fetched once more from the start
    dest6 = os.path.join(t, 'dl', 'blob6.bin')
    with open(dest6 + '.part', 'wb') as f:
        f.write(os.urandom(4096))
    served.update(hits=[])
    with contextlib.redirect_stdout(io.StringIO()) as out6:
        toolchain.download('blob', url, dest6, SHA)
    check('download: a foreign .part -> once more from the start, then right', open(dest6, 'rb').read() == BLOB and
          served['hits'][0] == 'bytes=4096-' and served['hits'][-1] is None and 'from the start' in out6.getvalue(),
          str(served['hits']))
    check('rate_text: speed and time left', toolchain.rate_text(0, 0, 0) == '' and
          toolchain.rate_text(50 * 2**20, 100 * 2**20, 2 * 2**20) == ', 2.0 MB/s, 25 s left' and
          toolchain.rate_text(0, 600 * 2**20, 2**20) == ', 1.0 MB/s, 10 min left' and
          toolchain.rate_text(10, 0, 50 * 1024) == ', 50 KB/s', toolchain.rate_text(50 * 2**20, 100 * 2**20, 2 * 2**20))
    # a 404: no retries, the host and the code said
    served.update(hits=[])
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            toolchain.download('gone', url.replace('/blob', '/missing'), os.path.join(t, 'gone.bin'))
        e404 = None
    except toolchain.ToolchainError as e:
        e404 = str(e)
    check('download: a 404 is said at once (one request, no retries)', e404 and 'answered 404' in e404 and
          served['hits'] == ['404'], f'{e404} {served["hits"]}')
    # unreachable: a clear error after the retries
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            toolchain.download('nothing', 'http://127.0.0.1:9/x', os.path.join(t, 'x'), tries=2)
        err = None
    except toolchain.ToolchainError as e:
        err = str(e)
    check('download: unreachable -> says so and that a rerun continues', err and 'internet' in err, str(err))

    # stamps
    d = os.path.join(t, 'part')
    os.makedirs(d)
    check('stamp: none yet', not toolchain.stamped(d, {'v': 1}))
    toolchain.stamp(d, {'v': 1})
    check('stamp: same / changed', toolchain.stamped(d, {'v': 1}) and not toolchain.stamped(d, {'v': 2}))

# the package choice: the newest numeric MSVC, its five parts, the newest Windows 11 SDK
pk = lambda i, **kw: dict(id=i, payloads=[{'url': 'u', 'fileName': 'f'}], **kw)
man = [pk(f'Microsoft.VC.{v}.{s}') for v in ('14.43.17.13', '14.44.17.14')
       for s in ('Tools.HostX64.TargetX64.base', 'Tools.HostX64.TargetX64.Res.base', 'CRT.Headers.base',
                 'CRT.x64.Desktop.base', 'CRT.x64.Store.base')]
man += [pk('Microsoft.VC.14.45.Premium.Tools.HostX64.TargetX64.base'), pk('Win11SDK_10.0.22621'),
        pk('Win11SDK_10.0.26100'), pk('Win10SDK_10.0.19041'), pk('Microsoft.VC.14.44.17.14.CRT.Headers.base', language='de-DE')]
v, picked, sdk = toolchain._pick(man)
check('pick: newest numeric MSVC, its parts, newest SDK', v == '14.44.17.14' and len(picked) == 5 and
      sdk['id'] == 'Win11SDK_10.0.26100', f'{v} {list(picked)} {sdk["id"]}')
later = [pk(p['id'].replace('14.44.17.14', '14.46.18.1')) for p in man if '14.44.17.14' in p['id']]
with contextlib.redirect_stdout(io.StringIO()) as note:
    v2, _, _ = toolchain._pick([p for p in man if '14.44.17.14' not in p['id']] + later)
check('pick: the pinned MSVC gone -> the newest, said', v2 == '14.46.18.1' and 'no longer offered' in note.getvalue(), v2)
v3, _, _ = toolchain._pick(man + later)
check('pick: the pinned MSVC while a newer one is offered', v3 == toolchain.MSVC_PIN, v3)
try:
    toolchain._pick([p for p in man if 'Desktop' not in p['id']])
    err = None
except toolchain.ToolchainError as e:
    err = str(e)
check('pick: a missing part is named', err and 'crt.x64.desktop' in err, str(err))

# the environment for the build: LLVM first on PATH, then MSVC's tools, then the PC's own
env = toolchain.apply({'NXRECOMP_LLVM_DIR': r'T:\llvm', 'VULKAN_SDK': r'T:\vk', 'LIB': 'L', 'INCLUDE': 'I',
                       'PATH_PREFIX': r'T:\msvc\bin'}, {'Path': r'C:\Windows', 'OTHER': '1'})
check('apply: PATH order, variables set, no PATH_PREFIX', env['Path'] == r'T:\llvm\bin;T:\msvc\bin;C:\Windows' and
      env['LIB'] == 'L' and env['VULKAN_SDK'] == r'T:\vk' and 'PATH_PREFIX' not in env and env['OTHER'] == '1', str(env))
env = toolchain.apply({'NXRECOMP_LLVM_DIR': r'T:\llvm', 'VULKAN_SDK': r'T:\vk'}, {'PATH': r'C:\x'})
check('apply: Visual Studio installed (no MSVC part)', env['PATH'] == r'T:\llvm\bin;C:\x', env['PATH'])

# an installed Visual Studio is used only if its default MSVC is at least the pin's (issue #6: an older STL lacks the
# helpers the Vulkan libraries' objects call); else the downloaded tools, said
with tempfile.TemporaryDirectory() as t:
    vs = os.path.join(t, 'VS')
    os.makedirs(os.path.join(vs, 'VC', 'Auxiliary', 'Build'))
    open(os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'Microsoft.VCToolsVersion.default.txt'), 'w').write('14.34.31933\n')
    check('vs_msvc_version reads the default toolset', toolchain.vs_msvc_version(vs) == '14.34.31933')
    check('vs_msvc_version: none -> None', toolchain.vs_msvc_version(t) is None)
    real = (toolchain.installed_vs, toolchain.ensure_msvc, toolchain.vs_env, toolchain.ensure_packages,
            toolchain.ensure_llvm, toolchain.ensure_vulkan)
    toolchain.ensure_packages = lambda *a: None
    toolchain.ensure_llvm = toolchain.ensure_vulkan = lambda tools: tools
    toolchain.ensure_msvc = lambda tools: {'LIB': 'downloaded'}
    toolchain.vs_env = lambda: {'LIB': 'installed'}
    toolchain.installed_vs = lambda: vs
    with contextlib.redirect_stdout(io.StringIO()) as buf:
        e = toolchain.ensure(os.path.join(t, 'tools'))
    check('ensure: an older installed MSVC -> the downloaded tools, said', e['LIB'] == 'downloaded' and
          'older than' in buf.getvalue(), buf.getvalue())
    open(os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'Microsoft.VCToolsVersion.default.txt'), 'w').write('14.44.35207\n')
    with contextlib.redirect_stdout(io.StringIO()) as buf:
        e = toolchain.ensure(os.path.join(t, 'tools'))
    check('ensure: a new enough installed MSVC -> its own variables', e['LIB'] == 'installed', buf.getvalue())
    toolchain.installed_vs = lambda: None
    with contextlib.redirect_stdout(io.StringIO()):
        e = toolchain.ensure(os.path.join(t, 'tools'))
    check('ensure: no Visual Studio -> the downloaded tools', e['LIB'] == 'downloaded')
    (toolchain.installed_vs, toolchain.ensure_msvc, toolchain.vs_env, toolchain.ensure_packages, toolchain.ensure_llvm,
     toolchain.ensure_vulkan) = real

srv.shutdown()
print(f'{len(fails)} failed: {fails}' if fails else 'all passed')
sys.exit(1 if fails else 0)
