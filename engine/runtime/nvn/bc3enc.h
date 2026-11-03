// RGBA8 -> BC3 (DXT5), the low-VRAM texture mode's encoder (ram-diet, RIVAL-034: graphics.compact_textures). Decoded
// ASTC is RGBA8 on GPUs without ASTC, 4 bytes a texel; BC3 is 1. This is the reference: the runtime encodes on the GPU
// with vulkan/bc3_encode.comp, a line-by-line port (same steps, same float order), and here only the levels the CPU
// decoded (LARECOMP_ASTC_GPU=0, texpatch levels, levels over the GPU scratch). engine/tools/astcgpu checks the two
// against each other and both against the source (PSNR).
// Colour: the principal axis of the 16 texels (covariance, power iteration), endpoints at the extreme projections,
// indices by projection onto the quantised 565 endpoints, then one least-squares refit of the endpoints for those
// indices, kept if its error is lower. Always written with c0 > c1 (BC1's four-colour order, which BC3 assumes anyway).
// Alpha: the eight-value mode between the block's min and max, indices by rounding. Texels past the level's edge repeat
// the last row / column. sRGB levels are encoded as stored (BC3_SRGB decodes the endpoints the same way).
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t bc3_q565(const float e[3]) {
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = e[i] < 0.0f ? 0.0f : e[i] > 255.0f ? 255.0f : e[i];
    uint32_t r = (uint32_t)(c[0] * (31.0f / 255.0f) + 0.5f), g = (uint32_t)(c[1] * (63.0f / 255.0f) + 0.5f),
             b = (uint32_t)(c[2] * (31.0f / 255.0f) + 0.5f);
    return r << 11 | g << 5 | b;
}

static inline void bc3_x565(uint32_t c, float o[3]) {
    uint32_t r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = (float)(r << 3 | r >> 2);
    o[1] = (float)(g << 2 | g >> 4);
    o[2] = (float)(b << 3 | b >> 2);
}

// positions 0..3 along e0 -> e1 (0 = e0, 3 = e1) for the quantised endpoints; returns the squared error
static inline float bc3_fit(const float px[16][3], uint32_t q0, uint32_t q1, uint32_t pos[16]) {
    float a[3], b[3], d[3];
    bc3_x565(q0, a);
    bc3_x565(q1, b);
    for (int k = 0; k < 3; k++) d[k] = b[k] - a[k];
    float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], err = 0.0f;
    for (int i = 0; i < 16; i++) {
        float t = 0.0f;
        if (dd > 0.0f) t = ((px[i][0] - a[0]) * d[0] + (px[i][1] - a[1]) * d[1] + (px[i][2] - a[2]) * d[2]) / dd;
        float s = t * 3.0f + 0.5f;
        uint32_t p = s < 0.0f ? 0u : s >= 3.0f ? 3u : (uint32_t)s;
        pos[i] = p;
        // the decoder's colour: (3 - p) / 3 of a + p / 3 of b
        for (int k = 0; k < 3; k++) {
            float v = (a[k] * (float)(3 - p) + b[k] * (float)p) / 3.0f - px[i][k];
            err += v * v;
        }
    }
    return err;
}

// one 4x4 block: px RGB (0..255), al alpha (0..255) -> 16 bytes
static inline void bc3_block(const float px[16][3], const uint32_t al[16], uint8_t out[16]) {
    // alpha
    uint32_t amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { if (al[i] < amin) amin = al[i]; if (al[i] > amax) amax = al[i]; }
    uint32_t alo = 0, ahi = 0;
    if (amax > amin) {
        uint32_t r = amax - amin;
        for (int i = 0; i < 16; i++) {
            uint32_t t = ((al[i] - amin) * 14 + r) / (2 * r);   // 0 = amin .. 7 = amax
            uint32_t idx = t == 7 ? 0u : t == 0 ? 1u : 8u - t;
            uint32_t bit = 3u * (uint32_t)i;
            if (bit < 32) alo |= idx << bit;
            if (bit + 3 > 32) ahi |= bit >= 32 ? idx << (bit - 32) : idx >> (32 - bit);
        }
    }
    // colour: mean, covariance, principal axis
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) mean[k] += px[i][k];
    for (int k = 0; k < 3; k++) mean[k] /= 16.0f;
    float cv[6] = {0, 0, 0, 0, 0, 0};   // xx xy xz yy yz zz
    for (int i = 0; i < 16; i++) {
        float x = px[i][0] - mean[0], y = px[i][1] - mean[1], z = px[i][2] - mean[2];
        cv[0] += x * x; cv[1] += x * y; cv[2] += x * z; cv[3] += y * y; cv[4] += y * z; cv[5] += z * z;
    }
    // (start: the covariance's column of the largest variance; cov * (1, 1, 1) is 0 for a block that varies along
    // (1, -1, 0))
    int k0 = cv[0] >= cv[3] && cv[0] >= cv[5] ? 0 : cv[3] >= cv[5] ? 1 : 2;
    float v[3] = {k0 == 0 ? cv[0] : k0 == 1 ? cv[1] : cv[2], k0 == 0 ? cv[1] : k0 == 1 ? cv[3] : cv[4],
                  k0 == 0 ? cv[2] : k0 == 1 ? cv[4] : cv[5]};
    for (int it = 0; it < 6; it++) {
        float m = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (m < 1e-12f) break;
        float s = 1.0f / __builtin_sqrtf(m);
        float n0 = v[0] * s, n1 = v[1] * s, n2 = v[2] * s;
        v[0] = cv[0] * n0 + cv[1] * n1 + cv[2] * n2;
        v[1] = cv[1] * n0 + cv[3] * n1 + cv[4] * n2;
        v[2] = cv[2] * n0 + cv[4] * n1 + cv[5] * n2;
    }
    float m = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    float e0[3], e1[3];
    if (m < 1e-12f) {
        for (int k = 0; k < 3; k++) e0[k] = e1[k] = mean[k];
    } else {
        float s = 1.0f / __builtin_sqrtf(m), tmin = 1e30f, tmax = -1e30f;
        for (int k = 0; k < 3; k++) v[k] *= s;
        for (int i = 0; i < 16; i++) {
            float t = (px[i][0] - mean[0]) * v[0] + (px[i][1] - mean[1]) * v[1] + (px[i][2] - mean[2]) * v[2];
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
        }
        for (int k = 0; k < 3; k++) { e0[k] = mean[k] + v[k] * tmax; e1[k] = mean[k] + v[k] * tmin; }
    }
    uint32_t q0 = bc3_q565(e0), q1 = bc3_q565(e1), pos[16], pos2[16];
    float err = bc3_fit(px, q0, q1, pos);
    // least-squares refit for these positions: minimise sum ((1 - w) a + w b - x)^2, w = pos / 3
    float aa = 0, ab = 0, bb = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        float w = (float)pos[i] / 3.0f, u = 1.0f - w;
        aa += u * u; ab += u * w; bb += w * w;
        for (int k = 0; k < 3; k++) { ax[k] += u * px[i][k]; bx[k] += w * px[i][k]; }
    }
    float det = aa * bb - ab * ab;
    if (det > 1e-6f) {
        float r0[3], r1[3];
        for (int k = 0; k < 3; k++) { r0[k] = (ax[k] * bb - bx[k] * ab) / det; r1[k] = (bx[k] * aa - ax[k] * ab) / det; }
        uint32_t p0 = bc3_q565(r0), p1 = bc3_q565(r1);
        float err2 = bc3_fit(px, p0, p1, pos2);
        if (err2 < err) { q0 = p0; q1 = p1; memcpy(pos, pos2, sizeof pos2); }
    }
    // four-colour order: c0 > c1 (equal: every texel is c0)
    static const uint32_t idx_of[4] = {0, 2, 3, 1};   // position 0 = c0, 1/3 = colour 2, 2/3 = colour 3, 3 = c1
    uint32_t ci = 0;
    if (q0 < q1) {
        uint32_t t = q0; q0 = q1; q1 = t;
        for (int i = 0; i < 16; i++) pos[i] = 3 - pos[i];
    }
    if (q0 != q1)
        for (int i = 0; i < 16; i++) ci |= idx_of[pos[i]] << (2 * i);
    uint32_t w[4] = {amax | amin << 8 | (alo & 0xffff) << 16, alo >> 16 | ahi << 16, q0 | q1 << 16, ci};
    if (amax == amin) w[0] = amax | amax << 8, w[1] = 0;
    memcpy(out, w, 16);
}

// One level: rgba (row texels a row, w x h) -> ceil(w / 4) x ceil(h / 4) blocks, 16 bytes each, row by row.
static inline void bc3_encode(const uint8_t *rgba, uint32_t row, uint32_t w, uint32_t h, uint8_t *out) {
    uint32_t bxn = (w + 3) / 4, byn = (h + 3) / 4;
    for (uint32_t by = 0; by < byn; by++)
        for (uint32_t bx = 0; bx < bxn; bx++) {
            float px[16][3];
            uint32_t al[16];
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w) x = w - 1;
                if (y >= h) y = h - 1;
                const uint8_t *p = rgba + ((uint64_t)y * row + x) * 4;
                px[i][0] = p[0]; px[i][1] = p[1]; px[i][2] = p[2];
                al[i] = p[3];
            }
            bc3_block(px, al, out + ((uint64_t)by * bxn + bx) * 16);
        }
}

// The decoder (the tests' and the encoder check's): one level back to RGBA8.
static inline void bc3_decode(const uint8_t *in, uint32_t w, uint32_t h, uint8_t *rgba) {
    uint32_t bxn = (w + 3) / 4, byn = (h + 3) / 4;
    for (uint32_t by = 0; by < byn; by++)
        for (uint32_t bx = 0; bx < bxn; bx++) {
            const uint8_t *b = in + ((uint64_t)by * bxn + bx) * 16;
            uint32_t a0 = b[0], a1 = b[1], ap[8];
            ap[0] = a0; ap[1] = a1;
            for (uint32_t i = 2; i < 8; i++)
                ap[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : i == 6 ? 0 : 255;
            uint64_t ab = 0;
            for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
            uint32_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8, ci = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
            float f0[3], f1[3];
            bc3_x565(c0, f0);
            bc3_x565(c1, f1);
            uint32_t cp[4][3];
            for (int k = 0; k < 3; k++) {
                cp[0][k] = (uint32_t)f0[k]; cp[1][k] = (uint32_t)f1[k];
                cp[2][k] = (2 * cp[0][k] + cp[1][k]) / 3; cp[3][k] = (cp[0][k] + 2 * cp[1][k]) / 3;
            }
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t *p = rgba + ((uint64_t)y * w + x) * 4;
                uint32_t c = ci >> (2 * i) & 3;
                p[0] = (uint8_t)cp[c][0]; p[1] = (uint8_t)cp[c][1]; p[2] = (uint8_t)cp[c][2];
                p[3] = (uint8_t)ap[ab >> (3 * i) & 7];
            }
        }
}
