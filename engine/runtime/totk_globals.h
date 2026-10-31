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
    const u64 var = ld64(totk_main(slot));
    return totk_ptr_ok(var) ? ld64(var) : 0;
}

// The player actor: the global at GOT slot main+0x4562308 holds a shaft::baseproc::BaseProcLinkBase at +0x20, read the
// way main reads it (main+0x18adbb8 -> 0x18adcd4, the link's getter main+0x6fd8c4): the link's id at +0x10 (-1: none),
// its unit at +0x8 whose id (+0x48) must match, the proc at unit +0x40, not while the proc's state (+0x24) is 7..11
// (being deleted). 0: none.
inline u64 totk_player_actor() {
    const u64 root = totk_global(0x4562308);
    if (!totk_ptr_ok(root)) return 0;
    const u64 link = root + 0x20;
    const s32 id = (s32)ld32(link + 0x10);
    const u64 unit = ld64(link + 8);
    if (id == -1 || !totk_ptr_ok(unit) || (s32)ld32(unit + 0x48) != id) return 0;
    const u64 proc = ld64(unit + 0x40);
    if (!totk_ptr_ok(proc)) return 0;
    const u64 vt = ld64(proc);   // a game object: its vtable in main's image
    if (vt < kTotkMain || vt >= kTotkMain + 0x6000000 || ld32(proc + 0x24) - 7 < 5) return 0;
    return proc;
}
