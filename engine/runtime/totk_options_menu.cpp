// totk_options_menu.cpp: PC rows in the game's own Options screen.
//
// The screen is data-driven: OptionsMgr::initialize (main+0xf6092c) builds one 0x28-byte entry per row of
// UI/OptionSetTable/OptionSetTable.game__ui__OptionSetTable.bgyml, and UISystemOptionScreen clones a toggle or
// selection part for each entry it finds there. Through gen.py --hook (GEN_MAIN in tools/engine/build.py):
//  - initialize: after the game's entries, kRows are appended (OptionType 50 and up), so the screen gets their rows;
//  - the five row controllers: the game's own code for the game's rows, ours for ours (ctl below: the value is a
//    pc_settings key, not a member of the save's option struct).
// The layouts and the UI calls are totk_options_ui.h's; guest code is reached through engine/runtime/guest/guest.h.
// The texts (label "10%02d", choices "10%02d_%02d", caption "%04d" of LayoutMsg/SystemOption_00.msbt) come from the
// overlay set games/totk/overlays/options_menu.py makes; its ENTRIES has the same types and choice order.
// Without that set the row shows no text; the game itself is unaffected.
// TOTK_OPTIONS_MENU=0: the game's code only (no rows added, the controllers run f_<off>_orig).
#include "totk_options_ui.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <excpt.h>  // GetExceptionCode (cbdiag)
#endif
#include <vector>

#include "pc.h"  // pc_fault_guard (cbdiag)
#include "pc_bind_ui.h"
#include "pc_overlay.h"
#include "pc_input.h"
#include "pc_lang.h"
#include "pc_settings.h"
#include "totk_menu_cursor.h"

using namespace totk_options_ui;

// Our rows' values (defined below, under this name)
namespace game::ui::pc_rows {
bool owns(s32 type);
s32 choice(s32 type);                 // the current choice's index, -1: none matches
void set_choice(s32 type, s32 choice);
s32 toggle(s32 type);                 // 0 / 1, -1: unknown
void set_toggle(s32 type, bool on);
}  // namespace game::ui::pc_rows
namespace pc_rows = game::ui::pc_rows;

namespace {

struct PcRow {
    s32 type;                   // OptionType: 50 and up (two digits in "10%02d")
    const char *key;            // its pc_settings row
    const char *const *values;  // the value of each choice, in the order of the overlay set's choice texts
};
// The value lists. A choice row (the setting has values) must have every one of these among its values; a slider
// (min..max) must have them in its range, and the choice shown is the nearest one.
// "steady" (the highest rate the scene holds, in whole refreshes of the monitor) so Options shows every frame-rate mode
// F10 has, a first run's detected one included
const char *const kFps[] = {"30", "60", "90", "120", "unlocked", "steady", nullptr};
const char *const kPreset[] = {"low", "medium", "high", "ultra", "ultra+", "custom", nullptr};
const char *const kScale[] = {"0.67", "0.8", "0.9", "1", "1.5", "2", "auto", nullptr};
// Mouse sensitivity: 0.1 steps up to 2 (10 % or less a step around the default), coarser above, up to 20
const char *const kSens[] = {"0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9", "1", "1.1", "1.2", "1.3",
                             "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2", "2.2", "2.4", "2.6", "2.8", "3", "3.2",
                             "3.4", "3.6", "3.8", "4", "4.5", "5", "5.5", "6", "6.5", "7", "7.5", "8", "9", "10", "11",
                             "12", "13", "14", "15", "16", "17", "18", "19", "20", nullptr};
// The mouse while the bow is drawn: a multiplier
const char *const kAimScale[] = {"0.1", "0.15", "0.2", "0.25", "0.3", "0.35", "0.4", "0.45", "0.5", "0.55", "0.6", "0.65", "0.7", "0.75", "0.8", "0.85", "0.9", "0.95", "1", "1.05", "1.1", "1.15", "1.2", "1.25", "1.3", "1.35", "1.4", "1.45", "1.5", "1.6", "1.7", "1.8", "1.9", "2", "2.25", "2.5", "2.75", "3",
                                 nullptr};
// The shortest a movement key's tap lasts (the controls.key_min_hold_ms, 8eef8190): 0.. 250 ms, steps of 10
const char *const kKeyHold[] = {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100", "110", "120", "130",
                                "140", "150", "160", "170", "180", "190", "200", "210", "220", "230", "240", "250",
                                nullptr};
const char *const kGlyphs[] = {"auto", "switch", "xbox", "ps", "kbm", nullptr};
const char *const kFast[] = {"0", "1", nullptr};
const char *const kOffOn[] = {"0", "1", nullptr};
const char *const kVsync[] = {"", "off", nullptr};            // On (default), Off
const char *const kVrr[] = {"", "on", nullptr};               // Off (default), On
const char *const kOffYes[] = {"", "1", nullptr};             // Off (default), On
const char *const kOnDef[] = {"", "0", nullptr};              // On (default), Off
const char *const kHdr[] = {"", "hdr10", "scrgb", nullptr};   // Off (default), HDR10, scRGB
const char *const kHdrPeak[] = {"400", "500", "600", "700", "800", "1000", "1200", "1500", "2000", "2500", "3000", "4000",
                                nullptr};                     // nits, a 400 .. 4000 slider (step 50), default 1000
const char *const kHdrPaper[] = {"80", "100", "120", "140", "160", "180", "200", "220", "250", "300", "400", "500",
                                 nullptr};                    // nits, an 80 .. 500 slider (step 10), default 200
const char *const kGameplay[] = {"console", "pc", nullptr};  // Console layout, PC
const char *const kGyro[] = {"0.25", "0.5", "0.75", "1", "1.25", "1.5", "1.75", "2", "2.5", "3", nullptr};
const char *const kFov[] = {"0.8", "0.9", "1", "1.1", "1.2", "1.3", "1.4", "1.5", nullptr};  // of a 0.5 .. 2 slider
const char *const kWide[] = {"16:9", "window", nullptr};      // the game's (default), the window's shape
const char *const kWindowMode[] = {"windowed", "borderless", "exclusive", nullptr};  // borderless: the default
// A row whose choices are its setting's own values / names (pc_window.cpp builds them from the hardware in its static
// initializer, before any game code, and never changes them). Only choice 0 ("" Remember / the desktop's) has overlay
// texts; every other choice shows the setting's name (show_dynamic).
const char *const kDynamic[] = {"", nullptr};
// Must match ENTRIES in games/totk/overlays/options_menu.py (types and values); the screen shows them in this order
const PcRow kRows[] = {
    {108, "display.window_mode", kWindowMode},  // live
    {109, "display.monitor", kDynamic},            // live: the monitors at start, "Monitor 2: <model>, 2560 x 1440"
    {110, "display.exclusive_size", kDynamic},     // live: the start monitor's modes
    {111, "display.exclusive_refresh", kDynamic},  // live: its refresh rates
    {50, "display.fps_cap", kFps},              // live (pc_settings.cpp fps_apply -> hle_vi.cpp, present d998930a)
    {51, "graphics.preset", kPreset},           // live: sets its members (some of them restart)
    {52, "graphics.render_scale", kScale},      // restart
    {64, "graphics.dlss", kOffYes},  // restart
    {65, "graphics.frame_gen", kOffYes},  // restart
    {67, "graphics.fov", kFov},  // live
    {68, "graphics.aspect_ratio", kWide},  // restart (totk_engine.c, the ultrawide; patched main only)
    {117, "graphics.native_ui", kOnDef},  // restart
    {53, "controls.mouse_sensitivity", kSens},  // live (hle_hid.cpp), a slider of 0.1 .. 20
    {116, "controls.mouse_aim_scale", kAimScale},  // live: the mouse while the bow is drawn
    {119, "controls.key_min_hold_ms", kKeyHold},  // live: the W / A / D tap slide
    {54, "controls.glyphs", kGlyphs},  // live (hle_hid.cpp): every prompt, the tabs and HUD too
    {55, "game.fast_travel", kFast},            // restart (games/totk/engine/totk_engine.c; patched main only)
    {56, "controls.menu_cursor", kOffOn},  // live
    {57, "display.vsync", kVsync},              // live (pc_window.cpp, present)
    {58, "display.vrr", kVrr},                  // live (pc_window.cpp, present)
    {60, "controls.gyro_scale", kGyro},  // live, a slider of 0.25.. 3
    {61, "display.hdr", kHdr},                  // live (pc_window.cpp, present: a new swapchain at the next frame)
    {62, "display.hdr_peak", kHdrPeak},         // live (present)
    {63, "display.hdr_paper", kHdrPaper},       // live (present)
    {114, "game.pause_unfocused", kOffYes},  // live
    {115, "audio.mute_unfocused", kOffOn},  // live
    {66, "controls.gameplay", kGameplay},  // live: the key block's first row
};
constexpr s32 kKeyBlockType = 66;  // a separator goes in front of it

// The key rows, after the separator: generated from pc_bind_pages.inc (key_lines), every entry of its on-foot pages (kKeyPages) at a group or
// WORLD scope (and kKeyCtxRows'), in the file's order, at that entry's scope: the same binding F10's Keys tab shows (the other contexts'
// entries, Horse and Menus are F10's only). A page change moves the rows with it. A on a row captures the next key or mouse button
// (pc_input_capture_*) and binds it (pc_bind_set: a key another action has swaps with it, every key row is shown again); the choice pane
// shows the key's name, written directly (no message id). kKeyRows: each action's OptionType, whose label and caption the overlay has
// ("10%02d", "%04d"; types of 100 and up are "10100"...: no clash with the game's 1000.. 1013). Must match KEY_ENTRIES in options_menu.py. An
// action the pages gain before it has a type here gets kKeyFallback + its index and its page label written into the row (English).
struct KeyRow {
    s32 type;
    const char *action;  // pc_scheme_gameplay.inc / pc_scheme.inc
};
// (76 was parry: gone from the PC scheme; 74 was aim, gone with 4.23 (A): LMB attacks and draws the bow; 75 was guard on
// Q, gone with 4.26: RMB's lock_on is the shield too; their texts stay in the overlay for older exes)
const KeyRow kKeyRows[] = {
    {87, "move_up"},      {88, "move_down"},   {89, "move_left"},  {90, "move_right"},  {70, "jump"},
    {71, "sprint"},       {85, "crouch"},      {91, "dive"},       {73, "attack"},
    {86, "lock_on"},     {106, "holster"},    {112, "bow_toggle"},
    {92, "throw"},        {93, "weapon_next"}, {94, "weapon_prev"},
    {72, "interact"},     {95, "weapons"},     {96, "shields"},    {97, "bows"},        {98, "materials"},
    {99, "scope"},        {100, "whistle"},    {84, "inventory"},  {83, "map"},         {101, "adventure_log"},
    {102, "pause"},       {77, "ultrahand"},   {78, "fuse"},       {79, "ascend"},      {80, "recall"},
    {81, "autobuild"},    {82, "camera"},      {103, "ability_wheel"}, {104, "quit"},
    {113, "leave"},
};
const char *const kKeyPages[] = {"Movement", "Combat", "Items and tools", "Abilities"};
// The page entries at a context's own scope that get a row too (the rest are F10's): ABILITY's "leave" (Tab: its own
// action, so the Options row of ROOT's "quit" no longer moves it). Mirrored by check_pages in the overlay
struct KeyCtxRow {
    int scope;
    const char *action;
};
const KeyCtxRow kKeyCtxRows[] = {{PCX_ABILITY, "leave"}};
constexpr s32 kKeyFallback = 200;
constexpr size_t kMaxKeys = 64;
// The most entries the Options screen gets (the game's 16 included). Every drawn row takes ~37 KB of eui's per-frame
// ui2d constants (cbdiag), so with every row drawn 69 overran the 1703936-byte half and 48 peaked at 84% of it. With
// the cull pass (namespace cull) only the rows near the screen are drawn: 65 entries peaked at 1074432, 63% (cull2),
// and a longer list only adds hidden rows. 80 holds the patched key with the patch sets (35 settings rows incl. The 4
// display rows, the 35 key rows and the reset row: 75) with room. TOTK_OPTIONS_CULL=0 draws every row again, and then
// the old 48 holds.
constexpr u32 kSafeEntriesCulled = 80, kSafeEntriesDrawn = 48, kSafeEntriesZoned = 120;
// The row that stands for the key rows when they don't fit: A opens F10 at its Keys tab (pc_overlay_open_keys). Its
// texts: KEY_LINK in options_menu.py
constexpr s32 kKeyLinkType = 105;
// The last row of the key block: every key back to the PC defaults (pc_bind_reset of each scope the key rows use).
// The first confirm asks again (KS_CONFIRM_RESET), a second within kResetWindow resets. Texts: KEY_RESET in
// options_menu.py. No button name anywhere: on PC the confirm is Enter / E / a click.
constexpr s32 kKeyResetType = 107;
std::atomic<bool> g_key_link{false};  // append_rows added it
std::atomic<bool> g_key_reset{false};  // likewise

// Sections. The screen shows one section's rows at a time: the first row of every section is the section row
// (kZoneType, its choices the sections' names: ZONES in options_menu.py), whose left / right (the D-pad, the arrow
// keys, a click on its < >) or confirm shows another section. kZoneRows: each section's rows, in the order shown (the
// game's own types 0.. 13 and ours); a key row, the keys link and the reset row are Keys'; a row no section lists (a
// new one) goes to General, at its end, never lost. The game's separators are left out (a section is short).
// TOTK_OPTIONS_ZONES=0: the one long list again.
enum Zone : s32 { Z_GENERAL, Z_DISPLAY, Z_GRAPHICS, Z_AUDIO, Z_CONTROLS, Z_KEYS, Z_INTERFACE, Z_COUNT };
constexpr s32 kZoneType = 118;
struct ZoneRow {
    s32 type, zone;
};
const ZoneRow kZoneRows[] = {
    // General: amiibo (game 0), fast travel, pause when the window loses focus
    {0, Z_GENERAL}, {55, Z_GENERAL}, {114, Z_GENERAL},
    // Display: the window, the monitor, frame rate and sync, HDR, the picture's shape
    {108, Z_DISPLAY}, {109, Z_DISPLAY}, {110, Z_DISPLAY}, {111, Z_DISPLAY}, {50, Z_DISPLAY}, {57, Z_DISPLAY},
    {58, Z_DISPLAY}, {61, Z_DISPLAY}, {62, Z_DISPLAY}, {63, Z_DISPLAY}, {68, Z_DISPLAY},
    // Graphics
    {51, Z_GRAPHICS}, {52, Z_GRAPHICS}, {117, Z_GRAPHICS}, {64, Z_GRAPHICS}, {65, Z_GRAPHICS}, {67, Z_GRAPHICS},
    // Audio: voice language (game 11), muted when the window loses focus
    {11, Z_AUDIO}, {115, Z_AUDIO},
    // Controls: the scheme, the mouse, the game's camera / gyro / jump / Ultrahand rows (1 2 3 4 5 12 13)
    {66, Z_CONTROLS}, {53, Z_CONTROLS}, {116, Z_CONTROLS}, {119, Z_CONTROLS}, {56, Z_CONTROLS}, {1, Z_CONTROLS},
    {2, Z_CONTROLS}, {3, Z_CONTROLS}, {4, Z_CONTROLS}, {60, Z_CONTROLS}, {5, Z_CONTROLS}, {12, Z_CONTROLS},
    {13, Z_CONTROLS},
    // Interface: the HUD (game 8), the minimap (7), the message window (10), ruby text (9), the button icons
    {8, Z_INTERFACE}, {7, Z_INTERFACE}, {10, Z_INTERFACE}, {9, Z_INTERFACE}, {54, Z_INTERFACE},
};
struct KeyLine {
    s32 type;
    const PcPageEntry *entry;  // its pc_bind_pages.inc entry: scope, action, label
    bool labelled;             // the overlay has its texts (else the page's label is written into the row)
};
struct KeyLines {
    KeyLine at[kMaxKeys];
    size_t n = 0;
    KeyLines() {
        int count = 0;
        const PcPageEntry *const e = pc_bind_page_entries(&count);
        // TOTK_OPTIONS_KEYS_MAX=N: the first N rows only (a boot that looks for the screen's row budget)
        const char *const cap = getenv("TOTK_OPTIONS_KEYS_MAX");
        const size_t want = cap && *cap ? strtoul(cap, nullptr, 10) : kMaxKeys;
        const size_t most = want < kMaxKeys ? want : kMaxKeys;
        for (int i = 0; i < count && n < most; i++) {
            bool page = false;
            for (const char *p : kKeyPages) page = page || !strcmp(e[i].page, p);
            const int s = e[i].scope;
            bool row = s == PC_SCOPE_ROOT || s == PC_SCOPE_GAMEPLAY || s == PCX_WORLD;
            for (const KeyCtxRow &c : kKeyCtxRows) row = row || (c.scope == s && !strcmp(c.action, e[i].action));
            if (!page || !row) continue;
            s32 type = kKeyFallback + static_cast<s32>(i);
            bool labelled = false;
            for (const KeyRow &k : kKeyRows)
                if (!strcmp(k.action, e[i].action)) type = k.type, labelled = true;
            for (size_t j = 0; j < n; j++)  // the action's second entry (another scope): its own type
                if (at[j].type == type) type = kKeyFallback + static_cast<s32>(i), labelled = false;
            at[n++] = {type, &e[i], labelled};
        }
    }
};
const KeyLines &key_lines() {
    static const KeyLines lines;
    return lines;
}
// "1069_%02d": the key rows' status texts (no row of type 69); the order of STATUS in options_menu.py
enum KeyStatus {
    KS_PRESS, KS_UNBOUND, KS_NOT_READY, KS_LAST_KEY, KS_RESERVED, KS_FIXED, KS_CHAIN, KS_KIND, KS_UNKNOWN,
    KS_SWAPPED,        // "Swapped:" before the key a swap gave the other action's row (for 2 s; no comma here:
                       // check_c_table counts this enum's commas)
    KS_CONFIRM_RESET,  // the reset row after the first confirm
    KS_RESET_DONE      // the reset row after the reset
};

bool enabled() {
    static const bool on = getenv("TOTK_OPTIONS_MENU") && strcmp(getenv("TOTK_OPTIONS_MENU"), "0") != 0;
    return on;
}

const pc_setting *setting_of(const char *key) {
    for (int i = 0, n = pc_settings_count(); i < n; i++) {
        const pc_setting *d = pc_settings_at(i).def;
        if (!strcmp(d->key, key)) return d;
    }
    return nullptr;
}

u32 count_of(const char *const *v) {
    u32 n = 0;
    while (v && v[n]) n++;
    return n;
}

bool has_value(const pc_setting *d, const char *value) {
    for (u32 i = 0; d->values[i]; i++)
        if (!strcmp(d->values[i], value)) return true;
    return false;
}

// Every value of the row is one the setting takes
bool fits(const PcRow &r, const pc_setting *d) {
    for (u32 i = 0; r.values[i]; i++) {
        // a slider's bounds are floats: compared as a float, "0.1" is in 0.1f .. (as a double it is below it)
        const float v = static_cast<float>(atof(r.values[i]));
        if (d->values ? !has_value(d, r.values[i]) : !(v >= d->min && v <= d->max))
            return false;
    }
    return true;
}

// The row of ours for an OptionType with its setting, else null. A row whose setting is missing or does not take its
// values is left out (initialize does not add it); checked once, the settings table is fixed after start.
const PcRow *row_of(s32 type, const pc_setting **def) {
    struct Checked {
        const pc_setting *def[sizeof kRows / sizeof kRows[0]];
        Checked() {
            for (size_t i = 0; i < sizeof kRows / sizeof kRows[0]; i++) {
                const pc_setting *d = setting_of(kRows[i].key);
                def[i] = d && fits(kRows[i], d) ? d : nullptr;
            }
        }
    };
    static const Checked checked;
    for (size_t i = 0; i < sizeof kRows / sizeof kRows[0]; i++) {
        if (kRows[i].type != type || !checked.def[i]) continue;
        if (def) *def = checked.def[i];
        return &kRows[i];
    }
    return nullptr;
}

// The choice for the setting's current value (a preset row's value is what its members make it, live_value); a
// slider's nearest choice; -1 if none
// A row's values: its own list, or a kDynamic row's setting's (see kDynamic)
const char *const *values_of(const PcRow &r, const pc_setting *d) {
    return r.values == kDynamic && d && d->values ? d->values : r.values;
}
bool dynamic(const PcRow &r) { return r.values == kDynamic; }

s32 index_of(const PcRow &r, const pc_setting *d) {
    const char *value = d->live_value ? d->live_value() : pc_settings_get(r.key);
    if (!value) return -1;
    const char *const *const values = values_of(r, d);
    s32 best = -1;
    double dist = 0;
    for (u32 i = 0; values[i]; i++) {
        if (!strcmp(values[i], value)) return static_cast<s32>(i);
        if (d->values) continue;
        const double e = atof(values[i]) - atof(value);
        if (best < 0 || e * e < dist) best = static_cast<s32>(i), dist = e * e;
    }
    return best;
}


// TOTK_OPTIONS_KEYS=demo: while the rebinding API answers PCB_NOT_READY, every key row shows "Space / Ñ" through the
// same direct-text path as a real key name (a boot that checks that path before the API exists)
bool key_demo() {
    static const bool on = getenv("TOTK_OPTIONS_KEYS") && !strcmp(getenv("TOTK_OPTIONS_KEYS"), "demo");
    return on;
}

// The key block is shown once the rebinding API answers (the stubs return PCB_NOT_READY: then every key row would
// read "Not available yet"), or with TOTK_OPTIONS_KEYS=1 or demo (a boot that checks the rows themselves)
bool key_block_shown() {
    static const bool on = [] {
        const char *e = getenv("TOTK_OPTIONS_KEYS");
        if (e && *e) return strcmp(e, "0") != 0;
        int in[4];
        unsigned flags = 0;
        const KeyLines &k = key_lines();
        return k.n && pc_bind_get(k.at[0].entry->scope, k.at[0].entry->action, in, 4, &flags) != PCB_NOT_READY;
    }();
    return on;
}

// The key row of an OptionType: its index in key_lines(), -1 if none
int key_index(s32 type) {
    const KeyLines &k = key_lines();
    for (size_t i = 0; i < k.n; i++)
        if (k.at[i].type == type) return static_cast<int>(i);
    return -1;
}
bool key_row(s32 type) { return key_index(type) >= 0; }
const char *action_of(size_t i) { return key_lines().at[i].entry->action; }
int scope_of(size_t i) { return key_lines().at[i].entry->scope; }

// ---------------------------------------------------------------- sections (kZoneRows)
// The screen asks OptionsMgr for its rows only through two functions: the count of shown entries (0x1a06840) and the
// n-th shown one (0x1a068cc), each walking mgr->count entries from mgr->entries and skipping those whose visibleHash
// GameData bool is false. Their callers are the screen's creation (0x1ad7a78, the rows are built there, once), two of
// its own methods (vtable slots 45 and 88) and its rebind of every row (0x1981214). The hooks run them on a view: an
// OptionsMgr whose entries are the section's, copies of the real ones (each a 0x28-byte value; choiceIds shared), so
// the visibility test, the order and every field are the game's, and the section is all a screen ever sees.
// Every controller still finds its entry by type in the real list (FindEntry: step, decide).
namespace zones {
bool hooked();  // the key has hook_f_1a06840 / hook_f_1a068cc (below the hooks)
bool on() {
    static const bool v = [] {
        const char *e = getenv("TOTK_OPTIONS_ZONES");
        return enabled() && hooked() && !(e && !strcmp(e, "0"));
    }();
    return v;
}
std::atomic<s32> g_zone{Z_GENERAL};    // the section the open (or last) Options screen shows
std::atomic<s32> g_pending{-1};        // the section the next one shows (-1: the same), taken as it is built
// The screen's creation (0x1ad7a78) asks for the count first, its call returning to 0x1ad7af4: a pending section
// becomes the shown one there and only there, so a screen still closing (its update reads the count / index too)
// never sees its rows change under it, whatever eui does with a closed screen
constexpr u64 kCreateCountReturn = guest_bridge::kEngineMain + 0x1ad7af4;
OptionsMgr g_view[Z_COUNT];
const OptionEntry *g_src;  // the real list the views were made from
u32 g_src_n;
bool g_ok;                 // the section row is in it

s32 zone_of(s32 type) {
    for (const ZoneRow &z : kZoneRows)
        if (z.type == type) return z.zone;
    if (key_row(type) || type == kKeyLinkType || type == kKeyResetType) return Z_KEYS;
    return Z_GENERAL;
}
bool listed(s32 type) {
    for (const ZoneRow &z : kZoneRows)
        if (z.type == type) return true;
    return false;
}

// One array per section: its section row, the rows kZoneRows lists for it in that order, then its other rows in the
// list's order (separators left out). Never freed: a screen holds entries of the view it was built from, and a new
// list (a second OptionsMgr) only makes new arrays.
void build(const OptionsMgr *real) {
    const OptionEntry *const e = real->entries;
    const u32 n = real->count;
    const OptionEntry *head = nullptr;
    for (u32 i = 0; i < n; i++)
        if (e[i].type == kZoneType && e[i].buttonType == 1) head = &e[i];
    g_src = e, g_src_n = n, g_ok = head != nullptr;
    if (!g_ok) return (void)printf("[options-menu] sections: no section row in the list; one list\n");
    for (s32 z = 0; z < Z_COUNT; z++) {
        std::vector<OptionEntry> v{*head};
        for (const ZoneRow &r : kZoneRows)
            for (u32 i = 0; r.zone == z && i < n; i++)
                if (e[i].type == r.type && e[i].buttonType != 3) v.push_back(e[i]);
        for (u32 i = 0; i < n; i++)
            if (e[i].buttonType != 3 && e[i].type != kZoneType && !listed(e[i].type) && zone_of(e[i].type) == z)
                v.push_back(e[i]);
        // Conditional entries (visibleHash != 0: shown only while that GameData bool is set) go last, in their order:
        // the screen's slot 88 and its rebind (0x1981214) bind rows by SHOWN entries with a counter per button type,
        // which a hidden one does not advance, so a hidden entry with a later shown one of its type moved every later
        // row's label and value by one. The game's own list keeps them at the end; the sections' reorder must too
        const auto shown_first = std::stable_partition(v.begin() + 1, v.end(), [](const OptionEntry &x) { return x.visibleHash == 0; });
        if (shown_first != v.end())
            printf("[options-menu] section %d: %zu conditional row(s) moved to its end\n", z, (size_t)(v.end() - shown_first));
        OptionEntry *const a = new OptionEntry[v.size()];
        memcpy(a, v.data(), v.size() * sizeof(OptionEntry));
        g_view[z].count = static_cast<u32>(v.size());
        g_view[z]._4 = 0;
        g_view[z].entries = a;
        printf("[options-menu] section %d: %zu rows (its section row included)\n", z, v.size());
    }
}

// The view for the open screen, or null (sections off, or no section row: the game's list as it is). lr: the call's
// return address (the creation's count call takes a pending section)
const OptionsMgr *view(const OptionsMgr *real, u64 lr) {
    if (!on() || !real || !real->entries || !real->count) return nullptr;
    if (real->entries != g_src || real->count != g_src_n) build(real);
    if (!g_ok) return nullptr;
    if (lr == kCreateCountReturn) {
        const s32 p = g_pending.exchange(-1);
        if (p >= 0 && p < Z_COUNT) {
            g_zone = p;
            printf("[options-menu] section %d: a new Options screen shows it\n", static_cast<int>(p));
        }
    }
    const s32 z = g_zone.load();
    return &g_view[z >= 0 && z < Z_COUNT ? z : Z_GENERAL];
}

// An entry's position on its screen: in the current view, else its own index (cull's opening view)
u32 shown_index(const OptionEntry *entry) {
    if (!entry) return 0;
    if (g_ok) {
        const s32 z = g_zone.load();
        const OptionsMgr &v = g_view[z >= 0 && z < Z_COUNT ? z : Z_GENERAL];
        if (v.entries && entry >= v.entries && entry < v.entries + v.count) return static_cast<u32>(entry - v.entries);
    }
    return entry->index;
}
}  // namespace zones

// TOTK_UI_CBDIAG=1: a thread that samples eui's per-frame constant buffers and logs their peak use against their size.
// Read only; a sample can land anywhere in the frame, so the peak is a lower bound.
namespace cbdiag {
bool plausible(u64 p) { return p >= 0x10000 && p < (1ull << 48) && !(p & 7); }
bool in_main(u64 p) { return p >= 0x7100000000ull && p < 0x7110000000ull; }
// One nn::font::GpuBuffer as read: size +0x18, buffer count +0x28, index +0x2c, the bump offset +0x40 (through a
// pointer if bit 0 of +0)
struct Buf {
    u64 used, size;
    u32 count, index, flags;
};
struct Sample {
    u64 cb;
    Buf buf[2];  // ui2d (+0x120), font (+0x170)
};
// Every pointer from the global down, again each sample (a load can rebuild the UI system), under __try: a stale
// pointer is no sample, not a crash (pc.h: rt_main's handler is vectored, so pc_fault_guard brackets the read)
bool sample_try(Sample *out) {
#ifdef _WIN32
    __try {
#endif
        u64 sm = *reinterpret_cast<const volatile u64 *>(guest_mem::guest_data<u64>(0x10045596a8));
        if (!plausible(sm)) return false;
        if (!in_main(*reinterpret_cast<const volatile u64 *>(sm))) {  // the slot that holds the instance
            sm = *reinterpret_cast<const volatile u64 *>(sm);
            if (!plausible(sm) || !in_main(*reinterpret_cast<const volatile u64 *>(sm))) return false;
        }
        const u64 cb = *reinterpret_cast<const volatile u64 *>(sm + 0x428);
        if (!plausible(cb)) return false;
        out->cb = cb;
        for (int k = 0; k < 2; k++) {
            const u64 gb = cb + (k ? 0x170 : 0x120);
            Buf &b = out->buf[k];
            b.flags = *reinterpret_cast<const volatile u32 *>(gb);
            b.size = *reinterpret_cast<const volatile u64 *>(gb + 0x18);
            b.count = *reinterpret_cast<const volatile u32 *>(gb + 0x28);
            b.index = *reinterpret_cast<const volatile u32 *>(gb + 0x2c);
            u64 off = *reinterpret_cast<const volatile u64 *>(gb + 0x40);
            if (b.flags & 1) off = plausible(off) ? *reinterpret_cast<const volatile u64 *>(off) : 0;
            b.used = off;
        }
        return true;
#ifdef _WIN32
    } __except (GetExceptionCode() == 0xC0000005 /* access violation */ ? 1 : 0) {
        return false;
    }
#endif
}
bool sample(Sample *out) {
    pc_fault_guard(1);
    const bool ok = sample_try(out);
    pc_fault_guard(0);
    return ok;
}
// "OVER": the bump offset past the size. Each frame's half is its own BufferImpl, mapped at its own base, and Map /
// Unmap (0x83e280 / 0x83e214) reset the offset to 0: the offset is relative to its half and +0x18 is the half's size
// (the capacity test is off + size <= *(gb+0x18))
const char *over(const Buf &b, u64 peak) { return peak > b.size ? " OVER" : ""; }
void run() {
    Sample s = {}, first = {};
    for (int i = 0; i < 6000 && !sample(&first); i++)  // up to a minute for the UI system
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!first.cb) return (void)printf("[options-menu] cbdiag: no eui::ConstantBuffer found\n");
    for (int k = 0; k < 2; k++)
        printf("[options-menu] cbdiag: ConstantBuffer %llx %s: size %llu (each frame's half), count %u, index %u, "
               "flags %x\n", static_cast<unsigned long long>(first.cb), k ? "font" : "ui2d",
               static_cast<unsigned long long>(first.buf[k].size), first.buf[k].count, first.buf[k].index,
               first.buf[k].flags);
    u64 peak[2] = {}, shown[2] = {};
    unsigned faults = 0;
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        if (sample(&s)) {
            for (int k = 0; k < 2; k++)
                if (s.buf[k].used > peak[k]) peak[k] = s.buf[k].used;
        } else {
            faults++;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(2)) {
            last = now;
            if (peak[0] != shown[0] || peak[1] != shown[1]) {
                printf("[options-menu] cbdiag: screen %u, ui2d peak %llu of %llu%s, font peak %llu of %llu%s (a lower "
                       "bound: 200 us samples; %u missed)\n",
                       totk_menu_screen() & 0xff, static_cast<unsigned long long>(peak[0]),
                       static_cast<unsigned long long>(s.buf[0].size), over(s.buf[0], peak[0]),
                       static_cast<unsigned long long>(peak[1]), static_cast<unsigned long long>(s.buf[1].size),
                       over(s.buf[1], peak[1]), faults);
                shown[0] = peak[0], shown[1] = peak[1];
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}
void start() {
    static std::atomic<bool> once{false};
    const char *const e = getenv("TOTK_UI_CBDIAG");
    if (!e || !*e || !strcmp(e, "0") || once.exchange(true)) return;
    std::thread(run).detach();
}
}  // namespace cbdiag

// ---------------------------------------------------------------- off-screen rows not drawn Each row is a cloned
// eui::LayoutEx whose root pane the screen creator (0x1ad7a78) appends to the list, and every shown pane allocates its
// materials' constants (Material::AllocateConstantBuffer 0x6f978, the text's 0x3260c) from eui's per-frame
// ConstantBuffer half each frame: ~37 KB per row, scrolled off or not, so 69 entries overran the ui2d half. nn::ui2d
// skips a hidden pane and its whole subtree: Pane::Calculate (0xc20fb8) returns before its children when +0x58 bit 0
// is clear (unless the context's +0x1d asks for hidden panes' matrices), and Window / Picture::Calculate allocate only
// when 0x4877c says the pane is shown. So rows well outside the screen are hidden here once a frame and shown again
// before they scroll in: the per-frame cost follows the rows near the screen, not the list's length. Never a row the
// game hid itself (its bit 0 was clear when first seen), never the first or the last row (a wrap from one end lands
// there), and only while Options is in front: a popup over it (TMS_CHOICE) or a transition only pauses the pass, any
// other screen means the rows are freed. TOTK_OPTIONS_CULL=0: off.
namespace cull {
// nn::ui2d::Pane (CalculateMtx 0x4770c): parent +0x18, translate +0x30 / +0x34 / +0x38, size +0x50 / +0x54, flags
// +0x58 (bit 0 shown), global matrix rows +0x70 / +0x80 / +0x90 (the y row: m10 m11 m12 m13 at +0x80); a layout's root
// pane at +0x18
constexpr u64 kRoot = 0x18, kParent = 0x18, kTx = 0x30, kH = 0x54, kFlag = 0x58, kMtxY = 0x80;
constexpr float kHalfScreen = 360.0f;  // the 1280 x 720 layouts' half height, origin at the centre
constexpr int kGiveUp = 600;           // frames to wait for Options in front after the rows were bound
// Rows past this table position are hidden already at bind, inside the screen's creation: the screen opens at its top
// and draws every row it has before the first pass (cull1: the opening frames alone took ui2d to 2099200, OVER, while
// the whole walk after them stayed under it). The first pass shows whatever the real scroll needs.
constexpr u32 kOpenRows = 20;

struct Row {
    s32 type;
    u64 root;
    bool hidden;  // by us
};
std::vector<Row> g_rows;
u64 g_list;   // the pane every recorded row hangs from (each root's parent at bind)
bool g_live, g_armed, g_posted;
int g_wait, g_away;
unsigned g_hidden_most;

bool on() {
    static const bool v = [] {
        const char *e = getenv("TOTK_OPTIONS_CULL");
        return !(e && !strcmp(e, "0"));
    }();
    return v;
}
bool ok(u64 p) { return p >= 0x10000 && p < (1ull << 48) && !(p & 7); }
template <class T> T rd(u64 a) { return *reinterpret_cast<const volatile T *>(a); }
template <class T> void wr(u64 a, T v) { *reinterpret_cast<volatile T *>(a) = v; }

// root is still a child of list: its parent link is list, and list's child list (an IntrusiveList at +0x20: root node
// m_Prev +0x20 / m_Next +0x28; a child's node at child +0x08, m_Next at +0x08 of the node Pane::AppendChild 0x6f5f40,
// Pane::Calculate's child loop 0xc2115c) holds it. Reads under the fault guard (a freed pane's memory may be
// anything): false on any fault: a freed row must never be written.
bool member_try(u64 list, u64 root) {
#ifdef _WIN32
    __try {
#endif
        if (rd<u64>(root + kParent) != list) return false;
        const u64 head = list + 0x20;
        u64 node = rd<u64>(head + 8);
        for (int n = 0; n < 512 && node != head; n++) {
            if (!ok(node)) return false;
            if (node - 8 == root) return true;
            node = rd<u64>(node + 8);
        }
        return false;
#ifdef _WIN32
    } __except (GetExceptionCode() == 0xC0000005 /* access violation */ ? 1 : 0) {
        return false;
    }
#endif
}
bool member(u64 list, u64 root) {
    if (!ok(list) || !ok(root)) return false;
    pc_fault_guard(1);
    const bool r = member_try(list, root);
    pc_fault_guard(0);
    return r;
}

// The pane's centre y in the screen's layout: its parent's global matrix (fresh: the list is shown) applied to its
// own translate, so a hidden row (whose own matrix is no longer computed) still follows the scroll
bool centre_y(u64 pane, float *y) {
    const u64 par = rd<u64>(pane + kParent);
    if (!ok(par)) return false;
    float m[4], t[3];
    memcpy(m, reinterpret_cast<const void *>(par + kMtxY), sizeof m);
    memcpy(t, reinterpret_cast<const void *>(pane + kTx), sizeof t);
    *y = m[0] * t[0] + m[1] * t[1] + m[2] * t[2] + m[3];
    return *y == *y;
}

void drop(const char *why) {
    // a row we hid that is still a live child of the list is shown again first (a drop while the screen lives on, a
    // popup reading as 'no focus box' for long, must not leave blank rows); a freed one fails the check, untouched
    unsigned shown = 0;
    for (const Row &r : g_rows)
        if (r.hidden && member(g_list, r.root)) {
            wr<u8>(r.root + kFlag, static_cast<u8>(rd<u8>(r.root + kFlag) | 1));
            shown++;
        }
    if (!g_rows.empty())
        printf("[options-menu] cull: %zu rows, at most %u hidden at once; %s%s\n", g_rows.size(), g_hidden_most, why,
               shown ? " (rows shown again: the screen still had them)" : "");
    g_rows.clear();
    g_list = 0;
    g_live = g_armed = false;
    g_wait = g_away = 0;
    g_hidden_most = 0;
}

void pass() {
    // every row checked against the list first: one that isn't its child any more (the screen went and the memory
    // was reused) ends the list, nothing written
    for (const Row &r : g_rows)
        if (!member(g_list, r.root)) return drop("a recorded row is no longer in the list (screen gone)");
    float top = -1e30f, bottom = 1e30f;
    size_t first = SIZE_MAX, last = SIZE_MAX;
    std::vector<float> ys(g_rows.size(), 0.0f);
    std::vector<bool> known(g_rows.size(), false);
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (!centre_y(g_rows[i].root, &ys[i])) continue;
        known[i] = true;
        if (!g_rows[i].hidden && !(rd<u8>(g_rows[i].root + kFlag) & 1)) continue;  // the game's hidden rows: no ends
        if (ys[i] > top) top = ys[i], first = i;  // y grows upwards: the list's first row is the highest
        if (ys[i] < bottom) bottom = ys[i], last = i;
    }
    unsigned hidden = 0;
    for (size_t i = 0; i < g_rows.size(); i++) {
        Row &r = g_rows[i];
        if (!known[i]) continue;
        const u8 f = rd<u8>(r.root + kFlag);
        if (!r.hidden && !(f & 1)) continue;  // hidden by the game: not ours
        const float h = rd<float>(r.root + kH);
        const float margin = 3.0f * (h > 0.0f && h < 200.0f ? h : 60.0f);  // three rows beyond the edge stay drawn
        const bool want = i == first || i == last || (ys[i] <= kHalfScreen + margin && ys[i] >= -kHalfScreen - margin);
        if (want && r.hidden) {
            wr<u8>(r.root + kFlag, static_cast<u8>(f | 1));
            r.hidden = false;
        } else if (!want && !r.hidden) {
            wr<u8>(r.root + kFlag, static_cast<u8>(f & ~1));
            r.hidden = true;
        }
        hidden += r.hidden;
    }
    if (hidden > g_hidden_most) g_hidden_most = hidden;
}

void tick(void *, bool ran) {
    g_posted = false;
    if (!g_live) return;
    if (!ran) return drop("eui stopped (a load)");
    const u32 screen = totk_menu_screen() & 0xff;
    if (screen == TMS_OPTIONS) {
        g_armed = true;
        g_away = 0;
        pass();
        if (!g_live) return;
    } else if (g_armed && (screen == TMS_PAUSE || screen == TMS_TITLE)) {
        return drop("Options closed");  // back where Options opens from: its rows are freed, never touched again
    } else if (g_armed && screen == TMS_NONE && ++g_away > 30) {
        return drop("Options closed to the world");  // (TMS_NONE is only 'no focus box': half a second of it, not one)
    } else if (!g_armed && ++g_wait > kGiveUp) {
        return drop("Options never came to the front");
    }
    // else a popup over Options (yes / no, a dialogue) or a transition: the rows live on, nothing written
    if (!g_posted && totk_ui_post(tick, nullptr)) g_posted = true;
}

// A row bound (OptionToggle / OptionSelect::bind, inside the screen's creation). A new screen is told by its list
// pane (each root's parent): another list pane, or a recorded root address under another type, drops the old list
// (its rows were freed with the old screen). The same row bound again (same root, same type: the screen refreshing its
// values) keeps its state, unless its pane shows although we hid it: then it is a fresh pane at a reused address, and
// it is treated as new.
void add(s32 type, u32 index, const void *layout) {
    if (!on() || !layout) return;
    const u64 root = rd<u64>(reinterpret_cast<u64>(layout) + kRoot);
    if (!ok(root)) return;
    const u64 list = rd<u64>(root + kParent);
    if (!ok(list)) return;
    if (!g_rows.empty() && list != g_list) drop("a new screen (another list pane)");
    Row *same = nullptr;
    for (Row &r : g_rows) {
        if (r.root == root && r.type != type) {
            drop("a new screen (a recorded address, another row)");
            break;
        }
        if (r.root == root) {
            same = &r;
            break;
        }
        if (r.type == type) {
            drop("a new screen (a recorded row, another address)");
            break;
        }
    }
    const u8 f = rd<u8>(root + kFlag);
    if (same && !(same->hidden && (f & 1))) return;  // a refresh
    bool hide = false;
    if (index >= kOpenRows && (f & 1)) {  // below the opening view; a row the game hid (bit 0 clear) stays the game's
        wr<u8>(root + kFlag, static_cast<u8>(f & ~1));
        hide = true;
    }
    if (same) {
        same->hidden = hide;
    } else {
        g_list = list;
        g_rows.push_back({type, root, hide});
    }
    g_live = true;
    if (!g_posted && totk_ui_post(tick, nullptr)) g_posted = true;
}
}  // namespace cull

// ---------------------------------------------------------------- the layout's unused row parts SystemOption_00.bflyt
// has one part of each row kind: Pa_ToggleBtn_00, Pa_SelectBtn_00, Pa_AmiiboBtn_00 and Pa_Separator_00. The screen's
// creation takes it for the kind's first entry and clones it for the others. The game's one list has every kind, so
// each is used; a section (zones) may have none of a kind, and its part then stays where the layout put it, never
// bound; no section has a separator, and Display, Graphics, Audio and Keys no toggle or amiibo either). Hidden at the
// rows' bind: the part of each kind the shown section has no entry of (shown or conditional: the creation takes it for
// either). A section change opens a new screen, whose parts start shown.
namespace orphans {
constexpr u64 kPaneName = 0xb0;  // nn::ui2d::Pane's name (totk_menu_cursor.cpp's kPaneName)
const char *const kPart[4] = {"Pa_ToggleBtn_00", "Pa_SelectBtn_00", "Pa_AmiiboBtn_00", "Pa_Separator_00"};  // by buttonType

// list's children (cull::member_try's walk) named for a kind not in has: hidden. False on any fault or bad link
bool hide_try(u64 list, const bool *has, s32 zone) {
#ifdef _WIN32
    __try {
#endif
        const u64 head = list + 0x20;
        u64 node = cull::rd<u64>(head + 8);
        for (int n = 0; n < 512 && node != head; n++) {
            if (!cull::ok(node)) return false;
            const u64 pane = node - 8;
            const char *const name = reinterpret_cast<const char *>(pane + kPaneName);
            for (int k = 0; k < 4; k++) {
                if (has[k] || strncmp(name, kPart[k], 24) != 0) continue;
                const u8 f = cull::rd<u8>(pane + cull::kFlag);
                if (!(f & 1)) continue;
                cull::wr<u8>(pane + cull::kFlag, static_cast<u8>(f & ~1));
                printf("[options-menu] section %d: %s unused (no entry of its kind), hidden\n", static_cast<int>(zone),
                       kPart[k]);
            }
            node = cull::rd<u64>(node + 8);
        }
        return true;
#ifdef _WIN32
    } __except (GetExceptionCode() == 0xC0000005 /* access violation */ ? 1 : 0) {
        return false;
    }
#endif
}

// A row of the open screen was bound (layout: its part's LayoutEx; its root pane's parent is the rows' list)
void hide(const void *layout) {
    if (!zones::on() || !zones::g_ok || !layout) return;
    const s32 z = zones::g_zone.load();
    const OptionsMgr &v = zones::g_view[z >= 0 && z < Z_COUNT ? z : Z_GENERAL];
    if (!v.entries) return;
    bool has[4] = {};
    for (u32 i = 0; i < v.count; i++)  // (buttonType is signed: a stray negative one must not index has)
        if (const u32 k = static_cast<u32>(v.entries[i].buttonType); k < 4) has[k] = true;
    if (has[0] && has[1] && has[2] && has[3]) return;
    const u64 root = cull::rd<u64>(reinterpret_cast<u64>(layout) + cull::kRoot);
    if (!cull::ok(root)) return;
    const u64 list = cull::rd<u64>(root + cull::kParent);
    if (!cull::ok(list)) return;
    pc_fault_guard(1);
    hide_try(list, has, z);
    pc_fault_guard(0);
}
}  // namespace orphans

void append_rows(OptionsMgr *mgr, u64 heap) {
    cbdiag::start();
    struct Add {
        s32 type, buttonType;  // a separator: type -1, buttonType 3 (the allocator's default entry)
        u32 choices;
        const char *what;
    };
    std::vector<Add> add;
    // the section row first (a cap below never drops it); its choices are the sections (ZONES in options_menu.py)
    if (zones::on()) add.push_back({kZoneType, 1, Z_COUNT, "sections"});
    const bool keys = key_block_shown();
    // Settings that act only through game patches' RomFS sets (totk_engine.c: the canvas's heap set, the fast-travel
    // set): without TOTK_PATCH_SETS (the package sets none) choosing them would change nothing, so no row
    static const char *const kNeedSets[] = {"graphics.aspect_ratio", "game.fast_travel"};
    const char *const sets = getenv("TOTK_PATCH_SETS");
    const bool have_sets = sets && *sets;
    for (const PcRow &r : kRows) {
        if (r.type == kKeyBlockType && !keys) continue;
        bool needs_sets = false;
        for (const char *k : kNeedSets) needs_sets = needs_sets || !strcmp(r.key, k);
        if (needs_sets && !have_sets) {
            printf("[options-menu] %s: needs TOTK_PATCH_SETS (game patches' RomFS sets), not set; not added\n", r.key);
            continue;
        }
        const pc_setting *d;
        if (!row_of(r.type, &d)) {
            printf("[options-menu] %s: no such setting, or it does not take the row's values; not added\n", r.key);
            continue;
        }
        if (r.type == kKeyBlockType) add.push_back({-1, 3, 0, "separator"});
        add.push_back({r.type, 1, count_of(values_of(r, d)), r.key});
        if (dynamic(r)) printf("[options-menu] %s: %u choices from the hardware\n", r.key, count_of(values_of(r, d)));
    }
    // (with sections a screen holds one section: the largest, Keys, is ~38 rows, under even the drawn cap; the list's
    // own length costs nothing per frame)
    const u32 safe = zones::on() ? kSafeEntriesZoned : cull::on() ? kSafeEntriesCulled : kSafeEntriesDrawn;
    if (keys) {
        if (add.empty() || add.back().type != kKeyBlockType) add.push_back({-1, 3, 0, "separator"});
        const KeyLines &k = key_lines();
        // The screen's entries stay at or under safe unless a boot sets TOTK_OPTIONS_KEYS_MAX (key_lines). All the
        // key rows or none: when they don't all fit, one row (kKeyLinkType) opens F10's Keys tab, which has every one
        const size_t base = mgr->count + add.size();
        const size_t room = getenv("TOTK_OPTIONS_KEYS_MAX") ? k.n : base < safe ? safe - base : 0;
        const size_t shown = room >= k.n + 1 ? k.n : 0;  // (+1: the reset row ends the block)
        if (!shown) {
            printf("[options-menu] the %zu key rows don't fit under the cap %u (room %zu): the F10 Keys link row "
                   "instead\n", k.n, safe, room);
            add.push_back({kKeyLinkType, 1, 1, "keys link (F10 > Keys)"});
            g_key_link = true;
        }
        for (size_t i = 0; i < shown; i++) {
            add.push_back({k.at[i].type, 1, 1, k.at[i].entry->action});  // one choice: the text is ours
            if (!k.at[i].labelled)
                printf("[options-menu] %s: no type in kKeyRows (no overlay texts): row %d, its page label in English\n",
                       k.at[i].entry->action, k.at[i].type);
        }
        if (shown) {
            add.push_back({kKeyResetType, 1, 1, "reset keys"});
            g_key_reset = true;
        }
        for (const KeyRow &r : kKeyRows)
            if (key_index(r.type) < 0)
                printf("[options-menu] %s: not an on-foot pc_bind_pages.inc entry; no row\n", r.action);
    } else {
        printf("[options-menu] key rows not added: the rebinding API is not ready (TOTK_OPTIONS_KEYS=1 shows them)\n");
    }
    const u32 n = mgr->count;
    // the cap holds for the whole screen: settings rows past it are dropped too (the key rows already stopped at it)
    if (n + add.size() > safe && !getenv("TOTK_OPTIONS_KEYS_MAX")) {
        const size_t keep_rows = n < safe ? safe - n : 0;
        printf("[options-menu] WARNING: %zu PC rows would take the screen to %zu entries, past the cap %u: the last"
               " %zu dropped (%s ..)\n", add.size(), n + add.size(), safe, add.size() - keep_rows,
               add[keep_rows].what);
        add.resize(keep_rows);
    }
    OptionEntry *const old = mgr->entries;
    if (add.empty() || n == 0 || !old) return;
    for (u32 i = 0; i < n; i++)
        if (old[i].type >= 50) return;  // already ours (a second initialize on the same array)
    const std::vector<OptionEntry> keep(old, old + n);
    guest::EntriesAlloc(mgr, n + static_cast<u32>(add.size()), heap);
    if (mgr->entries == old) {
        printf("[options-menu] no memory for %zu more entries\n", add.size());
        return;
    }
    // the old array stays allocated (the game frees neither; initialize runs once per OptionsMgr)
    memcpy(mgr->entries, keep.data(), n * sizeof(OptionEntry));
    for (size_t i = 0; i < add.size(); i++) {
        OptionEntry &e = mgr->entries[n + i];
        e.index = n + static_cast<u32>(i);
        e.nameHash = 0;      // no OptionData member: the value is pc_settings' (or the binding table's)
        e.visibleHash = 0;   // always shown
        if (add[i].buttonType == 3) {  // left as the allocator made it: {-1, 3, no choices}
            printf("[options-menu] separator added\n");
            continue;
        }
        e.type = add[i].type;
        e.buttonType = 1;  // selection
        const u32 choices = add[i].choices;
        guest::ChoicesAlloc(&e.choiceCount, choices, heap);
        if (e.choiceCount != choices) e.buttonType = 3;  // no memory for its choices: an empty separator
        printf("[options-menu] row %d (%s) added after the game's %u\n", e.type, add[i].what, n);
    }
}

// The selection part each of our rows was last bound to (one Options screen is open at a time and binds all its rows
// when it opens, so these are that screen's; game UI thread only). A closed screen's parts are freed and nothing tells
// us, so a row bound a second time starts a new set (a screen binds each row once, at its creation; the new one may
// reuse the old parts' addresses): the old screen's are forgotten, and a row the new screen has not bound is not
// refreshed instead of being written through a freed part. refresh_others' own binds do not come here.
OptionSelect *g_bound[sizeof kRows / sizeof kRows[0]];
OptionSelect *g_key_bound[kMaxKeys];  // the same for the key rows (index into key_lines())
std::atomic<int> g_capture{-1};       // the key row waiting for a key, -1 none

void note_bound(OptionSelect *self) {
    OptionSelect **slot = nullptr;
    for (size_t i = 0; i < sizeof kRows / sizeof kRows[0]; i++)
        if (kRows[i].type == self->type) slot = &g_bound[i];
    if (const int i = key_index(self->type); i >= 0) slot = &g_key_bound[i];
    if (!slot) return;
    if (*slot) {
        memset(g_bound, 0, sizeof g_bound);
        memset(g_key_bound, 0, sizeof g_key_bound);
        printf("[options-menu] row %d bound again: a new screen, the old bindings forgotten\n", self->type);
        // a capture started on the old screen has no part to show its key in: its tick sees the empty slot, cancels
    }
    *slot = self;
}

namespace ctl {
void select_bind(OptionSelect *s, const OptionEntry *e);
}

// After one of our rows changed its setting: the others whose value that changed (a preset sets render_scale, a
// render_scale makes the preset "custom") show their new choice. Inside a hook (guest calls).
void show_dynamic(OptionSelect *sel);

void refresh_others(s32 changed) {
    OptionsMgr *const mgr = options_mgr();
    if (!mgr) return;
    for (size_t i = 0; i < sizeof kRows / sizeof kRows[0]; i++) {
        OptionSelect *const sel = g_bound[i];
        const pc_setting *d;
        if (!sel || kRows[i].type == changed || sel->type != kRows[i].type || !row_of(sel->type, &d)) continue;
        const s32 c = index_of(kRows[i], d);
        if (c < 0 || c == sel->choice) continue;
        if (const OptionEntry *e = guest::FindEntry(mgr, sel->type)) ctl::select_bind(sel, e);
        show_dynamic(sel);
    }
}

// ---------------------------------------------------------------- the key rows' texts and capture
// All on eui's thread: the row hooks, and the ticks totk_ui_post runs there (inside the bridge).

// eui::MessageString as FindMessage leaves it (f_b906ec, f_121ea58): the UTF-16 text, its length in units, -1.
// SetMessageString hands text and length to the pane (f_997ea0), so a text of ours shows without a message id.
struct MsgText {
    const char16_t *text;
    u32 length;
    u32 _c;
    s64 tail;
};
static_assert(sizeof(MsgText) == 0x18, "eui::MessageString is 0x18 bytes");

// The choice pane: the guest string SelectShowChoice (main+0x1a85d84) passes to SetMessageString
const char *choice_pane() { return guest_mem::guest_data<const char>(0x10036f84ce); }

// The label pane: showLabel's T_Option_00
const char *label_pane() { return guest_mem::guest_data<const char>(0x1003700670); }

char16_t g_key_text[kMaxKeys][64];    // each key row's shown text (kept: the pane may read it later)
char16_t g_key_label[kMaxKeys][64];   // the page label of a key row without overlay texts

void show_text(OptionSelect *sel, const char16_t *text, u32 length, const char *pane = nullptr) {
    const MsgText m = {text, length, 0, -1};
    guest::SetMessageString(sel->layout, pane ? pane : choice_pane(),
                                      reinterpret_cast<const MessageString *>(&m), true, nullptr);
}

// A status text ("1069_%02d" of SystemOption_00) into the row's choice pane, as showLabel finds a label
void show_status(OptionSelect *sel, KeyStatus st) {
    MessageString msg = {};
    guest::MessageStringCtor(&msg);
    u64 file = reinterpret_cast<u64>(guest_mem::guest_data<const char>(0x1003719b8b));  // "LayoutMsg/SystemOption_00"
    FixedSafeString32 label = {};
    guest::FormatFixedSafeString32(&label.top, "1069_%02d", static_cast<s32>(st));
    *reinterpret_cast<char *>(reinterpret_cast<uintptr>(label.top) + static_cast<s64>(label.bufferSize) - 1) = 0;
    guest::FindMessage(&msg, reinterpret_cast<const char *const *>(&file), &label, true);
    guest::SetMessageString(sel->layout, choice_pane(), &msg, true, nullptr);
}

// The field of view row (the graphics.fov): its choice text and the camera's width in degrees at that choice, "1.10x
// (85°)", over what SelectShowChoice wrote (after bind, step, decide). The degrees are the first number of fov_format's
// "(85° wide, 50° tall)" (totk_engine.c: the camera's own fovy, the data page's aspect), language-neutral; fov_format has
// none unpatched or before the first camera frame, and then the choice text stays alone.
constexpr s32 kFovType = 67;
char16_t g_fov_text[80];
static_assert(sizeof(MessageString) == sizeof(MsgText), "read as eui::MessageString");

// A kDynamic row's choice text: the setting's own name for every choice past 0 (choice 0 has overlay texts, which
// SelectShowChoice already wrote), over what SelectShowChoice wrote (after bind, step, decide, a refresh)
char16_t g_dynamic_text[4][96];
u32 put_utf8(char16_t *out, u32 at, u32 cap, const char *s);
void show_dynamic(OptionSelect *sel) {
    if (!sel || sel->choice <= 0) return;
    const pc_setting *d;
    const PcRow *const r = row_of(sel->type, &d);
    if (!r || !dynamic(*r) || !d->names || static_cast<u32>(sel->choice) >= count_of(values_of(*r, d))) return;
    u32 slot = 0;
    for (const PcRow &k : kRows) {
        if (&k == r) break;
        slot += dynamic(k);
    }
    if (slot >= sizeof g_dynamic_text / sizeof g_dynamic_text[0]) return;
    char16_t *const t = g_dynamic_text[slot];
    show_text(sel, t, put_utf8(t, 0, 96, d->names[sel->choice]));
}

void show_fov(OptionSelect *sel) {
    const pc_setting *d;
    const PcRow *r = row_of(sel->type, &d);
    if (!r || !d->format || sel->choice < 0 || static_cast<u32>(sel->choice) >= count_of(values_of(*r, d))) return;
    const char *const f = d->format(static_cast<float>(atof(values_of(*r, d)[sel->choice])));
    const char *const deg = f ? strstr(f, "\xc2\xb0") : nullptr;
    const char *num = deg;
    while (num && num > f && num[-1] >= '0' && num[-1] <= '9') num--;
    if (!deg || num == deg || deg - num > 3) return;
    MessageString msg = {};
    guest::MessageStringCtor(&msg);
    u64 file = reinterpret_cast<u64>(guest_mem::guest_data<const char>(0x1003719b8b));  // "LayoutMsg/SystemOption_00"
    char format[16];
    snprintf(format, sizeof format, "10%02d_%%02d", static_cast<int>(sel->type));
    FixedSafeString32 label = {};
    guest::FormatFixedSafeString32(&label.top, format, sel->choice);
    *reinterpret_cast<char *>(reinterpret_cast<uintptr>(label.top) + static_cast<s64>(label.bufferSize) - 1) = 0;
    guest::FindMessage(&msg, reinterpret_cast<const char *const *>(&file), &label, true);
    const MsgText *const m = reinterpret_cast<const MsgText *>(&msg);
    constexpr u32 cap = sizeof g_fov_text / sizeof g_fov_text[0];
    u32 n = 0;
    while (m->text && n < m->length && n + 8 < cap && m->text[n]) g_fov_text[n] = m->text[n], n++;
    g_fov_text[n++] = u' ';
    g_fov_text[n++] = u'(';
    for (const char *p = num; p < deg; p++) g_fov_text[n++] = static_cast<char16_t>(*p);
    g_fov_text[n++] = u'°';
    g_fov_text[n++] = u')';
    g_fov_text[n] = 0;
    show_text(sel, g_fov_text, n);
}

// UTF-8 (pc_key_name's) appended as UTF-16 at out[at], NUL-terminated within cap; the new length
u32 put_utf8(char16_t *out, u32 at, u32 cap, const char *s) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p && at + 2 < cap) {
        u32 cp, n;
        if (*p < 0x80) cp = *p, n = 1;
        else if ((*p >> 5) == 6) cp = *p & 0x1f, n = 2;
        else if ((*p >> 4) == 14) cp = *p & 0x0f, n = 3;
        else if ((*p >> 3) == 30) cp = *p & 0x07, n = 4;
        else { p++; continue; }  // a stray continuation byte
        for (u32 k = 1; k < n; k++) {
            if ((p[k] & 0xc0) != 0x80) {  // cut short (a NUL included): one replacement character
                cp = 0xfffd, n = k;
                break;
            }
            cp = cp << 6 | (p[k] & 0x3f);
        }
        p += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[at++] = static_cast<char16_t>(0xd800 + (cp >> 10));
            out[at++] = static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        } else {
            out[at++] = static_cast<char16_t>(cp);
        }
    }
    out[at] = 0;
    return at;
}

// The language pc_key_name names keys in: the game's (pc_lang.cpp, nn::oe::GetDesiredLanguage's answer)
const char *key_lang() { return pc_lang_key(); }

KeyStatus status_of(int err) {
    switch (err) {
    case PCB_LAST_KEY: return KS_LAST_KEY;
    case PCB_RESERVED: return KS_RESERVED;
    case PCB_FIXED: return KS_FIXED;
    case PCB_CHAIN: return KS_CHAIN;
    case PCB_KIND: return KS_KIND;
    case PCB_UNKNOWN: return KS_UNKNOWN;
    default: return KS_NOT_READY;
    }
}

// Key row i's keys (the first two: "Space / Mouse 5") into its part, if the open screen has bound it
void show_key(size_t i) {
    OptionSelect *const sel = g_key_bound[i];
    if (!sel) return;
    guest::ArrowSetDisabled(sel->arrowNext, true);  // one choice: < and > change nothing
    guest::ArrowSetDisabled(sel->arrowPrev, true);
    const KeyLine &line = key_lines().at[i];
    if (!line.labelled)  // over showLabel's (no "10%02d" for its type)
        show_text(sel, g_key_label[i], put_utf8(g_key_label[i], 0, 64, line.entry->label), label_pane());
    int in[4];
    unsigned flags = 0;
    const int n = pc_bind_get(scope_of(i), action_of(i), in, 4, &flags);
    char16_t *const t = g_key_text[i];
    if (n == PCB_NOT_READY && key_demo()) return show_text(sel, t, put_utf8(t, 0, 64, "Space / \xc3\x91"));
    if (n <= 0) return show_status(sel, n == 0 ? KS_UNBOUND : status_of(n));
    u32 len = 0;
    for (int k = 0; k < n && k < 2; k++) {
        if (k) len = put_utf8(t, len, 64, " / ");
        const char *const name = pc_key_name(in[k], key_lang());
        len = put_utf8(t, len, 64, name && *name ? name : "?");
    }
    show_text(sel, t, len);
}

using Clock = std::chrono::steady_clock;
std::atomic<bool> g_ticking{false};
// A key row showing a status or a swap for a while (an error, "not available yet", "Swapped: Q") until then, then
// its keys again; the epoch: none
Clock::time_point g_until[kMaxKeys];
bool g_any_until;
// The reset row (kKeyResetType) of the open screen, and its state: idle, asked again (KS_CONFIRM_RESET), or showing
// KS_RESET_DONE, until g_reset_until
OptionSelect *g_reset_sel;
enum { RS_IDLE, RS_ARMED, RS_DONE } g_reset_state = RS_IDLE;
Clock::time_point g_reset_until;
constexpr auto kResetWindow = std::chrono::seconds(3);
char16_t g_reset_text[64];

// A SystemOption_00 message ("1069_%02d", "10%02d_00": the format and its one number) as UTF-16 into out, NUL-ended,
// leaving room for a suffix; its length
u32 msg_text(const char *format, s32 v, char16_t *out, u32 cap) {
    MessageString msg = {};
    guest::MessageStringCtor(&msg);
    u64 file = reinterpret_cast<u64>(guest_mem::guest_data<const char>(0x1003719b8b));  // "LayoutMsg/SystemOption_00"
    FixedSafeString32 label = {};
    guest::FormatFixedSafeString32(&label.top, format, v);
    *reinterpret_cast<char *>(reinterpret_cast<uintptr>(label.top) + static_cast<s64>(label.bufferSize) - 1) = 0;
    guest::FindMessage(&msg, reinterpret_cast<const char *const *>(&file), &label, true);
    const MsgText *const m = reinterpret_cast<const MsgText *>(&msg);
    u32 n = 0;
    while (m->text && n < m->length && n + 24 < cap && m->text[n]) out[n] = m->text[n], n++;
    out[n] = 0;
    return n;
}

// The key row of (scope, action), -1: not a row of this screen's block
int key_row_of(int scope, const char *action) {
    if (!action) return -1;
    for (size_t j = 0; j < key_lines().n; j++)
        if (scope_of(j) == scope && !strcmp(action_of(j), action)) return static_cast<int>(j);
    return -1;
}

void tick(void *, bool ran);
bool post_tick() {
    if (g_ticking.exchange(true)) return true;
    if (totk_ui_post(tick, nullptr)) return true;
    g_ticking = false;
    return false;
}

// A status on row i for 2 s, then its keys again (without a tick, until the screen binds the row again)
void note(size_t i, KeyStatus st) {
    if (OptionSelect *const sel = g_key_bound[i]) show_status(sel, st);
    g_until[i] = Clock::now() + std::chrono::seconds(2);
    g_any_until = true;
    post_tick();
}

// A swap gave row j another key: "Swapped: Q" on that row for 2 s, so the conflict shows on the other action's own
// label
void note_swap(size_t j, int input) {
    OptionSelect *const sel = g_key_bound[j];
    if (!sel) return;
    char16_t *const t = g_key_text[j];
    u32 len = msg_text("1069_%02d", KS_SWAPPED, t, 64);
    const char *const name = pc_key_name(input, key_lang());
    len = put_utf8(t, len, 64, " ");
    len = put_utf8(t, len, 64, name && *name ? name : "?");
    show_text(sel, t, len);
    g_until[j] = Clock::now() + std::chrono::seconds(2);
    g_any_until = true;
    post_tick();
}

void bind_input(size_t i, int input) {
    PcBindChange ch[8];
    const int n = pc_bind_set(scope_of(i), action_of(i), 0, input, ch, 8);
    printf("[options-menu] %s <- input %d: %d\n", action_of(i), input, n);
    for (int k = 1; k < n && k < 8; k++)
        printf("[options-menu]   swapped: %s (scope %d, slot %d) %d -> %d\n", ch[k].action, ch[k].scope, ch[k].slot,
               ch[k].old_input, ch[k].new_input);
    if (n < 0) return note(i, status_of(n));
    for (size_t k = 0; k < key_lines().n; k++) show_key(k);  // the row, and the rows a swap changed
    for (int k = 1; k < n && k < 8; k++) {  // then the swapped rows say so
        const int j = key_row_of(ch[k].scope, ch[k].action);
        if (j >= 0 && static_cast<size_t>(j) != i && ch[k].new_input > 0) note_swap(static_cast<size_t>(j), ch[k].new_input);
    }
}

// The reset row's own text again ("Reset" / "Restablecer": its one choice)
void show_reset_idle() {
    if (!g_reset_sel) return;
    show_text(g_reset_sel, g_reset_text, msg_text("10%02d_00", kKeyResetType, g_reset_text, 64));
}

// Every key back to the PC defaults: each scope of every page entry once (pc_bind_reset's NULL action: the whole
// scope), as F10's 'Restore defaults' does, so a rebind in a scope with no Options row (the menus' strip sort, the
// dialogue log) goes too: the row's text says all keys
void reset_all_keys() {
    static PcBindChange ch[256];  // a scope's overrides and the swaps they undo
    std::vector<int> done;
    int count = 0;
    const PcPageEntry *const pages = pc_bind_page_entries(&count);
    for (int j = 0; pages && j < count; j++) {
        const int s = pages[j].scope;
        bool seen = false;
        for (int d : done) seen |= d == s;
        if (seen) continue;
        done.push_back(s);
        const int r = pc_bind_reset(s, nullptr, ch, 256);
        printf("[options-menu] reset keys, scope %d: %d\n", s, r);
    }
    for (size_t j = 0; j < key_lines().n; j++) {
        g_until[j] = Clock::time_point{};
        show_key(j);
    }
}

void drop_capture(const char *why) {
    if (g_capture.exchange(-1) < 0) return;
    pc_input_capture_cancel();
    printf("[options-menu] capture dropped: %s\n", why);
}

// Once a frame (totk_ui_post, posted again) while a key row waits for a key or shows a status
void tick(void *, bool ran) {
    g_ticking = false;
    auto forget = [] {  // no row of the old screen is touched again
        for (Clock::time_point &u : g_until) u = Clock::time_point{};
        g_any_until = false;
        g_reset_state = RS_IDLE;
    };
    if (!ran) {  // eui stopped (a load): no guest call now
        drop_capture("eui stopped");
        forget();
        return;
    }
    const u32 screen = totk_menu_screen() & 0xff;
    if (screen != TMS_OPTIONS && screen != TMS_TRANSITION) {  // the screen closed (a pad's B): its parts are freed
        if (g_capture.load() >= 0) printf("[options-menu] screen %u in front\n", screen);
        drop_capture("Options closed");
        forget();
        return;
    }
    const int row = g_capture.load();
    if (row >= 0 && !g_key_bound[row]) {
        drop_capture("its row is not on this screen");
    } else if (row >= 0) {
        const int r = pc_input_capture_poll();
        if (r != 0 && g_capture.exchange(-1) >= 0) {
            if (r > 0) {
                bind_input(static_cast<size_t>(row), r);
            } else {
                printf("[options-menu] %s: capture cancelled\n", action_of(static_cast<size_t>(row)));
                show_key(static_cast<size_t>(row));
            }
        }
    }
    const Clock::time_point now = Clock::now();
    if (g_any_until) {
        g_any_until = false;
        for (size_t j = 0; j < kMaxKeys; j++) {
            if (g_until[j] == Clock::time_point{}) continue;
            if (now >= g_until[j]) {
                g_until[j] = Clock::time_point{};
                if (j < key_lines().n) show_key(j);
            } else {
                g_any_until = true;
            }
        }
    }
    if (g_reset_state != RS_IDLE && now >= g_reset_until) {  // the second confirm didn't come, or 'done' was read
        g_reset_state = RS_IDLE;
        show_reset_idle();
    }
    if (g_capture.load() >= 0 || g_any_until || g_reset_state != RS_IDLE) post_tick();
}

// A on a key row: wait for the next key
void key_decide(OptionSelect *self, void *button) {
    if (guest::FindButton(self->parts->buttonGroup, self->layout) != button) return;
    const int row = key_index(self->type);
    if (row < 0 || g_capture.load() >= 0) return;  // one capture at a time
    const size_t i = static_cast<size_t>(row);
    if (!pc_input_capture_begin(0)) {
        printf("[options-menu] %s: capture not available\n", action_of(i));
        return note(i, KS_NOT_READY);
    }
    g_capture = row;
    show_status(self, KS_PRESS);
    printf("[options-menu] %s: waiting for a key\n", action_of(i));
    if (!post_tick()) {  // nothing would ever read the key (no eui hook on this key)
        drop_capture("no UI tick");
        show_status(self, KS_NOT_READY);
    }
}

// A confirm on the reset row: the first asks again (KS_CONFIRM_RESET for kResetWindow), a second within it resets
// every key (then KS_RESET_DONE for 2 s)
void reset_decide(OptionSelect *self, void *button) {
    if (guest::FindButton(self->parts->buttonGroup, self->layout) != button) return;
    if (g_capture.load() >= 0) return;  // a key row is waiting for its key
    g_reset_sel = self;
    const Clock::time_point now = Clock::now();
    if (g_reset_state == RS_ARMED && now < g_reset_until) {
        printf("[options-menu] reset keys: confirmed\n");
        reset_all_keys();
        show_status(self, KS_RESET_DONE);
        g_reset_state = RS_DONE;
        g_reset_until = now + std::chrono::seconds(2);
    } else {
        printf("[options-menu] reset keys: asked again\n");
        show_status(self, KS_CONFIRM_RESET);
        g_reset_state = RS_ARMED;
        g_reset_until = now + kResetWindow;
    }
    if (!post_tick()) {  // no tick to end the window: back to idle at once, so a later confirm asks again
        g_reset_state = RS_IDLE;
        show_reset_idle();
    }
}

// ---------------------------------------------------------------- the open screen's selection rows (< and > clicks)
// Every OptionSelect the open screen bound, the game's rows and ours, for totk_options_row_arrows (a click on a row's
// < or >, totk_menu_cursor.cpp). A new screen is told as cull tells it, by the list pane its rows hang from; a row is
// read only while its root pane is still that list's child (cull::member, under the fault guard). eui's thread only.
namespace sels {
struct Sel {
    OptionSelect *self;
    u64 layout, root;
};
std::vector<Sel> g_rows;
u64 g_list;
void add(OptionSelect *self) {
    const u64 layout = reinterpret_cast<u64>(self->layout);
    if (!cull::ok(layout)) return;
    const u64 root = cull::rd<u64>(layout + cull::kRoot);
    if (!cull::ok(root)) return;
    const u64 list = cull::rd<u64>(root + cull::kParent);
    if (!cull::ok(list)) return;
    if (list != g_list) g_rows.clear(), g_list = list;
    for (Sel &s : g_rows)
        if (s.root == root) return (void)(s = {self, layout, root});
    g_rows.push_back({self, layout, root});
}
}  // namespace sels

// ---------------------------------------------------------------- a section change: Options closed and opened again
// The screen builds its rows once, as it opens, so another section is another screen. The pad's own B closes Options
// (back to the System tab or the title menu, their focus still on Options), the section changes while no Options
// screen exists (zones::g_zone: every count / index call of one screen sees one section), and the pad's own A on that
// focus opens Options again on the new section, its section row focused. Each press 100 ms (the menu cursor's A
// pulse); each wait gives up after 1.5 s and leaves the menus where they are (the section changes only once Options
// has closed; the next opening shows it). A only on the title menu or the pause menu's System tab (tab 8): never an
// item tab, where A would use an item. On eui's thread (totk_ui_post), from the section row's step / decide.
namespace reopen {
enum State { IDLE, PRESS_B, WAIT_CLOSED, SETTLE, PRESS_A, WAIT_OPEN };
std::atomic<State> g_state{IDLE};  // (written on eui's thread; the cursor also reads it from the pad-read fallback)
s32 g_want;
Clock::time_point g_at;
// The opener's item that opened Options (its screen's active node at the request, Options in front: the Pouch's or the
// title's), and the focus seen when the opener came back. A is pressed only on that item: the cursor's hover and
// clicks are off meanwhile (totk_options_reopening), and a focus anywhere else stops the reopen (the section stays
// pending: hover could have moved the focus to Save / Quit.
u64 g_item, g_back;
constexpr u32 kPadA = 1u << 0, kPadB = 1u << 1;  // HidNpadButton
constexpr long long kPressMs = 100, kSettleMs = 150, kWaitMs = 1500;
void tick(void *, bool ran);
void stop(const char *why) {
    totk_menu_pad_hold(0);
    if (g_state != IDLE) printf("[options-menu] section %d: %s\n", static_cast<int>(g_want), why);
    g_state = IDLE;
}
void go(State s) {
    g_state = s;
    g_at = Clock::now();
}
void post() {
    if (!totk_ui_post(tick, nullptr)) stop("no UI tick (a load); the section is unchanged");
}
bool opener(u32 v) {
    const u32 s = v & 0xff;
    return s == TMS_TITLE || (s == TMS_PAUSE && !TOTK_MENU_SUBSCREEN(v) && TOTK_MENU_SUB(v) == 8);
}
void tick(void *, bool ran) {
    if (!ran) return stop("eui stopped (a load); the section is unchanged unless Options had closed");
    const u32 v = totk_menu_screen();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_at).count();
    switch (g_state.load()) {
    case IDLE:
        return;
    case PRESS_B:
        if (ms < kPressMs) {
            totk_menu_pad_hold(kPadB);
        } else {
            totk_menu_pad_hold(0);
            go(WAIT_CLOSED);
        }
        break;
    case WAIT_CLOSED:
        if (opener(v) && totk_menu_focus_node()) {
            zones::g_pending = g_want;  // (the next screen's creation takes it: zones::view)
            g_back = totk_menu_focus_node();
            if (g_item && g_back != g_item) return stop("the opener's focus is not on Options: not opened again");
            printf("[options-menu] section %d: Options closed, opening it again\n", static_cast<int>(g_want));
            go(SETTLE);
        } else if (ms > kWaitMs) {
            return stop("Options did not close; the section is unchanged");
        }
        break;
    case SETTLE:  // (the opener's focus box settles back on Options)
        if (!opener(v)) return stop("the menu Options opens from left the front: not opened again");
        if (totk_menu_focus_node() != g_back) return stop("the opener's focus moved off Options: not opened again");
        if (ms >= kSettleMs) go(PRESS_A);
        break;
    case PRESS_A:  // (SETTLE's last tick checked the focus; once A is down the focus leaves for Options, as it should)
        if (ms < kPressMs) {
            totk_menu_pad_hold(kPadA);
        } else {
            totk_menu_pad_hold(0);
            go(WAIT_OPEN);
        }
        break;
    case WAIT_OPEN:
        if ((v & 0xff) == TMS_OPTIONS) {
            printf("[options-menu] section %d: shown\n", static_cast<int>(g_want));
            g_state = IDLE;
            return;
        }
        if (ms > kWaitMs) return stop("Options did not open again (A on Options shows the new section)");
        break;
    }
    post();
}
// The section row asked for section z (its step or decide, on the open Options screen)
void request(s32 z) {
    if (z < 0 || z >= Z_COUNT || z == zones::g_zone.load() || g_state != IDLE) return;  // (one change at a time)
    g_want = z;
    g_item = totk_menu_layout_focus("Pouch_00");
    if (!g_item) g_item = totk_menu_layout_focus("Title_00");
    printf("[options-menu] section %d asked: closing Options (the opener's item %s)\n", static_cast<int>(z),
           g_item ? "known" : "unknown: the focus the opener comes back with");
    go(PRESS_B);
    post();
}
}  // namespace reopen

}  // namespace

// ---------------------------------------------------------------- the value of our rows (options_menu.h)
namespace game::ui::pc_rows {

bool owns(s32 type) {
    return type >= 50 && (row_of(type, nullptr) != nullptr || key_row(type) || (type == kKeyLinkType && g_key_link) ||
                          (type == kKeyResetType && g_key_reset) || (type == kZoneType && zones::on()));
}

s32 choice(s32 type) {
    if (type == kZoneType) return zones::g_zone.load();  // the section this screen shows
    if (key_row(type) || type == kKeyLinkType || type == kKeyResetType) return 0;  // one choice (a key row: its key)
    const pc_setting *d;
    const PcRow *r = row_of(type, &d);
    return r ? index_of(*r, d) : -1;
}

void set_choice(s32 type, s32 choice) {
    if (type == kZoneType) return reopen::request(choice);  // another section: another screen
    const pc_setting *d;
    const PcRow *r = row_of(type, &d);
    if (!r || choice < 0 || static_cast<u32>(choice) >= count_of(values_of(*r, d))) return;
    pc_settings_set(r->key, values_of(*r, d)[choice]);
    if (d && (d->flags & PC_SET_RESTART)) pc_overlay_restart_notice();   // (the caption says it; the notice says how)
    refresh_others(type);
}

s32 toggle(s32 type) { return choice(type); }  // a two-value row: off, on

void set_toggle(s32 type, bool on) { set_choice(type, on ? 1 : 0); }

}  // namespace game::ui::pc_rows

namespace {
// ---------------------------------------------------------------- the row controllers
// The game's rows run the game's own controllers (f_<off>_orig, from the hooks below). Ours (pc_rows::owns) are all
// selections over a pc_settings key's values, run here:
//  - bind: the row's label ("10%02d" of its type) and the setting's current choice ("10%02d_%02d"), < off on the first
//    choice and > off on the last;
//  - < / >: one choice back or on, stopping at the ends, the pressed arrow animated;
//  - A on the row: the next choice, wrapping to the first;
//  - a changed choice goes to pc_settings, then its text shows.
// A toggle of ours, should one come: the label and the button's state from the setting, the state back when it
// changes.
namespace ctl {
// a row whose hook calls run the game's own controller
bool game_row(s32 type) { return !pc_rows::owns(type); }

// "10%02d" of the row's type, from SystemOption_00 (the overlay set's texts for our types), into its label pane
void show_label(void *layout, s32 type) {
    MessageString msg = {};
    guest::MessageStringCtor(&msg);
    u64 file = reinterpret_cast<u64>(guest_mem::guest_data<const char>(0x1003719b8b));  // "LayoutMsg/SystemOption_00"
    FixedSafeString32 label = {};
    guest::FormatFixedSafeString32(&label.top, "10%02d", type);
    *reinterpret_cast<char *>(reinterpret_cast<uintptr>(label.top) + static_cast<s64>(label.bufferSize) - 1) = 0;
    guest::FindMessage(&msg, reinterpret_cast<const char *const *>(&file), &label, true);
    guest::SetMessageString(layout, label_pane(), &msg, true, nullptr);
}

s32 choices(const OptionSelect *s) {
    OptionsMgr *const mgr = options_mgr();
    const OptionEntry *const e = mgr ? guest::FindEntry(mgr, s->type) : nullptr;
    return e ? static_cast<s32>(e->choiceCount) : 0;
}

void show_arrows(OptionSelect *s, s32 count) {
    guest::ArrowSetDisabled(s->arrowPrev, s->choice <= 0);
    guest::ArrowSetDisabled(s->arrowNext, s->choice >= count - 1);
}

void choose(OptionSelect *s, s32 to, s32 count) {
    if (to == s->choice) return;
    s->choice = to;
    show_arrows(s, count);
    pc_rows::set_choice(s->type, to);
    guest::SelectShowChoice(s);
}

void select_bind(OptionSelect *s, const OptionEntry *e) {
    s->type = e->type;
    s->index = e->index;
    s->nameHash = e->nameHash;
    const s32 count = static_cast<s32>(e->choiceCount), now = pc_rows::choice(e->type);
    s->choice = now >= 0 && now < count ? now : 0;
    show_label(s->layout, s->type);
    show_arrows(s, count);
    guest::SelectShowChoice(s);
}

void select_step(OptionSelect *s, u32 previous) {
    const s32 count = choices(s);
    if (count <= 0) return;
    const bool back = previous & 1;
    const s32 to = back ? std::max(s->choice - 1, 0) : std::min(s->choice + 1, count - 1);
    if (to != s->choice) guest::ArrowPlay(back ? s->arrowPrev : s->arrowNext);
    choose(s, to, count);
}

void select_decide(OptionSelect *s, void *button) {
    if (guest::FindButton(s->parts->buttonGroup, s->layout) != button) return;
    const s32 count = choices(s);
    if (count > 0) choose(s, (s->choice + 1) % count, count);
}

void toggle_bind(OptionToggle *t, const OptionEntry *e) {
    t->type = e->type;
    t->index = e->index;
    t->nameHash = e->nameHash;
    show_label(t->layout, t->type);
    const s32 on = pc_rows::toggle(t->type);
    if (on >= 0) guest::ButtonSetToggle(guest::FindButton(t->parts->buttonGroup, t->layout), static_cast<u32>(on));
}

void toggle_write(OptionToggle *t, void *button) {
    void *const found = guest::FindButton(t->parts->buttonGroup, t->layout);
    if (found == button) pc_rows::set_toggle(t->type, *(static_cast<const u8 *>(found) + kToggleState) != 0);
}
}  // namespace ctl

}  // namespace

// ---------------------------------------------------------------- hooks (gen.py --hook) A key without these hooks
// never calls hook_f_<off> and has no f_<off>_orig: on Windows the linker then takes options_orig_stub
// (/alternatename), elsewhere the weak reference stays null, so one runtime links against keys with and without them.
#ifdef _WIN32
void options_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_f6092c_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a862e4_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a86420_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a85b3c_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a8604c_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a85e4c_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a06840_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#pragma comment(linker, "/alternatename:?f_1a068cc_orig@@YAXPEIAUCtx@@@Z=?options_orig_stub@@YAXPEIAUCtx@@@Z")
#define OPTIONS_ORIG
#else
#define OPTIONS_ORIG __attribute__((weak))
#endif

OPTIONS_ORIG void f_f6092c_orig(Ctx *__restrict c);
void hook_f_f6092c(Ctx *__restrict c) {  // OptionsMgr::initialize(mgr, heap) -> bool
    OptionsMgr *const mgr = reinterpret_cast<OptionsMgr *>(c->x[0]);
    const u64 heap = c->x[1];
    f_f6092c_orig(c);
    const u64 ok = c->x[0];
    if (!enabled() || !(ok & 1) || !mgr) return;
    {
        guest_bridge::Enter e(c);
        append_rows(mgr, heap);
    }
    c->x[0] = ok;
}

#define OPTIONS_HOOK(off, call)                         \
    OPTIONS_ORIG void f_##off##_orig(Ctx *__restrict c);\
    void hook_f_##off(Ctx *__restrict c) {              \
        if (!enabled()) return f_##off##_orig(c);       \
        call;                                           \
    }
OPTIONS_HOOK(1a862e4, {   // a toggle row's bind (x0 the controller, x1 its entry)
    OptionToggle *const self = reinterpret_cast<OptionToggle *>(c->x[0]);
    const OptionEntry *const entry = reinterpret_cast<const OptionEntry *>(c->x[1]);
    const s32 row_type = entry->type;
    if (ctl::game_row(row_type)) f_1a862e4_orig(c);
    guest_bridge::Enter e(c);
    if (!ctl::game_row(row_type)) ctl::toggle_bind(self, entry);
    cull::add(self->type, zones::shown_index(entry), self->layout);
    orphans::hide(self->layout);
})
OPTIONS_HOOK(1a86420, {   // a toggle row's button changed state (x1 the button)
    OptionToggle *const self = reinterpret_cast<OptionToggle *>(c->x[0]);
    const s32 row_type = self->type;
    if (ctl::game_row(row_type)) return f_1a86420_orig(c);
    guest_bridge::Enter e(c);
    ctl::toggle_write(self, reinterpret_cast<void *>(c->x[1]));
})
OPTIONS_HOOK(1a85b3c, {   // a selection row's bind
    OptionSelect *const self = reinterpret_cast<OptionSelect *>(c->x[0]);
    const OptionEntry *const entry = reinterpret_cast<const OptionEntry *>(c->x[1]);
    const s32 row_type = entry->type;
    if (ctl::game_row(row_type)) f_1a85b3c_orig(c);
    guest_bridge::Enter e(c);
    if (!ctl::game_row(row_type)) ctl::select_bind(self, entry);
    cull::add(self->type, zones::shown_index(entry), self->layout);
    orphans::hide(self->layout);
    sels::add(self);
    if (!pc_rows::owns(self->type)) return;
    note_bound(self);
    if (const int i = key_index(self->type); i >= 0) show_key(static_cast<size_t>(i));  // over the placeholder
    if (self->type == kFovType) show_fov(self);
    show_dynamic(self);
    if (self->type == kKeyLinkType || self->type == kKeyResetType) {  // one choice: < and > change nothing
        guest::ArrowSetDisabled(self->arrowNext, true);
        guest::ArrowSetDisabled(self->arrowPrev, true);
    }
    if (self->type == kKeyResetType) {  // a new screen's reset row: idle
        g_reset_sel = self;
        g_reset_state = RS_IDLE;
    }
})
OPTIONS_HOOK(1a8604c, {   // < or > on a selection row (x1 bit 0: <)
    OptionSelect *const self = reinterpret_cast<OptionSelect *>(c->x[0]);
    const s32 row_type = self->type;
    if (ctl::game_row(row_type)) return f_1a8604c_orig(c);
    guest_bridge::Enter e(c);
    const s32 was = self->choice;
    ctl::select_step(self, static_cast<u32>(c->x[1]));
    if (self->type == kFovType && self->choice != was) show_fov(self);
    show_dynamic(self);
})
OPTIONS_HOOK(1a85e4c, {   // A on a selection row (x1 the cursor's button)
    OptionSelect *const self = reinterpret_cast<OptionSelect *>(c->x[0]);
    void *const button = reinterpret_cast<void *>(c->x[1]);
    const s32 row_type = self->type;
    if (ctl::game_row(row_type)) return f_1a85e4c_orig(c);
    guest_bridge::Enter e(c);
    const s32 was = self->choice;
    if (key_row(self->type)) {
        key_decide(self, button);
    } else if (self->type == kKeyLinkType) {  // A: F10 at its Keys tab (the game gets no input while it is open)
        if (guest::FindButton(self->parts->buttonGroup, self->layout) == button) {
            printf("[options-menu] keys link: F10 > Keys\n");
            pc_overlay_open_keys();
        }
    } else if (self->type == kKeyResetType) {
        reset_decide(self, button);
    } else {
        ctl::select_decide(self, button);
        show_dynamic(self);
    }
    if (self->type == kFovType && self->choice != was) show_fov(self);
})
#undef OPTIONS_HOOK

// The shown entries' count (0x1a06840: mgr -> u32) and the n-th shown entry (0x1a068cc: mgr, n -> entry), on the
// section's view (zones above); without sections, or with TOTK_OPTIONS_MENU=0, the game's list as it is
OPTIONS_ORIG void f_1a06840_orig(Ctx *__restrict c);
OPTIONS_ORIG void f_1a068cc_orig(Ctx *__restrict c);
void hook_f_1a06840(Ctx *__restrict c) {
    if (const OptionsMgr *v = zones::view(reinterpret_cast<const OptionsMgr *>(c->x[0]), c->x[30]))
        c->x[0] = reinterpret_cast<u64>(v);
    f_1a06840_orig(c);
}
void hook_f_1a068cc(Ctx *__restrict c) {
    if (const OptionsMgr *v = zones::view(reinterpret_cast<const OptionsMgr *>(c->x[0]), 0)) c->x[0] = reinterpret_cast<u64>(v);
    f_1a068cc_orig(c);
}
#undef OPTIONS_ORIG

namespace {
// A key built before these two hooks has neither: the alternate name makes each _orig the stub (Windows), the weak
// reference null (elsewhere); sections then stay off and the one list shows, as before. (Read through volatile
// pointers: the compiler may take two distinct functions' addresses as unequal and fold the test; the linker's
// alternate name is what makes them equal.)
bool zones::hooked() {
#ifdef _WIN32
    void (*volatile count)(Ctx *__restrict) = &f_1a06840_orig;
    void (*volatile index)(Ctx *__restrict) = &f_1a068cc_orig;
    void (*volatile stub)(Ctx *__restrict) = &options_orig_stub;
    return count != stub && index != stub;
#else
    return f_1a06840_orig != nullptr && f_1a068cc_orig != nullptr;
#endif
}
}  // namespace

// OptionSelect +0x38 / +0x30: the controllers of the row's Pa_ArrowL_00 / Pa_ArrowR_00 (0x40 bytes each, made by
// 0x1a85a1c from the row's 0x1ad8098), each with its part's pane at +0x20 (0x6f7584)
bool totk_options_reopening(void) { return enabled() && reopen::g_state != reopen::IDLE; }
bool totk_options_section_step(int dir) {
    if (!enabled() || !zones::on() || (dir != -1 && dir != 1)) return false;
    // (on eui's thread, as the section row's own step: reopen::request reads the opener's focus there)
    return totk_ui_post([](void *arg, bool ran) {
        // (no section row in this list, zones::g_ok, written on eui's thread: view() shows the one list, so nothing to
        // step to; closing and reopening Options would be for nothing)
        if (!ran || !zones::g_ok) return;
        const s32 d = static_cast<s32>(reinterpret_cast<intptr_t>(arg));
        reopen::request((zones::g_zone.load() + d + Z_COUNT) % Z_COUNT);
    }, reinterpret_cast<void *>(static_cast<intptr_t>(dir)));
}

bool totk_options_row_arrows(uint64_t layout, uint64_t *prev_pane, uint64_t *next_pane) {
    if (!enabled() || (totk_menu_screen() & 0xff) != TMS_OPTIONS || !layout) return false;
    for (const sels::Sel &s : sels::g_rows) {
        if (s.layout != layout) continue;
        if (!cull::member(sels::g_list, s.root)) return false;  // (the screen went: its rows are freed)
        const u64 l = reinterpret_cast<u64>(s.self->arrowPrev), r = reinterpret_cast<u64>(s.self->arrowNext);
        *prev_pane = cull::ok(l) ? cull::rd<u64>(l + 0x20) : 0;
        *next_pane = cull::ok(r) ? cull::rd<u64>(r + 0x20) : 0;
        return true;
    }
    return false;
}

