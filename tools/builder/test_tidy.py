"""tools/builder/tidy.py on a synthetic work folder: the key just built, the newest key of another game version and
the objects they link stay; older keys, orphaned cached objects, older source snapshots and tmp go; never anything
without the key. Seconds.

    python -I tools/builder/test_tidy.py
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tidy  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}", flush=True)
    if not ok:
        fails.append(name)


def key(eng, parent, name, version=None, objs=(), mtime=None):
    d = os.path.join(eng, parent, name)
    for sub in ('gen/main', 'obj/gen', 'exe'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    open(os.path.join(d, 'gen', 'main', 't_1.cpp'), 'w').write('int f() { return 1; }\n' * 2000)
    open(os.path.join(d, 'exe', 'totk.exe'), 'wb').write(b'MZ' + b'\0' * 1000)
    json.dump({'game_version': version} if version else {}, open(os.path.join(d, 'manifest.json'), 'w'))
    for o in objs:   # obj/ hard-linked into objcache, as build.py objects() does
        c = os.path.join(eng, 'objcache', o[:2], o + '.o')
        if not os.path.exists(c):
            os.makedirs(os.path.dirname(c), exist_ok=True)
            open(c, 'wb').write(os.urandom(4096))
        os.link(c, os.path.join(d, 'obj', 'gen', o + '.o'))
    if mtime:
        os.utime(os.path.join(d, 'exe', 'totk.exe'), (mtime, mtime))
    return d


w = tempfile.mkdtemp(prefix='tidy-')
eng = os.path.join(w, 'engine')
cur = key(eng, 'game-var', 'aaaa', objs=('a1', 'a2', 's1'))
old = key(eng, 'game-var', 'bbbb', objs=('b1', 's1'), mtime=1000)       # an earlier version's 1.0.0 key
base = key(eng, 'game', 'cccc', objs=('c1',), mtime=1000)                 # an old -O2 key
v143a = key(eng, 'game-var', 'dddd', '1.4.3', objs=('d1',), mtime=2000)   # 1.4.3: newest stays
v143b = key(eng, 'game-var', 'eeee', '1.4.3', objs=('e1',), mtime=1000)
os.makedirs(os.path.join(eng, 'objcache', 'zz'))
open(os.path.join(eng, 'objcache', 'zz', 'zz.o'), 'wb').write(b'x' * 4096)   # linked by nothing
for s in ('abcdef123456', '000000000000'):
    os.makedirs(os.path.join(eng, 'src', s, 'engine'))
os.makedirs(os.path.join(eng, 'tmp', 'x'))
open(os.path.join(eng, 'tmp', 'y.txt'), 'w').write('y')
for keep in ('img', 'overlay', 'venv', 'pgo'):
    os.makedirs(os.path.join(eng, keep))
    open(os.path.join(eng, keep, 'f'), 'w').write('keep')

calls = []
r = tidy.tidy(w, 'nope', 'abcdef1234567890', log=lambda m: calls.append(m))
check('1 an unknown key removes nothing', r == (0, []) and os.path.isdir(old) and 'nothing removed' in calls[-1])

freed, done = tidy.tidy(w, 'aaaa', 'abcdef1234567890', log=lambda m: calls.append(m))
exists = lambda *p: os.path.exists(os.path.join(*p))
check('2 the key just built stays whole', all(exists(cur, p) for p in ('gen/main/t_1.cpp', 'obj/gen/a1.o', 'exe/totk.exe',
                                                                      'manifest.json')))
check('3 older keys of the same version go, the newest 1.4.3 stays',
      not exists(old) and not exists(base) and exists(v143a) and not exists(v143b), str(done))
cached = sorted(f for _, _, fs in os.walk(os.path.join(eng, 'objcache')) for f in fs)
check('4 objcache keeps what the kept keys link, nothing else', cached == ['a1.o', 'a2.o', 'd1.o', 's1.o'], str(cached))
check('5 the shared object still reads through the kept key', open(os.path.join(cur, 'obj', 'gen', 's1.o'), 'rb').read(10))
check('6 only the built commit\'s source snapshot stays', os.listdir(os.path.join(eng, 'src')) == ['abcdef123456'])
check('7 tmp emptied', os.listdir(os.path.join(eng, 'tmp')) == [])
check('8 img / overlay / venv / pgo untouched', all(exists(eng, k, 'f') for k in ('img', 'overlay', 'venv', 'pgo')))
check('9 freed bytes and a log line', freed > 0 and calls[-1].startswith('tidy: '), calls[-1])
if os.name == 'nt':
    check('10 the kept key\'s gen/ compressed (NTFS)', 'generated code compressed' in done and
          tidy.tree_bytes(os.path.join(cur, 'gen')) < os.path.getsize(os.path.join(cur, 'gen', 'main', 't_1.cpp')),
          str(done))
check('11 the plan is logged before it runs', any(m.startswith('tidy: removing ') for m in calls), str(calls[:3]))

# an update from an older install (installer's release_check 2026-11-02: 49.7 GB left): its unpacked RomFS folder and
# cached objects shared by hard links with another folder
w2 = tempfile.mkdtemp(prefix='tidy2-')
eng2 = os.path.join(w2, 'engine')
cur2 = key(eng2, 'game-var', 'ffff', objs=('f1',))
other = tempfile.mkdtemp(prefix='tidy2-other-')   # another install's copy of an old object (the same file)
os.makedirs(os.path.join(eng2, 'objcache', 'o1'))
old_obj = os.path.join(eng2, 'objcache', 'o1', 'o1.o')
open(old_obj, 'wb').write(os.urandom(4096))
os.link(old_obj, os.path.join(other, 'o1.o'))
v100 = os.path.join(w2, 'extracted', 'v100')
os.makedirs(os.path.join(v100, 'romfs', 'Pack'))
open(os.path.join(v100, 'romfs', 'Pack', 'a.zs'), 'wb').write(b'a' * 100)
open(os.path.join(v100, 'romfs.bin'), 'wb').write(b'\0' * 1000)
st = os.stat(os.path.join(v100, 'romfs.bin'))
json.dump({'status': 'pass', 'expected_match': True, 'romfs_size': 1000, 'total_file_bytes': 100},
          open(os.path.join(v100, 'romfs-report.json'), 'w'))
verified = os.path.join(v100, 'romfs.bin.verified.json')
json.dump({'size': 1000, 'mtime_ns': st.st_mtime_ns + 1}, open(verified, 'w'))   # (a record that does not match)
log2 = []
tidy.tidy(w2, 'ffff', 'abcdef1234567890', log=log2.append)
check('12 the unpacked RomFS stays while romfs.bin is not proven', os.path.exists(os.path.join(v100, 'romfs', 'Pack', 'a.zs')))
check('13 a cached object no kept build links goes, even shared with another folder (that copy stays)',
      not os.path.exists(old_obj) and os.path.exists(os.path.join(other, 'o1.o')) and
      os.path.exists(os.path.join(eng2, 'objcache', 'f1'[:2], 'f1.o')), str(log2))
json.dump({'size': 1000, 'mtime_ns': st.st_mtime_ns}, open(verified, 'w'))
log2 = []
freed, done = tidy.tidy(w2, 'ffff', 'abcdef1234567890', log=log2.append)
check('14 romfs.bin proven: the unpacked RomFS goes, its empty folder stays (the mount root), listed first',
      os.path.isdir(os.path.join(v100, 'romfs')) and not os.listdir(os.path.join(v100, 'romfs')) and
      os.path.exists(os.path.join(v100, 'romfs.bin')) and any('romfs' in m and m.startswith('tidy: removing') for m in log2) and
      'the unpacked game files (romfs.bin serves the game)' in done, f'{done} {log2}')
# a RomFS folder that is a link out of the work folder (a shared extraction): never followed, never emptied
w3 = tempfile.mkdtemp(prefix='tidy3-')
key(os.path.join(w3, 'engine'), 'game-var', 'gggg')
shared = tempfile.mkdtemp(prefix='tidy3-shared-')
os.makedirs(os.path.join(shared, 'v100', 'romfs'))
open(os.path.join(shared, 'v100', 'romfs', 'keep.zs'), 'wb').write(b'k')
for f in ('romfs.bin', 'romfs-report.json', 'romfs.bin.verified.json'):
    open(os.path.join(shared, 'v100', f), 'wb').write(open(os.path.join(v100, f), 'rb').read())
os.makedirs(os.path.join(w3, 'extracted'))
if os.name == 'nt':
    subprocess_ok = os.system(f'cmd /c mklink /J "{os.path.join(w3, "extracted", "v100")}" "{os.path.join(shared, "v100")}" >nul') == 0
    tidy.tidy(w3, 'gggg', 'abcdef1234567890', log=lambda m: None)
    check('15 a RomFS reached through a link out of <work>: kept', subprocess_ok and
          os.path.exists(os.path.join(shared, 'v100', 'romfs', 'keep.zs')))
print('FAIL' if fails else 'OK', len(fails))
sys.exit(1 if fails else 0)
