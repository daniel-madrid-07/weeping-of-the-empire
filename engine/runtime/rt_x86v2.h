// x86-64-v2 builds (tools/engine/build.py --march x86-64-v2, the Builder's --cpu v2 for processors without AVX2: Sandy /
// Ivy Bridge, AMD FX and Phenom, Pentium / Celeron up to 2020). build.py force-includes this header (-include) into the
// game objects and the runtime (C and C++) of such a build only, so rt.h and every x86-64-v3 key stay as they are.
//
// What a v2 compile lacks, and what stands in:
//  - _mm_cmp_ps (AVX: a VEX-only compare with a predicate; clang's is a macro over __builtin_ia32_cmpps): the two
//    predicates rt.h uses map to SSE compares giving the same lane masks: _CMP_GE_OQ -> _mm_cmpge_ps (cmpleps with
//    the operands swapped: false on NaN, as the ordered predicate; it is the signalling form, which only differs in the
//    MXCSR invalid flag, masked and never read), _CMP_UNORD_Q -> _mm_cmpunord_ps. Any other predicate traps.
//  - FMA (C++ only): std::fma and __builtin_elementwise_fma stay exact (one rounding, as ARM's fmla / fmadd), and the
//    float forms run inline below instead of a call to the CRT's fmaf per lane (RIVAL-034, the rival's #333 does the
//    same): the product of two floats is exact in double, the sum is rounded to odd in double, and that double rounded
//    to float in the MXCSR mode is the fused result (round-to-odd at 53 >= 24 + 2 bits: no double rounding in any
//    mode, subnormal results included). DAZ / FTZ act as on vfmadd: the float inputs go through cvtps2pd (DAZ), the
//    result through cvtpd2ps (FTZ, the same tininess rule); no double in between can be subnormal (|a*b| >= 2^-298).
//    A double fma whose two factors are floats (frsqrts) is the product plus the addend, one rounding; any other
//    double fma still calls the CRT. No mul + add stand-in: that rounds twice. tools/engine/test_fma_v2.py checks the
//    bits against vfmadd under the four rounding modes with and without DAZ / FTZ.
//    C TUs keep the CRT's fmaf (the runtime's C has no FMA on a hot path).
//  - F16C: _Float16 conversions become compiler-rt calls (__extendhfsf2 / __truncsfhf2), the same values.
#pragma once
#include <immintrin.h>

#ifndef __AVX__
static inline __attribute__((always_inline)) __m128 rt_x86v2_cmp_ps(__m128 a, __m128 b, int p) {
    if (p == _CMP_GE_OQ) return _mm_cmpge_ps(a, b);
    if (p == _CMP_UNORD_Q) return _mm_cmpunord_ps(a, b);
    __builtin_trap();
}
#undef _mm_cmp_ps
#define _mm_cmp_ps(a, b, p) rt_x86v2_cmp_ps((a), (b), (p))
#endif

#if !defined(__FMA__) && defined(__cplusplus)
#include <cmath>   // (before the fma macro below: <cmath>'s own fma declarations stay as they are)

namespace rt_x86v2 {
typedef float f4 __attribute__((vector_size(16)));
typedef float f2 __attribute__((vector_size(8)));
typedef double d2 __attribute__((vector_size(16)));
typedef unsigned long long u2 __attribute__((vector_size(16)));

// p + c rounded to odd, with MXCSR in round-to-nearest: TwoSum gives s = p + c rounded and its exact error e; an
// inexact s with an even last bit is replaced by the other neighbour of the exact sum, one ulp away (+1 on the bits when
// e has s's sign: the sum is further from zero than s, -1 when nearer). e is NaN only when s is infinite (an infinite
// operand: exact), and that lane is left alone.
static inline __attribute__((always_inline)) d2 sum_odd(d2 p, d2 c) {
    d2 s = p + c, pp = s - c, cc = s - pp;
    d2 e = (p - pp) + (c - cc);
    u2 sb = __builtin_bit_cast(u2, s), eb = __builtin_bit_cast(u2, e);
    u2 fix = (u2)((e < 0) | (e > 0)) & ~sb & 1;
    u2 nearer = (sb ^ eb) >> 63;
    return __builtin_bit_cast(d2, sb + fix - ((fix & nearer) << 1));
}
static inline __attribute__((always_inline)) double sum_odd1(double p, double c) {
    double s = p + c, pp = s - c, cc = s - pp;
    double e = (p - pp) + (c - cc);
    unsigned long long sb = __builtin_bit_cast(unsigned long long, s), eb = __builtin_bit_cast(unsigned long long, e);
    unsigned long long fix = (unsigned long long)((e < 0) | (e > 0)) & ~sb & 1;
    return __builtin_bit_cast(double, sb + fix - ((fix & ((sb ^ eb) >> 63)) << 1));
}
// the exact products a * b and the addends, lanes 0-1 and 2-3, in double
static inline __attribute__((always_inline)) void widen(f4 a, f4 b, f4 c, d2& pl, d2& cl, d2& ph, d2& ch) {
    pl = __builtin_convertvector(__builtin_shufflevector(a, a, 0, 1), d2) * __builtin_convertvector(__builtin_shufflevector(b, b, 0, 1), d2);
    ph = __builtin_convertvector(__builtin_shufflevector(a, a, 2, 3), d2) * __builtin_convertvector(__builtin_shufflevector(b, b, 2, 3), d2);
    cl = __builtin_convertvector(__builtin_shufflevector(c, c, 0, 1), d2);
    ch = __builtin_convertvector(__builtin_shufflevector(c, c, 2, 3), d2);
}
static inline __attribute__((always_inline)) f4 to_f4(d2 lo, d2 hi) {
    return __builtin_shufflevector(__builtin_convertvector(lo, f2), __builtin_convertvector(hi, f2), 0, 1, 2, 3);
}
// MXCSR.RC other than nearest (TOTK_FPCR=1 with a directed FPCR.RMode): the odd sum in round-to-nearest (RC cleared,
// DAZ / FTZ kept), the final rounding in the caller's mode. An exact zero sum takes its sign from the caller's mode too
// (x + -x is -0 rounding down): those lanes are p + c again after the mode is back, exact. The empty asm statements keep
// the arithmetic between the MXCSR writes (clang does not order FP arithmetic against ldmxcsr; the asm statements and
// ldmxcsr are side effects, which keep their order).
static __attribute__((noinline, cold)) f4 fma4_dir(f4 a, f4 b, f4 c, unsigned m) {
    d2 pl, cl, ph, ch;
    widen(a, b, c, pl, cl, ph, ch);   // (exact in any mode)
    _mm_setcsr(m & ~0x6000u);
    __asm__ volatile("" : "+x"(pl), "+x"(cl), "+x"(ph), "+x"(ch));
    d2 lo = sum_odd(pl, cl), hi = sum_odd(ph, ch);
    __asm__ volatile("" : "+x"(lo), "+x"(hi));
    _mm_setcsr(m);
    __asm__ volatile("" : "+x"(lo), "+x"(hi), "+x"(pl), "+x"(cl), "+x"(ph), "+x"(ch));
    lo = lo == 0 ? pl + cl : lo;
    hi = hi == 0 ? ph + ch : hi;
    return to_f4(lo, hi);
}
static __attribute__((noinline, cold)) float fma1_dir(float a, float b, float c, unsigned m) {
    double p = (double)a * (double)b, cc = c;
    _mm_setcsr(m & ~0x6000u);
    __asm__ volatile("" : "+x"(p), "+x"(cc));
    double r = sum_odd1(p, cc);
    __asm__ volatile("" : "+x"(r));
    _mm_setcsr(m);
    __asm__ volatile("" : "+x"(r), "+x"(p), "+x"(cc));
    return (float)(r == 0 ? p + cc : r);
}
static inline __attribute__((always_inline)) f4 fma4(f4 a, f4 b, f4 c) {
    const unsigned m = _mm_getcsr();
    if (__builtin_expect(m & 0x6000, 0)) return fma4_dir(a, b, c, m);
    d2 pl, cl, ph, ch;
    widen(a, b, c, pl, cl, ph, ch);
    return to_f4(sum_odd(pl, cl), sum_odd(ph, ch));
}
static inline __attribute__((always_inline)) float fma1(float a, float b, float c) {
    const unsigned m = _mm_getcsr();
    if (__builtin_expect(m & 0x6000, 0)) return fma1_dir(a, b, c, m);
    return (float)sum_odd1((double)a * (double)b, (double)c);
}
// double: exact inline when both factors are floats (their product is exact in double, so one rounding of product +
// addend is the fused result in any mode); otherwise the CRT's fma
static inline __attribute__((always_inline)) double fmad(double a, double b, double c) {
    if (__builtin_expect((double)(float)a == a && (double)(float)b == b, 1)) {
        double p = a * b;
        return p + c;
    }
    return __builtin_fma(a, b, c);
}
}   // namespace rt_x86v2

static inline __attribute__((always_inline)) float rt_x86v2_fma(float a, float b, float c) { return rt_x86v2::fma1(a, b, c); }
static inline __attribute__((always_inline)) double rt_x86v2_fma(double a, double b, double c) { return rt_x86v2::fmad(a, b, c); }
template <class A, class B, class C> static inline __attribute__((always_inline)) auto rt_x86v2_fma(A a, B b, C c) {
    return std::fma(a, b, c);   // (mixed / integer arguments: <cmath>'s promotion to double, then the CRT)
}
static inline __attribute__((always_inline)) rt_x86v2::f4 rt_x86v2_vfma(rt_x86v2::f4 a, rt_x86v2::f4 b, rt_x86v2::f4 c) { return rt_x86v2::fma4(a, b, c); }
static inline __attribute__((always_inline)) float rt_x86v2_vfma(float a, float b, float c) { return rt_x86v2::fma1(a, b, c); }
static inline __attribute__((always_inline)) double rt_x86v2_vfma(double a, double b, double c) { return rt_x86v2::fmad(a, b, c); }
template <class V> static inline __attribute__((always_inline)) V rt_x86v2_vfma(V a, V b, V c) {
    return __builtin_elementwise_fma(a, b, c);   // (double lanes: the CRT's fma per lane, as before)
}
// std::fma (rt.h's fmlaA / fmlaS / fmlaN / frecps / frsqrts) and __builtin_elementwise_fma (rt.h's vfop / vfopN) as the
// functions above. Adding rt_x86v2_fma to std is what lets `std::fma(` keep its spelling in rt.h and the generated code.
namespace std { using ::rt_x86v2_fma; }
#define fma(a, b, c) rt_x86v2_fma(a, b, c)
#define __builtin_elementwise_fma(a, b, c) rt_x86v2_vfma(a, b, c)
#endif
