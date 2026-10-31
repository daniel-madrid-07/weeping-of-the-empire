// x86-64-v2 builds (tools/engine/build.py --march x86-64-v2, the Builder's --cpu v2 for processors without AVX2: Sandy /
// Ivy Bridge, AMD FX and Phenom, Pentium / Celeron up to 2020). build.py force-includes this header (-include) into the
// game objects and the runtime of such a build only, so rt.h and every x86-64-v3 key stay as they are.
//
// What a v2 compile lacks, and what stands in:
//  - _mm_cmp_ps (AVX: a VEX-only compare with a predicate; clang's is a macro over __builtin_ia32_cmpps): the two
//    predicates rt.h uses map to SSE compares giving the same lane masks: _CMP_GE_OQ -> _mm_cmpge_ps (cmpleps with
//    the operands swapped: false on NaN, as the ordered predicate; it is the signalling form, which only differs in the
//    MXCSR invalid flag, masked and never read), _CMP_UNORD_Q -> _mm_cmpunord_ps. Any other predicate traps.
//  - FMA: std::fma and __builtin_elementwise_fma stay exact (one rounding, as ARM's fmla): without the instruction
//    clang calls the CRT's fma / fmaf, the same bits, only slower. No mul + add stand-in: that rounds twice.
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
