"""Setup's Win32 calls (Builder.cs: the junction, the process-tree end, the start outside its job, the shortcuts) do
what cmd mklink / taskkill / explorer.exe / WScript.Shell did: compiles them with test_win32.cs (Windows' own C#
compiler, as make_setup.py does) and runs it. Child processes are the test exe itself: no window opens. Seconds.
    py tools/builder/gui/test_win32.py"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_setup import CSC  # noqa: E402
from test_preflight import RELEASE  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix='win32test') as tmp:
        rel, exe = os.path.join(tmp, 'Release.cs'), os.path.join(tmp, 'test_win32.exe')
        with open(rel, 'w', encoding='utf-8') as f:
            f.write(RELEASE)
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:Win32Test', f'/out:{exe}',
                            '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.IO.Compression.dll',
                            '/r:System.IO.Compression.FileSystem.dll', os.path.join(HERE, 'Builder.cs'), rel,
                            os.path.join(HERE, 'test_win32.cs')], capture_output=True, text=True)
        if r.returncode:
            sys.exit('csc failed:\n' + r.stdout + r.stderr)
        p = subprocess.run([exe], capture_output=True, text=True, timeout=180)
        sys.stdout.write(p.stdout + p.stderr)
    print('all passed' if p.returncode == 0 else 'FAILED')
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
