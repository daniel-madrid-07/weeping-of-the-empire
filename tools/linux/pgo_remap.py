"""The release's PGO profile for a Linux build (linux-port, 2026-11-02).

The profile is trained on the Windows build, where the generated functions are C++ names in Microsoft's mangling
(?f_1b4d760@@YAXPEIAUCtx@@@Z); the Linux build names the same functions the Itanium way (_Z9f_1b4d760P3Ctx). Their
bodies are the same source (gen.py's output is target-independent), so their profile records apply as they are: this
renames every generated function's record (f_ / s_ / the --fn-prefix ones and --split chunks: `void <name>(Ctx*)`) and
drops the rest (the runtime's: other code on Linux, and pgo_rt stays off there). clang still checks each record's CFG
hash against the function it compiles and ignores one that differs, as for a Windows key whose code moved on.

usage: python tools/linux/pgo_remap.py <windows.profdata> <out.profdata> [--llvm DIR]
  prints how many records were renamed and dropped; the output is build.py's --pgo input on Linux
"""
import argparse, os, re, subprocess, sys, tempfile

# void name(Ctx* __restrict) (PEIAU), or Ctx* (PEAU: the sdk import stubs s_imp_*), C++ linkage; on Itanium both P3Ctx
MSVC_GEN = re.compile(r'^\?([A-Za-z_][A-Za-z0-9_]*)@@YAXPEI?AUCtx@@@Z$')


def itanium(name):
    """the Itanium name of `void <name>(Ctx*)` (the pointer's own restrict is not part of the signature)"""
    return f'_Z{len(name)}{name}P3Ctx'


def remap_text(lines):
    """the text profile's lines with each record renamed or dropped; (lines, renamed, dropped). A record starts with its
    name line (no leading '#', not a number, not ':ir' / ':fe' headers) followed by '# Func Hash:'"""
    out, renamed, dropped, keep = [], 0, 0, True
    i, n = 0, len(lines)
    while i < n:
        ln = lines[i]
        if i + 1 < n and lines[i + 1].startswith('# Func Hash:') and not ln.startswith('#'):
            m = MSVC_GEN.match(ln.rstrip('\n'))
            keep = bool(m)
            if m:
                out.append(itanium(m.group(1)) + '\n')
                renamed += 1
            else:
                dropped += 1
            i += 1
            continue
        if keep or ln.startswith(':'):   # (':ir' and the other header lines stay)
            out.append(ln)
        i += 1
    return out, renamed, dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--llvm', default=os.environ.get('NXRECOMP_LLVM_DIR', ''))
    a = ap.parse_args()
    exe = 'llvm-profdata.exe' if os.name == 'nt' else 'llvm-profdata'
    tool = os.path.join(a.llvm, 'bin', exe) if a.llvm else exe
    with tempfile.TemporaryDirectory() as t:
        txt = os.path.join(t, 'in.proftext')
        subprocess.run([tool, 'merge', '--text', a.src, '-o', txt], check=True)
        with open(txt, encoding='utf-8', errors='surrogateescape') as f:
            out, renamed, dropped = remap_text(f.readlines())
        if not renamed:   # (nothing written: builder.py keeps a dst it finds, so an empty one would stay for good)
            print(f'pgo_remap: no generated-function record in {a.src} ({dropped} runtime records); {a.dst} not written')
            return 1
        new = os.path.join(t, 'out.proftext')
        with open(new, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
            f.writelines(out)
        subprocess.run([tool, 'merge', new, '-o', a.dst + '.tmp'], check=True)
        os.replace(a.dst + '.tmp', a.dst)
    print(f'pgo_remap: {renamed} generated-function records renamed to Itanium names, {dropped} runtime records dropped -> {a.dst}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
