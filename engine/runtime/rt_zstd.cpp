// rt_zstd.cpp: zstd 1.5.7's decoder (third_party/zstd, BSD; its decompression sources only, no assembly) compiled as one
// runtime TU, as pc_imgui.cpp does with Dear ImGui, in the order and with the settings of zstd's own single-file decoder
// (build/single_file_libs/zstddeclib-in.c). hle_fs.cpp reads the game's resource size tables with it, to merge a mod's
// table into ours (hle_fs.cpp rstb_merged).
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_STRIP_ERROR_STRINGS
#define ZSTD_TRACE 0
#define ZSTD_DISABLE_ASM 1
#define ZSTD_DEPS_NEED_MALLOC
#include "../third_party/zstd/common/zstd_deps.h"
#include "../third_party/zstd/common/debug.c"
#include "../third_party/zstd/common/entropy_common.c"
#include "../third_party/zstd/common/error_private.c"
#include "../third_party/zstd/common/fse_decompress.c"
#include "../third_party/zstd/common/zstd_common.c"
#include "../third_party/zstd/decompress/huf_decompress.c"
#include "../third_party/zstd/decompress/zstd_ddict.c"
#include "../third_party/zstd/decompress/zstd_decompress.c"
#include "../third_party/zstd/decompress/zstd_decompress_block.c"
