// totk_globals.h: TotK's global pointers as main's code reads them. main reaches a global through its GOT: `adrp x8,
// slot@page; ldr x8, [x8, slot@off]` gives the variable's address, `ldr x8, [x8]` its value (e.g. GameDataMgr at
// main+0x1acf594 / ExecutePlayerCalcKey at main+0x1ccfbb8). The `*(T **)0x100455xxxx` names the variable and leaves
// the GOT step out, so a slot is dereferenced twice.
#pragma once
#include "rt.h"
#include "totk_version.h"

constexpr u64 kTotkMain = 0x7100000000ull;   // main's base in the engine (gen.py IMGBASE)

inline bool totk_ptr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47) && !(p & 7); }

// The value of the global whose GOT slot is at main+slot (0 when the slot or the variable is not mapped yet).
inline u64 totk_global(u64 slot) {
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
    if (!totk_main_known_rt(slot)) return 0;   // (another version: the slot has no place; logged once)
#else
    if (!totk_main_known(slot)) return 0;   // (a diagnostic build of another version: the slot has no place)
#endif
    const u64 var = ld64(totk_main(slot));
    return totk_ptr_ok(var) ? ld64(var) : 0;
}

// The player actor: the global at GOT slot main+0x4562308 holds a shaft::baseproc::BaseProcLinkBase at +0x20, read the
// way main reads it (main+0x18adbb8 -> 0x18adcd4, the link's getter main+0x6fd8c4): the link's id at +0x10 (-1: none),
// its unit at +0x8 whose id (+0x48) must match, the proc at unit +0x40, not while the proc's state (+0x24) is 7..11
// (being deleted). 0: none. The fields through totk_field (totk_version.h): a diagnostic build of another version with
// one unplaced finds no player.
inline u64 totk_player_actor() {
    if (!(totk_field_known(PlayerRoot, 0x20) && totk_field_known(BaseProcLink, 0x8) &&
          totk_field_known(BaseProcLink, 0x10) && totk_field_known(BaseProcUnit, 0x40) &&
          totk_field_known(BaseProcUnit, 0x48) && totk_field_known(BaseProc, 0x24)))
        return 0;
    const u64 root = totk_global(0x4562308);
    if (!totk_ptr_ok(root)) return 0;
    const u64 link = root + totk_field(PlayerRoot, 0x20);
    const s32 id = (s32)ld32(link + totk_field(BaseProcLink, 0x10));
    const u64 unit = ld64(link + totk_field(BaseProcLink, 0x8));
    if (id == -1 || !totk_ptr_ok(unit) || (s32)ld32(unit + totk_field(BaseProcUnit, 0x48)) != id) return 0;
    const u64 proc = ld64(unit + totk_field(BaseProcUnit, 0x40));
    if (!totk_ptr_ok(proc)) return 0;
    const u64 vt = ld64(proc);   // a game object: its vtable in main's image
    if (vt < kTotkMain || vt >= kTotkMain + 0x6000000 || ld32(proc + totk_field(BaseProc, 0x24)) - 7 < 5) return 0;
    return proc;
}

// The frame-delta setter's object (VFRMgr; totk_pc_vk.cpp's TimeScale): the global at GOT slot main+0x45577c0. Its
// update stores the frame's 30 Hz ratio r (1.0 at 30 fps, 0.5 at 60) that the game-time hooks scale by: 1.0.0
// main+0x7a83e4 at +8 (ratio, seconds: `stp s0, s1, [x0, #8]`), 1.4.3 main+0x3e67f0 at +0x10 (ratio, 1 / ratio,
// seconds, fps). totk_frame_delta() is 0 (each hook's original alone) when the object or the ratio's offset is not
// known in this build.
inline u64 totk_frame_delta() { return totk_field_known(TimeScale, 0x8) ? totk_global(0x45577c0) : 0; }
inline float totk_frame_ratio(u64 fd) { return ldf32(fd + totk_field(TimeScale, 0x8)); }
