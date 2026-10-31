// totk_player_state.h: what the player is doing, for the gameplay contexts of the PC controls. Read lock-free from
// any thread; updated at every pad read.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bits of totk_player_state(). TPS_VALID: the player actor was found (off: the title, a load, no player).
// Several bits can be set at once (AIM and AIR while shooting from a glide); the router picks the context by priority.
enum TotkPlayerFlag {
    TPS_VALID = 1u << 0,
    TPS_CLIMB = 1u << 1,     // climbing a wall or a ladder, a waterfall, a climbing jump
    TPS_AIR = 1u << 2,       // in the air without the glider (a jump, a fall, a skydive)
    TPS_GLIDE = 1u << 3,     // the paraglider (or a Zonaite wing / an animal) open
    TPS_SWIM = 1u << 4,      // swimming, or diving into water
    TPS_AIM = 1u << 5,  // the bow in hand (UseBow 59 stays while it is out, drawn or not); AIM also
                             // needs the bow key held (pc_gameplay.cpp from_player)
    TPS_GUARD = 1u << 6,     // GuardKeep (248) in a slot. NOT the standing guard: Q / pad ZL with the shield raised
                             // logs 65 + 57. Nothing routes on it (a debug force only)
    TPS_LOCKON = 1u << 7,    // locked on to a target
    TPS_ABILITY = 1u << 8,   // an ability (Ultrahand, Fuse, Ascend, Recall, Autobuild) in use
    TPS_HORSE = 1u << 9,     // riding (a horse, a dragon)
    TPS_VEHICLE = 1u << 10,  // a mech, a sled, a steering stick
    TPS_SURF = 1u << 11,     // shield surfing
    TPS_TALK = 1u << 12,     // talking (an event owns the input)
    TPS_CAMERA = 1u << 13,   // the Purah Pad camera or the scope
    TPS_EVENT = 1u << 14,    // an event runs (EventMgr's main event: talk, cutscene; the game cuts the player's input)
    TPS_HOLDING = 1u << 15,  // Ultrahand holds an object (UltraHand mState 4 Grabbing / 5 Grabbed)
    // What is in the hands: the equipment slot's actor is EquipedDrawn, the game's own WeaponDrawn test per
    // slot. Off: sheathed, not equipped, or no player.
    TPS_WEAPON_OUT = 1u << 16,  // the melee weapon in hand (DynamicEquipmentSlot 0)
    TPS_BOW_OUT = 1u << 17,     // the bow in hand (slot 3), drawn (aimed) or not
    TPS_SHIELD_OUT = 1u << 18,  // the shield on the arm (slot 4)
    // Equipped at all, in hand or on the back: the slot's equipment actor is alive (EquipmentUser::isSlotEquipped,
    // main+0xd3a544). Off: nothing in that slot (none chosen, or the last one broke), or no player.
    TPS_WEAPON_EQUIPPED = 1u << 19,
    TPS_BOW_EQUIPPED = 1u << 20,
    TPS_SHIELD_EQUIPPED = 1u << 21,
    // (bit 22: the TPS_BOW_DRAWN, to come) LMB's meaning: the bow is in Link's hand (= TPS_BOW_OUT; kept for readers of
    // the 4.23 name). F's take-out window is MAC_ATTACK's own. The 4.23 hand latch no longer feeds it
    TPS_BOW_MODE = 1u << 23,
    // Moving fast enough that the game's VK 9 from a sheathed weapon is the running draw-slash: slot-0 verb Walk (1,
    // walk / run / sprint, momentum included) and the Player's move rate >= 0.25, the walk scheduler's DashAttack test
    // (main+0x16e9130). Off: standing (Wait 40), where a 1-frame VK 9 only draws
    TPS_MOVING = 1u << 28,   // (bits 24-27 are the ability)
};
// Bits 24-27 of totk_player_state(): the ability in use (enum game::specialpower::SpecialPowerType)
enum TotkAbility {
    TPA_NONE = 0, TPA_ULTRAHAND = 1, TPA_FUSE = 2 /*OneTouchBond*/, TPA_ASCEND = 3 /*CeilingClipper*/,
    TPA_RECALL = 4 /*ReverseRecorder*/, TPA_AUTOBUILD = 5 /*AutoBuilder*/, TPA_CAMERA = 6 /*SheikahCamera*/,
    TPA_MAP = 7, TPA_AMIIBO = 8,
};
#define TOTK_PLAYER_ABILITY(s) ((enum TotkAbility)(((s) >> 24) & 0xf))
#define TOTK_PLAYER_HANDS 1   // the TPS_*_OUT / TPS_*_EQUIPPED bits exist (callers can #if on it)
uint32_t totk_player_state(void);
// The player's action verb (enum game::bsa::ActionVerb) in AS slot 0-4, -1 without a player; for logs and tests.
int totk_player_verb(int slot);
// The weapon class last chosen or seen in Link's hand: TPS_BOW_OUT / UseBow 59 / DrawnBow 205 arriving, or the bow
// strip's release = the bow; TPS_WEAPON_OUT / Drawn 57 arriving, or the weapon strip's release = the melee weapon. It
// stays after the bow goes back on Link's back (the bow strip's release), so LMB keeps drawing until a melee weapon is
// chosen; the bow only while one is equipped (TPS_BOW_EQUIPPED). TPH_NONE until seen.
enum TotkHand { TPH_NONE = 0, TPH_MELEE = 1, TPH_BOW = 2 };
int totk_player_hand(void);
// A strip's release sets the latch (the 1 strip: melee, the 3 strip: bow): a pick can leave the item on Link's back with
// no verb of its own (owner1's equip; nk3 / nk4: the bow strip's release clears 59 to "40 0 0"). The latch has exactly
// two writers, both atomic exchanges, and the last real event wins: read_player (the game thread, the state's arrivals)
// and this note (the input thread, from the macro ticks: the 1 / 3 strip releases, the wheel's weapon switch, tap 1, F's
// draw; (B)'s tap 3 too). No third path. The note is unconditional: TPS_BOW_MODE needs TPS_BOW_EQUIPPED, so a "None"
// pick in the bow strip still leaves LMB the attack
void totk_player_hand_note(int hand);
// Select ability n (TPA_ULTRAHAND.. TPA_CAMERA) as the wheel would: SpecialPower's mRequestedSpecialPower, which the
// actor's update copies to the current one. Written at the next pad read, on the game's thread. false: not an ability
// id, or no player. The game's own unlock check is not traced yet: the caller must not offer a locked ability.
bool totk_player_request_ability(int n);
// hle_hid.cpp GetNpadStates: read the player once per pad read (cheap; several reads a frame share one answer).
// Skipped while the game thread reads it (below) once a frame.
void totk_player_state_frame(void);
// totk_pcvk.cpp pc_vk_frame, from the ExecutePlayerCalcKey hook: read the player on the game thread, in its own AI
// update (the actor cannot be freed under the read there)
void totk_player_state_game_frame(void);
// Holding a steering stick: the game's own state test 23 (RideControlStick), asked by totk_player_stick_frame.
// Read from any thread.
bool totk_player_on_stick(void);
// Riding Mineru's construct: an attachable Zonai device in reach: 1 yes, 0 no, -1 not riding it / not readable. Any
// thread (plain loads under SEH)
int totk_construct_attach_in_reach(void);
#define TOTK_HAS_CONSTRUCT_REACH 1
// the ExecutePlayerCalcKey hook, after pc_vk_frame, on its guest context (the game thread): while slot-0 verb 48
// Manipulate shows and the player's ride link is live, call the state test once per link (a guest call).
struct Ctx;
void totk_player_stick_frame(struct Ctx* c);
// Take out the bow as the bow strip's release does (the game's own request: GameData bool 0x2f03cd3e -> Player
// +0xb00 bit 21 -> UseBow 59, no ZR, no aim, no arrow), from the sword drawn too. Any thread; performed at the next
// hook frame by totk_player_game_calls, or dropped after 250 ms. false: no player, no bow equipped, the bow already
// out, an event, a horse / vehicle, or an ability in use.
bool totk_player_request_bow(void);
// the ExecutePlayerCalcKey hook, after pc_vk_frame, on its guest context (the game thread): every game call asked of
// that thread, as guest calls on that Ctx: the steering stick's state test (totk_player_stick_frame), then the queued
// requests (the bow's take-out). The hook's one call site.
void totk_player_game_calls(struct Ctx* c);

#ifdef __cplusplus
}
#endif
