"""Setup's uninstall (Builder.cs Uninstaller): compiles it with test_uninstall.cs (Windows' own C# compiler, as
make_setup.py does) and runs it on a made-up install in the temp folder. Nothing real is touched; seconds.
    py tools/builder/gui/test_uninstall.py"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_setup import CSC  # noqa: E402
from test_preflight import RELEASE  # noqa: E402


def main():
    with tempfile.TemporaryDirectory(prefix='uninstall') as tmp:
        rel, exe = os.path.join(tmp, 'Release.cs'), os.path.join(tmp, 'test_uninstall.exe')
        with open(rel, 'w', encoding='utf-8') as f:
            f.write(RELEASE)
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:UninstallTest', f'/out:{exe}',
                            '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.IO.Compression.dll',
                            '/r:System.IO.Compression.FileSystem.dll', os.path.join(HERE, 'Builder.cs'), rel,
                            os.path.join(HERE, 'test_uninstall.cs')], capture_output=True, text=True)
        if r.returncode:
            sys.exit('csc failed:\n' + r.stdout + r.stderr)
        p = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(p.stdout + p.stderr)
        return p.returncode


if __name__ == '__main__':
    sys.exit(main())
