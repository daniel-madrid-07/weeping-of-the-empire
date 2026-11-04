"""The abilities' camera pitch (ExecutePlayerCameraSpecialPower's mode update main+0x1ce6eb0) as totk_ability_pitch.cpp
(hook_f_1347600 at lr main+0x1ce78fc) relies on it, on the 1.0.0 image's own words: `bl 0x1cd3678` (PlayerCamera's
pitch speed getter, `ldr s0, [x0, #0xc2c]`) then `fneg s9, s0`; `bl 0x1347600` (the mode's LatStickScale getter) then
`ldr s0, [x0]`, `fmul s1, s0, s9`, `fmul s12, s12, s1` (the per-call pitch: stick y x LatStickScale x -speed, no frame
time) and `b 0x1ce7740`, whose first instructions reload s0 / s1 (main+0x1ce78e8..0x1ce7908). Also checks the getter's
+0x5c and that the camera pre-update stores the speed straight from a parameter read (`ldr s0, [x0]; str s0, [x19,
#0xc2c]` inside main+0xd057e4), and the yaw's LngStickScale read the live-scales hook publishes (`bl 0x1347654; ldr s0,
[x0]` at main+0x1ce7680, the getter's +0x64). The hooks do not patch the image: `original` is the one checked."""
import capstone

SPEED_CALL, CALL, AFTER, END = 0x1ce78e8, 0x1ce78f8, 0x1ce78fc, 0x1ce790c
WANT = {0x1ce78e8: ('bl', '#0x1cd3678'), 0x1ce78f0: ('fneg', 's9, s0'), 0x1ce78f8: ('bl', '#0x1347600'),
        0x1ce78fc: ('ldr', 's0, [x0]'), 0x1ce7900: ('fmul', 's1, s0, s9'), 0x1ce7904: ('fmul', 's12, s12, s1'),
        0x1ce7908: ('b', '#0x1ce7740')}
YAW = {0x1ce7680: ('bl', '#0x1347654'), 0x1ce7684: ('ldr', 's0, [x0]')}   # the yaw's LngStickScale read (live scales)


def _ins(md, image, lo, hi):
    return {i.address: (i.mnemonic, i.op_str) for i in md.disasm(image[lo:hi], lo)}


def verify(original, patched=None):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    failures = []
    ins = _ins(md, original, SPEED_CALL, END)
    for a, want in WANT.items():
        if ins.get(a) != want:
            failures.append(f'main+{a:#x} is {ins.get(a)}, not {want}')
    yaw = _ins(md, original, 0x1ce7680, 0x1ce7688)
    for a, want in YAW.items():
        if yaw.get(a) != want:
            failures.append(f'main+{a:#x} is {yaw.get(a)}, not {want}')
    lng = _ins(md, original, 0x1347654, 0x13476a8)
    if not any(ops.endswith('#0x64') for _, ops in lng.values()):
        failures.append('the LngStickScale getter has no +0x64')
    back = _ins(md, original, 0x1ce7740, 0x1ce774c)
    if back.get(0x1ce7740, ('', ''))[1].split(',')[0] != 's0' or back.get(0x1ce7748, ('', ''))[1].split(',')[0] != 's1':
        failures.append(f'main+0x1ce7740 does not reload s0 / s1 first: {back}')
    g = _ins(md, original, 0x1cd3678, 0x1cd3680)
    if g.get(0x1cd3678) != ('ldr', 's0, [x0, #0xc2c]'):
        failures.append(f'the speed getter is {g.get(0x1cd3678)}, not ldr s0, [x0, #0xc2c]')
    lat = _ins(md, original, 0x1347600, 0x1347654)
    if not any(ops.endswith('#0x5c') for _, ops in lat.values()):
        failures.append('the LatStickScale getter has no +0x5c')
    pre = sorted(_ins(md, original, 0xd057e4, 0xd06b68).items())
    stores = [i for i, (a, (mn, ops)) in enumerate(pre) if (mn, ops) == ('str', 's0, [x19, #0xc2c]')]
    if not stores or any(pre[i - 1][1] != ('ldr', 's0, [x0]') for i in stores):
        failures.append('the camera pre-update does not store +0xc2c straight from a parameter read')
    return dict(name='ability_pitch', status='fail' if failures else 'pass', checked=7, failures=failures)
