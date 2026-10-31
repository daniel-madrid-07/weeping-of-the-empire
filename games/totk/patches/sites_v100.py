"""Tears of the Kingdom 1.0.0 (main build id 082CE09B06E33A12...) code patches, as high-level definitions.

`python -m games.totk.patches generate` turns these into the table v100.toml (module, offset, new word, reason,
feature); `apply` reads only the TOML. `check` regenerates and compares, so the two never drift. Everything here is a
main offset in the 1.0.0 image. Sites and veneer code are ported from MacNativeGames (MIT):
games/totk/runtime/game_patches.c (its 1.0.0 block, GAME_ADDR(x) = x) and aspect_math.h.

No word of the game is written here or in the table. What a patch replaces is checked against the user's image by a
SHA-256 per feature (of every word the feature touches or copies, taken from the image when the table is generated);
a veneer that runs a replaced instruction elsewhere names it, Orig(offset), and apply copies it from the image.

Unlike MacNativeGames, which patches the loaded image at start, these patches go in before recompilation, so every value a
player may want to change (frame step, output size, swapchain size, scene size, aspect ratio) is read by the patched
code from the TotK data page, a page right after main's image that the runtime commits and writes
(include/totk/data_page.h). One recompiled build serves every setting; only the on/off of each code patch is baked.

Veneers (code that does more than the one replaced instruction) live in main's text padding: the zero bytes between the
end of .text (text_end) and the next page. `b <veneer>` replaces the site and the veneer ends with `b <site + 4>`.
"""
from dataclasses import dataclass, field

from . import arm64 as a

DATA_PAGE_SIZE = 0x4000
# Data page slots (byte offsets). Keep in step with include/totk/data_page.h.
DP_FRAME_RATIO, DP_FRAME_SECONDS = 0, 4         # f32: 30 Hz ratio (seconds * 30), measured frame seconds
DP_RENDER_SIZE = 16                             # f32 w, f32 h: scene render size
DP_OUTPUT_SIZE, DP_RENDERER = 32, 40            # u32 w, u32 h: the game's output (canvas) size; u64 renderer (game)
DP_SWAPCHAIN_SIZE = 48                          # u32 w, u32 h: the swapchain size the framework is created with
DP_ASPECT = 56                                  # f32: aspect ratio for the camera, 0 = the game's own 16:9
DP_RES_FLOOR = 64                               # f32: lowest dynamic-resolution scale, 1.0 = fixed, 0 = the game's own
DP_FOV_SCALE = 68                               # f32: camera field-of-view multiplier, 0 = the game's own
DP_SHADOW_SCALE = 72                            # f32: depth shadow map size multiplier, 0 = the game's own
DP_LOD_SCALE = 76                               # f32: model LOD distance multiplier, 0 = the game's own
DP_MOUSE_SCALE = 80                             # f32: mouse camera multiplier (setting), 0 = off (platform's stick)
DP_MOUSE_RATE = 84                              # f32: stick deflection per mouse count at the 30 Hz ratio (runtime)
DP_PRESENTS = 88                                # u32: presents so far (runtime)
DP_MOUSE_BEAT = 92                              # u32 (game): DP_PRESENTS at the last camera input step
DP_MOUSE_LAST = 96                              # f64 x, f64 y (game): the PC page's mouse totals already applied
DP_ACTOR_LOD = 112                              # f32: actor LOD area distance multiplier, 0 = the game's own
DP_TERRAIN_LOD = 116                            # f32: terrain LoD distance multiplier, 0 = the game's own
DP_SIM_RADIUS = 120                             # f32: actor simulation distance multiplier, > 0 (1 = the game's own)
DP_FOV_BASE = 124                               # f32 (game): the camera's own fovy (radians) at the last fov site
DP_MOUSE_PITCH = 128                            # f32: mouse pitch gain g, y = sign(v) sqrt(g |v|); <= 0 = linear (runtime)
DP_MOUSE_PITCH_ACK = 132                        # u32 (game): 1 = the mouse veneer shapes pitch (set at each applied step)
DP_CAMERA = 136                                 # u64 (game): the PlayerCamera of the last camera pre-update
# +144..+175 (runtime): the mouse late latch's live block, TOTK_DP_LATCH_* (seq, flags, totals, QPC); no veneer reads
# it (the render thread does). +176..+207: reserved for the per-consume block (a new key)
DP_PC = 0x100                                   # platform's PlatPcPage (0x80 bytes): mouse totals, glyphs, overlay
PC_MOUSE_X, PC_MOUSE_Y, PC_CAPTURED, PC_CLAIMED = 0x08, 0x10, 0x20, 0x64   # offsets inside PlatPcPage
MOUSE_STALE = 3        # presents without a step: motion dropped, claim released (= data_page.h; verify_sites)


@dataclass(frozen=True)
class Orig:
    """The 1.0.0 word at main+offset, copied from the user's image when the patch is applied (a relocated instruction)."""
    offset: int


@dataclass
class Word:
    offset: int          # main offset
    new: object          # int, or Orig: the image's own word at another offset
    reason: str
    pad: bool = False    # in the text padding (zero in the image)
    original: int = None   # the image's word at offset, once read from the image (with_image: the porter needs it)


@dataclass
class Patch:
    feature: str
    words: list = field(default_factory=list)
    veneers: list = field(default_factory=list)   # [start, end) ranges of the veneer code this patch adds in the padding

    def put(self, offset, new, reason):
        self.words.append(Word(offset, new, reason))

    def code(self, at, words, reason):
        """Veneer code at `at` (text padding: zero in the image)."""
        for i, w in enumerate(words):
            self.words.append(Word(at + 4 * i, w, f'{reason} [{i}]', pad=True))
        lo, hi = at, at + 4 * len(words)
        if self.veneers and self.veneers[-1][1] == lo:       # contiguous with the last range: extend it
            self.veneers[-1] = (self.veneers[-1][0], hi)
        else:
            self.veneers.append((lo, hi))
        return hi


@dataclass
class Layout:
    """What the patches need to know about the image: the end of .text, and the data page's offset from main's base."""
    text_end: int
    data_off: int

    @property
    def pad(self):            # first 16-byte boundary in the text padding
        return (self.text_end + 15) & ~15

    @property
    def pad_end(self):        # end of the last text page (4 KiB pages: the next segment starts there at the earliest)
        return (self.text_end + 0xfff) & ~0xfff


# Text padding use, from layout.pad (same places as MacNativeGames' 1.0.0 / 1.2.1 builds):
PAD_ASPECT = 0        # 4 x 32-byte aspect stubs (+0 .. +128)
PAD_BOUNCE = 128      # 20-byte trampoline veneer
PAD_CLOTH = 160       # cloth / hair veneers (224 bytes)
PAD_DYNRES = 384      # 20-byte dynamic-resolution floor veneer
PAD_FOV = 416         # 2 x 64-byte field-of-view veneers
PAD_SHADOW = 544      # 52-byte shadow map size veneer
PAD_LOD = 608         # 3 x 52-byte LOD metric veneers, 64 apart
PAD_MOUSE = 800       # mouse camera veneers: the camera pointer (16 bytes), the input veneer (+16, 176 bytes)
PAD_ACTOR_LOD = 992   # 56-byte actor LOD distance veneer
PAD_TERRAIN_LOD = 1056   # terrain LoD veneers: 3 x 72-byte guarded, 3 x 52-byte, 1 x 56-byte (428 bytes, to +1484)
PAD_MOUSE_PITCH = 1484   # 36-byte mouse pitch tail of the input veneer (to +1520, the end of the padding in 1.0.0)

TEXT_END_V100 = 0x2aa6a10   # main .text size in 1.0.0 (checked against the image at apply)


def fps60():
    """60 FPS: the adaptive present-interval controller (main+0x8f6bec, Framework + 0x248: {min, max} at +0 / +4, the
    current interval at +0x14) may pick a lower interval when its stored candidate allows it. The minimum is the
    controller's own field, as in the game: the runtime (totk_interval_min.cpp, hook on procFrame_ main+0x8f689c) writes
    it before each frame from the live frame-rate setting, 2 at "30" (the Switch's controller) and 1 otherwise (60 Hz).
    It still raises the interval to 2 or 3 when a frame's work does not fit, so slow scenes fall back at correct speed.
    (Until 2026-10-27 the sites below hard-coded minimum 1: "30" then ran at 60 and present's pacer clamp made the
    controller flap 1 <-> 2 against 33 ms frames"""
    p = Patch('fps60')
    p.put(0x8f6c88, a.NOP, 'lower trigger: nop the stored-candidate load (try lower when desired < current)')
    p.put(0x8f6d98, 0x1a9fa555, 'lower path: csinc w21, w10, wzr, ge (result never below 1, a guard: the '
          'clamps keep it >= the minimum)')
    return p


def realtime(lay):
    """Real-time frame step: the variable-frame-rate step (main+0x7a83e4) and the global 30 Hz ratio (main+0x8f68b4)
    read the measured frame time from the data page (+0 ratio = seconds * 30, +4 seconds) instead of deriving it from the
    present interval. The runtime writes both at every present (totk_frame_step_update)."""
    p = Patch('realtime')
    d = lay.data_off
    p.put(0x7a83f4, a.adrp(9, 0x7a83f4, d), 'scvtf s0, w1 -> adrp x9, <data page>')
    p.put(0x7a83f8, a.ldr_s(0, 9, DP_FRAME_RATIO), 'mov w8, #0x42700000 (60.0) -> ldr s0, [x9] (ratio)')
    p.put(0x7a83fc, a.ldr_s(1, 9, DP_FRAME_SECONDS), 'fmov s1, w8 -> ldr s1, [x9, #4] (seconds)')
    p.put(0x7a8400, a.NOP, 'fmov s2, #0.5 -> nop')
    p.put(0x7a840c, a.NOP, 'fdiv s1, s0, s1 -> nop')
    p.put(0x7a8410, a.NOP, 'fmul s0, s0, s2 -> nop')
    p.put(0x8f68b8, a.adrp(9, 0x8f68b8, d), 'ldr s0, [x19, #0x25c] -> adrp x9, <data page>')
    p.put(0x8f68bc, a.ldr_s(0, 9, DP_FRAME_RATIO), 'fmov s1, #0.5 -> ldr s0, [x9] (global ratio)')
    p.put(0x8f68cc, a.NOP, 'scvtf s0, s0 -> nop')
    p.put(0x8f68d0, a.NOP, 'fmul s0, s0, s1 -> nop')
    return p


# One inline place of the hair / cloth clock: fmul s<r>, s<f>, s<r> (s<r> = 1/30, s<f> = the time factor) at fmul; the
# factor goes into the clock's ratio slot at store: str s<f>, [sp, #off] or (pair) stp s<r>, s<f>, [x29, #off].
CLOTH_INLINE = ((0x7b5988, 0x7b598c, 0, 8, -0x40, True), (0x7b6b28, 0x7b6b2c, 0, 8, -0x40, True),
                (0x7f5ec4, 0x7f5ed0, 1, 0, 0x114, False))
CLOTH_CALL, CLOTH_UPDATE = 0x7b6358, 0x82c224


def cloth(lay):
    """Hair and cloth at real-time speed (MacNativeGames #79): the actor animation update builds a second clock
    {seconds = factor / 30, ratio = factor} every rendered frame, so above 30 FPS hair and cloth swing too fast (4x at
    120 FPS). Each of the three inline places gets two veneers (seconds = measured seconds * factor at the fmul; ratio =
    measured ratio * factor at the store), and the call into the shared update scales s2 / s3 by the measured ratio.
    The veneers save the registers they use on the stack. Needs `realtime` (it reads the same data page words)."""
    p = Patch('cloth')
    d, at = lay.data_off, lay.pad + PAD_CLOTH
    for fmul, store, r, f, off, pair in CLOTH_INLINE:
        sec = [0xf81f0fe9,                              # str x9, [sp, #-16]!
               a.adrp(9, at + 4, d),
               a.ldr_s(r, 9, DP_FRAME_SECONDS),         # ldr s<r>, [x9, #4]
               Orig(fmul),                              # the site's fmul s<r>, s<f>, s<r>
               0xf84107e9,                              # ldr x9, [sp], #16
               a.b(at + 20, fmul + 4)]
        p.put(fmul, a.b(fmul, at), f'cloth clock: fmul s{r}, s{f}, s{r} -> b <seconds veneer>')
        at = p.code(at, sec, f'cloth seconds veneer for main+{fmul:#x}')
        store_new = a.stp_s(r, 31, 29, off) if pair else a.str_s(31, 31, off + 32)
        rat = [0xd10083ff,                              # sub sp, sp, #32
               0xf90003e9,                              # str x9, [sp]
               0x3d8007ff,                              # str q31, [sp, #16]
               a.adrp(9, at + 12, d),
               a.ldr_s(31, 9, DP_FRAME_RATIO),          # ldr s31, [x9]
               a.fmul_s(31, f, 31),                     # fmul s31, s<f>, s31
               store_new,                               # the original store with s31 for the factor
               0x3dc007ff,                              # ldr q31, [sp, #16]
               0xf94003e9,                              # ldr x9, [sp]
               0x910083ff,                              # add sp, sp, #32
               a.b(at + 40, store + 4)]
        p.put(store, a.b(store, at), f'cloth clock: ratio store -> b <ratio veneer>')
        at = p.code(at, rat, f'cloth ratio veneer for main+{store:#x}')
    # the call: s2 = factor / 30, s3 = factor; x8 and s4 are caller-saved and not arguments of the update
    call = [a.adrp(8, at, d), a.ldr_s(4, 8, DP_FRAME_RATIO),
            a.fmul_s(2, 2, 4), a.fmul_s(3, 3, 4),       # s2 *= ratio, s3 *= ratio
            a.b(at + 16, CLOTH_UPDATE)]
    p.put(CLOTH_CALL, a.bl(CLOTH_CALL, at), 'cloth clock: bl <update> -> bl <call veneer>')
    at = p.code(at, call, 'cloth call veneer (tail-branches to the update)')
    if at > lay.pad_end:
        raise ValueError('cloth veneers reach past the last text page')
    return p


BOUNCE_SITE = 0xce3fec


def highfps(lay):
    """Trampolines above 100 FPS (MacNativeGames #61): main+0xce3fec compares X+0x28 = 30 / (ratio * time scale) with
    100 and, when greater, scales the velocity Link takes from the ground body by 0.1. Above 100 FPS that fires with time
    running normally. The veneer compares X+0x28 * X+0x38 (= 30 / time scale) instead: same value at 30 Hz."""
    p = Patch('highfps')
    stub = lay.pad + PAD_BOUNCE
    p.put(BOUNCE_SITE, a.b(BOUNCE_SITE, stub), 'trampoline check: ldr s0, [x8, #0x28] -> b <veneer>')
    # The two following words stay (guards: checked, not changed); the veneer does the mov itself and returns to the fmov.
    p.put(BOUNCE_SITE + 4, Orig(BOUNCE_SITE + 4), 'guard: the 100.0 constant, skipped by the veneer (checked, kept)')
    p.put(BOUNCE_SITE + 8, Orig(BOUNCE_SITE + 8), 'guard: where the veneer returns (checked, kept)')
    veneer = [Orig(BOUNCE_SITE),            # the site's own load of X+0x28 into s0
              a.ldr_s(1, 8, 0x38),          # ldr s1, [x8, #0x38] (the 30 Hz ratio; the next original instruction sets s1 again)
              a.fmul_s(0, 0, 1),            # fmul s0, s0, s1
              Orig(BOUNCE_SITE + 4),        # the 100.0 constant into w8
              a.b(stub + 16, BOUNCE_SITE + 8)]
    p.code(stub, veneer, 'trampoline veneer')
    return p


def render_res(lay):
    """Scene render size from the data page (+16, two floats) instead of the game's parameter (1600x900 docked,
    1280x720 handheld): main+0xc63064 stores it in the renderer at +0xcc4 / +0xcc8, every scene render target is sized
    from there. The runtime writes the size before the game starts (default 1600x900, the docked size)."""
    p = Patch('render_res')
    p.put(0xc630bc, a.adrp(9, 0xc630bc, lay.data_off), 'scvtf s0, s8 -> adrp x9, <data page>')
    p.put(0xc630c4, a.ldp_s(0, 1, 9, DP_RENDER_SIZE), 'str s0, [x19, #0xcc4] -> ldp s0, s1, [x9, #16]')
    p.put(0xc630c8, Orig(0xc630c4), 'scvtf s1, s9 -> the store of s0 from main+0xc630c4, one word later')
    return p


# The dynamic-resolution controller (main+0xc62128, called once per frame from the renderer's update at main+0xc60950)
# keeps a window of frame-time samples and steps through resolution tiers; it returns the tier's scale as f32 bits in the
# low word of x0 (bit 33: the tier did not change) and the caller stores scale * render width / output width at renderer
# +0xccc. Every path of the controller reaches its one epilogue through main+0xc622ac (mov w8, w8: the scale, before
# orr x0, x9, x8). The same function as MacNativeGames' 1.4.3 TOTK_FIXED_RES site (main+0x5848a8, mov w9, w23).
DYNRES_SITE = 0xc622ac


def dynres(lay):
    """Dynamic resolution floor: the controller's scale is raised to the data page's floor (+64, f32; 1.0 = always the
    full scene size, so the render size render_res sets is never lowered; 0 = the game's own tiers). The floor and the
    scale are non-negative floats, so their bit patterns order like the values and one unsigned integer compare picks the
    larger: no FP register is touched. x11 is dead at the site (the epilogue reads x8, x9, x10, x19..x21); the flags are
    dead too (orr, ldp, ret, then the caller's ubfx / cbnz)."""
    p = Patch('dynres')
    at = lay.pad + PAD_DYNRES
    p.put(DYNRES_SITE, a.b(DYNRES_SITE, at), 'dynres: mov w8, w8 (the tier scale) -> b <floor veneer>')
    veneer = [a.adrp(11, at, lay.data_off),
              a.ldr_w(11, 11, DP_RES_FLOOR),          # ldr w11, [x11, #64] (the floor, f32 bits)
              a.cmp_w(8, 11),
              a.csel_w(8, 8, 11, 'hi'),               # w8 = max(scale, floor), zero-extended like the mov
              a.b(at + 16, DYNRES_SITE + 4)]
    p.code(at, veneer, 'dynamic resolution floor veneer')
    return p


# The two places the game camera hands its vertical field of view (camera +0x13c, radians) to sead's
# PerspectiveProjection with setFovy inlined (fovy to +0xa0, sin / cos / tan of half of it to +0xa4..+0xac): the
# per-frame camera update (FUN_c7dc18, the same routine as the first aspect site) and GameCamera::postInitialize.
FOV_SITES = (0xc7e4f8, 0x1233e48)
FOV_MAX = 2.75            # radians (158 degrees): the multiplied fovy's ceiling
FMOV_S31_FOV_MAX = 0x1e20d01f   # fmov s31, #2.75


def fov(lay):
    """Field of view: each site branches to a veneer that loads the camera's fovy, records it on the data page (+124, for
    the overlay's degrees) and, while a multiplier sits on the page (+68, 0 = off), multiplies it, at most to FOV_MAX
   . The projection, its frustum (culling) and everything sized
    from it follow; the camera's own value stays, so the game's zoom / aim logic is unchanged. s1 (0.5, the next fmul's
    operand) is live, so the veneer saves x10 and q31 on the stack and touches no flags."""
    p = Patch('fov')
    d = lay.data_off
    for i, site in enumerate(FOV_SITES):
        at = lay.pad + PAD_FOV + 64 * i
        veneer = [Orig(site),                          # the site's load of the camera's fovy into s0
                  0xd10083ff,                          # sub sp, sp, #32
                  0xf90003ea,                          # str x10, [sp]
                  0x3d8007ff,                          # str q31, [sp, #16]
                  a.adrp(10, at + 16, d),
                  a.str_s(0, 10, DP_FOV_BASE),         # str s0, [x10, #124]
                  a.ldr_w(10, 10, DP_FOV_SCALE),       # ldr w10, [x10, #68]
                  a.cbz_w(10, at + 28, at + 48),       # off: keep the game's (to the restore)
                  a.fmov_s_w(31, 10),
                  a.fmul_s(0, 0, 31),                  # fovy *= multiplier
                  FMOV_S31_FOV_MAX,                    # fmov s31, #2.75
                  0x1e3f7800,                          # fminnm s0, s0, s31
                  0x3dc007ff,                          # ldr q31, [sp, #16]
                  0xf94003ea,                          # ldr x10, [sp]
                  0x910083ff,                          # add sp, sp, #32
                  a.b(at + 60, site + 4)]
        p.put(site, a.b(site, at), 'fov: ldr s0, [x19, #0x13c] (camera fovy) -> b <veneer>')
        p.code(at, veneer, f'fov veneer for main+{site:#x}')
    return p


# gsys::ModelSceneShadow::drawDepthShadow sizes the depth shadow map every frame: s0 = a viewport-derived factor
# (capped by s8), width = s0 * int(+0x198), height = s0 * int(+0x19c) of the shadow's parameters, each at least 32 and
# rounded up to a multiple of 8, stored in the ShadowMap (DepthShadow +0x370: +0x68 / +0x88, here +0x3d8 / +0x3f8) and
# reallocated (main+0xa2d2f4) when it changed. The viewport and the texture both come from the stored size.
SHADOW_SITE = 0xa22658


def shadowres(lay):
    """Shadow map resolution: the veneer multiplies the factor s0 by the data page's multiplier (+72, 0 = off) before
    both sizes are computed from it, so width, height, viewport and texture stay consistent. x10 holds the 32 minimum
    (csel below) and is saved with q31 on the stack; no flags are touched. Bigger maps need more GPU memory: 2x is 4x."""
    p = Patch('shadowres')
    at = lay.pad + PAD_SHADOW
    veneer = [0xd10083ff,                              # sub sp, sp, #32
              0xf90003ea,                              # str x10, [sp]
              0x3d8007ff,                              # str q31, [sp, #16]
              a.adrp(10, at + 12, lay.data_off),
              a.ldr_w(10, 10, DP_SHADOW_SCALE),        # ldr w10, [x10, #72]
              a.cbz_w(10, at + 20, at + 32),           # off: keep the game's factor (to the restore)
              a.fmov_s_w(31, 10),
              a.fmul_s(0, 0, 31),                      # factor *= multiplier
              0x3dc007ff,                              # ldr q31, [sp, #16]
              0xf94003ea,                              # ldr x10, [sp]
              0x910083ff,                              # add sp, sp, #32
              Orig(SHADOW_SITE),                       # the site's fmul (width = factor * base width)
              a.b(at + 48, SHADOW_SITE + 4)]
    p.put(SHADOW_SITE, a.b(SHADOW_SITE, at), 'shadowres: fmul s1, s0, s2 (shadow width) -> b <veneer>')
    p.code(at, veneer, 'shadow map size veneer')
    return p


# gsys picks a model's LOD level from its screen size: metric = radius * projection scale / view depth (the camera's
# +0x198 and the view row at +0xc0, the bounding sphere's radius at +0xc), stored at +0x10 of the per-model LOD state
# and compared with the model's LOD thresholds (main+0x21c8e04: the smaller the metric, the coarser the level). The
# metric is computed by one helper (FUN_21c8da4) and inlined twice (FUN_21caa3c, FUN_21cbfe4); each ends in this fdiv.
# (main+0x21c8e04 has a second, "simple" mode, gsys::ModelViewInfo::enableLodCalcSimple, that compares the depth
# instead; it is not scaled here.)
LOD_SITES = ((0x21c8de8, 0, 1), (0x21cae1c, 1, 1), (0x21cc3c8, 1, 1))   # (site, metric register, numerator register)


def lod(lay):
    """LOD distance: the metric is multiplied by the data page's multiplier (+76, 0 = off) after each fdiv, so every
    model keeps each LOD level k times further away (k = 2: detail at twice the distance). Objects behind the camera
    (FLT_MAX) are not touched. The veneers save x10 and q31 on the stack and touch no flags."""
    p = Patch('lod')
    for i, (site, m, num) in enumerate(LOD_SITES):
        at = lay.pad + PAD_LOD + 64 * i
        veneer = [Orig(site),                              # the site's fdiv s<m>, s<num>, s0
                  0xd10083ff,                              # sub sp, sp, #32
                  0xf90003ea,                              # str x10, [sp]
                  0x3d8007ff,                              # str q31, [sp, #16]
                  a.adrp(10, at + 16, lay.data_off),
                  a.ldr_w(10, 10, DP_LOD_SCALE),           # ldr w10, [x10, #76]
                  a.cbz_w(10, at + 24, at + 36),           # off: the game's metric (to the restore)
                  a.fmov_s_w(31, 10),
                  a.fmul_s(m, m, 31),                      # metric *= multiplier
                  0x3dc007ff,                              # ldr q31, [sp, #16]
                  0xf94003ea,                              # ldr x10, [sp]
                  0x910083ff,                              # add sp, sp, #32
                  a.b(at + 48, site + 4)]
        p.put(site, a.b(site, at), f'lod: fdiv s{m}, s{num}, s0 (screen-size metric) -> b <veneer>')
        p.code(at, veneer, f'lod veneer for main+{site:#x}')
    return p


# ExecutePlayerCameraPreUpdate::updateImpl_ loads the PlayerCamera (player component chain +0x8 / +0x8 / +0x1f8 /
# +0x220 / +0x3b0) into x19 and calls FUN_d057e4 with it (main+0xd050b8: mov x0, x19; bl).
CAMERA_SITE = 0xd050b8
# It then tail-calls the camera's stick step FUN_d052ac (x0 = camera + 0x428, x1 = the input block, its first two
# floats the right stick, w2 = (camera +0x34 >> 4) & 7: bit 0 no stick, bits 1 / 2 the game's x / y inversion). The
# step applies the dead zone (0.005), the unit clamp and the response curve (powf by the camera parameter +0x180), then
# stores x (s11) to camera +0x840 and y (s10) to +0x844, inverts them per w2 and derives the idle flags (+0x838) from
# them; the camera turns from +0x840 / +0x844 later. With the stick off (w2 bit 0) or a scripted source (camera +0x568
# bit 1, [x20, #0x140]) the step uses zero / the script's values.
MOUSE_SITE = 0xd05364


def mouse(lay):
    """Mouse camera.
    1. The PlayerCamera pointer goes to the data page (+136) once per camera pre-update (TOTK_DEBUG_CAMERA logs its
       fields). x8 is dead at the site (loaded over before its next read, and the callee clobbers it).
    2. Direct mouse input: at the stick step's first store a veneer adds the mouse motion since the last step, from
       platform's PC page (+0x100: running totals of counts), to the processed stick (x += dx * k, y -= dy * k, mouse
       down = stick down) with k = rate (+84) * multiplier (+80) / 30 Hz ratio (+0): the game turns by stick * speed *
       ratio, so the turn per count is the same at any frame rate and the mouse skips the dead zone and the curve. The
       game's own inversion and the idle flags follow, as for the stick. The first step after the mouse is captured
       (or after more than MOUSE_STALE presents without a step: pause, loading, a cutscene) only takes the totals; the
       veneer keeps PC page +0x64 (guest_mouse_claimed) at 1 while it applies the mouse, so platform stops emulating
       the stick, and clears it when the mouse is released or the multiplier is 0 (then platform's stick emulation
       drives the camera again). The step does not run in the map, menus or other camera modes, so the claim is a
       heartbeat: the runtime (totk_frame_step_update) releases it once the beat (+92) is more than MOUSE_STALE
       presents old, and the next step primes again. x8 .. x10, s0 .. s3 and the flags are dead at the site (written
       before their next reads).
    3. Pitch: the camera turns by stick x linearly (70 x deg/s) but by stick y quadratically (51 y |y| deg/s), so with
       a pitch gain g > 0 at +128 (runtime: 70 / 51) the veneer applies y -= sign(v) sqrt(g |v|), v = dy * k: pitch
       per count is then yaw's at any speed. g <= 0 (or NaN) keeps y -= v, for a runtime that shapes the PC page's y
       total itself. Each applied step also stores 1 at +132 whatever g is: a runtime keeps shaping y itself (and g at
       0) until it sees that, so an older key never gets a raw y total. The tail sits apart (PAD_MOUSE_PITCH): no other
       gap in the padding holds the whole veneer."""
    p = Patch('mouse')
    at = lay.pad + PAD_MOUSE
    veneer = [Orig(CAMERA_SITE),                       # the site's mov x0, x19
              a.adrp(8, at + 4, lay.data_off),
              a.str_x(19, 8, DP_CAMERA),               # str x19, [x8, #136]
              a.b(at + 12, CAMERA_SITE + 4)]
    p.put(CAMERA_SITE, a.b(CAMERA_SITE, at), 'mouse: mov x0, x19 (PlayerCamera) -> b <veneer>')
    at = p.code(at, veneer, 'camera pointer veneer')

    v = at
    claimed, captured = DP_PC + PC_CLAIMED, DP_PC + PC_CAPTURED
    RELEASE, PRIME, DONE = 38, 40, 42                  # word indexes of the labels
    t = lay.pad + PAD_MOUSE_PITCH                      # the pitch tail
    APPLY = 7

    def to(i, label):                                  # target address of the branch at word i
        return v + 4 * i, v + 4 * label
    words = [a.adrp(9, v, lay.data_off),
             a.ldr_w(10, 9, captured),                 # mouse captured?
             a.ldr_w(8, 9, DP_MOUSE_SCALE),            # multiplier bits, 0 = off
             a.cbz_w(10, *to(3, RELEASE)),
             a.cbz_w(8, *to(4, RELEASE)),
             a.ldr_w(10, 9, DP_PRESENTS),
             a.ldr_w(8, 9, DP_MOUSE_BEAT),
             a.str_w(10, 9, DP_MOUSE_BEAT),
             a.sub_w(8, 10, 8),                        # w8 = presents since the last step
             a.ldr_d(0, 9, DP_PC + PC_MOUSE_X),
             a.ldr_d(1, 9, DP_PC + PC_MOUSE_Y),
             a.ldr_d(2, 9, DP_MOUSE_LAST),
             a.ldr_d(3, 9, DP_MOUSE_LAST + 8),
             a.str_d(0, 9, DP_MOUSE_LAST),             # the totals are taken in every case
             a.str_d(1, 9, DP_MOUSE_LAST + 8),
             a.ldr_w(10, 9, claimed),
             a.cbz_w(10, *to(16, PRIME)),              # not applying yet: prime
             a.cmp_w_imm(8, MOUSE_STALE),
             a.b_cond('hi', *to(18, DONE)),            # steps were skipped: drop that motion
             a.tbnz(21, 0, *to(19, DONE)),             # the game has the stick off
             a.ldrb_w(8, 20, 0x140),
             a.tbnz(8, 1, *to(21, DONE)),              # scripted camera input
             a.fsub_d(0, 0, 2),                        # dx
             a.fsub_d(1, 1, 3),                        # dy
             a.fcvt_s_d(0, 0),
             a.fcvt_s_d(1, 1),
             a.ldr_s(2, 9, DP_MOUSE_SCALE),
             a.ldr_s(3, 9, DP_FRAME_RATIO),
             a.fdiv_s(2, 2, 3),
             a.ldr_s(3, 9, DP_MOUSE_RATE),
             a.fmul_s(2, 2, 3),                        # k = multiplier / ratio * rate
             a.fmul_s(0, 0, 2),
             a.fmul_s(1, 1, 2),                        # v = dy * k
             a.fadd_s(11, 11, 0),                      # x += dx * k
             a.str_w(10, 9, DP_MOUSE_PITCH_ACK),       # w10 = the claim, 1: this veneer shapes pitch (version 1)
             a.ldr_s(3, 9, DP_MOUSE_PITCH),            # pitch gain
             a.fcmp_s_zero(3),
             a.b(v + 4 * 37, t),                       # the pitch tail: y, then DONE
             a.str_w(31, 9, claimed),                  # RELEASE: platform emulates the stick again
             a.b(*to(39, DONE)),
             a.movz_w(10, 1),                          # PRIME: claim the mouse from the next step on
             a.str_w(10, 9, claimed),
             Orig(MOUSE_SITE),                         # DONE: the site's store of x (camera +0x840)
             a.b(v + 4 * 43, MOUSE_SITE + 4)]
    assert len(words) == 44
    tail = [a.b_cond('le', t, t + 4 * APPLY),          # gain <= 0 or NaN: linear
            a.fabs_s(2, 1),
            a.fmul_s(2, 2, 3),
            a.fsqrt_s(2, 2),                           # sqrt(g |v|)
            a.fneg_s(3, 2),
            a.fcmp_s_zero(1),
            a.fcsel_s(1, 2, 3, 'ge'),                  # with v's sign
            a.fsub_s(10, 10, 1),                       # APPLY: y -= v (mouse down = stick down)
            a.b(t + 4 * 8, v + 4 * DONE)]
    p.put(MOUSE_SITE, a.b(MOUSE_SITE, v), 'mouse: str s11, [x20, #0x418] (stick x) -> b <input veneer>')
    at = p.code(v, words, 'mouse input veneer')
    if at > lay.pad + PAD_ACTOR_LOD:
        raise ValueError('mouse veneers overrun their padding')
    if p.code(t, tail, 'mouse pitch tail') > lay.pad_end:
        raise ValueError('mouse pitch tail overruns the text padding')
    return p


# engine::lod::LODModule::calc sorts each actor into an LOD area per camera: s14 = squared distance from the camera
# (camera +0x50..+0x58) to the actor (main+0x742714), compared with (radius + AreaNearDistance)^2 (area 5, near: full
# calc and draw), (radius + AreaNearDistance * AreaMiddle0DistanceScale)^2 (area 4) and two global thresholds (areas 3,
# 2; else 1); the lower areas skip calc / draw / cloth frames (the actor's LODParam: EnableCalcSkip, EnableDrawSkip,
# ...). Before that the distance is also kept as the nearest camera's (sp+0xe0, main+0x742724 / 0x742740), which feeds
# the actor's far / near check at main+0x742f60 (FUN_70062c: sleep / delete requests), so it must stay unscaled. Both
# paths (and the NaN-camera one, s14 = 0) join at main+0x742744, the only way into the area comparisons (s14 is read
# at 0x74283c .. 0x742a40, callee-saved across the calls in between).
ACTOR_LOD_SITE = 0x742744


def actor_lod(lay):
    """Actor detail distance: the squared camera distance is divided by k^2 (k = data page +112, 0 = off) after the
    nearest-camera distance was stored, so every LOD area reaches k times further: distant NPCs, enemies and objects
    keep their full update and draw rate longer (CPU cost). The far / near check (which actors sleep or are deleted)
    and placement are not changed. The veneer saves x10 and q31 and touches no flags."""
    p = Patch('actor_lod')
    at = lay.pad + PAD_ACTOR_LOD
    veneer = [Orig(ACTOR_LOD_SITE),                    # the site's load (sp-relative: before the sub)
              0xd10083ff,                              # sub sp, sp, #32
              0xf90003ea,                              # str x10, [sp]
              0x3d8007ff,                              # str q31, [sp, #16]
              a.adrp(10, at + 16, lay.data_off),
              a.ldr_w(10, 10, DP_ACTOR_LOD),           # ldr w10, [x10, #112]
              a.cbz_w(10, at + 24, at + 40),           # off: the game's distance (to the restore)
              a.fmov_s_w(31, 10),
              a.fmul_s(31, 31, 31),                    # k^2
              a.fdiv_s(14, 14, 31),                    # distance^2 / k^2
              0x3dc007ff,                              # ldr q31, [sp, #16]
              0xf94003ea,                              # ldr x10, [sp]
              0x910083ff,                              # add sp, sp, #32
              a.b(at + 52, ACTOR_LOD_SITE + 4)]
    p.put(ACTOR_LOD_SITE, a.b(ACTOR_LOD_SITE, at),
          'actor_lod: ldr s0, [sp, #0xd0] (after the nearest-distance store) -> b <veneer>')
    p.code(at, veneer, 'actor LOD distance veneer')
    return p


# tera's terrain quadtree (ground and water render lists): each frame the list's update FUN_ce54a0 walks the areas
# (recursing through FUN_ce98a8) with a context at sp+0xd0 (FUN_ce98a8: x0): +0x00 the node pool (u32 capacity, the
# 0x80-byte nodes at +8), +0x18 the next node, +0x24 the nearest limit, +0x28 the LoD table (the list's
# tera::RenderInfo + 8, from getRenderInfo at main+0xce55dc), +0x30 the range. The table holds one {morph start, end}
# pair per quadtree level; a node of level L is split while its camera distance is <= end[L] (the THRESHOLD sites), its
# morph flag compares end[L - 1] (MORPH_FLAG sites). The same update then copies the pairs into the list's uniform
# block (+0x218: start, end, 1 / (end - start), ..., MORPH sites) for the shader's geomorph. The game's own
# ExecuteTerrainAdjustLoD (an AI node) only writes RenderInfo +0 / +4 (BaseDistance, NearestLimitDistance). A full node
# pool gives sead's null element (a crash), so the split threshold is only scaled while the pool is less than half full.
TERRAIN_LOD_THRESHOLD = ((0xce61d0, 1, 'sp'),   # (site: ldr s<r>, [x8, #4], r, context base)
                         (0xce8f94, 4, 'sp'),
                         (0xce9b90, 3, 'x0'))
TERRAIN_LOD_MORPH_FLAG = ((0xce62c4, 2), (0xce9c88, 5))
TERRAIN_LOD_MORPH_FIRST = 0xce7ef8                # ldr s4, [x0, #0xc]! (end[0])
TERRAIN_LOD_MORPH_PAIR = 0xce7f48                 # ldp s4, s5, [x11, #-4] (start[i], end[i]; flags live)
TERRAIN_CTX_SP = 0xd0                                        # FUN_ce54a0's context, sp-relative


def terrain_lod(lay):
    """Terrain LoD distance: every read of the quadtree's per-level {morph start, end} distances is multiplied by the data
    page's multiplier (+116, 0 = off), so each terrain level (ground and water) reaches k times further and the shader's
    geomorph ranges move with it. More nodes are drawn (CPU and GPU cost); while the node pool is half full or more
    the split thresholds stay the game's, so it never runs out. The veneers save x10 (x11) and q31 on the stack; only
    the guarded ones touch the flags, at sites where an fcmp follows."""
    p = Patch('terrain_lod')
    at = lay.pad + PAD_TERRAIN_LOD
    d = lay.data_off
    save, restore = [0xd10083ff, 0xf90003ea, 0x3d8007ff], [0x3dc007ff, 0xf94003ea, 0x910083ff]   # x10 + q31

    def plain(at, site, scale, what):
        """the site's instruction; s<r> *= k for each r in scale."""
        n = len(scale)
        words = [Orig(site), *save,
                 a.adrp(10, at + 16, d),
                 a.ldr_w(10, 10, DP_TERRAIN_LOD),
                 a.cbz_w(10, at + 24, at + 4 * (8 + n)),       # off: to the restore
                 a.fmov_s_w(31, 10),
                 *[a.fmul_s(r, r, 31) for r in scale],
                 *restore]
        words.append(a.b(at + 4 * len(words), site + 4))
        p.put(site, a.b(site, at), f'terrain_lod: {what} -> b <veneer>')
        return p.code(at, words, f'terrain LoD veneer for main+{site:#x}')

    for site, r, base in TERRAIN_LOD_THRESHOLD:
        sp_off = 32 if base == 'sp' else 0                    # the context, after the veneer's sub sp
        cb, co = (31, TERRAIN_CTX_SP + sp_off) if base == 'sp' else (0, 0)
        words = [Orig(site),
                 0xd10083ff,                                  # sub sp, sp, #32
                 0xa9002fea,                                  # stp x10, x11, [sp]
                 0x3d8007ff,                                  # str q31, [sp, #16]
                 a.adrp(10, at + 16, d),
                 a.ldr_w(10, 10, DP_TERRAIN_LOD),
                 a.cbz_w(10, at + 24, at + 56),               # off: to the restore
                 a.ldr_x(11, cb, co),                         # the node pool
                 a.ldr_w(11, 11, 0),                          # its capacity
                 a.fmov_s_w(31, 10),
                 a.ldr_w(10, cb, co + 0x18),                  # the next node
                 a.cmp_w(11, 10, lsl=1),
                 a.b_cond('ls', at + 48, at + 56),            # capacity <= 2 * next: the game's threshold
                 a.fmul_s(r, r, 31),                          # end[L] *= k
                 0x3dc007ff,                                  # ldr q31, [sp, #16]
                 0xa9402fea,                                  # ldp x10, x11, [sp]
                 0x910083ff,                                  # add sp, sp, #32
                 a.b(at + 68, site + 4)]
        p.put(site, a.b(site, at), f'terrain_lod: ldr s{r}, [x8, #4] (split distance) -> b <veneer>')
        at = p.code(at, words, f'terrain LoD split veneer for main+{site:#x}')
    for site, r in TERRAIN_LOD_MORPH_FLAG:
        at = plain(at, site, (r,), f'ldr s{r}, [x8, #4] (parent level\'s distance, morph flag)')
    at = plain(at, TERRAIN_LOD_MORPH_FIRST, (4,), 'ldr s4, [x0, #0xc]! (uniform block: level 0 distance)')
    at = plain(at, TERRAIN_LOD_MORPH_PAIR, (4, 5), 'ldp s4, s5, [x11, #-4] (uniform block: morph range)')
    if at > lay.pad + PAD_MOUSE_PITCH:
        raise ValueError('terrain LoD veneers overrun the text padding')
    return p


# After the LOD areas, LODModule::calc asks FUN_70062c whether the actor is beyond its ForbidPushJobDistance (x its
# scale, by ApplyScaleTypeToForbidPushJob) from the nearest camera: then its jobs are no longer pushed (no calc, no
# update: asleep). The squared nearest distance goes in at sp+0xb98 (main+0x742f7c), NaN (no camera) made canonical by a
# fcmp / fcsel first. That canonicalisation is not needed (FUN_70062c only tests NAN()), so its four words make room for
# the division in place: no veneer (the text padding is full).
SIM_SITE = 0x742f64


def sim_radius(lay):
    """Actor simulation distance: the squared nearest-camera distance FUN_70062c compares with ForbidPushJobDistance is
    divided by k^2 (k = data page +120), so actors and their physics keep being updated k times further (CPU cost).
    The slot must not be 0 (k^2 = 0 puts every actor to sleep): the runtime writes 1 when the setting is off. x8 is free
    at the site (the original's NaN constant, clobbered by the call); the flags were only read by the removed fcsel."""
    p = Patch('sim_radius')
    words = ((0, a.adrp(8, SIM_SITE, lay.data_off), 'mov w8, #0x7fc00000 (NaN) -> adrp x8, <data page>'),
             (2, a.ldr_s(0, 8, DP_SIM_RADIUS), 'fmov s0, w8 -> ldr s0, [x8, #120] (k)'),   # (1, 3 stay: add x1, mov x0)
             (4, a.fmul_s(0, 0, 0), 'fcmp s1, s1 -> fmul s0, s0, s0'),
             (5, a.fdiv_s(0, 1, 0), 'fcsel s0, s1, s0, vc -> fdiv s0, s1, s0 (squared distance / k^2)'))
    for i, new, why in words:
        p.put(SIM_SITE + 4 * i, new, f'sim_radius: {why}')
    return p


def output_res(lay):
    """Output resolution ("true 2K"). Two sizes are 1920x1080 in the game:
    1. The swapchain: nnMain writes the framework's display size as a constant (main+0xe996ac/b0, mov/movk x8) before
       creating the graphics framework. Here it is loaded from the data page (+48, {u32 w, u32 h}), so the runtime picks
       the largest output size at start without a rebuild.
    2. The output (canvas) size main+0xc63064 gives to the window crop and the renderer's display slot: read from the
       data page (+32) instead of the graphics parameters, and the renderer pointer is stored next to it (+40) so the
       runtime can request a new layout (totk_output_res_set: bit 0x200 of renderer+0xca4 + oe message 30).
    Bigger than 1920x1080 needs the HeapDef overlay (overlays: heapdef) and the application pool grown by
    totk_output_extra_bytes()."""
    p = Patch('output_res')
    d = lay.data_off
    p.put(0xe996ac, a.adrp(8, 0xe996ac, d), 'mov x8, #0x780 (1920) -> adrp x8, <data page>')
    p.put(0xe996b0, a.ldr_x(8, 8, DP_SWAPCHAIN_SIZE), 'movk x8, #0x438, lsl #32 (1080) -> ldr x8, [x8, #48] (swapchain size)')
    p.put(0xc630a4, a.adrp(21, 0xc630a4, d), 'mov x0, x20 -> adrp x21, <data page>')
    p.put(0xc630a8, a.str_x(19, 21, DP_RENDERER), 'bl main+0xc632b0 (parameters\' output size) -> str x19, [x21, #40] (renderer)')
    p.put(0xc630ac, a.ldr_x(21, 21, DP_OUTPUT_SIZE), 'ldr x21, [x0] -> ldr x21, [x21, #32] (output size)')
    return p


# The four places 1.0.0 hands an aspect ratio to sead's PerspectiveProjection (+0xb0), found for this port by pattern:
# two camera sites inline setFovy (sinf / cosf / tanf of half the fovy to +0xa4 / +0xa8 / +0xac, the dirty flag at +8)
# and load the camera's ratio (+0x140); two copies of a viewport routine divide its width by its height. The same four
# kinds of site as MacNativeGames' 1.2.1 (0xcb4b10, 0x12521cc camera; 0x19b36f0, 0x1ecdf68 viewport) and 1.4.3 sets.
ASPECT_SITES = ((0xc7e530, 0, 'camera: ldr s0, [x19, #0x140]'),   # (site, the ratio's register s<d>, what)
                (0x1233e80, 0, 'camera: ldr s0, [x19, #0x140]'),
                (0x1968c2c, 0, 'viewport: fdiv s0, s0, s1'),
                (0x1e5eae8, 0, 'viewport: fdiv s0, s0, s1'))


def aspect(lay):
    """Other picture shapes (ultrawide, 16:10): each site branches to a stub in the text padding that runs the original
    instruction and, while a ratio sits on the data page (+56, 0 = off), puts that in its place. The stub keeps its
    scratch register on the stack, so no liveness is assumed. The game keeps rendering its scene at the scene size; the
    canvas (output_res) has the screen's shape, so the final upscale stretches the wider view back to the right
    proportions. The UI needs the matching layout overlay (overlays: aspect-<w>x<h>)."""
    p = Patch('aspect')
    d = lay.data_off
    for i, (site, dst, what) in enumerate(ASPECT_SITES):
        s = lay.pad + PAD_ASPECT + 32 * i
        stub = [Orig(site),                            # the site's own instruction: the game's ratio into s<d>
                0xf81f0fea,                            # str x10, [sp, #-16]!
                a.adrp(10, s + 8, d),
                a.ldr_w(10, 10, DP_ASPECT),            # ldr w10, [x10, #56]
                a.cbz_w(10, s + 16, s + 24),           # off: keep the game's
                a.fmov_s_w(dst, 10),                   # fmov s<d>, w10
                0xf84107ea,                            # ldr x10, [sp], #16
                a.b(s + 28, site + 4)]
        p.put(site, a.b(site, s), f'aspect {what} -> b <stub>')
        p.code(s, stub, f'aspect stub for main+{site:#x}')
    return p


# The player's guard (shield up) reads virtual key 2 (ZL) held, like lock-on does: each site is the `mov w1, #2`
# before Player::getVirtualKeyHoldValue (main+0x1ccadcc). The VK 2 hold reads of the two guard states, decided one by
# one:
# - ExecutePlayerDrawn::enterImpl_ 0x1cfe324, 0x1cfe6c8, 0x1cfe6e0: guard -> 27
# - ExecutePlayerDrawn::getResult 0x1cfdf04: right after the shield slot tests (0xd3a544 / 0x8f4c60 with slot 4), the
#   "still guarding?" test of the shield mode; left on 2 a PC guard key alone would end the state at once  -> 27
# - ExecutePlayerLockOnWait::updateImpl_ 0x1d080e4: after the shield slot test (0xd3a544, 4): guard     -> 27
# - ExecutePlayerLockOnWait::updateImpl_ 0x1d0846c: no shield test, then 0x1ccc240 / 0x16e5d40: the lock-on
#   -> stays 2
# - SelectorPlayerSchedulerShieldSurf 0x211e538: Q + Space + E on
#   PC; the lock-on key alone must not start it)                                                      -> 27
# - inline 0x8f5a24 in FUN_8f5890 (the equipment draw decision after Equipment::isSlotValidActor): "shield wanted" =
#   hold VK 2 || mSubActionStatus bit, then the shield slot's link is resolved and the shield drawn;
#   `ldrb w8, [x8]; tbnz w8, #2` becomes `ldrb w8, [x8, #3]; tbnz w8, #3` (bit 27)                      -> 27
# - FUN_8f5890's trig read 0x8f5930 is lock-on
#   with a two-handed weapon and no shield): strafe and lock-on movement                                -> stay 2
#   The PC guard key sets 2 + 27 and the pad mirror does too, so guard sees every one of them as before; only the
#   lock-on key alone (2) differs from pad ZL, by the reads above that moved to 27.
VK_GUARD = 27
GUARD_SITES = (0x1cfe320, 0x1cfe6c4, 0x1cfe6dc, 0x1cfdf00, 0x1d080e0, 0x211e534)
GUARD_INLINE, GUARD_INLINE_TO = 0x8f5a20, 0x8f5c60   # FUN_8f5890: ldrb w8, [x8]; tbnz w8, #2, <shield>


def vk_guard():
    """PC controls G1: guard reads its own virtual key, VK_GUARD (27, one the game leaves unused),
    so a PC key can lock on (VK 2) without raising the shield. The engine's hook on the virtual-key writer
    (engine/runtime/totk_pc_vk.cpp, hook_f_1ccf894) copies the pad's VK 2 to 27, so ZL still guards on a controller:
    only for keys whose GEN_MAIN has --hook 1ccf894 (without it nothing sets 27 and the shield never rises)."""
    p = Patch('vk_guard')
    for site in GUARD_SITES:
        p.put(site, a.movz_w(1, VK_GUARD), f'vk_guard: mov w1, #2 -> #{VK_GUARD} (guard: VK hold)')
    # the inline read in the shield draw decision: VK 27 is bit 3 of the hold mask's byte 3
    p.put(GUARD_INLINE, a.ldrb_w(8, 8, VK_GUARD // 8), 'vk_guard: ldrb w8, [x8] -> [x8, #3] (hold byte 3)')
    p.put(GUARD_INLINE + 4, a.tbnz(8, VK_GUARD % 8, GUARD_INLINE + 4, GUARD_INLINE_TO), 'vk_guard: tbnz w8, #2 -> #3 (shield wanted: VK 27)')
    return p


# The UI's per-frame constant buffers: f_129bc74 fills the UI module's config, and its +24 / +32 become eui's
# ConstantBuffer halves (ConstantBuffer::initialize 0x119d000 allocates 2 x align(ui2d + font) from the heap it is
# given). Shipped: ui2d 0x1a0000, font 0x40000 per half. The TOTK_UI_CBDIAG peaks: the Options screen with 69 entries
# needs 2206464 bytes of ui2d, 48 entries with the longest texts 1436672 (84%); font stays under 58%. ui2d does not
# depend on the text (same exe, 48 entries; only font grows, +11%), so 69 entries need ~2206464 whatever the language:
# 0x300000 (3 MiB) keeps that at 0.70 of the half (the 0.8 rule on a lower-bound peak) with room for the Window /
# Picture panes' inline bumps, which the runtime's check (totk_ui_cb.cpp) cannot see (0x2c0000 would be 0.77). +2.75
# MiB from that heap in all: default off until a boot shows it fits.
UI_CB_UI2D = 0x129c5a0      # f_129bc74: mov w8, #0x1a0000 (stored at config +24)
UI_CB_UI2D_HALF = 0x300000


def ui_cb():
    p = Patch('ui_cb')
    p.put(UI_CB_UI2D, a.movz_w(8, UI_CB_UI2D_HALF >> 16, 16),
          f'ui_cb: the ui2d constant buffer half 0x1a0000 -> {UI_CB_UI2D_HALF:#x}')
    return p


FEATURES = {
    # name: (builder, default on, needs)
    'fps60': (lambda lay: fps60(), True, ()),
    'realtime': (realtime, True, ()),
    'cloth': (cloth, True, ('realtime',)),
    'highfps': (highfps, True, ()),
    'output_res': (output_res, True, ()),
    'render_res': (render_res, True, ()),
    'aspect': (aspect, True, ()),
    'dynres': (dynres, True, ()),
    'fov': (fov, True, ()),
    'shadowres': (shadowres, True, ()),
    'lod': (lod, True, ()),
    'mouse': (mouse, True, ()),
    'actor_lod': (actor_lod, True, ()),
    'terrain_lod': (terrain_lod, True, ()),
    'sim_radius': (sim_radius, True, ()),
    # engine only: needs hook_f_1ccf894, which mirrors the pad's VK 2 to 27 only when it finds this feature's word at
    # GUARD_SITES[0]. `engine --features default` adds it (engine.ENGINE_DEFAULT), `apply`'s default (no hook there)
    # does not.
    'vk_guard': (lambda lay: vk_guard(), False, ()),
    'ui_cb': (lambda lay: ui_cb(), False, ()),  # off: as is it CRASHES at start; the heap growth is on gp-uicb
}


def build(lay, features):
    unknown = [f for f in features if f not in FEATURES]
    if unknown:
        raise ValueError(f'unknown features {unknown}; known: {", ".join(FEATURES)}')
    for f in features:
        for need in FEATURES[f][2]:
            if need not in features:
                raise ValueError(f'feature {f} needs {need}')
    return [FEATURES[f][0](lay) for f in FEATURES if f in features]


def with_image(patches, image):
    """`patches` with each word's `original` read from `image` (the flat main of the build they are for): what a tool that
    reasons about the replaced instructions (the porter) needs. The copies (Orig) stay references."""
    for p in patches:
        for w in p.words:
            w.original = int.from_bytes(image[w.offset:w.offset + 4], 'little')
    return patches
