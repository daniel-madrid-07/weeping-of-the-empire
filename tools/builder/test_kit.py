"""Checks of the download kit (tools/builder/kit.py) through the code that uses it, without the internet: a local
server stands in for the network. Making a kit records each download (by content, with its url) and Microsoft's
manifests; using one takes a file from it without a request, by its pinned sha256 or its url; a damaged copy is
refused and fetched again; a file the kit lacks, offline, fails saying it is not in the kit either; the wheels folder
is used only when every wheel is the one listed; build.py's DLSS / FidelityFX fetch takes the kit's copy. Seconds.

    python -I tools/builder/test_kit.py
"""
import contextlib, hashlib, http.server, io, json, os, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kit  # noqa: E402
import toolchain  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


BLOB = os.urandom(300_000)
SHA = hashlib.sha256(BLOB).hexdigest()
MAN = json.dumps({'channelItems': [], 'n': 1}).encode()
hits = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        hits.append(self.path)
        body = MAN if self.path.endswith('.json') else BLOB
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{srv.server_address[1]}'


def quiet(f, *a, **kw):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        r = f(*a, **kw)
    return r, out.getvalue()


def env(make=None, use=None):
    for k, v in (('NXR_KIT_MAKE', make), ('NXR_KIT', use)):
        if v:
            os.environ[k] = v
        else:
            os.environ.pop(k, None)


def check_setup():
    """Setup's side (Builder.cs class Kit), compiled with gui/test_kit.cs by Windows' own C# compiler"""
    import subprocess
    gui = os.path.join(HERE, 'gui')
    sys.path.insert(0, gui)
    from make_setup import CSC
    with tempfile.TemporaryDirectory(prefix='kitcs') as t:
        rel, exe = os.path.join(t, 'Release.cs'), os.path.join(t, 'test_kit.exe')
        with open(rel, 'w', encoding='utf-8') as f:
            f.write('static class Release { public const string Product = "Test Product", Tag = "v0.0.0", Repo = "x/y", '
                    'LlvmVersion = "1", Commit = "", LlvmSha256 = "", LlvmAsset = "", LlvmTag = "", VulkanSha256 = "", VulkanAsset = "", '
                    'VulkanTag = ""; }\n')
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:KitTest', f'/out:{exe}', '/r:System.Windows.Forms.dll',
                            '/r:System.Drawing.dll', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
                            os.path.join(gui, 'Builder.cs'), rel, os.path.join(gui, 'test_kit.cs')], capture_output=True, text=True)
        if r.returncode:
            check('Setup side: compiles', False, (r.stdout + r.stderr)[-1500:])
            return
        os.makedirs(os.path.join(t, 'k'))
        import shutil
        git = shutil.which('git')   # (the fetched-commit cases need one; without it they are skipped, said)
        if not git:
            print('note: no git on PATH: the annotated-tag cases are skipped')
        p = subprocess.run([exe, os.path.join(t, 'k')] + ([git] if git else []), capture_output=True, text=True, timeout=120)
        sys.stdout.write(p.stdout + p.stderr)
        check('Setup side (Kit)', p.returncode == 0)


def main():
    with tempfile.TemporaryDirectory(prefix='kit') as t:
        k = os.path.join(t, 'nxr-kit')
        os.makedirs(k)
        # ---- making
        env(make=k)
        quiet(toolchain.download, 'the blob', base + '/blob.zip', os.path.join(t, 'a', 'blob.zip'), SHA)
        doc = kit.load(k)
        e = next((e for e in doc['files'] if e['url'] == base + '/blob.zip'), None)
        check('make: a download is recorded by content, with its url', e is not None and e['sha256'] == SHA
              and e['path'] == 'files/' + SHA and open(os.path.join(k, 'files', SHA), 'rb').read() == BLOB, str(e))
        quiet(toolchain.fetch_json, base + '/man.json')
        m = next((e for e in kit.load(k)['files'] if e['url'] == base + '/man.json'), None)
        check('make: a manifest is recorded', m is not None and open(os.path.join(k, *m['path'].split('/')), 'rb').read() == MAN)
        quiet(toolchain.download, 'the blob', base + '/blob.zip', os.path.join(t, 'a', 'blob.zip'), SHA)
        check('make: a file already there is recorded once', sum(e['url'] == base + '/blob.zip' for e in kit.load(k)['files']) == 1)
        whl = os.path.join(k, 'wheels')
        os.makedirs(whl)
        open(os.path.join(whl, 'pkg-1.0-py3-none-any.whl'), 'wb').write(b'wheel')
        kit.record_wheels(whl)

        # ---- using
        env(use=k)
        hits.clear()
        dest = os.path.join(t, 'b', 'blob.zip')
        _, said = quiet(toolchain.download, 'the blob', base + '/blob.zip', dest, SHA)
        check('use: taken from the kit, no request', not hits and open(dest, 'rb').read() == BLOB and 'from the download kit' in said,
              f'{hits} {said!r}')
        dest = os.path.join(t, 'c', 'blob.zip')
        quiet(toolchain.download, 'the blob', 'http://127.0.0.1:9/moved/blob.zip', dest, SHA)
        check('use: by its pinned sha256 under another url', os.path.isfile(dest) and not hits)
        doc, _ = quiet(toolchain.fetch_json, base + '/man.json')
        check('use: a manifest from the kit, no request', doc == json.loads(MAN) and not hits, str(hits))
        check('use: the wheels folder when every wheel is listed', kit.wheels(lambda s: None) == whl)
        open(os.path.join(whl, 'pkg-1.0-py3-none-any.whl'), 'wb').write(b'other')
        check('use: a changed wheel: the kit\'s wheels are not used', kit.wheels(lambda s: None) is None)

        good = open(os.path.join(k, 'files', SHA), 'rb').read()
        open(os.path.join(k, 'files', SHA), 'wb').write(b'damaged' + good[7:])
        dest = os.path.join(t, 'd', 'blob.zip')
        _, said = quiet(toolchain.download, 'the blob', base + '/blob.zip', dest, SHA)
        check('use: a damaged copy is refused and fetched from the network', hits == ['/blob.zip'] and
              open(dest, 'rb').read() == BLOB and 'damaged' in said, f'{hits} {said!r}')
        open(os.path.join(k, 'files', SHA), 'wb').write(good)

        try:
            quiet(toolchain.download, 'nothing', 'http://127.0.0.1:9/x', os.path.join(t, 'x'), tries=1)
            check('use: missing and offline fails', False)
        except toolchain.ToolchainError as ex:
            check('use: missing and offline says it is not in the kit either', 'not in the download kit' in str(ex)
                  and k in str(ex), str(ex)[-160:])

        # build.py's DLSS / FidelityFX fetch
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'engine'))
        import build
        hits.clear()
        out = os.path.join(t, 'dlss')
        quiet(build.dlss_fetch, {'lib/blob.dll': SHA}, out, True, url='http://127.0.0.1:9/', what='test SDK')
        check('use: build.py\'s SDK fetch takes the kit\'s copy (pinned sha256)', open(os.path.join(out, 'blob.dll'), 'rb').read() == BLOB)

        # no kit
        env()
        check('no kit: nothing found, nothing said', kit.find(base + '/blob.zip', SHA) is None and kit.missing_note() == '')
        os.environ['NXR_KIT'] = t   # (a folder without kit.json)
        check('a folder without kit.json is no kit', kit.using() is None)
        env()
    srv.shutdown()
    check_setup()
    print('all passed' if not fails else f'FAILED: {len(fails)}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
