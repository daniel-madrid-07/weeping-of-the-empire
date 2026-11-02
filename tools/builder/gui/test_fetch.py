"""Setup's downloads (Builder.cs Fetch.Download): compiles it with test_fetch.cs (Windows' own C# compiler, as
make_setup.py does) and runs it against a local server: /blob serves a file with ranges, /flaky drops every response
after 512 KB. No internet, no window; seconds.
    py tools/builder/gui/test_fetch.py"""
import hashlib
import http.server
import os
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_setup import CSC  # noqa: E402
from test_preflight import RELEASE  # noqa: E402

BLOB = os.urandom(3 * 2**20 + 77)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith('/missing'):
            self.send_response(404)
            self.end_headers()
            return
        rng = self.headers.get('Range')
        start = int(rng.split('=')[1].split('-')[0]) if rng else 0
        if start >= len(BLOB):
            self.send_response(416)
            self.end_headers()
            return
        body = BLOB[start:]
        self.send_response(206 if start else 200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if self.path.startswith('/flaky'):
            body = body[:512 * 1024]
        try:
            self.wfile.write(body)
        except OSError:
            pass


def main():
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='fetch') as tmp:
        rel, exe, blob = os.path.join(tmp, 'Release.cs'), os.path.join(tmp, 'test_fetch.exe'), os.path.join(tmp, 'blob.bin')
        open(rel, 'w', encoding='utf-8').write(RELEASE)
        open(blob, 'wb').write(BLOB)
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:FetchTest', f'/out:{exe}',
                            '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.IO.Compression.dll',
                            '/r:System.IO.Compression.FileSystem.dll', os.path.join(HERE, 'Builder.cs'), rel,
                            os.path.join(HERE, 'test_fetch.cs')], capture_output=True, text=True)
        if r.returncode:
            sys.exit('csc failed:\n' + r.stdout + r.stderr)
        out = os.path.join(tmp, 'out')
        os.makedirs(out)
        p = subprocess.run([exe, f'http://127.0.0.1:{srv.server_address[1]}', hashlib.sha256(BLOB).hexdigest(), out, blob],
                           capture_output=True, text=True, timeout=180)
        sys.stdout.write(p.stdout + p.stderr)
        srv.shutdown()
        return p.returncode


if __name__ == '__main__':
    sys.exit(main())
