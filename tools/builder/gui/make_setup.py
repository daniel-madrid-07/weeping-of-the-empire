"""Make the Setup exe (for a GitHub release) or Builder.exe (to keep in a clone), from Builder.cs, with the C# compiler
that ships with Windows (.NET Framework 4.x).

    py tools/builder/gui/make_setup.py --tag v1.0.0 [--repo OWNER/NAME] [--product NAME] [--out DIR]
        -> <out>/<Product>-Setup-<tag>.exe: builds the repository at that tag on the player's PC, and next to it the
           release assets it downloads, as tools/builder/assets.json pins them (name, the release that carries it,
           sha256: made from this PC's tools, the release's versions, checked; the same tools make the same bytes, and
           a zip that differs from its pin stops here unless --repin pins it to this tag):
             setup-llvm.zip        the part of LLVM the build runs (clang, lld-link, llvm-lib, llvm-profdata, llvm-rc,
                                   clang's headers and runtime libraries; Apache-2.0 WITH LLVM-exception, its license
                                   goes with it)
             setup-vulkan-sdk.zip  the Vulkan headers and libraries the build links (vulkan-1.lib, shaderc_combined.lib)
           Besides those a player's PC downloads Python, MinGit, the Python packages and, without Visual Studio,
           Microsoft's C++ libraries
    py tools/builder/gui/make_setup.py --builder
        -> Builder.exe in the repository root (builds the clone it sits in with the PC's own tools)

The tag must exist on the public repository before the exe is handed out: Setup fetches exactly that tag.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
from builder import RELEASE_TOOLS  # noqa: E402

PUBLIC_REPO = 'daniel-madrid-07/weeping-of-the-empire'
# the Setup's release assets: pinned in tools/builder/assets.json (used_by 'setup:<Key>', the one list of every pinned
# asset; installer's release_publish.py checks a draft against it), baked into Release.cs as <Key>Asset / Tag / Sha256
ASSETS_JSON = os.path.join(os.path.dirname(HERE), 'assets.json')
SETUP_ASSETS = ('Vulkan', 'Llvm')
PRODUCT = 'Weeping of the Empire'
CSC = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe')
# what the engine build takes from the Vulkan SDK (engine/tools/recomp/mkninja_win.py: -I<sdk>/Include,
# <sdk>/Lib/vulkan-1.lib and shaderc_combined.lib): headers by folder (vulkan/: the C headers only), two libraries
VK_HEADERS = {'vulkan': ('.h',), 'vk_video': ('.h',), 'shaderc': ('.h', '.hpp'), 'spirv-tools': ('.h', '.hpp')}
VK_LIBS = ('vulkan-1.lib', 'shaderc_combined.lib')
VK_NOTICE = """These files come from the LunarG Vulkan SDK {ver} (https://vulkan.lunarg.com): the Khronos Vulkan headers
(Apache-2.0 OR MIT), the Vulkan loader's import library (Apache-2.0) and shaderc_combined.lib (shaderc, glslang,
SPIRV-Tools: Apache-2.0 / BSD-3-Clause / MIT, see LICENSE.txt). Setup unpacks them into its tools folder so the build
can link the game's renderer; they are not installed system-wide.
"""
# The LLVM programs the build runs (clang.exe is also clang-cl.exe and clang++.exe: Setup links the copies) and the
# parts of clang's resource folder it reads: its headers (immintrin.h...) and the builtins / profile runtimes
LLVM_BIN = ('clang.exe', 'lld-link.exe', 'llvm-lib.exe', 'llvm-profdata.exe', 'llvm-rc.exe')
LLVM_RES_LIBS = ('clang_rt.builtins-x86_64.lib', 'clang_rt.profile-x86_64.lib')
LLVM_NOTICE = """LLVM {ver} (https://llvm.org), the parts the build runs, as installed by the official LLVM {ver} release for
Windows x64: the Apache License v2.0 with LLVM Exceptions, LICENSE.TXT. clang-cl.exe and clang++.exe are copies of
clang.exe that Setup makes when it unpacks this.
"""


class Zip:
    """a zip with fixed dates and order: the same inputs make the same bytes, so Setup can tell it unpacked them"""
    def __init__(self, path):
        self.z = zipfile.ZipFile(path, 'w')

    def put(self, name, data):
        self.z.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED, 9)

    def put_file(self, name, path):
        with open(path, 'rb') as f:
            self.put(name, f.read())

    def put_tree(self, prefix, top, keep=lambda rel: True):
        for root, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                rel = os.path.relpath(os.path.join(root, f), top).replace('\\', '/')
                if keep(rel):
                    self.put_file(f'{prefix}/{rel}', os.path.join(root, f))

    def close(self):
        self.z.close()


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def vulkan_zip(dest):
    sdk = os.environ.get('VULKAN_SDK', '')
    ver = os.path.basename(sdk.rstrip('\\/'))
    if not all(os.path.isfile(os.path.join(sdk, 'Lib', f)) for f in VK_LIBS):
        sys.exit('VULKAN_SDK must point at a Vulkan SDK with Lib\\shaderc_combined.lib (the one releases are built with)')
    if ver != RELEASE_TOOLS['vulkan_sdk']:
        print(f'note: Vulkan SDK {ver}, the releases use {RELEASE_TOOLS["vulkan_sdk"]}')
    z = Zip(dest)
    for sub, exts in VK_HEADERS.items():
        z.put_tree(f'Include/{sub}', os.path.join(sdk, 'Include', sub),
                   lambda rel: rel.endswith(exts) and (sub != 'vulkan' or '/' not in rel))
    for f in VK_LIBS:
        z.put_file('Lib/' + f, os.path.join(sdk, 'Lib', f))
    lic = os.path.join(sdk, 'Licenses', 'LICENSE.txt')
    if os.path.isfile(lic):
        z.put_file('LICENSE.txt', lic)
    z.put('NOTICE.txt', VK_NOTICE.format(ver=ver))
    z.close()
    return sha256(dest)


def llvm_zip(dest):
    llvm = os.environ.get('NXRECOMP_LLVM_DIR') or r'C:\Program Files\LLVM'
    ver = RELEASE_TOOLS['clang']
    out = subprocess.run([os.path.join(llvm, 'bin', 'clang.exe'), '--version'], capture_output=True, text=True).stdout
    if f'clang version {ver} ' not in out:
        sys.exit(f'{llvm} is not LLVM {ver} (the release\'s; NXRECOMP_LLVM_DIR names another): {out[:80]!r}')
    res = os.path.join(llvm, 'lib', 'clang', ver.split('.')[0])
    z = Zip(dest)
    for f in LLVM_BIN:
        z.put_file('bin/' + f, os.path.join(llvm, 'bin', f))
    rel_res = 'lib/clang/' + ver.split('.')[0]
    z.put_tree(rel_res + '/include', os.path.join(res, 'include'))
    for f in LLVM_RES_LIBS:
        z.put_file(f'{rel_res}/lib/windows/{f}', os.path.join(res, 'lib', 'windows', f))
    z.put_file('LICENSE.TXT', os.path.join(HERE, 'licenses', 'LLVM-LICENSE.txt'))
    z.put('NOTICE.txt', LLVM_NOTICE.format(ver=ver))
    z.close()
    return sha256(dest)


def cs_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--tag', help='the public repository tag this Setup builds (e.g. v1.0.0)')
    g.add_argument('--builder', action='store_true', help='Builder.exe for the repository root')
    ap.add_argument('--repo', default=PUBLIC_REPO)
    ap.add_argument('--product', default=PRODUCT)
    ap.add_argument('--out', default=REPO)
    ap.add_argument('--repin', action='store_true',
                    help='the zips differ from assets.json (a new toolset): pin them there to this tag (commit it, then tag)')
    a = ap.parse_args()
    pins = {key: dict(name='', tag='', sha256='') for key in SETUP_ASSETS}
    built = []
    with tempfile.TemporaryDirectory(prefix='make_setup_') as tmp:
        if a.tag:
            # release assets next to the exe (the exe pins them and downloads them from the release that carries them; a
            # copy next to it is used instead): not resources, 89 MB of compressed data in the exe's code section is what
            # antivirus heuristics read as packed (issue #14). The same tools make the same bytes, so the zip built here
            # must be the one assets.json pins (the release that first carried it keeps serving it)
            os.makedirs(a.out, exist_ok=True)
            doc = json.load(open(ASSETS_JSON, encoding='utf-8'))
            for key, make in (('Vulkan', vulkan_zip), ('Llvm', llvm_zip)):
                x = next(x for x in doc['assets'] if x['used_by'] == 'setup:' + key)
                p = os.path.join(a.out, x['name'])
                sha, size = make(p), os.path.getsize(p)
                if sha != x['sha256'] or size != x['size']:
                    if not a.repin:
                        sys.exit(f'{x["name"]} built here (sha256 {sha[:12]}) is not the one assets.json pins to {x["tag"]} '
                                 f'({x["sha256"][:12]}): another toolset. Run with --repin to pin it to {a.tag}, commit '
                                 'tools/builder/assets.json, then tag (that release must carry it)')
                    x.update(tag=a.tag, sha256=sha, size=size)
                    print(f'repinned {x["name"]} to {a.tag} ({sha[:12]}): commit tools/builder/assets.json')
                pins[key] = dict(name=x['name'], tag=x['tag'], sha256=x['sha256'])
                built.append((x['name'], size, x['tag']))
            if a.repin:
                with open(ASSETS_JSON, 'w', encoding='utf-8', newline='\n') as f:
                    json.dump(doc, f, indent=1)
                    f.write('\n')
            exe = os.path.join(a.out, f'{a.product.replace(" ", "-")}-Setup-{a.tag}.exe')
        else:
            exe = os.path.join(REPO, 'Builder.exe')
        rel = os.path.join(tmp, 'Release.cs')
        # the exe's Details (Windows' version resource, csc writes it from these): an exe with none, unsigned, that
        # downloads and starts programs, is what antivirus heuristics flag (public issue #14)
        nums = re.findall(r'\d+', a.tag or '')
        ver = '.'.join((nums + ['0'] * 4)[:4]) if re.fullmatch(r'v?\d+(\.\d+){1,3}', a.tag or '') else '0.0.0.0'
        what = 'Setup' if a.tag else 'Builder'
        attrs = {'AssemblyTitle': f'{a.product} {what}', 'AssemblyProduct': a.product,
                 'AssemblyCompany': f'{a.product} project (open source)',
                 'AssemblyCopyright': f'Open source: github.com/{a.repo}', 'AssemblyVersion': ver,
                 'AssemblyFileVersion': ver, 'AssemblyInformationalVersion': a.tag or 'main'}
        with open(rel, 'w', encoding='utf-8') as f:
            f.write('// written by make_setup.py\n'
                    + ''.join(f'[assembly: System.Reflection.{k}({cs_string(v)})]\n' for k, v in attrs.items())
                    + 'static class Release\n{\n'
                    f'    public const string Product = {cs_string(a.product)};\n'
                    f'    public const string Tag = {cs_string(a.tag or "main")};\n'
                    f'    public const string Repo = {cs_string(a.repo)};\n'
                    f'    public const string LlvmVersion = {cs_string(RELEASE_TOOLS["clang"])};\n'
                    + ''.join(f'    public const string {k}Sha256 = {cs_string(v["sha256"])};\n'
                              f'    public const string {k}Asset = {cs_string(v["name"])};\n'
                              f'    public const string {k}Tag = {cs_string(v["tag"])};\n' for k, v in pins.items()) + '}\n')
        cmd = [CSC, '/nologo', '/target:winexe', '/optimize+', f'/out:{exe}', '/r:System.Windows.Forms.dll',
               '/r:System.Drawing.dll', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll',
               os.path.join(HERE, 'Builder.cs'), rel]
        r = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(r.stdout + r.stderr)
        if r.returncode:
            sys.exit(f'csc failed ({r.returncode})')
    print(f'{exe} ({os.path.getsize(exe) / 2**20:.1f} MB)' +
          ''.join(f', {name} {size / 2**20:.1f} MB (release {tag})' for name, size, tag in built))


if __name__ == '__main__':
    main()
