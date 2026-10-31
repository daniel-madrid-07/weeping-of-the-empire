// totk_pc_vk.cpp: the hook on the player's virtual-key writer. game::ai::execute::ExecutePlayerCalcKey::updateImpl_
// (main+0x1ccf894, gen.py --hook 1ccf894 in GEN_MAIN) turns the main controller into the player's three u64
// virtual-key masks once a frame. After the original, pc_vk_frame ORs in the PC scheme's keys, gated the way the
// original gates the pad.
//
// What the original does (main+0x1ccf894, read from the image):
// - input off: when the flag-set entry 0xf6e0a4c8 (flag set at *main+0x455aa88, +0x30) is set, all three masks are 0;
// - lockout: while Player+0x10d8 > 2^-23, trig keeps only ZL, LStick, RStick and the D-pad (VK 2, 3, 17, 18, 20-23),
//   hold only ZL and the D-pad (VK 2, 18, 20-23), release nothing;
// - outputs: node +0x28 / +0x38 / +0x48 point at the trig / hold / release u64s; +0x20 / +0x30 / +0x40 at u8 change
//   counters it bumps on every run (so nothing to bump here);
// - the ZL tail: with hold VK 2 and the time scale >= 1 it calls FUN_1ccff34(AttentionReceiver) (the target lock)
//   unless Player+0x28 & 0x200080000 or the verb 0x34 / 0x35 (Scope, TakingPhoto) is on. It reads the pad's hold
//   only, so when the PC keys add VK 2 to a hold the pad left without it, the hook runs that tail itself.
// G1 (feature vk_guard in games/totk/patches): the guard reads VK 27 instead of VK 2; the hook copies the pad's VK 2
// to 27 first thing, so ZL on a controller still guards and locks on, and a PC key can lock on (VK 2) alone. Without
// vk_guard (read from its first site) the hook adds nothing to the pad's masks.
#include "rt.h"
#include "totk_version.h"
#include "totk_globals.h"
#include "totk_input_cuts.h"
#include "totk_pcvk.h"
#include "totk_player_state.h"

#include <cstdio>
#include <cstdlib>

// The whole hook is platform-free (totk_pcvk.cpp is too): on Linux / Deck a key with 1ccf894 in GEN_MAIN gets the same
// pad mirror and gating. Only how a key without the hook links differs, below.
namespace {
constexpr u64 kMain = kTotkMain;
// GOT slots (totk_globals.h: two dereferences, as the original's adrp / ldr / ldr)
constexpr u64 kInputFlags = 0x455aa88;                  // the flag set the original tests for input off (+0x30)
constexpr u32 kInputOffHash = 0xf6e0a4c8u;
constexpr u64 kZlGate = 0x4557b30;                      // +0x58 != 0: the original skips the ZL tail
constexpr u64 kTimeScale = 0x45577c0;                   // +0x88 * +0x108 < 1: the slow-motion path instead
constexpr u64 kVerbOn = totk_main(0x165b4e8);              // (player, verb, flag) -> bool
constexpr u64 kLockTarget = totk_main(0x1ccff34);          // (AttentionReceiver)
constexpr u64 kVkZl = 1ull << 2;
// The guard's own virtual key (games/totk/patches/sites_v100.py VK_GUARD, feature vk_guard): the pad's ZL sets it
// with VK 2, so a controller still guards; a PC key can set VK 2 alone (lock-on, shield down).
constexpr u64 kVkGuard = 1ull << 27;
constexpr u64 kLockoutTrig = 0xF6000Cull, kLockoutHold = 0xF40004ull;
// vk_guard's first site (GUARD_SITES[0]): `mov w1, #27` when the feature is applied, the shipped `mov w1, #2` otherwise
constexpr u64 kGuardSite = totk_main(0x1cfe320);
constexpr u32 kMovW1Guard = 0x52800361u;   // movz w1, #27

// Whether the guard reads VK 27 (vk_guard applied). Without it nothing reads 27, so the hook leaves the masks as the
// original wrote them. main's code is not written after load: read once.
bool guard_patched() {
    static const bool on = [] {
        const u32 w = ld32(kGuardSite);
        printf("[pc-vk] vk_guard %s (main+0x1cfe320 = %#x)\n", w == kMovW1Guard ? "applied: pad ZL mirrored to VK 27"
                                                                               : "not applied: no VK 27 mirror", w);
        return w == kMovW1Guard;
    }();
    return on;
}

bool ptr_ok(u64 p) { return totk_ptr_ok(p); }
bool data_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47); }   // any alignment (blackboard storage)

// Each reason the hook leaves a run alone, logged the first time it happens (h1-first showed no [pc-vk] line at all).
void left_alone(int why, u64 a, u64 b) {
    static bool told[4];
    static const char *const what[4] = {"no node", "the node's mask pointers", "no actor / components", "no player"};
    if (told[why]) return;
    told[why] = true;
    printf("[pc-vk] ExecutePlayerCalcKey left alone: %s (%#llx %#llx)\n", what[why], (unsigned long long)a,
           (unsigned long long)b);
}

// The lookup the game itself does at main+0x1ccf8a8 .. 0x1ccf938: an open-addressing table of u32 hashes, the bit index of each
// in a byte array, the bits in the set's words. 1 set, 0 clear, -1 not in the set (or the set unreadable).
int cut_reason(u32 hash, int *bit_out) {
    const u64 base = totk_global(kInputFlags);
    if (!ptr_ok(base)) return -1;
    const u64 set = base + 0x30, tab = ld64(set + 0x10);
    if (!ptr_ok(tab)) return -1;
    const u32 n = ld32(tab + 0x14);
    const u64 keys = ld64(tab);
    if (!n || !ptr_ok(keys)) return -1;
    const u32 start = hash % n;
    u32 i = start;
    for (;;) {
        const u32 k = ld32(keys + 4ull * i);
        if (k == hash) break;
        if (!k) return -1;
        i = i + 1 >= n ? 0 : i + 1;
        if (i == start) return -1;
    }
    const u64 idx = ld64(tab + 8);
    if (!ptr_ok(idx)) return -1;
    const u32 bit = ld8(idx + i);
    *bit_out = (int)bit;
    return (ld32(set + ((bit >> 3) & 0x1c)) >> (bit & 31)) & 1;
}
bool input_off(int *bit_out) { return cut_reason(kInputOffHash, bit_out) == 1; }


u64 call(Ctx *c, u64 fn, u64 a0, u64 a1 = 0, u64 a2 = 0) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    call_ind(c, fn);
    return c->x[0];
}

// What the game does after its ZL test (main+0x1ccfbb4 .. 0x1ccfc14, 0x1ccfde0 .. 0x1ccfe20), for a VK 2 hold that
// only the PC keys give.
void zl_tail(Ctx *c, u64 comps) {
    const u64 gate = totk_global(kZlGate);
    if (ptr_ok(gate) && ld64(gate + 0x58)) return;
    const u64 att = ld64(comps + 0x128), player = ld64(comps + 0x3a8);
    if (!ptr_ok(att) || !ptr_ok(player)) return;
    const u64 ts = totk_global(kTimeScale);
    if (!ptr_ok(ts) || ldf32(ts + 0x88) * ldf32(ts + 0x108) < 1.0f) return;
    if (ld64(player + 0x28) & 0x200080000ull) return;
    if (call(c, kVerbOn, player, 0x34, 1) & 1) return;
    if (call(c, kVerbOn, player, 0x35, 0) & 1) return;
    call(c, kLockTarget, att);
}
}  // namespace

// the pc_ctx reads the pause manager's reasons through this, on its input thread under its fault guard (the one
// reader: it works in pause, where the player's node stops)
extern "C" int totk_input_reason(uint32_t hash) {
    int bit;
    return cut_reason(hash, &bit);
}

// A key without the hook never calls hook_f_1ccf894 and has no f_1ccf894_orig: on Windows the linker then takes
// pcvk_orig_stub (/alternatename), elsewhere the weak reference stays null, so one runtime links against keys with and
// without it (as totk_menu_cursor.cpp does).
#ifdef _WIN32
void f_1ccf894_orig(Ctx *__restrict c);
void pcvk_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_1ccf894_orig@@YAXPEIAUCtx@@@Z=?pcvk_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_1ccf894_orig(Ctx *__restrict c);
#endif
void hook_f_1ccf894(Ctx *__restrict c) {   // ExecutePlayerCalcKey::updateImpl_(this)
    const u64 node = c->x[0];
#ifndef _WIN32
    if (!f_1ccf894_orig) return;
#endif
    f_1ccf894_orig(c);
    // TOTK_PCVK=orig runs the original alone, as a key without the hook (on a key with vk_guard the pad's ZL then no
    // longer guards: nothing sets VK 27).
    static const int mode = [] {
        const char *e = getenv("TOTK_PCVK");
        const int m = !e ? 2 : !strcmp(e, "orig") ? 0 : e[0] == '0' ? 1 : 2;
        if (m < 2) printf("[pc-vk] TOTK_PCVK=%s: hook_f_1ccf894 runs %s\n", e,
                          m ? "the original and the vk_guard mirror only" : "the original only");
        return m;
    }();
    if (!mode) return;
    const u64 ret = c->x[0];
    static bool entered;
    if (!entered) {
        entered = true;
        printf("[pc-vk] hook_f_1ccf894 runs (node %#llx)\n", (unsigned long long)node);
    }
    if (!ptr_ok(node)) return left_alone(0, node, 0);
    const u64 trig = ld64(node + 0x28), hold = ld64(node + 0x38), rel = ld64(node + 0x48);
    if (!data_ok(trig) || !data_ok(hold) || !data_ok(rel)) return left_alone(1, trig, hold);
    // With vk_guard the pad's VK 2 also means guard: mirrored to VK 27 in all three masks, before anything else can
    // return (a controller must guard whatever the PC side does).
    const u64 guard = guard_patched() ? kVkGuard : 0;
    if (guard)
        for (const u64 m : {trig, hold, rel})
            if (ld64(m) & kVkZl) st64(m, ld64(m) | kVkGuard);
    if (mode == 1) return;
    const u64 pad_h = ld64(hold);   // the pad's hold, for the ZL tail below
    const u64 a = ld64(node + 8), b = ptr_ok(a) ? ld64(a + 8) : 0, actor = ptr_ok(b) ? ld64(b + 0x1f8) : 0;
    const u64 comps = ptr_ok(actor) ? ld64(actor + 0x220) : 0;
    if (!ptr_ok(comps)) return left_alone(2, actor, comps);
    // Only the player's node (D1: the player actor is the proc of the link at the global main+0x4562308 + 0x20;
    // totk_globals.h, the same reader as totk_player_state.cpp).
    const u64 player_actor = totk_player_actor();
    if (!player_actor) return left_alone(3, actor, totk_global(0x4562308));
    if (actor != player_actor) {
        static bool told;
        if (player_actor && !told) {
            told = true;
            printf("[pc-vk] ExecutePlayerCalcKey on actor %#llx, not the player (%#llx): left alone\n",
                   (unsigned long long)actor, (unsigned long long)player_actor);
        }
        return;
    }
    static bool alive;
    if (!alive) {
        alive = true;
        printf("[pc-vk] the hook on ExecutePlayerCalcKey is alive (node %#llx, player actor %#llx; globals: input "
               "flags %#llx, player root %#llx, ZL gate %#llx, time scale %#llx)\n", (unsigned long long)node,
               (unsigned long long)actor, (unsigned long long)totk_global(kInputFlags),
               (unsigned long long)totk_global(0x4562308),
               (unsigned long long)totk_global(kZlGate), (unsigned long long)totk_global(kTimeScale));
    }
    u64 kt = ~0ull, kh = ~0ull, kr = ~0ull;
    int bit = -1;
    const bool off = input_off(&bit);
    // The flag test is the game's own lookup (cut_reason above): logged on change,
    // with the bit the probe found (the set's words sit below its table, so it must be < 128).
    static int was = -1;
    if (off != was) {
        was = off;
        printf("[pc-vk] input %s (flag 0xf6e0a4c8 at bit %d)\n", off ? "off" : "on", bit);
    }
    bool lockout = false;
    if (off) kt = kh = kr = 0;
    else {
        const u64 player = ld64(comps + 0x3a8);
        lockout = ptr_ok(player) && ldf32(player + 0x10d8) > bitsf(0x34000000u);
        if (lockout) kt = kLockoutTrig | guard, kh = kLockoutHold | guard, kr = 0;
    }
    static bool was_lockout;
    if (lockout != was_lockout) {
        was_lockout = lockout;
        printf("[pc-vk] lockout %s\n", lockout ? "on (PC keys: ZL, crouch, R-stick, D-pad only)" : "off");
    }
    pc_vk_frame((uint64_t *)trig, (uint64_t *)hold, (uint64_t *)rel, kt, kh, kr);
    totk_player_game_calls(c);  // game calls on this thread: the steering stick's state test 23,
                                 // then the queued requests, e.g. the bow's take-out (totk_player_state.cpp)
    if ((ld64(hold) & kVkZl) && !(pad_h & kVkZl)) zl_tail(c, comps);
    // TOTK_VK_DIAG=1: on each change, the node's hold bits 2 / 27 next to what the game's own reader
    // (Player::getVirtualKeyHoldValue, main+0x1ccadcc) returns for 2 and 27, and the action verb
    static const bool diag = [] { const char *e = getenv("TOTK_VK_DIAG"); return e && atoi(e) > 0; }();
    if (diag) {
        const u64 player = ld64(comps + 0x3a8);
        if (ptr_ok(player)) {
            const u64 h = ld64(hold);
            const u64 g2 = call(c, totk_main(0x1ccadcc), player, 2) & 1, g27 = call(c, totk_main(0x1ccadcc), player, 27) & 1;
            const u64 key = (h & (kVkZl | kVkGuard)) | g2 << 40 | g27 << 41;
            static u64 last = ~0ull;
            if (key != last) {
                last = key;
                printf("[pc-vk] diag: node hold 2:%d 27:%d (mask %#llx), the game's reader 2:%d 27:%d\n",
                       (int)((h >> 2) & 1), (int)((h >> 27) & 1), (unsigned long long)h, (int)g2, (int)g27);
            }
        }
    }
    c->x[0] = ret;
}
