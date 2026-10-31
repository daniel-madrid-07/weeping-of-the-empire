"""The few AArch64 encodings the TotK patches build (the rest of the words are literal and named in sites_v100.py).

Every encoder checks its operands and raises ValueError instead of wrapping silently: a word that does not fit is a
patch that would branch or load from the wrong place. Offsets are module offsets (main+X); ADRP is page relative, so a
module base aligned to 4 KiB (every base here is 2 MiB aligned) gives the same word at any base.
Ported from MacNativeGames (MIT): games/totk/runtime/game_patches.c, aspect_math.h.
"""

NOP = 0xd503201f


def _reg(r):
    if not 0 <= r <= 31:
        raise ValueError(f'register {r}')
    return r


def b(site, target):
    """b <target> at site."""
    delta = target - site
    if delta & 3 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError(f'b {site:#x} -> {target:#x} out of range')
    return 0x14000000 | ((delta >> 2) & 0x3ffffff)


def bl(site, target):
    return b(site, target) | 0x80000000


def adrp(rd, site, target):
    """adrp x<rd>, <page of target> at site."""
    pages = (target >> 12) - (site >> 12)
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError(f'adrp {site:#x} -> {target:#x} out of range')
    imm = pages & 0x1fffff
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | _reg(rd)


def movz_x(rd, imm16, shift=0):
    if not 0 <= imm16 <= 0xffff or shift not in (0, 16, 32, 48):
        raise ValueError(f'movz #{imm16:#x}, lsl #{shift}')
    return 0xd2800000 | (shift // 16) << 21 | imm16 << 5 | _reg(rd)


def movk_x(rd, imm16, shift=0):
    if not 0 <= imm16 <= 0xffff or shift not in (0, 16, 32, 48):
        raise ValueError(f'movk #{imm16:#x}, lsl #{shift}')
    return 0xf2800000 | (shift // 16) << 21 | imm16 << 5 | _reg(rd)


def ldr_s(rt, rn, off):
    """ldr s<rt>, [x<rn>, #off] (unsigned offset)."""
    if off & 3 or not 0 <= off < 0x4000:
        raise ValueError(f'ldr s, #{off}')
    return 0xbd400000 | (off // 4) << 10 | _reg(rn) << 5 | _reg(rt)


def str_s(rt, rn, off):
    if off & 3 or not 0 <= off < 0x4000:
        raise ValueError(f'str s, #{off}')
    return 0xbd000000 | (off // 4) << 10 | _reg(rn) << 5 | _reg(rt)


def ldr_w(rt, rn, off):
    if off & 3 or not 0 <= off < 0x4000:
        raise ValueError(f'ldr w, #{off}')
    return 0xb9400000 | (off // 4) << 10 | _reg(rn) << 5 | _reg(rt)


def ldr_x(rt, rn, off):
    if off & 7 or not 0 <= off < 0x8000:
        raise ValueError(f'ldr x, #{off}')
    return 0xf9400000 | (off // 8) << 10 | _reg(rn) << 5 | _reg(rt)


def str_x(rt, rn, off):
    if off & 7 or not 0 <= off < 0x8000:
        raise ValueError(f'str x, #{off}')
    return 0xf9000000 | (off // 8) << 10 | _reg(rn) << 5 | _reg(rt)


def ldp_s(rt, rt2, rn, off):
    """ldp s<rt>, s<rt2>, [x<rn>, #off] (signed offset)."""
    if off & 3 or not -256 <= off < 256:
        raise ValueError(f'ldp s, #{off}')
    return 0x2d400000 | ((off // 4) & 0x7f) << 15 | _reg(rt2) << 10 | _reg(rn) << 5 | _reg(rt)


def stp_s(rt, rt2, rn, off):
    if off & 3 or not -256 <= off < 256:
        raise ValueError(f'stp s, #{off}')
    return 0x2d000000 | ((off // 4) & 0x7f) << 15 | _reg(rt2) << 10 | _reg(rn) << 5 | _reg(rt)


def fmul_s(rd, rn, rm):
    return 0x1e200800 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def fmov_s_w(rd, rn):
    """fmov s<rd>, w<rn>."""
    return 0x1e270000 | _reg(rn) << 5 | _reg(rd)


def cbz_w(rt, site, target):
    delta = target - site
    if delta & 3 or not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('cbz out of range')
    return 0x34000000 | ((delta >> 2) & 0x7ffff) << 5 | _reg(rt)


def cmp_w(rn, rm, lsl=0):
    """cmp w<rn>, w<rm>[, lsl #lsl] (subs wzr, w<rn>, w<rm>, lsl #lsl)."""
    if not 0 <= lsl < 32:
        raise ValueError(f'cmp w, lsl #{lsl}')
    return 0x6b00001f | _reg(rm) << 16 | lsl << 10 | _reg(rn) << 5


COND = dict(eq=0, ne=1, hs=2, lo=3, mi=4, pl=5, vs=6, vc=7, hi=8, ls=9, ge=10, lt=11, gt=12, le=13)


def csel_w(rd, rn, rm, cond):
    """csel w<rd>, w<rn>, w<rm>, <cond>."""
    return 0x1a800000 | _reg(rm) << 16 | COND[cond] << 12 | _reg(rn) << 5 | _reg(rd)


def ldr_d(rt, rn, off):
    """ldr d<rt>, [x<rn>, #off] (unsigned offset)."""
    if off & 7 or not 0 <= off < 0x8000:
        raise ValueError(f'ldr d, #{off}')
    return 0xfd400000 | (off // 8) << 10 | _reg(rn) << 5 | _reg(rt)


def str_d(rt, rn, off):
    if off & 7 or not 0 <= off < 0x8000:
        raise ValueError(f'str d, #{off}')
    return 0xfd000000 | (off // 8) << 10 | _reg(rn) << 5 | _reg(rt)


def str_w(rt, rn, off):
    if off & 3 or not 0 <= off < 0x4000:
        raise ValueError(f'str w, #{off}')
    return 0xb9000000 | (off // 4) << 10 | _reg(rn) << 5 | _reg(rt)


def ldrb_w(rt, rn, off):
    if not 0 <= off < 0x1000:
        raise ValueError(f'ldrb w, #{off}')
    return 0x39400000 | off << 10 | _reg(rn) << 5 | _reg(rt)


def movz_w(rd, imm16, shift=0):
    if not 0 <= imm16 <= 0xffff or shift not in (0, 16):
        raise ValueError(f'movz w, #{imm16:#x}, lsl #{shift}')
    return 0x52800000 | shift // 16 << 21 | imm16 << 5 | _reg(rd)


def sub_w(rd, rn, rm):
    return 0x4b000000 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def cmp_w_imm(rn, imm):
    """cmp w<rn>, #imm (subs wzr, w<rn>, #imm)."""
    if not 0 <= imm < 0x1000:
        raise ValueError(f'cmp w, #{imm}')
    return 0x7100001f | imm << 10 | _reg(rn) << 5


def fadd_s(rd, rn, rm):
    return 0x1e202800 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def fsub_s(rd, rn, rm):
    return 0x1e203800 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def fdiv_s(rd, rn, rm):
    return 0x1e201800 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def fsub_d(rd, rn, rm):
    return 0x1e603800 | _reg(rm) << 16 | _reg(rn) << 5 | _reg(rd)


def fcvt_s_d(rd, rn):
    """fcvt s<rd>, d<rn>."""
    return 0x1e624000 | _reg(rn) << 5 | _reg(rd)


def fabs_s(rd, rn):
    return 0x1e20c000 | _reg(rn) << 5 | _reg(rd)


def fneg_s(rd, rn):
    return 0x1e214000 | _reg(rn) << 5 | _reg(rd)


def fsqrt_s(rd, rn):
    return 0x1e21c000 | _reg(rn) << 5 | _reg(rd)


def fcmp_s_zero(rn):
    """fcmp s<rn>, #0.0."""
    return 0x1e202008 | _reg(rn) << 5


def fcsel_s(rd, rn, rm, cond):
    """fcsel s<rd>, s<rn>, s<rm>, <cond>."""
    return 0x1e200c00 | _reg(rm) << 16 | COND[cond] << 12 | _reg(rn) << 5 | _reg(rd)


def _imm14(site, target):
    delta = target - site
    if delta & 3 or not -(1 << 15) <= delta < (1 << 15):
        raise ValueError('tbz out of range')
    return (delta >> 2) & 0x3fff


def tbz(rt, bit, site, target):
    """tbz w<rt>, #bit, <target> (bit < 32)."""
    if not 0 <= bit < 32:
        raise ValueError(f'tbz bit {bit}')
    return 0x36000000 | bit << 19 | _imm14(site, target) << 5 | _reg(rt)


def tbnz(rt, bit, site, target):
    return tbz(rt, bit, site, target) | 0x01000000


def b_cond(cond, site, target):
    delta = target - site
    if delta & 3 or not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('b.cond out of range')
    return 0x54000000 | ((delta >> 2) & 0x7ffff) << 5 | COND[cond]
