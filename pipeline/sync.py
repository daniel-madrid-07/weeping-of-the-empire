"""Move a freshly generated module over the previous one, touching only files whose bytes changed.

Ninja decides by mtime: a regenerated shard whose content is identical must keep its old file (and mtime) so it is not
recompiled. Generated files the new run no longer produces are removed (a stale shard would still be globbed and
linked). Only the top level is managed: subdirectories belong to other tools (the compile check keeps its objects in
`<module>/obj/`) and are never entered, and only files with a generated-source suffix are ever removed."""
import filecmp
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

GENERATED_SUFFIXES = ('.c', '.h', '.json', '.tsv', '.txt')


def sync_tree(src, dst, suffixes=GENERATED_SUFFIXES):
    """Make the top-level files of `dst` equal to those of `src`, moving changed/new files out of `src`.
    Returns dict(same, changed, added, removed, changed_files=[names])."""
    src, dst = Path(src), Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    new = {p.name: p for p in src.iterdir() if p.is_file()}
    old = {p.name for p in dst.iterdir() if p.is_file() and p.suffix in suffixes}
    counts = dict(same=0, changed=0, added=0, removed=0, changed_files=[])

    def identical(name):
        target = dst / name
        return target.is_file() and target.stat().st_size == new[name].stat().st_size and \
            filecmp.cmp(new[name], target, shallow=False)

    # ~2,000 files / 650 MB for main: the byte comparisons are I/O-bound, run them side by side.
    with ThreadPoolExecutor(max_workers=8) as pool:
        same = dict(zip(sorted(new), pool.map(identical, sorted(new))))
    for name, p in sorted(new.items()):
        target = dst / name
        if same[name]:
            counts['same'] += 1
            continue
        if target.is_file():
            counts['changed'] += 1
        else:
            counts['added'] += 1
        counts['changed_files'].append(name)
        os.replace(p, target)
    for name in sorted(old - new.keys()):
        (dst / name).unlink()
        counts['removed'] += 1
        counts['changed_files'].append(name)
    return counts
