"""Keeps the translator cache's configuration hash honest (vulkan/exec.c tc_env_vars): it must hash exactly the
environment variables translate.c reads. A variable translate.c reads but the list misses would let a cached
translation outlive a change of that variable (a stale translation); one the list has but translate.c no longer reads
throws the cache away for nothing. Also checks translate.c reads its variables only through literal getenv calls (a
wrapper or a computed name would hide one from this test).
Usage: python test_tcache_env.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    tr = open(os.path.join(HERE, 'translate.c'), encoding='utf-8', errors='replace').read()
    ex = open(os.path.join(HERE, '..', 'vulkan', 'exec.c'), encoding='utf-8', errors='replace').read()
    calls = re.findall(r'\bgetenv\s*\(([^)]*)\)', tr)
    bad = [c for c in calls if not re.fullmatch(r'\s*"[A-Z0-9_]+"\s*', c)]
    reads = {c.strip().strip('"') for c in calls if c not in bad}
    m = re.search(r'tc_env_vars\[\]\s*=\s*\{(.*?)\};', ex, re.S)
    if not m:
        print('FAIL: exec.c has no tc_env_vars list')
        return 1
    listed = re.findall(r'"([A-Z0-9_]+)"', m.group(1))
    fails = []
    if bad:
        fails.append(f'translate.c getenv with a non-literal name: {bad}')
    if len(listed) != len(set(listed)):
        fails.append('tc_env_vars has a duplicate')
    if reads - set(listed):
        fails.append(f'read by translate.c, missing from exec.c tc_env_vars: {sorted(reads - set(listed))}')
    if set(listed) - reads:
        fails.append(f'in tc_env_vars, not read by translate.c: {sorted(set(listed) - reads)}')
    for f in fails:
        print('FAIL:', f)
    if not fails:
        print(f'ok: {len(reads)} variables, translate.c and tc_env_vars agree')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
