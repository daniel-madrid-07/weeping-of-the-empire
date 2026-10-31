// totk_ability.cpp: select an ability by number (PC controls G2; see totk_ability.h). The sequence is the L wheel's
// own commit, with mgr = *(GameDataMgr **)(main+0x4557b20):
//   1. getStruct(mgr, &s, 0x060890b0)                     the player's ability state
//   2. setStructEnum(mgr, hash[n], &s, 0xe32721fd)        the chosen ability
//   3. setStructBool(mgr, 1, &s, 0x1a1a4e5f)              "changed"
//   4. setBool(mgr, 0, 0x2f03cd3e)
//   5. PouchMgr (*(main+0x455db58)) +0x1e64 |= 4
//   6. setBool(mgr, 0, 0x16fe5c99)
// It runs as a task on eui's thread (totk_ui_post: inside the guest bridge, so on the hook's guest context). The wheel
// offers only unlocked abilities: UIShortCutMainItemScreen::openStartImpl_ (main+0x1ace33c) asks
// game::ui::CheckIsGet(flag) (main+0xb60880: GameData struct 0xade0ef78, bool field `flag`) per ability, and so does this
// before writing. TOTK_ABILITY_UNCHECKED=1 skips that (tests only: it can hand out an ability early).
#include "totk_ability.h"

#include "guest/guest.h"
#include "totk_globals.h"
#include "totk_menu_cursor.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
GUEST_FN(GetStruct, 1000de5ce0, void(u64 mgr, u64 s, u32 hash));
GUEST_FN(SetStructEnum, 1000820bfc, void(u64 mgr, u32 value, u64 s, u32 hash));
GUEST_FN(SetStructBool, 1000755f20, void(u64 mgr, bool value, u64 s, u32 hash));
GUEST_FN(SetBool, 1000a02aec, void(u64 mgr, bool value, u32 hash));
GUEST_FN(CheckIsGet, 1000b60880, bool(u32 flag));   // game::ui::CheckIsGet: the ability is unlocked
// GOT slots of the two globals (totk_globals.h; the wheel: main+0x1acf58c / 0x1acf638)
constexpr u64 kGameDataMgr = 0x4557b20, kPouchMgr = 0x455db58;
// Both tables are indexed by the wheel's ability id, which is the ability here plus one (UIShortCutMainItemScreen::
// updateImpl_ names id 2 Obj_UltraHand... 7 Obj_Camera; id 1 is a special entry): the correction, per ability 1..6
constexpr u32 kAbilityHash[7] = {0, 0xc7c5b66a, 0x5bc8fbbb, 0xee51f844, 0x99f9e6a4, 0x84328ce6, 0x3651e258};
// CheckIsGet's flag per ability, 1..6 (the wheel's, same index)
constexpr u32 kUnlockFlag[7] = {0, 0xdc817e80, 0x8e370020, 0x7925b7d7, 0x4cfd5c91, 0x5d930d12, 0x9297657b};
const char *const kAbilityName[7] = {"none", "Ultrahand", "Fuse", "Ascend", "Recall", "Autobuild", "Camera"};

bool ptr_ok(u64 p) { return totk_ptr_ok(p); }

bool unlocked(int n) {   // eui's thread, inside the bridge
    static const bool unchecked = [] { const char *e = getenv("TOTK_ABILITY_UNCHECKED"); return e && atoi(e) > 0; }();
    return unchecked || CheckIsGet(kUnlockFlag[n]);
}

void apply(void *arg, bool ran) {
    const int n = (int)(intptr_t)arg;
    if (!ran) {
        printf("[ability] %s: the UI did not update in time (loading, a cutscene), dropped\n", kAbilityName[n]);
        return;
    }
    const u64 mgr = totk_global(kGameDataMgr), pouch = totk_global(kPouchMgr);
    static bool shown;
    if (!shown) {
        shown = true;
        printf("[ability] GameDataMgr %#llx, PouchMgr %#llx\n", (unsigned long long)mgr, (unsigned long long)pouch);
    }
    if (!ptr_ok(mgr) || !ptr_ok(pouch)) {
        printf("[ability] %s: no GameDataMgr / PouchMgr yet (%#llx / %#llx), dropped\n", kAbilityName[n],
               (unsigned long long)mgr, (unsigned long long)pouch);
        return;
    }
    if (!unlocked(n)) {
        printf("[ability] %s: not unlocked yet (CheckIsGet %#x), refused\n", kAbilityName[n], kUnlockFlag[n]);
        return;
    }
    // The struct handle lives on the guest stack, below the hook's sp, for the six calls.
    Ctx *const c = guest_bridge::tl_ctx;
    const u64 sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    const u64 s = c->sp;
    for (u64 i = 0; i < 0x40; i += 8) st64(s + i, 0);
    GetStruct(mgr, s, 0x060890b0u);
    SetStructEnum(mgr, kAbilityHash[n], s, 0xe32721fdu);
    SetStructBool(mgr, true, s, 0x1a1a4e5fu);
    SetBool(mgr, false, 0x2f03cd3eu);
    st8(pouch + 0x1e64, ld8(pouch + 0x1e64) | 4);
    SetBool(mgr, false, 0x16fe5c99u);
    c->sp = sp;
    printf("[ability] selected %s (%d)\n", kAbilityName[n], n);
}
}  // namespace

extern "C" bool totk_ability_request(int n) {
    if (n < 1 || n > 6) return false;
    return totk_ui_post(apply, (void *)(intptr_t)n);
}
