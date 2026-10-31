"""Locate the native toolchain on Windows: MSVC environment (vcvars64), CMake, Ninja, LLVM."""
import json
import os
import shutil
import subprocess
from pathlib import Path

from .nx.common import require

PROGRAM_FILES_X86 = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
VSWHERE = PROGRAM_FILES_X86 / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
LLVM_BIN = Path(os.environ.get('NXRECOMP_LLVM_DIR', r'C:\Program Files\LLVM')) / 'bin'


def vs_installation():
    """Install path of the newest VS / Build Tools with the x64 C++ tools, or None."""
    if VSWHERE.exists():
        out = subprocess.run([str(VSWHERE), '-latest', '-products', '*', '-requires',
                              'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json'],
                             capture_output=True, text=True)
        rows = json.loads(out.stdout or '[]')
        if rows:
            return Path(rows[0]['installationPath'])
    for edition in ('BuildTools', 'Community', 'Professional', 'Enterprise'):
        p = PROGRAM_FILES_X86 / 'Microsoft Visual Studio' / '2022' / edition
        if (p / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat').exists():
            return p
    return None


MSVC_VARS = ('LIB', 'LIBPATH', 'INCLUDE', 'EXTERNAL_INCLUDE', 'VCToolsInstallDir', 'VCINSTALLDIR', 'VSINSTALLDIR',
             'WindowsSdkDir', 'WindowsSDKVersion', 'WindowsSDKLibVersion', 'UCRTVersion', 'UniversalCRTSdkDir',
             'VCToolsVersion', 'VisualStudioVersion', 'Platform', 'VSCMD_ARG_TGT_ARCH', 'VSCMD_ARG_HOST_ARCH')


def msvc_environment(base=None, cache=None):
    """os.environ plus what vcvars64.bat sets (LIB, INCLUDE, PATH...). Unchanged when already in a dev prompt.

    `cache` (a JSON path) keeps vcvars64's variables keyed by its path + mtime: vcvars takes ~20 s idle and minutes on
    a loaded machine, and its result only changes when Visual Studio is updated."""
    env = dict(base or os.environ)
    if env.get('VCToolsInstallDir') and env.get('LIB'):
        return env
    vs = vs_installation()
    require(vs is not None, 'Visual Studio 2022 / Build Tools with the C++ x64 tools not found (needed for the '
                            'Windows SDK and CRT libraries)')
    vcvars = vs / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    key = f'{vcvars}|{vcvars.stat().st_mtime_ns}'
    if cache and Path(cache).exists():
        try:
            saved = json.loads(Path(cache).read_text(encoding='utf-8'))
        except ValueError:
            saved = {}
        if saved.get('key') == key and all(Path(d).is_dir() for d in saved['vars'].get('LIB', '').split(';') if d):
            env.update(saved['vars'])
            path_key = 'PATH' if 'PATH' in env else 'Path'
            env[path_key] = saved['path_prefix'] + env.get(path_key, '')
            return env
    before_path = env.get('PATH') or env.get('Path') or ''
    out = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True, env=env)
    require(out.returncode == 0, f'vcvars64.bat failed: {out.stderr.strip()[:400]}')
    names = {k.upper(): k for k in env}   # Windows variables are case-insensitive: `Path` from set == `PATH` here
    for line in out.stdout.splitlines():
        if '=' in line and not line.startswith('='):
            k, v = line.split('=', 1)
            env[names.setdefault(k.upper(), k)] = v
    require(env.get('LIB'), 'vcvars64.bat did not set LIB')
    if cache:
        new_path = env.get('PATH') or env.get('Path') or ''
        prefix = new_path[:len(new_path) - len(before_path)] if new_path.endswith(before_path) else ''
        Path(cache).parent.mkdir(parents=True, exist_ok=True)
        with open(cache, 'w', newline='\n', encoding='utf-8') as f:
            json.dump(dict(key=key, vars={k: env[k] for k in MSVC_VARS if k in env}, path_prefix=prefix), f, indent=1)
    return env


def find_tool(name, env, extra=()):
    """Path of `name` from PATH in `env`, else from VS's bundled CMake/Ninja, else `extra` dirs."""
    found = shutil.which(name, path=env.get('PATH') or env.get('Path'))
    if found:
        return found
    vs = vs_installation()
    dirs = list(extra)
    if vs:
        cm = vs / 'Common7' / 'IDE' / 'CommonExtensions' / 'Microsoft' / 'CMake'
        dirs += [cm / 'CMake' / 'bin', cm / 'Ninja']
    for d in dirs:
        p = Path(d) / f'{name}.exe'
        if p.exists():
            return str(p)
    return None


def native_env(cache=None):
    """MSVC environment with LLVM, CMake and Ninja on PATH. Returns (env, {tool: path})."""
    env = msvc_environment(cache=cache)
    tools = {}
    for name in ('cmake', 'ninja', 'clang-cl', 'lld-link'):
        tools[name] = find_tool(name, env, extra=[LLVM_BIN])
        require(tools[name], f'{name} not found (install with winget: Kitware.CMake / Ninja-build.Ninja / LLVM.LLVM)')
    path_key = 'PATH' if 'PATH' in env else 'Path'
    extra = os.pathsep.join(dict.fromkeys(str(Path(p).parent) for p in tools.values()))
    env[path_key] = extra + os.pathsep + env.get(path_key, '')
    return env, tools
