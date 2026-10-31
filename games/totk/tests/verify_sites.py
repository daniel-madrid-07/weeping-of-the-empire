"""Execute the remaining patched 1.0.0 sites with Unicorn: the trampoline check (highfps), the swapchain and output
size (output_res), the scene size (render_res), the four aspect stubs (aspect), the dynamic-resolution floor (dynres),
the field of view (fov), the shadow map size (shadowres), the LOD metrics (lod), the mouse camera (mouse), the actor
LOD distance (actor_lod) and the terrain quadtree distances (terrain_lod). Each runs the patched image from the site to the next unpatched instruction with the data
page filled in, checks what the original code would have handed on (registers, stores) and that the scratch registers /
sp / flags come back. Where the original sequence is self-contained it runs too, as the negative control."""
import math
import re
import struct
from pathlib import Path

from unicorn.arm64_const import (UC_ARM64_REG_NZCV, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_W8, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X8, UC_ARM64_REG_X9, UC_ARM64_REG_X10, UC_ARM64_REG_X11, UC_ARM64_REG_X19,
                                 UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X23, UC_ARM64_REG_X25, UC_ARM64_REG_X1, UC_ARM64_REG_Q4, UC_ARM64_REG_Q31)

from games.totk.patches.sites_v100 import (ACTOR_LOD_SITE, ASPECT_SITES, BOUNCE_SITE, CAMERA_SITE, DP_ACTOR_LOD,
                                           DP_CAMERA, DP_FOV_BASE, DP_FOV_SCALE, DP_FRAME_RATIO, DP_LOD_SCALE, DP_MOUSE_BEAT,
                                           DP_MOUSE_LAST, DP_MOUSE_PITCH, DP_MOUSE_PITCH_ACK, DP_MOUSE_SCALE, DP_PC, DP_PRESENTS, DP_RES_FLOOR,
                                           DP_SHADOW_SCALE, DP_SIM_RADIUS, DP_TERRAIN_LOD, DYNRES_SITE, FOV_SITES, LOD_SITES,
                                           MOUSE_SITE, MOUSE_STALE, PC_CAPTURED, PC_CLAIMED, PC_MOUSE_X, SHADOW_SITE, SIM_SITE,
                                           FOV_MAX, TERRAIN_CTX_SP, TERRAIN_LOD_MORPH_FIRST, TERRAIN_LOD_MORPH_FLAG,
                                           TERRAIN_LOD_MORPH_PAIR, TERRAIN_LOD_THRESHOLD)

from .common import DATA, STACK, bits, get_s, machine, reg_s

OBJ = 0x7000a000   # in the stack mapping, above sp
DATA_PAGE_H = Path(__file__).resolve().parents[1] / 'include' / 'totk' / 'data_page.h'


def _page(**slots):
    page = bytearray(0x200)
    for off, fmt, *vals in slots.values():
        struct.pack_into(fmt, page, off, *vals)
    return bytes(page)


def highfps(original, patched):
    """X+0x28 = 30 / (ratio * time scale), X+0x38 = ratio: the patched check compares 30 / time scale with 100."""
    out = []
    for ratio, scale in ((1.0, 1.0), (0.25, 1.0), (0.2, 1.0), (1.0, 0.2), (0.25, 0.2), (0.5, 0.5)):
        x28 = 30 / (ratio * scale)
        for name, image in (('patched', patched), ('original', original)):
            mu = machine(image)
            mu.mem_write(OBJ + 0x28, struct.pack('<f', x28))
            mu.mem_write(OBJ + 0x38, struct.pack('<f', ratio))
            mu.reg_write(UC_ARM64_REG_X8, OBJ)
            mu.emu_start(BOUNCE_SITE, BOUNCE_SITE + 12, count=12)
            assert mu.reg_read(UC_ARM64_REG_PC) == BOUNCE_SITE + 12, f'{name}: did not reach the fcmp'
            assert get_s(mu, 1) == 100.0 and mu.reg_read(UC_ARM64_REG_W8) == 0x42c80000, f'{name}: s1 / w8 not 100.0'
            assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{name}: sp changed'
            want = 30 / scale if name == 'patched' else x28
            assert abs(get_s(mu, 0) - want) < 1e-3 * want, (name, ratio, scale, get_s(mu, 0), want)
        out.append(f'ratio {ratio} scale {scale}: patched {30 / scale:g} vs 100 (original {x28:g})')
    return out


def output_res(original, patched):
    out = []
    for w, h, mw, mh in ((1920, 1080, 1920, 1080), (1920, 1080, 2560, 1440), (2560, 1440, 2560, 1440),
                         (3840, 2160, 3840, 2160), (3440, 1440, 3440, 1440)):
        page = _page(out=(32, '<II', w, h), swap=(48, '<II', mw, mh))
        mu = machine(patched, page)   # the swapchain size, from nnMain
        mu.emu_start(0xe996ac, 0xe996b4, count=4)
        assert mu.reg_read(UC_ARM64_REG_X8) == mh << 32 | mw, f'swapchain {mw}x{mh}: x8 {mu.reg_read(UC_ARM64_REG_X8):#x}'
        mu = machine(patched, page)   # the output size, main+0xc63064
        mu.reg_write(UC_ARM64_REG_X19, OBJ)
        mu.emu_start(0xc630a4, 0xc630b0, count=6)
        assert mu.reg_read(UC_ARM64_REG_X21) == h << 32 | w, f'output {w}x{h}: x21 {mu.reg_read(UC_ARM64_REG_X21):#x}'
        assert struct.unpack('<Q', mu.mem_read(DATA + 40, 8))[0] == OBJ, 'renderer not stored at +40'
        out.append(f'swapchain {mw}x{mh}, output {w}x{h}')
    mu = machine(original)   # negative control: the shipping constant
    mu.emu_start(0xe996ac, 0xe996b4, count=4)
    assert mu.reg_read(UC_ARM64_REG_X8) == 1080 << 32 | 1920, 'original swapchain size is not 1920x1080'
    return out


def render_res(original, patched):
    out = []
    for w, h in ((1600, 900), (1920, 1080), (2560, 1440), (3840, 2160)):
        mu = machine(patched, _page(render=(16, '<ff', float(w), float(h))))
        mu.reg_write(UC_ARM64_REG_X19, OBJ)
        mu.reg_write(UC_ARM64_REG_X0, OBJ + 0x2000)   # the ldr w20, [x0] between the sites
        mu.reg_write(reg_s(8), 1280)                   # the parameter's integers, which must no longer matter
        mu.reg_write(reg_s(9), 720)
        mu.emu_start(0xc630bc, 0xc630dc, count=12)
        got = struct.unpack('<ff', mu.mem_read(OBJ + 0xcc4, 8))
        assert got == (w, h), f'scene {w}x{h}: renderer +0xcc4 holds {got}'
        out.append(f'scene {w}x{h}')
    mu = machine(original)   # negative control: the parameter's size
    mu.reg_write(UC_ARM64_REG_X19, OBJ)
    mu.reg_write(UC_ARM64_REG_X0, OBJ + 0x2000)
    mu.reg_write(reg_s(8), 1600)
    mu.reg_write(reg_s(9), 900)
    mu.emu_start(0xc630bc, 0xc630dc, count=12)
    assert struct.unpack('<ff', mu.mem_read(OBJ + 0xcc4, 8)) == (1600, 900), 'original does not store the parameter size'
    return out


def aspect(original, patched):
    out = []
    game = 1920 / 1080
    for slot in (0.0, 21 / 9, 32 / 9, 1.6):
        for site, _, what in ASPECT_SITES:
            word = struct.unpack_from("<I", original, site)[0]   # (the site's own instruction, from the image)
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(aspect=(56, '<f', slot)))
                if word >> 22 == 0xbd400000 >> 22:          # ldr s0, [x19, #0x140]: the camera's ratio
                    mu.reg_write(UC_ARM64_REG_X19, OBJ)
                    mu.mem_write(OBJ + 0x140, struct.pack('<f', game))
                else:                                        # fdiv s0, s0, s1: viewport width / height
                    mu.reg_write(reg_s(0), bits(1920.0))
                    mu.reg_write(reg_s(1), bits(1080.0))
                mu.reg_write(UC_ARM64_REG_X10, 0x5555aaaa5555aaaa)
                mu.reg_write(UC_ARM64_REG_NZCV, 0x60000000)
                mu.emu_start(site, site + 4, count=12)
                assert mu.reg_read(UC_ARM64_REG_PC) == site + 4, f'{site:#x}: did not return'
                want = slot if name == 'patched' and slot else game
                assert abs(get_s(mu, 0) - want) < 1e-5, (name, hex(site), slot, get_s(mu, 0))
                assert mu.reg_read(UC_ARM64_REG_X10) == 0x5555aaaa5555aaaa, f'{site:#x}: x10 changed'
                assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{site:#x}: sp changed'
                assert mu.reg_read(UC_ARM64_REG_NZCV) == 0x60000000, f'{site:#x}: flags changed'
        out.append(f'slot {slot:.4f}: 4 sites')
    return out


def dynres(original, patched):
    """The controller's epilogue from main+0xc622ac to the orr: x0 = (unchanged-tier flag in x9) | scale, with the scale
    raised to the floor. x8's upper half holds garbage the original mov w8, w8 clears; x9 / x10 / sp must come back."""
    out = []
    flag = 1 << 33
    for floor in (0.0, 1.0, 0.75, 0.5):
        for scale in (1.0, 0.9, 0.8, 0.7, 0.5):
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(floor=(DP_RES_FLOOR, '<f', floor)))
                mu.reg_write(UC_ARM64_REG_X8, 0xdeadbeef00000000 | bits(scale))
                mu.reg_write(UC_ARM64_REG_X9, flag)
                mu.reg_write(UC_ARM64_REG_X10, 3)
                mu.reg_write(UC_ARM64_REG_X19, OBJ)
                mu.reg_write(UC_ARM64_REG_SP, STACK + 0x4000 - 0x30)   # the epilogue's ldp x20, x19 / ldp x29, x30
                mu.emu_start(DYNRES_SITE, DYNRES_SITE + 16, count=12)   # up to the ldp x29, x30 before ret
                assert mu.reg_read(UC_ARM64_REG_PC) == DYNRES_SITE + 16, f'{name}: did not reach the orr'
                want = max(scale, floor) if name == 'patched' else scale
                assert mu.reg_read(UC_ARM64_REG_X0) == flag | bits(want), (name, floor, scale, hex(mu.reg_read(UC_ARM64_REG_X0)))
                assert struct.unpack('<I', mu.mem_read(OBJ + 0x58, 4))[0] == 3, f'{name}: tier store changed'
                assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000 - 0x30, f'{name}: sp changed'
        out.append(f'floor {floor}: x0 = max(scale, floor) for scales 1.0 .. 0.5 (original: the scale)')
    return out


def fov(original, patched):
    """From the fovy load to the store of the projection's fovy (+0xa0): s0 = min(fovy * multiplier, FOV_MAX), s8 = half
    of it; the camera's own fovy is recorded on the page (+124); s1 (0.5), x10, q31, sp and the flags come back."""
    out = []
    for game, slot in ((0.8, 0.0), (0.8, 1.2), (0.8, 0.75), (0.8, 2.0), (1.6, 2.0)):   # radians (46 and 92 degrees)
        for site, proj_reg in zip(FOV_SITES, (UC_ARM64_REG_X20, UC_ARM64_REG_X23)):
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(fov=(DP_FOV_SCALE, '<f', slot)))
                mu.reg_write(UC_ARM64_REG_X19, OBJ)
                mu.mem_write(OBJ + 0x13c, struct.pack('<f', game))
                proj = OBJ + 0x1000
                mu.reg_write(proj_reg, proj)
                mu.reg_write(reg_s(1), bits(0.5))
                mu.reg_write(UC_ARM64_REG_X10, 0x5555aaaa5555aaaa)
                mu.reg_write(UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
                mu.reg_write(UC_ARM64_REG_NZCV, 0x60000000)
                mu.emu_start(site, site + 12, count=24)    # the load, fmul s8, s0, s1, str s0, [proj, #0xa0]
                assert mu.reg_read(UC_ARM64_REG_PC) == site + 12, f'{site:#x}: did not return'
                want = min(game * slot, FOV_MAX) if name == 'patched' and slot else game
                assert abs(get_s(mu, 0) - want) < 1e-5 and abs(get_s(mu, 8) - want / 2) < 1e-5, (name, hex(site), slot, get_s(mu, 0))
                assert abs(struct.unpack('<f', mu.mem_read(proj + 0xa0, 4))[0] - want) < 1e-5, f'{site:#x}: projection fovy'
                base = struct.unpack('<f', mu.mem_read(DATA + DP_FOV_BASE, 4))[0]
                assert abs(base - (game if name == 'patched' else 0.0)) < 1e-6, f'{site:#x}: recorded fovy {base}'
                assert get_s(mu, 1) == 0.5, f'{site:#x}: s1 changed'
                assert mu.reg_read(UC_ARM64_REG_X10) == 0x5555aaaa5555aaaa, f'{site:#x}: x10 changed'
                assert mu.reg_read(UC_ARM64_REG_Q31) == 0x0123456789abcdef0011223344556677, f'{site:#x}: q31 changed'
                assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{site:#x}: sp changed'
                assert mu.reg_read(UC_ARM64_REG_NZCV) == 0x60000000, f'{site:#x}: flags changed'
        out.append(f'fovy {game} x {slot}: 2 sites (ceiling {FOV_MAX}, fovy recorded)')
    return out


def shadowres(original, patched):
    """Only the size multiply: s1 = factor * multiplier * base width, s0 = the scaled factor; x10 (the 32 minimum), q31,
    sp and the flags come back."""
    out = []
    for slot in (0.0, 2.0, 0.5):
        for name, image in (('patched', patched), ('original', original)):
            mu = machine(image, _page(shadow=(DP_SHADOW_SCALE, '<f', slot)))
            mu.reg_write(reg_s(0), bits(0.75))
            mu.reg_write(reg_s(2), bits(2048.0))
            mu.reg_write(UC_ARM64_REG_X10, 0x20)
            mu.reg_write(UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
            mu.reg_write(UC_ARM64_REG_NZCV, 0x60000000)
            mu.emu_start(SHADOW_SITE, SHADOW_SITE + 4, count=20)
            assert mu.reg_read(UC_ARM64_REG_PC) == SHADOW_SITE + 4, 'shadowres: did not return'
            k = slot if name == 'patched' and slot else 1.0
            assert abs(get_s(mu, 1) - 0.75 * k * 2048) < 1e-2 and abs(get_s(mu, 0) - 0.75 * k) < 1e-6, (name, slot, get_s(mu, 1))
            assert mu.reg_read(UC_ARM64_REG_X10) == 0x20, 'shadowres: x10 changed'
            assert mu.reg_read(UC_ARM64_REG_Q31) == 0x0123456789abcdef0011223344556677, 'shadowres: q31 changed'
            assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, 'shadowres: sp changed'
            assert mu.reg_read(UC_ARM64_REG_NZCV) == 0x60000000, 'shadowres: flags changed'
        out.append(f'multiplier {slot}')
    return out


def lod(original, patched):
    """Each fdiv site: s<m> = numerator / s0 * multiplier; x10, q31, sp and the flags come back."""
    out = []
    for slot in (0.0, 2.0, 4.0):
        for site, m, num in LOD_SITES:
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(lod=(DP_LOD_SCALE, '<f', slot)))
                mu.reg_write(reg_s(num), bits(-30.0))
                mu.reg_write(reg_s(0), bits(-120.0))
                mu.reg_write(UC_ARM64_REG_X10, 0x5555aaaa5555aaaa)
                mu.reg_write(UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
                mu.reg_write(UC_ARM64_REG_NZCV, 0x60000000)
                mu.emu_start(site, site + 4, count=20)
                assert mu.reg_read(UC_ARM64_REG_PC) == site + 4, f'{site:#x}: did not return'
                want = 0.25 * (slot if name == 'patched' and slot else 1.0)
                assert abs(get_s(mu, m) - want) < 1e-6, (name, hex(site), slot, get_s(mu, m))
                assert mu.reg_read(UC_ARM64_REG_X10) == 0x5555aaaa5555aaaa, f'{site:#x}: x10 changed'
                assert mu.reg_read(UC_ARM64_REG_Q31) == 0x0123456789abcdef0011223344556677, f'{site:#x}: q31 changed'
                assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{site:#x}: sp changed'
                assert mu.reg_read(UC_ARM64_REG_NZCV) == 0x60000000, f'{site:#x}: flags changed'
        out.append(f'multiplier {slot}: 3 sites')
    return out


def mouse(original, patched):
    """The camera pointer store, then the input veneer from the stick step's x store (main+0xd05364) to the next
    instruction: s11 (x) / s10 (y) after the mouse, the store of x, the claim, the beat and the taken totals for each
    path (apply, prime, stale steps, released, multiplier 0, stick off, scripted camera); x11, q4 and sp come back.
    The veneer's stale threshold must be the one the host releases the claim at (data_page.h, review D5)."""
    out = []
    m = re.search(r'#define\s+TOTK_MOUSE_STALE_PRESENTS\s+(\d+)u?\b', DATA_PAGE_H.read_text(encoding='utf-8'))
    assert m, f'TOTK_MOUSE_STALE_PRESENTS not found in {DATA_PAGE_H}'
    assert int(m.group(1)) == MOUSE_STALE, f'data_page.h TOTK_MOUSE_STALE_PRESENTS {m.group(1)} != sites_v100 MOUSE_STALE {MOUSE_STALE}'
    out.append(f'stale threshold {MOUSE_STALE} presents in both the veneer and data_page.h')
    mu = machine(patched)
    mu.reg_write(UC_ARM64_REG_X19, OBJ)
    mu.emu_start(CAMERA_SITE, CAMERA_SITE + 4, count=8)
    assert mu.reg_read(UC_ARM64_REG_PC) == CAMERA_SITE + 4, 'camera pointer: did not return'
    assert mu.reg_read(UC_ARM64_REG_X0) == OBJ, 'camera pointer: x0 is not the PlayerCamera'
    assert struct.unpack('<Q', mu.mem_read(DATA + DP_CAMERA, 8))[0] == OBJ, 'camera pointer: not stored at +136'
    out.append('camera pointer stored at +136, x0 = x19')
    x0, y0, k = 0.25, -0.5, 1.0 / 0.5 * 0.02   # stick after the game's processing; k = multiplier / ratio * rate
    # name, multiplier, captured, claimed, beat (presents = 10), w21, scripted -> x, y, claim after, totals taken
    cases = (('apply', 1.0, 1, 1, 9, 0, 0, x0 + 10 * k, y0 - 10 * k, 1, True),
             (f'apply after {MOUSE_STALE}', 1.0, 1, 1, 10 - MOUSE_STALE, 0, 0, x0 + 10 * k, y0 - 10 * k, 1, True),
             ('prime', 1.0, 1, 0, 9, 0, 0, x0, y0, 1, True),
             ('stale', 1.0, 1, 1, 9 - MOUSE_STALE, 0, 0, x0, y0, 1, True),
             ('released', 1.0, 0, 1, 9, 0, 0, x0, y0, 0, False),
             ('multiplier 0', 0.0, 1, 1, 9, 0, 0, x0, y0, 0, False),
             ('stick off', 1.0, 1, 1, 9, 1, 0, x0, y0, 1, True),
             ('scripted', 1.0, 1, 1, 9, 0, 2, x0, y0, 1, True))
    for name, scale, captured, claimed, beat, w21, script, wx, wy, wclaim, taken in cases:
        page = _page(ratio=(DP_FRAME_RATIO, '<f', 0.5), mouse=(DP_MOUSE_SCALE, '<ff', scale, 0.02),
                     beat=(DP_PRESENTS, '<II', 10, beat), last=(DP_MOUSE_LAST, '<dd', 90.0, 40.0),
                     totals=(DP_PC + PC_MOUSE_X, '<dd', 100.0, 50.0), cap=(DP_PC + PC_CAPTURED, '<I', captured),
                     claim=(DP_PC + PC_CLAIMED, '<I', claimed))
        for image in ('patched', 'original'):
            mu = machine(patched if image == 'patched' else original, page)
            mu.reg_write(UC_ARM64_REG_X20, OBJ)
            mu.mem_write(OBJ + 0x140, bytes([script]))
            mu.reg_write(UC_ARM64_REG_X21, w21)
            mu.reg_write(reg_s(11), bits(x0))
            mu.reg_write(reg_s(10), bits(y0))
            mu.reg_write(UC_ARM64_REG_X11, 0x5555aaaa5555aaaa)
            mu.reg_write(UC_ARM64_REG_Q4, 0x0123456789abcdef0011223344556677)
            mu.emu_start(MOUSE_SITE, MOUSE_SITE + 4, count=60)
            assert mu.reg_read(UC_ARM64_REG_PC) == MOUSE_SITE + 4, f'mouse {name} ({image}): did not return'
            want = (wx, wy) if image == 'patched' else (x0, y0)
            got = (get_s(mu, 11), get_s(mu, 10))
            assert all(abs(g - w) < 1e-5 for g, w in zip(got, want)), (name, image, got, want)
            assert abs(struct.unpack('<f', mu.mem_read(OBJ + 0x418, 4))[0] - want[0]) < 1e-5, f'mouse {name} ({image}): x store'
            assert mu.reg_read(UC_ARM64_REG_X11) == 0x5555aaaa5555aaaa, f'mouse {name}: x11 changed'
            assert mu.reg_read(UC_ARM64_REG_Q4) == 0x0123456789abcdef0011223344556677, f'mouse {name}: q4 changed'
            assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'mouse {name}: sp changed'
            if image == 'original':
                continue
            pg = bytes(mu.mem_read(DATA, 0x200))
            assert struct.unpack_from('<I', pg, DP_PC + PC_CLAIMED)[0] == wclaim, f'mouse {name}: claim'
            assert struct.unpack_from('<dd', pg, DP_MOUSE_LAST) == ((100.0, 50.0) if taken else (90.0, 40.0)), f'mouse {name}: totals'
            assert struct.unpack_from('<I', pg, DP_MOUSE_BEAT)[0] == (10 if taken else beat), f'mouse {name}: beat'
            assert struct.unpack_from('<I', pg, DP_MOUSE_PITCH_ACK)[0] == name.startswith('apply'), f'mouse {name}: ack'
        out.append(f'{name}: x {wx:g}, y {wy:g}, claim {wclaim}')
    # Pitch (applied step): gain g > 0 turns y by sign(v) sqrt(g |v|), v = dy * k; g <= 0 or NaN keeps y -= v.
    m = re.search(r'TOTK_DP_MOUSE_PITCH\s*=\s*(\d+)', DATA_PAGE_H.read_text(encoding='utf-8'))
    assert m and int(m.group(1)) == DP_MOUSE_PITCH, f'data_page.h TOTK_DP_MOUSE_PITCH != sites_v100 DP_MOUSE_PITCH {DP_MOUSE_PITCH}'
    m = re.search(r'TOTK_DP_MOUSE_PITCH_ACK\s*=\s*(\d+)', DATA_PAGE_H.read_text(encoding='utf-8'))
    assert m and int(m.group(1)) == DP_MOUSE_PITCH_ACK, f'data_page.h TOTK_DP_MOUSE_PITCH_ACK != {DP_MOUSE_PITCH_ACK}'
    for gain in (0.0, -1.0, float('nan'), 70.0 / 51.0, 4.0):
        for ty in (50.0, 30.0, 40.0, 40.0 + 1e-6):
            v = (ty - 40.0) * k
            shaped = gain > 0
            wy = y0 - (math.copysign(math.sqrt(gain * abs(v)), v) if shaped else v)
            page = _page(ratio=(DP_FRAME_RATIO, '<f', 0.5), mouse=(DP_MOUSE_SCALE, '<ff', 1.0, 0.02),
                         beat=(DP_PRESENTS, '<II', 10, 9), last=(DP_MOUSE_LAST, '<dd', 90.0, 40.0),
                         totals=(DP_PC + PC_MOUSE_X, '<dd', 100.0, ty), cap=(DP_PC + PC_CAPTURED, '<I', 1),
                         claim=(DP_PC + PC_CLAIMED, '<I', 1), pitch=(DP_MOUSE_PITCH, '<f', gain))
            mu = machine(patched, page)
            mu.reg_write(UC_ARM64_REG_X20, OBJ)
            mu.mem_write(OBJ + 0x140, bytes([0]))
            mu.reg_write(UC_ARM64_REG_X21, 0)
            mu.reg_write(reg_s(11), bits(x0))
            mu.reg_write(reg_s(10), bits(y0))
            mu.reg_write(UC_ARM64_REG_Q4, 0x0123456789abcdef0011223344556677)
            mu.emu_start(MOUSE_SITE, MOUSE_SITE + 4, count=60)
            assert mu.reg_read(UC_ARM64_REG_PC) == MOUSE_SITE + 4, f'mouse pitch {gain} {ty}: did not return'
            got = (get_s(mu, 11), get_s(mu, 10))
            assert abs(got[0] - (x0 + 10 * k)) < 1e-5 and abs(got[1] - wy) < 1e-5, ('pitch', gain, ty, got, wy)
            assert mu.reg_read(UC_ARM64_REG_Q4) == 0x0123456789abcdef0011223344556677, f'mouse pitch {gain}: q4 changed'
            assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'mouse pitch {gain}: sp changed'
            assert struct.unpack('<I', mu.mem_read(DATA + DP_MOUSE_PITCH_ACK, 4))[0] == 1, f'mouse pitch {gain}: ack'
        out.append(f'pitch gain {gain:g}: ' + ('y -= sign(v) sqrt(g |v|)' if gain > 0 else 'linear') +
                   ' for dy +10, -10, 0, 1e-6; ack 1')
    return out


def actor_lod(original, patched):
    """From the distance (main+0x742714) to the area comparisons (0x742748), through both nearest-distance paths (none
    yet: NaN; one already stored, nearer or further): s14 = d^2 / k^2, the stored nearest distance (sp+0xe0) unscaled
    (review D2), s0 = the original load (sp+0xd0); x10, q31 and sp come back."""
    out = []
    d2 = 3.0 ** 2 + 4.0 ** 2
    for slot in (0.0, 1.0, 2.0, 0.5):
        for nearest in (float('nan'), 1.0, 1000.0):
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(alod=(DP_ACTOR_LOD, '<f', slot)))
                sp = STACK + 0x4000
                mu.mem_write(sp + 0xe0, struct.pack('<f', nearest))
                mu.mem_write(sp + 0xd0, struct.pack('<f', 7.0))
                mu.reg_write(reg_s(0), bits(9.0 + 16.0))
                mu.reg_write(reg_s(1), bits(0.0))
                mu.reg_write(UC_ARM64_REG_X10, 0x5555aaaa5555aaaa)
                mu.reg_write(UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
                mu.emu_start(0x742714, ACTOR_LOD_SITE + 4, count=40)
                assert mu.reg_read(UC_ARM64_REG_PC) == ACTOR_LOD_SITE + 4, f'actor_lod {slot}: did not return'
                k2 = slot * slot if name == 'patched' and slot else 1.0
                assert abs(get_s(mu, 14) - d2 / k2) < 1e-4, (name, slot, nearest, get_s(mu, 14))
                stored = struct.unpack('<f', mu.mem_read(sp + 0xe0, 4))[0]
                want = d2 if nearest != nearest else min(d2, nearest)
                assert stored == want, f'actor_lod {slot} ({name}, nearest {nearest}): sp+0xe0 = {stored}, not {want}'
                assert get_s(mu, 0) == 7.0, f'actor_lod {slot} ({name}): s0 is not the original load'
                assert mu.reg_read(UC_ARM64_REG_X10) == 0x5555aaaa5555aaaa, f'actor_lod {slot}: x10 changed'
                assert mu.reg_read(UC_ARM64_REG_Q31) == 0x0123456789abcdef0011223344556677, f'actor_lod {slot}: q31 changed'
                assert mu.reg_read(UC_ARM64_REG_SP) == sp, f'actor_lod {slot}: sp changed'
        out.append(f'multiplier {slot}: s14 = d^2 / {slot * slot if slot else 1:g}, nearest distance unscaled')
    return out


def terrain_lod(original, patched):
    """Each quadtree distance read, from the site to the next instruction: the split distance end[L] x k while the node
    pool is less than half full (context at sp+0xd0 or x0), the morph flag's end[L - 1] and the uniform block's
    pairs x k; x8 / x0 / x11 (the pre-index writeback included), x10, x11, q31 and sp come back, and the uniform pair
    veneer (inside the copy loop, before its b.ne) keeps the flags."""
    out = []
    table, ctx, pool = OBJ, OBJ + 0x200, OBJ + 0x300
    scratch = dict(x10=0x5555aaaa5555aaaa, x11=0x1111222233334444)

    def run(image, site, slot, setup, check):
        mu = machine(image, _page(tlod=(DP_TERRAIN_LOD, '<f', slot)))
        sp = STACK + 0x4000
        mu.mem_write(table, struct.pack('<4f', 30.0, 100.0, 70.0, 200.0))
        mu.reg_write(UC_ARM64_REG_X8, table)
        mu.reg_write(UC_ARM64_REG_X10, scratch['x10'])
        mu.reg_write(UC_ARM64_REG_Q31, 0x0123456789abcdef0011223344556677)
        mu.reg_write(UC_ARM64_REG_NZCV, 0x60000000)
        setup(mu, sp)
        mu.emu_start(site, site + 4, count=30)
        assert mu.reg_read(UC_ARM64_REG_PC) == site + 4, f'terrain_lod {site:#x}: did not return'
        assert mu.reg_read(UC_ARM64_REG_X10) == scratch['x10'], f'terrain_lod {site:#x}: x10 changed'
        assert mu.reg_read(UC_ARM64_REG_Q31) == 0x0123456789abcdef0011223344556677, f'terrain_lod {site:#x}: q31'
        assert mu.reg_read(UC_ARM64_REG_SP) == sp, f'terrain_lod {site:#x}: sp changed'
        check(mu)

    for site, r, base in TERRAIN_LOD_THRESHOLD:
        for slot in (0.0, 1.0, 2.0, 0.5):
            for used in (10, 49, 50, 99):            # nodes taken of 100
                def setup(mu, sp, used=used):
                    mu.mem_write(pool, struct.pack('<I', 100))
                    at = sp + TERRAIN_CTX_SP if base == 'sp' else ctx
                    mu.mem_write(at, struct.pack('<Q', pool))
                    mu.mem_write(at + 0x18, struct.pack('<i', used))
                    mu.reg_write(UC_ARM64_REG_X0, ctx)
                    mu.reg_write(UC_ARM64_REG_X11, scratch['x11'])
                for name, image in (('patched', patched), ('original', original)):
                    k = slot if name == 'patched' and slot and 2 * used < 100 else 1.0
                    def check(mu, k=k, name=name):
                        assert get_s(mu, r) == 100.0 * k, (name, hex(site), slot, used, get_s(mu, r))
                        assert mu.reg_read(UC_ARM64_REG_X11) == scratch['x11'], f'{site:#x}: x11 changed'
                        assert mu.reg_read(UC_ARM64_REG_X0) == ctx and mu.reg_read(UC_ARM64_REG_X8) == table
                    run(image, site, slot, setup, check)
        out.append(f'split main+{site:#x} (context {base}): end[L] x k below half the pool, else the game\'s')
    for slot in (0.0, 1.0, 2.0, 0.5):
        for name, image in (('patched', patched), ('original', original)):
            k = slot if name == 'patched' and slot else 1.0
            for site, r in TERRAIN_LOD_MORPH_FLAG:
                def check(mu, r=r, k=k):
                    assert get_s(mu, r) == 100.0 * k, (name, hex(site), slot, get_s(mu, r))
                run(image, site, slot, lambda mu, sp: None, check)
            site = TERRAIN_LOD_MORPH_FIRST
            def check(mu, k=k):
                assert get_s(mu, 4) == 200.0 * k, (name, 'first', slot, get_s(mu, 4))
                assert mu.reg_read(UC_ARM64_REG_X0) == table + 0xc, 'pre-index writeback lost'
            run(image, site, slot, lambda mu, sp: mu.reg_write(UC_ARM64_REG_X0, table), check)
            site = TERRAIN_LOD_MORPH_PAIR
            def check(mu, k=k):
                assert (get_s(mu, 4), get_s(mu, 5)) == (70.0 * k, 200.0 * k), (name, 'pair', slot)
                assert mu.reg_read(UC_ARM64_REG_X11) == table + 0xc, 'x11 changed'
                assert mu.reg_read(UC_ARM64_REG_NZCV) == 0x60000000, 'flags changed inside the copy loop'
            run(image, site, slot, lambda mu, sp: mu.reg_write(UC_ARM64_REG_X11, table + 0xc), check)
        out.append(f'multiplier {slot}: morph flag and uniform pairs x {slot if slot else 1:g}')
    return out


def sim_radius(original, patched):
    """From the nearest-distance load (main+0x742f60) to the call of FUN_70062c: sp+0xb98 = d^2 / k^2 (the original:
    d^2, canonical NaN without a camera), x0 = the actor (x25), x1 = sp + 0xb98, sp unchanged."""
    out = []
    sp = STACK + 0x4000
    for slot in (1.0, 2.0, 1.5, 0.5):
        for d2 in (25.0, 1.0e6, float('nan')):
            for name, image in (('patched', patched), ('original', original)):
                mu = machine(image, _page(sim=(DP_SIM_RADIUS, '<f', slot)))
                mu.mem_write(sp + 0xe0, struct.pack('<f', d2))
                mu.reg_write(UC_ARM64_REG_X25, OBJ)
                mu.emu_start(SIM_SITE - 4, SIM_SITE + 0x1c, count=8)
                got = struct.unpack('<f', mu.mem_read(sp + 0xb98, 4))[0]
                if d2 != d2:
                    assert got != got, (name, slot, got)
                else:
                    k2 = slot * slot if name == 'patched' else 1.0
                    assert abs(got - d2 / k2) <= 1e-6 * d2 / k2, (name, slot, d2, got)
                assert mu.reg_read(UC_ARM64_REG_X0) == OBJ and mu.reg_read(UC_ARM64_REG_X1) == sp + 0xb98, name
                assert mu.reg_read(UC_ARM64_REG_SP) == sp, f'sim_radius {slot}: sp changed'
        out.append(f'multiplier {slot}: FUN_70062c gets d^2 / {slot * slot:g}, NaN kept')
    return out


def verify(original, patched):
    results = []
    for fn in (highfps, output_res, render_res, aspect, dynres, fov, shadowres, lod, mouse, actor_lod, terrain_lod, sim_radius):
        try:
            results.append(dict(name=fn.__name__, status='pass', cases=fn(original, patched)))
        except AssertionError as e:
            results.append(dict(name=fn.__name__, status='fail', failures=[str(e)]))
    return results
