// Tegra X1 texture layout: formats and block-linear (GOB) swizzling.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bpb;     // bytes per block (per texel for uncompressed)
    uint8_t bw, bh;  // block dimensions in texels
    uint8_t kind;    // FMT_COLOR, FMT_DEPTH, ...
    const char *name;
} FormatInfo;

enum { FMT_COLOR = 0, FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_DEPTH_STENCIL = 3, FMT_COMPRESSED = 4 };

const FormatInfo *nvn_format_info(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  // depth: 3D depth; layers: array layers / cube faces
    const FormatInfo *fmt;
    uint32_t block_height_log2;  // GOBs per block (log2) for level 0
    uint32_t block_depth_log2;
    uint64_t pitch;              // pitch-linear row stride in bytes (NVN LINEAR / LINEAR_RENDER_TARGET); 0 = block-linear
} TexLayout;

// NVN texture flags that mean pitch-linear storage: LINEAR (0x10) and LINEAR_RENDER_TARGET (0x40). The second was read
// as block-linear until 2026-10-15: TotK's terrain heightmaps (260x260 R16, flags 0x40, stride 640) decoded as dashes
// and the grass placed on them became the vertical streaks. LARECOMP_LINEAR_RT_BLOCK=1: the old reading (A/B).
bool nvn_flags_linear(uint32_t flags);

typedef struct {
    uint64_t offset;              // from the start of the layer
    uint32_t w, h, d;             // texel dimensions
    uint32_t wb, hb;              // in blocks
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

// Default block height for a texture (what the NVN driver picks), and the full storage layout.
uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks);
uint32_t tegra_old_block_height_log2(uint32_t height_in_blocks);   // (the rule before LARECOMP_NVN_BLOCK_HEIGHT)
uint32_t tegra_nvn_block_height_log2(uint32_t height_in_blocks);
bool tegra_nvn_block_height_on(void);
void tegra_layout_init(TexLayout *t);
LevelLayout tegra_level(const TexLayout *t, uint32_t level);
uint64_t tegra_layer_size(const TexLayout *t);
uint64_t tegra_total_size(const TexLayout *t);

// Copy a block-linear level slice into a linear buffer (row pitch = wb * bpb).
void tegra_deswizzle_rows(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bh_log2,
                          uint32_t bd_log2, uint32_t y0, uint32_t y1);   // block rows [y0, y1), depth 1
void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t block_height_log2, uint32_t block_depth_log2);
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);
// Bytes [x0, x0 + nx) of block rows [y0, y0 + ny) of slices [z0, z0 + nz) of a block-linear surface (wb x hb x d blocks
// of bpb bytes) to (to_surface) or from linear data whose row y of slice z is at lin + z * is + y * rs: the region
// alone, at its swizzled addresses (texels_rw used to deswizzle and reswizzle the whole level around each write).
void tegra_copy_region(uint8_t *surf, uint8_t *lin, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                       uint32_t block_height_log2, uint32_t block_depth_log2, uint32_t x0, uint32_t y0, uint32_t z0,
                       uint32_t nx, uint32_t ny, uint32_t nz, uint64_t rs, uint64_t is, bool to_surface);
