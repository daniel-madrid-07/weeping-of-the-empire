// totk_version.h's out-of-line half: only builds for another game version use it (1.0.0: nothing here).
#include "totk_version.h"

#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#include <cstdio>
#include <cstdlib>

extern "C" [[noreturn]] void totk_main_unmapped(uint64_t off) {
    std::fprintf(stderr, "[totk] main+%#llx (1.0.0) has no place in game version %d: missing from "
                 "totk_main_v%d.inc (python -m games.totk.patches engine-map)\n",
                 (unsigned long long)off, TOTK_GAME_VERSION, TOTK_GAME_VERSION);
    std::fflush(stderr);
    std::abort();
}

extern "C" uint64_t totk_main_off_c(uint64_t off) { return totk_main_off(off); }
#endif
