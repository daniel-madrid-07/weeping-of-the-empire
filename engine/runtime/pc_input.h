// pc_input.h: PC-native controls: the context the keyboard and mouse are read in, and the routing table from (context,
// PC input) to the game's action. pc_input.cpp; the table pc_scheme.inc.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The context. Stage 1: the UI part; the player states (climb, air, swim, aim, ability...) come with D1.
enum PcCtx {
    PCX_WORLD, PCX_OVERLAY, PCX_TITLE, PCX_SAVES, PCX_PAUSE, PCX_POPUP, PCX_PAD, PCX_MAP, PCX_OPTIONS, PCX_SHOP, PCX_MENU,
    PCX_DIALOGUE, PCX_CHOICE, PCX_CUTSCENE,
    PCX_TRANSITION,  // leaving a menu: until the focus lookup and the scene both say gameplay for
                      // 250 ms, every key is dead (a menu class context: the pointer stays released)
    // the player's states (stage 2: pc_ctx_player's push, mapped by the registered pc_ctx_from_player; WORLD = on foot)
    PCX_CLIMB, PCX_AIR, PCX_GLIDE, PCX_SWIM, PCX_AIM, PCX_ABILITY, PCX_HORSE, PCX_VEHICLE,
    PCX_LOADING,      // no rows: every key dead
#define PC_HAS_QUICKMENU 1  // (the rows test it)
    PCX_QUICKMENU,  // the L ability wheel / the D-pad strips: UI screens in the GAMEPLAY class, the
                      // world's rows (the key that opened it keeps its hold), the mouse as the R stick, LMB A, Esc B
    PCX_PADLOG,  // the Purah Pad on its Adventure Log page (the sub 1): the Pad's rows, J closes it
#define PC_HAS_CAMERA 1  // (the camera rows test it)
#define PC_CTX_CAMERA 1  // (the camera rows are in pc_scheme_gameplay.inc, f404465)
    PCX_CAMERA,  // the Purah Pad camera (the TOTK_TMS_CAMERA, or the player push): GAMEPLAY class, its own
                      // rows (doc 3.5 / 4.17: capture, self-portrait, album, quit, zoom), its own glyph set
#define PC_HAS_SCOPE 1  // (the Scope rows test it)
    PCX_SCOPE,  // the Scope (V, verb 52: doc 4.18): AppMap_00 is up, but it is gameplay
                      // class (the pointer stays captured, the mouse aims), its own rows (quit, place pin) and glyph set
#define PC_HAS_CONSTRUCT 1   // (Mineru's construct: pc_scheme_gameplay.inc's CONSTRUCT rows)
    PCX_CONSTRUCT,  // Mineru's construct: split from VEHICLE (the stick, the sled,
                      // the telescope), its own rows (the arms, the back part, guard, attach, get off) and HUD bar set
#define PC_HAS_PCX_RECIPE 1
    PCX_RECIPE,  // the cooking recipe book (RecipeCard_00 over the pause menu; the TMS_RECIPE): a menu, its own
                      // rows and glyph set
    PCX_COUNT
};
int pc_ctx(void);                   // the context at the last pad read (any thread)
int pc_ui_sub(void);  // the sub at the last decide (the pause menu's tab,...); -1 none
const char *pc_ctx_name(int ctx);
bool pc_ctx_is_ui(int ctx);         // title, menus, dialogue, choices
// controls.gameplay's live apply (pc_gameplay.cpp's F10 row): pc / on / 1 = pc_scheme_gameplay.inc's rows in the gameplay
// contexts, else the console layout there; the table rebuilds at the next read
void pc_input_gameplay_set(const char *value);
bool pc_scheme_on(void);            // the PC scheme (default) or the console layout (controls.scheme / TOTK_CONTROLS=console)
// The Sticky / Toggle / Filter Keys hotkeys (five Shifts, Num Lock held, right Shift held) off while the game is in front,
// as they were otherwise (4.11 P6: shim_win.c's WM_ACTIVATEAPP; at exit; pc_crash.cpp before a report)
void pc_input_focus(bool focused);
void pc_input_keys_restore(void);

// the screen in front, pushed at its update point (game thread): screen = enum TotkMenuScreen
// (totk_menu_cursor.h), sub its sub value (the pause menu's tab; 0xff none). Taken while under 250 ms old, else
// pc_ctx_decide reads totk_menu_screen() itself.
void pc_ctx_ui(uint32_t screen, uint32_t sub);
// The player's state, pushed once per game frame from the game thread (the TotK producer, D1); stale after 250 ms (then
// on foot unless a UI context is up). flags: PCP_*; ability: PCA_* while PCP_ABILITY. Which context a set of flags is
// is the gameplay side's: pc_ctx_player_map_register (none: on foot). A change between gameplay contexts takes effect
// after 30 ms of the same answer (4.10 point 5); UI changes at once.
enum PcPlayerFlag {
    PCP_CLIMB = 1u << 0, PCP_AIR = 1u << 1, PCP_GLIDE = 1u << 2, PCP_SWIM = 1u << 3, PCP_AIM = 1u << 4, PCP_GUARD = 1u << 5,
    PCP_ABILITY = 1u << 6, PCP_HORSE = 1u << 7, PCP_VEHICLE = 1u << 8, PCP_EVENT = 1u << 9, PCP_LOADING = 1u << 10,
    PCP_SCOPE = 1u << 11,  // (the Scope: the map screen it opens is PCX_SCOPE, not PCX_MAP)
    PCP_CONSTRUCT = 1u << 12   // (Mineru's construct, RideRobot 25: PCX_CONSTRUCT, not PCX_VEHICLE)
};
enum PcAbility { PCA_NONE, PCA_ULTRAHAND, PCA_FUSE, PCA_ASCEND, PCA_RECALL, PCA_AUTOBUILD, PCA_CAMERA };
void pc_ctx_player(uint32_t flags, uint8_t ability);
void pc_ctx_player_map_register(int (*from_player)(uint32_t flags, uint8_t ability));   // -> a PcCtx
uint32_t pc_ctx_player_flags(void);   // the last push (0 when stale)
uint8_t pc_ctx_ability(void);         // the last push's ability (PCA_NONE when stale)
// Virtual buttons: bit n of a 64-bit word = the game's own action bit n (PlayerVirtualKey, ExecutePlayerCalcKey; 27..63
// free for new split actions), set while its key is held in the context. The hook on ExecutePlayerCalcKey registers its
// two calls: hold(word) gets the held level at every change, tap(bits) the row's bit at each press (its latch: a tap
// shorter than a game frame still gives one press). Trigger / hold / release per frame are the hook's. Pads stay on the
// game's own path.
void pc_vbtn_writer_register(void (*hold)(uint64_t word), void (*tap)(uint64_t bits));
// Menu operations. fn is called on the input thread at a press; false: it cannot do that op now, and the router pulses
// the row's pad buttons instead (2 game frames, once per press).
enum PcMenuOp { PCOP_NONE, PCOP_CONFIRM, PCOP_BACK, PCOP_TAB_PREV, PCOP_TAB_NEXT, PCOP_TAB_GOTO, PCOP_CHOICE_PICK, PCOP_SCROLL,
                PCOP_MENU_GOTO };
// PCOP_MENU_GOTO: go to a menu page from wherever the player is, the chain (a tab_goto in the same screen; across
// screens close one, open the other, then the tab). arg: 0..8 a pause tab, PCMG_PAD + n the Purah Pad's page n (the map
// PCMG_PAD + 2, the Adventure Log PCMG_PAD + 1), PCMG_ITEM the last item tab used (never System; weapons on a fresh
// start), PCMG_CANCEL: stop a running chain (the router sends it at another key's press within 3 s). The world's rows
// too: with no menu the chain opens the screen itself. The page you are on already closes the menu (the first step
// only); Esc's System with a sub-screen open is declined, so the row's B backs out one level. Declined (the cursor off,
// no hook): the row's pad buttons, as for every 'O' op. (totk_menu_cursor.h: TOTK_MC_GOTO_PAGE / _INVENTORY / _CANCEL,
// the same values)
#define PC_HAS_MENU_GOTO 1
// PCMG_LAST: the pause menu on the last tab shown, System included (the TOTK_MC_GOTO_LAST); in the pause menu, the
// toggle closes it
enum { PCMG_PAD = 0x10, PCMG_ITEM = 0x20, PCMG_LAST = 0x30, PCMG_CANCEL = -1 };
void pc_menu_ops_register(bool (*fn)(int op, int arg));
// The quickmenus' pointer (the L wheel, the D-pad strips) at UI level: while the context is a quickmenu the camera gets
// no mouse; at every read the router hands the executor the raw counts since the last read (sensitivity not applied, y
// down +), the wheel notches (+ = away) and the quickmenu's sub (TMS_QM_*). true: taken (UI level). false (no hook in the
// key, the hook not alive, a screen it does not know): the counts are dropped (4.22: only the wheel turns a quickmenu)
// and the notches step the strips through hle_hid's quick-menu wheel.
void pc_quickmenu_feed_register(bool (*fn)(double dx, double dy, int notches, int sub));
// pc_mouse.cpp: the pointer is over one of its focus-box items (or a pause tab icon), so a left press that starts there
// is its click, not the Purah Pad map's drag / pin
void pc_mouse_cursor_over_item(bool over);
// pc_mouse.cpp: a pad is the device in use and the released pointer is hidden (hle_hid.cpp's prompts, 90bf2295) until
// the mouse moves on purpose or clicks; the hover ignores the pointer's motion meanwhile
bool pc_mouse_pointer_hidden(void);
#define PC_HAS_POINTER_HIDDEN 1

// Glyphs: the key a pad button's action has in the current context class (4.11 P2: two sets, the world's for gameplay,
// the pause menu's for every UI context; the live rewrite lands 250-500 ms late, so the set changes only with the class),
// first key of the row; 0 the layout's, -1 none. pc_glyph_sig changes with the set (the glyph epoch).
int pc_glyph_vk(int pad_bit);   // (pad_bit 0-15 the buttons, 16-19 / 20-23 the left / right stick's directions)
unsigned pc_glyph_sig(void);
// The quickmenu in front (the sub): 0 the ability wheel, 1-4 the item strips, 5 the Autobuild list
int pc_quickmenu_sub(void);
// Any thread (the strip slide, eui's thread): the quickmenu's opening keys are all let go, so it is closing (the game
// equips / picks on that release and leaves a few frames later; pc_route's g_qm_closing)
bool pc_quickmenu_closing(void);
#define PC_HAS_QM_CLOSING 1
// The same for a named set (a PcCtx whose table it is: PCX_WORLD = the gameplay set of the active Gameplay keys mode)
int pc_glyph_vk_in(int set, int pad_bit);
unsigned pc_glyph_sig_in(int set);
// The set the world's action guide (the HUD's prompt bar) draws: HORSE / GLIDE / SWIM / VEHICLE / ABILITY / AIM when in force
// (or the last one under a menu), else PCX_WORLD (GLIDE's "Close [B]" is Space, on foot's B is Shift; the horse's Go! /
// Dismount / Soothe). A jump's AIR and CLIMB stay on foot's; a mode with on foot's keys is PCX_WORLD; a quickmenu keeps its
// opener's; any thread.
int pc_glyph_game_set(void);
#define PC_HAS_GLYPH_GAME_SET 1
// A key press that is about to put up another context's glyphs names it at once: from then pc_glyph_vk / pc_glyph_sig
// / pc_glyph_game_set answer that context's set, until the context's own sets move (the game followed, or did
// something else) or 400 ms pass (the game refused). sub: a quickmenu's (0 the wheel, else a strip). Any thread.
void pc_glyph_intent(int ctx, int sub);
bool pc_glyph_intent_active(void);
// The glyph set in force is a quickmenu's item strip (its Select draws the mouse wheel; hle_hid)
bool pc_glyph_qm_strip(void);
// hle_fs: the live rewrite, run at once when an intent changes the epoch (no 20 ms poll, no settle look)
void pc_glyph_kick_register(void (*fn)(void));
#define PC_HAS_GLYPH_INTENT 1
// Every context's prompt keys against its routes: a prompt naming a key that produces nothing there counts (and, with log,
// is said); 0 = every prompt's key does what it says
int pc_glyph_audit(int log);
// The audit's runtime half for 'E' rows (another part reads the key: hle_hid's map click, the clicks): each such part
// registers whether it turns `vk` into the row's buttons in `ctx` right now; at an E key's press the router asks them, and
// one nobody answers is said once per context and key ("[pc] E key not answered": tools/engine/ check_pc_controls.py
// counts it). pc_external_check: that test for a press (1 answered or no button row, 0 not)
void pc_external_register(const char *who, bool (*answers)(int ctx, int vk));
int pc_external_check(int ctx, int vk);
// The System tab's controls diagram: the code point that draws a game button character (U+E040.. U+E068) as the
// gameplay set's key, through the prompts' own lookup (bindings, the gameplay mode, the console layout's keys where no
// row routes the button). With keys and mouse in use: a bank glyph's code point (U+F1xx), or the game's own pad glyph
// (U+F0xx) when that button has no key there (never a blank, never another set's key). Other styles, or no glyph bank:
// cp unchanged (the pad glyphs are the same in every context). pc_glyph_gameplay_epoch changes whenever an answer may
// have: redo the panes then. Any thread: a table per epoch behind a short lock.
#define PC_HAS_GLYPH_GAMEPLAY 1
uint16_t pc_glyph_gameplay_cp(uint16_t cp);
unsigned pc_glyph_gameplay_epoch(void);
// The same through a named context's set (a PcCtx: a Special Controls page that describes the Scope draws PCX_SCOPE's
// keys); pc_glyph_gameplay_cp is pc_glyph_cp_in(PCX_WORLD, cp). Its epoch per set likewise.
#define PC_HAS_GLYPH_CP_IN 1
uint16_t pc_glyph_cp_in(int set, uint16_t cp);
unsigned pc_glyph_epoch_in(int set);

// ---- Rebinding. Options' rows and F10's pages call these, never pc_settings_set, so they cannot disagree. Any thread
// (one mutex); a change is live (the table is rebuilt at the next read, the glyph epoch bumps when the prompts change)
// and saved with the settings' normal save. With the console scheme (controls.scheme = console) every call returns
// PCB_NOT_READY (capture_begin false); so does the SDL (Linux) path, which routes nothing.
//
// scope: a PcCtx (PCX_WORLD ... ) or a group. ROOT = the shared action (Options' rows: controls.<action>); a context or
// a group = an F10 override (controls.<scope>.<action>, scope = pc_ctx_name's name or gameplay / ui / menus).
#define PC_SCOPE_ROOT 100
#define PC_SCOPE_GAMEPLAY 101
#define PC_SCOPE_UI 102
#define PC_SCOPE_MENUS 103
// input: layout-independent. 1..255 = the Windows VK a US keyboard has at that key (the scan code mapped through the US
// layout: AZERTY's key under "A" is 'Q'), the mouse buttons as VK_LBUTTON / RBUTTON / MBUTTON / XBUTTON1 / XBUTTON2,
// Shift / Ctrl / Alt the generic VKs; PC_INPUT_WHEEL_UP / DOWN the wheel; 0 none. The router compares the same codes;
// pc_key_name shows what the active layout prints there.
#define PC_INPUT_WHEEL_UP 256
#define PC_INPUT_WHEEL_DOWN 257
// Stored: settings.ini, the key above = a comma list of canonical names (letters and digits by US position, hle_hid's
// names for the rest, "none" = deliberately unbound), packed (no holes), the first one the prompt's glyph. The most
// specific level wins: context > group > root > the scheme's row.
enum PcBindErr {
    PCB_NOT_READY = -1,
    PCB_UNKNOWN = -2,    // no such action at that scope
    PCB_FIXED = -3,  // the action's row is E (the / the map's own) or X (K rows, read through pc_bind_get,
                         // are rebindable); or the input is a FIXED / E / X
                         // row's in a reached context (no swap possible: that row is named in out[0])
    PCB_RESERVED = -4,   // F10 (the overlay, always the way back), F12 (shots), the Windows keys, PrintScreen
    PCB_LAST_KEY = -5,   // would leave UI confirm, UI back or the gameplay pause with no key
    PCB_CHAIN = -6,      // the swap would collide again, or B's colliding key comes from different levels in different
                         // reached contexts
    PCB_KIND = -7,       // the wheel (no release) on a held row (H, held G): only pulse kinds (P / O / F, G taps)
};
struct PcBindChange {   // one binding written (or, for PCB_FIXED, the row in the way)
    int scope;
    char action[32];
    int slot, old_input, new_input;
};
// slot 0 = the glyph key, 1-3 the extra keys; input 0 clears the slot (clearing slot 0 promotes slot 1). Swap on
// conflict: in each context the write reaches (the scope's contexts that have the action and no more specific override
// of it), an action B that has the input gets the action's old key of that slot, written at the level B's colliding
// key comes from (root or the scheme's row: root; a context / group override: that level). A dry run decides first: on
// any error nothing is written. A gameplay-file action while Gameplay keys = Console is checked against the PC
// gameplay table, as if it were active (it is stored and acts once Gameplay keys = PC). Exclusive pairs (a macro's
// trigger and a key it reads while running: {fire, aim}) may never share a key in any gameplay context where either
// exists: PCB_CHAIN, no swap, the other action in out[0]. Returns the number of changes written (out[0] the request,
// then the swaps; up to max), or a PcBindErr.
int pc_bind_set(int scope, const char *action, int slot, int input, struct PcBindChange *out, int max);
// The effective keys at that scope (up to max; returns the count or a PcBindErr). flags: PCB_F_*.
enum { PCB_F_OVERRIDDEN = 1, PCB_F_INACTIVE = 2, PCB_F_FIXED = 4 };   // own key at that level; a gameplay-file action
                                                                      // under Gameplay keys = Console; an E / X row
int pc_bind_get(int scope, const char *action, int *inputs, int max, unsigned *flags);
// Drops the scope's override of action (action NULL: every override at that scope, Options' "Restore defaults"), so
// it inherits again; the same swap / dry-run rule. Returns the changes written or a PcBindErr.
int pc_bind_reset(int scope, const char *action, struct PcBindChange *out, int max);
// The actions reachable at a scope (F10's pages), each with its PCB_F_* flags. Returns the count or a PcBindErr.
int pc_bind_actions(int scope, void (*fn)(const char *action, unsigned flags, void *user), void *user);
// Capture: the next key down (WM_KEYDOWN / WM_SYSKEYDOWN, so Alt is captured and the window menu does not take it;
// Shift / Ctrl / Alt on press), mouse button down, or wheel notch (PCC_WHEEL) becomes the result, swallowed: the
// overlay never gets it and the router holds it dead until released, so neither the game nor a menu sees the press or
// the release. Esc cancels unless PCC_ESC (Esc is bound from the curated list); focus loss cancels.
enum { PCC_WHEEL = 1, PCC_ESC = 2 };
bool pc_input_capture_begin(unsigned flags);
int pc_input_capture_poll(void);     // 0 waiting, > 0 the input, -1 cancelled (or not capturing)
void pc_input_capture_cancel(void);
// pc_mouse.cpp's subclass of the game window, ahead of everything else: every message first (true: swallowed), and the
// raw wheel's notches while a capture wants them (true: the capture's, not the game's)
bool pc_input_capture_msg(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
bool pc_input_capture_wants_wheel(void);
bool pc_input_capture_wheel(int notches);
// The key's name for the player, UTF-8, static storage. lang: the game's language code (en, es, fr, de, it, nl, ru, ja,
// ko, zh-Hans, zh-Hant). Named keys (Space, Shift, Ctrl, Alt, Tab, Enter, Esc, Backspace, the arrows, the mouse
// buttons, the wheel, the numpad, F1-F24) translated; letters, digits and punctuation what the active layout prints at
// that key; anything else GetKeyNameTextW's.
const char *pc_key_name(int input, const char *lang);

#ifdef __cplusplus
}

// hle_hid.cpp keyboard_read, once per read (input thread). bits: hle_hid's action bits (0-15 the Npad buttons, 16-19
// move left / up / right / down). claimed[vk]: the layout (TOTK_KEYBOARD_LAYOUT's bindings) must not read that key now:
// the table routes it here, or it is held dead (pressed in another context), or the context takes every key.
struct PcRouteIn {
    int ctx;
    uint64_t now_ms;
    bool (*down)(int vk, void *user); void *user;   // a key or mouse button down (mouse buttons logical)
    int wheel;                                       // notches since the last read (+ = away from the user)
    int gen;                                         // hle_hid's g_ctl_gen: the settings changed, rebuild
    uint64_t (*layout_bits)(int vk, void *user);     // the console layout's action bits for a key (a transition's replay)
    uint64_t frame;                                  // presents so far: pulses last 2 game frames (4.11 P4)
    bool refocus = false;                            // the first read in front after reads behind: keys down are dead
};
struct PcRouteOut {
    uint64_t bits; bool claimed[256]; bool wheel_taken;
    bool stick[2]; int32_t sx[2], sy[2];   // a macro's stick (0 left, 1 right), over the layout's and the pad's
    uint64_t vbtn;                         // the G rows' word this read (bit n = the game's action bit n)
};
// Macros (kind M rows: op = PCOP_NONE, arg = the macro id): a player, registered by id (pc_gameplay.cpp). Up to 4 run
// at once, one per key (PcMacroIn.input tells them apart); a new press of a key replaces that key's run. begin at
// the row's press; tick at every read while it runs (count steps in game frames, at least 2 per step: 4.11 P4) with the
// live keys and the mouse it asked for; abort when the context changes to one where its key has another action (4.11
// P1), or when tick returns false (done). Its pad bits and sticks are ORed / laid over the read's.
struct PcMacroIn {
    int ctx; int input;                        // the context it runs in, the key that started it
    uint64_t frame, now_ms;
    bool (*down)(int vk, void *user); void *user;
    double mouse_dx, mouse_dy; int wheel;      // (only while the last tick set take_mouse: then the camera gets none)
};
// claim (MAC_BOW owns LMB while RMB is held): up to 4 virtual keys (0 = none) whose edges are the macro's while its
// ticks keep naming them. From the next read on, their rows do nothing (a press of one is dead, one held with its row
// lets go) and the layout does not read them; the macro reads them through PcMacroIn.down. A key still held when the
// claim ends stays dead until released, so it never fires the row it would have had.
struct PcMacroOut { uint64_t bits; bool stick[2]; int32_t sx[2], sy[2]; bool take_mouse; int claim[4]; };
struct PcMacroOps {
    void (*begin)(int ctx, int input);
    bool (*tick)(const PcMacroIn *in, PcMacroOut *out);   // false: done
    void (*abort)(int input);   // (the run of that key ends)
};
void pc_macro_register(int id, const PcMacroOps *ops);
void pc_route(const PcRouteIn &in, PcRouteOut *out);
// hle_hid.cpp: the context's inputs; decided there once per pad read from these (pc_ctx_decide logs a change) quick_hint:
// what the buttons the game saw last say about a quickmenu (0 L held: the wheel, 1 a strip's d-pad, -1 none): a TMS_OTHER
// screen while one is held is that quickmenu
int pc_ctx_decide(bool overlay, bool title_phase, uint32_t menu_screen, int screen_guess, int quick_hint);
bool pc_ctx_force(const char *name);   // TOTK_CTL "ctx <name>|auto"
// TOTK_CTL "player <flags> [ability] | off": the player push faked and held (boots before the producer exists)
void pc_ctx_player_fake(bool on, uint32_t flags, uint8_t ability);
#endif
