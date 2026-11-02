"""Tears of the Kingdom 1.4.3 (main build id 277178B7DBA1B6D4...) code patches, written for that build directly.

The porter (port.py) places little of the 1.0.0 set in 1.4.3 (the update recompiled most functions), so these sites are
1.4.3's own: ported from MacNativeGames (MIT, Copyright (c) 2026 MacNativeGames contributors; THIRD_PARTY_NOTICES.md),
games/totk/runtime/game_patches_v143.c at upstream 73c4821 and 3e9e7f67 (fades), each site and the registers it uses
checked against the 1.4.3 main. Same model as sites_v100.py: no word of the game here (a re-run instruction is
Orig(offset)), veneers in main's text padding, the frame step from the TotK data page (include/totk/data_page.h),
which sits after 1.4.3's own span (DATA_PAGE_OFFSET; the runtime finds it from TOTK_MAIN_SPAN, build.py).

`python -m games.totk.patches port --version 1.4.3 ...` writes v143.toml from these on the player's PC (port.py: a
version with its own sites module takes them instead of the porter); like every version table it never ships. Not
here yet: fps60 (1.4.3's controller has no place for the 1.0.0 runtime's minimum interval hook, and the frame-count
hooks 1.4.3 lost keep it at 30 anyway: engine-map safe_mode cap30), output_res (an output above 1920x1080 needs the
HeapDef overlay, made from 1.0.0's files), shadowres (1.4.3 sizes the map differently), the LOD / mouse features.
"""
from . import arm64 as a
from .sites_v100 import (DP_ASPECT, DP_FOV_BASE, DP_FOV_SCALE, DP_FRAME_RATIO, DP_FRAME_SECONDS, DP_RENDER_SIZE,
                         DP_RES_FLOOR, FMOV_S31_FOV_MAX, Layout, Orig, Patch)

VERSION = '1.4.3'
MAIN_NSO_SHA256 = 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e'
MAIN_IMAGE_SHA256 = '41220e5d1598a7d76a976da70b053b588bca02ca99b935f71a9f4ee9c74ae4f0'
MAIN_BUILD_ID = '277178b7dba1b6d4949a84778d4abc58b31b34f5'
MAIN_SPAN = 0x3adf000
TEXT_END = 0x2bb9c00        # 1024 bytes of padding to the next page (1.0.0: 1520)
DATA_PAGE_OFFSET = (MAIN_SPAN + 0x3fff) & ~0x3fff

# Text padding use, from layout.pad (the rest, +528 .. +1024, is free)
PAD_REALTIME = 0      # 20-byte frame-step veneer
PAD_BOUNCE = 32       # 16-byte trampoline veneer
PAD_CLOTH = 64        # 2 x 28-byte clock veneers, 2 x 20-byte call veneers (to +160)
PAD_FADE = 160        # 4 x 20-byte fade veneers (to +240)
PAD_DYNRES = 240      # 20-byte dynamic-resolution floor veneer
PAD_ASPECT = 272      # 4 x 32-byte aspect stubs (to +400)
PAD_FOV = 400         # 2 x 64-byte field-of-view veneers (to +528)


def layout():
    return Layout(text_end=TEXT_END, data_off=DATA_PAGE_OFFSET)


def game(lay=None):
    """The [game] section of v143.toml (games.totk.patches.table_toml, load_main_image)."""
    lay = lay or layout()
    return dict(title_id='0100F2C0115B6000', version=VERSION, build_id=MAIN_BUILD_ID, nso_sha256=MAIN_NSO_SHA256,
                image_sha256=MAIN_IMAGE_SHA256, text_end=lay.text_end, span=MAIN_SPAN, data_page=lay.data_off)


# The frame-step producer (in the function around main+0x3e5630) gives FrameRateMgr s8 = 60 / [fps] (the step in 60 Hz
# ticks) * a 0.5 / 1.0 coefficient (sys+0x1da bit 0, the fcsel just before); Framework's per-frame update (main+0x2daa30,
# 1.0.0's procFrame_ main+0x8f689c; the interval controller at Framework+0x200) stores the global 30 Hz ratio
# 60 / [x19+0x250] * 0.5. The equipment cooldown at player state +0x109c steps only while above FLT_EPSILON
# (main+0x2b036c) but the equipment gate (main+0x31cc7c) waits for 0: a measured ratio can leave a remainder in
# (0, FLT_EPSILON] that is never stepped again and locks weapons, shields and bows until a load (MacNativeGames #18).
STEP_SITE = 0x3e5658        # fmul s8, s0, s1
RATIO_SITE = 0x2daa54       # ldr s0, [x19, #0x250] .. fmul s0, s0, s1 at +0x24
EQUIP_COOLDOWN = 0x2b036c   # movi v1.2s, #0x34, lsl #24 (FLT_EPSILON)
MOVI_V1_2S_ZERO = 0x0f000401


def realtime(lay):
    """Real-time frame step: the producer's step in 60 Hz ticks (2 * the data page's ratio: seconds * 60) and the global
    ratio (+0, seconds * 30) come from the data page instead of the present interval. The runtime writes both at every
    present (totk_frame_step_update), as for 1.0.0."""
    p = Patch('realtime')
    d, at = lay.data_off, lay.pad + PAD_REALTIME
    p.put(STEP_SITE, a.b(STEP_SITE, at), 'frame step: fmul s8, s0, s1 -> b <step veneer>')
    veneer = [a.adrp(8, at, d),                       # x8 is free: the ldxr two words on loads it
              a.ldr_s(0, 8, DP_FRAME_RATIO),          # ldr s0, [x8] (ratio = seconds * 30)
              a.fadd_s(0, 0, 0),                      # s0 = seconds * 60, the step in 60 Hz ticks (was 60 / fps)
              Orig(STEP_SITE),                        # the site's fmul s8, s0, s1 (the coefficient stays)
              a.b(at + 16, STEP_SITE + 4)]
    p.code(at, veneer, 'frame step veneer')
    r = RATIO_SITE
    p.put(r, a.adrp(9, r, d), 'ldr s0, [x19, #0x250] -> adrp x9, <data page> (x9: the ldrh at +0x14 loads it)')
    p.put(r + 4, a.ldr_s(0, 9, DP_FRAME_RATIO), 'mov w8, #60.0 -> ldr s0, [x9] (global ratio)')
    p.put(r + 0x18, a.NOP, 'scvtf s0, s0 -> nop')
    p.put(r + 0x1c, a.NOP, 'fdiv s0, s1, s0 -> nop')
    p.put(r + 0x20, a.NOP, 'fmov s1, #0.5 -> nop')
    p.put(r + 0x24, a.NOP, 'fmul s0, s0, s1 -> nop')
    p.put(EQUIP_COOLDOWN, MOVI_V1_2S_ZERO, 'equipment cooldown: movi v1.2s, #FLT_EPSILON -> #0 (step while above 0)')
    return p


# Hair and cloth (MacNativeGames #79): the actor animation update builds a second clock {seconds = factor / 30,
# ratio = factor} per rendered frame. 1.4.3 inlines it in two actor paths, where the factor was just stored to the ratio
# slot (fmul s<r>, s<f>, s<r>: s<r> = 1/30): the veneer stores measured ratio * factor there and computes measured
# seconds * factor. Two other callers share the out-of-line update (main+0x2a0e40), whose s2 (seconds) and s3 (ratio) the
# call veneer scales by the measured ratio; its third caller already passes measured time and stays. x8 and s4 are free
# at all four sites (x8: loaded again before it is read; the update's first word loads x8, s4 is not an argument).
CLOTH_INLINE = ((0x246eac, 0, 10, 0x364), (0x345904, 1, 0, 0x114))   # (fmul site, r, f, ratio slot [sp, #off])
CLOTH_CALLS, CLOTH_UPDATE = (0x24944c, 0x28bc98), 0x2a0e40


def cloth(lay):
    """Hair and cloth at real-time speed. Needs `realtime` (the same data page words)."""
    p = Patch('cloth')
    d, at = lay.data_off, lay.pad + PAD_CLOTH
    for site, r, f, slot in CLOTH_INLINE:
        veneer = [a.adrp(8, at, d),
                  a.ldr_s(r, 8, DP_FRAME_RATIO),
                  a.fmul_s(r, f, r),                  # measured ratio * factor
                  a.str_s(r, 31, slot),               # into the ratio slot (the factor was stored there)
                  a.ldr_s(r, 8, DP_FRAME_SECONDS),
                  Orig(site),                         # the site's fmul: measured seconds * factor
                  a.b(at + 24, site + 4)]
        p.put(site, a.b(site, at), f'cloth clock: fmul s{r}, s{f}, s{r} -> b <clock veneer>')
        at = p.code(at, veneer, f'cloth clock veneer for main+{site:#x}')
    for site in CLOTH_CALLS:                          # one block per call: engine.layout enters a block from one site
        call = [a.adrp(8, at, d), a.ldr_s(4, 8, DP_FRAME_RATIO),
                a.fmul_s(2, 2, 4), a.fmul_s(3, 3, 4),   # s2 *= ratio, s3 *= ratio
                a.b(at + 16, CLOTH_UPDATE)]
        p.put(site, a.bl(site, at), 'cloth clock: bl <update> -> bl <call veneer>')
        at = p.code(at, call, f'cloth call veneer for main+{site:#x} (tail-branches to the update)')
    return p


BOUNCE_SITE = 0x562a64      # ldr s0, [x9, #0x28]; then mov w9, #100.0; fmov s1, w9; fcmp s0, s1


def highfps(lay):
    """Trampolines above 100 FPS (MacNativeGames #61): the contact code scales the bounce vector by 0.1 when time object
    +0x28 (30 / (ratio * time scale)) is over 100, which above 100 FPS fires with time running normally. The veneer
    compares +0x28 * +0x38 (30 / time scale) instead: the same value at 30 Hz. s1 is set again two words on."""
    p = Patch('highfps')
    at = lay.pad + PAD_BOUNCE
    p.put(BOUNCE_SITE, a.b(BOUNCE_SITE, at), 'trampoline check: ldr s0, [x9, #0x28] -> b <veneer>')
    p.code(at, [Orig(BOUNCE_SITE), a.ldr_s(1, 9, 0x38), a.fmul_s(0, 0, 1), a.b(at + 12, BOUNCE_SITE + 4)],
           'trampoline veneer')
    return p


# Fades (MacNativeGames patch_fade_increment, upstream 3e9e7f67): the distance / LOD mesh fades and the pre-actor fades
# add trunc(2.1 * ratio) to a counter each frame: 2 at 30 fps, 1 at 60, 0 above about 63.5 fps, where a fade stops and
# its object stays translucent or invisible until a slower frame. The veneer floors the increment at 1 without the flags
# (clz is 32 only for 0: increment += clz >> 5). Scratch: a register the next word writes (w10 at 0xa7ac4: ldrb w10;
# w11 at the pre-actor sites: add w11), or at 0x95dd24 a temporary the function no longer reads (add, strb, return).
FADE_SITES = ((0x0a7ac4, 8, 10), (0x95dd24, 8, 10), (0x6155e4, 10, 11), (0x615660, 10, 11))   # (fcvtzs site, w, scratch)


def fade(lay):
    """Fades keep moving above 63 FPS."""
    p = Patch('fade')
    at = lay.pad + PAD_FADE
    for site, w, t in FADE_SITES:
        p.put(site, a.b(site, at), f'fade: fcvtzs w{w}, s0 -> b <floor veneer>')
        at = p.code(at, [Orig(site), a.clz_w(t, w), a.lsr_w(t, t, 5), a.add_w(w, w, t), a.b(at + 16, site + 4)],
                    f'fade floor veneer for main+{site:#x}')
    return p


RENDER_SITE = 0x585e48      # scvtf s0, w11; ldr w2; str s0, [x19, #0xcc4]; scvtf s0, w10; ldr x8; ldr x0; str s0, [x19, #0xcc8]


def render_res(lay):
    """Scene render size from the data page (+16, two floats) instead of the game's parameter, as 1.0.0's render_res:
    every path that picks the size meets here and stores it in the renderer at +0xcc4 / +0xcc8 (the same fields as
    1.0.0). x3 is not used anywhere in the function (nor by its callee at +0x38); s1 lives from the ldp to the second
    store (the words between load integers)."""
    p = Patch('render_res')
    r = RENDER_SITE
    p.put(r, a.adrp(3, r, lay.data_off), 'scvtf s0, w11 -> adrp x3, <data page>')
    p.put(r + 8, a.ldp_s(0, 1, 3, DP_RENDER_SIZE), 'str s0, [x19, #0xcc4] -> ldp s0, s1, [x3, #16]')
    p.put(r + 12, Orig(r + 8), 'scvtf s0, w10 -> the store of s0 from main+0x585e50, one word later')
    p.put(r + 0x18, a.str_s(1, 19, 0xcc8), 'str s0, [x19, #0xcc8] -> str s1, [x19, #0xcc8]')
    return p


# The dynamic-resolution controller's one epilogue (1.0.0 main+0xc622ac): `mov w9, w23` puts the tier's scale (f32 bits)
# beside x8's flag bits before `orr x0, x8, x9`.
DYNRES_SITE = 0x5848a8


def dynres(lay):
    """Dynamic resolution floor, as 1.0.0's dynres: the scale is raised to the data page's floor (+64, f32 bits; 1.0 =
    the full scene size, 0 = the game's own tiers) by one unsigned compare. x11 and the flags are dead (ldp, orr, ldp,
    add sp, ret)."""
    p = Patch('dynres')
    at = lay.pad + PAD_DYNRES
    p.put(DYNRES_SITE, a.b(DYNRES_SITE, at), 'dynres: mov w9, w23 (the tier scale) -> b <floor veneer>')
    p.code(at, [a.adrp(11, at, lay.data_off),
                a.ldr_w(11, 11, DP_RES_FLOOR),
                a.cmp_w(23, 11),
                a.csel_w(9, 23, 11, 'hi'),            # w9 = max(scale, floor)
                a.b(at + 16, DYNRES_SITE + 4)], 'dynamic resolution floor veneer')
    return p


# The two places the camera hands its fovy (radians) to sead's PerspectiveProjection with setFovy inlined (1.0.0's
# FOV_SITES): the per-frame camera update (+0x13c of the camera in x19) and the camera setup (+0x64 of x22). The next
# word, fmul s8, s0, s1, reads s1 (0.5): the veneer keeps everything but s0.
FOV_SITES = (0x628c3c, 0xb4342c)


def fov(lay):
    """Field of view, 1.0.0's veneer word for word at 1.4.3's sites (sites_v100.fov): the fovy recorded on the data
    page (+124) and, while a multiplier sits on the page (+68, 0 = off), multiplied, at most to FOV_MAX."""
    p = Patch('fov')
    d = lay.data_off
    for i, site in enumerate(FOV_SITES):
        at = lay.pad + PAD_FOV + 64 * i
        veneer = [Orig(site),                          # the site's load of the camera's fovy into s0
                  0xd10083ff,                          # sub sp, sp, #32
                  0xf90003ea,                          # str x10, [sp]
                  0x3d8007ff,                          # str q31, [sp, #16]
                  a.adrp(10, at + 16, d),
                  a.str_s(0, 10, DP_FOV_BASE),
                  a.ldr_w(10, 10, DP_FOV_SCALE),
                  a.cbz_w(10, at + 28, at + 48),       # off: keep the game's (to the restore)
                  a.fmov_s_w(31, 10),
                  a.fmul_s(0, 0, 31),
                  FMOV_S31_FOV_MAX,
                  0x1e3f7800,                          # fminnm s0, s0, s31
                  0x3dc007ff,                          # ldr q31, [sp, #16]
                  0xf94003ea,                          # ldr x10, [sp]
                  0x910083ff,                          # add sp, sp, #32
                  a.b(at + 60, site + 4)]
        p.put(site, a.b(site, at), 'fov: ldr s0, <camera fovy> -> b <veneer>')
        p.code(at, veneer, f'fov veneer for main+{site:#x}')
    return p


# The four places 1.4.3 hands an aspect ratio to the projection (+0xb0): the two camera routines above load the camera's
# ratio, two viewport routines divide width by height (MacNativeGames' 1.4.3 aspect set).
ASPECT_SITES = ((0x628c74, 'camera: ldr s0, [x19, #0x140]'), (0xb43460, 'camera setup: ldr s0, [x22, #0x68]'),
                (0x1ff04f4, 'viewport: fdiv s0, s0, s1'), (0x253fbb0, 'viewport: fdiv s0, s0, s1'))


def aspect(lay):
    """Other picture shapes, 1.0.0's stub (sites_v100.aspect) at 1.4.3's sites: the original instruction, then the data
    page's ratio (+56) in s0 while one is set (0 = the game's 16:9). x10 is kept on the stack; no flags."""
    p = Patch('aspect')
    d = lay.data_off
    for i, (site, what) in enumerate(ASPECT_SITES):
        s = lay.pad + PAD_ASPECT + 32 * i
        stub = [Orig(site),
                0xf81f0fea,                            # str x10, [sp, #-16]!
                a.adrp(10, s + 8, d),
                a.ldr_w(10, 10, DP_ASPECT),
                a.cbz_w(10, s + 16, s + 24),           # off: keep the game's
                a.fmov_s_w(0, 10),
                0xf84107ea,                            # ldr x10, [sp], #16
                a.b(s + 28, site + 4)]
        p.put(site, a.b(site, s), f'aspect {what} -> b <stub>')
        p.code(s, stub, f'aspect stub for main+{site:#x}')
    return p


FEATURES = {
    # name: (builder, default on, needs); the names are the 1.0.0 set's where the fix is the same
    'realtime': (realtime, True, ()),
    'cloth': (cloth, True, ('realtime',)),
    'highfps': (highfps, True, ()),
    'fade': (fade, True, ()),
    'render_res': (render_res, True, ()),
    'dynres': (dynres, True, ()),
    'fov': (fov, True, ()),
    'aspect': (aspect, True, ()),
}


def build(lay, features):
    unknown = [f for f in features if f not in FEATURES]
    if unknown:
        raise ValueError(f'unknown features {unknown}; known: {", ".join(FEATURES)}')
    for f in features:
        for need in FEATURES[f][2]:
            if need not in features:
                raise ValueError(f'feature {f} needs {need}')
    out = [FEATURES[f][0](lay) for f in FEATURES if f in features]
    for p in out:
        if max([e for _, e in p.veneers] + [0]) > lay.pad_end:
            raise ValueError(f'{p.feature}: veneers reach past the last text page')
    return out
