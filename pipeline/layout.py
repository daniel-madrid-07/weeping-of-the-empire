"""Guest memory layout: the single source of truth for module bases.

    layout.json (written by the pipeline, read by recomp and runtime):
    {
      "format": 1,
      "title_id": "0100F2C0115B6000", "version": "1.0.0",
      "code_region": {"base": "0x1000000000", "end": "0x...", "limit": "0x1100000000", "module_alignment": "0x200000"},
      "thunk_region_base": "0x7f00000000",
      "modules": [                                   # load order: main -> subsdk0 -> sdk
        {"name": "main", "base": "0x1000000000", "span": "0x...",   # span = pages incl. .bss
         "image": "images/main.img", "image_size": N, "image_sha256": "...",   # flat image, no .bss
         "nso": "extracted/v100/exefs/main", "nso_sha256": "...", "build_id": "...",
         "segments": {"text": {"offset", "size", "sha256", "hash_checked"}, "rodata": ..., "data": ...,
                      "bss": {"offset", "size"}},
         "mod0_offset": N, "dynamic_offset": N, "mod0": {...}, "needed": [...],
         "imports": N, "exports": N, "elf": "elf/main.elf"}
      ],
      "regions": {...} # heap/alias/stack/TLS
    }

Addresses are hex strings (JSON numbers lose precision above 2^53 in some readers); offsets/sizes inside a module are
plain integers. Paths are relative to the work folder."""
from pathlib import Path

CODE_BASE = 0x10_0000_0000          # 64 GiB
CODE_LIMIT = 0x11_0000_0000         # exclusive; RT_CODE_LIMIT in include/guest/rt_api.h (dispatch table size)
MODULE_ALIGN = 0x20_0000            # 2 MiB
THUNK_BASE = 0x7F_0000_0000
GUEST_LIMIT = 1 << 39               # the SDK encodes IPC buffer addresses in 39 bits
LOAD_ORDER = ('main', 'subsdk0', 'sdk')
FORMAT = 1


def align(value, boundary):
    return (value + boundary - 1) & ~(boundary - 1)


def assign_bases(spans, order=LOAD_ORDER, base=CODE_BASE, alignment=MODULE_ALIGN):
    """{name: span} -> [(name, base)] consecutive and `alignment`-aligned, in `order`."""
    out = []
    cursor = base
    for name in order:
        cursor = align(cursor, alignment)
        out.append((name, cursor))
        cursor += spans[name]
    if cursor > CODE_LIMIT:
        raise ValueError(f'Modules overflow the code region (RT_CODE_LIMIT {CODE_LIMIT:#x}): {cursor:#x}')
    return out, align(cursor, alignment)


def hexa(value):
    return f'{value:#x}'


def build_layout(modules, work, title_id, version, regions=None):
    """`modules`: {name: dict(span=..., **fields)} as produced by pipeline.modules; returns the layout dict."""
    bases, end = assign_bases({name: m['span'] for name, m in modules.items()})
    rows = []
    for name, base in bases:
        m = dict(modules[name])
        m['span'] = hexa(m['span'])
        rows.append(dict(name=name, base=hexa(base), **{k: v for k, v in m.items() if k != 'name'}))
    return dict(format=FORMAT, title_id=title_id, version=version,
                code_region=dict(base=hexa(CODE_BASE), end=hexa(end), limit=hexa(CODE_LIMIT),
                                 module_alignment=hexa(MODULE_ALIGN)),
                thunk_region_base=hexa(THUNK_BASE), guest_address_limit=hexa(GUEST_LIMIT),
                modules=rows, regions=regions or {})


def module_base(layout, name):
    """Integer base of module `name` in a loaded layout dict."""
    for m in layout['modules']:
        if m['name'] == name:
            return int(m['base'], 16)
    raise KeyError(name)


def regions_file(repo):
    return Path(repo) / 'runtime' / 'layout' / 'regions.json'
