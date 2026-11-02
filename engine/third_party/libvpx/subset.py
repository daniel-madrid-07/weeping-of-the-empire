"""Make engine/third_party/libvpx/lib/vpx.lib: the VP9 decoder's members of a vcpkg libvpx, debug info stripped.

vcpkg's x64-windows-static-md vpx.lib is 16 MB (both codecs' encoders and decoders, every SIMD level). The engine calls
the VP9 decoder only (runtime/movie_core.h VpxVp9Dec), so this links a probe that calls exactly those functions with
/VERBOSE, keeps the members lld-link loaded, strips their debug sections and packs them as a new library. The headers
next to it (include/vpx) come from the same tree.

usage: python engine/third_party/libvpx/subset.py <vcpkg installed\\x64-windows-static-md> [--llvm DIR]
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = r'''
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
int main(void) {
    vpx_codec_ctx_t c; vpx_codec_dec_cfg_t cfg = {4, 1920, 1080};
    vpx_codec_iface_t* i = vpx_codec_vp9_dx();
    if (vpx_codec_dec_init(&c, i, &cfg, 0)) return 1;
    vpx_codec_iter_t it = 0;
    vpx_codec_decode(&c, 0, 0, 0, 0);
    while (vpx_codec_get_frame(&c, &it)) {}
    return vpx_codec_error(&c)[0] + vpx_codec_iface_name(i)[0] + vpx_codec_destroy(&c);
}
'''


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {" ".join(cmd)}\n{r.stdout}{r.stderr}')
    return r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vcpkg')
    ap.add_argument('--llvm', default=os.environ.get('NXRECOMP_LLVM_DIR', r'C:\Program Files\LLVM') + r'\bin')
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools', 'engine'))
    import build as B  # noqa: E402  (the MSVC environment)
    env = B.build_env()
    src = os.path.join(a.vcpkg, 'lib', 'vpx.lib')
    tool = lambda n: os.path.join(a.llvm, n)
    tmp = tempfile.mkdtemp(prefix='vpx-subset-')
    try:
        probe = os.path.join(tmp, 'probe.c')
        with open(probe, 'w', newline='\n') as f: f.write(PROBE)
        out = run([tool('clang.exe'), '--target=x86_64-pc-windows-msvc', '-fms-runtime-lib=dll', '-fuse-ld=lld', '-O1',
                   '-I', os.path.join(HERE, 'include'), probe, src, '-o', os.path.join(tmp, 'probe.exe'), '-Wl,/VERBOSE'],
                  env=env)
        # lld-link /VERBOSE: "Loaded vpx.lib(<member>) for <symbol>"
        members = sorted({m for m in re.findall(r'Loaded .*?vpx\.lib\((.+?)\) for ', out)})
        if not members: sys.exit('no vpx.lib member loaded (lld-link /VERBOSE format changed?)')
        names = run([tool('llvm-ar.exe'), 't', src]).split()
        dup = {n for n in members if names.count(n) > 1}
        if dup: sys.exit(f'members with the same name in vpx.lib, cannot extract by name: {sorted(dup)[:5]}')
        x = os.path.join(tmp, 'x'); os.makedirs(x)
        run([tool('llvm-ar.exe'), 'x', src] + members, cwd=x)
        objs = []
        for m in members:
            p = os.path.join(x, os.path.basename(m))
            run([tool('llvm-objcopy.exe'), '--strip-debug', p])
            objs.append(p)
        dst = os.path.join(HERE, 'lib', 'vpx.lib')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        rsp = os.path.join(tmp, 'objs.rsp')
        with open(rsp, 'w', newline='\n') as f: f.write('\n'.join(f'"{o}"' for o in objs))
        run([tool('llvm-lib.exe'), f'/out:{dst}', f'@{rsp}'])
        # the subset links the probe by itself
        run([tool('clang.exe'), '--target=x86_64-pc-windows-msvc', '-fms-runtime-lib=dll', '-fuse-ld=lld', '-O1',
             '-I', os.path.join(HERE, 'include'), probe, dst, '-o', os.path.join(tmp, 'probe2.exe')], env=env)
        print(f'{dst}: {len(members)} of {len(set(names))} members, {os.path.getsize(dst)} bytes (from {os.path.getsize(src)})')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
