"""NSP (PFS0) / XCI (HFS0) containers, tickets, NCA3 headers and AES-CTR sections.

Port of the helpers in MacNativeGames `games/totk/scripts/extract_exefs.py` (MIT). Keys stay in memory and are never
printed; error messages name keys, never their values.
"""
import hashlib
import struct
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import modes

from .common import decrypt, read_at, require

TOTK_TITLE_ID = '0100F2C0115B6000'
TOTK_UPDATE_TITLE_ID = '0100F2C0115B6800'

# NCA content types (header byte 0x205).
CONTENT_PROGRAM = 0
CONTENT_META = 1
CONTENT_CONTROL = 2
CONTENT_DATA = 4
CONTENT_PUBLIC_DATA = 5

# FS header (fs[2], fs[3]) = (fs type, hash type).
FS_PFS0 = (1, 2)    # PartitionFs, HierarchicalSha256
FS_ROMFS = (0, 3)   # RomFs, HierarchicalIntegrity (IVFC)
CRYPT_NONE = 1
CRYPT_CTR = 3
# the FS header's crypt type (fs[4]), for messages
CRYPT_NAMES = {0: 'crypt type 0 (auto)', 1: 'plain (crypt type 1)', 2: 'AES-XTS (crypt type 2)',
               3: 'AES-CTR (crypt type 3)', 4: "AES-CTR-EX (crypt type 4: an update's BKTR patch)",
               5: 'AES-CTR, layer hash skipped (crypt type 5)', 6: 'AES-CTR-EX, layer hash skipped (crypt type 6)'}


def hfs(f, offset, bound):
    return partition_entries(f, offset, bound, b'HFS0', 64)


def partition_entries(f, offset, bound, magic=b'PFS0', stride=24):
    """Read a bounded PFS0/HFS0 table without loading its file payloads.

    Returns [(name, absolute_offset, size)].
    """
    require(bound >= 16, 'Truncated partition header')
    hdr = read_at(f, offset, 16)
    require(hdr[:4] == magic, f'Expected {magic.decode()}')
    count, strings_size = struct.unpack_from('<II', hdr, 4)
    require(count <= 4096 and strings_size <= 1024 * 1024, 'Invalid partition table')
    size = 16 + count * stride + strings_size
    require(size <= bound, 'Partition table out of bounds')
    raw = read_at(f, offset, size)
    strings = raw[16 + count * stride:]
    entries = []
    names = set()
    for i in range(count):
        off, length, nameoff = struct.unpack_from('<QQI', raw, 16 + i * stride)
        require(nameoff < len(strings), 'Invalid partition filename offset')
        end = strings.find(b'\0', nameoff)
        require(end >= 0, 'Unterminated partition filename')
        name = strings[nameoff:end].decode('utf-8')
        require(name not in ('', '.', '..') and '/' not in name and '\\' not in name, 'Unsafe partition filename')
        require(name not in names, 'Duplicate partition filename')
        names.add(name)
        require(size + off + length <= bound, 'Partition entry out of bounds')
        entries.append((name, offset + size + off, length))
    end = offset + size
    for _, start, length in sorted(entries, key=lambda e: e[1]):
        require(start >= end, 'Overlapping partition entries')
        end = start + length
    return entries


def container_entries(f, size):
    """Return ('nsp'|'xci', [(name, offset, size)]) for the files of an NSP or of an XCI secure partition."""
    if read_at(f, 0, 4) == b'PFS0':
        return 'nsp', partition_entries(f, 0, size)
    require(size >= 0x138 and read_at(f, 0x100, 4) == b'HEAD', 'Expected an XCI or NSP (PFS0) game file')
    root = struct.unpack('<Q', read_at(f, 0x130, 8))[0]
    partitions = hfs(f, root, size - root)
    secure = [e for e in partitions if e[0] == 'secure']
    require(len(secure) == 1, 'Expected one XCI secure partition')
    return 'xci', hfs(f, secure[0][1], secure[0][2])


def load_keys(path):
    """Parse a prod.keys file into {name: bytes}. Never echoes values, even on malformed lines."""
    keys = {}
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            name, value = line.split('=', 1)
            try:
                keys[name.strip().lower()] = bytes.fromhex(value.strip())
            except ValueError:
                raise ValueError(f'Invalid key encoding at line {number}') from None
    return keys


def common_tickets(f, entries):
    """Collect {rights_id: encrypted title key} from common tickets (.tik) in the container, in memory only."""
    tickets = {}
    bodies = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80,
              0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}
    for name, offset, size in entries:
        if not name.lower().endswith('.tik'):
            continue
        require(size >= 4, 'Truncated NSP ticket')
        # Switch ticket signature type is little-endian: RSA2048/SHA256 = 04 00 01 00.
        sig = struct.unpack('<I', read_at(f, offset, 4))[0]
        require(sig in bodies, 'Unsupported NSP ticket signature type')
        body = bodies[sig]
        require(size >= body + 0x180, 'Truncated NSP ticket body')
        data = read_at(f, offset + body, 0x180)
        require(data[0x141] == 0, 'Personalized NSP tickets are unsupported; a common ticket is required')
        rights, key = data[0x160:0x170], data[0x40:0x50]
        require(rights not in tickets or tickets[rights] == key, 'Conflicting NSP tickets')
        tickets[rights] = key
    return tickets


def read_nca_header(f, offset, size, keys):
    require(size >= 0xc00, 'Truncated NCA header')
    require('header_key' in keys, 'Missing header_key in prod.keys')
    raw = read_at(f, offset, 0xc00)
    hdr = b''.join(decrypt(keys['header_key'], modes.XTS(i.to_bytes(16, 'big')),
                           raw[i * 512:(i + 1) * 512]) for i in range(6))
    require(hdr[0x200:0x204] == b'NCA3', 'Expected NCA3 (check prod.keys and the game file)')
    require(struct.unpack_from('<Q', hdr, 0x208)[0] == size, 'NCA size does not match container entry')
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + i * 16)
        require((start == end == 0) or 0xc00 <= start * 512 < end * 512 <= size, 'NCA section out of bounds')
    return hdr


def nca_ctr_key(hdr, keys, tickets):
    generation = max(max(hdr[0x206], hdr[0x220]) - 1, 0)
    rights = hdr[0x230:0x240]
    if rights != bytes(16):
        require(rights in tickets, 'Missing common ticket for rights-managed NCA in the NSP')
        keyname = f'titlekek_{generation:02x}'
        require(keyname in keys, f'Missing {keyname} in prod.keys')
        return decrypt(keys[keyname], modes.ECB(), tickets[rights])
    require(hdr[0x207] < 3, 'Unsupported NCA key area')
    area = ('application', 'ocean', 'system')[hdr[0x207]]
    keyname = f'key_area_key_{area}_{generation:02x}'
    require(keyname in keys, f'Missing {keyname} in prod.keys')
    return decrypt(keys[keyname], modes.ECB(), hdr[0x300:0x340])[32:48]


def nca_inventory(f, entries, keys):
    records = []
    for name, offset, length in entries:
        if not name.lower().endswith('.nca'):
            continue
        hdr = read_nca_header(f, offset, length, keys)
        record = dict(name=name, nca_offset=offset, size=length, content_type=hdr[0x205],
                      title_id=f'{struct.unpack_from("<Q", hdr, 0x210)[0]:016X}')
        records.append((record, hdr))
    return records


def program_nca(records, title_id=TOTK_TITLE_ID):
    """Select the single base-game Program NCA; refuse updates, DLC, combined packages and other games."""
    metadata = [r for r, _ in records if r['content_type'] == CONTENT_META]
    require(len(metadata) <= 1 and all(r['title_id'] == title_id for r in metadata),
            'Expected base game metadata; updates, DLC and combined packages are unsupported')
    programs = [(r, h) for r, h in records if r['content_type'] == CONTENT_PROGRAM]
    require(len(programs) == 1, 'Expected one base Program NCA; updates, DLC and combined packages are unsupported')
    record, hdr = programs[0]
    require(record['title_id'] == title_id, f'Expected base game {title_id}; updates, DLC and other games are unsupported')
    require(not any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4)), 'Expected a base game, not a BKTR update')
    return record, hdr


class NcaSection:
    """One AES-CTR section of an NCA inside an open container file; read() decrypts relative ranges."""

    def __init__(self, f, nca_offset, hdr, index, ctr_key):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * index)
        require(start != 0, f'NCA section {index} is empty')
        self.fs = hdr[0x400 + 512 * index:0x600 + 512 * index]
        require(hashlib.sha256(self.fs).digest() == hdr[0x280 + 32 * index:0x2A0 + 32 * index],
                'FS header hash mismatch')
        self.crypt = self.fs[4]
        self.f = f
        self.index = index
        self.start = start * 512
        self.size = (end - start) * 512
        self.base = nca_offset
        self.key = ctr_key
        self.kind = (self.fs[2], self.fs[3])

    def read(self, relative, size):
        require(0 <= relative and relative + size <= self.size, 'Section read out of bounds')
        if self.crypt == CRYPT_NONE:
            return read_at(self.f, self.base + self.start + relative, size)
        require(self.crypt == CRYPT_CTR, f'NCA section {self.index}: only plain and AES-CTR sections are supported')
        absolute = self.start + relative
        aligned = absolute & ~15
        skip = absolute - aligned
        iv = self.fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
        data = read_at(self.f, self.base + aligned, size + skip)
        return decrypt(self.key, modes.CTR(iv), data)[skip:]


def update_program(f, keys, size, title_id=TOTK_UPDATE_TITLE_ID):
    """Open an update container (the player's own dump of the update title): (kind, records, program_record, hdr,
    ctr_key). Its ExeFS is a plain AES-CTR section (NcaSection); its RomFS is BKTR (bktr.BktrSection over the base
    game's RomFS). The title key comes from the update's own common ticket."""
    kind, entries = container_entries(f, size)
    tickets = common_tickets(f, entries)
    records = nca_inventory(f, entries, keys)
    metadata = [r for r, _ in records if r['content_type'] == CONTENT_META]
    require(len(metadata) == 1 and metadata[0]['title_id'] == title_id,
            f'Expected the update {title_id} (one metadata NCA); base games, DLC and combined packages go elsewhere')
    programs = [(r, h) for r, h in records if r['content_type'] == CONTENT_PROGRAM]
    require(len(programs) == 1 and programs[0][0]['title_id'] == title_id, f'Expected one Program NCA of {title_id}')
    record, hdr = programs[0]
    require(any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4)), 'Expected an update (BKTR) Program NCA')
    return kind, [r for r, _ in records], record, hdr, nca_ctr_key(hdr, keys, tickets)


def program_sections(f, keys, size, title_id=TOTK_TITLE_ID):
    """Open a base game container and return (kind, records, program_record, [NcaSection])."""
    kind, entries = container_entries(f, size)
    tickets = common_tickets(f, entries)
    records = nca_inventory(f, entries, keys)
    record, hdr = program_nca(records, title_id)
    key = nca_ctr_key(hdr, keys, tickets)
    sections = [NcaSection(f, record['nca_offset'], hdr, i, key)
                for i in range(4) if struct.unpack_from('<I', hdr, 0x240 + 16 * i)[0]]
    return kind, [r for r, _ in records], record, sections
