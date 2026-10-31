// totk_player_state.cpp: what the player is doing (climbing, gliding, swimming, aiming, riding...), for the gameplay
// contexts of the PC controls. On foot Shift sprints; climbing, B (Shift's pad button) lets go, so the router must
// know the player state before it routes a key.
//
// The game's own test is FUN_10012af610(actor, EventQueryPlayerState), which the event flowcharts call. Most of its
// cases compare the player's current action verb; this file reads the verbs the same way and calls no guest code:
//  - the player: totk_globals.h's totk_player_actor() (the global behind GOT slot main+0x4562308, + 0x20, is a
//    shaft::baseproc::BaseProcLinkBase whose proc is the player while the ids match).
//  - the actor's component array (+0x220); its entry +0x158 is the AS component, whose 5 slots (+0x1f0, one pointer
//    each) hold the slot's current verb at +0x28, or at +0x4c when +0x28 is 284 (Invalid): Player::getActionVerb
//    (main+0x1cced1c).
//  - enum game::bsa::ActionVerb.
//  - the ability: the component array's SpecialPower component (+0x478: GameActor::getComponent<SpecialPower>,
//    main+0x1c019f8), its mCurrentSpecialPower (+0x6c, enum game::specialpower::SpecialPowerType).
// The verb sets below are first readings of the enum, checked in boots (TOTK_PLAYER_LOG=1 logs each verb change).
// controls: none; TOTK_PLAYER_LOG=1 (tests) logs every verb change, otherwise only the flag changes (400 lines).
#include "rt.h"
#include "totk_version.h"
#include "totk_player_state.h"
#include "pc_input.h"
#include "totk_globals.h"
#include "pc.h"
#include "totk_pcvk.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::atomic<uint32_t> g_state{0};
std::atomic<int> g_verb[5] = {{-1}, {-1}, {-1}, {-1}, {-1}};
std::atomic<int> g_request{0};   // an ability to select at the next pad read (0 none)
// take out the bow at the next hook frame (totk_player_request_bow): the request's GetTickCount64, 0 none
std::atomic<unsigned long long> g_bow_request{0};
std::atomic<bool> g_stick{false};   // holding a steering stick (totk_player_stick_frame)
std::atomic<int> g_hand{TPH_NONE};   // the weapon class last seen in hand (totk_player_hand)

#ifdef _WIN32
constexpr u64 kMain = 0x7100000000ull;           // main's base in the engine (gen.py IMGBASE)
constexpr u64 kEventMgrSlot = 0x4557b30;   // GOT slot of EventMgr*; +0x58 mpMainEvent != 0 while an event runs (D1)
// GameDataMgr (its GOT slot), its setBool, and the bool the bow strip sets to take the bow out (totk_player_game_calls)
constexpr u64 kGameDataMgrSlot = 0x4557b20, kGameDataSetBool = 0xa02aec;
constexpr u32 kGdBowTakeOut = 0x2f03cd3e;
// the actor's component array and the components read here
constexpr u64 kActorComponents = 0x220, kCompAttention = 0x128, kCompAS = 0x158, kCompPlayer = 0x3a8,
              kCompSpecialPower = 0x478;
constexpr u64 kASSlots = 0x1f0, kSlotVerb = 0x28, kSlotVerbAlt = 0x4c;   // BSA: 5 slots, each one's verb
constexpr u64 kPlStateBytes = 0x1424;                                    // Player
// The Player's move rate as the game reads it for the running attack (main+0x1076b40: +0xf24, a float, clamped to
// 0..0.5; when it is ~0, 0.5 if +0xad8 bit 0 else 0); the walk scheduler's DashAttack test (main+0x16e9130) wants
// >= 0.25 with slot-0 verb Walk (1), the weapon sheathed and a VK 9 trigger: the draw-slash
constexpr u64 kPlMoveRate = 0xf24, kPlMoveFlags = 0xad8;
constexpr int kVerbWalk = 1;
constexpr u64 kAttState = 0xe8;                                          // AttentionReceiver
constexpr u64 kSpRequested = 0x68, kSpCurrent = 0x6c, kSpUltraHand = 0x28;   // SpecialPower
constexpr u64 kUhState = 0x18;                                           // UltraHand
// What is in the hands: the game's own test, the state test's WeaponDrawn (27) = Equipment::isSlotValidActor
// (main+0x8f4c60) on DynamicEquipmentSlot 0 Weapon, 3 Bow, 4 Shield. The component array's EquipmentUser (+0x230)
// links each slot's equipment actor (mDynamicEquipmentLinks +0x20, 8 ActorLinks of 0x18: +8 the link data, +0x10 the
// proc id; the data's +0x48 id, +0x40 proc, the proc's +0x24 state, 7-11 going away). That actor's DynamicEquipment
// (its component array +0x208) keeps mStateController (+0x3d0): mFlags bit 0 set -> mEquipmentState (+0x3d4) 1
// EquipedDrawn, 2 / 4 sheathed (isSheathed main+0x697a9c, getEquipState main+0x8b738c); bit 0 clear counts as
// sheathed. EquipmentUser +0x4610 + slot, a byte per slot, overrides it: bit 1 drawn, bit 2 sheathed. On the game
// thread (a calc thread) the game reads all this with no lock.
constexpr u64 kCompEquipmentUser = 0x230, kCompDynamicEquipment = 0x208;
constexpr u64 kEuLinks = 0x20, kEuLinkSize = 0x18, kEuSlotForce = 0x4610;
constexpr u64 kLinkData = 0x8, kLinkProcId = 0x10, kLinkDataProc = 0x40, kLinkDataId = 0x48, kProcState = 0x24;
constexpr u64 kDeStateFlags = 0x3d0, kDeState = 0x3d4;
constexpr int kSlotWeapon = 0, kSlotBow = 3, kSlotShield = 4;   // enum game::equipment::DynamicEquipmentSlot
// a slot's state: none (no live equipment actor), drawn, sheathed, or kHandRaw | another EquipState (0 too: the
// actor is there, so it is equipped; logged raw)
constexpr u8 kHandNone = 0, kHandDrawn = 1, kHandSheathed = 2, kHandRaw = 0x80;
// (logged only, a lead for the class LMB uses: does either keep "bow" after the bow strip's cancel?)
// EquipmentUser.mEquippedDrawnWeaponType (+0x3ebc) and mAttachReadySlot (+0x3ee4, a DynamicEquipmentSlot)
constexpr u64 kEuDrawnType = 0x3ebc, kEuReadySlot = 0x3ee4;
constexpr int kVerbInvalid = 284;
// the SHIELD SURF's refused latch (Player::preCalc clears it once either key is up). Not the parry
constexpr u64 kParryWord = 0x28;
// the active slow motion
constexpr u64 kSpStepType = 0x12e8;
constexpr u32 kSpStepNone = 0x11, kSpStepGuardJust = 1;
constexpr int kParryBit = 57;
// The steering stick. Holding one shows only as verb 48 Manipulate, which other held devices share, and no Player
// field says which: the game's own test, EventQueryPlayerState 23 RideControlStick, takes the player's ride link
// (component array +0x410), checks it is live (FUN_1000bc0e1c: +0x38 == 1, id +0x30 != -1) and asks the linked actor's
// link entry (+0x3c) for its kind, 5 = control stick (FUN_1000654514 -> FUN_100081df10: per-actor parameter data, game
// code only). So the test itself is called: on the game thread only (ExecutePlayerCalcKey's hook, in the player's own
// update), gated on slot-0 verb 48 and a live link, and asked again only when the link changes. There it takes no lock
// (the link's mutex +0x28 is taken only off a BaseProcMgr thread, FUN_10006fd914) and the reference it takes on the
// linked actor (ActorLinkBase::getReference) it drops before it returns.
constexpr u64 kCompRideLink = 0x410;
constexpr u64 kLinkId = 0x30, kLinkLive = 0x38, kLinkSlot = 0x3c;
constexpr u64 kQueryPlayerState = 0x12af610;   // FUN_10012af610(actor, EventQueryPlayerState) -> bool
constexpr u32 kQueryRideControlStick = 23;
constexpr int kVerbManipulate = 48;

template <class T> T rd(u64 a) {
    T v;
    memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), sizeof v);
    return v;
}
template <class T> void wr(u64 a, T v) { memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, sizeof v); }
bool ptr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0; }
// An object of the game: its vtable in main's image (keeps a stale or garbage pointer from being followed further)
bool game_object(u64 p) {
    if (!ptr_ok(p)) return false;
    const u64 vt = rd<u64>(p);
    return vt >= kMain && vt < kMain + 0x6000000;
}

// (main's globals are reached through its GOT: totk_globals.h reads the slot, then the variable)
u64 player_actor() { return totk_player_actor(); }

// enum game::bsa::ActionVerb, grouped by the context they mean for the PC keys
bool any_of(int v, std::initializer_list<int> set) {
    for (int s : set) if (v == s) return true;
    return false;
}
uint32_t flags_of(const int (&v)[5]) {
    uint32_t f = TPS_VALID;
    const int main = v[0];
    // slot 0 (the one the game's state test reads): the body's movement
    if (any_of(main, {70 /*Climb*/, 71 /*ClimbLadder*/, 96 /*WaterfallClimbing*/, 68 /*Clamber*/, 69 /*DashUp*/,
                      14 /*WallSlip*/, 15 /*WallJump*/}))
        f |= TPS_CLIMB;
    if (any_of(main, {94 /*ParasailGlide*/, 95 /*AnimalGlide*/, 56 /*HangingFlight*/}))
        f |= TPS_GLIDE;
    // 5 (Glide) is the skydive's head-first dive, not the paraglider, C (VK 14) in a skydive (33) gave 5, then the
    // landing (175) half a second later with most hearts lost, no glider (window_00855 / 00945)
    if (any_of(main, {13 /*Jump*/, 33 /*Fall*/, 5 /*Glide*/, 19 /*AirDive*/, 20 /*GuideDive*/, 16 /*JumpToCeiling*/,
                      17 /*JumpOffCeiling*/, 259 /*FallAttack*/, 131 /*JumpAttack*/, 173 /*InterruptFall*/}))
        f |= TPS_AIR;
    if (any_of(main, {97 /*Swim*/, 18 /*WaterDive*/, 191 /*WaterJump*/})) f |= TPS_SWIM;
    if (any_of(main, {24 /*Ride*/, 26 /*RideDragon*/, 27 /*RideJump*/, 132 /*RideJumpAttack*/})) f |= TPS_HORSE;
    if (any_of(main, {25 /*RideRobot*/, 199 /*RideSled*/, 257 /*RideTelescopeBase*/})) f |= TPS_VEHICLE;
    if (main == 30 /*Surf*/) f |= TPS_SURF;
    // any slot: the arms and the upper body
    for (int s : v) {
        if (s == 205 /*DrawnBow*/ || s == 59 /*UseBow*/) f |= TPS_AIM;
        if (s == 248 /*GuardKeep*/) f |= TPS_GUARD;
        if (any_of(s, {65 /*LockOnWait*/, 66 /*LockOnMove*/, 209 /*LockOnFrontMove*/, 210 /*LockOnBackMove*/}))
            f |= TPS_LOCKON;
        if (s == 61 /*UseSpecialPower*/) f |= TPS_ABILITY;
        if (s == 88 /*Talk*/ || s == 89 /*Listen*/) f |= TPS_TALK;
        if (s == 53 /*TakingPhoto*/ || s == 52 /*Scope*/) f |= TPS_CAMERA;
    }
    return f;
}

bool verbose() {
    static const bool on = getenv("TOTK_PLAYER_LOG") && strcmp(getenv("TOTK_PLAYER_LOG"), "0") != 0;
    return on;
}

// One walk of the chain. The pad reads run on up to 4 threads while the game thread may be destroying the actor (a
// load), so a stale pointer can lead anywhere: the walk runs under SEH and an access fault reads as "no player".
// pc_fault_guard lets the fault reach this __except: on_crash's vectored handler would end the process first. Each
// field is one aligned load. req: an ability to write.
struct Sample { int v[5]; u32 power; u8 pl[2]; bool ok, wrote, locked, event, holding; u64 parry_word; u32 slowmo;
                u8 hand[3]; s32 eu_type, eu_ready; float move_rate; };   // hand: the weapon, bow, shield slot (kHand*)
// One slot of the EquipmentUser: getEquipState's answer, with isSlotValidActor's / isSheathed's overrides
u8 hand_state(u64 eu, int slot) {
    const u64 link = eu + kEuLinks + kEuLinkSize * slot;
    const s32 id = rd<s32>(link + kLinkProcId);
    if (id == -1) return kHandNone;
    const u64 data = rd<u64>(link + kLinkData);
    if (!ptr_ok(data) || rd<s32>(data + kLinkDataId) != id) return kHandNone;
    const u64 proc = rd<u64>(data + kLinkDataProc);
    if (!game_object(proc) || (u32)(rd<s32>(proc + kProcState) - 7) < 5) return kHandNone;
    const u8 force = rd<u8>(eu + kEuSlotForce + slot);
    if (force >> 1 & 1) return kHandDrawn;
    if (force >> 2 & 1) return kHandSheathed;
    const u64 comps = rd<u64>(proc + kActorComponents);
    const u64 de = ptr_ok(comps) ? rd<u64>(comps + kCompDynamicEquipment) : 0;
    if (!game_object(de)) return kHandRaw | 0x7f;   // (equipped, no state to read)
    if (!(rd<u8>(de + kDeStateFlags) & 1)) return kHandSheathed;
    const s32 st = rd<s32>(de + kDeState);
    if (st == 1) return kHandDrawn;
    if (st == 2 || st == 4) return kHandSheathed;
    return kHandRaw | (u8)(st >= 0 && st < 0x7f ? st : 0x7e);
}
bool sample_walk(Sample* s, int req) {
    __try {
        // an event (talk, cutscene): ExecutePlayerCalcKey's own test for cutting the player's input, read with or
        // without a player
        const u64 em = totk_global(kEventMgrSlot);
        s->event = ptr_ok(em) && rd<u64>(em + 0x58) != 0;
        const u64 actor = player_actor();
        const u64 comps = actor ? rd<u64>(actor + kActorComponents) : 0;
        const u64 as = ptr_ok(comps) ? rd<u64>(comps + kCompAS) : 0;
        if (!game_object(as)) {   // (no player: ok stays false, the event test stands)
            if (verbose()) {   // tests: where the walk stops, every 2 s
                static unsigned long long last;
                const unsigned long long now = GetTickCount64();
                if (now - last > 2000) {
                    last = now;
                    // totk_globals.h's walk: the global's link (+0x20): id (+0x10), unit (+0x8, its id +0x48), proc
                    const u64 root = totk_global(0x4562308), link = root + 0x20;
                    const u64 unit = ptr_ok(root) ? rd<u64>(link + 8) : 0;
                    const u64 proc = ptr_ok(unit) ? rd<u64>(unit + 0x40) : 0;
                    printf("[player] no player: root %#llx id %d unit %#llx unit id %d proc %#llx vt %#llx state %u; "
                           "actor %#llx comps %#llx as %#llx as vt %#llx\n",
                           (unsigned long long)root, ptr_ok(root) ? rd<s32>(link + 0x10) : 0, (unsigned long long)unit,
                           ptr_ok(unit) ? rd<s32>(unit + 0x48) : 0, (unsigned long long)proc,
                           ptr_ok(proc) ? (unsigned long long)rd<u64>(proc) : 0ull,
                           ptr_ok(proc) ? rd<u32>(proc + 0x24) : 0u, (unsigned long long)actor,
                           (unsigned long long)comps, (unsigned long long)as,
                           ptr_ok(as) ? (unsigned long long)rd<u64>(as) : 0ull);
                }
            }
            return true;
        }
        for (int i = 0; i < 5; i++) {
            const u64 slot = rd<u64>(as + kASSlots + 8 * i);
            if (!ptr_ok(slot)) continue;
            int verb = rd<s32>(slot + kSlotVerb);
            if (verb == kVerbInvalid) verb = rd<s32>(slot + kSlotVerbAlt);
            s->v[i] = verb >= 0 && verb <= kVerbInvalid ? verb : -1;
        }
        // the Player component's state bytes the game's own test reads too (+0x1424 bit 4 swimming, +0x1425 bit 1
        // climbing), for the states no verb shows
        const u64 pl = rd<u64>(comps + kCompPlayer);
        if (game_object(pl)) { s->pl[0] = rd<u8>(pl + kPlStateBytes); s->pl[1] = rd<u8>(pl + kPlStateBytes + 1); }
        if (game_object(pl)) {   // the move rate, main+0x1076b40's arithmetic
            const float r = rd<float>(pl + kPlMoveRate);
            s->move_rate = r > 1.1920929e-07f ? (r < 0.5f ? r : 0.5f) : ((rd<u8>(pl + kPlMoveFlags) & 1) ? 0.5f : 0.f);
        }
        if (verbose() && game_object(pl)) {   // (tests only)
            s->parry_word = rd<u64>(pl + kParryWord);
            s->slowmo = rd<u32>(pl + kSpStepType);
        }
        // locked on: the attention client's state 2 or 3
        const u64 att = rd<u64>(comps + kCompAttention);
        if (game_object(att)) { const s32 st = rd<s32>(att + kAttState); s->locked = st == 2 || st == 3; }
        const u64 sp = rd<u64>(comps + kCompSpecialPower);
        s->power = game_object(sp) ? rd<u32>(sp + kSpCurrent) : 0;
        const u64 eu = rd<u64>(comps + kCompEquipmentUser);
        if (game_object(eu)) {
            s->hand[0] = hand_state(eu, kSlotWeapon);
            s->hand[1] = hand_state(eu, kSlotBow);
            s->hand[2] = hand_state(eu, kSlotShield);
            s->eu_type = rd<s32>(eu + kEuDrawnType);
            s->eu_ready = rd<s32>(eu + kEuReadySlot);
        }
        if (req && game_object(sp)) { wr<u32>(sp + kSpRequested, (u32)req); s->wrote = true; }
        // Ultrahand holding something: its mState 4 Grabbing / 5 Grabbed
        if (s->power == TPA_ULTRAHAND) {
            const u64 uh = rd<u64>(sp + kSpUltraHand);
            if (game_object(uh)) { const s32 st = rd<s32>(uh + kUhState); s->holding = st == 4 || st == 5; }
        }
        s->ok = true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#ifdef TOTK_PLAYER_STATE_TEST
}  // namespace
// tools/engine/test_pc_router.py: the walk replaced by scripted verbs and the stick test's answer (no game memory)
extern "C" bool totk_player_state_test_sample(int v[5], bool* stick);
namespace {
#endif
bool sample(Sample* s, int req) {
#ifdef TOTK_PLAYER_STATE_TEST
    (void)req;
    bool stick = false;
    s->ok = totk_player_state_test_sample(s->v, &stick);
    g_stick.store(stick, std::memory_order_release);
    return s->ok;
#endif
    pc_fault_guard(1);
    const bool ok = sample_walk(s, req);
    pc_fault_guard(0);
    return ok;
}

// The ride link's identity, plain loads under SEH (the walk's rules): false without a live link.
struct RideLink { u64 actor; s32 id; u32 slot; };
bool ride_link_walk(RideLink* r) {
    __try {
        const u64 actor = player_actor();
        const u64 comps = actor ? rd<u64>(actor + kActorComponents) : 0;
        const u64 link = ptr_ok(comps) ? rd<u64>(comps + kCompRideLink) : 0;
        if (!ptr_ok(link) || rd<u32>(link + kLinkLive) != 1) return false;
        const s32 id = rd<s32>(link + kLinkId);
        if (id == -1) return false;
        *r = RideLink{actor, id, rd<u32>(link + kLinkSlot)};
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
bool ride_link(RideLink* r) {
    pc_fault_guard(1);
    const bool ok = ride_link_walk(r);
    pc_fault_guard(0);
    return ok;
}

// Mineru's construct: an attachable Zonai device in reach (the construct AI's IsFoundAttachObj = a DoAction client
// whose actor has affordance 5), 1 / 0, or -1 when the construct cannot be reached. The ridden actor from the ride
// link: data +0x28, id +0x30, the data's id (+0x48) must match, its proc (+0x40). Then its AttentionReceiver
// (components +0x128): active (state +0xe8 2 or 3) with clients (+0x58), the best client entry (**(+0x60), its +0x20
// bit 0), the client actor (+0x8), its affordance mask (+0x5a0) bit 5. No lock (the game takes +0x78 for this read):
// every pointer checked, a torn read is one frame's 'no'
constexpr u64 kRideLinkData = 0x28, kRecvState = 0xe8, kRecvClients = 0x58, kRecvBest = 0x60, kClientEntryFlags = 0x20,
              kClientEntryActor = 0x8, kActorAffordance = 0x5a0;
int construct_reach_walk() {
    __try {
        const u64 actor = player_actor();
        const u64 comps = actor ? rd<u64>(actor + kActorComponents) : 0;
        const u64 link = ptr_ok(comps) ? rd<u64>(comps + kCompRideLink) : 0;
        if (!ptr_ok(link) || rd<u32>(link + kLinkLive) != 1) return -1;
        const s32 id = rd<s32>(link + kLinkId);
        const u64 data = rd<u64>(link + kRideLinkData);
        if (id == -1 || !ptr_ok(data) || rd<s32>(data + kLinkDataId) != id) return -1;
        const u64 golem = rd<u64>(data + kLinkDataProc);
        // (and not dying / being deleted, proc state 7..11, as the game's own isRidingRaumiGolem 0x10008ce504 rejects,
        // which confirms this link layout)
        if (!game_object(golem) || (u32)(rd<s32>(golem + kProcState) - 7) < 5) return -1;
        const u64 gcomps = rd<u64>(golem + kActorComponents);
        const u64 recv = ptr_ok(gcomps) ? rd<u64>(gcomps + kCompAttention) : 0;
        if (!game_object(recv)) return 0;
        if ((rd<u32>(recv + kRecvState) & ~1u) != 2 || rd<u32>(recv + kRecvClients) == 0) return 0;
        const u64 head = rd<u64>(recv + kRecvBest);
        const u64 e = ptr_ok(head) ? rd<u64>(head) : 0;
        if (!ptr_ok(e) || !(rd<u16>(e + kClientEntryFlags) & 1)) return 0;
        const u64 client = rd<u64>(e + kClientEntryActor);
        if (!game_object(client)) return 0;
        return (rd<u32>(client + kActorAffordance) >> 5) & 1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

void read_player() {
    Sample s = {{-1, -1, -1, -1, -1}, 0, {0, 0}, false, false, false, false, false, 0, kSpStepNone, {0, 0, 0}, -1, -1, 0.f};
    uint32_t f = 0;
    const int req = g_request.exchange(0);
    if (!sample(&s, req))
        s = Sample{{-1, -1, -1, -1, -1}, 0, {0, 0}, false, false, false, false, false, 0, kSpStepNone, {0, 0, 0}, -1, -1, 0.f};
    if (s.event) f |= TPS_EVENT;
    const int (&v)[5] = s.v;
    if (s.ok) {
        f |= flags_of(s.v);
        if (s.pl[0] >> 4 & 1) f |= TPS_SWIM;
        if (s.pl[1] >> 1 & 1) f |= TPS_CLIMB;
        if (s.locked) f |= TPS_LOCKON;
        if (s.holding) f |= TPS_HOLDING;
        // holding a steering stick: the game's state test 23 (only verb 48 shows otherwise, which other held devices
        // share; rc_veh2): VEHICLE's rows then apply (WASD steer, E lets go)
        if (g_stick.load(std::memory_order_acquire)) f |= TPS_VEHICLE;
        if (s.hand[0] == kHandDrawn) f |= TPS_WEAPON_OUT;
        if (s.hand[1] == kHandDrawn) f |= TPS_BOW_OUT;
        if (s.hand[2] == kHandDrawn) f |= TPS_SHIELD_OUT;
        if (s.hand[0] != kHandNone) f |= TPS_WEAPON_EQUIPPED;
        if (s.hand[1] != kHandNone) f |= TPS_BOW_EQUIPPED;
        if (s.hand[2] != kHandNone) f |= TPS_SHIELD_EQUIPPED;
        if (v[0] == kVerbWalk && s.move_rate >= 0.25f) f |= TPS_MOVING;
        if (verbose()) {   // the hands' slots changing (the raw state when it is neither drawn nor sheathed)
            static u8 was_hand[3] = {0xfe, 0xfe, 0xfe};
            static s32 was_type = -2, was_ready = -2;
            static int hand_logs;
            if ((memcmp(was_hand, s.hand, sizeof was_hand) != 0 || was_type != s.eu_type || was_ready != s.eu_ready) &&
                hand_logs++ < 1000) {
                was_type = s.eu_type;
                was_ready = s.eu_ready;
                memcpy(was_hand, s.hand, sizeof was_hand);
                auto name = [](u8 h, char* b) {
                    if (h == kHandNone) return "none";
                    if (h == kHandDrawn) return "drawn";
                    if (h == kHandSheathed) return "sheathed";
                    snprintf(b, 16, "state %u", h & 0x7fu);
                    return (const char*)b;
                };
                char b0[16], b1[16], b2[16];
                printf("[player] hands: weapon %s, bow %s, shield %s (drawn type %d, ready slot %d; verbs %d %d %d)\n",
                       name(s.hand[0], b0), name(s.hand[1], b1), name(s.hand[2], b2), s.eu_type, s.eu_ready, v[0],
                       v[1], v[2]);
            }
        }
        const u32 power = s.power;
        if (req) printf("[player] ability %d %s (current %u)\n", req, s.wrote ? "requested" : "not written", power);
        // (mCurrentSpecialPower is the selected ability, kept while none is in use (h1-first: 1 while standing); in
        // use is the verb UseSpecialPower, flags_of's TPS_ABILITY)
        if (power <= TPA_AMIIBO) f |= power << 24;
        if (verbose()) {   // the slow motion's type changing: type 1 is the perfect guard (the success readout)
            static std::atomic<u32> was_type{kSpStepNone};
            static std::atomic<int> slow_logs{0};
            const u32 before = was_type.exchange(s.slowmo);
            if (before != s.slowmo && slow_logs++ < 300) {
                if (s.slowmo == kSpStepGuardJust) printf("[player] perfect guard (slow motion type 1)\n");
                else printf("[player] slow motion type %#x (was %#x; verbs %d %d %d)\n", s.slowmo, before, v[0], v[1], v[2]);
            }
        }
        if (verbose()) {   // the shield-surf chord refused (bit 57 0 -> 1)
            static std::atomic<int> was{0}, logs_refused{0};
            const int now = (int)(s.parry_word >> kParryBit & 1);
            if (was.exchange(now) == 0 && now && logs_refused++ < 300)
                printf("[player] shield-surf refused (Player +0x28 bit 57; verbs %d %d %d)\n", v[0], v[1], v[2]);
        }
        if (verbose()) {  // the guard and dodge outcomes; which one a perfect guard
            static const struct { int verb; const char *what; } kOutcomes[] = {
                {120, "parry"}, {164, "guard just repelled"}, {165, "guard just repelled (weapon drop)"},
                {160, "guarded hit"}, {161, "guard and riposte"}, {162, "guard break"},
                {163, "guard break (shield drop)"}, {144, "perfect dodge"}, {133, "flurry rush"},
            };
            static std::atomic<unsigned> had{0};
            unsigned has = 0;
            for (int x : v)
                for (unsigned i = 0; i < sizeof kOutcomes / sizeof *kOutcomes; i++)
                    if (x == kOutcomes[i].verb) has |= 1u << i;
            const unsigned rose = has & ~had.exchange(has);
            for (unsigned i = 0; i < sizeof kOutcomes / sizeof *kOutcomes; i++)
                if (rose >> i & 1) printf("[player] %s (verb %d)\n", kOutcomes[i].what, kOutcomes[i].verb);
        }
    }
    // RideJump 27 / RideJumpAttack 132 count as the horse only when the horse was already the state: the steering
    // stick's jump-off logs 27 too and read as HORSE for ~0.8 s, the horse rows live. Before the context push below:
    // the router's context is what put the horse rows live (pc_ctx vehicle -> horse -> world)
    if ((f & TPS_HORSE) && (v[0] == 27 || v[0] == 132) && !(g_state.load(std::memory_order_relaxed) & TPS_HORSE))
        f &= ~TPS_HORSE;
    // the router's gameplay context (pc_input.h): pushed at every read while there is a player (stale after 250 ms)
    if (s.ok) {
        uint32_t p = 0;
        if (f & TPS_CLIMB) p |= PCP_CLIMB;
        if (f & TPS_AIR) p |= PCP_AIR;
        if (f & TPS_GLIDE) p |= PCP_GLIDE;
        if (f & TPS_SWIM) p |= PCP_SWIM;
        if (f & TPS_AIM) p |= PCP_AIM;
        if (f & TPS_GUARD) p |= PCP_GUARD;
        if (f & (TPS_ABILITY | TPS_CAMERA)) p |= PCP_ABILITY;
        for (int verb : v) if (verb == 52 /*Scope*/) p |= PCP_SCOPE;   // (its map screen is the Scope, doc 4.18)
        if (f & TPS_HORSE) p |= PCP_HORSE;
        if (f & TPS_VEHICLE) p |= PCP_VEHICLE;
        if (v[0] == 25 /*RideRobot*/) p |= PCP_CONSTRUCT;   // (Mineru's construct: its own context, PCX_CONSTRUCT)
        pc_ctx_player(p, (uint8_t)TOTK_PLAYER_ABILITY(f));
    }
    static int logs;
    static int last_v[5] = {-2, -2, -2, -2, -2};
    const uint32_t was = g_state.exchange(f);
    const bool verbs_changed = memcmp(v, last_v, sizeof v) != 0;
    if (verbs_changed) memcpy(last_v, v, sizeof v);
    for (int i = 0; i < 5; i++) g_verb[i].store(v[i]);
    // the hand latch: the bow in hand (TPS_BOW_OUT, the bow actor's own drawn state; or UseBow 59 / DrawnBow 205) = the
    // bow, else the melee weapon in hand (TPS_WEAPON_OUT, or Drawn 57) = melee; neither keeps the last one, so a bow
    // the strip's release put back on Link's back still reads as the bow. On an ARRIVAL only: a state that lags a few
    // frames past the 1 strip's release must not undo the strip's note (totk_player_hand_note). The verbs stay beside
    // the bits until a boot has shown the bits alone suffice. seen is the UNMASKED class; the guard masks only drop the
    // melee WRITE, the bow edge stays live (BOW_EQUIPPED gates bow mode at the press)
    static int seen = TPH_NONE;
    const uint64_t tick = GetTickCount64();
    // no player for 1 s (a load, the title): the latch resets, so a new save starts at none (= the attack)
    static uint64_t invalid_ms;
    if (!(f & TPS_VALID)) {
        if (!invalid_ms) invalid_ms = tick;
        if (tick - invalid_ms >= 1000) seen = TPH_NONE;
        if (tick - invalid_ms >= 1000 && g_hand.exchange(TPH_NONE) != TPH_NONE) {
            static int reset_logs;
            if (reset_logs++ < 100) printf("[player] hand latch: reset (no player for 1 s)\n");
        }
    } else {
        invalid_ms = 0;
    }
    bool bow = (f & TPS_BOW_OUT) != 0, melee = (f & TPS_WEAPON_OUT) != 0;
    for (int verb : v) {
        bow = bow || verb == 59 || verb == 205;
        melee = melee || verb == 57;
    }
    const int now = bow ? TPH_BOW : melee ? TPH_MELEE : TPH_NONE;
    // a guard / lock-on is not a class choice: a melee edge counts only in a sample without LockOnWait 65; as an edge
    // against the unmasked seen, a level that rose during a guard never counts later. No time window. The bow edge
    // stays live; the strips' notes and tap 1 always win, being choices
    int hand = now != seen ? now : TPH_NONE;
    seen = now;
    // (the test is TPS_LOCKON, the one test: set for LockOnWait 65 in any slot, and for LockOnMove 66 /
    // LockOnFrontMove 209 / LockOnBackMove 210, which moving with the guard / lock-on held logs instead, and the
    // attention lock. Plus the router's own VK 2 while held: it is set at the Q / RMB press, before the game reacts,
    // so a WEAPON_OUT rising one sample before the lock-on verbs is covered too; no window, only while the key is
    // down; the pad's ZL relies on TPS_LOCKON alone)
    if (hand == TPH_MELEE && ((f & TPS_LOCKON) || (totk_pcvk_held() & (1ull << 2)))) hand = TPH_NONE;
    // a bow edge with no bow equipped is ZR's own false 59 (rc_horse4: the empty aim stance; the 3 strip with no bows
    // holds ZR too), not a choice. Only where the hands read works (an _EQUIPPED bit shows)
    const bool hands_read = (f & (TPS_WEAPON_EQUIPPED | TPS_BOW_EQUIPPED | TPS_SHIELD_EQUIPPED)) != 0;
    if (hand == TPH_BOW && hands_read && !(f & TPS_BOW_EQUIPPED)) hand = TPH_NONE;
    if (hand != TPH_NONE && g_hand.exchange(hand) != hand) {
        static int hand_logs;
        if (hand_logs++ < 300)
            printf("[player] hand latch: %s (state %#x verbs %d %d %d %d %d)\n", hand == TPH_BOW ? "bow" : "melee", f,
                   v[0], v[1], v[2], v[3], v[4]);
    }
    const int cap = verbose() ? 4000 : 400;
    if ((was != f || (verbose() && verbs_changed)) && logs < cap) {
        logs++;
        printf("[player] state %#x verbs %d %d %d %d %d\n", f, v[0], v[1], v[2], v[3], v[4]);
    }
}
#endif

}  // namespace

extern "C" uint32_t totk_player_state(void) {
    const uint32_t st = g_state.load(std::memory_order_relaxed);
    // LMB's meaning: the bow while it is in Link's hand. The ~2 game frames between F's take-out request and the
    // game showing it are MAC_ATTACK's own window
    return (st & TPS_BOW_OUT) ? st | TPS_BOW_MODE : st;
}

extern "C" bool totk_player_on_stick(void) { return g_stick.load(std::memory_order_acquire); }

extern "C" int totk_construct_attach_in_reach(void) {
#if defined(_WIN32) && !defined(TOTK_PLAYER_STATE_TEST)
    if (g_verb[0].load(std::memory_order_relaxed) != 25 /*RideRobot*/) return -1;
    pc_fault_guard(1);
    const int r = construct_reach_walk();
    pc_fault_guard(0);
    return r;
#else
    return -1;
#endif
}

extern "C" int totk_player_verb(int slot) {
    return slot >= 0 && slot < 5 ? g_verb[slot].load(std::memory_order_relaxed) : -1;
}

extern "C" int totk_player_hand(void) {
    // a bow latched but none equipped (the last one broke, or none chosen): the melee weapon, so LMB never presses ZR
    // without a bow
    const int h = g_hand.load(std::memory_order_relaxed);
    const uint32_t st = g_state.load(std::memory_order_relaxed);
    // the source, once, for the gate: the hands read works when an _EQUIPPED bit has shown
    static std::atomic<bool> told{false};
    if ((st & TPS_VALID) && !told.exchange(true))
        printf("[player] bow mode: %s\n", st & (TPS_WEAPON_EQUIPPED | TPS_BOW_EQUIPPED | TPS_SHIELD_EQUIPPED)
                                              ? "hands read (TPS_*_EQUIPPED)"
                                              : "verb fallback (no equipment seen in the hands read)");
    if (h == TPH_BOW && (st & TPS_VALID) && !(st & TPS_BOW_EQUIPPED)) return TPH_MELEE;
    return h;
}

extern "C" void totk_player_hand_note(int hand) {
    if (hand != TPH_MELEE && hand != TPH_BOW) return;
    if (g_hand.exchange(hand) != hand) {
        static int logs;
        if (logs++ < 300) printf("[player] hand latch: %s (a strip's release)\n", hand == TPH_BOW ? "bow" : "melee");
    }
}

// Take out the bow, the way the bow strip's release does it: the strip (onOutBefore, main+0x1a80b38) sets GameData bool
// 0x2f03cd3e through GameDataMgr::setBool (main+0xa02aec, (mgr, value, hash)); Player::syncRequestFlags (main+0x671424,
// in Player::calc) turns it into Player +0xb00 bit 21 and clears it (setBool(mgr, 0, hash), on that same calc thread);
// the take-out test (main+0x16e80c0) = ZR's trigger or bit 21, so the bow comes out (UseBow 59) with no ZR: no aim, no
// arrow. The bool waits until consumed, so the hook's place in the frame does not matter. setBool is a guest call: on
// the hook's Ctx, game thread only, like syncRequestFlags' own call. With the sword drawn too: the strip's release from
// a drawn sword brought the bow out by itself (ow2a:16064, ow2g:16527, ow2f:16588 /:19393). Not with the bow already
// out (F puts it away then: the holster's VK 5 tap), nor in an event, in a vehicle or with an ability in use. In the
// saddle it is the same test as ZR's: the take-out test's only state-dependent part is its gate (main+0xbc1870, on slot
// 0's verb), which has a Ride (24) case (Player +0xae0 bit 2 set, bits 1 and 4 clear, then the common checks) and
// answers "no" for RideDragon (26) and RideJumpAttack (132), for ZR and bit 21 alike. The Bow child is picked by
// SelectorPlayerSubSchedulerSheathed (+0x130 / +0x140 / +0x148), not by SelectorPlayerSchedulerRide or
// ExecutePlayerRide, which never call the test. preCalc clears bit 21 every frame, so a request the gate refuses is
// gone, not kept for after the dismount. A request the hook has not taken within 250 ms (a menu, a load) is dropped.
static bool bow_request_ok(uint32_t st) {
    const uint32_t no = TPS_BOW_OUT | TPS_EVENT | TPS_VEHICLE | TPS_ABILITY;
    return (st & TPS_VALID) && (st & TPS_BOW_EQUIPPED) && !(st & no);
}

extern "C" bool totk_player_request_bow(void) {
#ifdef _WIN32
    if (!bow_request_ok(totk_player_state())) return false;
    g_bow_request.store(GetTickCount64());
    return true;
#else
    return false;
#endif
}

extern "C" void totk_player_game_calls(Ctx* c) {
#ifdef _WIN32
    totk_player_stick_frame(c);  // the steering stick: the game's state test 23
    const unsigned long long asked = g_bow_request.exchange(0);
    if (!asked) return;
    const uint32_t st = totk_player_state();
    const u64 gdm = totk_global(kGameDataMgrSlot);
    const bool stale = GetTickCount64() - asked > 250;
    const bool ok = !stale && bow_request_ok(st) && ptr_ok(gdm);
    if (ok) {   // (after this the hook reads no guest register but sets x0 itself: totk_pc_vk.cpp)
        c->x[0] = gdm;
        c->x[1] = 1;
        c->x[2] = kGdBowTakeOut;
        call_ind(c, totk_main(kGameDataSetBool));
    }
    static int logs;
    if (logs++ < 200)
        printf("[player] take out the bow: %s (state %#x)\n", ok ? "requested" : stale ? "dropped (stale)" : "not now",
               st);
#else
    (void)c;
#endif
}

extern "C" bool totk_player_request_ability(int n) {
    if (n < TPA_ULTRAHAND || n > TPA_CAMERA || !(totk_player_state() & TPS_VALID)) return false;
    g_request.store(n);
    return true;
}

#ifdef _WIN32
namespace {
std::mutex g_mu;
std::atomic<unsigned long long> g_game_read{0};   // GetTickCount64 of the last game-thread read
}  // namespace
#endif

extern "C" void totk_player_state_frame(void) {
#ifdef _WIN32
    // several pad reads a frame, from up to 4 threads: one reader at a time, the others keep the last answer; none
    // while the game thread has read it in the last 250 ms
    const unsigned long long g = g_game_read.load(std::memory_order_relaxed);
    if (g && GetTickCount64() - g < 250) return;
    std::unique_lock<std::mutex> l(g_mu, std::try_to_lock);
    if (!l.owns_lock()) return;
    read_player();
#endif
}

extern "C" void totk_player_stick_frame(Ctx* c) {
#ifdef _WIN32
    static RideLink asked{0, -1, 0};   // (the game thread only)
    static bool on;
    static int logs;
    bool now = false;
    RideLink r;
    if (g_verb[0].load(std::memory_order_relaxed) == kVerbManipulate && ride_link(&r)) {
        if (r.actor != asked.actor || r.id != asked.id || r.slot != asked.slot) {
            asked = r;
            c->x[0] = r.actor;
            c->x[1] = kQueryRideControlStick;
            call_ind(c, totk_main(kQueryPlayerState));
            on = (c->x[0] & 1) != 0;
            if (logs++ < 200)
                printf("[player] verb 48 on ride link %d (entry %u): %s\n", r.id, r.slot,
                       on ? "a steering stick (state 23)" : "not a steering stick");
        }
        now = on;
    } else {
        asked = RideLink{0, -1, 0};
    }
    if (g_stick.exchange(now, std::memory_order_acq_rel) != now && logs++ < 200)
        printf("[player] steering stick %s\n", now ? "held" : "let go");
#else
    (void)c;
#endif
}

extern "C" void totk_player_state_game_frame(void) {
#ifdef _WIN32
    std::lock_guard<std::mutex> l(g_mu);
    g_game_read.store(GetTickCount64(), std::memory_order_relaxed);
    read_player();
#endif
}
