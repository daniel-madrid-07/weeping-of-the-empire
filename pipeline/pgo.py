"""Profile-guided optimisation, the merge step: raw counters from instrumented runs -> one .profdata.

    py pipeline/build.py --variant release --pgo gen        instrumented TotK (out/TotK-release-pgogen)
    <play: a drive through the scenes to optimise; the exe writes pgo-raw/totk-<pid>.profraw next to itself>
    py -m pipeline.pgo merge                                 -> <work>/pgo/totk.profdata
    py pipeline/build.py --variant release --pgo use        optimised TotK (out/TotK-release-pgo)

The raw files must come from the same build the merge feeds back (llvm-profdata matches functions by name and CFG
hash: code that changed since is compiled without profile, silently). Several runs merge into one profile (counts
add up); --weight is not exposed, every run counts the same.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_WORK = Path(os.environ.get('NXRECOMP_WORK', r'C:\nxrecomp-work'))


def llvm_profdata() -> Path:
    llvm = Path(os.environ.get('NXRECOMP_LLVM_DIR', r'C:\Program Files\LLVM'))
    exe = llvm / 'bin' / 'llvm-profdata.exe'
    if not exe.is_file():
        raise SystemExit(f'pgo: {exe} not found (set NXRECOMP_LLVM_DIR)')
    return exe


def raw_files(work: Path, raw: list[Path]) -> list[Path]:
    """The .profraw files: those named (files or folders), else every out/TotK*-pgogen*/pgo-raw/*.profraw."""
    dirs = raw or sorted((work / 'out').glob('TotK*-pgogen*/pgo-raw'))
    out = []
    for d in dirs:
        out += [d] if d.is_file() else sorted(d.glob('*.profraw'))
    return [f for f in out if f.stat().st_size > 0]


def merge(work: Path, raw: list[Path], out: Path) -> Path:
    files = raw_files(work, raw)
    if not files:
        raise SystemExit('pgo: no .profraw files (run the --pgo gen build first; it writes pgo-raw/ next to TotK.exe)')
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.profdata.tmp')
    # --failure-mode=all: a bad input (a run killed while a profile was being written) is skipped with a warning
    # instead of failing the whole merge; it fails only when no input is readable
    cmd = [str(llvm_profdata()), 'merge', '--failure-mode=all', '-o', str(tmp), *map(str, files)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f'pgo: llvm-profdata merge failed: {(p.stdout + p.stderr).strip()[-2000:]}')
    skipped = [f for f in files if str(f) in p.stderr]
    for f in skipped:
        print(f'pgo: skipped unreadable {f}')
    os.replace(tmp, out)
    size = sum(f.stat().st_size for f in files)
    print(f'pgo: merged {len(files) - len(skipped)} of {len(files)} raw profile(s) ({size / 2**20:.1f} MiB) -> {out} ({out.stat().st_size / 2**20:.1f} MiB)')
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='merge raw profiles into the .profdata that --pgo use reads')
    m.add_argument('--work', type=Path, default=DEFAULT_WORK)
    m.add_argument('--raw', type=Path, action='append', default=[],
                   help='a .profraw file or a folder of them (repeatable); default: out/TotK*-pgogen*/pgo-raw')
    m.add_argument('--out', type=Path, default=None, help='default <work>/pgo/totk.profdata')
    a = p.parse_args(argv)
    merge(a.work.resolve(), a.raw, (a.out or a.work / 'pgo' / 'totk.profdata').resolve())
    return 0


if __name__ == '__main__':
    sys.exit(main())
