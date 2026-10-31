"""Write build.ninja for the native Windows x64 build: a reconstruction of the upstream tools/recomp/mkninja.py the Linux
script says it mirrors (that file is not published). clang (MSVC ABI, UCRT), lld-link.

usage: python mkninja_win.py --gen DIR --out DIR --obj DIR --vcpkg DIR [--gen-opt -O1] [--gen-pool N] [--prebuilt-gen | --game-only]
  --prebuilt-gen  link the generated objects in --obj, do not build them (a runtime-only build against shared game objects)
  --game-only     only the generated objects (no runtime, no link), with paths relative to --out, so that a copy of the
                  directory (tools/engine/build.py: a patched key cloned from its base) keeps the same ninja command lines
  --rt-include D  (--game-only) take rt.h from D instead of <root>/runtime, so the command lines do not depend on the tree
  --vcpkg   an installed vcpkg tree (x64-windows-static-md: lib/opus.lib)
  --vksdk   the Vulkan SDK (default $VULKAN_SDK): Include/, Lib/vulkan-1.lib, Lib/shaderc_combined.lib
Same source sets as mkninja_linux.py, but the Win32 side: runtime/nvn/shim (SDL3 / pthread / unistd / vma stand-ins,
compat.h force-included into the Vulkan TUs), nvn/vulkan/shim_win.c instead of runtime/linux/*.
"""
import glob, os, sys

args = sys.argv[1:]
def opt(name, default=None):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
prebuilt = '--prebuilt-gen' in args
if prebuilt: args.remove('--prebuilt-gen')
game_only = '--game-only' in args
if game_only: args.remove('--game-only')
rt_include = opt('--rt-include')     # game-only: the directory holding rt.h (a stable path per rt.h hash, not a tree)
gen_opt = opt('--gen-opt')
gen_pool = int(opt('--gen-pool', '2'))
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(opt('--root', os.path.join(here, '..', '..')))
gen = os.path.abspath(opt('--gen'))
out = os.path.abspath(opt('--out'))
obj = os.path.abspath(opt('--obj'))
vcpkg = os.path.abspath(opt('--vcpkg', '.'))                  # x64-windows-static-md: opus
vksdk = os.path.abspath(opt('--vksdk', os.environ.get('VULKAN_SDK', '')))   # Vulkan SDK: headers, vulkan-1, shaderc_combined
CC, CXX = os.environ.get('CC', 'clang'), os.environ.get('CXX', 'clang++')

def fw(p): return p.replace('\\', '/')
def esc(p):
    if game_only: p = os.path.relpath(p, out)
    return fw(p).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')

RT = fw(os.path.join(root, 'runtime'))
rt_cpp = sorted(glob.glob(os.path.join(RT, '*.cpp')))
nvn_c = [s for s in sorted(glob.glob(os.path.join(RT, 'nvn', '*.c'))) if os.path.basename(s) != 'backend_null.c']
vk_c = sorted(glob.glob(os.path.join(RT, 'nvn', 'vulkan', '*.c')))
mw_c = sorted(glob.glob(os.path.join(RT, 'nvn', 'maxwell', '*.c')))
sdk_dir = os.path.join(os.path.dirname(gen), 'sdk')
VL = fw(os.path.join(vcpkg, 'lib')); SI = fw(os.path.join(vksdk, 'Include')); SL = fw(os.path.join(vksdk, 'Lib'))
os.makedirs(out, exist_ok=True)

# -fdenormal-fp-math=dynamic: with TOTK_FPCR=1 the guest's FPCR.FZ sets MXCSR.DAZ/FTZ at run time; in clang's default
# (IEEE) mode it may turn an FP compare with zero or a denormal test into integer bit tests, which DAZ disagrees with.
# -fno-math-errno: with errno semantics clang calls the UCRT's sqrtf / sqrt instead of sqrtss / sqrtps (fsqrt: 4
# calls per 4s op); no generated or runtime code reads errno after a math call.
common = '--target=x86_64-pc-windows-msvc -O2 -march=x86-64-v3 -fdenormal-fp-math=dynamic -fno-math-errno -Wno-everything -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -D_USE_MATH_DEFINES -gline-tables-only -fms-runtime-lib=dll'
cxxflags = f'-std=c++20 {common} -fno-exceptions -I{fw(rt_include) if game_only and rt_include else RT}'
cflags = f'-std=gnu17 {common} -I{RT}/nvn -I{RT}/nvn/shim'
# (-Werror=implicit-function-declaration: under -Wno-everything C took an undeclared function as returning int, and
# latch.c's vk_glsl_to_spirv / vk_module pointers were cut to 32 bits: a driver crash, 4a90bee0; the Vulkan TUs have none)
cvkflags = f'$cflags -Werror=implicit-function-declaration -I{SI} -include {RT}/nvn/shim/compat.h'

with open(os.path.join(out, 'build.ninja'), 'w', newline='\n') as f:
    f.write(f'cxx = {CXX}\ncc_ = {CC}\ncxxflags = {cxxflags}\ncflags = {cflags}\ncvkflags = {cvkflags}\n')
    f.write(f'pool gen\n  depth = {gen_pool}\n')
    f.write('rule cxx\n  command = $cxx $cxxflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CXX $out\n')
    gflags = cxxflags.replace(' -O2 ', f' {gen_opt} ') if gen_opt else cxxflags
    # clang 22 crashed in RegisterCoalescer on some generated functions with SLP + AVX2 (upstream note): retry without SLP
    f.write(f'rule gen\n  command = cmd /c "$cxx {gflags} -MD -MF $out.d -c $in -o $out || $cxx {gflags} -fno-slp-vectorize -MD -MF $out.d -c $in -o $out"\n'
            '  depfile = $out.d\n  deps = gcc\n  description = GEN $out\n  pool = gen\n')
    f.write('rule c\n  command = $cc_ $cflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CC $out\n')
    f.write('rule cvk\n  command = $cc_ $cvkflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CVK $out\n')
    gen_objs, objs = [], []
    def gen_edge(sub, src):
        o = os.path.join(obj, sub, os.path.splitext(os.path.basename(src))[0] + '.o')
        if not prebuilt: f.write(f'build {esc(o)}: gen {esc(src)}\n')
        gen_objs.append(o)
    for src in sorted(glob.glob(os.path.join(gen, '*.cpp'))): gen_edge('gen', src)
    for src in sorted(glob.glob(os.path.join(sdk_dir, '*.cpp'))): gen_edge('sdk', src)
    if game_only:
        f.write('build objs-gen: phony ' + ' '.join(esc(o) for o in gen_objs) + '\n')
        f.write('default objs-gen\n')
        print('game objects |', len(gen_objs), 'generated objects ->', out)
        sys.exit(0)
    for src in rt_cpp:
        o = os.path.join(out, 'rt', os.path.splitext(os.path.basename(src))[0] + '.o')
        f.write(f'build {esc(o)}: cxx {esc(src)}\n'); objs.append(o)
    for src in nvn_c:
        o = os.path.join(out, 'nvn', os.path.splitext(os.path.basename(src))[0] + '.o')
        f.write(f'build {esc(o)}: c {esc(src)}\n'); objs.append(o)
    for src in vk_c + mw_c:
        sub = os.path.basename(os.path.dirname(src))
        o = os.path.join(out, 'nvn', sub, os.path.splitext(os.path.basename(src))[0] + '.o')
        f.write(f'build {esc(o)}: cvk {esc(src)}\n'); objs.append(o)
    f.write('build objs-gen: phony ' + ' '.join(esc(o) for o in gen_objs) + '\n')
    f.write('build objs-rt: phony ' + ' '.join(esc(o) for o in objs) + '\n')
    # NVIDIA NGX (DLSS, runtime/nvn/vulkan/ngx.c): the SDK's static loader for the dynamic CRT; the feature DLLs load at run time
    ngx_lib = fw(os.path.join(root, 'third_party', 'dlss', 'lib', 'nvsdk_ngx_d.lib'))
    libs = (f'{SL}/vulkan-1.lib {SL}/shaderc_combined.lib {VL}/opus.lib {ngx_lib} '
            + ' '.join('-l' + l for l in 'user32 gdi32 kernel32 ole32 oleaut32 uuid shell32 advapi32 winmm mfplat mfuuid mf '
                                         'mfreadwrite propsys psapi dbghelp avrt synchronization ntdll'.split())
            + ' -rtlib=compiler-rt')     # __divti3 / __udivti3 for the generated u128 code (clang_rt.builtins)
    f.write('rule link\n  command = $cxx --target=x86_64-pc-windows-msvc -fuse-ld=lld -fms-runtime-lib=dll -Wl,/STACK:8388608 -Wl,/DEBUG -Wl,/MAP:$out.map '
            f'-o $out @$out.rsp {libs}\n  rspfile = $out.rsp\n  rspfile_content = $in\n  description = LINK $out\n')
    exe = os.path.join(out, 'totk.exe')
    f.write(f'build {esc(exe)}: link ' + ' '.join(esc(o) for o in gen_objs + objs) + '\n')
    f.write(f'default {esc(exe)}\n')
print('vulkan |', len(gen_objs), 'generated objects,', len(objs), 'runtime objects ->', out)
