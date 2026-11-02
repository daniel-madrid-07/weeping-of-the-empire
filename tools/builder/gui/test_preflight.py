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
    # Setup's disk sizes are builder.py's (v0.1.8: builder.py read the data in the dump, Setup still asked for romfs.bin)
    cs = open(os.path.join(HERE, 'Builder.cs'), encoding='utf-8').read()
    sizes = dict(re.findall(r'(\w+Gb) = (\d+)', re.search(r'const int FirstRunGb[^;]*;', cs).group(0)))
    want = {'FirstRunGb': builder.FIRST_RUN_GB, 'CopyRomfsGb': builder.COPY_ROMFS_GB, 'ResumeGb': builder.RESUME_GB,
            'ToolsGb': builder.TOOLS_GB, 'UpdateUnpackGb': builder.UPDATE_UNPACK_GB}
    same = {k: int(v) for k, v in sizes.items()} == want
    print(f'disk sizes: Setup {sizes} vs builder.py {"ok" if same else "FAIL " + str(want)}')
    # a dump in OneDrive is copied out (Setup's DumpDriveCanGo says the same: test_preflight.cs)
    env = {v: os.environ.pop(v, None) for v in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial')}
    os.environ['OneDrive'] = r'C:\Users\x\OneDrive'
    try:
        od = (builder.dump_drive(r'C:\Users\x\OneDrive\Desktop\TotK.nsp') == 'OneDrive'
              and not builder.in_onedrive(r'C:\Users\x\OneDriveBackup\TotK.nsp'))
    finally:
        for k, v in env.items():
            os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
    print(f'onedrive dump: {"ok" if od else "FAIL"}')
    return 0 if ok and same and od else 1


def check_inputs(exe, tmp):
    """Setup's Inputs.Check (the dump / update / keys headers, before any download) against builder.py's check_keys +
    check_dump (what runs again before step 1) on synthetic dumps (pipeline/tests/synthetic.py: test keys, no game
    data): the same sentence for each case, empty only for the good ones, never a key value in the output. Needs the
    pipeline's packages (cryptography, lz4): run with the repo's venv Python."""
    import contextlib
    import io
    import struct
    try:
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(HERE))))
        sys.path.insert(0, os.path.dirname(HERE))
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        from pipeline.tests import synthetic as syn
        import builder
    except ImportError as e:
        print(f'inputs: FAIL, run with the venv Python ({e})')
        return 1

    def xts(data, enc):
        out = b''
        for i in range(len(data) // 512):
            c = Cipher(algorithms.AES(syn.HEADER), modes.XTS(i.to_bytes(16, 'big')))
            c = c.encryptor() if enc else c.decryptor()
            out += c.update(data[i * 512:(i + 1) * 512]) + c.finalize()
        return out

    def patched(nca, fn):   # (an NCA whose decrypted header fn changes, encrypted again)
        hdr = bytearray(xts(nca[:0xc00], False))
        fn(hdr)
        return xts(bytes(hdr), True) + nca[0xc00:]

    UPDATE, DLC, OTHER = syn.TITLE | 0x800, syn.TITLE + 0x1001, 0x01006A800016E000
    meta_update = syn.nca(True, title=UPDATE, content=1)
    update_files = [('program.nca', syn.nca(True, exefs_crypt=4)), ('meta.cnmt.nca', meta_update), ('common.tik', syn.ticket())]
    gen5 = patched(syn.nca(), lambda h: h.__setitem__(0x220, 5))   # (keys older than the dump's key generation)
    fs_bad = patched(syn.nca(), lambda h: h.__setitem__(0x410, h[0x410] ^ 1))
    size_bad = patched(syn.nca(), lambda h: struct.pack_into('<Q', h, 0x208, 0x123400))
    images = {
        'base.nsp': syn.game_image('nsp'), 'base-managed.nsp': syn.game_image('nsp', True), 'base.xci': syn.game_image('xci'),
        'cut.nsp': syn.game_image('nsp')[:-700], 'junk.nsp': b'\x00junk' * 100,
        'cut.xci': syn.game_image('xci')[:-700], 'update-as-base.xci': syn.game_image('xci', title=UPDATE),
        'bad-root.xci': syn.game_image('xci')[:0x130] + struct.pack('<Q', 1 << 40) + syn.game_image('xci')[0x138:],
        'update-as-base.nsp': syn.game_image('nsp', title=UPDATE), 'dlc.nsp': syn.game_image('nsp', title=DLC),
        'other.nsp': syn.game_image('nsp', title=OTHER), 'bktr.nsp': syn.game_image('nsp', exefs_crypt=4),
        'no-ticket.nsp': syn.partition([('program.nca', syn.nca(True)), ('control.nca', syn.nca(True, content=2))]),
        'personal.nsp': syn.partition([('program.nca', syn.nca(True)), ('p.tik', syn.ticket(personalized=True))]),
        'gen5.nsp': syn.partition([('program.nca', gen5)]), 'fs-bad.nsp': syn.partition([('program.nca', fs_bad)]),
        'size-bad.nsp': syn.partition([('program.nca', size_bad)]),
        'update.nsp': syn.partition(update_files), 'update-no-meta.nsp': syn.partition(update_files[::2]),
    }
    good = syn.keys_text()
    keyfiles = {'prod.keys': good, 'short.keys': 'header_key = ' + syn.HEADER.hex() + '\n',
                'badhex.keys': good + 'titlekek_01 = zz\n',
                'other.keys': good.replace(syn.HEADER.hex(), bytes(32).hex()),
                'comment.keys': '# dumped by a tool\n' + good.replace('\n', '  # note\n')}
    for name, data in images.items():
        with open(os.path.join(tmp, name), 'wb') as f:
            f.write(data)
    for name, text in keyfiles.items():
        with open(os.path.join(tmp, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    # (dump, keys, update, good?)
    cases = [('base.nsp', 'prod.keys', None, True), ('base-managed.nsp', 'prod.keys', None, True),
             ('base.xci', 'prod.keys', None, True), ('base.nsp', 'comment.keys', None, True),
             ('base.nsp', 'prod.keys', 'update.nsp', True), ('base-managed.nsp', 'prod.keys', 'update.nsp', True)]
    cases += [(d, 'prod.keys', None, False) for d in images if d not in ('base.nsp', 'base-managed.nsp', 'base.xci', 'update.nsp')]
    cases += [('base.nsp', k, None, False) for k in ('short.keys', 'badhex.keys', 'other.keys')]
    cases += [('base.nsp', 'prod.keys', u, False) for u in ('base.nsp', 'update-no-meta.nsp', 'cut.nsp', 'personal.nsp')]
    cases += [('update.nsp', 'prod.keys', 'base.nsp', False)]

    def python(dump, keys, update):
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                builder.check_keys(keys)
        except SystemExit:
            return out.getvalue().strip().replace('[Builder] ERROR: ', '')
        return builder.check_dump(dump, keys, update)

    p = lambda n: n and os.path.join(tmp, n)   # noqa: E731
    with open(os.path.join(tmp, 'cases.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(f'{p(d)}\t{p(k)}\t{p(u) or "-"}\n' for d, k, u, _ in cases)
    r = subprocess.run([exe, 'inputs', os.path.join(tmp, 'cases.txt')], capture_output=True, text=True, timeout=120)
    got = [ln.split('\t', 1)[1] for ln in r.stdout.splitlines() if ln.startswith('INPUTS\t')]
    fails = 0 if len(got) == len(cases) else 1
    if fails:
        print(f'inputs: {len(got)} answers for {len(cases)} cases\n{r.stdout}{r.stderr}')
    for (d, k, u, ok), cs in zip(cases, got):
        py = python(p(d), p(k), p(u))
        right = cs == py and (cs == '') == ok and not any(s.hex() in cs.lower() for s in syn.SECRETS)
        fails += not right
        print(f'inputs: {d} + {k}' + (f' + update {u}' if u else '') + f': {"ok" if right else "FAIL"} '
              f'{cs[:110] if right else f"C# {cs!r} / Python {py!r}"}')
    print(f'inputs: {len(cases)} cases, {"all agree" if not fails else f"{fails} FAIL"}')
    return 1 if fails else 0


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
        wine = dict(os.environ, SETUP_WINE='1', HOME='/home/nobody')   # (Setup's Wine paths, forced on Windows)
        wine.pop('STEAM_COMPAT_DATA_PATH', None)
        proton = dict(wine, STEAM_COMPAT_DATA_PATH='/home/nobody/compatdata/1')
        for args, env in (([], None), (['novk'], novk), (['wine'], wine), (['proton'], proton)):
            p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=120)
            sys.stdout.write(p.stdout + p.stderr)
            rc |= p.returncode
        rc |= check_inputs(exe, tmp)
    print('all passed' if rc == 0 else 'FAILED')
    return rc


if __name__ == '__main__':
    sys.exit(main())
