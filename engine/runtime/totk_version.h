// Guest main addresses in the engine's own code are TotK 1.0.0 offsets. Name every one with totk_main(off) (an
// address) or totk_main_off(off) (an offset), never as main's base + a literal: in a build for another game version
// they become that version's. tools/engine/test_version_sites.py refuses the literal forms.
//
// 1.0.0 (the default): the identity, nothing changes. Another version: tools/engine/build.py (TOTK_GAME_VERSION=1.4.3)
// compiles with -DTOTK_GAME_VERSION=143 and this header includes totk_main_v143.inc, written by
// `python -m games.totk.patches engine-map` from the porter (tools/version_port.py): sorted {1.0.0 offset, offset in
// that build} pairs for the hex constants of the engine's sources the porter placed. An offset the table lacks is a
// build error where the address is a constant expression (constexpr / GUEST_FN), else a logged stop at its
// first use: never a 1.0.0 address in another build.
#pragma once
#include <stdint.h>

#define TOTK_MAIN_BASE 0x7100000000ull   // main's base in the engine (gen.py IMGBASE)

#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#ifdef __cplusplus
#if TOTK_GAME_VERSION == 143
#include "totk_main_v143.inc"           // constexpr uint64_t kTotkMainMap[][2], kTotkMainMapCount
#else
#error "no main address table for this TOTK_GAME_VERSION (python -m games.totk.patches engine-map)"
#endif
extern "C" [[noreturn]] void totk_main_unmapped(uint64_t off);   // totk_version.cpp: logs the offset and stops
constexpr uint64_t totk_main_off(uint64_t off) {
    uint64_t lo = 0, hi = kTotkMainMapCount;
    while (lo < hi) {
        const uint64_t m = (lo + hi) / 2;
        if (kTotkMainMap[m][0] < off) lo = m + 1; else hi = m;
    }
    if (lo == kTotkMainMapCount || kTotkMainMap[lo][0] != off) totk_main_unmapped(off);   // not constant: build error
    return kTotkMainMap[lo][1];
}
#else
uint64_t totk_main_off_c(uint64_t off);   // totk_version.cpp
#define totk_main_off(off) totk_main_off_c(off)
#endif
#else
// 1.0.0: macros, not functions, so the code is the same as before them everywhere: clang does not inline even an
// always_inline function into a SEH __try region (pc_fault_guard), and would emit a call there.
#define totk_main_off(off) ((uint64_t)(off))
#endif

#define totk_main(off) (TOTK_MAIN_BASE + totk_main_off(off))
