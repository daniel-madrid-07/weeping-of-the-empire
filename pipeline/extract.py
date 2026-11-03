"""Extraction steps (NSP -> ExeFS / RomFS) shared by build.py and the standalone command line.

    python -m pipeline.extract romfs --nsp <nsp> --keys <prod.keys> --out <work>/extracted/v100
    python -m pipeline.extract {exefs,romfs} --nsp <game.nup> --out <work>/extracted/v100     (a NUP: no keys)
    python -m pipeline.extract {exefs,romfs} --nsp <base nsp> --update <update nsp> --keys <prod.keys> --out <work>/extracted/v143

Each step writes a JSON report next to its outputs; build.py records it in the build manifest.
"""
import argparse
import hashlib
import io
import struct
import sys
import time
from pathlib import Path

from .nx import nup as nup_mod
from .nx import romfs as romfs_mod
from .nx import romfs_build
from .nx import romsource
from .nx.common import below_normal_priority, require, wait_for_ram, write_json
from .nx.bktr import CRYPT_CTR_EX, BktrSection
from .nx.container import (CRYPT_CTR, CRYPT_NONE, CRYPT_NAMES, FS_PFS0, FS_ROMFS, TOTK_TITLE_ID, TOTK_UPDATE_TITLE_ID,
                            NcaSection, load_keys, nca_body, partition_entries, program_sections, update_program)

TOTK_UPDATE_VERSION = 720896   # (the NCM version of 1.4.3: pipeline/expected/totk-1.4.3.json; a NUP's update package)


def source_identity(path):
    st = Path(path).stat()
    return dict(path=str(Path(path).resolve()), size=st.st_size, mtime_ns=st.st_mtime_ns)


def update_sections(f, path, keys):
    """The update's (kind, records, program, sections): its plain sections as NcaSection (the ExeFS among them); the
    BKTR RomFS section is left out (patched_romfs reads it)."""
    kind, records, program, hdr, key = update_program(f, keys, Path(path).stat().st_size)
    sections = [NcaSection(f, program['nca_offset'], hdr, i, key, nca_body(f, program)) for i in range(4)
                if struct.unpack_from('<I', hdr, 0x240 + 16 * i)[0] and hdr[0x400 + 512 * i + 4] != CRYPT_CTR_EX]
    return kind, records, program, sections


def patched_romfs(fu, update, base_sections, keys, spill=None):
    """The update's RomFS merged over the base game's (bktr.BktrSection): (update program record, section)."""
    _, _, program, hdr, key = update_program(fu, keys, Path(update).stat().st_size)
    idx = [i for i in range(4) if struct.unpack_from('<I', hdr, 0x240 + 16 * i)[0]
           and hdr[0x400 + 512 * i + 4] == CRYPT_CTR_EX and tuple(hdr[0x400 + 512 * i + 2:0x400 + 512 * i + 4]) == FS_ROMFS]
    require(len(idx) == 1, f'Expected one BKTR RomFS section in the update, found {len(idx)}')
    return program, BktrSection(fu, program['nca_offset'], hdr, idx[0], key, romfs_mod.find_romfs_section(base_sections),
                                nca_body(fu, program, spill))


def nup_package(path, update):
    """(Nup, package): the base game's, or (update) the 1.4.3 update's (nx/nup.py)"""
    if update:
        return nup_mod.open_package(path, TOTK_UPDATE_TITLE_ID, update=True, version=TOTK_UPDATE_VERSION)
    return nup_mod.open_base(path, TOTK_TITLE_ID)


def nup_exefs(nsp, out_dir, log=print, update=False):
    """A NUP's base game ExeFS (or the update's) into out_dir/exefs (nx/nup.py: plaintext, each file checked against its
    SHA-256; build.py then checks the modules against the pinned hashes, as for an NSP)"""
    n, p = nup_package(nsp, update)
    with n.f:
        files = [(name, n.read(e)) for name, e in nup_mod.exefs_files(n, p)]
    exefs = Path(out_dir) / 'exefs'
    exefs.mkdir(parents=True, exist_ok=True)
    extracted = []
    for name, data in files:
        (exefs / name).write_bytes(data)
        extracted.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    report = dict(source=source_identity(nsp), container='nup', nca_inventory=[],
                  program=dict(name=f"NUP {'update' if update else 'base'} package", title_id=p.title,
                               version=p.version), section_index=None,
                  integrity="NUP TOC SHA-256 and every file's SHA-256 verified (an unencrypted dump: no NCA)",
                  files=extracted)
    write_json(Path(out_dir) / 'exefs-report.json', report)
    log(f'exefs: {len(extracted)} files (NUP)')
    return report


def nup_romfs(nsp, image, log=print, update=False):
    """A NUP's base game RomFS tree (or the update's own whole tree) laid out as the console's image (nx/romfs_build.py:
    byte for byte, so the pinned SHA-256 checks it), written to image (through a .partial). Returns extract_image()'s
    dict."""
    n, p = nup_package(nsp, update)
    tmp = Path(image).with_name(Path(image).name + '.partial')
    digest = hashlib.sha256()
    with n.f:
        entries = dict(n.walk(p.romfs))
        log(f'romfs: NUP, {len(entries)} files, laying out the image (each file checked against its SHA-256)')
        with open(tmp, 'wb') as out:
            lay = romfs_build.build([(path, e.size) for path, e in entries.items()],
                                    lambda path: n.stream(entries[path]), out, digest, log)
    tmp.replace(image)
    return dict(romfs_size=lay.size, romfs_sha256=digest.hexdigest(), ivfc_blocks=[], image_offset=None)


def extract_exefs(nsp, keys_path, out_dir, log=print, update=None):
    """Extract + verify the Program ExeFS into out_dir/exefs (PFS0 hash table and every data block): the base game's,
    or with `update` (the player's dump of the update NSP) the update's, which replaces every module. A NUP: nup_exefs."""
    out_dir = Path(out_dir)
    if nup_mod.is_nup_path(update or nsp):   # (the update's own ExeFS replaces every module, as with an NSP)
        return nup_exefs(update or nsp, out_dir, log, update=bool(update))
    require(keys_path, 'prod.keys is needed for an NSP / XCI (a NUP needs none)')
    keys = load_keys(keys_path)
    source = update or nsp
    with open(source, 'rb') as f:
        if update:
            kind, records, program, sections = update_sections(f, update, keys)
        else:
            kind, records, program, sections = program_sections(f, keys, Path(nsp).stat().st_size)
        # The ExeFS is the first PFS0 section; a later one (the plain logo section) is not code.
        found = [s for s in sections if s.kind == FS_PFS0]
        require(found, 'No ExeFS section found')
        s = found[0]
        # AES-CTR as the console stores it, or plain (crypt type 1: some dump tools / converters write the NCA's
        # sections decrypted); the hashes below check either the same way
        require(s.crypt in (CRYPT_CTR, CRYPT_NONE),
                f'the ExeFS section is {CRYPT_NAMES.get(s.crypt, f"crypt type {s.crypt}")}, neither AES-CTR nor plain: '
                'pick the base game file (version 1.0.0) as dumped from your console, not an update, a DLC or a '
                'converted file')
        blocksize, levels = struct.unpack_from('<II', s.fs, 0x28)
        hashoff, hashsize, pfsoff, pfssize = struct.unpack_from('<QQQQ', s.fs, 0x30)
        require(levels == 2 and blocksize > 0 and pfssize <= 512 * 1024 * 1024, 'Unsupported ExeFS hash layout')
        hashes = s.read(hashoff, hashsize)
        require(hashlib.sha256(hashes).digest() == s.fs[8:40], 'ExeFS hash-table mismatch')
        pfs = s.read(pfsoff, pfssize)
        for index, pos in enumerate(range(0, pfssize, blocksize)):
            require(hashlib.sha256(pfs[pos:pos + blocksize]).digest() == hashes[index * 32:(index + 1) * 32],
                    f'ExeFS block hash mismatch: {index}')
        files = [(name, pfs[off:off + size]) for name, off, size in partition_entries(io.BytesIO(pfs), 0, len(pfs))]
        require(any(name == 'main.npdm' for name, _ in files), 'PFS0 is not ExeFS')
    exefs = out_dir / 'exefs'
    exefs.mkdir(parents=True, exist_ok=True)
    extracted = []
    for name, data in files:
        (exefs / name).write_bytes(data)
        extracted.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    report = dict(source=source_identity(source), container=kind, nca_inventory=records, program=program,
                  section_index=s.index,
                  integrity='FS header, ExeFS hash table, and every ExeFS data block verified; RSA signatures not checked',
                  files=extracted)
    write_json(out_dir / 'exefs-report.json', report)
    log(f'exefs: {len(extracted)} files')
    return report


def extract_romfs(nsp, keys_path, out_dir, expected=None, unpack=True, log=print, update=None, in_dump=False):
    """Verify every IVFC level and write out_dir/romfs.bin, out_dir/romfs-index.json and (unpack) out_dir/romfs/.
    With `update` (the player's dump of the update NSP) the image is the update's RomFS merged over the base game's.
    With `in_dump` the image stays in the dump: out_dir/romfs-source.txt (nx/romsource.py) replaces romfs.bin (the
    same full pass and pinned hash; no 17 GB copy; a base game only, no unpacked tree).

    `expected` = {'size', 'sha256'} of the image; a mismatch raises and leaves no image behind.
    """
    require(not (in_dump and (update or unpack)), 'the RomFS can stay in the dump only for a base game without a tree')
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    nup = nup_mod.is_nup_path(update or nsp)   # (an update NUP's RomFS: its own whole tree, no base needed)
    require(not (nup and in_dump), 'the RomFS of a NUP cannot stay in the dump (zstd, file by file): extract it to '
            'romfs.bin (without --in-dump)')
    image = out_dir / (romsource.SOURCE if in_dump else 'romfs.bin')
    t0 = time.monotonic()
    if nup:
        result = nup_romfs(update or nsp, image, log=log, update=bool(update))
        source, program, section = None, dict(name=f"NUP {'update' if update else 'base'} package"), None
    else:
        result, source, program, section = _nca_romfs(nsp, keys_path, image, in_dump, update, log)
    t_image = time.monotonic() - t0
    return _romfs_finish(nsp, out_dir, image, result, source, program, section, expected, unpack, log, update, in_dump,
                         t_image)


def _nca_romfs(nsp, keys_path, image, in_dump, update, log):
    """extract_romfs over an NSP / XCI's Program NCA: (result, romsource or None, program record, section)"""
    require(keys_path, 'prod.keys is needed for an NSP / XCI (a NUP needs none)')
    keys = load_keys(keys_path)
    if update:   # (a spill a killed / crashed run left: NczBody removes its own only on close or at exit)
        for old in Path(image).parent.glob('ncz-*.plain'):
            try:
                old.unlink()
                log(f'romfs: {old.name} (an earlier run\'s spilled NSZ / XCZ plaintext) removed')
            except OSError:
                pass
    with open(nsp, 'rb') as f, (open(update, 'rb') if update else io.BytesIO()) as fu:
        kind, records, program, sections = program_sections(f, keys, Path(nsp).stat().st_size,
                                                            spill=str(Path(image).parent) if update else None)
        if update:
            # (an update NSZ / XCZ: its patch data from a solid stream may be read out of order: spilled beside the
            # image then, a temp file of the update's size, removed after)
            program, section = patched_romfs(fu, update, sections, keys, spill=str(Path(image).parent))
        else:
            section = romfs_mod.find_romfs_section(sections)
        # an NSZ / XCZ's image is zstd inside the dump: the game cannot read it in place, so it is copied out (romfs.bin;
        # Setup picks the copy for these dumps)
        require(not (in_dump and section.body is not None), 'the RomFS of an NSZ / XCZ dump cannot stay in the dump: '
                'extract it to romfs.bin (without --in-dump)')
        del keys
        if section.size > 1024 ** 3:   # heavy job on a shared machine (memory itself stays ~100 MB)
            wait_for_ram(log=log)
        log(f'romfs: section {section.index}, {section.size / 2**30:.2f} GiB, verifying IVFC levels 0-5' +
            (' (the image stays in the dump)' if in_dump else ''))
        result = romfs_mod.extract_image(section, None if in_dump else image, log=log)
        source = romsource.describe(section, nsp, result['image_offset'], result['romfs_size'],
                                    result['romfs_sha256']) if in_dump else None
    return result, source, program, section


def _romfs_finish(nsp, out_dir, image, result, source, program, section, expected, unpack, log, update, in_dump,
                  t_image):
    if expected:
        if result['romfs_size'] != expected['size'] or result['romfs_sha256'] != expected['sha256']:
            if not in_dump:
                image.rename(image.with_name('romfs.bin.mismatch'))
            raise ValueError(f"RomFS hash/size mismatch: got {result['romfs_size']} {result['romfs_sha256']}, "
                             f"expected {expected['size']} {expected['sha256']}")
    if in_dump:
        romsource.write_source(image, source)
    # the other form of the same image goes (romfs.bin when the dump serves it now, romfs-source.txt when copied out),
    # with its verified record: step 2 records the new one
    olds = ('romfs.bin.verified.json', 'romfs.bin') if in_dump else () if update else \
        (romsource.SOURCE + '.verified.json', romsource.SOURCE)
    for old in olds:
        try:
            (out_dir / old).unlink(missing_ok=True)
        except OSError as e:   # (open in a running game: left, the next run removes it)
            log(f'romfs: {old} left in place ({e.strerror})')
    files = romfs_mod.index_romfs(image)
    write_json(out_dir / 'romfs-index.json', dict(image=image.name, files=files))
    unpacked = None
    t_unpack = 0.0
    if unpack:
        t1 = time.monotonic()
        log(f'romfs: unpacking {len(files)} files into {out_dir / "romfs"}')
        unpacked = romfs_mod.unpack(image, files, out_dir / 'romfs', log=log)
        t_unpack = time.monotonic() - t1
    report = dict(status='pass', source=source_identity(nsp), update=source_identity(update) if update else None,
                  program=program['name'], section=section.index if section is not None else None,
                  ivfc_blocks=result['ivfc_blocks'], romfs_size=result['romfs_size'],
                  romfs_sha256=result['romfs_sha256'], expected_match=bool(expected),
                  file_count=len(files), total_file_bytes=sum(e['size'] for e in files),
                  image=image.name, index='romfs-index.json', tree='romfs' if unpack else None,
                  seconds=dict(image=round(t_image, 1), unpack=round(t_unpack, 1)),
                  integrity=("NUP TOC and every file's SHA-256 verified, the image laid out as the console's"
                             if section is None else 'FS header and all six IVFC levels verified; RSA signature not checked'))
    write_json(out_dir / 'romfs-report.json', report)
    log(f"romfs: pass, {len(files)} files, {result['romfs_size']} bytes, sha256 {result['romfs_sha256']}")
    return report


def default_firmware():
    import os
    return Path(os.environ.get('APPDATA', '')) / 'Ryujinx' / 'bis' / 'system' / 'Contents' / 'registered'


def extract_fonts(firmware, keys_path, out_dir, expected=None, log=print):
    """Six shared fonts into out_dir/fonts + out_dir/fonts-report.json."""
    from .nx.fonts import extract_fonts as run
    out_dir = Path(out_dir)
    t0 = time.monotonic()
    fonts = run(firmware, load_keys(keys_path), out_dir / 'fonts', expected_sha256=expected)
    for r in fonts:
        state = 'hash match' if r['match'] else ('VALID BFTTF, DIFFERENT BYTES' if r['match'] is False else 'no reference')
        log(f"fonts: {r['type']} {r['name']}: {r['font_bytes']} bytes, {state}")
    report = dict(status='pass', firmware=str(Path(firmware).resolve()), fonts=fonts,
                  all_match=all(r['match'] for r in fonts), seconds=round(time.monotonic() - t0, 1),
                  integrity='NCA FS header and six IVFC levels verified per NCA; BFTTF header decodes to an sfnt font; '
                            'RSA not checked')
    write_json(out_dir / 'fonts-report.json', report)
    return report


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('step', choices=['exefs', 'romfs', 'fonts'])
    p.add_argument('--nsp', type=Path)
    p.add_argument('--update', type=Path, help="the player's dump of the update NSP (exefs: the update's; romfs: merged)")
    p.add_argument('--firmware', type=Path)
    p.add_argument('--keys', type=Path, help='prod.keys (not for a NUP, whose files are unencrypted)')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--expected', type=Path, help='expected-v1.0.0.json')
    p.add_argument('--no-unpack', action='store_true')
    p.add_argument('--in-dump', action='store_true',
                   help='romfs: leave the image in the dump (romfs-source.txt instead of romfs.bin; implies --no-unpack)')
    a = p.parse_args(argv)
    below_normal_priority()
    log = lambda m: print(m, flush=True)  # noqa: E731
    import json
    expected = json.loads(a.expected.read_text()) if a.expected else {}
    if a.step == 'fonts':
        extract_fonts(a.firmware or default_firmware(), a.keys, a.out, expected=expected.get('shared_fonts_sha256'),
                      log=log)
    elif a.step == 'exefs':
        extract_exefs(a.nsp, a.keys, a.out, log=log, update=a.update)
    else:
        extract_romfs(a.nsp, a.keys, a.out, expected=expected.get('romfs'), unpack=not (a.no_unpack or a.in_dump),
                      log=log, update=a.update, in_dump=a.in_dump)


if __name__ == '__main__':
    sys.exit(main())
