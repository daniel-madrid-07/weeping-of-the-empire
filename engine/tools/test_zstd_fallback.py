"""The step-7 engine fixes (rstb_slack.py, make_gameover_fix.py, make_quickmenu_fix.py) without Git's libzstd.dll
(public issue #8): their zstd block run with NXR_LIBZSTD=zstandard (the package) and with the DLL, each one's frames
read by the other, with and without a dictionary.

    python engine/tools/test_zstd_fallback.py     (Python with zstandard; the DLL half needs Git for Windows)"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = ('rstb_slack.py', 'make_gameover_fix.py', 'make_quickmenu_fix.py')
fails = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (f' {detail}' if detail and not ok else ''))
    if not ok:
        fails.append(name)


def zstd_block(tool, how):
    """the tool's own module-level zstd code (from def _libzstd() to the first function after zstd()), run alone"""
    src = open(os.path.join(HERE, tool), encoding='utf-8').read()
    start = src.index('def _libzstd():')
    end = src.index('\ndef ', src.index('if sys.platform'))   # (the next top-level function: the zstd ones are indented)
    old = os.environ.get('NXR_LIBZSTD')
    os.environ.pop('NXR_LIBZSTD', None)
    if how == 'zstandard':
        os.environ['NXR_LIBZSTD'] = 'zstandard'
    try:
        ns = {}
        exec('import ctypes, ctypes.util, os, shutil, sys\n' + src[start:end], ns)
    finally:
        os.environ.pop('NXR_LIBZSTD', None)
        if old is not None:
            os.environ['NXR_LIBZSTD'] = old
    return ns


data = b''.join(b'%d some text that repeats, ' % (i % 97) for i in range(20000))
import zstandard  # noqa: E402
dic = zstandard.train_dictionary(4096, [data[i:i + 900] for i in range(0, len(data) - 900, 450)]).as_bytes()
for tool in TOOLS:
    pkg = zstd_block(tool, 'zstandard')
    dll = zstd_block(tool, 'dll')
    has_dll = (dll.get('ZSTD') or dll.get('Z')) is not None
    check(f'{tool}: NXR_LIBZSTD=zstandard takes the package', (pkg.get('ZSTD') or pkg.get('Z')) is None)
    if 'ZSTD' in pkg:   # (rstb_slack: no dictionary)
        z = pkg['zstd'](data, 3)
        check(f'{tool}: package round trip', pkg['unzstd'](z) == data)
        if has_dll:
            check(f'{tool}: package frame -> DLL', dll['unzstd'](z) == data)
            check(f'{tool}: DLL frame -> package', pkg['unzstd'](dll['zstd'](data, 3)) == data)
    else:
        for d in (dic, b''):
            what = 'with a dictionary' if d else 'no dictionary'
            z = pkg['zstd'](data, d, 3)
            check(f'{tool}: package round trip, {what}', pkg['unzstd'](z, d) == data)
            if has_dll:
                check(f'{tool}: package frame -> DLL, {what}', dll['unzstd'](z, d) == data)
                check(f'{tool}: DLL frame -> package, {what}', pkg['unzstd'](dll['zstd'](data, d, 3), d) == data)
    if not has_dll:
        print(f'(no libzstd.dll on this PC: {tool} DLL half skipped)')

print(f'{len(fails)} failed: {fails}' if fails else 'all passed')
sys.exit(1 if fails else 0)
