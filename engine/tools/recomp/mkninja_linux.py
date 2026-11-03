"""Write build.ninja for the native Linux x86-64 build: generated TUs + runtime + NVN/Vulkan backend.

usage: python3 mkninja_linux.py --gen DIR --out DIR --obj DIR --deps DIR [--sysroot DIR] [--gen-opt -O1] [--gen-pool N]
                                [--prebuilt-gen | --game-only] [--null] [--pgo gen|use:FILE]

The same interface and source sets as mkninja_win.py (tools/engine/build.py drives either one; linux-port, 2026-11-02),
with the Linux side: runtime/linux (the Win32 subset over POSIX, plat_posix.cpp) and SDL3 itself instead of
runtime/nvn/shim, linux/shim_sdl3.c instead of nvn/vulkan/shim_win.c.
  --deps      engine/tools/linux/build_deps.sh's prefix: sdl3-install/, opus-install/, vpx-install/, vulkan-sdk/
              (headers + libshaderc_combined.a of the Vulkan SDK release the Windows build uses)
  --sysroot   a glibc / libstdc++ sysroot to compile and link against (a player's Setup: tools/linux/sysroot.sh); none
              (the default): the system's, i.e. tools/linux/env.sh's Ubuntu 22.04 root
  --null      null GPU backend (nvn/backend_null.c: presents nothing, no Vulkan / shaderc): headless smoke tests
  --pgo gen   instrumented build (-fprofile-generate=pgo, -DTOTK_PGO_GEN); --pgo use:FILE builds with that profile
  --game-only, --prebuilt-gen, --rt-include, --rt-exclude, --rt-extra, --define, --exe, --gen-opt, --gen-pool: as in
              mkninja_win.py (default --exe totk)
  env CC / CXX: the clang to use (default clang / clang++: the pinned LLVM on PATH)
"""
import glob, os, sys

args = sys.argv[1:]
def opt(name, default=None):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
def flag(name):
    if name in args: args.remove(name); return True
    return False
null = flag('--null')
prebuilt = flag('--prebuilt-gen')
game_only = flag('--game-only')
rt_include = opt('--rt-include')
gen_opt = opt('--gen-opt')
rt_exclude = [p for p in (opt('--rt-exclude') or '').split(',') if p]
rt_extra = [p for p in (opt('--rt-extra') or '').split(',') if p]
defines = [d for d in (opt('--define') or '').split(',') if d]
exe_name = opt('--exe', 'totk')
gen_pool = int(opt('--gen-pool', '2'))
pgo = opt('--pgo', '')
sysroot = opt('--sysroot')
for ignored in ('--vcpkg', '--vksdk'):   # (mkninja_win.py's library options: build.py passes them on both targets)
    opt(ignored)
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(opt('--root', os.path.join(here, '..', '..')))
gen = os.path.abspath(opt('--gen'))
out = os.path.abspath(opt('--out'))
obj = os.path.abspath(opt('--obj'))
deps = os.path.abspath(opt('--deps', os.path.join(root, '..', 'linux-deps')))
if args: sys.exit(f'mkninja_linux.py: unknown arguments {args}')
CC, CXX = os.environ.get('CC', 'clang'), os.environ.get('CXX', 'clang++')

def esc(p):
    if game_only: p = os.path.relpath(p, out)
    return p.replace('$', '$$').replace(' ', '$ ').replace(':', '$:')

RT = os.path.join(root, 'runtime')
rt_cpp = sorted(glob.glob(os.path.join(RT, '*.cpp'))) + [os.path.join(RT, 'linux', 'plat_posix.cpp')]
nvn_c = sorted(glob.glob(os.path.join(RT, 'nvn', '*.c')))
vk_c, mw_c = [], []
if null:
    # all of nvn/*.c including backend_null.c; the vulkan/ and maxwell/ trees are not built. runtime/linux/null_stubs.c supplies the
    # few symbols the runtime takes from the Vulkan backend (totk_hooks.c / exec.c / device.c)
    nvn_c.append(os.path.join(RT, 'linux', 'null_stubs.c'))
else:
    nvn_c = [s for s in nvn_c if os.path.basename(s) != 'backend_null.c']
    vk_c = [s for s in sorted(glob.glob(os.path.join(RT, 'nvn', 'vulkan', '*.c'))) if os.path.basename(s) != 'shim_win.c']
    vk_c.append(os.path.join(RT, 'linux', 'shim_sdl3.c'))
    mw_c = sorted(glob.glob(os.path.join(RT, 'nvn', 'maxwell', '*.c')))
if rt_exclude or rt_extra:   # another game: leave out its TotK-only sources, add its own (runtime-relative paths)
    import fnmatch
    def kept(src): return not any(fnmatch.fnmatchcase(os.path.relpath(src, RT).replace(os.sep, '/'), p) for p in rt_exclude)
    rt_cpp, nvn_c, vk_c, mw_c = ([s for s in l if kept(s)] for l in (rt_cpp, nvn_c, vk_c, mw_c))
    for e in rt_extra:
        src = os.path.join(RT, e)
        (rt_cpp if src.endswith('.cpp') else nvn_c).append(src)
sdk_dir = os.path.join(os.path.dirname(gen), 'sdk')
SDL, OPUS, VPX, VKSDK = (os.path.join(deps, d) for d in ('sdl3-install', 'opus-install', 'vpx-install', 'vulkan-sdk'))
if not game_only and not null and not os.path.isfile(os.path.join(VKSDK, 'lib', 'libshaderc_combined.a')):
    sys.exit(f'missing {VKSDK}/lib/libshaderc_combined.a: run engine/tools/linux/build_deps.sh {deps}')
os.makedirs(out, exist_ok=True)

# The game objects' flags are mkninja_win.py's minus the MSVC ABI ones (-fdenormal-fp-math=dynamic, -fno-math-errno:
# see there). No -mtune: generic, as on Windows. -fno-pic: the exe is linked -no-pie (guest code calls absolute addresses
# through tables; a PIE costs a GOT load per call).
common = '-O2 -march=x86-64-v3 -fdenormal-fp-math=dynamic -fno-math-errno -fno-pic -pthread -Wno-everything -D_USE_MATH_DEFINES -gline-tables-only'
if sysroot: common += f' --sysroot={os.path.abspath(sysroot)}'
if defines: common += ''.join(' -D' + d for d in defines)
pgo_cc = pgo_link = ''
if pgo == 'gen':
    pgo_cc, pgo_link = ' -fprofile-generate=pgo -DTOTK_PGO_GEN', ' -fprofile-generate=pgo'
elif pgo.startswith('use:'):
    pgo_cc = f' -fprofile-use={pgo[4:]} -mllvm -pgso=false'
elif pgo:
    sys.exit(f'--pgo {pgo}: gen or use:<profdata>')
common += pgo_cc
cxxflags = f'-std=c++20 {common} -fno-exceptions -I{rt_include if game_only and rt_include else RT}'
# the runtime's C++: the game objects' flags (build.py's small_link edits cxxflags for both, as on Windows) plus
# runtime/linux (windows.h & co.; no name there clashes with runtime/'s) and SDL3
rtcxxflags = f'$cxxflags -I{RT}/linux -I{SDL}/include -I{OPUS}/include -I{VPX}/include -include {RT}/linux/crt_compat.h'
cflags = f'-std=gnu17 {common} -D_GNU_SOURCE -I{RT}/linux -I{RT}/nvn -I{SDL}/include -include {RT}/linux/crt_compat.h'   # (_GNU_SOURCE: as C++ has it)
# (-Werror=implicit-function-declaration: mkninja_win.py's reason, latch.c's pointers cut to 32 bits)
cvkflags = (f'$cflags -Werror=implicit-function-declaration -I{VKSDK}/include -I{RT}/linux/vmashim '
            f'-include {RT}/linux/compat_posix.h')

with open(os.path.join(out, 'build.ninja'), 'w', newline='\n') as f:
    # (--game-only: no runtime line, so the game rule's text, which build.py's key hashes, names no source tree)
    f.write(f'cxx = {CXX}\ncc_ = {CC}\ncxxflags = {cxxflags}\n' + ('' if game_only else f'rtcxxflags = {rtcxxflags}\n') +
            f'cflags = {cflags}\ncvkflags = {cvkflags}\n')
    f.write(f'pool gen\n  depth = {gen_pool}\n')
    f.write('rule cxx\n  command = $cxx $rtcxxflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CXX $out\n')
    gflags = cxxflags.replace(' -O2 ', f' {gen_opt} ') if gen_opt else cxxflags
    # clang 22 crashed in RegisterCoalescer on some generated functions with SLP + AVX2 (upstream note): retry without SLP
    f.write(f"rule gen\n  command = sh -c '$cxx {gflags} -MD -MF $out.d -c $in -o $out || $cxx {gflags} -fno-slp-vectorize -MD -MF $out.d -c $in -o $out'\n"
            '  depfile = $out.d\n  deps = gcc\n  description = GEN $out\n  pool = gen\n')
    f.write('rule c\n  command = $cc_ $cflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CC $out\n')
    f.write('rule cvk\n  command = $cc_ $cvkflags -MD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CVK $out\n')
    gen_objs, objs = [], []
    rt_extra_set = {os.path.join(RT, e) for e in rt_extra}   # (game_<name>.o: never clashes with a runtime/ object)
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
        if src in rt_extra_set: o = os.path.join(out, 'rt', 'game_' + os.path.splitext(os.path.basename(src))[0] + '.o')
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
    # static: Opus, libvpx's VP9 decoder, shaderc (the SDK's combined archive: glslang + SPIRV-Tools inside), libstdc++,
    # libgcc; shared: SDL3 (next to the exe as lib/libSDL3.so.0), libvulkan (the system's loader), glibc
    libs = f'-L{SDL}/lib -lSDL3 {OPUS}/lib/libopus.a {VPX}/lib/libvpx.a'
    if not null: libs += f' -lvulkan {VKSDK}/lib/libshaderc_combined.a'
    libs += ' -ldl -lpthread -lm'
    link = '$cxx' + pgo_link + (f' --sysroot={os.path.abspath(sysroot)}' if sysroot else '')
    # --build-id=sha1 over the output (with -ffile-prefix-map-free, path-stable inputs): the same objects relink to the
    # same bytes, the Linux twin of lld-link /Brepro
    f.write(f'rule link\n  command = {link} -fuse-ld=lld -no-pie -static-libstdc++ -static-libgcc -pthread -Wl,-z,noexecstack '
            "-Wl,--build-id=sha1 -Wl,-Map=$out.map -Wl,-rpath,'$$ORIGIN/lib' -o $out @$out.rsp " + libs + '\n'
            '  rspfile = $out.rsp\n  rspfile_content = $in\n  description = LINK $out\n')
    exe = os.path.join(out, exe_name)
    f.write(f'build {esc(exe)}: link ' + ' '.join(esc(o) for o in gen_objs + objs) + '\n')
    f.write(f'default {esc(exe)}\n')
print('backend:', 'null' if null else 'vulkan', '|', len(gen_objs), 'generated objects,', len(objs), 'runtime objects ->', out)
