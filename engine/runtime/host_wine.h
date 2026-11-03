// host_wine.h: whether the Windows build runs under Wine / Proton (Linux, Steam Deck): Wine's ntdll exports
// wine_get_version, Windows' never does (Proton 9 / 10 keep it). Where Wine lacks or fakes what the runtime relies on,
// the callers take another path: cpu_place.cpp (CPU Sets are stubs), hle_os_mem.cpp (write watch off, as on Linux),
// shim_win.c (memory figures from Linux). C and C++; always 0 on the Linux build. host_steam_deck: whether the machine
// is a Steam Deck, and which (pc_settings.cpp's first-start defaults, hle_hid.cpp's Deck IMU, the Linux launcher's frame
// rate; one helper for every Deck-only path). Both builds.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define HOST_DMI_DIR "Z:\\sys\\devices\\virtual\\dmi\\id\\"
#else
static inline int host_is_wine(void) { return 0; }
#define HOST_DMI_DIR "/sys/devices/virtual/dmi/id/"
#endif

// The board's name decides: Jupiter (LCD) or Galileo (OLED), from Linux's DMI files (natively, or under Wine on its Z:
// drive) and on Windows the BIOS key Windows and Wine fill from SMBIOS. SteamDeck=1 (Steam in the Deck's Game Mode,
// through Proton too) only when no name can be read: SteamOS-like sessions on desktop PCs (Bazzite, ChimeraOS: Steam
// -steamdeck) set it too. 0: not a Deck, HOST_DECK_LCD (Jupiter; also SteamDeck=1, the model unknown), HOST_DECK_OLED
// (Galileo): non-zero is a Deck.
enum { HOST_DECK_LCD = 1, HOST_DECK_OLED = 2 };
static inline int host_steam_deck_name(const char *s) {
    return !strncmp(s, "Jupiter", 7) ? HOST_DECK_LCD : !strncmp(s, "Galileo", 7) ? HOST_DECK_OLED : 0;
}
static inline int host_steam_deck(void) {
    static int deck = -1;   // (as host_is_wine)
    if (deck >= 0) return deck;
    static const char *const dmi[] = {HOST_DMI_DIR "product_name", HOST_DMI_DIR "board_name"};
    char name[64];
    int read = 0, is = 0;
#ifdef _WIN32
    static const char *const keys[] = {"SystemProductName", "BaseBoardProduct"};
    for (int i = 0; i < 2; i++) {
        DWORD n = sizeof name;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", keys[i], RRF_RT_REG_SZ, NULL, name, &n) ==
                ERROR_SUCCESS && name[0]) {
            read = 1;
            if (!is) is = host_steam_deck_name(name);
        }
    }
    const int files = host_is_wine();
#else
    const int files = 1;
#endif
    for (int i = 0; i < 2 && files; i++) {
        FILE *f = fopen(dmi[i], "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && name[0] && name[0] != '\n') {
            read = 1;
            if (!is) is = host_steam_deck_name(name);
        }
        fclose(f);
    }
    if (!read) {
        const char *e = getenv("SteamDeck");
        is = e && !strcmp(e, "1") ? HOST_DECK_LCD : 0;
    }
    deck = is;
    return deck;
}
