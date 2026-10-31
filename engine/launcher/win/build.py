"""Build the Windows launcher: clang for x86_64-pc-windows-msvc with tools/engine/build.py's toolchain (MSVC headers
and libraries through recomp.toolchain.msvc_env). A few seconds; no game objects involved.

usage: python engine/launcher/win/build.py [launcher] [--out DIR]   "TotK Launcher.exe" (default <work>/engine/launcher)
       python engine/launcher/win/build.py test     [--out DIR]   build and run test_saves.exe and test_launcher.exe (fake trees in %TEMP%)
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'engine'))
import build as B  # noqa: E402  (tools/engine/build.py: WORK, LLVM, build_env)

CFLAGS = ['--target=x86_64-pc-windows-msvc', '-std=gnu17', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
          '-Wno-missing-field-initializers', '-fms-runtime-lib=static', '-g', '-fuse-ld=lld']
# comctl32 v6 (TaskDialogIndirect) through an embedded manifest that lld-link writes itself
MANIFEST = ['-Wl,/manifest:embed', "-Wl,/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "
            "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"]


def clang(args):
    cmd = [os.path.join(B.LLVM, 'clang.exe')] + args
    print('$', ' '.join(f'"{a}"' if ' ' in a else a for a in cmd), flush=True)
    subprocess.check_call(cmd, env=B.build_env())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', nargs='?', default='launcher', choices=['launcher', 'test'])
    ap.add_argument('--out', default=os.path.join(B.WORK, 'engine', 'launcher'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    saves = os.path.join(HERE, 'saves.c')
    if a.what == 'test':
        exe = os.path.join(a.out, 'test_saves.exe')
        clang(CFLAGS + [os.path.join(HERE, 'test_saves.c'), saves, '-o', exe])
        rc = subprocess.call([exe])
        # launcher.c's log pruning and early-exit window, with fake_game.exe as the packages' totk.exe
        fake, tl = os.path.join(a.out, 'fake_game.exe'), os.path.join(a.out, 'test_launcher.exe')
        clang(CFLAGS + [os.path.join(HERE, 'fake_game.c'), '-o', fake])
        clang(CFLAGS + MANIFEST + [os.path.join(HERE, 'test_launcher.c'), saves, '-o', tl, '-luser32', '-lshell32', '-lcomctl32',
                        '-lole32', '-luuid', '-ladvapi32'])
        sys.exit(rc or subprocess.call([tl, fake]))
    exe = os.path.join(a.out, 'TotK Launcher.exe')
    clang(CFLAGS + MANIFEST + [os.path.join(HERE, 'launcher.c'), saves, '-o', exe, '-Wl,/subsystem:windows',
                               '-Wl,/entry:wWinMainCRTStartup', '-luser32', '-lshell32', '-lcomctl32', '-lole32',
                               '-luuid', '-ladvapi32'])
    print('->', exe)


if __name__ == '__main__':
    main()
