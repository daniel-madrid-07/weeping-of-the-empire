"""clang-cl invocation for compile checks: MSVC environment discovery and the flags generated code needs."""
from __future__ import annotations

import functools
import glob
import io
import json
import os
import subprocess
import tarfile

LLVM_BIN = os.path.join(os.environ.get('NXRECOMP_LLVM_DIR') or r'C:\Program Files\LLVM', 'bin')   # (pipeline/toolchain.py)
CLANG_CL = os.path.join(LLVM_BIN, 'clang-cl.exe')
LLVM_LIB = os.path.join(LLVM_BIN, 'llvm-lib.exe')

# Flags every generated translation unit is compiled with (see recomp/simd/README.md for -ffp-contract=off).
# -fdenormal-fp-math=dynamic: guest FPCR.FZ sets MXCSR.DAZ/FTZ at run time (rt_fpcr_changed); in the default (IEEE)
# mode clang rewrites integer denormal tests on float bits (a64_ffz, a64_fzdn) into FP compares, which DAZ answers
# as zero, so a denormal would skip its flush. CFLAGS holds literals only: cmake/NxGuestModule.cmake parses them.
CFLAGS = ['/nologo', '/c', '/std:c11', '/clang:-ffp-contract=off', '/clang:-fdenormal-fp-math=dynamic',
          '/clang:-march=x86-64-v3', '/clang:-fno-strict-aliasing', '/GS-', '/Zl', '/clang:-Wno-unused-label']
FP_DENORMAL = '/clang:-fdenormal-fp-math=dynamic'


def _vcvars() -> str | None:
    pats = [r'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat']
    for p in pats:
        hits = sorted(glob.glob(p))
        if hits:
            return hits[-1]
    # Visual Studio installed elsewhere (another drive): its installer's vswhere knows where
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)') or r'C:\Program Files (x86)', 'Microsoft Visual Studio',
                           'Installer', 'vswhere.exe')
    if os.path.isfile(vswhere):
        root = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                               'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                              capture_output=True, text=True).stdout.strip()
        bat = os.path.join(root, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
        if root and os.path.isfile(bat):
            return bat
    return None


@functools.lru_cache(maxsize=1)
def msvc_env() -> dict[str, str]:
    """Environment with INCLUDE/LIB/PATH from vcvars64 (cached on disk next to the venv-independent temp dir)."""
    env = dict(os.environ)
    if env.get('INCLUDE') and env.get('LIB'):
        return env
    cache = os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'nxrecomp', 'msvc_env.json')
    try:
        with open(cache) as f:
            saved = json.load(f)
        if all(os.path.isdir(d) for d in saved['INCLUDE'].split(';') if d):
            env.update(saved)
            return env
    except (OSError, KeyError, ValueError):
        pass
    bat = _vcvars()
    if bat is None:
        raise RuntimeError('vcvars64.bat not found (MSVC Build Tools needed for headers/libs)')
    out = subprocess.run(f'cmd /s /c ""{bat}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    saved = {}
    for line in out.splitlines():
        k, _, v = line.partition('=')
        if k.upper() in ('INCLUDE', 'LIB', 'LIBPATH', 'PATH'):
            saved[k.upper()] = v
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, 'w', newline='\n') as f:
        json.dump(saved, f)
    env.update(saved)
    return env


def compile_cmd(src: str, obj: str, include_dirs: list[str], opt: str = '/O1') -> list[str]:
    return [CLANG_CL, *CFLAGS, opt, *[f'-I{d}' for d in include_dirs], src, f'/Fo{obj}']


# ---- pinned recomp/simd -----------------------------------------------------------------------------------------
# recomp/simd's mapping.py (Python) and a64_*.h (C) must come from the same revision: the emitted helper calls have
# to match the header the shards are compiled against. NXRECOMP_SIMD_ROOT names a directory containing `simd/`
# (a snapshot of recomp/simd); when set, recomp imports mapping.py from there and puts it first on the C include
# path. A generated module records the root it was emitted with in <gen>/simd_root.txt.
SIMD_ENV = 'NXRECOMP_SIMD_ROOT'
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def simd_root() -> str | None:
    r = os.environ.get(SIMD_ENV)
    return r if r and os.path.isdir(os.path.join(r, 'simd')) else None


def make_simd_snapshot(dest_root: str, rev: str = 'HEAD') -> tuple[str, str]:
    """Extract recomp/simd at git revision `rev` into <dest_root>/<tree12>/recomp; returns (root, commit)."""
    commit = subprocess.run(['git', '-C', REPO, 'rev-parse', rev], capture_output=True, text=True,
                            check=True).stdout.strip()
    tree = subprocess.run(['git', '-C', REPO, 'rev-parse', f'{commit}:recomp/simd'], capture_output=True, text=True,
                          check=True).stdout.strip()
    dest = os.path.join(dest_root, tree[:12])                    # one snapshot per recomp/simd tree
    root = os.path.join(dest, 'recomp')
    if not os.path.isfile(os.path.join(root, 'simd', 'mapping.py')):
        tar = subprocess.run(['git', '-C', REPO, 'archive', '--format=tar', commit, 'recomp/simd'],
                             capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
            tf.extractall(dest, filter='data')
    return root, commit


def use_simd_root(root: str) -> None:
    os.environ[SIMD_ENV] = os.path.abspath(root)
