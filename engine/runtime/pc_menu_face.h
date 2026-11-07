// The pads' face buttons in menus (hle_hid.cpp menu_face_buttons, controls.menu_buttons). Pure, no Windows:
// tools/engine/test_pc_menu_face.py tests it.
//
// Why (Reddit musical-miller, LEAD 11-07 06:05): the menus' confirm / back are the game's A / B, so they followed Face
// buttons and the Pad buttons rows, the world's layout: a player who moved jump to the printed B moved menu confirm too.
// The row gives the menus their own: by the printed letter (an Xbox / PlayStation pad's A or Cross is the game's A,
// confirm; B or Circle its B, back) or by the Switch's positions (right = A, bottom = B). A Nintendo pad's letters are
// its positions, so only the other pads' face buttons ever move.
// A face button keeps the meaning it was pressed with until released: a B that closes a menu must not land in the
// world, still held, as the game's A (talk, pick up), nor a world press held into a menu become the menu's.
// Bits: the Switch's A 0, B 1, X 2, Y 3 (hle_hid.cpp BtnA..BtnY).
#pragma once
#include <cstdint>

namespace pcmf {

// A <-> B, X <-> Y: an Xbox / PlayStation pad's letters <-> the Switch's positions; the other bits kept
inline uint64_t swap(uint64_t b) {
    return (b & ~0xFull) | (b & 1) << 1 | (b & 2) >> 1 | (b & 4) << 1 | (b & 8) >> 1;
}

// per face button, 0..3 the Nintendo pads' A B X Y, 4..7 the other pads' by position: 0 up, 1 the game's, 2 the menu's
struct Latch { int8_t as[8] = {}; };

// face: the A / B / X / Y bits the pads' face buttons gave, as Face buttons maps them (labels: by label); xface: those
// of them from the other pads. mode: 0 as the game, 1 by the printed letter, 2 by position; menu: a UI context now.
// game: the face bits pressed as the world's (as given: the Pad buttons rows still apply); ui: the game's A / B / X / Y
// for those pressed as the menu's (past the rows).
inline void split(Latch& l, uint64_t face, uint64_t xface, bool labels, int mode, bool menu, uint64_t* game,
                  uint64_t* ui) {
    const uint64_t nin = face & ~xface & 0xF, other = labels ? swap(xface & 0xF) : xface & 0xF;
    *game = *ui = 0;
    for (int i = 0; i < 8; i++) {
        const uint64_t bit = 1ull << (i & 3);
        if (!((i < 4 ? nin : other) & bit)) { l.as[i] = 0; continue; }
        if (!l.as[i]) l.as[i] = mode && menu ? 2 : 1;
        if (i < 4) *(l.as[i] == 2 ? ui : game) |= bit;
        else if (l.as[i] == 2) *ui |= mode == 1 ? swap(bit) : bit;
        else *game |= labels ? swap(bit) : bit;
    }
}

}  // namespace pcmf
