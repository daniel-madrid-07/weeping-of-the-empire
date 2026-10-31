// spvz: the SPIR-V disk cache's compressed entries (shaders.c cache_load / cache_store; spvz.c). Windows' own XPRESS
// Huffman (cabinet.dll's Compression API, Windows 8+; no library of ours); elsewhere entries stay raw.
// An entry is {'TKSZ', 4 = XPRESS Huffman, SPIR-V bytes, compressed bytes} + the compressed data, padded to whole words.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SPVZ_MAGIC = 0x5A534B54u, SPVZ_XPRESS_HUFF = 4 };

// spv[bytes] -> a compressed entry (malloc'd, *out_bytes) when it saves a tenth or more; NULL: store it raw (no codec,
// LARECOMP_SPIRV_COMPRESS=0, or it does not shrink). Any thread.
uint8_t *spvz_pack(const uint32_t *spv, size_t bytes, size_t *out_bytes);
// A compressed entry file[n] -> its SPIR-V words (malloc'd, *bytes); NULL when it is not a well-formed entry or there is
// no codec. Any thread.
uint32_t *spvz_unpack(const uint8_t *file, size_t n, size_t *bytes);
// The raw entries (<hash>.spv) an older build left in dir (cache/spirv) compressed to <hash>.spz and removed, once
// (dir/compact2.done after a whole pass; compact.done was db866e6d's, before the.spz name), a file at a time with a 5 ms
// pause, in Windows' background mode (low I/O priority): for an idle thread (exec.c, the precompile's last worker). One
// open elsewhere is skipped (the next pass takes it). A compressed entry under the raw name is renamed. Returns the
// entries packed; *raw / *packed their bytes.
int spvz_compact(const char *dir, size_t *raw, size_t *packed);
