// pc_gameplay.h: the gameplay half of the PC controls. The player-state contexts, the macros of the 'M' rows in
// pc_scheme_gameplay.inc and the virtual-key writer, all registered with the router (pc_input.h) by
// pc_gameplay.cpp. pc_input.cpp includes this for the ids.
#pragma once

// The 'M' rows' macro ids (the row's arg). Steps count game frames, at least 2 per step. The ability wheel and
// the d-pad strips are plain holds: the quickmenu context feeds them the mouse (4.13).
enum PcMacroId {
    MAC_NONE,
    MAC_DODGE,       // ZL, then ZL + X, with the direction the movement keys hold: side hop / backflip
    MAC_PARRY,       // A while the shield is up; else ZL 4 frames, then ZL + A
    MAC_BOW,         // RMB: ZR while held; each LMB press fires (ZR off 2 frames); the release lowers the bow, no shot
    MAC_ABILITY_1,   // select an ability by id (no pad input): Ultrahand
    MAC_ABILITY_2,   // Fuse
    MAC_ABILITY_3,   // Ascend
    MAC_ABILITY_4,   // Recall
    MAC_ABILITY_5,   // Autobuild
    MAC_ABILITY_6,   // the Purah Pad camera
    MAC_NEXT,        // wheel: next weapon (d-right held, a right-stick flick, release)
    MAC_PREV,        // ... previous
    MAC_PULL,        // Ultrahand / Autobuild: the wheel's notch holds d-pad up 6 frames (pull closer)
    MAC_PUSH,        // ... d-pad down (push away)
    MAC_ROTATE,      // R held: every 40 px of mouse is a 2-frame d-pad press that way (camera off)
    MAC_RAISE,       // the right stick up while held (a held object up)
    MAC_LOWER,       // ... down
    MAC_UNSTICK,     // the right stick shaken left / right for 24 frames
    MAC_SECONDARY,   // RMB in an ability: Ultrahand / Autobuild release (A), Recall stop (L), Fuse onto the shield (ZL)
    MAC_PRIMARY,     // LMB in an ability: A (grab, pick, ascend, build, photo); Fuse onto the weapon: Y
    MAC_ARROW_NEXT,  // the wheel while aiming: next arrow material (d-up held, a right-stick flick, release; 3.3)
    MAC_ARROW_PREV,  // ... previous
    MAC_VEHICLE_LEAVE,  // VEHICLE's Shift: on a steering stick, a sled or the telescope one tap of
                     // the pad B's set (VK 1 | 5 | 25: the let-go / step-off), Shift claimed until released
    MAC_ATTACK,      // LMB (4.23 A): with the hand latch on the bow (totk_player_hand) the bow's draw (ZR held, AIM;
                     // the release shoots), else attack: VK 9 held for as long as LMB is down (tap a swing, hold the charge)
    MAC_BOW_STRIP,   // "3": the bow strip (ZR + d-right held, VK 13 masked from the player: no aim); at the close ZR
                     // up one frame before d-right, so the game takes nothing out (4.26: 3 only chooses). With the bow
                     // already drawn (LMB's MAC_ATTACK) it holds d-right only
    MAC_WEAPON_STRIP, // "1" held (the T kind starts it at 250 ms): the weapon strip (d-right held, as the H row it
                     // replaced); its release latches the melee weapon (totk_player_hand_note), so LMB swings after it
                     // even with the sword left on Link's back
    MAC_HOLSTER,     // "Q" (4.26 addendum, 4.23 B's job): the weapon or the bow in hand -> one VK 5 tap (put away);
                     // empty hands -> one VK 9 tap with a melee weapon equipped (it draws, no swing standing)
    MAC_BOW_TOGGLE,  // "F" (4.26): the bow in hand -> one VK 5 tap (put away); else the game's take-out request
                     // (totk_player_request_bow: no aim, no arrow, from a drawn sword too); no bow equipped -> nothing
    MAC_SPRINT,      // Shift (4.25): VK 1 held (sprint); pressed on foot with something in hand, one VK 5 tap first (put
                     // away), the pad B's trigger and hold in one go. WORLD / AIR / AIM, so the run carries the hold
    MAC_BOW_OUT,  // "F" in the air and gliding: MAC_BOW_TOGGLE's take-out only; the bow in hand ->
                     // nothing (its VK 5 tap would close the glider, ExecutePlayerGlide)
    MAC_CONSTRUCT_ATTACH,   // "E" on Mineru's construct: one VK 26 tap (the pad's X, "Attach"), then the attach window: the
                            // part keys (right / left arm, back part) only pick, one tap each, and Esc cancels (B)
    MAC_CONSTRUCT_LEAVE,    // "Space" on the construct: get off (the pad's B, then its A if still riding); in the attach
                            // window the "B Cancel" only
    MAC_RECIPE_OPEN,        // "R" in the pause menu: the pad's X on the Food tab (pause sub 5), which opens the recipe
                            // book; nothing on the other tabs
    MAC_BOW_DRAW,  // F held: MAC_ATTACK's bow mode from the hold's start, whatever is in hand: ZR held
                     // (the draw), counted in g_bow_held so on foot / in the air the context is AIM, the release shoots
    MAC_RECIPE_HISTORY,     // "H" in the pause menu: the pad's Y on the Food tab (pause sub 5), its "View history"
                            // elsewhere nothing (Y is Sort)
    MAC_COUNT
};
