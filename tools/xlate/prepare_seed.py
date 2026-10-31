"""Offline shader seed: every Maxwell shader of the game, translated to SPIR-V before play.

Finds the shaders the way MacNativeGames does (games/totk/scripts/prepare_shader_seed.py, MIT): every RomFS file that
can hold one (Zstandard with the game's dictionaries, SARC packages, BFSHA/BNSH archives, agl .sharcb packages) and the
game modules. Translates, with nxrecomp-xlate (tools/xlate), the default bind state of every shader found plus every
bind state the community list (refs/MacNativeGames/games/totk/assets/shader_list.tsv) names for it, validates each
SPIR-V with spirv-val, and writes the store gfx/shaders reads (src/store.cpp):

  <output>/variants.jsonl   one canonical JSON line per variant (deps, key, opts, refl, spv, stage, v, warm)
  <output>/spirv.bin        the SPIR-V, concatenated
  <output>/pipelines.jsonl  (vs, ps) variant pairs the list's render pipelines name, with their warm flags
  <output>/failed.jsonl     translations or validations that failed (not in the seed)
  <output>/seed-manifest.json   identity, counts, timings, file hashes
  <output>.inventory/       shaders.jsonl + shaders.bin: the control section and code of every shader found (tests,
                            re-translation without rescanning the RomFS)
Everything is game-derived: it stays in the work folder, never in the repository.

usage: prepare_seed.py [--romfs DIR] [--exefs DIR] [--output DIR] [--jobs N] [--limit N] [--no-list]
"""
import argparse
import base64
import concurrent.futures as cf
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORK = Path(r'C:\nxrecomp-work')
DEFAULT_LIST = REPO / 'refs' / 'MacNativeGames' / 'games' / 'totk' / 'assets' / 'shader_list.tsv'
ZSTD = b'\x28\xb5\x2f\xfd'
YAZ0 = b'Yaz0'
BELOW_NORMAL = 0x00004000
STAGES = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs'}
# Must equal gfx/shaders/src/deps.cpp translator_options() for TotK's device state on a device with depth clip control,
# fragment shader interlock and transform feedback, subgroup size 32 (RTX 5080).
SEED_OPTIONS = {
    'debug': True, 'depth_clip_control': True, 'depth_minus_w': True, 'fragment_shader_interlock': True,
    'origin_upper_left': True, 'reduced': True, 'storage_alignment': 64, 'subgroup_size': 32,
    'transform_feedback': True, 'vs_ballot_lowering': False,
}
MAC_ANSWERS = dict(tex={}, img={}, attr={}, fout={})


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


OPTS = sha(canonical(SEED_OPTIONS).encode())[:12]


def mac_suffix(answers):
    return sha(canonical(answers).encode())[:12]


def set_priority():
    """This process and its children at below-normal priority (machine rule)."""
    if os.name == 'nt':
        k32 = ctypes.windll.kernel32
        k32.SetPriorityClass(k32.GetCurrentProcess(), BELOW_NORMAL)


def free_ram_gb():
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                    ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong),
                    ('ullAvailPageFile', ctypes.c_ulonglong), ('ullTotalVirtual', ctypes.c_ulonglong),
                    ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPhys / (1 << 30)


def wait_for_ram(minimum=6.0):
    waited = False
    while free_ram_gb() < minimum:
        if not waited:
            print(f'waiting: free RAM {free_ram_gb():.1f} GB < {minimum} GB', flush=True)
            waited = True
        time.sleep(5)


# ---------------------------------------------------------------------------------------------------------------
# Containers (ported from MacNativeGames prepare_shader_seed.py)
# ---------------------------------------------------------------------------------------------------------------
def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset+size]


def control_key_len(control):
    """Bytes of a control section the shader key hashes (MacNativeGames ctl_key_len; gfx/shaders cache.cpp)."""
    size = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return size if 0x7cc <= size < 0x880 else 0x880


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian+'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian+'H', data, header+6)[0]
    names = header+12+count*16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian+'4I', checked(data, header+12+i*16, 16))
        off = names+(attribute & 0xffffff)*4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start+lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/'):
            name = name[1:]
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start+lo, hi-lo)
    return result


def bnsh_shaders(data):
    """(key, stage, control, code) for each shader of one BNSH file. Offsets are relative to that BNSH."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count*64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations+i*64+16)[0]
        checked(data, program, 56)
        for stage, relative in (('vs', 8), ('ps', 40), ('cs', 48)):
            offset = struct.unpack_from('<Q', data, program+relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset+8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < 0x7cc:
                continue   # the SDK utility BNSH's older, shorter control layout
            key_len = control_key_len(control)
            if len(control) < key_len:
                continue
            control = control[:key_len]
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            code = code[:length]
            yield sha(control+code), stage, control, code


def sharcb_shaders(data):
    """agl shader packages (.sharcb, magic BAHS): records <u64 code offset, u32 control size, i32 control offset>."""
    if len(data) < 32 or data[:4] != b'BAHS':
        raise ValueError('expected an agl shader package')
    start, pool_size, pool = struct.unpack_from('<3I', data, 20)
    if not 32 <= start <= pool or pool + pool_size > len(data):
        raise ValueError('agl shader package layout outside the file')
    seen = set()
    for size in (b'\x80\x08\x00\x00', b'\x8c\x08\x00\x00', b'\xd1\x07\x00\x00', b'\x48\x08\x00\x00'):
        at = start
        while (at := data.find(size, at, pool)) >= 0:
            record, at = at - 8, at + 4
            if record < start or record % 4:
                continue
            code_off = struct.unpack_from('<Q', data, record)[0]
            control_at = record + struct.unpack_from('<i', data, record+12)[0]
            key_len = control_key_len(data[control_at:control_at+0x880]) if control_at >= start else 0
            if not key_len or not start <= control_at <= pool - key_len or code_off >= pool_size:
                continue
            control = data[control_at:control_at+key_len]
            c1, length = struct.unpack_from('<II', control, 0x700)
            stage = {0: 'vs', 1: 'ps', 5: 'cs'}.get(struct.unpack_from('<I', control, 0x714)[0])
            if not stage or not 0 < length <= pool_size - code_off or c1 > length:
                continue
            code = data[pool+code_off:pool+code_off+length]
            key = sha(control+code)
            if key not in seen:
                seen.add(key)
                yield key, stage, control, code


def plausible(stage, control, code):
    """A real NVN shader: graphics code is the 0x30-byte NVN header, then the shader program header (type 1 for
    vertex, 2 for fragment, in bits 0-4) and instructions. agl's .sharcb records are found by pattern, and a few of
    those hits are not shaders (1 to 288 bytes of 'code', no program header)."""
    length = struct.unpack_from('<I', control, 0x704)[0]
    if stage == 'cs':
        return length >= 8
    if length < 0x30 + 0x50 + 8 or len(code) < 0x34:
        return False
    return struct.unpack_from('<I', code, 0x30)[0] & 0x1f == (2 if stage == 'ps' else 1)


def nso_image(raw):
    """A game module (NSO0) as its decompressed text + ro + data segments, concatenated."""
    import lz4.block
    if raw[:4] != b'NSO0':
        return raw
    flags = struct.unpack_from('<I', raw, 0x0c)[0]
    out = bytearray()
    for i in range(3):
        file_off, _, size, _ = struct.unpack_from('<4I', raw, 0x10 + 0x10*i)
        csize = struct.unpack_from('<I', raw, 0x60 + 4*i)[0]
        seg = raw[file_off:file_off+(csize if flags >> i & 1 else size)]
        out += lz4.block.decompress(seg, uncompressed_size=size) if flags >> i & 1 else seg
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------
# Inventory (worker processes)
# ---------------------------------------------------------------------------------------------------------------
_dicts = None


def _decoder(romfs):
    global _dicts
    import zstandard
    if _dicts is None:
        _dicts = {}
        raw = (Path(romfs)/'Pack'/'ZsDic.pack.zs').read_bytes()
        for member in sarc(zstandard.ZstdDecompressor().decompress(raw, max_output_size=64 << 20)).values():
            d = zstandard.ZstdCompressionDict(member)
            _dicts[d.dict_id()] = d

    def decode(data):
        if data[:4] != ZSTD:
            return data
        params = zstandard.get_frame_parameters(data)
        if params.content_size > 512 << 20:
            raise ValueError('unbounded compressed file')
        d = _dicts.get(params.dict_id) if params.dict_id else None
        if params.dict_id and d is None:
            raise ValueError(f'unknown zstd dictionary {params.dict_id}')
        dec = zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()
        return dec.decompress(data, max_output_size=512 << 20)
    return decode


def _visit(name, data, found, decode, depth=0):
    if data[:4] == b'BAHS':
        for key, stage, control, code in sharcb_shaders(data):
            found.setdefault(key, (stage, control, code, name, True))
    if data[:4] == b'SARC':
        if depth > 4:
            return
        for member, raw in sarc(data).items():
            _visit(name+'/'+member, decode(raw), found, decode, depth+1)
        return
    pos = 0
    while True:
        pos = data.find(b'BNSH\0\0\0\0', pos)
        if pos < 0:
            break
        size = struct.unpack_from('<I', checked(data, pos+28, 4))[0]
        for key, stage, control, code in bnsh_shaders(checked(data, pos, size)):
            found.setdefault(key, (stage, control, code, name, False))
        pos += max(size, 8)


def scan_files(args):
    """Worker: (romfs, [relative paths]) -> ({key: (stage, control, code, source, agl)}, files read, errors)."""
    romfs, names = args
    set_priority()
    decode = _decoder(romfs)
    found, errors, containers = {}, [], 0
    for name in names:
        try:
            p = Path(romfs)/name if not name.startswith('module:') else Path(name[7:])
            raw = p.read_bytes()
            if name.startswith('module:'):
                data = nso_image(raw)
            else:
                if raw[:4] not in (ZSTD, YAZ0, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
                    continue
                if raw[:4] == YAZ0:
                    continue   # Breath of the Wild only
                data = decode(raw)
            containers += 1
            _visit(name, data, found, decode)
        except Exception as e:   # one bad file must not hide the others
            errors.append(f'{name}: {type(e).__name__}: {e}')
    return found, containers, errors


def startup_package(name):
    return (name == 'Lib/NintendoSDK/TextureShader.bnsh' or name.startswith('Pack/Bootup.Nin_NX_NVN.pack.zs') or
            name.startswith('Shader/') and '.zs' in name)


def inventory(romfs, exefs, jobs):
    names = []
    for root, _, files in os.walk(romfs):
        for f in files:
            names.append(os.path.relpath(os.path.join(root, f), romfs).replace('\\', '/'))
    names.sort()
    modules = [f'module:{exefs/m}' for m in ('main', 'subsdk0', 'sdk') if (exefs/m).is_file()] if exefs else []
    print(f'inventory: {len(names)} RomFS files, {len(modules)} modules, {jobs} processes', flush=True)
    # Spread big files: round-robin over the sorted list, in chunks.
    chunks = [names[i::jobs*8] for i in range(jobs*8)] + [[m] for m in modules]
    shaders, startup, errors, containers = {}, set(), [], 0
    t0 = time.monotonic()
    with cf.ProcessPoolExecutor(max_workers=jobs) as pool:
        for i, (found, n, errs) in enumerate(pool.map(scan_files, [(str(romfs), c) for c in chunks])):
            containers += n
            errors += errs
            for key, (stage, control, code, source, agl) in found.items():
                if key not in shaders:
                    shaders[key] = (stage, control, code, source)
                if agl or startup_package(source) or source.startswith('module:'):
                    startup.add(key)
            if (i + 1) % 8 == 0:
                print(f'  {i+1}/{len(chunks)} chunks, {len(shaders)} shaders, {time.monotonic()-t0:.0f} s', flush=True)
    print(f'inventory: {containers} containers read, {len(shaders)} unique shaders ({len(startup)} startup), '
          f'{len(errors)} unreadable files, {time.monotonic()-t0:.0f} s', flush=True)
    return shaders, startup, errors, len(names)


def write_inventory(folder, shaders, startup):
    folder.mkdir(parents=True, exist_ok=True)
    with open(folder/'shaders.bin', 'wb') as b, open(folder/'shaders.jsonl', 'w', newline='\n') as j:
        for key in sorted(shaders):
            stage, control, code, source = shaders[key]
            at = b.tell(); b.write(control); b.write(code)
            j.write(canonical(dict(key=key, stage=stage, source=source, startup=key in startup,
                                   control=[at, len(control)], code=[at+len(control), len(code)])) + '\n')


# ---------------------------------------------------------------------------------------------------------------
# Shader list
# ---------------------------------------------------------------------------------------------------------------
def load_list(path):
    variants, warm, pipelines = {}, {}, []
    if not path or not Path(path).is_file():
        return variants, warm, pipelines
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if f[0] == 'V' and len(f) == 3 and len(f[1]) == 64:
            try:
                variants.setdefault(f[1], []).append(json.loads(f[2]))
            except ValueError:
                pass
        elif f[0] == 'S' and len(f) >= 3:
            warm[f[1][:-2] if f[1].endswith('_b') else f[1]] = f[3] if len(f) > 3 else 'm'
        elif f[0] == 'C' and len(f) >= 2:
            warm.setdefault(f[1][:-2] if f[1].endswith('_b') else f[1], f[2] if len(f) > 2 else 'm')
        elif f[0] == 'R' and len(f) >= 2:
            try:
                parts = bytes.fromhex(f[1]).split(b'\0')
                names = [p.decode('ascii') for p in parts[:2]]
                if all(n.endswith('_b') and len(n) == 79 for n in names):
                    pipelines.append(([n[:-2] for n in names], f[2] if len(f) > 2 else 'm'))
            except (ValueError, UnicodeDecodeError):
                pass
    return variants, warm, pipelines


def answers_from_mac(deps):
    """A MacNativeGames deps object as our translator's answers: depth qualifiers dropped (no SPIR-V effect), texture
    slots given for both NVN namespaces (their numeric keys mix combined and separate slots)."""
    tex = {}
    for k, v in deps.get('tex', {}).items():
        t = v.split('_')[0] if isinstance(v, str) else '2d'
        tex[k] = t
        tex['s' + k] = t
    img = {}
    for k, v in deps.get('img', {}).items():
        if isinstance(v, dict):
            o = {}
            if 'type' in v:
                o['type'] = str(v['type']).split('_')[0]
            if 'fmt' in v:
                o['fmt'] = v['fmt']
            img[k] = o
    return dict(tex=tex, img=img, attr=dict(deps.get('attr', {})), fout=dict(deps.get('fout', {})))


# ---------------------------------------------------------------------------------------------------------------
# Translation
# ---------------------------------------------------------------------------------------------------------------
class Translator:
    def __init__(self, exe, spirv_val, scratch):
        self.exe, self.spirv_val, self.scratch = exe, spirv_val, scratch
        self.proc = None
        self.next_id = 1

    def drop(self):
        """Forget a dead translator process without the 'Exception ignored' noise of flushing its broken pipe."""
        if self.proc:
            for f in (self.proc.stdin, self.proc.stdout):
                try:
                    f.close()
                except OSError:
                    pass
            self.proc.wait()
        self.proc = None

    def _start(self):
        self.proc = subprocess.Popen([str(self.exe), '--service'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, creationflags=BELOW_NORMAL if os.name == 'nt' else 0)

    def translate(self, stage, control, code, answers):
        c1, length = struct.unpack_from('<II', control, 0x700)
        rq = dict(stage=stage, code=base64.b64encode(code[0 if stage == 'cs' else 0x30:length]).decode(),
                  answers=answers, options=SEED_OPTIONS)
        if c1 < length:
            rq['c1'] = base64.b64encode(code[c1:length]).decode()
        if stage == 'cs':
            rq['local'] = list(struct.unpack_from('<III', control, 0x748))
        for attempt in range(2):
            if self.proc is None or self.proc.poll() is not None:
                self._start()
            rq['id'] = self.next_id
            self.next_id += 1
            try:
                self.proc.stdin.write((canonical(rq) + '\n').encode())
                self.proc.stdin.flush()
                line = self.proc.stdout.readline()
            except OSError:
                line = b''
            if line:
                reply = json.loads(line)
                if reply.get('id') != rq['id']:
                    raise RuntimeError('translator reply out of order')
                return reply
            self.drop()   # crashed on this shader: one retry on a fresh process
        return dict(ok=False, error='Crash: translator exited on this shader twice')

    def validate(self, spirv):
        path = Path(self.scratch)/'v.spv'
        path.write_bytes(spirv)
        r = subprocess.run([str(self.spirv_val), '--target-env', 'vulkan1.3', str(path)], capture_output=True, text=True,
                           creationflags=BELOW_NORMAL if os.name == 'nt' else 0)
        return None if r.returncode == 0 else (r.stdout + r.stderr).strip()[:600]

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def remove_tree(path):
    """Deletes a previous seed folder, and nothing that does not look like one."""
    path = Path(path).resolve()
    if len(path.parts) < 3 or not ((path/'seed-manifest.json').is_file() or path.name.endswith('.partial')):
        raise SystemExit(f'refusing to delete {path}: not a shader seed folder')
    shutil.rmtree(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', type=Path, default=WORK/'extracted'/'v100'/'romfs')
    ap.add_argument('--exefs', type=Path, default=WORK/'extracted'/'v100'/'exefs')
    ap.add_argument('--shader-list', type=Path, default=DEFAULT_LIST)
    ap.add_argument('--no-list', action='store_true')
    ap.add_argument('--translator', type=Path, default=WORK/'tools'/'xlate'/'nxrecomp-xlate.exe')
    ap.add_argument('--spirv-val', type=Path, default=Path(os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.357.0'))/'Bin'/'spirv-val.exe')
    ap.add_argument('--output', type=Path, default=WORK/'shader-seed')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--limit', type=int, default=0, help='translate only the first N shaders (development)')
    ap.add_argument('--reuse-inventory', action='store_true', help='read <output>.inventory instead of the RomFS')
    args = ap.parse_args()
    set_priority()
    jobs = max(1, min(args.jobs, 6))
    for p in (args.translator, args.spirv_val):
        if not p.is_file():
            raise SystemExit(f'missing {p}')
    started = time.monotonic()
    wait_for_ram()

    inv_dir = args.output.with_name(args.output.name + '.inventory')
    if args.reuse_inventory and (inv_dir/'shaders.jsonl').is_file():
        shaders, startup, scan_errors = {}, set(), []
        blob = (inv_dir/'shaders.bin').read_bytes()
        for line in (inv_dir/'shaders.jsonl').read_text().splitlines():
            r = json.loads(line)
            c0, cl = r['control']; d0, dl = r['code']
            shaders[r['key']] = (r['stage'], blob[c0:c0+cl], blob[d0:d0+dl], r['source'])
            if r['startup']:
                startup.add(r['key'])
        romfs_files = None
        print(f'inventory reused: {len(shaders)} shaders', flush=True)
    else:
        if not (args.romfs/'Pack'/'ZsDic.pack.zs').is_file():
            raise SystemExit(f'{args.romfs} is not an extracted TotK RomFS (no Pack/ZsDic.pack.zs)')
        shaders, startup, scan_errors, romfs_files = inventory(args.romfs, args.exefs, jobs)
        write_inventory(inv_dir, shaders, startup)
    t_inventory = time.monotonic() - started
    rejected = sorted(k for k, (st, control, code, _) in shaders.items() if not plausible(st, control, code))
    for k in rejected:
        del shaders[k]
        startup.discard(k)
    print(f'{len(rejected)} pattern hits without a shader program header dropped; {len(shaders)} shaders', flush=True)

    listed, warm_rows, pipeline_rows = ({}, {}, []) if args.no_list else load_list(args.shader_list)
    listed_rows = sum(len(v) for v in listed.values())
    listed_found = sum(len(v) for k, v in listed.items() if k in shaders)
    print(f'shader list: {listed_rows} bind states for {len(listed)} shaders, {listed_found} of them for shaders found here; '
          f'{len(warm_rows)} warm rows, {len(pipeline_rows)} render pipelines', flush=True)

    keys = sorted(shaders)
    if args.limit:
        keys = keys[:args.limit]
    # Per shader: the default bind state, then the listed ones (each as Mac named it, for its warm flags).
    plan = []
    for key in keys:
        jobs_for_key = [({'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}, key + '-' + mac_suffix(MAC_ANSWERS))]
        for deps in listed.get(key, []):
            jobs_for_key.append((answers_from_mac(deps), key + '-' + mac_suffix({**MAC_ANSWERS, **deps})))
        plan.append((key, jobs_for_key))

    staging = args.output.with_name(args.output.name + '.partial')
    if staging.exists():
        remove_tree(staging)
    staging.mkdir(parents=True)
    spirv_out = open(staging/'spirv.bin', 'wb')   # written as variants are validated: the SPIR-V never piles up in RAM

    results = {}            # key -> {canonical deps: record}
    failures = []
    default_of = {}         # key -> our variant id for the default bind state
    lock = threading.Lock()
    counters = dict(requests=0, translated=0, duplicates=0, translate_failed=0, invalid=0, ms=0.0)
    t1 = time.monotonic()
    pending = iter(plan)

    def worker():
        scratch = tempfile.mkdtemp(prefix='nxs-seed-')
        tr = Translator(args.translator, args.spirv_val, scratch)
        try:
            while True:
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, jobs_for_key = item
                stage, control, code, source = shaders[key]
                mine = {}
                for answers, mac_name in jobs_for_key:
                    reply = tr.translate(stage, control, code, answers)
                    with lock:
                        counters['requests'] += 1
                        counters['ms'] += reply.get('ms', 0.0)
                    if not reply.get('ok'):
                        with lock:
                            counters['translate_failed'] += 1
                            failures.append(dict(key=key, stage=stage, source=source, answers=answers, error=reply.get('error')))
                        continue
                    deps = canonical(reply['deps'])
                    vid = key + '-' + sha(deps.encode())[:12]
                    if mac_name.endswith(mac_suffix(MAC_ANSWERS)) and answers == {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}:
                        with lock:
                            default_of[key] = vid
                    if deps in mine:
                        with lock:
                            counters['duplicates'] += 1
                        continue
                    spirv = base64.b64decode(reply['spirv'])
                    problem = tr.validate(spirv)
                    if problem:
                        with lock:
                            counters['invalid'] += 1
                            failures.append(dict(key=key, stage=stage, source=source, answers=answers, error='spirv-val: ' + problem))
                        continue
                    with lock:
                        at = spirv_out.tell()
                        spirv_out.write(spirv)
                        counters['translated'] += 1
                    mine[deps] = dict(v=vid, key=key, stage=stage, deps=reply['deps'], refl=reply['refl'], spv=[at, len(spirv)])
                with lock:
                    results[key] = mine
        finally:
            tr.close()
            shutil.rmtree(scratch, ignore_errors=True)

    def progress():
        while not done.wait(15):
            with lock:
                n = len(results)
            el = time.monotonic() - t1
            print(f'  translated {n}/{len(plan)} shaders, {counters["translated"]} variants, '
                  f'{counters["translate_failed"]} failed, {counters["invalid"]} invalid, {el:.0f} s', flush=True)

    done = threading.Event()
    reporter = threading.Thread(target=progress, daemon=True)
    reporter.start()
    threads = [threading.Thread(target=worker) for _ in range(jobs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    done.set()
    t_translate = time.monotonic() - t1

    # Warm flags: the list's S/C rows name MacNativeGames variants, whose suffix hashes the run-time answers (not
    # reproducible here); the flags apply to every variant of that shader key.
    warm_key = {}
    for mac, flags in warm_rows.items():
        warm_key[mac[:64]] = ''.join(sorted(set(warm_key.get(mac[:64], '') + flags)))

    spirv_out.close()
    n_variants = 0
    with open(staging/'variants.jsonl', 'w', newline='\n') as vl:
        for key in sorted(results):
            for deps in sorted(results[key]):
                r = results[key][deps]
                warm = warm_key.get(key, 'm' if key in startup else '')
                vl.write(canonical(dict(deps=r['deps'], key=key, opts=OPTS, refl=r['refl'], spv=r['spv'],
                                        stage=r['stage'], v=r['v'], warm=warm)) + '\n')
                n_variants += 1
    n_pipes = 0
    with open(staging/'pipelines.jsonl', 'w', newline='\n') as pl:
        seen_pairs = set()
        for names, flags in pipeline_rows:
            ours = [default_of.get(n[:64]) for n in names]
            if all(ours) and tuple(ours) not in seen_pairs:
                seen_pairs.add(tuple(ours))
                pl.write(canonical(dict(stages=ours, warm=flags)) + '\n')
                n_pipes += 1
    with open(staging/'failed.jsonl', 'w', newline='\n') as fl:
        for f in failures:
            fl.write(canonical(f) + '\n')
    build_info = args.translator.with_name('build-info.json')
    by_stage = {}
    for key in results:
        for r in results[key].values():
            by_stage[r['stage']] = by_stage.get(r['stage'], 0) + 1
    manifest = dict(
        schema=1, opts=OPTS, options=SEED_OPTIONS, translator_sha256=sha(args.translator.read_bytes()),
        translator_build=json.loads(build_info.read_text()) if build_info.is_file() else None,
        shader_list_sha256=sha(args.shader_list.read_bytes()) if args.shader_list.is_file() and not args.no_list else None,
        romfs=str(args.romfs), romfs_files=romfs_files, shaders_found=len(shaders), startup_shaders=len(startup),
        shaders_translated=len(results), limit=args.limit or None, unreadable_files=len(scan_errors),
        listed_bind_states=listed_rows, listed_for_found=listed_found, requests=counters['requests'],
        variants=n_variants, variants_by_stage=by_stage, duplicate_deps=counters['duplicates'],
        translate_failures=counters['translate_failed'], spirv_val_failures=counters['invalid'], pipelines=n_pipes,
        warm_title_shaders=sum(1 for k, v in warm_key.items() if 't' in v and k in results),
        not_shaders_dropped=len(rejected),
        seconds=dict(inventory=round(t_inventory, 1), translate=round(t_translate, 1),
                     total=round(time.monotonic() - started, 1), translator_ms=round(counters['ms'], 1)),
    )
    manifest['files'] = {n: sha((staging/n).read_bytes()) for n in ('variants.jsonl', 'spirv.bin', 'pipelines.jsonl', 'failed.jsonl')}
    (staging/'seed-manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', newline='\n')
    if scan_errors:
        (staging/'unreadable.txt').write_text('\n'.join(scan_errors) + '\n', newline='\n')
    if args.output.exists():
        remove_tree(args.output)
    staging.rename(args.output)
    if not args.no_list:
        # pipelines.jsonl with attachment formats (the warm-up links these): seed_pipelines.py
        subprocess.run([sys.executable, str(Path(__file__).with_name('seed_pipelines.py')), '--seed', str(args.output),
                        '--shader-list', str(args.shader_list)], check=False)
    print(json.dumps({k: v for k, v in manifest.items() if k not in ('files', 'options', 'translator_build')}, indent=1))
    return 0 if not counters['invalid'] else 1


if __name__ == '__main__':
    sys.exit(main())
