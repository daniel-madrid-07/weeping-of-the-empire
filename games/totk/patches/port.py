"""Move the 1.0.0 patch set to another build of main (1.4.3) without touching sites_v100.py.

    python -m games.totk.patches port --old <v100 exefs/main> --new <v143 exefs/main> --version 1.4.3 --out v143.toml
                                      [--report port-report.json]

sites_v100.py builds every word at 1.0.0 offsets. `relocate` takes those words and moves them:
  - a site word goes to the porter's address for its 1.0.0 offset (tools/version_port.py: matched functions, never
    a guess); a site the porter cannot place stops the whole table, with the list of what is missing;
  - veneer code moves with the text padding (its offset from the first 16-byte boundary after .text is kept);
  - every PC-relative word, original or new (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR, ADRP, LDR literal), is decoded,
    its target moved the same way (code -> porter, padding -> new padding, data page -> new data page) and re-encoded
    at its new place; a copy of the game's own word (Orig) moves with the word it copies. Everything else is kept: a
    word that holds a struct offset the update changed is caught by `apply`, which checks each feature's replaced
    words (original_sha256, over the new image) before writing any.
The replaced words come from the 1.0.0 image (sites_v100.with_image): no table holds them.
So `apply --table v143.toml` either patches exactly the 1.0.0 behaviour or refuses with the sites to look at.
"""
import json
from dataclasses import dataclass

from . import sites_v100
from .sites_v100 import Layout, Orig, Patch


class PortError(Exception):
    pass


# ------------------------------------------------------------------------------------------------- PC-relative words

def _sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# kind: (match mask, match value, field shift, field bits, scale)
_PCREL = (
    ('b', 0x7C000000, 0x14000000, 0, 26, 4),           # B, BL
    ('bcond', 0xFF000010, 0x54000000, 5, 19, 4),
    ('cb', 0x7E000000, 0x34000000, 5, 19, 4),          # CBZ, CBNZ
    ('tb', 0x7E000000, 0x36000000, 5, 14, 4),          # TBZ, TBNZ
    ('lit', 0x3B000000, 0x18000000, 5, 19, 4),         # LDR (literal)
)


def pcrel_target(word, pc):
    """(kind, target) of a PC-relative word at pc, or None."""
    if word & 0x1F000000 == 0x10000000:                # ADR / ADRP
        imm = _sx(((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3, 21)
        return ('adrp', (pc & ~0xFFF) + (imm << 12)) if word & 0x80000000 else ('adr', pc + imm)
    for kind, mask, val, shift, bits, scale in _PCREL:
        if word & mask == val:
            return kind, pc + scale * _sx((word >> shift) & ((1 << bits) - 1), bits)
    return None


def retarget(word, pc, target):
    """`word` (PC-relative) re-encoded at pc to reach target; every other field kept."""
    kind, _ = pcrel_target(word, pc)
    if kind in ('adr', 'adrp'):
        imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12 if kind == 'adrp' else target - pc
        if kind == 'adrp' and target & 0xFFF:
            raise PortError(f'adrp at {pc:#x} to an unaligned page {target:#x}')
        if not -(1 << 20) <= imm < (1 << 20):
            raise PortError(f'{kind} at {pc:#x} -> {target:#x} out of range')
        imm &= 0x1FFFFF
        return word & 0x9F00001F | (imm & 3) << 29 | (imm >> 2) << 5
    for k, mask, val, shift, bits, scale in _PCREL:
        if k == kind:
            delta = target - pc
            if delta % scale or not -(1 << (bits - 1)) <= delta // scale < (1 << (bits - 1)):
                raise PortError(f'{kind} at {pc:#x} -> {target:#x} out of range')
            field = ((1 << bits) - 1) << shift
            return word & ~field & 0xFFFFFFFF | ((delta // scale) & ((1 << bits) - 1)) << shift
    raise AssertionError(kind)


# -------------------------------------------------------------------------------------------------------- relocation

@dataclass
class PortedWord(sites_v100.Word):
    v100_offset: int = None


def needed_addresses(patches, old: Layout):
    """Every 1.0.0 code offset the patch set names: its sites and the code its PC-relative words reach."""
    need = set()
    for p in patches:
        for w in p.words:
            if w.offset < old.text_end:
                need.add(w.offset)
            for word in (w.original, w.new):
                if not isinstance(word, int):          # (a copy: its own place is a site already)
                    continue
                t = pcrel_target(word, w.offset)
                if t and 0 <= t[1] < old.text_end and t[0] != 'adrp':
                    need.add(t[1])
    return sorted(need)


def relocate(patches, old: Layout, new: Layout, port, data_page_size=sites_v100.DATA_PAGE_SIZE):
    """The patch set moved from layout `old` to `new`. `port(offset) -> new offset or None` places 1.0.0 code."""
    missing = set()

    def move(t, what):
        if old.text_end <= t < old.pad_end:                    # veneer padding (and its alignment gap)
            return t - old.pad + new.pad
        if old.data_off <= t < old.data_off + data_page_size:
            return t - old.data_off + new.data_off
        if 0 <= t < old.text_end:
            m = port(t)
            if m is None:
                missing.add((t, what))
                return None
            return m
        raise PortError(f'{what}: target {t:#x} is outside code, padding and data page')

    def word_at(word, pc_old, pc_new, what):
        t = pcrel_target(word, pc_old)
        if t is None:
            return word
        if t[0] == 'adrp' and not old.data_off <= t[1] < old.data_off + data_page_size:
            raise PortError(f'{what}: adrp to {t[1]:#x}, not the data page (a data reference: port it by hand)')
        nt = move(t[1], what)
        return None if nt is None or pc_new is None else retarget(word, pc_new, nt)

    out = []
    for p in patches:
        q = Patch(p.feature)
        q.veneers = [(move(a, f'{p.feature} veneer'), move(b - 4, f'{p.feature} veneer') + 4) for a, b in p.veneers]
        for w in p.words:
            what = f'{p.feature} main+{w.offset:#x}'
            at = move(w.offset, what)
            orig = word_at(w.original, w.offset, at, what)
            if isinstance(w.new, Orig):                 # the game's own word, moved with the code it comes from
                to = move(w.new.offset, what)
                new_w = None if to is None else Orig(to)
            else:
                new_w = word_at(w.new, w.offset, at, what)
            if at is not None and orig is not None and new_w is not None:
                q.words.append(PortedWord(at, new_w, w.reason, pad=w.pad, original=orig, v100_offset=w.offset))
        if new.pad_end < max([b for _, b in q.veneers] + [0]):
            raise PortError(f'{p.feature}: veneers end past the new text padding ({new.pad_end:#x}); the new build '
                            f'has {new.pad_end - new.text_end} bytes of padding, 1.0.0 had {old.pad_end - old.text_end}')
        out.append(q)
    if missing:
        raise PortError('the porter has no place for: ' + ', '.join(f'{w} -> {t:#x}' for t, w in sorted(missing)))
    return out


def placeable(r):
    """A porter answer a patch word may be placed by: same instruction of a matched function whose pair is confirmed
    (an agreeing call edge or agreeing neighbours). New words that branch to code are not covered by apply's check
    of the original words, so every address goes through this, sites and targets alike."""
    return r.get('how') in ('exact', 'aligned') and bool(r.get('confirmed'))


def port_table(old_nso, new_nso, version, new_data_off=None):
    """Run the porter on two main NSOs and relocate every feature. Returns (game, patches, report, image); patches is None
    (and report['error'] says why) when the set cannot be placed."""
    import hashlib
    import os
    import sys
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    sys.path.insert(0, os.path.join(root, 'tools'))
    import version_port as vp
    from recomp.nso import load_nso
    from . import layout_v100, load_main_image

    m = load_nso(str(new_nso), 'main')
    new = Layout(text_end=m.text.end, data_off=new_data_off or (m.size + 0x3fff) & ~0x3fff)
    old = layout_v100()
    patches = sites_v100.with_image(sites_v100.build(old, list(sites_v100.FEATURES)), load_main_image(old_nso))
    porter = vp.Porter(vp.load_image(str(old_nso)), vp.load_image(str(new_nso))).run()
    report = dict(stats=porter.stats(), padding=dict(old=old.pad_end - old.text_end, new=new.pad_end - new.text_end),
                  sites={})
    placed = {}
    for off in needed_addresses(patches, old):
        r = porter.map(off)
        report['sites'][hex(off)] = r
        if placeable(r):
            placed[off] = int(r['new'], 16)
    with open(new_nso, 'rb') as f:
        nso_sha = hashlib.sha256(f.read()).hexdigest()
    game = dict(title_id='0100F2C0115B6000', version=version, build_id=m.build_id.hex()[:40], nso_sha256=nso_sha,
                image_sha256=hashlib.sha256(m.data).hexdigest(), text_end=new.text_end, span=m.size,
                data_page=new.data_off)
    try:
        return game, relocate(patches, old, new, placed.get), report, m.data
    except PortError as e:
        report['error'] = str(e)
        return game, None, report, m.data


def main_port(args):
    """The `port` command: write the table (or refuse) and the report. Returns the exit code."""
    from . import table_toml
    game, moved, report, image = port_table(args.old, args.new, args.version)
    if args.report:
        with open(args.report, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(report, f, indent=1)
    summary = dict(stats=report['stats'], padding=report['padding'],
                   unplaced=sorted(k for k, v in report['sites'].items() if not placeable(v)))
    if moved is None:
        print(json.dumps(dict(status='refused', problem=report['error'], **summary)))
        return 1
    text = table_toml(game, moved, [
        f'# TotK {args.version} code patches. GENERATED by `python -m games.totk.patches port` from the 1.0.0 set',
        "# (sites_v100.py) and the porter's function matches (tools/version_port.py). Do not edit: fix the 1.0.0 site or",
        "# the porter. v100_offset names each word's 1.0.0 place. apply checks each feature's original_sha256 against",
        "# the image."], image)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text + '\n')
    print(json.dumps(dict(status='pass', wrote=str(args.out), words=sum(len(p.words) for p in moved), **summary)))
    return 0
