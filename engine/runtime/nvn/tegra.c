// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <stdlib.h>
#include <string.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

uint32_t tegra_old_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, 8);
    uint32_t log2 = 0;
    while ((1u << log2) < gobs && log2 < 4) log2++;
    return log2;
}

// NVN's own rule: GOBs = (hb + hb / 2) / 8, truncated, then the largest power of two not above it (MacNativeGames
// nvn_tex.c:44,138). The old rule above rounds ceil(hb / 8) up instead: a 160x160 BC3 texture (40 block rows) gets 8
// GOBs where NVN uses 4. Mip 0 has the same size either way, so a CPU-written texture decodes with zero padding over a
// rectangle. LARECOMP_NVN_BLOCK_HEIGHT=0: the old rule.
uint32_t tegra_nvn_block_height_log2(uint32_t height_in_blocks) {
    uint32_t g = (height_in_blocks + height_in_blocks / 2) / 8;
    return g >= 16 ? 4 : g >= 8 ? 3 : g >= 4 ? 2 : g >= 2 ? 1 : 0;
}

bool tegra_nvn_block_height_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("LARECOMP_NVN_BLOCK_HEIGHT") && !strcmp(getenv("LARECOMP_NVN_BLOCK_HEIGHT"), "0"));
    return on;
}

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    return tegra_nvn_block_height_on() ? tegra_nvn_block_height_log2(height_in_blocks) : tegra_old_block_height_log2(height_in_blocks);
}

static uint32_t block_depth_for(uint32_t depth) {
    uint32_t log2 = 0;
    while ((1u << log2) < depth && log2 < 5) log2++;
    return log2;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
    // 3D (TotK, measured 2026-10-15): a block holds at most 16 GOBs, depth first: block depth = min(log2 depth, 4) and
    // block height shrinks to fit. Evidence: the CPU-written 256x32x16 RGBA16F atmosphere LUT only decodes smoothly as
    // block height 1 GOB x depth 16 (z roughness 0.13 vs 0.51 for the old 4 x 16), and the packaged 64x64x64 BC4 volume's
    // NVNpackagedTextureLayout says the same (low word 0x40 = depth 2^4, height 2^0). LARECOMP_OLD_3D_LAYOUT=1: old.
    // LARECOMP_NVN_3D_LAYOUT=1: NVN's 3D rule (MacNativeGames nvn_tex.c:139): block height 1 GOB, block depth from the
    // truncated 1.5x depth like the 2D rule. Differs from the one below for 3D textures with more than 8 block rows and
    // depth below 16 (e.g. depth 8: 1 GOB high here, 2 below).
    static int nvn3d = -1;
    if (nvn3d < 0) nvn3d = getenv("LARECOMP_NVN_3D_LAYOUT") && strcmp(getenv("LARECOMP_NVN_3D_LAYOUT"), "0");
    if (t->depth > 1 && nvn3d) {
        uint32_t z = t->depth + t->depth / 2;
        t->block_height_log2 = 0;
        t->block_depth_log2 = z >= 16 ? 4 : z >= 8 ? 3 : z >= 4 ? 2 : z >= 2 ? 1 : 0;
    } else if (t->depth > 1 && !getenv("LARECOMP_OLD_3D_LAYOUT")) {
        if (t->block_depth_log2 > 4) t->block_depth_log2 = 4;
        if (t->block_height_log2 + t->block_depth_log2 > 4) t->block_height_log2 = 4 - t->block_depth_log2;
    }
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

bool nvn_flags_linear(uint32_t flags) {
    static int old = -1;
    if (old < 0) old = getenv("LARECOMP_LINEAR_RT_BLOCK") != NULL;
    return (flags & 0x10) || (!old && (flags & 0x40));
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    if (t->pitch) {   // pitch-linear: rows of t->pitch bytes (single level)
        if (out) { out->w = w; out->h = h; out->d = d; out->wb = wb; out->hb = hb; out->block_height_log2 = 0; out->block_depth_log2 = 0; }
        return t->pitch * hb * d;
    }
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size
        uint32_t bh = t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

// One run within a 16-byte sector. The fixed-size copy inlines to one 16-byte move; a variable-size memcpy per sector
// went through VCRUNTIME's memcpy (63 % of bake::ModuleMgr's CPU in the world load, ld_w2_title).
static inline void copy_run(uint8_t *dst, const uint8_t *src, uint32_t n) {
    if (n == 16) memcpy(dst, src, 16);
    else memcpy(dst, src, n);
}

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, bool to_linear) {
    uint32_t row_bytes = wb * bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < d; z++) {
        for (uint32_t y = 0; y < hb; y++) {
            uint8_t *lin = dst + ((uint64_t)z * hb + y) * row_bytes;
            const uint8_t *lin_src = src + ((uint64_t)z * hb + y) * row_bytes;
            // 16-byte runs are contiguous in both layouts
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint64_t o = gob_offset(x, y, z, width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_linear) copy_run(lin + x, src + o, n);
                else copy_run(dst + o, lin_src + x, n);
            }
        }
    }
}

void tegra_copy_region(uint8_t *surf, uint8_t *lin, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                       uint32_t bh_log2, uint32_t bd_log2, uint32_t x0, uint32_t y0, uint32_t z0, uint32_t nx,
                       uint32_t ny, uint32_t nz, uint64_t rs, uint64_t is, bool to_surface) {
    uint32_t width_gobs = (wb * bpb + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    uint32_t x1 = x0 + nx;
    for (uint32_t z = 0; z < nz && z0 + z < d; z++)
        for (uint32_t y = 0; y < ny && y0 + y < hb; y++) {
            uint8_t *row = lin + (uint64_t)z * is + (uint64_t)y * rs;
            for (uint32_t x = x0; x < x1;) {
                uint32_t n = 16 - (x & 15);
                if (n > x1 - x) n = x1 - x;
                uint64_t o = gob_offset(x, y0 + y, z0 + z, width_gobs, bh_log2, bd_log2, blocks_rows);
                if (to_surface) copy_run(surf + o, row + (x - x0), n);
                else copy_run(row + (x - x0), surf + o, n);
                x += n;
            }
        }
}

// Block rows [y0, y1) of a depth-1 surface into dst (its row 0 = row y0): the ASTC upload's workers each take a
// range.
void tegra_deswizzle_rows(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bh_log2,
                          uint32_t bd_log2, uint32_t y0, uint32_t y1) {
    uint32_t row_bytes = wb * bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << bh_log2;
    uint64_t blocks_rows = (hb + gob_rows - 1) / gob_rows;
    if (y1 > hb) y1 = hb;
    for (uint32_t y = y0; y < y1; y++) {
        uint8_t *lin = dst + (uint64_t)(y - y0) * row_bytes;
        for (uint32_t x = 0; x < row_bytes; x += 16) {
            uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
            copy_run(lin + x, src + gob_offset(x, y, 0, width_gobs, bh_log2, bd_log2, blocks_rows), n);
        }
    }
}
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}
