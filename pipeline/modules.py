"""ExeFS NSOs -> verified flat module images (<work>/images), analysis ELFs (<work>/elf) and <work>/layout.json.

    python -m pipeline.modules --work C:\\nxrecomp-work [--expected expected-v1.0.0.json]
"""
import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from . import layout as layout_mod
from .nx import nso
from .nx.common import read_json, require, write_json

REPO = Path(__file__).resolve().parent.parent
ANALYSIS_ONLY = ('rtld',)   # exported as ELF for analysis; never loaded (the runtime does rtld's job)


def _write_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def prepare_modules(work, exefs, expected_exefs=None, title_id='0100F2C0115B6000', version='1.0.0', log=print):
    work, exefs = Path(work), Path(exefs)
    t0 = time.monotonic()
    rows = {}
    summary = []
    for name in layout_mod.LOAD_ORDER + ANALYSIS_ONLY:
        raw = (exefs / name).read_bytes()
        nso_sha = hashlib.sha256(raw).hexdigest()
        if expected_exefs and name in expected_exefs:
            require(nso_sha == expected_exefs[name], f'{name}: NSO sha256 {nso_sha} != expected {expected_exefs[name]}')
        m = nso.Module(name, raw)
        elf, changed = m.to_elf()
        _write_bytes(work / 'elf' / f'{name}.elf', elf)
        inventory = dict(module=name, nso_sha256=nso_sha, build_id=m.build_id,
                         elf_sha256=hashlib.sha256(elf).hexdigest(),
                         elf_purpose='analysis carrier only (ET_DYN, module-relative addresses); load at the module '
                                     'base from layout.json',
                         segments=m.segment_table(), mod0_offset=m.mod0_offset, mod0=m.mod0,
                         dynamic_offset=m.dynamic_offset, dynamic_tags=m.dynamic, needed=m.needed,
                         imports=m.imports, exports=m.exports, svc_instruction_candidates=m.svc_candidates(),
                         elf_symbol_index_changes=changed)
        write_json(work / 'elf' / f'{name}-inventory.json', inventory)
        row = dict(nso=os.path.relpath(exefs / name, work).replace('\\', '/'), nso_sha256=nso_sha,
                   build_id=m.build_id, span=m.span, segments=m.segment_table(), mod0_offset=m.mod0_offset,
                   mod0=m.mod0, dynamic_offset=m.dynamic_offset, needed=m.needed, imports=len(m.imports),
                   exports=len(m.exports), elf=f'elf/{name}.elf')
        if name not in ANALYSIS_ONLY:
            image = bytes(m.image)
            _write_bytes(work / 'images' / f'{name}.img', image)
            row.update(image=f'images/{name}.img', image_size=len(image), image_sha256=hashlib.sha256(image).hexdigest())
            rows[name] = row
        summary.append(dict(module=name, build_id=m.build_id, text_bytes=m.segments[0]['size'], span=m.span,
                            imports=len(m.imports), exports=len(m.exports), svc_candidates=len(inventory['svc_instruction_candidates']),
                            elf=f'elf/{name}.elf', elf_sha256=inventory['elf_sha256']))
        log(f"modules: {name}: text {m.segments[0]['size']} B, span {m.span:#x}, {len(m.imports)} imports, "
            f"{len(m.exports)} exports")
    regions_path = layout_mod.regions_file(REPO)
    regions = read_json(regions_path) if regions_path.exists() else {}
    layout = layout_mod.build_layout(rows, work, title_id, version, regions=regions)
    write_json(work / 'layout.json', layout)
    write_json(work / 'elf' / 'modules.json', summary)
    for m in layout['modules']:
        log(f"layout: {m['name']} at {m['base']} (span {m['span']})")
    return dict(layout=layout, modules=summary, regions_from=str(regions_path) if regions else None,
                seconds=round(time.monotonic() - t0, 1))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--exefs', type=Path, help='default: <work>/extracted/v100/exefs')
    p.add_argument('--expected', type=Path, help='expected-v1.0.0.json')
    a = p.parse_args(argv)
    expected = json.loads(a.expected.read_text())['exefs_sha256'] if a.expected else None
    prepare_modules(a.work, a.exefs or a.work / 'extracted' / 'v100' / 'exefs', expected,
                    log=lambda m: print(m, flush=True))


if __name__ == '__main__':
    sys.exit(main())
