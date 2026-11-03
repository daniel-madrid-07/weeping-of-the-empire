"""The foot IK ground-height ease (phive's foot IK graphics controller main+0x9a396c) as totk_footik_ease.cpp
(hook_f_9a7130 at lr main+0x9a5a14) relies on it, on the 1.0.0 image's own words: the call is `bl 0x9a7130` at
main+0x9a5a10, the caller reads the factor once (`ldr s0, [x0]` at main+0x9a5a14), nothing writes s0 / v0 between that
load and the ease (main+0x9a5a18..0x9a5ad4, the foot loop in between works in s1-s4), and the ease itself is
`ldr s3, [x11, #0x70]; fsub s1, s1, s3; fmul s0, s0, s1; fadd s0, s3, s0; str s0, [x11, #0x70]` (main+0x9a5ad0..e0):
x70 += (target - x70) * k, which k' = 1 - (1 - k)^r turns into the 30 Hz ease (the model: test_footik_ease.py). Also
checks the getter returns &param + 0xf8 (`add x0, x19, #0xf8` at main+0x9a7148). The hook does not patch the image:
`original` is the one checked."""
import capstone

CALL, AFTER, EASE, END = 0x9a5a10, 0x9a5a14, 0x9a5ad0, 0x9a5ae4
WANT_EASE = [('ldr', 's3, [x11, #0x70]'), ('fsub', 's1, s1, s3'), ('fmul', 's0, s0, s1'), ('fadd', 's0, s3, s0'),
             ('str', 's0, [x11, #0x70]')]


def verify(original, patched=None):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    failures = []
    ins = {i.address: (i.mnemonic, i.op_str) for i in md.disasm(original[CALL:END], CALL)}
    if ins.get(CALL) != ('bl', '#0x9a7130') or ins.get(AFTER) != ('ldr', 's0, [x0]'):
        failures.append(f'the call site is {ins.get(CALL)} / {ins.get(AFTER)}, not bl 0x9a7130 / ldr s0, [x0]')
    for a in range(AFTER + 4, EASE, 4):
        mn, ops = ins.get(a, ('?', ''))
        dst = ops.split(', ')[0] if ops else ''
        if dst in ('s0', 'v0', 'd0', 'q0', 'h0', 'b0') or (mn.startswith('ld') and ops.startswith(('s0,', 'd0,', 'q0,'))):
            failures.append(f'main+{a:#x}: {mn} {ops} writes the factor register')
        if mn in ('bl', 'blr'):
            failures.append(f'main+{a:#x}: a call between the load and the ease ({mn} {ops})')
    got = [ins.get(EASE + 4 * i) for i in range(len(WANT_EASE))]
    if got != WANT_EASE:
        failures.append(f'the ease at main+{EASE:#x} is {got}')
    g = {i.address: (i.mnemonic, i.op_str) for i in md.disasm(original[0x9a7130:0x9a7150], 0x9a7130)}
    if g.get(0x9a7148) != ('add', 'x0, x19, #0xf8'):
        failures.append(f'the getter returns {g.get(0x9a7148)}, not x19 + 0xf8')
    return dict(name='footik_ease', status='fail' if failures else 'pass', checked=4, failures=failures)
