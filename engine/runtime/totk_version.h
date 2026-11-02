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
// A diagnostic build's table (engine-map --diagnostic, never shipped) maps an offset it could not place here: the
// build compiles, and code that may run without its hook asks totk_main_known() before it uses the address.
#define TOTK_MAIN_UNKNOWN 0xFFFFFFF0ull

#include "nxr_game.h"
#if !NXR_GAME_TOTK
// Another game (nxr_game.h): no TotK main address exists. Every offset is unknown, so the generic code's
// totk_main_known() checks skip what only TotK has (TOTK_GAME_VERSION is TotK's axis and ignored here).
#define totk_main_off(off) ((void)(off), TOTK_MAIN_UNKNOWN)
#define totk_main_known(off) 0
#elif defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#ifdef __cplusplus
#if TOTK_GAME_VERSION == 143
#include "totk_main_v143.inc"           // constexpr uint64_t kTotkMainMap[][2], kTotkMainMapCount
#elif TOTK_GAME_VERSION == 142
#include "totk_main_v142.inc"
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
constexpr bool totk_main_known(uint64_t off) { return totk_main_off(off) != TOTK_MAIN_UNKNOWN; }
// the same at run time, and each offset found unknown is logged once ("[totk] main+0x... not placed"): a beta drive
// counts them
extern "C" bool totk_main_known_rt(uint64_t off);   // totk_version.cpp
#else
uint64_t totk_main_off_c(uint64_t off);   // totk_version.cpp
#define totk_main_off(off) totk_main_off_c(off)
#define totk_main_known(off) (totk_main_off_c(off) != TOTK_MAIN_UNKNOWN)
#endif
#else
// 1.0.0: macros, not functions, so the code is the same as before them everywhere: clang does not inline even an
// always_inline function into a SEH __try region (pc_fault_guard), and would emit a call there.
#define totk_main_off(off) ((uint64_t)(off))
#define totk_main_known(off) 1
#endif

#define totk_main(off) (TOTK_MAIN_BASE + totk_main_off(off))

// Guest struct field offsets the same way: totk_field(Struct, 0x220) is the 1.0.0 offset of a field of the named game
// struct (one hex literal, spelled as the table spells it: lowercase, no suffix). 1.0.0: the literal itself. Another
// version: TF_<Struct>_<off> from totk_fields_v<N>.inc (`#define TF_Actor_0x220 0x228` lines, private like the main
// table), so a field the table lacks is a build error in C and C++ alike, never a 1.0.0 offset in another build. The
// struct name keeps two structs' equal 1.0.0 offsets apart (a field moves with its struct, not with its number). A
// diagnostic table maps a field it could not place to TOTK_FIELD_UNKNOWN; code that may run without its hook asks
// totk_field_known() first.
#define TOTK_FIELD_UNKNOWN 0x7FFFFFF0u
#if !NXR_GAME_TOTK
#define totk_field(S, off) ((void)(off), TOTK_FIELD_UNKNOWN)
#define totk_field_known(S, off) 0
#elif defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#if TOTK_GAME_VERSION == 143
#include "totk_fields_v143.inc"
#elif TOTK_GAME_VERSION == 142
#include "totk_fields_v142.inc"
#else
#error "no field table for this TOTK_GAME_VERSION"
#endif
#define totk_field(S, off) (TF_##S##_##off)
#define totk_field_known(S, off) (TF_##S##_##off != TOTK_FIELD_UNKNOWN)
#else
#define totk_field(S, off) (off)
#define totk_field_known(S, off) 1
#endif

#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
// The ActionVerb numbers the engine compares (totk_player_state.cpp's readers, totk_pc_vk.cpp's hook) are 1.0.0's.
// 1.4.3 keeps them all: the game's own per-verb name table (1.0.0 main+0x42258d0, 1.4.3 main+0x36370b8) names the
// same verb at every index 0.. 283 in both builds, so Invalid stays 284 and each id keeps its meaning (57: 0x16e8f1c
// -> 0x1d02818). Another version without such a check: 0, and that code stands down whatever the field table places.
// 1.0.0 builds do not define it.
#define TOTK_VERBS_KNOWN (TOTK_GAME_VERSION == 143)
#endif
