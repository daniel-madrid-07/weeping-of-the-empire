"""Read Ninja's `.ninja_log` (v5/v6/v7): per-output build durations.

    python -m pipeline.ninjalog <build dir> [--top 10]

Ninja appends one line per built output and occasionally recompacts the file (rewriting it in arbitrary order), so the
order of lines cannot tell runs apart. With `since` (a time.time() taken before the build), the outputs of that build
are the ones whose file on disk was written after it; without it, the last run is guessed from the start-time clock
restarting at 0 (good enough for a log that was not recompacted in between).
"""
import argparse
import sys
from pathlib import Path


def _rows(build_dir):
    lines = (Path(build_dir) / '.ninja_log').read_text(encoding='utf-8', errors='replace').splitlines()
    rows = []
    for line in lines[1:]:
        parts = line.split('\t')
        if len(parts) >= 4 and parts[0].isdigit() and parts[1].isdigit():
            rows.append((int(parts[0]), int(parts[1]), parts[3]))
    return rows


def last_run(build_dir, since=None):
    """[(output, seconds, start_ms, end_ms)] of the most recent build, longest first."""
    rows = _rows(build_dir)
    if since is not None:
        latest = {}
        for start, end, out in rows:
            latest[out] = (start, end)   # later lines win
        picked = {}
        for out, (start, end) in latest.items():
            try:
                if (Path(build_dir) / out).stat().st_mtime >= since - 1:
                    picked[out] = (start, end)
            except OSError:
                pass
    else:
        cut = 0
        for i in range(1, len(rows)):
            if rows[i][0] < rows[i - 1][0] and rows[i][0] < 1000:
                cut = i
        picked = {}
        for start, end, out in rows[cut:]:
            picked[out] = (start, end)
    return sorted(((o, (e - s) / 1000.0, s, e) for o, (s, e) in picked.items()), key=lambda r: -r[1])


def summary(build_dir, since=None, names=('guest_main.lib', 'guest_sdk.lib', 'guest_subsdk0.lib', 'TotK.exe')):
    """{name: seconds} for the guest archives / exe of the last build, plus 'objects' (count compiled) and 'wall'."""
    run = last_run(build_dir, since)
    out = {}
    for o, secs, _, _ in run:
        base = o.replace('\\', '/').rsplit('/', 1)[-1]
        if base in names:
            out[base] = secs
    out['objects'] = sum(1 for o, *_ in run if o.endswith('.obj'))
    out['wall'] = (max(e for *_, e in run) - min(s for _, _, s, _ in run)) / 1000.0 if run else 0.0
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('build_dir', type=Path)
    p.add_argument('--top', type=int, default=10)
    p.add_argument('--since', type=float, help='time.time() before the build (outputs written after it)')
    a = p.parse_args(argv)
    for o, secs, _, _ in last_run(a.build_dir, a.since)[:a.top]:
        print(f'{secs:9.1f}s  {o}')
    print(summary(a.build_dir, a.since))


if __name__ == '__main__':
    sys.exit(main())
