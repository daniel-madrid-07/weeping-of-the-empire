"""The six system shared fonts (BFTTF) from an installed or unpacked firmware NCA directory.

Port of MacNativeGames `games/totk/scripts/extract_shared_fonts.py` (MIT), `--firmware` mode, reusing the NCA and
IVFC code of this package. Fonts stay in the BFTTF form the console places in shared memory.
"""
import hashlib
import re
import struct
import tempfile
from pathlib import Path

from .common import read_at, require
from .container import CONTENT_DATA, FS_ROMFS, NcaSection, nca_ctr_key, read_nca_header
from .romfs import extract_image, index_romfs

# nn::pl::SharedFontType -> (name, system data title, BFTTF file name in that title's RomFS)
FONTS = [
    (0, 'standard', '0100000000000811', 'nintendo_udsg-r_std_003.bfttf'),
    (1, 'chinese-simplified', '0100000000000814', 'nintendo_udsg-r_org_zh-cn_003.bfttf'),
    (2, 'chinese-simplified-extension', '0100000000000814', 'nintendo_udsg-r_ext_zh-cn_003.bfttf'),
    (3, 'chinese-traditional', '0100000000000813', 'nintendo_udjxh-db_zh-tw_003.bfttf'),
    (4, 'korean', '0100000000000812', 'nintendo_udsg-r_ko_003.bfttf'),
    (5, 'nintendo-extension', '0100000000000810', 'nintendo_ext_003.bfttf'),
]
BFTTF_DECODED_MAGIC = 0x7f9a0218


def bfttf_size(data):
    """BFTTF: big-endian words XORed with key = word0 ^ 0x7f9a0218; word1 = font size; sfnt payload follows."""
    require(len(data) >= 12, 'BFTTF too small')
    magic, size = struct.unpack_from('>II', data, 0)
    key = magic ^ BFTTF_DECODED_MAGIC
    decoded = size ^ key
    require(decoded + 8 == len(data), 'BFTTF size does not match file')
    first = struct.unpack_from('>I', data, 8)[0] ^ key
    require(first in (0x00010000, 0x4f54544f), 'BFTTF payload is not a TrueType/OpenType font')
    return decoded


def _nca_paths(folder):
    """NCA files of an unpacked firmware dir, or of an installed registered/ dir (`<name>.nca/00`)."""
    for path in sorted(Path(folder).iterdir()):
        if path.is_dir():
            path = path / '00'
        if path.is_file() and (path.name == '00' or path.suffix.lower() == '.nca'):
            yield path


def firmware_data_ncas(folder, keys):
    """{title_id (lower hex): path} for the shared-font data NCAs in `folder`."""
    require(Path(folder).is_dir(), f'Firmware must be a directory containing NCA files: {folder}')
    wanted = {title for _, _, title, _ in FONTS}
    found = {}
    for path in _nca_paths(folder):
        with path.open('rb') as f:
            hdr = read_nca_header(f, 0, path.stat().st_size, keys)
        title = f'{struct.unpack_from("<Q", hdr, 0x210)[0]:016x}'
        if hdr[0x205] == CONTENT_DATA and title in wanted:
            require(title not in found, f'Duplicate shared-font title {title} in firmware')
            found[title] = path
    require(wanted <= found.keys(), 'Firmware is missing shared-font data NCAs: ' + ', '.join(sorted(wanted - found.keys())))
    return found


def nca_romfs_image(path, keys, image_path):
    """Verify all IVFC levels of a standalone data NCA's RomFS and write the image to `image_path`."""
    with open(path, 'rb') as f:
        size = Path(path).stat().st_size
        hdr = read_nca_header(f, 0, size, keys)
        key = nca_ctr_key(hdr, keys, {})
        for i in range(4):
            if not struct.unpack_from('<I', hdr, 0x240 + 16 * i)[0]:
                continue
            section = NcaSection(f, 0, hdr, i, key)
            if section.kind == FS_ROMFS:
                return extract_image(section, image_path, log=lambda m: None)
    raise ValueError(f'No RomFS in the NCA {path}')


def _pick(entries, filename):
    """Exact file name, else the single file with the same stem but another revision suffix (firmware updates)."""
    if filename in entries:
        return filename
    stem = re.sub(r'_\d+\.bfttf$', '', filename)
    candidates = [p for p in entries if re.fullmatch(re.escape(stem) + r'_\d+\.bfttf', p)]
    require(len(candidates) == 1, f'{filename} missing (candidates: {candidates})')
    return candidates[0]


def extract_fonts(firmware, keys, out_dir, expected_sha256=None):
    """Write out_dir/<type>-<name>.bfttf for the six fonts and return the per-font report.

    A font whose bytes differ from `expected_sha256` is still accepted when it decodes as a valid BFTTF (a different
    firmware revision), as MacNativeGames does; the report records `match: false` for it.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ncas = firmware_data_ncas(firmware, keys)
    report = []
    with tempfile.TemporaryDirectory(prefix='nxrecomp-fonts-') as tmp:
        images = {}
        for kind, name, title, filename in FONTS:
            if title not in images:
                image = Path(tmp) / f'{title}.romfs'
                nca_romfs_image(ncas[title], keys, image)
                images[title] = (image, {e['path']: e for e in index_romfs(image)})
            image, entries = images[title]
            chosen = _pick(entries, filename)
            e = entries[chosen]
            with image.open('rb') as f:
                data = read_at(f, e['offset'], e['size'])
            font_bytes = bfttf_size(data)
            digest = hashlib.sha256(data).hexdigest()
            out = out_dir / f'{kind}-{name}.bfttf'
            out.write_bytes(data)
            expected = (expected_sha256 or {}).get(name)
            report.append(dict(type=kind, name=name, title_id=title, file=chosen, path=out.name,
                               bfttf_bytes=len(data), font_bytes=font_bytes, sha256=digest,
                               expected_sha256=expected, match=(digest == expected) if expected else None,
                               source_nca=str(ncas[title])))
    return report
