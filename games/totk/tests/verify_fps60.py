"""Emulate TotK's adaptive present-interval controller (main+0x8f6bec) from the original and the patched 1.0.0 image
and check the intervals it picks. Ported from MacNativeGames (MIT) games/totk/scripts/verify_fps60_patch.py; here
the patched bytes come from the image `python -m games.totk.patches apply` wrote, not from a C table.

Shipping code clamps the interval to 2..3 (30 / 20 fps). With the patch and the controller's minimum at 1 (what the
runtime's totk_interval_min.cpp writes for any setting but "30") the controller must reach 1 (60 fps) under light frame
work, rise to 2 and 3 under heavier work, step back down when work is light again, honour a listener veto, and never
produce 0. With the patch and the minimum at 2 (the "30" setting) it must behave as the Switch: never below 2. The
unpatched controller is the negative control: it must never go below 2."""
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1

from .common import adrp_ldr

FN, FN_END = 0x8f6c60 - 0x74, 0x8f6d98 + 8
LOWER_TRIGGER = 0x8f6c88
TICK_HZ = 19_200_000
STOP, STUBS, HEAP, STACK = 0x10000, 0x20000, 0x100000, 0x200000
ASM = {   # tiny stub routines (pre-assembled AArch64)
    'ret': [0xd65f03c0],
    'record_w1': [0xd2a00048, 0xb9000101, 0xd65f03c0],   # movz x8, #0x2, lsl #16 (0x20000); str w1, [x8]; ret
    'veto': [0x52800028, 0x39004028, 0xd65f03c0],        # mov w8, #1; strb w8, [x1, #0x10]; ret
}


def function_bytes(original, patched, use_patch, skip=()):
    code = bytearray((patched if use_patch else original)[FN:FN_END])
    for off in skip:
        code[off - FN:off - FN + 4] = original[off:off + 4]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for ins in md.disasm(bytes(code), FN):   # every direct branch must stay inside the function
        if ins.mnemonic in ('b', 'bl') or ins.mnemonic.startswith(('b.', 'cb', 'tb')):
            tgt = int(ins.op_str.split('#')[-1], 16)
            if not FN <= tgt < FN_END:
                raise AssertionError(f'branch at {ins.address:#x} leaves the controller: {ins.mnemonic} {ins.op_str}')
    return bytes(code)


class Controller:
    def __init__(self, original, patched, use_patch, cand, cooldown, current=2, bias=0.0, threshold=3, cd_init=30,
                 listener=None, skip=(), minimum=2):
        tick_got = adrp_ldr(original, FN + 0x38, FN + 0x3c)
        window_got = adrp_ldr(original, FN + 0xbc, FN + 0xc4)
        mu = self.mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(FN & ~0xfff, 0x2000)
        mu.mem_write(FN, function_bytes(original, patched, use_patch, skip))
        mu.mem_map(STOP, 0x1000)
        mu.mem_write(STOP, struct.pack('<I', 0xd4200000))   # brk #0
        mu.mem_map(STUBS, 0x1000)
        stub = {}
        for i, (name, words) in enumerate(ASM.items()):
            stub[name] = STUBS + 0x100 + 0x40 * i
            mu.mem_write(stub[name], struct.pack('<%dI' % len(words), *words))
        for page in {tick_got & ~0xfff, window_got & ~0xfff}:
            mu.mem_map(page, 0x1000)
        mu.mem_map(HEAP, 0x10000)
        mu.mem_map(STACK, 0x10000)
        mu.mem_write(HEAP + 0x8000, struct.pack('<Q', TICK_HZ))
        mu.mem_write(tick_got, struct.pack('<Q', HEAP + 0x8000))
        mu.mem_write(HEAP + 0x8010, struct.pack('<Q', stub['record_w1']))
        mu.mem_write(window_got, struct.pack('<Q', HEAP + 0x8010))
        self.sys, self.ctrl, holder = HEAP, HEAP + 0x248, HEAP + 0x4000
        mu.mem_write(self.sys + 0x140, struct.pack('<Q', holder))
        mu.mem_write(holder + 0x20, struct.pack('<Q', 0x7000))   # non-null window
        mu.mem_write(self.ctrl, struct.pack('<IIfIIIIIIB', minimum, 3, bias, threshold, cd_init, current, cand, 0, cooldown, 0))
        sentinel = self.ctrl + 0x30
        if listener:
            node, obj, vt = HEAP + 0x5000, HEAP + 0x5100, HEAP + 0x5200
            mu.mem_write(node + 8, struct.pack('<QQ', sentinel, obj))
            mu.mem_write(obj + 0x28, struct.pack('<Q', vt))
            mu.mem_write(vt, struct.pack('<Q', stub[listener]))
            mu.mem_write(sentinel, struct.pack('<QQ', node, node))
        else:
            mu.mem_write(sentinel, struct.pack('<QQ', sentinel, sentinel))

    def frame(self, work_ms):
        mu = self.mu
        mu.mem_write(self.sys + 0x118, struct.pack('<q', int(work_ms * TICK_HZ / 1000)))
        mu.mem_write(0x20000, struct.pack('<I', 0xffffffff))
        mu.reg_write(UC_ARM64_REG_X0, self.ctrl)
        mu.reg_write(UC_ARM64_REG_X1, self.sys)
        mu.reg_write(UC_ARM64_REG_SP, STACK + 0x8000)
        mu.reg_write(UC_ARM64_REG_LR, STOP)
        mu.emu_start(FN, STOP, count=4000)
        if mu.reg_read(UC_ARM64_REG_PC) != STOP:
            raise AssertionError('controller did not return')
        cur = struct.unpack('<I', mu.mem_read(self.ctrl + 0x14, 4))[0]
        sys88 = struct.unpack('<I', mu.mem_read(self.sys + 0x88, 4))[0]
        window = struct.unpack('<I', mu.mem_read(0x20000, 4))[0]
        if not cur == sys88 == window:
            raise AssertionError(f'interval copies disagree: ctrl {cur} sys {sys88} window {window}')
        return cur


def _run(ctrl, plan):
    out = []
    for ms, n in plan:
        out += [ctrl.frame(ms) for _ in range(n)]
    return out


def verify(original, patched):
    LIGHT, MEDIUM, HEAVY = 8.0, 25.0, 40.0
    results, failures = [], []

    def check(name, ok, detail):
        results.append(dict(case=name, ok=bool(ok), detail=detail))
        if not ok:
            failures.append(name)

    plan = [(LIGHT, 200), (MEDIUM, 60), (HEAVY, 60), (LIGHT, 300)]
    for cand in (0, 2, 3):
        for cooldown in (0, 30):
            for listener in (None, 'ret'):
                tag = f'cand={cand} cooldown={cooldown} listener={listener or "none"}'
                seq = _run(Controller(original, patched, True, cand, cooldown, listener=listener, minimum=1), plan)
                first1 = seq.index(1) if 1 in seq else None
                check(f'patched {tag}: never 0', 0 not in seq, f'min {min(seq)}')
                check(f'patched {tag}: light reaches 1', first1 is not None and all(v == 1 for v in seq[first1:200]), f'first at {first1}')
                check(f'patched {tag}: medium rises to 2', seq[259] == 2, f'frame 259 = {seq[259]}')
                check(f'patched {tag}: heavy rises to 3', seq[319] == 3, f'frame 319 = {seq[319]}')
                check(f'patched {tag}: light steps back to 1', seq[-1] == 1, f'last {seq[-1]}')
                base = _run(Controller(original, patched, False, max(cand, 2), cooldown, listener=listener), plan)
                check(f'unpatched {tag}: never below 2 (negative control)', min(base) >= 2, f'min {min(base)}')
                at30 = _run(Controller(original, patched, True, max(cand, 2), cooldown, listener=listener), plan)
                check(f'patched, minimum 2 ("30") {tag}: never below 2, heavy 3, back to 2',
                      min(at30) >= 2 and at30[319] == 3 and at30[-1] == 2, f'min {min(at30)} last {at30[-1]}')
    no_trigger = _run(Controller(original, patched, True, 2, 0, listener='ret', skip={LOWER_TRIGGER}, minimum=1),
                      [(LIGHT, 300)])
    check('negative control: without the lower-trigger site a stored candidate of 2 never reaches 1', 1 not in no_trigger,
          f'seen {sorted(set(no_trigger))}')
    veto = _run(Controller(original, patched, True, 2, 0, listener='veto', minimum=1), [(LIGHT, 300)])
    check('patched with a vetoing listener stays at 2', set(veto) == {2}, f'seen {sorted(set(veto))}')
    biased = _run(Controller(original, patched, True, 2, 0, bias=0.5, minimum=1), [(LIGHT, 200), (12.0, 200)])
    check('bias 0.5: 8 ms frames reach 1, 12 ms frames go back to 2', 1 in biased[:200] and biased[-1] == 2, f'last {biased[-1]}')
    return dict(name='fps60', status='fail' if failures else 'pass', checked=len(results), failures=failures)
