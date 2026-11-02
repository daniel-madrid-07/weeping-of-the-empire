// win_ime.h: the game window without an input method (shim_win.c SDL_CreateWindow). An input method (Chinese, Japanese,
// Korean ...) takes the game's keys while the window has one: Shift switches its mode, letters go into its composition,
// and its candidate window shows over the game (rival parity: totk-pc-recomp #168 / #193). So the window gets no input
// context; the keyboard reaches the game and the F10 menu as on an English layout. imm32 through LoadLibrary (no link
// change). TOTK_IME=1 keeps the input method. Header-only so tools/engine/win_ime_test.c checks it on a hidden window.
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static inline bool win_no_ime(HWND h) {
    const char *e = getenv("TOTK_IME");
    if (e && *e == '1') return false;
    typedef BOOL(WINAPI * AssociateContextEx)(HWND, void *, DWORD);
    HMODULE imm = LoadLibraryW(L"imm32.dll");
    AssociateContextEx f = imm ? (AssociateContextEx)(void *)GetProcAddress(imm, "ImmAssociateContextEx") : NULL;
    if (!f || !f(h, NULL, 0)) return false;
    fprintf(stderr, "[win] input method off for the game window (TOTK_IME=1 keeps it)\n");
    return true;
}
