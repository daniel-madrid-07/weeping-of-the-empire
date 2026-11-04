"""UltraHand::calcGrab (main+0x172d1bc) as totk_grab_stick.cpp (hook_f_172d1bc, hook_f_171323c / hook_f_1713290) relies on
it, on the 1.0.0 image's own words: `mov x19, x0` in its prologue (the object is the hook's x0); the X / Y exponent reads
`bl 0x171323c; ...; ldr s8, [x0]` (return main+0x172d6d4) and `bl 0x1713290; ldr s10, [x0]` (return 0x172d6f0); the
stick reads `ldr s9, [x19, #0x11c]` at 0x172d708 and `ldr s8, [x19, #0x120]` at 0x172d734 around the two powf calls
(`bl 0x2aa42b0` at 0x172d730 / 0x172d75c); and no store to x19 + 0x11c / 0x120 in calcGrab's first 0x2000 bytes, so the
values the hook restores after the call are not ones calcGrab wrote. The hooks do not patch the image: `original` is the
one checked."""
import capstone

WANT = {0x172d6d0: ('bl', '#0x171323c'), 0x172d6d8: ('ldr', 's8, [x0]'), 0x172d6ec: ('bl', '#0x1713290'),
        0x172d6f0: ('ldr', 's10, [x0]'), 0x172d708: ('ldr', 's9, [x19, #0x11c]'), 0x172d730: ('bl', '#0x2aa42b0'),
        0x172d734: ('ldr', 's8, [x19, #0x120]'), 0x172d75c: ('bl', '#0x2aa42b0')}
START, SPAN = 0x172d1bc, 0x2000


def verify(original, patched=None):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    failures = []
    ins = {i.address: (i.mnemonic, i.op_str) for i in md.disasm(original[START:START + SPAN], START)}
    for a, want in WANT.items():
        if ins.get(a) != want:
            failures.append(f'main+{a:#x} is {ins.get(a)}, not {want}')
    if not any(v == ('mov', 'x19, x0') for a, v in ins.items() if a < START + 0x60):
        failures.append('calcGrab does not keep its object in x19 (no mov x19, x0 in its prologue)')
    for a, (mn, ops) in sorted(ins.items()):
        if mn.startswith('st') and ('[x19, #0x11c]' in ops or '[x19, #0x120]' in ops):
            failures.append(f'main+{a:#x}: {mn} {ops} writes the stick calcGrab reads')
    return dict(name='grab_stick', status='fail' if failures else 'pass', checked=4, failures=failures)
