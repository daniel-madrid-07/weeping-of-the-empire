// host_wine.h: whether the Windows build runs under Wine / Proton (Linux, Steam Deck): Wine's ntdll exports
// wine_get_version, Windows' never does (Proton 9 / 10 keep it). Where Wine lacks or fakes what the runtime relies on,
// the callers take another path: cpu_place.cpp (CPU Sets are stubs), hle_os_mem.cpp (write watch off, as on Linux),
// shim_win.c (memory figures from Linux). C and C++; always 0 on the Linux build.
#pragma once
#ifdef _WIN32
#include <windows.h>
static inline int host_is_wine(void) {
    static int wine = -1;   // (each translation unit asks once; a race only repeats the same answer)
    if (wine < 0) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        wine = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    }
    return wine;
}
#else
static inline int host_is_wine(void) { return 0; }
#endif
