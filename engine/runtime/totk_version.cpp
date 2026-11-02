// totk_version.h's out-of-line half: only builds for another game version use it (1.0.0: nothing here).
#include "totk_version.h"

#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#include <cstdio>
#include <cstdlib>

#include "rt.h"   // (Ctx: the diagnostic stubs)

extern "C" [[noreturn]] void totk_main_unmapped(uint64_t off) {
    std::fprintf(stderr, "[totk] main+%#llx (1.0.0) has no place in game version %d: missing from "
                 "totk_main_v%d.inc (python -m games.totk.patches engine-map)\n",
                 (unsigned long long)off, TOTK_GAME_VERSION, TOTK_GAME_VERSION);
    std::fflush(stderr);
    std::abort();
}

extern "C" uint64_t totk_main_off_c(uint64_t off) { return totk_main_off(off); }

#ifdef __cplusplus
#include <mutex>
#include <set>
extern "C" bool totk_main_known_rt(uint64_t off) {
    if (totk_main_known(off)) return true;
    static std::mutex m;
    static std::set<uint64_t> seen;
    {
        std::lock_guard<std::mutex> l(m);
        if (!seen.insert(off).second) return false;
    }
    std::printf("[totk] main+%#llx (1.0.0) not placed in game version %d: its reader stands down\n",
                (unsigned long long)off, TOTK_GAME_VERSION);
    std::fflush(stdout);
    return false;
}
#endif

// a diagnostic build (engine-map --diagnostic, never shipped): trap stubs for the 1.0.0 names it could not place
#if TOTK_GAME_VERSION == 143 && __has_include("totk_main_v143_stubs.inc")
#include "totk_main_v143_stubs.inc"
#elif TOTK_GAME_VERSION == 142 && __has_include("totk_main_v142_stubs.inc")
#include "totk_main_v142_stubs.inc"
#endif
#endif
