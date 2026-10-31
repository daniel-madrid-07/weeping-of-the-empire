// TotK recomp runtime: guest CPU state and AArch64 instruction semantics used by generated code.
// Guest memory is identity-mapped: a guest address is a host address.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <climits>
#include <limits>
#include <utility>
#include <type_traits>
#include <immintrin.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef __int128 s128; typedef unsigned __int128 u128;

#define RT_INLINE static inline __attribute__((always_inline))

// Compile-time unrolled lane loop: LANES(B, E, stmt using constexpr int i). Generated code never contains real
// loops for lanes: thousands of tiny loops in one huge function make LLVM's per-loop passes (LICM, LoopRotate)
// quadratic (measured 2026-10-14: an 18k-insn function took > 300 s at -O1 / -Og, 6 s at -O0).
template <int B, class F, int... I> RT_INLINE void rt_unroll_(F& f, std::integer_sequence<int, I...>) { (f(std::integral_constant<int, B + I>{}), ...); }
template <int B, int E, class F> RT_INLINE void rt_unroll(F&& f) { if constexpr (E > B) rt_unroll_<B>(f, std::make_integer_sequence<int, E - B>{}); }
#define LANES(B, E, ...) rt_unroll<(B), (E)>([&](auto i_) __attribute__((always_inline)) { constexpr int i = decltype(i_)::value; __VA_ARGS__; })

union V128 {
    u8 b[16]; u16 h[8]; u32 s[4]; u64 d[2];
    s8 sb[16]; s16 sh[8]; s32 ss[4]; s64 sd[2];
    float f[4]; double fd[2];
};

struct Ctx {
    u64 x[32];          // x[31] unused: xzr reads are emitted as 0, sp lives in .sp
    u64 sp;
    V128 v[32];
    u8 n, z, c, vf;     // NZCV
    u32 fpcr, fpsr;
    u64 tpidr;          // TPIDR_EL0
    u64 excl_addr;      // exclusive monitor (~0 = open)
    u64 excl_val;
    u64 excl_val2;
    void* host;         // runtime per-thread data
};

// ---------------------------------------------------------------- memory (volatile: keep guest access order/count)
typedef u16 u16a __attribute__((aligned(1))); typedef u32 u32a __attribute__((aligned(1)));
typedef u64 u64a __attribute__((aligned(1)));
RT_INLINE u8  ld8 (u64 a) { return *(volatile u8*)a; }
RT_INLINE u16 ld16(u64 a) { return *(volatile u16a*)a; }
RT_INLINE u32 ld32(u64 a) { return *(volatile u32a*)a; }
RT_INLINE u64 ld64(u64 a) { return *(volatile u64a*)a; }
RT_INLINE void st8 (u64 a, u8 v)  { *(volatile u8*)a = v; }
RT_INLINE void st16(u64 a, u16 v) { *(volatile u16a*)a = v; }
RT_INLINE void st32(u64 a, u32 v) { *(volatile u32a*)a = v; }
RT_INLINE void st64(u64 a, u64 v) { *(volatile u64a*)a = v; }
RT_INLINE V128 ld128(u64 a) { V128 r; r.d[0] = ld64(a); r.d[1] = ld64(a + 8); return r; }
RT_INLINE void st128(u64 a, const V128& v) { st64(a, v.d[0]); st64(a + 8, v.d[1]); }
RT_INLINE float  ldf32(u64 a) { u32 b = ld32(a); float f; memcpy(&f, &b, 4); return f; }

// ---------------------------------------------------------------- vector register helpers
RT_INLINE V128 vzero() { V128 r; r.d[0] = 0; r.d[1] = 0; return r; }
RT_INLINE V128 vS(float f) { V128 r = vzero(); r.f[0] = f; return r; }
RT_INLINE V128 vD(double d) { V128 r = vzero(); r.fd[0] = d; return r; }
RT_INLINE V128 vU32(u32 x) { V128 r = vzero(); r.s[0] = x; return r; }
RT_INLINE V128 vU64(u64 x) { V128 r = vzero(); r.d[0] = x; return r; }
RT_INLINE V128 vU16(u16 x) { V128 r = vzero(); r.h[0] = x; return r; }
RT_INLINE V128 vU8(u8 x) { V128 r = vzero(); r.b[0] = x; return r; }
RT_INLINE V128 vlo(V128 a) { a.d[1] = 0; return a; }          // 64-bit arrangement result
RT_INLINE V128 v2(u64 lo, u64 hi) { V128 r; r.d[0] = lo; r.d[1] = hi; return r; }
RT_INLINE float  bitsf(u32 x) { float f; memcpy(&f, &x, 4); return f; }
RT_INLINE double bitsd(u64 x) { double f; memcpy(&f, &x, 8); return f; }
RT_INLINE u32 fbits(float f) { u32 x; memcpy(&x, &f, 4); return x; }
RT_INLINE u64 dbits(double f) { u64 x; memcpy(&x, &f, 8); return x; }

// ---------------------------------------------------------------- flags
// (templates: generated code passes its LCtx, see the end of this file; runtime code a Ctx)
template <class C> RT_INLINE void nz32(C* c, u32 r) { c->n = r >> 31; c->z = r == 0; }
template <class C> RT_INLINE void nz64(C* c, u64 r) { c->n = r >> 63; c->z = r == 0; }
template <class C> RT_INLINE u64 adds64(C* c, u64 a, u64 b, u64 cin = 0) {
    u128 w = (u128)a + b + cin; u64 r = (u64)w;
    c->n = r >> 63; c->z = r == 0; c->c = (u8)(w >> 64);
    c->vf = (u8)((~(a ^ b) & (a ^ r)) >> 63); return r;
}
template <class C> RT_INLINE u32 adds32(C* c, u32 a, u32 b, u32 cin = 0) {
    u64 w = (u64)a + b + cin; u32 r = (u32)w;
    c->n = r >> 31; c->z = r == 0; c->c = (u8)(w >> 32);
    c->vf = (u8)((~(a ^ b) & (a ^ r)) >> 31); return r;
}
template <class C> RT_INLINE u64 subs64(C* c, u64 a, u64 b) { return adds64(c, a, ~b, (u64)1); }
template <class C> RT_INLINE u32 subs32(C* c, u32 a, u32 b) { return adds32(c, a, ~b, (u32)1); }
template <class C> RT_INLINE void setnzcv(C* c, u32 f) { c->n = (f >> 3) & 1; c->z = (f >> 2) & 1; c->c = (f >> 1) & 1; c->vf = f & 1; }

#define C_EQ (c->z)
#define C_NE (!c->z)
#define C_HS (c->c)
#define C_LO (!c->c)
#define C_MI (c->n)
#define C_PL (!c->n)
#define C_VS (c->vf)
#define C_VC (!c->vf)
#define C_HI (c->c && !c->z)
#define C_LS (!(c->c && !c->z))
#define C_GE (c->n == c->vf)
#define C_LT (c->n != c->vf)
#define C_GT (!c->z && c->n == c->vf)
#define C_LE (!(!c->z && c->n == c->vf))
#define C_AL (1)

// ---------------------------------------------------------------- integer helpers
RT_INLINE u32 ror32(u32 x, unsigned s) { s &= 31; return s ? (x >> s) | (x << (32 - s)) : x; }
RT_INLINE u64 ror64(u64 x, unsigned s) { s &= 63; return s ? (x >> s) | (x << (64 - s)) : x; }
RT_INLINE u32 asr32(u32 x, unsigned s) { return (u32)((s32)x >> (s & 31)); }
RT_INLINE u64 asr64(u64 x, unsigned s) { return (u64)((s64)x >> (s & 63)); }
RT_INLINE u32 lsl32(u32 x, unsigned s) { return x << (s & 31); }
RT_INLINE u64 lsl64(u64 x, unsigned s) { return x << (s & 63); }
RT_INLINE u32 lsr32(u32 x, unsigned s) { return x >> (s & 31); }
RT_INLINE u64 lsr64(u64 x, unsigned s) { return x >> (s & 63); }
RT_INLINE u32 clz32(u32 x) { return x ? __builtin_clz(x) : 32; }
RT_INLINE u64 clz64(u64 x) { return x ? __builtin_clzll(x) : 64; }
RT_INLINE u64 rbit64(u64 x) {
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    return __builtin_bswap64(x);
}
RT_INLINE u32 rbit32(u32 x) { return (u32)(rbit64(x) >> 32); }
RT_INLINE u32 rev16_32(u32 x) { return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); }
RT_INLINE s32 sdiv32(s32 a, s32 b) { return b == 0 ? 0 : (a == INT32_MIN && b == -1) ? INT32_MIN : a / b; }
RT_INLINE s64 sdiv64(s64 a, s64 b) { return b == 0 ? 0 : (a == INT64_MIN && b == -1) ? INT64_MIN : a / b; }
RT_INLINE u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
RT_INLINE u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
RT_INLINE u64 smulh(s64 a, s64 b) { return (u64)(((s128)a * b) >> 64); }
RT_INLINE u64 umulh(u64 a, u64 b) { return (u64)(((u128)a * b) >> 64); }
// bitfield move (UBFM/SBFM/BFM semantics expressed via aliases in the emitter)
RT_INLINE u64 ubfx64(u64 x, unsigned lsb, unsigned w) { return w >= 64 ? x >> lsb : (x >> lsb) & ((1ull << w) - 1); }
RT_INLINE u64 sbfx64(u64 x, unsigned lsb, unsigned w) { return (u64)(((s64)(x << (64 - lsb - w))) >> (64 - w)); }
RT_INLINE u32 ubfx32(u32 x, unsigned lsb, unsigned w) { return w >= 32 ? x >> lsb : (x >> lsb) & ((1u << w) - 1); }
RT_INLINE u32 sbfx32(u32 x, unsigned lsb, unsigned w) { return (u32)(((s32)(x << (32 - lsb - w))) >> (32 - w)); }
RT_INLINE u64 bfi64(u64 d, u64 s, unsigned lsb, unsigned w) { u64 m = (w >= 64 ? ~0ull : ((1ull << w) - 1)) << lsb; return (d & ~m) | ((s << lsb) & m); }
RT_INLINE u32 bfi32(u32 d, u32 s, unsigned lsb, unsigned w) { u32 m = (w >= 32 ? ~0u : ((1u << w) - 1)) << lsb; return (d & ~m) | ((s << lsb) & m); }
RT_INLINE u64 extr64(u64 hi, u64 lo, unsigned s) { return s ? (lo >> s) | (hi << (64 - s)) : lo; }
RT_INLINE u32 extr32(u32 hi, u32 lo, unsigned s) { return s ? (lo >> s) | (hi << (32 - s)) : lo; }

// CRC32 (ISO-HDLC poly 0x04C11DB7 reflected 0xEDB88320) and CRC32C (0x82F63B78)
template <int BITS> RT_INLINE u32 crc_bits_(u32 crc, u64 val, u32 poly) {
    crc ^= (u32)val; u64 v = val >> 32;
    LANES(0, BITS, crc = (crc >> 1) ^ (poly & (0u - (crc & 1))); if (i == 31) crc ^= (u32)v;);
    return crc;
}
RT_INLINE u32 crc_bits(u32 crc, u64 val, int bits, u32 poly) {
    switch (bits) { case 8: return crc_bits_<8>(crc, val, poly); case 16: return crc_bits_<16>(crc, val, poly);
                    case 32: return crc_bits_<32>(crc, val, poly); default: return crc_bits_<64>(crc, val, poly); }
}
RT_INLINE u32 crc32b(u32 a, u32 v) { return crc_bits(a, v & 0xFF, 8, 0xEDB88320u); }
RT_INLINE u32 crc32h(u32 a, u32 v) { return crc_bits(a, v & 0xFFFF, 16, 0xEDB88320u); }
RT_INLINE u32 crc32w(u32 a, u32 v) { return crc_bits(a, v, 32, 0xEDB88320u); }
RT_INLINE u32 crc32x(u32 a, u64 v) { return crc_bits(a, v, 64, 0xEDB88320u); }
RT_INLINE u32 crc32cb(u32 a, u32 v) { return crc_bits(a, v & 0xFF, 8, 0x82F63B78u); }
RT_INLINE u32 crc32ch(u32 a, u32 v) { return crc_bits(a, v & 0xFFFF, 16, 0x82F63B78u); }
RT_INLINE u32 crc32cw(u32 a, u32 v) { return crc_bits(a, v, 32, 0x82F63B78u); }
RT_INLINE u32 crc32cx(u32 a, u64 v) { return crc_bits(a, v, 64, 0x82F63B78u); }

// ---------------------------------------------------------------- exclusives / barriers
template <class C> RT_INLINE u64 ldx64(C* c, u64 a) { u64 v = __atomic_load_n((u64*)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
template <class C> RT_INLINE u32 ldx32(C* c, u64 a) { u32 v = __atomic_load_n((u32*)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
template <class C> RT_INLINE u8  ldx8 (C* c, u64 a) { u8 v = __atomic_load_n((u8*)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
template <class C> RT_INLINE u32 stx64(C* c, u64 a, u64 v) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull; u64 e = c->excl_val;
    return __atomic_compare_exchange_n((u64*)a, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
template <class C> RT_INLINE u32 stx32(C* c, u64 a, u32 v) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull; u32 e = (u32)c->excl_val;
    return __atomic_compare_exchange_n((u32*)a, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
template <class C> RT_INLINE u32 stx8(C* c, u64 a, u8 v) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull; u8 e = (u8)c->excl_val;
    return __atomic_compare_exchange_n((u8*)a, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
template <class C> RT_INLINE void clrex(C* c) { c->excl_addr = ~0ull; }
RT_INLINE u64 lda64(u64 a) { return __atomic_load_n((u64*)a, __ATOMIC_SEQ_CST); }
RT_INLINE u32 lda32(u64 a) { return __atomic_load_n((u32*)a, __ATOMIC_SEQ_CST); }
RT_INLINE u8  lda8 (u64 a) { return __atomic_load_n((u8*)a, __ATOMIC_SEQ_CST); }
RT_INLINE void stl64(u64 a, u64 v) { __atomic_store_n((u64*)a, v, __ATOMIC_SEQ_CST); }
RT_INLINE void stl32(u64 a, u32 v) { __atomic_store_n((u32*)a, v, __ATOMIC_SEQ_CST); }
RT_INLINE void stl8 (u64 a, u8 v)  { __atomic_store_n((u8*)a, v, __ATOMIC_SEQ_CST); }
RT_INLINE void dmb_full() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
RT_INLINE void dmb_light() { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

// ---------------------------------------------------------------- scalar floating point All helpers below are
// BRANCH-FREE (integer masks + selects): unrolled NEON code calls them per lane, and branchy NaN checks turned
// one 18k-insn function into ~100k basic blocks (clang > 300 s). Same ARM semantics.
template <class F> RT_INLINE bool isnanT(F a) { return a != a; }
template <class F> using UB = std::conditional_t<sizeof(F) == 4, u32, u64>;
template <class F> RT_INLINE UB<F> fb(F a) { UB<F> u; memcpy(&u, &a, sizeof u); return u; }
template <class F> RT_INLINE F bf(UB<F> u) { F a; memcpy(&a, &u, sizeof a); return a; }
template <class F> constexpr UB<F> kQB = sizeof(F) == 4 ? (UB<F>)0x00400000u : (UB<F>)0x0008000000000000ull;   // quiet bit
template <class F> constexpr UB<F> kDN = sizeof(F) == 4 ? (UB<F>)0x7FC00000u : (UB<F>)0x7FF8000000000000ull;   // ARM default NaN
template <class F> RT_INLINE UB<F> mk(bool b) { return (UB<F>)0 - (UB<F>)b; }
template <class U> RT_INLINE U sel(U m, U x, U y) { return (x & m) | (y & ~m); }
RT_INLINE float  qnan_f(float a) { return bitsf(fbits(a) | 0x00400000u); }
RT_INLINE double qnan_d(double a) { return bitsd(dbits(a) | 0x0008000000000000ull); }
RT_INLINE float  qn(float a) { return qnan_f(a); }
RT_INLINE double qn(double a) { return qnan_d(a); }
template <class F> RT_INLINE bool issnan(F a) { return isnanT(a) & !(fb(a) & kQB<F>); }
template <class F> RT_INLINE UB<F> mnan(F a) { return mk<F>(isnanT(a)); }
template <class F> RT_INLINE UB<F> msnan(F a) { return mk<F>(isnanT(a) & !(fb(a) & kQB<F>)); }
// ARM FPProcessNaNs (2 operands), bits of the quietened result; only meaningful when a or b is NaN
template <class F> RT_INLINE UB<F> nanprop2(F a, F b) {
    UB<F> t = fb(b);
    t = sel(mnan(a), fb(a), t);       // first QNaN ...
    t = sel(msnan(b), fb(b), t);      // ... but SNaNs win, a before b
    t = sel(msnan(a), fb(a), t);
    return t | kQB<F>;
}
template <class F> RT_INLINE F nanprop(F a, F b) { return bf<F>(nanprop2(a, b)); }
// FPCR.FZ (TOTK_FPCR=1 maps it to MXCSR DAZ|FTZ): the native ops then flush as ARM does, but a form that returns an
// operand's own bits (the fmax / fmin family: compare + select) does not, while ARM flushes the operands first (FPUnpack:
// 67 of its 72 FZ failures). fzq(x) is x * 1 with the 1 hidden from clang: a denormal comes out as a zero of its sign
// under DAZ and as itself without DAZ (FZ off), every other number unchanged. A signalling NaN comes out quiet, so NaN
// operands keep their own path on the original values. Only with gen.py --fz (RT_FZ 1, defined at the top of each
// generated TU): fzo / opq are the identity without it, so the default key's code is the one before the fix. The cold
// templates below that reach them take Z = RT_FZ, so a TU with and one without never share a COMDAT copy. rt_core.cpp's
// estimates call fzq itself (runtime, not part of a key).
#ifndef RT_FZ
#define RT_FZ 0
#endif
template <class F> RT_INLINE F fzq(F a) { F one = 1; __asm__("" : "+x"(one)); return a * one; }
template <class F> RT_INLINE F fzo(F a) { if constexpr (RT_FZ) return fzq(a); else return a; }
// fcmp's operands as values clang cannot tie to a later select of the same registers: fcmp + fcsel (or fcmp + b.cond over
// a move) became minss / maxss, which flush a denormal operand under DAZ where ARM's fcsel keeps its bits (the other 5).
template <class F> RT_INLINE F opq(F a) { if constexpr (RT_FZ) __asm__("" : "+x"(a)); return a; }
// The sign bit. (std::signbit is a call to the UCRT's _fdsign / _dsign on this target: 102 calls in f_16f0bc's TU,
// one per fmax / fmin, each with an unwind frame and xmm6+ saved around it.)
template <class F> RT_INLINE bool sgn(F a) { return fb(a) >> (8 * sizeof(F) - 1); }
template <class F> RT_INLINE F fmaxA(F a0, F b0) {
    F a = fzo(a0), b = fzo(b0);
    UB<F> r = sel(mk<F>(a > b), fb(a), fb(b));
    r = sel(mk<F>((a == 0) & (b == 0)), sgn(a) ? fb(b) : fb(a), r);
    return bf<F>(sel(mnan(a0) | mnan(b0), nanprop2(a0, b0), r));
}
template <class F> RT_INLINE F fminA(F a0, F b0) {
    F a = fzo(a0), b = fzo(b0);
    UB<F> r = sel(mk<F>(a < b), fb(a), fb(b));
    r = sel(mk<F>((a == 0) & (b == 0)), sgn(a) ? fb(a) : fb(b), r);
    return bf<F>(sel(mnan(a0) | mnan(b0), nanprop2(a0, b0), r));
}
template <class F> RT_INLINE F fmaxnmA(F a, F b) {   // only quiet NaNs give way to a number (flushed, as in fmaxA)
    UB<F> r = fb(fmaxA(a, b));
    r = sel((mnan(b) & ~msnan(b)) & ~mnan(a), fb(fzo(a)), r);
    r = sel((mnan(a) & ~msnan(a)) & ~mnan(b), fb(fzo(b)), r);
    return bf<F>(r);
}
template <class F> RT_INLINE F fminnmA(F a, F b) {
    UB<F> r = fb(fminA(a, b));
    r = sel((mnan(b) & ~msnan(b)) & ~mnan(a), fb(fzo(a)), r);
    r = sel((mnan(a) & ~msnan(a)) & ~mnan(b), fb(fzo(b)), r);
    return bf<F>(r);
}
template <class F> RT_INLINE F fabdA(F a, F b) { return std::fabs(a - b); }
template <class C, class F> RT_INLINE void fcmpA(C* c, F a0, F b0) {
    // gt 0010, lt 1000, eq 0110, unordered 0011 = n: lt, z: eq, c: !lt, v: unordered. Plain compares per flag so clang can
    // fold fcmp + b.cond into one ucomis + jcc (the old packed 2 + 6*lt + 4*eq form with masks was not folded; 757 fcmpA in
    // the 8 hottest FP functions, Deck d48, 2026-10-17). Operands through opq (see there).
    F a = opq(a0), b = opq(b0);
    bool lt = a < b;
    c->n = lt; c->z = a == b; c->c = !lt; c->vf = isnanT(a) | isnanT(b);
}
// float -> int, round toward zero, saturating, NaN -> 0
template <class I, class F> RT_INLINE I fcvtzT(F a) {
    using U = std::make_unsigned_t<I>;
    bool hi = a >= (F)std::numeric_limits<I>::max(), lo = a <= (F)std::numeric_limits<I>::min(), nn = isnanT(a);
    F safe = bf<F>(sel(mk<F>(hi | lo | nn), (UB<F>)0, fb(a)));
    U r = (U)(I)safe;
    r = sel((U)(0 - (U)hi), (U)std::numeric_limits<I>::max(), r);
    r = sel((U)(0 - (U)lo), (U)std::numeric_limits<I>::min(), r);
    return (I)r;
}
// 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away, 5 FPCR's mode (frinti / frintx: MXCSR.RC, which follows
// FPCR.RMode with TOTK_FPCR=1). 0 is roundeven, not nearbyint: nearbyint follows MXCSR.RC, fcvtns / frintn do not.
template <class F> RT_INLINE F rnd(F a, int mode) {
    return mode == 0 ? __builtin_roundeven(a) : mode == 1 ? std::ceil(a) : mode == 2 ? std::floor(a) : mode == 3 ? std::trunc(a)
         : mode == 4 ? std::round(a) : std::nearbyint(a);
}
template <class I, class F> RT_INLINE I fcvt_rm(F a, int mode) { return fcvtzT<I, F>(rnd(a, mode)); }   // NaN -> 0 via fcvtzT
template <class F> RT_INLINE F frintA(F a, int mode) { return bf<F>(sel(mnan(a), fb(a) | kQB<F>, fb(rnd(a, mode)))); }
// arithmetic result fix-up: x86 makes a negative default NaN and does not prioritise SNaNs; ARM does.
template <class F> RT_INLINE F fixnan(F r, F a, F b) {
    UB<F> n = sel(mnan(a) | mnan(b), nanprop2(a, b), kDN<F>);
    return bf<F>(sel(mnan(r), n, fb(r)));
}
template <class F> RT_INLINE F fixnan1(F r, F a) {
    UB<F> n = sel(mnan(a), fb(a) | kQB<F>, kDN<F>);
    return bf<F>(sel(mnan(r), n, fb(r)));
}
// (the "A" suffix: glibc's <math.h> declares the C23 narrowing float fadd/fsub/fmul/fdiv(double, double); a non-template
// wins the overload tie, so on Linux every DOUBLE add/sub/mul/div called libm and came back rounded to float, without the
// NaN fix-up (Deck perf d45: f32mulf32x / f32addf32x in the worker threads, 2026-10-17). Windows' UCRT has no such names.
// The macros keep code generated before the rename compiling; gen.py emits the A names.)
template <class F> RT_INLINE F faddA(F a, F b) { return fixnan(a + b, a, b); }
template <class F> RT_INLINE F fsubA(F a, F b) { return fixnan(a - b, a, b); }
template <class F> RT_INLINE F fmulA(F a, F b) { return fixnan(a * b, a, b); }
template <class F> RT_INLINE F fdivA(F a, F b) { return fixnan(a / b, a, b); }
#define fadd faddA
#define fsub fsubA
#define fmul fmulA
#define fdiv fdivA
template <class F> RT_INLINE F fnmul(F a, F b) { return -fmulA(a, b); }
template <class F> RT_INLINE F fsqrtA(F a) { return fixnan1(std::sqrt(a), a); }
template <class F> RT_INLINE F fmlaA(F acc, F a, F b) {   // FPProcessNaNs3: SNaN acc,a,b then QNaN acc,a,b
    F r = std::fma(a, b, acc);
    UB<F> t = fb(b);
    t = sel(mnan(a), fb(a), t);
    t = sel(mnan(acc), fb(acc), t);
    t = sel(msnan(b), fb(b), t);
    t = sel(msnan(a), fb(a), t);
    t = sel(msnan(acc), fb(acc), t);
    UB<F> n = sel(mnan(acc) | mnan(a) | mnan(b), t | kQB<F>, kDN<F>);
    // FPMulAdd: a QUIET NaN addend with an inf * 0 product gives the default NaN (invalid op), not the addend
    bool i0 = (std::isinf(a) & (b == 0)) | (std::isinf(b) & (a == 0));
    n = sel(mk<F>(isnanT(acc) & !issnan(acc) & i0), kDN<F>, n);
    return bf<F>(sel(mnan(r), n, fb(r)));
}
// Fast forms (gen.py: scalar ops here; vector ops test all lanes at once, gen.py lanes_fp): the native result, and the
// exact ARM NaN handling (the A forms) out of line only when the result is NaN. A NaN operand always gives a NaN result
// for add/sub/mul/div/fma, so testing the result covers the operands too: same values as the A forms. The A forms are ~19
// x86 insns per scalar op (branch-free masks; x4 per vector op) = over half of the host code of FP-heavy hot functions
// (Deck d47, 2026-10-17). One test per op (not per lane) keeps the basic-block count sane (see the comment above).
template <class F> __attribute__((noinline, cold)) F fixnan_cold(F r, F a, F b) { return fixnan(r, a, b); }
template <class F> __attribute__((noinline, cold)) F fmla_cold(F acc, F a, F b) { return fmlaA(acc, a, b); }
template <class F> RT_INLINE F faddS(F a, F b) { F r = a + b; if (__builtin_expect(r != r, 0)) r = fixnan_cold(r, a, b); return r; }
template <class F> RT_INLINE F fsubS(F a, F b) { F r = a - b; if (__builtin_expect(r != r, 0)) r = fixnan_cold(r, a, b); return r; }
template <class F> RT_INLINE F fmulS(F a, F b) { F r = a * b; if (__builtin_expect(r != r, 0)) r = fixnan_cold(r, a, b); return r; }
template <class F> RT_INLINE F fdivS(F a, F b) { F r = a / b; if (__builtin_expect(r != r, 0)) r = fixnan_cold(r, a, b); return r; }
template <class F> RT_INLINE F fnmulS(F a, F b) { return -fmulS(a, b); }
template <class F> RT_INLINE F fmlaS(F acc, F a, F b) {
    F r = std::fma(a, b, acc);
    if (__builtin_expect(r != r, 0)) r = fmla_cold(acc, a, b);
    return r;
}
// Vector add/sub/mul/div/fmla/fmls (gen.py _fbin vop, 2026-10-17): ONE SIMD op + ONE NaN test over the live lanes; the exact
// per-lane A forms only out of line (vfop_cold). The earlier lanes_fp form (per-lane scalar ops and tests, exact path inlined
// at every site) made f_170744 21 host insns per guest insn. Same values: a lane is NaN in the native result iff the A form
// would do NaN handling there. OP: 0 add, 1 sub, 2 mul, 3 div, 4 fmla (acc + a*b, fused), 5 fmls (acc - a*b, fused), 6 fmax, 7 fmin, 8 fmaxnm, 9 fminnm.
// Lanes >= CNT come out zero (64-bit arrangements), as before.
typedef float vf32x4 __attribute__((vector_size(16)));
typedef double vf64x2 __attribute__((vector_size(16)));
typedef s32 vm32x4 __attribute__((vector_size(16)));
typedef s64 vm64x2 __attribute__((vector_size(16)));
template <class F> RT_INLINE F* vlanes(V128& v) { if constexpr (sizeof(F) == 4) return v.f; else return v.fd; }
template <class F> RT_INLINE V128 vbcast(F s) {   // (vector literal -> vbroadcastss / vmovddup)
    V128 r;
    if constexpr (sizeof(F) == 4) { vf32x4 v = {s, s, s, s}; memcpy(&r, &v, 16); }
    else { vf64x2 v = {s, s}; memcpy(&r, &v, 16); }
    return r;
}
template <int OP, int CNT, class F, bool Z = RT_FZ> __attribute__((noinline, cold)) V128 vfop_cold(V128 a, V128 b, V128 acc) {
    V128 r = vzero();
    F *x = vlanes<F>(a), *y = vlanes<F>(b), *z = vlanes<F>(acc), *o = vlanes<F>(r);
    for (int i = 0; i < CNT; i++)
        o[i] = OP == 0 ? faddA(x[i], y[i]) : OP == 1 ? fsubA(x[i], y[i]) : OP == 2 ? fmulA(x[i], y[i]) : OP == 3 ? fdivA(x[i], y[i])
             : OP == 4 ? fmlaA(z[i], x[i], y[i]) : OP == 5 ? fmlaA(z[i], -x[i], y[i])
             : OP == 6 ? fmaxA(x[i], y[i]) : OP == 7 ? fminA(x[i], y[i]) : OP == 8 ? fmaxnmA(x[i], y[i]) : fminnmA(x[i], y[i]);
    return r;
}
template <int OP, int CNT, class F> RT_INLINE V128 vfop(const V128& a, const V128& b, const V128& acc) {
    static_assert(CNT * sizeof(F) == 8 || CNT * sizeof(F) == 16, "vfop arrangement");
    using V = std::conditional_t<sizeof(F) == 4, vf32x4, vf64x2>;
    using M = std::conditional_t<sizeof(F) == 4, vm32x4, vm64x2>;
    V x, y, z, r;
    memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    if constexpr (OP == 0) r = x + y;
    else if constexpr (OP == 1) r = x - y;
    else if constexpr (OP == 2) r = x * y;
    else if constexpr (OP == 3) r = x / y;
    else if constexpr (OP <= 5) { memcpy(&z, &acc, 16); r = __builtin_elementwise_fma(OP == 4 ? x : -x, y, z); }
    else {
        // 6 fmax, 7 fmin, 8 fmaxnm, 9 fminnm: the larger (smaller) lane; equal lanes give a & b (a | b) bitwise,
        // which is ARM's +0 / -0 rule and the value itself otherwise. Without NaNs the NM forms are the same op; any NaN
        // operand takes the exact lanes (tested on the operands: the select itself does not produce NaN).
        // Operands flushed first under FZ, as fzo does (finding 7; --fz only).
        constexpr bool mx = OP == 6 || OP == 8;
        if constexpr (RT_FZ) {
            V one = {};
            one += (F)1;
            __asm__("" : "+x"(one));
            x *= one; y *= one;
        }
        M gt = mx ? (x > y) : (x < y), eq = x == y, xi, yi;
        memcpy(&xi, &x, 16); memcpy(&yi, &y, 16);
        M ri = (gt & xi) | (~gt & ((eq & (mx ? (xi & yi) : (xi | yi))) | (~eq & yi)));
        memcpy(&r, &ri, 16);
        M m = (x != x) | (y != y);
        bool nan;
        if constexpr (CNT * sizeof(F) == 16) nan = __builtin_reduce_or(m) != 0;
        else nan = (m[0] | m[1]) != 0;
        V128 o; memcpy(&o, &r, 16);
        if constexpr (CNT * sizeof(F) == 8) o.d[1] = 0;
        if (__builtin_expect(nan, 0)) o = vfop_cold<OP, CNT, F>(a, b, acc);
        return o;
    }
    M m = r != r;
    bool nan;
    if constexpr (CNT * sizeof(F) == 16) nan = __builtin_reduce_or(m) != 0;
    else nan = (m[0] | m[1]) != 0;                                  // 2S: lanes 0..1 only
    V128 o; memcpy(&o, &r, 16);
    if constexpr (CNT * sizeof(F) == 8) o.d[1] = 0;
    if (__builtin_expect(nan, 0)) o = vfop_cold<OP, CNT, F>(a, b, acc);
    return o;
}
// faddp (vector): the pairs of a, then of b, summed with one SIMD add and one NaN test, the exact lanes (faddA) out of
// line, as vfop. CNT = lanes of the result: 2s (2), 4s (4), 2d (2).
template <int CNT, class F> __attribute__((noinline, cold)) V128 vfaddp_cold(V128 a, V128 b) {
    V128 r = vzero(); F *x = vlanes<F>(a), *y = vlanes<F>(b), *o = vlanes<F>(r);
    for (int i = 0; i < CNT; i++) o[i] = i < CNT / 2 ? faddA(x[2 * i], x[2 * i + 1]) : faddA(y[2 * (i - CNT / 2)], y[2 * (i - CNT / 2) + 1]);
    return r;
}
template <int CNT, class F> RT_INLINE V128 vfaddp(const V128& a, const V128& b) {
    using V = std::conditional_t<sizeof(F) == 4, vf32x4, vf64x2>;
    using M = std::conditional_t<sizeof(F) == 4, vm32x4, vm64x2>;
    V x, y, p, q, r;
    memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    if constexpr (sizeof(F) == 8) { p = __builtin_shufflevector(x, y, 0, 2); q = __builtin_shufflevector(x, y, 1, 3); }
    else if constexpr (CNT == 4) { p = __builtin_shufflevector(x, y, 0, 2, 4, 6); q = __builtin_shufflevector(x, y, 1, 3, 5, 7); }
    else { p = __builtin_shufflevector(x, y, 0, 4, 0, 4); q = __builtin_shufflevector(x, y, 1, 5, 1, 5); }
    r = p + q;
    M m = r != r;
    bool nan;
    if constexpr (CNT * sizeof(F) == 16) nan = __builtin_reduce_or(m) != 0;
    else nan = (m[0] | m[1]) != 0;
    V128 o; memcpy(&o, &r, 16);
    if constexpr (CNT * sizeof(F) == 8) o.d[1] = 0;
    if (__builtin_expect(nan, 0)) o = vfaddp_cold<CNT, F>(a, b);
    return o;
}
float frecpe_f32(float a);
float frsqrte_f32(float a);
RT_INLINE bool inf_times_zero(float a, float b) { return (std::isinf(a) & (b == 0)) | (std::isinf(b) & (a == 0)); }
RT_INLINE float frecps_f32(float a, float b) {   // 2 - a*b, with inf*0 = 2; ARM negates op1 before NaN processing
    float r = fixnan(std::fma(-a, b, 2.0f), -a, b);
    return bf<float>(sel(mk<float>(inf_times_zero(a, b)), fbits(2.0f), fbits(r)));
}
RT_INLINE float frsqrts_f32(float a, float b) {  // (3 - a*b)/2, with inf*0 = 1.5; ARM halves before rounding
    float r = fixnan((float)(std::fma(-(double)a, (double)b, 3.0) * 0.5), -a, b);
    return bf<float>(sel(mk<float>(inf_times_zero(a, b)), fbits(1.5f), fbits(r)));
}
RT_INLINE float  f16tof32(u16 h) { _Float16 x; memcpy(&x, &h, 2); return (float)x; }
RT_INLINE u16    f32tof16(float f) { _Float16 x = (_Float16)f; u16 h; memcpy(&h, &x, 2); return h; }

// ---------------------------------------------------------------- run forms (gen.py --nanrun)
// The native op of vfop / faddS & co, with NaN-ness ORed into the run's flag and no branch: gen.py's nanrun_pass reruns
// the whole run with the exact forms when the flag is set at its end.
template <int OP, int CNT, class F> RT_INLINE V128 vfopN(const V128& a, const V128& b, const V128& acc, bool& nr) {
    static_assert(OP <= 5 && (CNT * sizeof(F) == 8 || CNT * sizeof(F) == 16), "vfopN");
    using V = std::conditional_t<sizeof(F) == 4, vf32x4, vf64x2>;
    using M = std::conditional_t<sizeof(F) == 4, vm32x4, vm64x2>;
    V x, y, z, r;
    memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    if constexpr (OP == 0) r = x + y;
    else if constexpr (OP == 1) r = x - y;
    else if constexpr (OP == 2) r = x * y;
    else if constexpr (OP == 3) r = x / y;
    else { memcpy(&z, &acc, 16); r = __builtin_elementwise_fma(OP == 4 ? x : -x, y, z); }
    M m = r != r;
    if constexpr (CNT * sizeof(F) == 16) nr |= __builtin_reduce_or(m) != 0;
    else nr |= (m[0] | m[1]) != 0;
    V128 o; memcpy(&o, &r, 16);
    if constexpr (CNT * sizeof(F) == 8) o.d[1] = 0;
    return o;
}
template <class F> RT_INLINE F faddN(F a, F b, bool& nr) { F r = a + b; nr |= r != r; return r; }
template <class F> RT_INLINE F fsubN(F a, F b, bool& nr) { F r = a - b; nr |= r != r; return r; }
template <class F> RT_INLINE F fmulN(F a, F b, bool& nr) { F r = a * b; nr |= r != r; return r; }
template <class F> RT_INLINE F fdivN(F a, F b, bool& nr) { F r = a / b; nr |= r != r; return r; }
template <class F> RT_INLINE F fnmulN(F a, F b, bool& nr) { return -fmulN(a, b, nr); }
template <class F> RT_INLINE F fmlaN(F acc, F a, F b, bool& nr) { F r = std::fma(a, b, acc); nr |= r != r; return r; }

// ---------------------------------------------------------------- SIMD fast forms (gen.py --simd)
// Each gives the same bits as the exact form it replaces with the default MXCSR; the exact form runs out of line when
// a NaN (or another case the fast form leaves out) appears.
// tbl: one pshufb per table register. Index bytes >= 16 * NTAB give 0, as on ARM: idx + 0x70 (unsigned saturating) keeps
// the low nibble and sets bit 7 exactly when idx >= 16, and pshufb writes 0 where bit 7 is set.
template <int NTAB, int TOTAL> RT_INLINE V128 vtbl_fast(const V128* tab, const V128& idx) {
    __m128i ix, acc = _mm_setzero_si128(), k70 = _mm_set1_epi8(0x70), k16 = _mm_set1_epi8(16);
    memcpy(&ix, &idx, 16);
    for (int t = 0; t < NTAB; t++) {
        __m128i tb; memcpy(&tb, &tab[t], 16);
        acc = _mm_or_si128(acc, _mm_shuffle_epi8(tb, _mm_adds_epu8(ix, k70)));
        ix = _mm_sub_epi8(ix, k16);    // indices below 16 wrap to >= 0xF0: saturate, bit 7 set, 0
    }
    V128 r; memcpy(&r, &acc, 16);
    if constexpr (TOTAL == 8) r.d[1] = 0;
    return r;
}
// fcvtzs / fcvtzu, 32-bit lanes: truncate, saturate, NaN -> 0 (fcvtzT per lane). cvttps2dq gives 0x80000000 for NaN and
// out-of-range lanes; those are patched with compares on the input.
// --vdirect adds MODE (0 fcvtn*, 1 fcvtp*, 2 fcvtm*: roundps to an integer first, which the truncation keeps; NaN stays
// NaN, an out-of-range value stays out of range) and SCALE, #fbits' 2^fbits for fcvtz*, multiplied in first as fbits_mul.
template <bool S, int CNT, int MODE = 3> RT_INLINE V128 vfcvtz(const V128& a, float scale = 1.0f) {
    static_assert(MODE >= 0 && MODE <= 3, "vfcvtz mode");
    __m128 x; memcpy(&x, &a, 16);
    if (scale != 1.0f) x = _mm_mul_ps(x, _mm_set1_ps(scale));
    if constexpr (MODE == 0) x = _mm_round_ps(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    else if constexpr (MODE == 1) x = _mm_round_ps(x, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    else if constexpr (MODE == 2) x = _mm_round_ps(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    __m128i r;
    if constexpr (S) {
        __m128i t = _mm_cvttps_epi32(x);
        __m128i hi = _mm_castps_si128(_mm_cmp_ps(x, _mm_set1_ps(2147483648.0f), _CMP_GE_OQ));
        __m128i nan = _mm_castps_si128(_mm_cmp_ps(x, x, _CMP_UNORD_Q));
        r = _mm_andnot_si128(nan, _mm_blendv_epi8(t, _mm_set1_epi32(0x7FFFFFFF), hi));
    } else {
        __m128 z = _mm_max_ps(x, _mm_setzero_ps());   // NaN and negatives -> 0 (maxps returns the 2nd operand for NaN)
        __m128 big = _mm_set1_ps(2147483648.0f);
        __m128i lo = _mm_cvttps_epi32(z);
        __m128i hi = _mm_xor_si128(_mm_cvttps_epi32(_mm_sub_ps(z, big)), _mm_set1_epi32((int)0x80000000));
        __m128i ge31 = _mm_castps_si128(_mm_cmp_ps(z, big, _CMP_GE_OQ));
        __m128i ge32 = _mm_castps_si128(_mm_cmp_ps(z, _mm_set1_ps(4294967296.0f), _CMP_GE_OQ));
        r = _mm_or_si128(_mm_blendv_epi8(lo, hi, ge31), ge32);
    }
    V128 o; memcpy(&o, &r, 16);
    if constexpr (CNT == 2) o.d[1] = 0;
    return o;
}
// --vdirect: scvtf / ucvtf (vector) as one __builtin_convertvector of the register (cvtdq2ps; u32: clang's packed
// two-half sequence, exact halves and one rounding; 64-bit lanes as clang lowers the lane casts), then * 2^-fbits, exact,
// as fbits_div. The u32 sequence sums -2^23 + 2^23 for a zero lane, -0 under round-down: such lanes are forced to +0.
template <bool S, class F, int CNT> RT_INLINE V128 vcvtf(const V128& a, F scale = 1) {
    static_assert(CNT * sizeof(F) == 8 || CNT * sizeof(F) == 16, "vcvtf arrangement");
    using V = std::conditional_t<sizeof(F) == 4, vf32x4, vf64x2>;
    using M = std::conditional_t<sizeof(F) == 4, vm32x4, vm64x2>;
    using I = std::conditional_t<sizeof(F) == 4, std::conditional_t<S, s32, u32>, std::conditional_t<S, s64, u64>>;
    typedef I VI __attribute__((vector_size(16)));
    VI x; memcpy(&x, &a, 16);
    V r = __builtin_convertvector(x, V);
    if (scale != 1) r *= scale;
    if constexpr (!S) {
        M nz = (M)(x != 0), ri;
        memcpy(&ri, &r, 16); ri &= nz; memcpy(&r, &ri, 16);
    }
    V128 o; memcpy(&o, &r, 16);
    if constexpr (CNT * sizeof(F) == 8) o.d[1] = 0;
    return o;
}
// fmax / fmin / fmaxnm / fminnm, scalar: the larger (smaller); equal values give a & b (a | b) bitwise (ARM's +0 / -0);
// a NaN operand takes the exact form (out of line). Without NaNs the NM forms are the same op.
template <bool MX, class F> RT_INLINE F fminmax_sel(F a0, F b0) {   // (no NaN here; operands flushed under FZ: fzo)
    F a = fzo(a0), b = fzo(b0);
    UB<F> ai = fb(a), bi = fb(b);
    return bf<F>((MX ? a > b : a < b) ? ai : a == b ? (MX ? ai & bi : ai | bi) : bi);
}
template <class F, bool Z = RT_FZ> __attribute__((noinline, cold)) F fmaxA_cold(F a, F b) { return fmaxA(a, b); }
template <class F, bool Z = RT_FZ> __attribute__((noinline, cold)) F fminA_cold(F a, F b) { return fminA(a, b); }
template <class F, bool Z = RT_FZ> __attribute__((noinline, cold)) F fmaxnmA_cold(F a, F b) { return fmaxnmA(a, b); }
template <class F, bool Z = RT_FZ> __attribute__((noinline, cold)) F fminnmA_cold(F a, F b) { return fminnmA(a, b); }
template <class F> RT_INLINE F fmaxS(F a, F b) { return __builtin_expect((a != a) | (b != b), 0) ? fmaxA_cold(a, b) : fminmax_sel<true>(a, b); }
template <class F> RT_INLINE F fminS(F a, F b) { return __builtin_expect((a != a) | (b != b), 0) ? fminA_cold(a, b) : fminmax_sel<false>(a, b); }
template <class F> RT_INLINE F fmaxnmS(F a, F b) { return __builtin_expect((a != a) | (b != b), 0) ? fmaxnmA_cold(a, b) : fminmax_sel<true>(a, b); }
template <class F> RT_INLINE F fminnmS(F a, F b) { return __builtin_expect((a != a) | (b != b), 0) ? fminnmA_cold(a, b) : fminmax_sel<false>(a, b); }
// fmaxnmv / fminnmv (4s): ARM's pairwise tree; without a NaN lane it is three selects.
template <bool MX, bool Z = RT_FZ> __attribute__((noinline, cold)) float fnmv_cold(V128 v) {
    return MX ? fmaxnmA(fmaxnmA(v.f[0], v.f[1]), fmaxnmA(v.f[2], v.f[3])) : fminnmA(fminnmA(v.f[0], v.f[1]), fminnmA(v.f[2], v.f[3]));
}
template <bool MX> RT_INLINE float fnmvS(const V128& v) {
    float a = v.f[0], b = v.f[1], c = v.f[2], d = v.f[3];
    if (__builtin_expect((a != a) | (b != b) | (c != c) | (d != d), 0)) return fnmv_cold<MX>(v);
    return fminmax_sel<MX>(fminmax_sel<MX>(a, b), fminmax_sel<MX>(c, d));
}

// ---------------------------------------------------------------- vector lane macros
// VMAP(T, N, expr using a[i], b[i]) -> V128 with N lanes of T computed; upper lanes zero when N*sizeof(T)==8
#define VLANES(T) (16 / sizeof(T))
template <class T> RT_INLINE T* lanes(V128& v) { return (T*)&v; }
template <class T> RT_INLINE const T* lanes(const V128& v) { return (const T*)&v; }
#define VBIN(T, N, A, B, EXPR) ([&]() { V128 r_ = vzero(); const T* a = lanes<T>(A); const T* b = lanes<T>(B); T* r = lanes<T>(r_); \
    LANES(0, (N), r[i] = (T)(EXPR)); return r_; }())
#define VUN(T, N, A, EXPR) ([&]() { V128 r_ = vzero(); const T* a = lanes<T>(A); T* r = lanes<T>(r_); \
    LANES(0, (N), r[i] = (T)(EXPR)); return r_; }())
#define VTER(T, N, D, A, B, EXPR) ([&]() { V128 r_ = vzero(); const T* d = lanes<T>(D); const T* a = lanes<T>(A); const T* b = lanes<T>(B); T* r = lanes<T>(r_); \
    LANES(0, (N), r[i] = (T)(EXPR)); return r_; }())

RT_INLINE V128 vins(V128 d, int bytes, int idx, u64 val) { memcpy(d.b + idx * bytes, &val, bytes); return d; }
RT_INLINE u64 vext_lane(const V128& s, int bytes, int idx) { u64 v = 0; memcpy(&v, s.b + idx * bytes, bytes); return v; }
RT_INLINE V128 vdup(int bytes, int nbytes_total, u64 val) {
    u64 m = bytes >= 8 ? ~0ull : (1ull << (bytes * 8)) - 1, rep = bytes == 1 ? 0x0101010101010101ull : bytes == 2 ? 0x0001000100010001ull : bytes == 4 ? 0x0000000100000001ull : 1;
    V128 r = vzero(); r.d[0] = (val & m) * rep; if (nbytes_total == 16) r.d[1] = r.d[0]; return r;
}
RT_INLINE V128 vext(const V128& a, const V128& b, int idx, int total) {   // EXT
    u8 cat[32]; memcpy(cat, a.b, total); memcpy(cat + total, b.b, total);
    V128 r = vzero(); memcpy(r.b, cat + idx, total); return r;
}
RT_INLINE V128 vtbl(const V128* tab, int ntab, const V128& idx, int total) {
    V128 r = vzero();
    auto one = [&](int i) __attribute__((always_inline)) { unsigned j = idx.b[i]; r.b[i] = j < 16u * ntab ? tab[j / 16].b[j % 16] : 0; };
    if (total == 16) LANES(0, 16, one(i)); else LANES(0, 8, one(i));
    return r;
}
template <class T> RT_INLINE V128 vzip(const V128& a, const V128& b, int part, int total) {
    V128 r = vzero(); int n = total / sizeof(T); const T* x = lanes<T>(a); const T* y = lanes<T>(b); T* o = lanes<T>(r);
    int base = part * n / 2;
    if (total == 16) LANES(0, 8 / (int)sizeof(T), { o[2 * i] = x[base + i]; o[2 * i + 1] = y[base + i]; }); else LANES(0, 4 / (int)sizeof(T), { o[2 * i] = x[base + i]; o[2 * i + 1] = y[base + i]; });
    return r;
}
template <class T> RT_INLINE V128 vuzp(const V128& a, const V128& b, int part, int total) {
    V128 r = vzero(); int n = total / sizeof(T); const T* x = lanes<T>(a); const T* y = lanes<T>(b); T* o = lanes<T>(r);
    if (total == 16) LANES(0, 8 / (int)sizeof(T), { o[i] = x[2 * i + part]; o[n / 2 + i] = y[2 * i + part]; }); else LANES(0, 4 / (int)sizeof(T), { o[i] = x[2 * i + part]; o[n / 2 + i] = y[2 * i + part]; });
    return r;
}
template <class T> RT_INLINE V128 vtrn(const V128& a, const V128& b, int part, int total) {
    V128 r = vzero(); int n = total / sizeof(T); const T* x = lanes<T>(a); const T* y = lanes<T>(b); T* o = lanes<T>(r);
    if (total == 16) LANES(0, 8 / (int)sizeof(T), { o[2 * i] = x[2 * i + part]; o[2 * i + 1] = y[2 * i + part]; }); else LANES(0, 4 / (int)sizeof(T), { o[2 * i] = x[2 * i + part]; o[2 * i + 1] = y[2 * i + part]; });
    return r;
}
// gen.py --vdirect: zip / uzp / trn as one __builtin_shufflevector of the two registers (punpck / shufps / pshufb),
// not lane copies through memory. K: 0 zip, 1 uzp, 2 trn; P: 0 (zip1 & co) or 1 (zip2 & co); TOTAL: 8 or 16 bytes
// (8: bytes 8..15 zero).
// Output lane j of n = TOTAL / sizeof(T), as vzip / vuzp / vtrn above; index N + k is lane k of b.
constexpr int vperm_ix(int K, int P, int n, int N, int j) {
    return j >= n ? -1
         : K == 0 ? (j % 2 ? N : 0) + P * n / 2 + j / 2
         : K == 1 ? (j < n / 2 ? 2 * j + P : N + 2 * (j - n / 2) + P)
         : (j % 2 ? N + j - 1 : j) + P;
}
template <int K, int P, class T, int TOTAL, int... J> RT_INLINE V128 vperm_(const V128& a, const V128& b, std::integer_sequence<int, J...>) {
    typedef T VT __attribute__((vector_size(16)));
    constexpr int N = 16 / sizeof(T), n = TOTAL / sizeof(T);
    VT x, y; memcpy(&x, &a, 16); memcpy(&y, &b, 16);
    VT r = __builtin_shufflevector(x, y, vperm_ix(K, P, n, N, J)...);
    V128 o; memcpy(&o, &r, 16);
    if constexpr (TOTAL == 8) o.d[1] = 0;
    return o;
}
template <int K, int P, class T, int TOTAL> RT_INLINE V128 vperm(const V128& a, const V128& b) {
    static_assert(TOTAL == 8 || TOTAL == 16, "vperm arrangement");
    return vperm_<K, P, T, TOTAL>(a, b, std::make_integer_sequence<int, 16 / sizeof(T)>{});
}
// gen.py --vdirect: addv / sminv as one reduction of the register (wrapping add, as the lane loop). OP: 0 add, 1 min,
// 2 max (T's signedness picks s / u). The result is lane 0, the rest of the register zero.
template <int OP, class T, int TOTAL> RT_INLINE V128 vreduce(const V128& a) {
    typedef T VT __attribute__((vector_size(TOTAL)));
    VT x; memcpy(&x, &a, TOTAL);
    T s = OP == 0 ? __builtin_reduce_add(x) : OP == 1 ? __builtin_reduce_min(x) : __builtin_reduce_max(x);
    V128 o = vzero(); memcpy(&o, &s, sizeof(T));
    return o;
}
template <class T> RT_INLINE V128 vrev64(const V128& a, int total) {
    V128 r = vzero(); int per = 8 / sizeof(T); const T* x = lanes<T>(a); T* o = lanes<T>(r);
    if (total == 16) LANES(0, 16 / (int)sizeof(T), o[i] = x[(i / per) * per + (per - 1 - i % per)]); else LANES(0, 8 / (int)sizeof(T), o[i] = x[(i / per) * per + (per - 1 - i % per)]);
    return r;
}
template <class T> RT_INLINE T satT(s64 v) {
    if (v > (s64)std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
    if (v < (s64)std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
    return (T)v;
}

// ---------------------------------------------------------------- control flow (implemented by the runtime)
typedef void (*GuestFn)(Ctx*);
void rt_call_indirect(Ctx* c, u64 target);           // BLR / tail BR to unknown target
extern GuestFn* g_ftab;                              // indexed by (addr - FTAB_BASE) >> 2
extern u64 g_ftab_n;
#define FTAB_BASE 0x7100000000ull
RT_INLINE void call_ind(Ctx* c, u64 t) {
    u64 i = (t - FTAB_BASE) >> 2; GuestFn f;
    if (i < g_ftab_n && (f = g_ftab[i]) != nullptr) f(c); else rt_call_indirect(c, t);
}
[[noreturn]] void rt_trap(Ctx* c, u64 pc, const char* why);
void rt_mrs_unknown(Ctx* c, u64 pc);
u64  rt_read_fpcr(Ctx* c);
void rt_write_fpcr(Ctx* c, u64 v);

// ---------------------------------------------------------------- generated-function register file gen.py functions run
// on an LCtx: x0-x30, sp and NZCV are members of a local whose address never leaves the function (after inlining, SROA
// keeps them in host registers / spill slots). Everything else refers to the real Ctx. gen.py writes the registers the
// function changes back before every call / return / trap (lso, mask = bits 0-30 x0-x30, 31 sp, 32 NZCV) and reloads the
// caller-saved ones after each call returns (lsi: x0-x18, x30, NZCV; x19-x29 / sp are preserved by the callee per
// AAPCS64, see lsi): the real Ctx stays exact wherever other code can look at it.
struct LCtx {
    u64 x[32];
    u64 sp;
    u8 n, z, c, vf;
    V128 (&v)[32];
    u32 &fpcr, &fpsr;
    u64 &tpidr, &excl_addr, &excl_val, &excl_val2;
    __attribute__((always_inline)) explicit LCtx(Ctx* r)
        : v(r->v), fpcr(r->fpcr), fpsr(r->fpsr), tpidr(r->tpidr), excl_addr(r->excl_addr), excl_val(r->excl_val),
          excl_val2(r->excl_val2) {
        LANES(0, 31, x[i] = r->x[i]);
        sp = r->sp; n = r->n; z = r->z; c = r->c; vf = r->vf;
    }
};
template <u64 M> RT_INLINE void lso(Ctx* __restrict r, const LCtx& l) {
    LANES(0, 31, if constexpr ((M >> i) & 1) r->x[i] = l.x[i]);
    if constexpr ((M >> 31) & 1) r->sp = l.sp;
    if constexpr ((M >> 32) & 1) { r->n = l.n; r->z = l.z; r->c = l.c; r->vf = l.vf; }
}
// After a guest call (gen.py emits lsi only after BL / BL to an import / BLR): the callee preserved x19..x29 and sp
// (AAPCS64), so only x0..x18, x30 and the flags come back from the Ctx; the locals keep x19..x29 / sp.
// tools/abi_scan.py: no function in main (242,860 FDE ranges) or sdk writes x19..x29 without saving them (no outlined
// code); fibers trap; no HLE writes them. Up to ~12 fewer loads per call site and the values stay in host registers.
// LARECOMP_FULL_LSI-style A/B needs a rebuild: define RT_FULL_LSI for the old full reload.
RT_INLINE void lsi(Ctx* __restrict r, LCtx& l) {
#ifdef RT_FULL_LSI
    LANES(0, 31, l.x[i] = r->x[i]);
    l.sp = r->sp;
#else
    LANES(0, 19, l.x[i] = r->x[i]);
    l.x[30] = r->x[30];
#endif
    l.n = r->n; l.z = r->z; l.c = r->c; l.vf = r->vf;
}
// (padding to the previous length: in-place restore while a stale compile kept this header mapped, 2026-10-16)
//                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  
