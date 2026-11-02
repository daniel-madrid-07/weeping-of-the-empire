"""Setup's PC check (Builder.cs Preflight): compiles it with test_preflight.cs (Windows' own C# compiler, as
make_setup.py does) and runs it twice: on this PC as it is, then with the Vulkan loader pointed at no driver (the check
must then stop the install with its sentence). No window opens; nothing is installed.
    py tools/builder/gui/test_preflight.py"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_setup import CSC  # noqa: E402

RELEASE = ('static class Release\n{\n    public const string Product = "Preflight Test";\n'
           '    public const string Tag = "test";\n    public const string Repo = "none/none";\n'
           '    public const string LlvmVersion = "0";\n    public const string VulkanSha256 = "";\n'
           '    public const string LlvmSha256 = "";\n    public const string VulkanAsset = "setup-vulkan-sdk.zip";\n'
           '    public const string LlvmAsset = "setup-llvm.zip";\n    public const string VulkanTag = "v9.9.8";\n'
           '    public const string LlvmTag = "v9.9.8";\n}\n')


def check_assets():
    """tools/builder/assets.json, the one list of pinned release assets: well formed, what builder.py and the Setup read"""
    import json
    import re
    sys.path.insert(0, os.path.dirname(HERE))
    import builder
    doc = json.load(open(os.path.join(os.path.dirname(HERE), 'assets.json'), encoding='utf-8'))
    bad = [x for x in doc['assets'] if set(x) != {'used_by', 'tag', 'name', 'size', 'sha256'}
           or not re.fullmatch(r'[0-9a-f]{64}', x['sha256']) or not re.fullmatch(r'v\d+(\.\d+){2,3}', x['tag'])
           or not isinstance(x['size'], int) or x['size'] <= 0]
    seen = [(x['tag'], x['name']) for x in doc['assets']]
    used = {x['used_by'] for x in doc['assets']}
    ok = (not bad and len(seen) == len(set(seen)) and len(used) == len(doc['assets'])
          and {'setup:Llvm', 'setup:Vulkan'} <= used
          and all(getattr(builder, k[8:]) == builder.pinned_asset(k) for k in used if k.startswith('builder:')))
    print(f'assets.json: {len(doc["assets"])} pins {"ok" if ok else "FAIL"} {bad}')
    return 0 if ok else 1


def main():
    if check_assets():
        return 1
    with tempfile.TemporaryDirectory(prefix='preflight') as tmp:
        rel, exe = os.path.join(tmp, 'Release.cs'), os.path.join(tmp, 'test_preflight.exe')
        with open(rel, 'w', encoding='utf-8') as f:
            f.write(RELEASE)
        r = subprocess.run([CSC, '/nologo', '/target:exe', '/main:PreflightTest', f'/out:{exe}',
                            '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.IO.Compression.dll',
                            '/r:System.IO.Compression.FileSystem.dll', os.path.join(HERE, 'Builder.cs'), rel,
                            os.path.join(HERE, 'test_preflight.cs')], capture_output=True, text=True)
        if r.returncode:
            sys.exit('csc failed:\n' + r.stdout + r.stderr)
        missing = os.path.join(tmp, 'no-driver.json')
        novk = dict(os.environ, VK_DRIVER_FILES=missing, VK_ICD_FILENAMES=missing)
        rc = 0
        for args, env in (([], None), (['novk'], novk)):
            p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=120)
            sys.stdout.write(p.stdout + p.stderr)
            rc |= p.returncode
    print('all passed' if rc == 0 else 'FAILED')
    return rc


if __name__ == '__main__':
    sys.exit(main())
