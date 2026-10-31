// totk_menu_cursor.h: what totk_menu_cursor.cpp tells the rest of the engine.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The menu screen in front, for the context (the pc_ctx): bits 0-7 the screen, bits 8-15 a sub value (the pause
// menu's tab 0-8: Armor, Bow, Shield, Weapon, Material, Food, SpecialParts, KeyItem, System; 0xff: none or
// unknown). Lock-free (one atomic load), from any thread; updated at every pad read. TMS_NONE means "no focus
// box", not "gameplay": the map, the Purah Pad's pages and the talk window read it too until the census names
// them, so a reader must still check its own scene test before routing gameplay.
enum TotkMenuScreen {
    TMS_NONE = 0,       // no focus box (not necessarily gameplay)
    TMS_TITLE,          // the title menu (Continue, New Game, Load, Options, ...)
    TMS_SAVE_LIST,      // a save list or the save / load screen
    TMS_PAUSE,          // the pause menu (inventory, System tab); sub = the tab
    TMS_ITEM_POPUP,     // an item's action popup in the pause menu (Equip / Drop / Eat ...); sub = the tab
    TMS_PURAH_PAD,      // the Purah Pad, not its map; sub = the page in its tab bar's order: 0 Character Profiles,
                        // 1 Adventure Log, 3 Album, 4 Hyrule Compendium (0xff another)
    TMS_MAP,            // the Purah Pad's map (sub 2: its page in the tab bar)
    TMS_DIALOGUE,       // the talk window showing a line (MessageDialog_00 without a focus box)
    TMS_CHOICE,         // a choice list (dialogue choices, yes / no)
    TMS_SHOP,           // a shop menu (ShopMenu_00, DyeShopMenu_00, HouseShopMenu_00) with its focus box
    TMS_OPTIONS,        // System > Options
    TMS_OTHER,          // another screen with a focus box
    TMS_TRANSITION,     // a menu's focus gap (a popup opening, a submenu closing, a tab switch), up to 300 ms
    TMS_QUICKMENU,      // the L ability wheel (ShortCutMainItem_00) or a D-pad strip (ShortCut_00); gameplay class
                        // the cursor stays off; sub = TMS_QM_*
    TMS_CAMERA,         // the Purah Pad's camera (CameraPointer_00 in the Pad's frame, no page shown); gameplay class
                        // the cursor stays off
    TMS_SCOPE,          // the Scope: the Pad's frame over AppMap_00 with CameraPointer_00 shown, no AppMenuBG_00 (not
                        // the map page); gameplay class, the cursor stays off
    TMS_RECIPE,         // the cooking recipe book (RecipeCard_00, UIRecipeCardScreen) over the pause menu; sub 0 the
                        // recipe cards, 1 the history (its PaRecipeLogSlideList_00 shown). Published only where the
                        // router knows it (pc_input.h's PC_HAS_PCX_RECIPE), else the screen stays TMS_OTHER
};
// TMS_QUICKMENU's sub value
enum { TMS_QM_WHEEL = 0, TMS_QM_WEAPON = 1, TMS_QM_SHIELD = 2, TMS_QM_MATERIAL = 3, TMS_QM_ARROW = 4,
       TMS_QM_AUTOBUILD = 5 };   // (5: the Autobuild list, AutoBuilder_00)
#define TOTK_TMS_QUICKMENU 13  // = TMS_QUICKMENU, for #ifdef in readers (the pc_ctx_decide)
#define TOTK_TMS_CAMERA 14      // = TMS_CAMERA, likewise
#define TOTK_TMS_SCOPE 15       // = TMS_SCOPE, likewise
#define TOTK_TMS_RECIPE 16      // = TMS_RECIPE, likewise
enum { TMS_RECIPE_CARDS = 0, TMS_RECIPE_HISTORY = 1 };   // TMS_RECIPE's sub value
#ifdef __cplusplus
static_assert(TMS_QUICKMENU == TOTK_TMS_QUICKMENU && TMS_CAMERA == TOTK_TMS_CAMERA && TMS_SCOPE == TOTK_TMS_SCOPE &&
              TMS_RECIPE == TOTK_TMS_RECIPE, "TOTK_TMS_* out of step with the enum");
#else
_Static_assert(TMS_QUICKMENU == TOTK_TMS_QUICKMENU && TMS_CAMERA == TOTK_TMS_CAMERA && TMS_SCOPE == TOTK_TMS_SCOPE &&
               TMS_RECIPE == TOTK_TMS_RECIPE, "TOTK_TMS_* out of step with the enum");
#endif
uint32_t totk_menu_screen(void);
// The construct's attach screen (the MAC_CONSTRUCT_ATTACH: the attach mode is UIAttachTargetSelectCommandScreen, eui
// hash 0x6dc85b0e, open and not closing): AttachTargetSelectCommand_00 in the Opened state (a closing screen has left
// it) at the last UI update. 1 open, 0 not, -1 unknown (no UI hook in this key, or eui not updating for 250 ms).
// Lock-free, any thread.
#define TOTK_HAS_ATTACH_SCREEN 1
// The inventory's model turned by an LMB drag: true while a drag that started on Link's model is held, *rx the right
// stick's x for it (-32767..32767, the pad's Rotate); hle_hid's GetNpadStates puts it in the pad state
bool totk_menu_cursor_rstick(int32_t *rx);
int totk_attach_screen_open(void);
// Pad buttons (HidNpadButton bits) held on every pad read until the next call (0: none): the Options sections' reopen
// (totk_options_menu.cpp: B closes Options, A on its opener opens it again), from eui's thread
void totk_menu_pad_hold(uint32_t buttons);
// totk_options_menu.cpp: the < and > of the open Options screen's selection row whose part layout is layout (the
// panes Pa_ArrowL_00 / Pa_ArrowR_00), for a click's hit test; false when layout is no such row. eui's thread only.
bool totk_options_row_arrows(uint64_t layout, uint64_t *prev_pane, uint64_t *next_pane);
// totk_options_menu.cpp: a section change is closing and reopening Options (B, then A on the opener): the cursor
// neither hovers nor clicks meanwhile, so the opener's focus stays on Options
bool totk_options_reopening(void);
// Settings' sections (totk_options_menu.cpp): the previous / next one (dir -1 / 1, wrapping), as the section row's
// step does, run on eui's thread (totk_ui_post). False when Options has no sections (TOTK_OPTIONS_ZONES=0, the cursor
// off) or the task cannot be queued: the caller falls back. Q / E in Settings
bool totk_options_section_step(int dir);
// The focused node of the focus screen as the last frame saw it (0: none). eui's thread.
uint64_t totk_menu_focus_node(void);
// The active node of the open screen of this layout ("Pouch_00", "Title_00"), in front or not; 0: none. eui's thread.
uint64_t totk_menu_layout_focus(const char *layout);

// Runs fn(arg, true) on eui's thread at its next UI update (design A's hook on UIController::updateState), inside the
// guest bridge, so fn may make guest calls (GUEST_FN): for game code that must run where the UI runs (the
// ability wheel's own commit). False when it cannot be queued: the key has no hook, the hook has not run in the last
// 250 ms (a loading screen), or 16 tasks wait. A queued task that could not run within 250 ms (eui stopped: a load,
// a cutscene) gets fn(arg, false) instead, on some thread, and must make no guest call then: free arg, fall back.
// Either way fn is called exactly once; it must not block.
typedef void (*TotkUiTask)(void *arg, bool ran);
bool totk_ui_post(TotkUiTask fn, void *arg);
// The key set (a PcCtx, pc_input.h) the System tab's controls diagram draws: HORSE's on the page the game titles
// "Riding Controls", by the two fields its refresh reads, on foot's (PCX_WORLD) on any other page. ride < 0
// (those fields unreadable): by the pause's origin instead, the pc_glyph_game_set() and totk_player_state() (in
// the saddle: PCX_HORSE or TPS_HORSE, a bow drawn there too). Pure (tools/oracle/hook_mc_test.cpp checks it).
int totk_controls_page_set(int ride, int kind, int game_set, uint32_t player_state);
// Basic Controls' second lines ("(Press) Crouch", "(Hold) Change Ability", "(Press) Scope": a callout part with no
// icon of its own) with the PC key in the bracket: in[0 .. len) the label as the game wrote it (UTF-16, eui's tags
// 0x0E / 0x0F skipped, never searched), key the key's name (UTF-8). The first bracketed group, ( ) [ ] or the
// full-width （ ） 【 】, gets the key as its content: "(Ctrl) Crouch", "しゃがむ [Ctrl]". Returns the new length,
// written to out with a terminator, or -1 (no bracket, an empty key, or no room under cap units with the terminator).
// Pure (tools/oracle/hook_mc_test.cpp checks it).
int totk_mc_sub_label(const uint16_t *in, int len, const char *key, uint16_t *out, int cap);
// The Autobuild list's direct set (its rows: UIAutoBuilderScreen +0x248, 0x14 each, +0xc < 0 a header): the row n
// notches from row at (past the headers, clamped at either end, no wrap: updateImpl_'s own walk), and the scroll
// target that centres row to (getDraftSlotHeight 0x1001a90abc; btn / hdr the row and header heights). Guest memory
// reads only (tools/oracle/hook_mc_test.cpp checks them).
int totk_ab_target_row(uint64_t rows, int count, int at, int notches);
float totk_ab_row_scroll(uint64_t rows, int to, float btn, float hdr);
// A D-pad strip's move for `notches`: from index `base` (the current one, or a pending target), with the strip's count
// items (0.. count - 1, wrapping) and its slide window [wlo, whi] (empty on a strip that shows all its items), `to`
// the chosen item and `pre` the one before it. TOTK_STRIP_STEP: one item, the game's own step (animated, its sound);
// SET_THEN_STEP: setPage onto `pre` now, the native step next update; SET: setPage onto `to` (no slide there: a short
// strip, a wrap, outside the window, or a snap pending); NONE: `to` is the current item. Pure
// (tools/oracle/hook_mc_test.cpp checks it).
enum { TOTK_STRIP_NONE, TOTK_STRIP_STEP, TOTK_STRIP_SET_THEN_STEP, TOTK_STRIP_SET };
int totk_strip_plan(int cur, int base, int notches, int count, int wlo, int whi, bool snap_free, int *to, int *pre);
// A scrolling list: its rows are drawn through a scissor pane, the window. A box: centre and half sizes, layout units, y
// up. totk_box_in_window: b's whole box inside w less the margins mx / my, on each axis where b is smaller than w (a row
// wider than its window never scrolls that way): the rows a hover may focus. totk_scroll_pick: the wheel's step from the
// focused row, dir -1 up / 1 down: among rows that way in its column, the nearest one not inside the window (the game
// scrolls it in: a notch is a row), else the farthest one inside (the list's end); -1: none. Pure
// (tools/oracle/hook_mc_test.cpp checks them).
typedef struct TotkBox { float cx, cy, hw, hh; } TotkBox;
bool totk_box_in_window(const TotkBox *b, const TotkBox *w, float mx, float my);
int totk_scroll_pick(const TotkBox *focused, const TotkBox *rows, int n, const TotkBox *w, float mx, float my, int dir);
// totk_row_at_edge: r is whole inside w but the last whole row on a side where its column goes on with a row that is not
// (cut by the window's edge or hidden past it): a list keeping a margin scrolls when that row takes the focus, so a hover
// leaves it
bool totk_row_at_edge(const TotkBox *r, const TotkBox *rows, int n, const TotkBox *w, float mx, float my);
// The menu hotkeys' chain: from any menu, or the world, to a pause-menu tab or a Purah Pad page, and the same key on
// its own menu closes it. The router asks with PCOP_MENU_GOTO (pc_input.h, PC_HAS_MENU_GOTO): arg
// TOTK_MC_GOTO_INVENTORY (the last item tab used, never System; weapons on a fresh start), a pause tab 0..8 (8 System),
// TOTK_MC_GOTO_PAGE + a Pad page (0x12 the map, 0x11 the adventure log), or TOTK_MC_GOTO_CANCEL (another key: stop). A
// new target replaces the running one. The steps, each a press of 60 ms then 60 ms released, the whole chain at most 3
// s:
//  - a sub-screen open over the menu (TOTK_MENU_F_SUB): B, until it is gone (at most 4);
//  - first step only: on the target already (the inventory: any item tab) -> B, the menu closes (the toggle);
//  - the target's menu open on another tab / page -> the tab walk (walk_kind 0 the pouch's tabs, 1 the Pad's pages);
//  - the other menu open -> B until no menu, then the target's opener: Plus (the pause menu) or Minus (the Pad), then
//    the walk;
//  - no menu -> the opener, then the walk;
//  - any other screen (a dialogue, a shop, the title) -> the chain stops (Esc on System from a sub-screen is the
//    router's B: the op declines it).
// Pure (tools/oracle/hook_mc_test.cpp checks it); the hook runs it each UI update and presses through the pad. (the
// pc_input.h names them PCMG_PAD / PCMG_ITEM / PCMG_NO_CLOSE: the same values. NO_CLOSE, or'ed in: only go there, never
// the toggle; the world's I sends it after its own Plus has opened the pause menu) TOTK_MC_GOTO_LAST: the pause menu on
// the last of its tabs shown at the top level (0..8, recorded while no chain runs), or where the game opens it when
// none was seen yet; in the pause menu on any tab, the toggle closes it.
enum {
    TOTK_MC_GOTO_CANCEL = -1, TOTK_MC_GOTO_PAGE = 0x10, TOTK_MC_GOTO_INVENTORY = 0x20, TOTK_MC_GOTO_LAST = 0x30,
    TOTK_MC_GOTO_NO_CLOSE = 0x100
};
#define PC_HAS_MENU_GOTO_LAST 1  // (the PCMG_LAST = TOTK_MC_GOTO_LAST)
typedef struct TotkChain {
    int target;            // -1: idle
    int phase;             // 0 decide, 1 pressing, 2 releasing, 3 wait closed, 4 wait open
    int first;             // the first decision (only it toggles)
    int backs;             // B presses made for sub-screens / closing
    uint32_t button;       // the button being pressed (HidNpadButton bits)
    int after;             // the phase after the press
    int last_item_tab;     // the last item tab seen (0..7; 3 at first)
    int last_tab;          // the last pause tab seen at the top level outside a chain (0..8; -1 at first)
    unsigned long long at, t;   // the chain's start, the phase's start (ms)
} TotkChain;
typedef struct TotkChainIn {
    unsigned long long now;
    uint32_t screen;       // totk_menu_screen()
    bool pad_open, pouch_open;
} TotkChainIn;
typedef struct TotkChainOut {
    uint32_t buttons;      // pad buttons to hold this read
    int walk_kind, walk_to;   // a tab walk to start (walk_to -1: none)
} TotkChainOut;
void totk_menu_chain_init(TotkChain *c);
void totk_menu_chain_goto(TotkChain *c, int target, unsigned long long now);
TotkChainOut totk_menu_chain_step(TotkChain *c, const TotkChainIn *in);
// Bit 17 of totk_menu_screen(): a sub-screen is open over a menu's top level (the pause menu or the Purah Pad is open
// and the focus is not its own page: an item's popup, Options, the save list, a yes / no choice, Special Controls).
// Clear on the pouch's tabs, the Pad's pages and the map, and with no menu. (Bit 16 is pc_input's.)
#define TOTK_MENU_F_SUB 0x20000u
#define TOTK_MENU_SUBSCREEN(v) (((v) & TOTK_MENU_F_SUB) != 0)
#define TOTK_MENU_SCREEN(v) ((enum TotkMenuScreen)((v) & 0xff))
#define TOTK_MENU_SUB(v) (((v) >> 8) & 0xff)

#ifdef __cplusplus
}
#endif
