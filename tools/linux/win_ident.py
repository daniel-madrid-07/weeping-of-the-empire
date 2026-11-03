"""Check that a change leaves the Windows runtime's code unchanged (linux-port, 2026-11-02).

Every Linux-port step must keep the Windows build identical: its runtime edits live in `#ifndef _WIN32` branches or
Linux-only files. This compiles the Windows runtime (mkninja_win.py's objs-rt: every runtime / NVN / Vulkan object,
the game objects are not involved) from two trees, the base revision and the working tree, with the same flags, and
compares each object with its debug sections stripped (line tables move when lines are added; the code does not).
Source paths are mapped to one prefix, so __FILE__ strings match.

An object that still differs is compared again as disassembly + data with every number masked: equal then means only
constants moved (e.g. a __LINE__ after an inserted #ifdef block), which the report lists apart from real code changes.

usage: python tools/linux/win_ident.py [--base REV] [--jobs 6] [--keep]
  --base  the revision to compare with (default: git merge-base HEAD main)
  exit 0: every object identical (or constants-only, listed); 1: code differs (listed); 2: a build failed
"""
import argparse, os, re, shutil, subprocess, sys, tarfile, tempfile, io

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'engine'))
import build  # noqa: E402  (build_env, NINJA, PY, LLVM, VCPKG, SNAPSHOT_PATHS)


def git(*a):
    return subprocess.run(['git', '-C', REPO, *a], capture_output=True, text=True, check=True).stdout


def export(rev, dst):
    """the base revision's engine tree (and what it includes) into dst, as build.py snapshot() does"""
    paths = [p for p in build.SNAPSHOT_PATHS if git('ls-tree', '--name-only', rev, '--', p).strip()]
    data = subprocess.run(['git', '-C', REPO, 'archive', '--format=tar', rev, '--', *paths], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(dst, filter='tar')


def compile_rt(tree, out, jobs, env):
    """mkninja_win.py --prebuilt-gen with an empty gen dir, then ninja objs-rt: the runtime objects only"""
    gen = os.path.join(out, 'gen', 'main')
    os.makedirs(gen, exist_ok=True)
    clang = os.path.join(build.LLVM, 'clang.exe')
    pm = f'-ffile-prefix-map={os.path.join(tree, "engine")}=E:/engine -ffile-prefix-map={tree}=E:'.replace('\\', '/')
    # (maxwell/translate.c's build stamp: one fixed date and time for both trees)
    pm += ' -Wno-builtin-macro-redefined -D__DATE__=\\"Jan_01_2000\\" -D__TIME__=\\"00:00:00\\"'
    e = dict(env, CC=f'"{clang}" {pm}', CXX=f'"{os.path.join(build.LLVM, "clang++.exe")}" {pm}')
    subprocess.run([build.PY, '-I', os.path.join(tree, 'engine', 'tools', 'recomp', 'mkninja_win.py'), '--prebuilt-gen',
                    '--root', os.path.join(tree, 'engine'), '--gen', gen, '--obj', os.path.join(out, 'obj'), '--out', out,
                    '--vcpkg', build.VCPKG], check=True, env=e, stdout=subprocess.DEVNULL)
    r = subprocess.run([build.NINJA, '-C', out, f'-j{jobs}', 'objs-rt'], env=e, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-2000:])
        return False
    return True


def objects(out):
    res = {}
    for sub, _, files in os.walk(out):
        if os.path.relpath(sub, out).split(os.sep)[0] in ('gen', 'obj'):
            continue
        for f in files:
            if f.endswith('.o'):
                p = os.path.join(sub, f)
                res[os.path.relpath(p, out).replace('\\', '/')] = p
    return res


def stripped(p, tmp):
    q = os.path.join(tmp, 'strip.o')
    subprocess.run([os.path.join(build.LLVM, 'llvm-objcopy.exe'), '--strip-debug', p, q], check=True)
    with open(q, 'rb') as f:
        b = bytearray(f.read())
    if b[:2] == b'\x64\x86':   # COFF x86-64: the header's TimeDateStamp is the compile time (no /Brepro on objects)
        b[4:8] = b'\0\0\0\0'
    return bytes(b)


NUM = re.compile(r'\b(0x[0-9a-fA-F]+|\d+)\b')


def masked(p):
    """the disassembly with numbers masked (constants-only differences compare equal)"""
    dump = subprocess.run([os.path.join(build.LLVM, 'llvm-objdump.exe'), '-d', '-r', '--no-show-raw-insn', '--no-leading-addr', p],
                          capture_output=True, text=True).stdout
    return NUM.sub('#', '\n'.join(l for l in dump.splitlines() if not l.startswith(p)))


def data_sections(p):
    """{section name: hex dump} of the sections that are not code (.text*) or debug"""
    dump = subprocess.run([os.path.join(build.LLVM, 'llvm-objdump.exe'), '-s', p], capture_output=True, text=True).stdout
    out, cur = {}, None
    for l in dump.splitlines():
        if l.startswith('Contents of section '):
            name = l[len('Contents of section '):].rstrip(':')
            cur = None if name.startswith(('.text', '.debug')) else name
            if cur: out[cur] = out.get(cur, '') + '\n'
        elif cur:
            out[cur] += l + '\n'
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    base = a.base or git('merge-base', 'HEAD', 'main').strip()
    tmp = tempfile.mkdtemp(prefix='win_ident_')
    try:
        env = build.build_env()
        export(base, os.path.join(tmp, 'base'))
        new_tree = os.path.join(tmp, 'new')   # the working tree's files (uncommitted edits included), same layout
        for p in build.SNAPSHOT_PATHS:
            src = os.path.join(REPO, p)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(new_tree, p), ignore=shutil.ignore_patterns('__pycache__'))
            elif os.path.isfile(src):
                os.makedirs(os.path.dirname(os.path.join(new_tree, p)), exist_ok=True)
                shutil.copy2(src, os.path.join(new_tree, p))
        outs = {}
        for name in ('base', 'new'):
            tree = os.path.join(tmp, name)
            outs[name] = os.path.join(tmp, 'out-' + name)
            if not compile_rt(tree, outs[name], a.jobs, env):
                print(f'win_ident: the {name} tree ({base[:10] if name == "base" else "working tree"}) failed to compile for Windows')
                return 2
        ob, on = objects(outs['base']), objects(outs['new'])
        same, consts, differ = [], [], []
        for k in sorted(set(ob) | set(on)):
            if k not in ob or k not in on:
                differ.append(f'{k} ({"added" if k in on else "removed"})')
            elif stripped(ob[k], tmp) == stripped(on[k], tmp):
                same.append(k)
            elif masked(ob[k]) == masked(on[k]):
                db, dn = data_sections(ob[k]), data_sections(on[k])
                consts.append(f'{k} (data differs in: {", ".join(s for s in sorted(set(db) | set(dn)) if db.get(s) != dn.get(s)) or "none"})')
            else:
                differ.append(k)
        print(f'win_ident vs {base[:10]}: {len(same)} objects identical, {len(consts)} constants-only, {len(differ)} differ')
        for k in consts: print('  constants only:', k)
        for k in differ: print('  DIFFERS:', k)
        return 1 if differ else 0
    finally:
        if a.keep:
            print('kept', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
