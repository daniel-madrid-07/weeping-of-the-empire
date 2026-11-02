// The largest of count indices of isz (2 or 4) bytes at src, the restart index left out; 0 when there is none. draw_'s
// bound for the vertex data it copies (exec.c). The same answer as the scalar
//     for (i = 0; i < count; i++) if (s[i] > m && s[i] != restart) m = s[i];
// 16 / 8 indices a step: clang vectorizes neither that loop nor its select form ("not identified as reduction"), and it
// was draw_'s hottest block (855fd396 profile: 12.8 G of draw_'s 37 G block counts). The restart index counts as 0,
// which never raises the maximum. Clang vector extensions, so the no-AVX keys (x86-64 / x86-64-v2) build it too.
#ifndef NVN_VK_INDEX_MAX_H
#define NVN_VK_INDEX_MAX_H
#include <stdint.h>
#include <string.h>

static inline uint32_t index_max(const void *src, uint32_t count, uint32_t isz, uint32_t restart) {
    uint32_t i = 0;
    if (isz == 2) {
        typedef uint16_t u16x16 __attribute__((vector_size(32)));
        const uint16_t *s = (const uint16_t *)src;
        const uint16_t r = (uint16_t)restart;
        u16x16 m = {0}, q;
        for (; i + 16 <= count; i += 16) {
            memcpy(&q, s + i, sizeof q);   // (unaligned)
            m = __builtin_elementwise_max(m, q & (u16x16)(q != r));
        }
        uint16_t mx = __builtin_reduce_max(m);
        for (; i < count; i++) if (s[i] > mx && s[i] != r) mx = s[i];
        return mx;
    }
    typedef uint32_t u32x8 __attribute__((vector_size(32)));
    const uint32_t *s = (const uint32_t *)src;
    u32x8 m = {0}, q;
    for (; i + 8 <= count; i += 8) {
        memcpy(&q, s + i, sizeof q);
        m = __builtin_elementwise_max(m, q & (u32x8)(q != restart));
    }
    uint32_t mx = __builtin_reduce_max(m);
    for (; i < count; i++) if (s[i] > mx && s[i] != restart) mx = s[i];
    return mx;
}
#endif
