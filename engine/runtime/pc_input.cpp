// pc_input.cpp: PC-native controls. The context the keyboard and mouse are read in, and the routing table from (context,
// PC input) to the game's action: pc_scheme.inc's rows, inherited down the context tree, with settings.ini overrides per
// level. hle_hid.cpp keyboard_read calls pc_route once per pad read; what the table does not claim in the world keeps the
// console layout (TOTK_KEYBOARD_LAYOUT) until stage 2.
//
// Edges, not levels (4.3): a P / O row acts once per press; an H row holds its pad bits while its key is held AND the
// context still routes that key to the same action. At a context change every pulse ends, and a key already held keeps
// its hold only if the new context gives it the same action as a held row and it was pressed on one; otherwise it is dead
// until released (no Shift run out of a menu, no Esc that backs out of the pause menu it just opened, no V that reopens
// the Scope it just closed).
#include "pc_input.h"
#include "totk_version.h"
#include "pc.h"   // pc_fault_guard (in_event)
#include "pc_settings.h"
#include "totk_menu_cursor.h"
#include "totk_input_cuts.h"  // totk_input_reason: the game's own input cuts

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
extern "C" int hid_vk_from_name(const char *name);   // hle_hid.cpp: the bindings' key names
extern "C" void *shim_window_hwnd(void);             // shim_win.c: the game window
extern "C" void osd_toast(const char *text, int ms); // hle_osd.cpp: the bottom-left notice line
extern "C" int osd_toast_busy(const char *text);     // (another notice is up there)
extern "C" int pc_lang_es(void);                     // pc_lang.cpp
extern "C" const char *pc_tr(const char *en);        // pc_text.cpp
extern "C" void pc_mouse_redirect(bool on);          // pc_mouse.cpp: a macro's take_mouse
extern "C" void pc_mouse_redirect_take(double *dx, double *dy);

namespace {

// hle_hid.cpp's action bits (HostInput buttons, then the left and the right stick's directions)
enum : uint64_t {
    B_A = 1u << 0, B_B = 1u << 1, B_X = 1u << 2, B_Y = 1u << 3, B_LS = 1u << 4, B_RS = 1u << 5, B_L = 1u << 6, B_R = 1u << 7,
    B_ZL = 1u << 8, B_ZR = 1u << 9, B_PLUS = 1u << 10, B_MINUS = 1u << 11, B_DL = 1u << 12, B_DU = 1u << 13, B_DR = 1u << 14,
    B_DD = 1u << 15, M_L = 1u << 16, M_U = 1u << 17, M_R = 1u << 18, M_D = 1u << 19,
    R_L = 1u << 20, R_U = 1u << 21, R_R = 1u << 22, R_D = 1u << 23,  // (hle_hid's camera axes: the inventory's Rotate)
    M_WALK = 1u << 24  // (hle_hid's kWalk slot: the keys' left stick at 45 %; no pad button, so no prompt)
};
// Scopes: the contexts (PcCtx values), then the groups
enum Scope { S_ROOT = 100, S_GAMEPLAY, S_UI, S_MENUS };
static_assert(S_ROOT == PC_SCOPE_ROOT && S_GAMEPLAY == PC_SCOPE_GAMEPLAY && S_UI == PC_SCOPE_UI && S_MENUS == PC_SCOPE_MENUS &&
              PCX_COUNT < S_ROOT, "pc_input.h's PC_SCOPE_* are the router's scopes");
#define WORLD PCX_WORLD
#define OVERLAY PCX_OVERLAY
#define TITLE PCX_TITLE
#define SAVES PCX_SAVES
#define PAUSE PCX_PAUSE
#define POPUP PCX_POPUP
#define PAD PCX_PAD
#define MAP PCX_MAP
#define OPTIONS PCX_OPTIONS
#define SHOP PCX_SHOP
#define MENU PCX_MENU
#define DIALOGUE PCX_DIALOGUE
#define CHOICE PCX_CHOICE
#define CUTSCENE PCX_CUTSCENE
#define CLIMB PCX_CLIMB
#define AIR PCX_AIR
#define GLIDE PCX_GLIDE
#define SWIM PCX_SWIM
#define AIM PCX_AIM
#define ABILITY PCX_ABILITY
#define HORSE PCX_HORSE
#define VEHICLE PCX_VEHICLE
#define QUICKMENU PCX_QUICKMENU
#define PADLOG PCX_PADLOG
#define CAMERA PCX_CAMERA
#define SCOPE PCX_SCOPE
#define CONSTRUCT PCX_CONSTRUCT
#define RECIPE PCX_RECIPE
#define LOADING PCX_LOADING
#define ROOT S_ROOT
#define GAMEPLAY S_GAMEPLAY
#define UI S_UI
#define MENUS S_MENUS

// vk: a G row's virtual-key mask (PC_VROW; several bits at once, e.g. guard + target); out stays its prompt's pad bits
struct Row { int scope; const char *action; char kind; int op, arg; uint64_t out; const char *keys; uint64_t vk; };
// pc_scheme.inc, then pc_scheme_gameplay.inc. PC_NOLAYOUT(scope): that gameplay scope no longer falls back to
// the console layout for what it does not list, set as its rows land.
#if __has_include("totk_pcvk.h")
#include "totk_pcvk.h"   // (totk_pcvk_frames: the virtual-key hook has run)
#define PC_HAVE_PCVK 1
#endif
#if __has_include("pc_gameplay.h")
#include "pc_gameplay.h"
#endif
#define PC_NOLAYOUT(scope)
const Row kRows[] = {
#define PC_ROW(scope, action, kind, op, arg, out, keys) {scope, action, kind, op, arg, out, keys, 0},
#define PC_VROW(scope, action, vk, glyph_out, keys) {scope, action, 'G', PCOP_NONE, 0, glyph_out, keys, vk},
#include "pc_scheme.inc"
#include "pc_scheme_gameplay.inc"
#undef PC_ROW
#undef PC_VROW
};
#undef PC_NOLAYOUT
#define PC_ROW(scope, action, kind, op, arg, out, keys)
#define PC_VROW(scope, action, vk, glyph_out, keys)
#define PC_NOLAYOUT(scope) scope,
const int kNoLayout[] = {
#include "pc_scheme.inc"
    -1};
const int kNoLayoutGameplay[] = {   // (pc_scheme_gameplay.inc's: only with controls.gameplay = pc)
#include "pc_scheme_gameplay.inc"
    -1};
#undef PC_ROW
#undef PC_VROW
#undef PC_NOLAYOUT
// kRows[0.. kStage1Rows) are pc_scheme.inc's, the rest pc_scheme_gameplay.inc's (the gameplay layer)
#define PC_ROW(scope, action, kind, op, arg, out, keys) 1,
#define PC_VROW(scope, action, vk, glyph_out, keys) 1,
#define PC_NOLAYOUT(scope)
const char kStage1Mark[] = {
#include "pc_scheme.inc"
};
#undef PC_ROW
#undef PC_VROW
#undef PC_NOLAYOUT
constexpr int kStage1Rows = (int)sizeof kStage1Mark;
// linked pairs (pc_scheme_pairs.inc, 4.19): a macro's trigger and a key it reads while it runs never share a key
struct Pair { const char *a, *b; };
const Pair kPairs[] = {
#define PC_PAIR(a, b) {a, b},
#include "pc_scheme_pairs.inc"
#undef PC_PAIR
    {nullptr, nullptr}   // (an end mark: the file may list none, 4.22 retired aim / fire)
};
constexpr int kPairCount = (int)(sizeof kPairs / sizeof kPairs[0]) - 1;
// controls.gameplay: "pc" = the gameplay contexts take pc_scheme_gameplay.inc's rows; "console" = they keep the
// console layout for every key but the stage-1 rows (Esc / I / M / J), as the stage-1 acceptance ran them. Live: the
// F10 row's change rebuilds the table at the next read.
std::atomic<int> g_gameplay_pc{-1};   // -1 not read yet
std::atomic<bool> g_rebuild{false};
// bumped by every change the bind answers depend on (a table build, a bind write, the gameplay mode): pc_bind_get's
// cache key
std::atomic<unsigned> g_bind_epoch{1};
bool gameplay_pc() {
    int v = g_gameplay_pc.load();
    if (v < 0) {   // (TOTK_GAMEPLAY, else the registry's row value; console, off or 0 in any case: console)
        char s[16] = {};
        const char *e = getenv("TOTK_GAMEPLAY");
        if (e && *e) snprintf(s, sizeof s, "%s", e);
        // (the row's value, as F10 and the in-game Options show it: a file value its list rejects is the default there,
        // so it is here too; raw, "off" ran console under a row reading PC)
        else pc_settings_copy("controls.gameplay", s, sizeof s);
        v = !(!_stricmp(s, "console") || !_stricmp(s, "off") || !strcmp(s, "0"));
        g_gameplay_pc = v;
    }
    return v != 0;
}
constexpr int kRowCount = (int)(sizeof kRows / sizeof kRows[0]);
// a T row's hold row (pc_scheme.inc: the next row, same scope and action; test_pc_scheme checks the pair), else -1
constexpr uint64_t kTapMs = 250;
int16_t hold_of(int16_t r) {
    static const auto tab = [] {
        std::vector<int16_t> h(kRowCount, -1);
        for (int i = 0; i + 1 < kRowCount; i++)
            if (kRows[i].kind == 'T' && kRows[i + 1].kind != 'T' && kRows[i + 1].scope == kRows[i].scope &&
                !strcmp(kRows[i + 1].action, kRows[i].action))
                h[i] = (int16_t)(i + 1);
        return h;
    }();
    return r >= 0 && r < kRowCount ? tab[r] : -1;
}

const char *const kCtxNames[PCX_COUNT] = {"world", "overlay", "title", "saves", "pause", "popup", "pad", "map", "options", "shop",
                                          "menu", "dialogue", "choice", "event", "transition", "climb", "air", "glide",
                                          "swim", "aim", "ability", "horse", "vehicle", "loading",
                                          "quickmenu", "padlog", "camera", "scope", "construct", "recipe"};
int parent(int s) {
    switch (s) {
    case PCX_CUTSCENE: return S_ROOT;   // (an event: its own rows only, every other key dead; still not a UI context)
    case PCX_QUICKMENU: return PCX_WORLD;   // (on foot's rows: the hold that opened it, WASD, carry on)
    case PCX_PADLOG: return PCX_PAD;        // (the Pad's rows, plus J = close)
    case PCX_WORLD: case PCX_CLIMB: case PCX_AIR: case PCX_GLIDE: case PCX_SWIM: case PCX_AIM:
    case PCX_ABILITY: case PCX_HORSE: case PCX_VEHICLE: case PCX_CAMERA: case PCX_SCOPE: case PCX_CONSTRUCT:
        return S_GAMEPLAY;
    case PCX_LOADING: return -1;
    case PCX_OVERLAY: return S_ROOT;
    case PCX_TRANSITION: return -1;   // (no rows at all: every key dead)
    case PCX_TITLE: case PCX_DIALOGUE: case PCX_CHOICE: return S_UI;
    case PCX_SAVES: case PCX_PAUSE: case PCX_POPUP: case PCX_PAD: case PCX_MAP: case PCX_OPTIONS: case PCX_SHOP: case PCX_MENU:
    case PCX_RECIPE:
        return S_MENUS;
    case S_MENUS: return S_UI;
    case S_UI: case S_GAMEPLAY: return S_ROOT;
    default: return -1;
    }
}
const char *scope_name(int s) {
    return s >= 0 && s < PCX_COUNT ? kCtxNames[s] : s == S_GAMEPLAY ? "gameplay" : s == S_UI ? "ui" : s == S_MENUS ? "menus"
           : s == S_ROOT ? "root" : nullptr;
}
// (bounded: a parent() mistake once made on foot its own parent, and the walk spun in the file thread's glyph serve
// before the first frame; tree_ok() below refuses such a table at the first build)
bool in_scope(int ctx, int s) { int n = 0; for (int c = ctx; c >= 0 && n++ < 16; c = parent(c)) if (c == s) return true; return false; }
bool tree_ok() {
    for (int c = 0; c < PCX_COUNT; c++) {
        int n = 0, s = c;
        while (s >= 0 && n++ < 16) s = parent(s);
        if (s >= 0) { printf("[pc] the context tree loops at %d: the PC scheme is off (the console layout)\n", c); return false; }
    }
    return true;
}

// inputs: the 256 virtual keys, then the wheel's two directions
constexpr int kWheelUp = 256, kWheelDown = 257, kInputs = 258;
// Letters and digits are bound by position, as most PC games bind them: a name is the key in that place on a US
// keyboard, its scan code, read as whatever key the keyboard layout in use puts there (WASD is ZQSD on AZERTY). The
// other keys (Esc, Enter, Space, Shift, the arrows, F-keys, the mouse) sit in the same place everywhere.
HKL g_hkl;   // the layout the table was built with (the game window's thread's)
HKL layout_now() {
    DWORD tid = 0;
    if (HWND h = (HWND)shim_window_hwnd()) tid = GetWindowThreadProcessId(h, nullptr);
    return GetKeyboardLayout(tid);
}
int us_scan(char c) {   // set-1 scan code of a US keyboard's letter or digit
    static const char rows[][11] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const int first[] = {0x02, 0x10, 0x1E, 0x2C};
    for (int r = 0; r < 4; r++) if (const char *p = strchr(rows[r], c)) return first[r] + (int)(p - rows[r]);
    return 0;
}
// the US keyboard's other character keys by position (set-1 scan codes), named "vk 0x.." in the settings: they too are
// bound by where they sit, read through the layout in use (pc_bind_set writes them so)
const struct { int vk, scan; } kUsOem[] = {{VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B}, {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34},
    {VK_OEM_2, 0x35}, {VK_OEM_102, 0x56}};
int us_scan_of(int vk) {   // a US VK's scan code when the key is bound by position (letters, digits, kUsOem), else 0
    if (vk >= 'A' && vk <= 'Z') return us_scan((char)(vk - 'A' + 'a'));
    if (vk >= '0' && vk <= '9') return us_scan((char)vk);
    for (auto &o : kUsOem) if (o.vk == vk) return o.scan;
    return 0;
}
int us_vk_of_scan(int scan) {   // the inverse: a scan code's US VK, 0 for a key not bound by position
    for (int vk = '0'; vk <= 'Z'; vk++) if ((vk <= '9' || vk >= 'A') && us_scan_of(vk) == scan) return vk;
    for (auto &o : kUsOem) if (o.scan == scan) return o.vk;
    return 0;
}
int input_from_name(std::string n) {
    while (!n.empty() && isspace((unsigned char)n.front())) n.erase(0, 1);
    while (!n.empty() && isspace((unsigned char)n.back())) n.pop_back();
    for (auto &c : n) c = (char)tolower((unsigned char)c);
    if (n == "wheel up") return kWheelUp;
    if (n == "wheel down") return kWheelDown;
    if (n.size() == 1 && isalnum((unsigned char)n[0]))
        if (const int sc = us_scan(n[0]))
            if (const int vk = (int)MapVirtualKeyExW((UINT)sc, MAPVK_VSC_TO_VK, g_hkl)) return vk;
    const int vk = n.empty() || n == "none" ? 0 : hid_vk_from_name(n.c_str());
    for (auto &o : kUsOem)   // (a punctuation key by position too)
        if (o.vk == vk)
            if (const int live = (int)MapVirtualKeyExW((UINT)o.scan, MAPVK_VSC_TO_VK, g_hkl)) return live;
    return vk;
}
const char *input_name(int k, char *buf, size_t n) {
    if (k == kWheelUp) return "Wheel Up";
    if (k == kWheelDown) return "Wheel Down";
    snprintf(buf, n, "vk 0x%02x", k);
    return buf;
}

// The table each context reads: per input the row it routes to (-1 none), and per pad button the prompt's key
struct Table {
    int16_t route[PCX_COUNT][kInputs];
    int glyph[PCX_COUNT][24];          // first key of the context's first row giving that button / stick direction (bits
                                       // 16-23: the left / right stick's); 0 the layout's, -1 none
    std::vector<int> keys[PCX_COUNT];  // the inputs routed in each context (read every pad read)
    bool layout[PCX_COUNT];            // the console layout reads what the context does not route (gameplay, stage 1)
    int gen = -1;
};
Table g_t;
std::mutex g_glyph_mu;   // g_t.glyph (the fs thread's glyph lookups read it)
int g_glyph_tab[PCX_COUNT][24];
std::atomic<int> g_ctx{PCX_WORLD};
std::atomic<int> g_last_game{PCX_WORLD};   // the last gameplay context (pc_glyph_game_set)
std::atomic<int> g_forced{-1};
std::atomic<bool (*)(int, int)> g_ops{nullptr};
std::atomic<int> g_scheme{-1};   // 1 the PC scheme, 0 the console layout; -1 not read

bool tree_ok();
bool scheme_read() {
    if (!tree_ok()) return false;
    char v[32] = {};
    const char *e = getenv("TOTK_CONTROLS");
    if (e && *e) snprintf(v, sizeof v, "%s", e);
    else pc_settings_raw("controls.scheme", v, sizeof v);
    // (default on since stage 1's acceptance TOTK_CONTROLS / controls.scheme = console, classic, off or 0, any case:
    // the console layout)
    return !(!_stricmp(v, "console") || !_stricmp(v, "classic") || !_stricmp(v, "off") || !strcmp(v, "0"));
}

std::vector<int> parse_keys(const char *s) {
    std::vector<int> r;
    std::string str(s ? s : "");
    for (size_t a = 0; a <= str.size();) {
        size_t e = str.find(',', a);
        if (e == std::string::npos) e = str.size();
        const std::string tok = str.substr(a, e - a);
        const int k = input_from_name(tok);
        if (k > 0) r.push_back(k);
        else if (tok.find_first_not_of(" ") != std::string::npos && input_from_name(tok) == 0 && tok != "none")
            printf("[pc] unknown key \"%s\"\n", tok.c_str());
        a = e + 1;
    }
    return r;
}

// Builds g_t from the rows and the settings: per context, each action from its most specific row in the context's
// chain, its keys from the most specific settings level that has them (controls.<ctx>.<action>, controls.<group>.<action>,
// controls.<action>), else the row's. Two actions on one input in one context: the later is refused and logged (the
// overlay's pc_bind_set swaps instead, stage 3).
// The settings a table is built from: the file's (pc_settings_raw), or those with pc_bind_set's proposal over them (a
// value "" = that key deleted). One key per action level: controls.<action> (ROOT), controls.<scope>.<action>.
struct Look {
    const std::vector<std::pair<std::string, std::string>> *over = nullptr;
    bool get(const char *key, char *val, size_t n) const {
        if (over)
            for (auto &e : *over)
                if (e.first == key) {
                    if (e.second.empty()) return false;
                    snprintf(val, n, "%s", e.second.c_str());
                    return true;
                }
        return pc_settings_raw(key, val, n);
    }
};
std::string setting_key(int scope, const char *action) {
    return scope == S_ROOT ? std::string("controls.") + action : std::string("controls.") + scope_name(scope) + "." + action;
}
// The row an action has in a context (the nearest up its chain; -1 none). rows2: pc_scheme_gameplay.inc's rows count
// (build's rule: always outside gameplay, else with controls.gameplay = pc; the quickmenu's always)
int row_of(int c, const char *action, bool gp) {
    const bool rows2 = gp || !in_scope(c, S_GAMEPLAY);
    for (int s = c; s >= 0; s = parent(s))
        for (int r = 0; r < kRowCount; r++)
            if (kRows[r].scope == s && !strcmp(kRows[r].action, action) &&
                !(r >= kStage1Rows && !rows2 && kRows[r].scope != PCX_QUICKMENU))
                return r;
    return -1;
}
// One context's table: each action from its most specific row in the context's chain, its keys from the most specific
// settings level that has them, else the row's. Two actions on one input: the later is refused (logged unless quiet).
void build_ctx(Table &t, int c, bool gp, const Look &look, int *overrides, int *conflicts, bool quiet) {
    // (controls.gameplay = console: pc_scheme_gameplay.inc's rows and no-layout marks are not the gameplay contexts')
    const bool rows2 = gp || !in_scope(c, S_GAMEPLAY);
    for (int k = 0; k < kInputs; k++) t.route[c][k] = -1;
    t.keys[c].clear();
    t.layout[c] = in_scope(c, S_GAMEPLAY);
    for (int s = c; s >= 0; s = parent(s))
        for (int n : kNoLayout) if (n == s) t.layout[c] = false;
    if (rows2)
        for (int s = c; s >= 0; s = parent(s))
            for (int n : kNoLayoutGameplay) if (n == s) t.layout[c] = false;
    // (-1 none, 0 the layout's: a no-layout gameplay context draws no key for a button no row gives, not the layout's,
    // which is dead there: GLIDE's L drew F, HORSE's ZR RMB, CAMERA's ZL Q)
    for (int b = 0; b < 24; b++) t.glyph[c][b] = in_scope(c, S_UI) || c == PCX_TRANSITION || !t.layout[c] ? -1 : 0;
    if (c == PCX_OVERLAY) return;   // (the overlay has the keys itself)
    std::vector<std::string> seen;
    int wheel_glyph[24] = {};   // (a button only the wheel gives: its glyph if no key row gives it, below)
    // the chain from the context up: a row of a nearer scope wins for its action
    for (int s = c; s >= 0; s = parent(s))
        for (int r = 0; r < kRowCount; r++) {
            const Row &row = kRows[r];
            // (the quickmenu keeps its own rows in both modes: LMB confirm, Esc cancel)
            if (row.scope != s || (r >= kStage1Rows && !rows2 && row.scope != PCX_QUICKMENU)) continue;
            bool dup = false;
            for (auto &a : seen) dup |= a == row.action;
            if (dup) continue;
            seen.push_back(row.action);
            if (row.kind == 'X') continue;   // (dead here: no override revives it, 4.20)
            std::vector<int> keys;
            bool over = false;
            // (an 'E' row is another part's fixed input: settings never move it, so a ROOT bind of the same action
            // name, e.g. attack, leaves the / the map's own key alone)
            for (int o = c; o >= 0 && !over && row.kind != 'E'; o = parent(o)) {
                char val[96];
                if (look.get(setting_key(o, row.action).c_str(), val, sizeof val)) { keys = parse_keys(val); over = true; }
            }
            if (over) ++*overrides;
            else keys = parse_keys(row.keys);
            for (int k : keys) {
                if (t.route[c][k] >= 0) {
                    char b[16];
                    if (!quiet)
                        printf("[pc] %s: %s is %s's already, not also %s's\n", kCtxNames[c], input_name(k, b, sizeof b),
                               kRows[t.route[c][k]].action, row.action);
                    ++*conflicts;
                    continue;
                }
                t.route[c][k] = (int16_t)r;
                t.keys[c].push_back(k);
            }
            // (a T row: its hold row's buttons too, which build never routes: hold 1's d-right strip prompt shows 1)
            const uint64_t shown = row.out | (row.kind == 'T' && hold_of((int16_t)r) >= 0 ? kRows[hold_of((int16_t)r)].out : 0);
            if (!keys.empty() && keys[0] < 256)
                for (int b = 0; b < 24; b++)
                    if ((shown >> b & 1) && t.glyph[c][b] <= 0) t.glyph[c][b] = keys[0];
            if (!keys.empty() && keys[0] >= 256)
                for (int b = 0; b < 24; b++)
                    if ((shown >> b & 1) && !wheel_glyph[b]) wheel_glyph[b] = keys[0];
        }
    // a button only a wheel row gives (a rebind to the wheel, the menus' scroll rows where no key has the D-pad's up /
    // down): it draws the wheel's direction (hle_hid vk_glyph 256 / 257, a pair the vertical wheel); a key row wins
    for (int b = 0; b < 24; b++) if (t.glyph[c][b] <= 0 && wheel_glyph[b]) t.glyph[c][b] = wheel_glyph[b];
}
// A linked pair (pc_scheme_pairs.inc) on one input anywhere in gameplay: the pair's index (*input: that input), else -1
int pair_clash(const Table &t, int *input) {
    for (int p = 0; p < kPairCount; p++) {
        bool in_a[kInputs] = {}, in_b[kInputs] = {};   // (the input thread's build and a bind's dry run may overlap)
        for (int c = 0; c < PCX_COUNT; c++)
            if (in_scope(c, S_GAMEPLAY))
                for (int k : t.keys[c]) {
                    const int16_t r = t.route[c][k];
                    if (r >= 0 && !strcmp(kRows[r].action, kPairs[p].a)) in_a[k] = true;
                    if (r >= 0 && !strcmp(kRows[r].action, kPairs[p].b)) in_b[k] = true;
                }
        for (int k = 0; k < kInputs; k++)
            if (in_a[k] && in_b[k]) { if (input) *input = k; return p; }
    }
    return -1;
}
void build(int gen) {
    int overrides = 0, conflicts = 0;
    const bool gp = gameplay_pc();
    static int gp_was = -1;
    if (gp_was != (int)gp) { gp_was = gp; printf("[pc] gameplay keys: %s\n", gp ? "pc" : "console"); }
    for (int c = 0; c < PCX_COUNT; c++) build_ctx(g_t, c, gp, Look(), &overrides, &conflicts, false);
    // a saved bind of an action the scheme no longer has (4.22 parry, 4.23 aim) is never read: no row looks it up.
    // Said once, so a dev settings file's stale line reads as ignored, not as a bind that silently failed
    static bool told_retired;
    if (!told_retired) {
        told_retired = true;
        for (const char *a : {"aim", "parry"})
            for (int o = 0; o < S_MENUS + 1; o = o == PCX_COUNT - 1 ? S_ROOT : o + 1) {
                char val[96];
                if (Look().get(setting_key(o, a).c_str(), val, sizeof val))
                    printf("[pc] settings: %s = %s ignored (no such action in the PC scheme any more)\n",
                           setting_key(o, a).c_str(), val);
            }
    }
    // a linked pair on one key anywhere in gameplay (the settings made it: the defaults are kept apart by the test):
    // logged, as a conflict (pc_bind_set refuses making one)
    int clash_in = 0;
    if (const int p = pair_clash(g_t, &clash_in); p >= 0) {
        char nb[16];
        printf("[pc] %s and %s share %s: the macro misreads it (settings; pc_scheme_pairs.inc)\n", kPairs[p].a, kPairs[p].b,
               input_name(clash_in, nb, sizeof nb));
        conflicts++;
    }
    g_t.gen = gen;
    g_bind_epoch++;
    {
        std::lock_guard<std::mutex> l(g_glyph_mu);
        memcpy(g_glyph_tab, g_t.glyph, sizeof g_glyph_tab);
    }
    {   // (a rebind can make a prompt name a key that does nothing there: said in the log, 4.29)
        static int audits;
        if (audits < 20)
            if (const int bad = pc_glyph_audit(1)) { audits++; printf("[pc] glyph audit: %d prompt key(s) that do nothing\n", bad); }
    }
    static int logs;
    if (logs++ < 20)
        printf("[pc] scheme: %d rows, %d settings overrides, %d conflicts refused, keyboard layout %08llx (W at the US W: %s), "
               "gameplay keys: %s\n",
               kRowCount, overrides, conflicts, (unsigned long long)(uintptr_t)g_hkl,
               [] { static char s[8]; const int vk = input_from_name("w"); snprintf(s, sizeof s, "%c", vk >= 'A' && vk <= 'Z' ? vk : '?'); return s; }(),
               gp ? "PC (pc_scheme_gameplay.inc)" : "console layout (controls.gameplay)");
}

// F10 > Controls: the scheme and the gameplay keys, both live (pc_input_scheme_set / pc_input_gameplay_set: the table
// rebuilt at the next read). The end state: this is the one controls.gameplay row (the removed). The scheme's readers
// all ask pc_scheme_on() per call (the HID frame, prompts, binds, the mouse capture), so a change from F10 (game input
// held while it is open) takes at the next frame. TOTK_CONTROLS still wins over the row.
extern "C" void pc_input_focus(bool focused);   // (below: the accessibility hotkeys off / back)
extern "C" void pc_input_keys_restore(void);
void pc_input_scheme_set(const char *v) {
    if (g_scheme.load() < 0) return;   // (not read yet: pc_scheme_on reads it, after tree_ok, at its first call)
    const char *e = getenv("TOTK_CONTROLS");
    const bool pc = e && *e ? g_scheme.load() != 0
                            : !(v && (!_stricmp(v, "console") || !_stricmp(v, "classic") || !_stricmp(v, "off") || !strcmp(v, "0")));
    if (pc == (g_scheme.load() != 0)) return;
    g_scheme = pc ? 1 : 0;
    g_rebuild = true;
    g_bind_epoch++;   // (prompts and bind lists follow, as for a gameplay keys change)
    printf("[pc] controls: %s (F10)\n", pc ? "the PC scheme" : "the console layout");
    // the accessibility hotkeys follow the scheme now, not at the next Alt+Tab
    if (pc) pc_input_focus(GetForegroundWindow() == (HWND)shim_window_hwnd());
    else pc_input_keys_restore();
}
const char *const kGameplayV[] = {"console", "pc", nullptr};
const char *const kGameplayN[] = {"Console layout", "PC", nullptr};
const char *const kSchemeV[] = {"pc", "console", nullptr};
const char *const kSchemeN[] = {"PC", "Console", nullptr};
const pc_setting kSchemeRows[] = {
    {"controls.scheme", "Controls", "Keyboard and mouse", "Control scheme", "PC: the keyboard and mouse controls of the "
     "menus (Q / E tabs, Esc back, the mouse pointer), Esc / I / M / J in the world and the prompts with PC keys. Console: "
     "the keyboard layout below in every screen.", "pc", kSchemeV, kSchemeN, 0, 0, 0, 0, nullptr, nullptr,
     pc_input_scheme_set, nullptr},
    {"controls.gameplay", "Controls", "Keyboard and mouse", "Gameplay keys", "TOTK_GAMEPLAY. With the PC scheme: PC (default) = "
     "one key per action: Space jumps, Shift sprints, F attacks, Q guards, 4-9 pick an ability, Tab opens the wheel...; "
     "Console layout = the keys as before in the world. Menus use the PC keys either way.", "pc", kGameplayV, kGameplayN, 0, 0, 0, 0,
     "TOTK_GAMEPLAY", nullptr, pc_input_gameplay_set, nullptr},
};
struct SchemeRows { SchemeRows() { pc_settings_add(kSchemeRows, (int)(sizeof kSchemeRows / sizeof kSchemeRows[0])); } } g_scheme_rows;

// router state (input thread)
// (any: the title's any-key A; mac: a macro's claim; trow: the T row the key went down on, tap undecided while
// row == trow, else row is its hold row; at: the press)
struct KeyState { bool down = false, dead = false, any = false, mac = false; int16_t row = -1, trow = -1; uint64_t at = 0; };
bool g_mclaim[256];   // the inputs the running macros claim (PcMacroOut.claim, their last ticks)
std::atomic<bool> g_title_press{false};   // the title's "press a button" phase (pc_ctx_decide)
KeyState g_keys[kInputs];
bool g_phys_prev[256];   // each key down at the previous read (pc_route: a press on a context change's own read is a fresh one)
bool cap_quiet();   // (a rebinding capture open or just over: below, with pc_input_capture_*)
bool g_op_done[kInputs];   // (the log: the press's menu op ran)
std::atomic<int> g_qm_sub{0};
std::atomic<bool (*)(double, double, int, int)> g_qm_feed{nullptr};  // the UI-level quickmenu feed // the quickmenu's kind: 0 the ability wheel (radial), else a strip (the sub)
struct Follow { int op = PCOP_NONE, arg = 0, from = -1; uint64_t until = 0; };
Follow g_follow;           // a kind F row's op, waiting for the context its pulse opens
struct Queued { int k; uint64_t at; };
Queued g_queued[8];        // presses made during "transition", played when the screen settles (pc_route)
bool g_qm_holders[256], g_qm_has;   // the keys a quickmenu opened with (pc_route)
std::atomic<bool> g_qm_closing{false};   // all let go: it is closing (input thread; pc_quickmenu_closing reads it anywhere)
bool g_qm_replay;          // its queued presses wait for the opening key's macro run to end (pc_route)
uint64_t g_qm_replay_at;
int g_queued_n;
// A press is one pulse of 2 game frames (4.11 P4: the game samples once a frame; counted in presents, so a hitch
// stretches it instead of eating it), 2 frames after the previous one on the same buttons ended (wheel notches).
struct Pulse { uint64_t bits; bool started; uint64_t start, end; };
std::vector<Pulse> g_pulses;
int g_last_ctx = -1;
constexpr uint64_t kPulseFrames = 2, kGapFrames = 2;

void pulse(uint64_t bits, uint64_t) {
    if (bits) g_pulses.push_back({bits, false, 0, 0});
}
// G rows go to the virtual keys once the key's hook has run in this process, for good ("ever seen": the node does not
// run in a cutscene, and keys must not flip back then); before that, and on keys without the hook, a G row acts as an
// H row on its prompt's pad bits
bool g_vk_live;
bool vk_live() {
#ifdef PC_HAVE_PCVK
    if (!g_vk_live && totk_pcvk_frames() > 0) { g_vk_live = true; printf("[pc] the virtual-key hook runs: G rows set virtual keys from now on\n"); }
#endif
    return g_vk_live;
}
// a G row's virtual keys: PC_VROW's mask, else PC_ROW's arg as one bit
uint64_t row_vk(const Row &r) { return r.vk ? r.vk : 1ull << (r.arg & 63); }
bool same_action(int16_t a, int16_t b) { return a >= 0 && b >= 0 && !strcmp(kRows[a].action, kRows[b].action); }
// The strips' keys stay open on a tap: a strip key let go within kTapMs reads as still held (its row's d-pad, its strip
// macro: the router's one key state, pc_route's is_down) until it is pressed again, LMB or Esc closes it (that press is
// the close and nothing else: dead until let go), which lets the key go, and the game equips the item in front, as a
// held key's release does. Held longer, it works as before. It also lets go once the strip has gone (the game closed
// it, a menu) or never came (1 s), and when another strip key is pressed (which opens its own).
struct Sticky { int key = 0; uint64_t at = 0; bool seen = false; };
Sticky g_stk;
bool g_stk_eaten[256];   // a close's press: dead until let go
const char *const kStripActions[] = {"weapons", "shields", "bows", "materials"};
bool strip_key(int c, int k) {   // k opens a strip in the gameplay context c (its row, a T pair's hold row)
    if (c < 0 || c >= PCX_COUNT || pc_ctx_is_ui(c) || k <= 0 || k >= 256) return false;
    int16_t r = g_t.route[c][k];
    if (r >= 0 && kRows[r].kind == 'T') r = hold_of(r);
    if (r < 0) return false;
    for (const char *a : kStripActions) if (!strcmp(kRows[r].action, a)) return true;
    return false;
}
// macros (kind M): up to 4 run at once, one per key (the bow held while R opens the arrow strip); a new press of a key
// replaces only that key's run. Outputs ORed, sticks from the run started last, the mouse to the run started last that
// asks for it.
constexpr int kMacros = 64, kRuns = 4;
std::atomic<const PcMacroOps *> g_macros[kMacros];
struct Running { int id = -1, input = 0, ctx = -1; int16_t row = -1; bool take_mouse = false; uint64_t seq = 0; };
Running g_runs[kRuns];
uint64_t g_run_seq;
void macro_stop(Running &r, bool abort) {
    if (r.id < 0) return;
    const PcMacroOps *m = g_macros[r.id].load();
    if (abort && m && m->abort) m->abort(r.input);
    r = Running();
}
bool macro_mouse_wanted() { for (auto &r : g_runs) if (r.id >= 0 && r.take_mouse) return true; return false; }
// TOTK_PC_LOG=1: one line per key edge pc_route: <ctx> <input> <down|up> -> <action> <kind I|N|M|G|E> [<pad buttons> |
// op <name>[, pulsed <pad buttons>]] pc_route: <ctx> <input> <down|up> -> dead kinds: I a pad button held or pulsed, N
// a menu op, M a macro, G a virtual button, E another part's (the click, the map's mouse). Names: pc_scheme.inc's ids,
// inputs in lower case.
bool route_log() {   // (TOTK_PC_TRACE=1, hle_hid's npad trace, turns it on too)
    static const bool on = (getenv("TOTK_PC_LOG") && atoi(getenv("TOTK_PC_LOG")) != 0) || (getenv("TOTK_PC_TRACE") && atoi(getenv("TOTK_PC_TRACE")) != 0);
    return on;
}
const char *input_id(int k, char *b, size_t n) {
    static const struct { int vk; const char *n; } names[] = {{VK_ESCAPE, "esc"}, {VK_RETURN, "enter"}, {VK_SPACE, "space"},
        {VK_TAB, "tab"}, {VK_BACK, "backspace"}, {VK_SHIFT, "shift"}, {VK_LSHIFT, "lshift"}, {VK_RSHIFT, "rshift"},
        {VK_CONTROL, "ctrl"}, {VK_LCONTROL, "lctrl"}, {VK_RCONTROL, "rctrl"}, {VK_MENU, "alt"}, {VK_LMENU, "lalt"},
        {VK_RMENU, "ralt"}, {VK_UP, "up"}, {VK_DOWN, "down"}, {VK_LEFT, "left"}, {VK_RIGHT, "right"}, {VK_LBUTTON, "lmb"},
        {VK_RBUTTON, "rmb"}, {VK_MBUTTON, "mmb"}, {VK_XBUTTON1, "mouse4"}, {VK_XBUTTON2, "mouse5"}, {VK_PRIOR, "pageup"},
        {VK_NEXT, "pagedown"}, {VK_HOME, "home"}, {VK_END, "end"}, {VK_INSERT, "insert"}, {VK_DELETE, "delete"}};
    if (k == kWheelUp) return "wheel_up";
    if (k == kWheelDown) return "wheel_down";
    for (auto &e : names) if (e.vk == k) return e.n;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) { snprintf(b, n, "%c", (char)tolower(k)); return b; }
    if (k >= VK_F1 && k <= VK_F12) { snprintf(b, n, "f%d", k - VK_F1 + 1); return b; }
    snprintf(b, n, "vk%02x", k);
    return b;
}
std::string pad_names(uint64_t bits) {
    static const char *const n[20] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "DPAD_LEFT",
                                      "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "MOVE_LEFT", "MOVE_UP", "MOVE_RIGHT", "MOVE_DOWN"};
    std::string r;
    for (int i = 0; i < 20; i++) if (bits >> i & 1) { if (!r.empty()) r += "+"; r += n[i]; }
    return r;
}
const char *op_name(int op) {
    static const char *const n[] = {"none", "confirm", "back", "tab_prev", "tab_next", "tab_goto", "choice_pick", "scroll",
                                    "menu_goto"};
    return op >= 0 && op < (int)(sizeof n / sizeof n[0]) ? n[op] : "?";
}
void log_edge(int ctx, int k, bool down, int16_t r, bool op_done) {
    char b[16];
    if (r < 0) { printf("[pc] pc_route: %s %s %s -> dead\n", kCtxNames[ctx], input_id(k, b, sizeof b), down ? "down" : "up"); return; }
    const Row &row = kRows[r];
    const char kind = row.kind == 'H' || row.kind == 'P' ? 'I' : row.kind == 'O' || row.kind == 'F' ? 'N' : row.kind;
    std::string what;
    if (row.kind == 'O') what = std::string("op ") + op_name(row.op) + (down && !op_done && row.out ? ", pulsed " + pad_names(row.out) : "");
    else if (row.kind == 'F') what = pad_names(row.out) + ", then op " + op_name(row.op) + " " + std::to_string(row.arg);
    else if (row.kind == 'G') { char m[24]; snprintf(m, sizeof m, "vk 0x%llx", (unsigned long long)row_vk(row)); what = m; }
    else if (row.kind == 'M') what = "macro " + std::to_string(row.arg);
    else what = pad_names(row.out);
    printf("[pc] pc_route: %s %s %s -> %s %c%s%s\n", kCtxNames[ctx], input_id(k, b, sizeof b), down ? "down" : "up", row.action, kind,
           what.empty() ? "" : " ", what.c_str());
}
std::atomic<void (*)(uint64_t)> g_vbtn_writer{nullptr}, g_vbtn_tap{nullptr};
// a row's macro (arg) started for that key: its run replaced, else a free one, else the oldest; null: not registered
const PcMacroOps *macro_start(int16_t r, int input) {
    const Row &row = kRows[r];
    const PcMacroOps *m = row.arg > MAC_NONE && row.arg < kMacros ? g_macros[row.arg].load() : nullptr;
    if (!m) return nullptr;
    Running *slot = nullptr;
    for (auto &x : g_runs) if (x.id >= 0 && x.input == input) slot = &x;
    if (!slot) for (auto &x : g_runs) if (x.id < 0) { slot = &x; break; }
    if (!slot) { slot = &g_runs[0]; for (auto &x : g_runs) if (x.seq < slot->seq) slot = &x; }
    macro_stop(*slot, true);
    slot->id = row.arg; slot->input = input; slot->ctx = g_last_ctx; slot->row = r; slot->seq = ++g_run_seq;
    if (m->begin) m->begin(g_last_ctx, input);
    return m;
}
// The glyph set a press puts up, named at the press (pc_glyph_intent, 4.27), from the row: a menu hotkey from
// gameplay, the pause menu's; an ability key (L, on foot or on a wall), the ability's; a strip key (d-pad left / right /
// up, from a context drawing on foot's or the horse's set), the quickmenu's; in an ability, Tab's leave (B), on foot's;
// in an item strip, the pick or Back (A / B, the sticky strip's close), the opener's. VK rows (G) are other actions on
// those buttons (the horse's soothe, the construct's arm).
void glyph_intent_press(const Row &row, int ctx) {
    if (!in_scope(ctx, S_GAMEPLAY) || row.kind == 'G') return;
    if (row.kind == 'O' && row.op == PCOP_MENU_GOTO) pc_glyph_intent(PCX_PAUSE, 0);
    else if (ctx == PCX_ABILITY) { if (row.out & B_B) pc_glyph_intent(PCX_WORLD, 0); }
    else if (ctx == PCX_QUICKMENU) {
        if (g_qm_sub.load() != 0 && (row.out & (B_A | B_B))) pc_glyph_intent(g_last_game.load(std::memory_order_relaxed), 0);
    } else if ((row.out & B_L) && (row.scope == PCX_WORLD || row.scope == PCX_CLIMB)) pc_glyph_intent(PCX_ABILITY, 0);
    else if ((row.out & (B_DL | B_DR | B_DU)) && ctx != PCX_CAMERA && ctx != PCX_SCOPE) pc_glyph_intent(PCX_QUICKMENU, 1);
}
// 'E' rows' answerers (pc_input.h): hle_hid's map mouse, the clicks
constexpr int kExtMax = 4;
static std::atomic<bool (*)(int, int)> g_ext_fn[kExtMax];
static const char *g_ext_who[kExtMax];
static std::atomic<int> g_ext_n{0};
}  // namespace
extern "C" void pc_external_register(const char *who, bool (*answers)(int ctx, int vk)) {
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    const int i = g_ext_n.load();
    for (int j = 0; j < i; j++) if (g_ext_fn[j].load() == answers) return;
    if (!answers || i >= kExtMax) return;
    g_ext_who[i] = who;
    g_ext_fn[i].store(answers);
    g_ext_n.store(i + 1);
    printf("[pc] E keys: %s answers its own\n", who);
}
extern "C" int pc_external_check(int ctx, int vk) {
    if (ctx < 0 || ctx >= PCX_COUNT || vk <= 0 || vk >= 256) return 1;
    const int16_t r = g_t.route[ctx][vk];
    if (r < 0 || kRows[r].kind != 'E' || !kRows[r].out) return 1;   // (a wheel-only E row gives no button: not asked)
    const int n = g_ext_n.load();
    for (int i = 0; i < n; i++)
        if (auto fn = g_ext_fn[i].load())
            if (fn(ctx, vk)) return 1;
    static uint8_t said[PCX_COUNT][256 / 8];
    if (!(said[ctx][vk / 8] >> (vk % 8) & 1)) {
        said[ctx][vk / 8] |= (uint8_t)(1 << (vk % 8));
        char kb[16];
        input_name(vk, kb, sizeof kb);
        std::string who;
        for (int i = 0; i < n; i++) who += std::string(i ? ", " : "") + g_ext_who[i];
        printf("[pc] E key not answered: %s %s (row %s, %s): %s\n", kCtxNames[ctx], kb, kRows[r].action,
               pad_names(kRows[r].out).c_str(), n ? ("none of " + who + " takes it here now").c_str() : "no part registered");
    }
    return 0;
}
namespace {
bool press(int16_t r, int input, uint64_t now) {
    const Row &row = kRows[r];
    glyph_intent_press(row, g_last_ctx);
    if (row.kind == 'E') pc_external_check(g_last_ctx, input);   // (the audit's runtime half, 4.29's map click)
    static int logs;
    static uint64_t chain_until;   // a menu hotkey's chain (PCOP_MENU_GOTO) may still run: another key stops it (4.25 3c)
    bool done = false;
    if (chain_until && row.op != PCOP_MENU_GOTO) {
        if (now < chain_until) if (auto fn = g_ops.load()) fn(PCOP_MENU_GOTO, PCMG_CANCEL);
        chain_until = 0;
    }
    if (row.kind == 'O' && row.op != PCOP_NONE)
        if (auto fn = g_ops.load()) done = fn(row.op, row.arg);
    if (done && row.op == PCOP_MENU_GOTO) chain_until = now + 3000;   // (the chain's own limit)
    if (!done && (row.kind == 'O' || row.kind == 'P' || row.kind == 'F')) pulse(row.out, now);
    if (row.kind == 'F') g_follow = {row.op, row.arg, g_last_ctx, now + 1500};
    if (row.kind == 'G' && vk_live())   // (the hook's latch: a press shorter than a game frame still counts once)
        if (auto t = g_vbtn_tap.load()) t(row_vk(row));
    const PcMacroOps *m = row.kind == 'M' ? macro_start(r, input) : nullptr;
    if (!route_log() && logs++ < 200)
        printf("[pc] %s: %s%s\n", kCtxNames[g_last_ctx], row.action, done ? " (menu op)" : row.kind == 'M' && !m ? " (macro not registered)" : "");
    return done;
}
// a T row's tap, at the release (its press did nothing: the hold row would have taken over at kTapMs): its macro (arg),
// started with the key already up, and / or its out bits pulsed
void tap(int16_t r, int input, uint64_t now) {
    const Row &row = kRows[r];
    pulse(row.out, now);
    const bool mac = row.arg > MAC_NONE;
    const PcMacroOps *m = mac ? macro_start(r, input) : nullptr;
    if (route_log()) {
        char b[16];
        printf("[pc] pc_route: %s %s tap -> %s%s%s%s\n", kCtxNames[g_last_ctx], input_id(input, b, sizeof b), row.action,
               row.out ? " " : "", pad_names(row.out).c_str(),
               !mac ? "" : m ? (" macro " + std::to_string(row.arg)).c_str() : " (macro not registered)");
    }
}
bool undecided(const KeyState &s) { return s.row >= 0 && kRows[s.row].kind == 'T'; }

}  // namespace

extern "C" int pc_ctx(void) { return g_ctx.load(std::memory_order_relaxed); }
extern "C" const char *pc_ctx_name(int c) { return c >= 0 && c < PCX_COUNT ? kCtxNames[c] : "?"; }
extern "C" bool pc_ctx_is_ui(int c) { return c == PCX_TRANSITION || (c >= 0 && c < PCX_COUNT && in_scope(c, S_UI)); }
extern "C" void pc_input_gameplay_set(const char *v) {   // (controls.gameplay changed: the table rebuilt at the next read)
    g_gameplay_pc = !(v && (!_stricmp(v, "console") || !_stricmp(v, "off") || !strcmp(v, "0")));   // (as gameplay_pc)
    g_rebuild = true;
    g_bind_epoch++;
}
extern "C" bool pc_scheme_on(void) {
    int s = g_scheme.load();
    if (s < 0) { s = scheme_read() ? 1 : 0; g_scheme = s; printf("[pc] controls: %s\n", s ? "the PC scheme" : "the console layout"); }
    return s != 0;
}
extern "C" void pc_quickmenu_feed_register(bool (*fn)(double, double, int, int)) {
    g_qm_feed.store(fn);
    printf("[pc] quickmenu feed %s (else the router's pad-level one)\n", fn ? "registered: the menu cursor's, at UI level, when it takes" : "removed");
}
extern "C" void pc_menu_ops_register(bool (*fn)(int, int)) {
    g_ops.store(fn);
    printf("[pc] menu ops %s\n", fn ? "registered" : "removed");
}
// The glyph sets: menus = the pause menu's keys (every UI context), an ability, the horse, the camera and the Scope their own (deliberate
// modes lasting seconds: A / B mean other keys there), every other gameplay context the world's (no switch on air / ground / aim). A set with
// no rows of its own equals the world's, and an unchanged set changes nothing (pc_glyph_sig).
static int font_set_of(int c) {
    // (a quickmenu too: its strips draw Back [B] = Esc and Select / Sort from its own rows, not on foot's Shift / LMB)
    // (the recipe book its own: its History prompt is H there, on every other menu the pause set's Y)
    return c == PCX_RECIPE ? c : pc_ctx_is_ui(c) ? PCX_PAUSE
           : c == PCX_ABILITY || c == PCX_HORSE || c == PCX_CAMERA || c == PCX_SCOPE || c == PCX_QUICKMENU ||
             c == PCX_CONSTRUCT ? c : PCX_WORLD;   // (the construct: a mode lasting minutes whose A / B / X / Y are other keys)
}
// The HUD bar's set for a gameplay context (pc_glyph_game_set, below); g_glyph_mu held
static int game_set_of(int c) {
    // (the camera and the Scope too, 4.29: their bar drew on foot's E / Shift / Esc for their A / B / +)
    if (c != PCX_HORSE && c != PCX_GLIDE && c != PCX_SWIM && c != PCX_VEHICLE && c != PCX_ABILITY && c != PCX_AIM &&
        c != PCX_CAMERA && c != PCX_SCOPE && c != PCX_CONSTRUCT)
        return PCX_WORLD;
    for (int b = 0; b < 16; b++)
        if (g_glyph_tab[c][b] > 0 && g_glyph_tab[c][b] != g_glyph_tab[PCX_WORLD][b]) return c;
    return PCX_WORLD;
}
// The glyph intent (pc_input.h). Tab drew the ability's "Quit" in on foot's Shift for a moment, then Tab: the context
// follows the game (verb 61 some frames after the press, then the 30 ms debounce), the live rewrite follows the context
// (20-40 ms, then the font cache reset), and the HUD bar holds a set 100 ms; whatever the game laid out meanwhile drew
// the old set. The router knows the set a press puts up (an ability key, a strip key, a menu hotkey; Tab leaving an
// ability; a strip closing; the bow drawn or let go), so it names it at the press: the glyph lookups answer that set from
// then, and hle_fs rewrites at once (the kick). Held until the context's own sets move (the game followed, or did
// something else: dropped for good) or kIntentMs pass (the game refused the press: the old set again). Under g_glyph_mu.
constexpr ULONGLONG kIntentMs = 400;
static int g_gi_ctx = -1, g_gi_sub, g_gi_font_from, g_gi_game_from;
static ULONGLONG g_gi_until;
static std::atomic<void (*)(void)> g_glyph_kick{nullptr};
static int intent_locked() {
    if (g_gi_ctx < 0) return -1;
    if (GetTickCount64() >= g_gi_until || font_set_of(pc_ctx()) != g_gi_font_from ||
        game_set_of(g_last_game.load(std::memory_order_relaxed)) != g_gi_game_from)
        g_gi_ctx = -1;
    return g_gi_ctx;
}
static int glyph_set() {   // (g_glyph_mu held)
    const int i = intent_locked();
    return font_set_of(i >= 0 ? i : pc_ctx());
}
static bool qm_strip_locked() {   // (g_glyph_mu held) the quickmenu in front, or named, is an item strip
    return intent_locked() == PCX_QUICKMENU ? g_gi_sub != 0 : g_qm_sub.load() != 0;
}
extern "C" void pc_glyph_intent(int ctx, int sub) {
    if (ctx < 0 || ctx >= PCX_COUNT || !pc_scheme_on()) return;
    const int nat = pc_ctx();
    int font_to, game_to;
    {
        std::lock_guard<std::mutex> l(g_glyph_mu);
        const int font_from = font_set_of(nat), game_from = game_set_of(g_last_game.load(std::memory_order_relaxed));
        font_to = font_set_of(ctx);
        // (the bar draws gameplay contexts only; a quickmenu keeps its opener's, a menu the last gameplay one's)
        game_to = in_scope(ctx, S_GAMEPLAY) && ctx != PCX_QUICKMENU ? game_set_of(ctx) : game_from;
        const bool strip_to = font_to == PCX_QUICKMENU && sub != 0, strip_from = font_from == PCX_QUICKMENU && qm_strip_locked();
        if (font_to == font_from && game_to == game_from && strip_to == strip_from) { g_gi_ctx = -1; return; }   // (in force)
        g_gi_ctx = ctx; g_gi_sub = sub; g_gi_font_from = font_from; g_gi_game_from = game_from;
        g_gi_until = GetTickCount64() + kIntentMs;
    }
    static int logs;
    if (logs++ < 200)
        printf("[pc] glyph intent: %s (sub %d) from %s: glyphs %s, bar %s; tick %llu\n", kCtxNames[ctx], sub, kCtxNames[nat],
               kCtxNames[font_to], kCtxNames[game_to], (unsigned long long)GetTickCount64());
    if (auto k = g_glyph_kick.load()) k();
}
extern "C" bool pc_glyph_intent_active(void) {
    std::lock_guard<std::mutex> l(g_glyph_mu);
    return intent_locked() >= 0;
}
extern "C" bool pc_glyph_qm_strip(void) {
    if (!pc_scheme_on()) return false;
    std::lock_guard<std::mutex> l(g_glyph_mu);
    return glyph_set() == PCX_QUICKMENU && qm_strip_locked();
}
extern "C" void pc_glyph_kick_register(void (*fn)(void)) { g_glyph_kick.store(fn); }
// The prompts' keys against the routes. For every context, the set its prompts draw (the font glyphs, font_set_of; in
// gameplay also the HUD bar's, game_set_of) and every button that set gives a key: that key must route there to a row
// producing that button (its out bits: a P / H / O / F / M / G row), or to an 'E' row (another part reads it there:
// hle_hid's map / menus), or fall to the console layout (the context reads it). Anything else is a prompt naming a key
// that does nothing: counted and, with log, said. Table only. The other direction: the buttons the game prompts in a
// context, from its own data, mapped to the contexts the guides show in. Each must have a key that gives it there, so its
// prompt has a glyph that works. Stick moves (LS / RS / their directions, RS_Shake) are the mouse and WASD, not keys:
// left out. B_OptionX / X_OptionB are their defaults (B / X; the swap option is jump_swap's, after the router). Rows
// whose label has no text (AutoBuilderShortCut A, OneTouchBondBow ZR, GyroTilt) are left out: the game draws no prompt
// for them.
struct Prompted { int ctx; uint64_t bits; const char *from; };
static const Prompted kPrompted[] = {
    {PCX_WORLD, B_PLUS, "the default guide: + Controls"},
    {PCX_WORLD, B_Y, "FirstBattle / SpinAttack: Y"},
    {PCX_WORLD, B_X | B_B, "Jump: X Jump, B Dash"},
    {PCX_WORLD, B_R, "Grab: R Throw"},
    {PCX_WORLD, B_A | B_B | B_DD, "CarryNear / CarryFar: A Drop, B Quit, D-pad Down Far / Near"},
    {PCX_WORLD, B_DU, "MaterialThrow: D-pad Up"},
    {PCX_WORLD, B_ZL | B_ZR | B_MINUS | B_LS | B_RS, "tags: ZL lock on, ZR bow, - map, LS crouch, RS scope"},
    {PCX_WORLD, B_L, "tags: L abilities (ReverseRecorderReplay's L)"},
    {PCX_CLIMB, B_X | B_B, "Climb / ClimbTree / JumpOff: X Jump, B Let go / Climb down"},
    {PCX_SWIM, B_X, "Swim: X Dash"},
    {PCX_AIR, B_R, "Fall / FallNoParasail / FastFall*: R Dive / Accelerate"},
    {PCX_AIR, B_X, "tags: X opens the paraglider"},
    // the DoCommand prompts, whose button the code sets: the sages' A ("Let's go!") falling, "Swim up" swimming, a
    // ride's A
    {PCX_AIR, B_A, "DoCommand: a sage's A (Let's go!)"},
    {PCX_SWIM, B_A, "DoCommand 0056: A Swim up"},
    {PCX_VEHICLE, B_A | B_B | B_X, "a ride: A (Yunobo, Control), B let go / step off, X jump off"},
    {PCX_GLIDE, B_R, "Parasail: R Dive"},
    {PCX_GLIDE, B_A, "Tulin's gust: A (D2 DoCommand \"Gust!\" / \"Let's go!\", the button set by code)"},
    {PCX_GLIDE, B_B, "the HUD bar's Close: B (ExecutePlayerGlide reads VK 5)"},
    {PCX_AIM, B_DU, "SelectArrowAttachment: D-pad Up"},
    {PCX_ABILITY, B_B, "UltraHand* / ReverseRecorder / OneTouchBond / Tooreroof* / AutoBuilder*: B Cancel / Back"},
    {PCX_ABILITY, B_L, "ReverseRecorderReplay: L Stop"},
    {PCX_ABILITY, B_R, "UltraHandGrabbing* / AutoBuilder: R Rotate (hold)"},
    {PCX_ABILITY, B_DU | B_DD | B_DL | B_DR | B_ZL, "UltraHand / AutoBuilder (Rotate): D-pad, ZL Reset"},
    {PCX_HORSE, B_ZL | B_L | B_A, "Horse: ZL Reset camera, L Soothe, A Go!"},
    {PCX_CONSTRUCT, B_ZL | B_L | B_R | B_Y | B_X, "GolemNormalMode: ZL Guard, L / R hands, Y Back Part, X Attach"},
    {PCX_CONSTRUCT, B_B, "Golem*Mode / GolemPreGetOff: B Cancel"},
    {PCX_MAP, B_DU | B_DD, "tags: D-pad Up / Down map layers"},
    {PCX_MAP, B_Y, "tags: Y sensor target on the map"},
    {PCX_SCOPE, B_A, "tags: A places the Scope's pin"},
    {PCX_SCOPE, B_B, "the Scope's bar: B Quit"},
    {PCX_CAMERA, B_A | B_X | B_B, "the camera's bar: A shutter, X self-portrait, B quit"},
    {PCX_CAMERA, B_DU | B_DD | B_MINUS, "the camera's bar: D-pad Zoom, - Album (buttons by code)"},
    {PCX_QUICKMENU, B_A | B_B | B_Y, "ShortCut_00 / AutoBuilder_00: A Select, B Back, Y Sort / Set as Favorite"},
    {PCX_QUICKMENU, B_X, "ShortCut_00 Pa_Guide_02: X Drop (the strip's drop, roasting)"},
    {PCX_CUTSCENE, B_A | B_X | B_PLUS, "events: A advance, X Skip (short scenes), + Skip"},
    {PCX_HORSE, B_B | B_X, "DoCommand 0067 Dismount: B; X leap off"},
    // every menu: A confirms, B backs out (the click / the table's Esc)
    {PCX_TITLE, B_A | B_B, "menus"}, {PCX_SAVES, B_A | B_B, "menus"}, {PCX_PAUSE, B_A | B_B, "menus"},
    {PCX_POPUP, B_A | B_B, "menus"}, {PCX_PAD, B_A | B_B, "menus"}, {PCX_MAP, B_A | B_B, "menus"},
    {PCX_OPTIONS, B_A | B_B, "menus"}, {PCX_SHOP, B_A | B_B, "menus"}, {PCX_MENU, B_A | B_B, "menus"},
    {PCX_DIALOGUE, B_A, "menus"}, {PCX_CHOICE, B_A | B_B, "menus"},
    {PCX_PAUSE, B_Y | B_L | B_R | B_PLUS, "tags: Y sort, L / R tabs, + closes"},
    {PCX_PAUSE, B_X | B_RS, "the pause bar: X Hold / Check recipe, RS click Rotate"},
    {PCX_OPTIONS, B_Y, "the Options bar: Y Default settings"},
    {PCX_MAP, B_X | B_L | B_R | B_DL | B_DR, "the map bar: X Hero's Path / Dungeon Map; Hero's Path: X Play, Y Display, D-pad L / R Scroll, L / R Speed"},
    {PCX_PAD, B_Y, "the Album / Compendium viewer: Y Full Screen; Album select: Y Select All"},
    {PCX_DIALOGUE, B_Y, "the conversation's Log: Y"},
    {PCX_SHOP, B_A | B_B | B_Y | B_L | B_R, "ShopMenu_00: A Select, B Back, Y Sort, L / R the category tabs"},
    {PCX_LOADING, B_A, "LoadingFade_00: A Next tip"},
    {PCX_RECIPE, B_B | B_Y, "RecipeCard_00: B closes the book, from the history back to the cards; Y the history (FoodRecipeGuide 'View history', Food tab)"},
};
static uint64_t prompted_in(int c, uint64_t bit = ~0ull, std::string *from = nullptr) {   // (from: the guides of bit)
    uint64_t bits = 0;
    for (const Prompted &p : kPrompted)
        if (p.ctx == c && (p.bits & bit)) {
            bits |= p.bits;
            if (from) *from += std::string(from->empty() ? "" : "; ") + p.from;
        }
    return bits;
}
extern "C" int pc_glyph_audit(int log) {
    // a row's buttons (a T row: its hold row's too, as build_ctx's glyphs count them)
    auto outs = [](int16_t r) -> uint64_t {
        if (r < 0) return 0;
        const int16_t h = kRows[r].kind == 'T' ? hold_of(r) : -1;
        return kRows[r].out | (h >= 0 ? kRows[h].out : 0);
    };
    int bad = 0;
    for (int c = 0; c < PCX_COUNT; c++) {
        if (c == PCX_OVERLAY || c == PCX_TRANSITION) continue;   // (the overlay has its keys; a transition, every key dead)
        uint64_t used = 0;   // the buttons some key gives in this context: the ones its prompts can name
        for (int k = 1; k < kInputs; k++) used |= outs(g_t.route[c][k]);
        // the buttons the game prompts here that no key gives (its prompt would name a key that does nothing: an 'E' row
        // counts as given, another part answers it)
        if (const uint64_t miss = prompted_in(c) & ~used) {
            for (int b = 0; b < 16; b++)
                if (miss >> b & 1) {
                    bad++;
                    if (log) {
                        std::string from;
                        prompted_in(c, 1ull << b, &from);
                        printf("[pc] glyph audit: %s: the game prompts %s there (%s), but no key gives it\n", kCtxNames[c],
                               pad_names(1ull << b).c_str(), from.c_str());
                    }
                }
        }
        // the set its prompts draw: the font glyphs' (font_set_of); a gameplay mode the HUD bar keys on its own (GLIDE's
        // Close, AIM's cancel: game_set_of) while its font set stays on foot's (no rewrite at every jump / draw) has its
        // prompts in the bar: that set (both: the font prompts draw from font_set_of's set, on foot's in AIR / GLIDE /
        // SWIM / CLIMB / VEHICLE, and the HUD bar from game_set_of's; each must name keys that work here)
        const int font = font_set_of(c);
        int bar = -1;
        if (in_scope(c, S_GAMEPLAY) && c != PCX_QUICKMENU && font == PCX_WORLD) {
            std::lock_guard<std::mutex> l(g_glyph_mu);
            bar = game_set_of(c);
        }
        int sets[2] = {font, bar != font ? bar : -1};
        const uint64_t prompted = prompted_in(c);
        for (int set : sets) {
            if (set < 0) continue;
            for (int b = 0; b < 16; b++) {
                const int k = g_t.glyph[set][b];
                // the set draws no PC key (-1: the pad's own button) for a button a key gives here or the game prompts
                // here: a pad glyph in a PC prompt. 0 is the console layout's key. Every context, menus too: the menus'
                // set has M for − (PAUSE's map row) and the arrows for the D-pad
                if (k < 0 && ((used | prompted) >> b & 1)) {
                    bad++;
                    if (log)
                        printf("[pc] glyph audit: %s draws %s as the pad's own button (the %s set has no key for it), "
                               "though %s\n", kCtxNames[c], pad_names(1ull << b).c_str(), kCtxNames[set],
                               used >> b & 1 ? "a key gives it there" : "the game prompts it there");
                    continue;
                }
                if (!(used >> b & 1) || k <= 0) continue;   // (a button no key gives here; the layout's own glyph)
                const int16_t r = g_t.route[c][k];
                if ((outs(r) >> b & 1) || (r >= 0 && kRows[r].kind == 'E')) continue;
                // (a key another part answers there outside the table, each with its reason)
                if (c == PCX_DIALOGUE && b == 0 && k == VK_LBUTTON) continue;
                // (the aim's D-pad Right is the bow strip on 3, which the HUD bar draws from the aim's own set; no font
                // prompt shows it while the bow is drawn, so on foot's 1 there is never named)
                if (c == PCX_AIM && b == 14 && set == PCX_WORLD) continue;
                // (nor a + prompt: the default guide's "+ Controls" is on foot's idle bar; + draws Esc, events' skip)
                if (c == PCX_AIM && b == 10 && set == PCX_WORLD) continue;
                bad++;
                if (log) {
                    char kb[16];
                    input_name(k, kb, sizeof kb);
                    const std::string what = r >= 0 ? std::string(kRows[r].action) + " (" + pad_names(outs(r)) + ")" : "nothing";
                    printf("[pc] glyph audit: %s draws %s as %s (the %s set), but %s there is %s\n", kCtxNames[c],
                           pad_names(1ull << b).c_str(), kb, kCtxNames[set], kb, what.c_str());
                }
            }
        }
    }
    return bad;
}
extern "C" int pc_glyph_vk(int bit) {
    if (bit < 0 || bit >= 24 || !pc_scheme_on()) return 0;
    std::lock_guard<std::mutex> l(g_glyph_mu);
    return g_glyph_tab[glyph_set()][bit];
}
extern "C" unsigned pc_glyph_sig(void) {
    if (!pc_scheme_on()) return 0;
    std::lock_guard<std::mutex> l(g_glyph_mu);
    const int set = glyph_set();
    unsigned h = 2166136261u;
    for (int v : g_glyph_tab[set]) h = (h ^ (unsigned)v) * 16777619u;
    if (set == PCX_QUICKMENU && qm_strip_locked()) h ^= 0x5157u;   // (a strip: its Select draws the wheel, hle_hid)
    return h;
}
// the quickmenu in front: 0 the ability wheel, 1-4 the item strips, 5 the Autobuild list (the sub)
extern "C" int pc_quickmenu_sub(void) { return g_qm_sub.load(); }
extern "C" bool pc_quickmenu_closing(void) { return g_qm_closing.load(std::memory_order_relaxed); }
static_assert(PCX_WORLD == 0, "hle_fs.cpp's pc_glyph_gameplay_cp names the gameplay set 0");
// The same for a named set, whatever the context (4.15: the System tab's controls diagram shows the gameplay set,
// PCX_WORLD's, inside the pause menu)
extern "C" int pc_glyph_vk_in(int set, int bit) {
    if (bit < 0 || bit >= 24 || set < 0 || set >= PCX_COUNT || !pc_scheme_on()) return 0;
    std::lock_guard<std::mutex> l(g_glyph_mu);
    return g_glyph_tab[set][bit];
}
// The HUD bar's set: the riding / gliding / swimming / vehicle / ability / bow-aim mode in force, else on foot's (a
// jump's AIR, CLIMB), and a mode whose keys are on foot's own is on foot's set (no rewrite for nothing: the records
// key on the set). ("its own keys": every button the mode keys (a nonzero entry) keyed as on foot; a button it has
// no row for draws no prompt there, so SWIM / VEHICLE, which only lack rows, stay on foot's)
extern "C" int pc_glyph_game_set(void) {
    std::lock_guard<std::mutex> l(g_glyph_mu);
    const int i = intent_locked();   // (a gameplay context named by a press: its set at once; else the context in force)
    return game_set_of(i >= 0 && in_scope(i, S_GAMEPLAY) && i != PCX_QUICKMENU ? i : g_last_game.load(std::memory_order_relaxed));
}
extern "C" unsigned pc_glyph_sig_in(int set) {
    if (set < 0 || set >= PCX_COUNT || !pc_scheme_on()) return 0;
    std::lock_guard<std::mutex> l(g_glyph_mu);
    unsigned h = 2166136261u;
    for (int v : g_glyph_tab[set]) h = (h ^ (unsigned)v) * 16777619u;
    return h;
}

// 4.11 P6: the Sticky / Toggle / Filter Keys hotkeys (Shift five times, Num Lock held, right Shift held 8 s: a sprint
// key held or tapped opens their dialogs over the game) off while the window is in front, only where the feature itself
// is off, without SPIF_UPDATEINIFILE (the session's setting, not the profile's); put back as they were at a focus loss,
// at exit and before a crash report.
namespace {
std::mutex g_acc_mu;
bool g_acc_saved;
STICKYKEYS g_sk = {sizeof(STICKYKEYS)};
TOGGLEKEYS g_tk = {sizeof(TOGGLEKEYS)};
FILTERKEYS g_fk = {sizeof(FILTERKEYS)};
}  // namespace
extern "C" void pc_input_keys_restore(void) {
    std::unique_lock<std::mutex> l(g_acc_mu, std::try_to_lock);   // (a crash with the lock held: restore anyway)
    if (!g_acc_saved) return;
    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof g_sk, &g_sk, 0);
    SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof g_tk, &g_tk, 0);
    SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof g_fk, &g_fk, 0);
    g_acc_saved = false;
}
static void keys_restore_at_exit() { pc_input_keys_restore(); }
extern "C" void pc_input_focus(bool focused) {
    if (!focused) { pc_input_keys_restore(); return; }
    // (the PC scheme's rule; not in test boots driven by TOTK_CTL / TOTK_PRESS: they are killed from outside, where
    // nothing puts the hotkeys back)
    static const bool tested = (getenv("TOTK_CTL") && *getenv("TOTK_CTL")) || (getenv("TOTK_PRESS") && *getenv("TOTK_PRESS"));
    if (!pc_scheme_on() || tested) return;
    std::lock_guard<std::mutex> l(g_acc_mu);
    if (g_acc_saved) return;
    if (!SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof g_sk, &g_sk, 0) || !SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof g_tk, &g_tk, 0) ||
        !SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof g_fk, &g_fk, 0))
        return;
    static const bool at_exit = (atexit(keys_restore_at_exit), true);
    (void)at_exit;
    g_acc_saved = true;
    STICKYKEYS sk = g_sk;
    TOGGLEKEYS tk = g_tk;
    FILTERKEYS fk = g_fk;
    int off = 0;
    if (!(sk.dwFlags & SKF_STICKYKEYSON) && (sk.dwFlags & SKF_HOTKEYACTIVE)) {
        sk.dwFlags &= ~SKF_HOTKEYACTIVE; off++; SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof sk, &sk, 0);
    }
    if (!(tk.dwFlags & TKF_TOGGLEKEYSON) && (tk.dwFlags & TKF_HOTKEYACTIVE)) {
        tk.dwFlags &= ~TKF_HOTKEYACTIVE; off++; SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof tk, &tk, 0);
    }
    if (!(fk.dwFlags & FKF_FILTERKEYSON) && (fk.dwFlags & FKF_HOTKEYACTIVE)) {
        fk.dwFlags &= ~FKF_HOTKEYACTIVE; off++; SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof fk, &fk, 0);
    }
    static int logs;
    if (logs++ < 10) printf("[pc] in front: %d accessibility hotkeys off (Sticky / Toggle / Filter Keys)\n", off);
}

bool pc_ctx_force(const char *name) {
    if (!strcmp(name, "auto")) { g_forced = -1; return true; }
    if (!strcmp(name, "quickmenu") || !strcmp(name, "quickstrip")) {   // (the wheel, or a strip)
        g_qm_sub = name[5] == 's' ? 1 : 0;   // (quickstrip: a weapon strip)
        g_forced = PCX_QUICKMENU;
        return true;
    }
    for (int c = 0; c < PCX_COUNT; c++) if (!strcmp(name, kCtxNames[c])) { g_forced = c; return true; }
    return false;
}

static std::atomic<uint32_t> g_pushed{0};
static std::atomic<int> g_ui_sub{-1};   // (pc_ui_sub: the decided screen's sub)
extern "C" int pc_ui_sub(void) { return g_ui_sub.load(std::memory_order_relaxed); }
static std::atomic<ULONGLONG> g_pushed_at{0};
extern "C" void pc_ctx_ui(uint32_t screen, uint32_t sub) {
    g_pushed.store((screen & 0xff) | (sub & 0xff) << 8);
    g_pushed_at.store(GetTickCount64());
}

// the player's state (stage 2): the push, or TOTK_CTL "player" (held until "player off")
static std::atomic<uint32_t> g_pl_flags{0};
static std::atomic<uint8_t> g_pl_ability{0};
static std::atomic<ULONGLONG> g_pl_at{0};
static std::atomic<bool> g_pl_fake{false};
static std::atomic<int (*)(uint32_t, uint8_t)> g_pl_map{nullptr};
extern "C" void pc_ctx_player(uint32_t flags, uint8_t ability) {
    if (g_pl_fake.load()) return;
    g_pl_flags.store(flags); g_pl_ability.store(ability); g_pl_at.store(GetTickCount64());
}
void pc_ctx_player_fake(bool on, uint32_t flags, uint8_t ability) {
    g_pl_fake.store(on);
    g_pl_flags.store(on ? flags : 0); g_pl_ability.store(on ? ability : 0); g_pl_at.store(on ? GetTickCount64() : 0);
}
static bool player_fresh() { return g_pl_fake.load() || GetTickCount64() - g_pl_at.load() < 250; }
extern "C" uint32_t pc_ctx_player_flags(void) { return player_fresh() ? g_pl_flags.load() : 0; }
extern "C" uint8_t pc_ctx_ability(void) { return player_fresh() ? g_pl_ability.load() : (uint8_t)PCA_NONE; }
extern "C" void pc_ctx_player_map_register(int (*fn)(uint32_t, uint8_t)) { g_pl_map.store(fn); }
extern "C" void pc_vbtn_writer_register(void (*hold)(uint64_t), void (*tap)(uint64_t)) {
    g_vbtn_writer.store(hold);
    g_vbtn_tap.store(tap);
}
void pc_macro_register(int id, const PcMacroOps *ops) {
    if (id >= 0 && id < kMacros) g_macros[id].store(ops);
}

// The player is in an event (a cutscene, or a talk without a focus box): EventMgr->mpMainEvent (+0x58) set, the test
// ExecutePlayerCalcKey cuts the player's input with. main+0x4557b30 is EventMgr*'s GOT slot: the variable's address,
// then EventMgr (totk_globals.h; one load, as before, read +0x58 of the variable's neighbours, set in the world).
// Aligned loads under SEH.
static bool in_event_try() {
    __try {
        auto ok = [](uint64_t p) { return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0; };
        const uint64_t var = *(const volatile uint64_t *)totk_main(0x4557b30);
        if (!ok(var)) return false;
        const uint64_t mgr = *(const volatile uint64_t *)var;
        return ok(mgr) && *(const volatile uint64_t *)(mgr + 0x58) != 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
static bool in_event() {   // (rt_main's on_crash is vectored: without the guard a fault here ends the process, pc.h)
    pc_fault_guard(1);
    const bool r = in_event_try();
    pc_fault_guard(0);
    return r;
}

// The game's own input cuts: the pause manager's reasons, read here with the totk_input_reason, under the fault
// guard as in_event's. Any -1 (the table unread, a hash not in it) is unknown: the 250 ms fallback, never "clear".
static int reason_try(uint32_t hash) {
    __try {
        return totk_input_reason(hash);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}
static int reason(uint32_t hash) {
    // TOTK_PC_REASONS=0: no read at all (every answer unknown: the 250 ms settle), so a build bisects this reader by a
    // toggle
    static const bool off = [] { const char *e = getenv("TOTK_PC_REASONS"); return e && *e == '0'; }();
    if (off) return -1;
    pc_fault_guard(1);
    const int r = reason_try(hash);
    pc_fault_guard(0);
    return r;
}
// 1 a game menu or the load's cut is up, 0 both known clear, -1 unknown
static int menu_cut() {
    const int a = reason(0x262e9f13u), b = reason(0x7f771e39u), l = reason(0xf6e0a4c8u);
    if (a == 1 || b == 1 || l == 1) return 1;
    return a == 0 && b == 0 && l == 0 ? 0 : -1;
}
static bool cut_mapped(int ctx) {
    return ctx == PCX_PAUSE || ctx == PCX_POPUP || ctx == PCX_PAD || ctx == PCX_PADLOG || ctx == PCX_MAP ||
           ctx == PCX_OPTIONS || ctx == PCX_SAVES;
}
int pc_ctx_decide(bool overlay, bool title_phase, uint32_t menu_screen, int guess, int quick_hint) {
    int c = PCX_WORLD;
    const int f = g_forced.load();
    if (GetTickCount64() - g_pushed_at.load() < 250) menu_screen = g_pushed.load();  // (the push, when fresh)
    const TotkMenuScreen ms = TOTK_MENU_SCREEN(menu_screen);
    g_ui_sub.store(ms == TMS_NONE ? -1 : (int)TOTK_MENU_SUB(menu_screen), std::memory_order_relaxed);
    if (f >= 0) c = f;
    else if (overlay) c = PCX_OVERLAY;
    // (the focus gap inside a menu: TMS_TRANSITION = 12, 948bdb8) (inside an event the gap stays
    // the event: its text can still be advanced, D1)
    else if ((menu_screen & 0x10000u) || (int)ms == 12) c = !title_phase && in_event() ? PCX_CUTSCENE : PCX_TRANSITION;
    else if (ms == TMS_TITLE || (title_phase && ms == TMS_NONE)) c = PCX_TITLE;
    else switch (ms) {
        case TMS_SAVE_LIST: c = PCX_SAVES; break;
        case TMS_PAUSE: c = PCX_PAUSE; break;
        case TMS_ITEM_POPUP: c = PCX_POPUP; break;
        case TMS_PURAH_PAD: c = TOTK_MENU_SUB(menu_screen) == 1 ? PCX_PADLOG : PCX_PAD; break;   // (sub: the tab bar's page)
#ifdef TOTK_TMS_SCOPE
        // (the TMS_SCOPE names the Scope from its screens, so the map page is the map, the real map opened from inside
        // the Scope as the Scope, a Scope closing as the map)
        case TMS_MAP: c = PCX_MAP; break;
#else
        // (the Scope opens AppMap_00 too, page 2 like the map's: the player's push tells them apart, doc 4.18)
        case TMS_MAP: c = player_fresh() && (g_pl_flags.load() & PCP_SCOPE) ? PCX_SCOPE : PCX_MAP; break;
#endif
        case TMS_DIALOGUE: c = PCX_DIALOGUE; break;
        case TMS_CHOICE: c = PCX_CHOICE; break;
        case TMS_SHOP: c = PCX_SHOP; break;
        case TMS_OPTIONS: c = PCX_OPTIONS; break;
        // (quick_hint, the interim "OTHER while L / a strip's d-pad is held = a quickmenu", is retired; an ability held
        // with an unrelated screen up must not read as the wheel)
        case TMS_OTHER: (void)quick_hint; c = guess == 1 ? PCX_PAD : PCX_MENU; break;
#ifdef TOTK_TMS_CAMERA
        case (TotkMenuScreen)TOTK_TMS_CAMERA: c = PCX_CAMERA; break;
#endif
#ifdef TOTK_TMS_SCOPE  // (4.18's fallback (b): the camera over the map page with no AppMenuBG_00)
        case (TotkMenuScreen)TOTK_TMS_SCOPE: c = PCX_SCOPE; break;
#endif
#ifdef TOTK_TMS_RECIPE
        case (TotkMenuScreen)TOTK_TMS_RECIPE: c = PCX_RECIPE; break;
#endif
#ifdef TOTK_TMS_QUICKMENU
        case (TotkMenuScreen)TOTK_TMS_QUICKMENU: c = PCX_QUICKMENU; g_qm_sub = TOTK_MENU_SUB(menu_screen); break;
#endif
        default:  // none: hle_hid's screen guess (1 the Purah Pad
                   // else an event (below every UI context: choices, menus win), else gameplay
            c = guess == 1 ? PCX_PAD : guess == 2 ? PCX_PAUSE : !title_phase && in_event() ? PCX_CUTSCENE : PCX_WORLD;
    }
    // the Scope over the guess: it opens AppMap_00, which screen_track reads as the Purah Pad, a UI context: the pointer
    // shown mid-screen and the mouse off the camera. The player's push is the better source there; a named screen still
    // wins.
    if (f < 0 && guess == 1 && c == PCX_PAD && (ms == TMS_NONE || ms == TMS_OTHER) && player_fresh() &&
        (g_pl_flags.load() & PCP_SCOPE))
        c = PCX_SCOPE;
    g_title_press.store(c == PCX_TITLE && ms == TMS_NONE);   // (Title_00's "press a button": no focus box yet)
    const ULONGLONG now = GetTickCount64();
    const int prev = g_ctx.load();
    // the player's context when no UI one is up (on foot without a push or a mapper)
    if (f < 0 && c == PCX_WORLD && player_fresh()) {
        // (one source for the event: pc_ctx's own EventMgr read above; a pushed PCP_EVENT is ignored, TOTK_CTL "player
        // event" still forces it for tests: the decision)
        const uint32_t fl = g_pl_fake.load() ? g_pl_flags.load() : g_pl_flags.load() & ~(uint32_t)PCP_EVENT;
        int p = fl & PCP_LOADING ? PCX_LOADING : fl & PCP_EVENT ? PCX_CUTSCENE : PCX_WORLD;
        if (auto map = g_pl_map.load()) p = map(fl, g_pl_ability.load());
        if ((fl & PCP_SCOPE) && p != PCX_LOADING && p != PCX_CUTSCENE) p = PCX_SCOPE;   // (a frame without its map screen)
        if (p >= 0 && p < PCX_COUNT && !pc_ctx_is_ui(p) && p != PCX_OVERLAY) c = p;
    }
    // a change between gameplay contexts once the same answer has held 30 ms (the state flags flicker at transitions)
    static int cand = -1;
    static ULONGLONG cand_since;
    if (f < 0 && !pc_ctx_is_ui(prev) && prev != PCX_OVERLAY && !pc_ctx_is_ui(c) && c != PCX_OVERLAY && c != prev) {
        if (c != cand) { cand = c; cand_since = now; }
        if (now - cand_since < 30) c = prev;
    } else cand = -1;
    // 4.11 P3: the menu class is left only when the focus lookup (no screen) and screen_track's scene signal (guess 0)
    // both say gameplay for 250 ms; until then "transition", every key dead (a focus gap inside a menu)
    static ULONGLONG game_since;
    static int from_ui = -1;   // the UI screen the way back to gameplay comes from (not the transition itself)
    if (pc_ctx_is_ui(prev) && prev != PCX_TRANSITION) from_ui = prev;
    const bool to_game = !pc_ctx_is_ui(c) && c != PCX_OVERLAY;
    static ULONGLONG cut_since;   // the game's cut has read lifted since (0: not now)
    if (f < 0 && to_game && pc_ctx_is_ui(prev)) {
        // 4.21: the game's own menu cut lifted ends it (screens probe4 mapped, the focus lookup empty, no event) once
        // that has held 40 ms, over a game frame: a hop between two menus whose focus gap reads as gameplay for a moment
        // must not drop to the world if the reasons blink there; else the 250 ms settle on screen_track's guess, as
        // before (no menu screen in front: none, or a gameplay-class one, the Scope / the camera / a quickmenu, as when
        // the map opened from inside the Scope closes back to it)
        const bool menu_free = ms == TMS_NONE
#ifdef TOTK_TMS_SCOPE
                               || (int)ms == TOTK_TMS_SCOPE
#endif
#ifdef TOTK_TMS_CAMERA
                               || (int)ms == TOTK_TMS_CAMERA
#endif
#ifdef TOTK_TMS_QUICKMENU
                               || (int)ms == TOTK_TMS_QUICKMENU
#endif
            ;
        const bool lifted = cut_mapped(from_ui) && menu_free && !(menu_screen & 0x10000u) && menu_cut() == 0 && !in_event();
        if (!lifted) cut_since = 0;
        else if (!cut_since) cut_since = now;
        const bool cut_over = cut_since && now - cut_since >= 40;
        if (cut_over) game_since = 0;
        else {
            if (guess != 0) game_since = 0;
            else if (!game_since) game_since = now;
            if (!game_since || now - game_since < 250) c = PCX_TRANSITION;
        }
        static int cut_logs;
        if (cut_over && c != PCX_TRANSITION && cut_logs++ < 100)
            printf("[pc] pc_ctx: %s's menu cut lifted (4.21): %s after %llu ms\n", kCtxNames[from_ui], kCtxNames[c],
                   (unsigned long long)(now - cut_since));
    } else {
        cut_since = 0;
        if (!to_game) game_since = 0;
    }
    // the load's input cut (4.21): gameplay reads as loading while it is on, every key dead (unknown: as before)
    if (f < 0 && to_game && c != PCX_LOADING && c != PCX_CUTSCENE && !title_phase && reason(0xf6e0a4c8u) == 1)
        c = PCX_LOADING;
    const int was = g_ctx.exchange(c);
    if (in_scope(c, S_GAMEPLAY) && c != PCX_QUICKMENU) g_last_game.store(c, std::memory_order_relaxed);   // (pc_glyph_game_set)
    // Mineru's construct has no get-off prompt (no Golem guide row: the Switch shows none either): a PC line
    // bottom-left while riding it, "<key>: get off" with the get_off key as bound, refreshed every 100 ms for 300 ms so
    // it goes 0.3 s after the ride ends. TOTK_PC_HINTS=0: none
    if (c == PCX_CONSTRUCT && pc_scheme_on() && gameplay_pc()) {
        static const bool hints = !(getenv("TOTK_PC_HINTS") && !strcmp(getenv("TOTK_PC_HINTS"), "0"));
        static ULONGLONG hint_at;
        if (hints && now - hint_at >= 100) {
            hint_at = now;
            int k[4];
            const int n = pc_bind_get(PCX_CONSTRUCT, "get_off", k, 4, nullptr);
            char text[96];
            snprintf(text, sizeof text, pc_tr("%s: get off"), pc_key_name(n > 0 ? k[0] : VK_SPACE, pc_lang_es() ? "es" : "en"));
            if (!osd_toast_busy(text)) osd_toast(text, 300);   // (never over another notice, F12's)
        }
    }
    static int logs;
    if (was != c && logs++ < 400)
        printf("[pc] pc_ctx: %s -> %s (%s) screen %u sub %u tick %llu\n", kCtxNames[was], kCtxNames[c],
               f >= 0 ? "forced" : c == PCX_CUTSCENE && !player_fresh() ? "event" : ms != TMS_NONE || (menu_screen & 0x10000u) ? "menu"
               : guess ? "guess" : player_fresh() ? "player" : "scene",
               (unsigned)ms, TOTK_MENU_SUB(menu_screen), (unsigned long long)now);
    return c;
}

void pc_route(const PcRouteIn &in, PcRouteOut *out) {
    out->bits = 0;
    out->wheel_taken = false;
    out->stick[0] = out->stick[1] = false;
    out->vbtn = 0;
    memset(out->claimed, 0, sizeof out->claimed);
    static uint64_t hkl_at;   // (the keyboard layout, looked at once a second: a switch rebuilds the positions' keys)
    if (!g_hkl || in.now_ms - hkl_at >= 1000) {
        hkl_at = in.now_ms;
        const HKL l = layout_now();
        if (l != g_hkl) { g_hkl = l; g_t.gen = -1; }
    }
    if (g_t.gen != in.gen || g_rebuild.exchange(false)) build(in.gen);
    const int c = in.ctx;
    const bool ui = pc_ctx_is_ui(c) || c == PCX_OVERLAY || !g_t.layout[c];   // (every key the table's, or dead)
    {   // a strip key's tap keeps its strip open (g_stk, above): the physical edges, before the routing reads the keys
        static bool pp[256], cand[256];
        static uint64_t at[256];
        auto close = [&](const char *why, int eat) {
            if (eat) g_stk_eaten[eat] = true;
            static int logs;
            char nb[16];
            if (logs++ < 200) printf("[pc] strip: %s let go (%s)\n", input_id(g_stk.key, nb, sizeof nb), why);
            g_stk = Sticky();
        };
        for (int k = 1; k < 256; k++) {
            const bool d = in.down(k, in.user), eaten = g_stk_eaten[k];
            if (eaten && !d) g_stk_eaten[k] = false;   // (a close's release: no tap of its own)
            if (d && !pp[k]) {
                at[k] = in.now_ms;
                cand[k] = strip_key(c, k);
                if (g_stk.key && (k == g_stk.key || k == VK_LBUTTON || k == VK_ESCAPE))
                    close(k == g_stk.key ? "pressed again" : k == VK_LBUTTON ? "a click" : "Esc", k);
                else if (g_stk.key && cand[k]) close("another strip key", 0);
            } else if (!d && pp[k] && cand[k] && !g_stk.key && !eaten && in.now_ms - at[k] < kTapMs &&
                       !pc_ctx_is_ui(c)) {
                g_stk = {k, in.now_ms, false};
                static int logs;
                char nb[16];
                if (logs++ < 200) printf("[pc] strip: %s tapped: held on until pressed again, a click or Esc\n", input_id(k, nb, sizeof nb));
            }
            pp[k] = d;
        }
        if (g_stk.key) {
            if (c == PCX_QUICKMENU) g_stk.seen = true;
            else if (g_stk.seen) close("the strip has closed", 0);
            else if (c != PCX_TRANSITION && in.now_ms - g_stk.at > 1000) close("no strip in 1 s", 0);
        }
    }
    auto is_down = [&](int k) { return k > 0 && k < 256 && ((in.down(k, in.user) && !g_stk_eaten[k]) || k == g_stk.key); };
    // the presses queued in a transition (or a quickmenu's close), played in the settled context c
    auto replay_queued = [&]() {
        for (int i = 0; i < g_queued_n; i++) {
            const int k = g_queued[i].k;
            if (g_mclaim[k]) continue;   // (a macro's: never its row)
            const int16_t r = g_t.route[c][k];
            if (!is_down(k)) {
                if (in.now_ms - g_queued[i].at > 1000) continue;
                uint64_t bits = 0;
                if (r >= 0) {
                    const int prev = g_last_ctx;
                    g_last_ctx = c;
                    if (kRows[r].kind == 'T') tap(r, k, in.now_ms);   // (pressed and let go: a tap)
                    else g_op_done[k] = press(r, k, in.now_ms);   // (P / O / F / G / M: as a press)
                    g_last_ctx = prev;
                    if (kRows[r].kind == 'H' || (kRows[r].kind == 'G' && !vk_live())) bits = kRows[r].out;
                    if (route_log()) log_edge(c, k, true, r, g_op_done[k]);
                } else if (!ui && in.layout_bits) bits = in.layout_bits(k, in.user);
                pulse(bits, in.now_ms);
                if (route_log()) {
                    char b[16];
                    printf("[pc] pc_route: %s %s tap from the transition -> replayed%s\n", kCtxNames[c], input_id(k, b, sizeof b),
                           r < 0 ? (bits ? " (layout)" : ": nothing here") : "");
                }
                continue;
            }
            KeyState &s = g_keys[k];
            if (r < 0) {
                if (!ui) s = KeyState();   // (the layout's key now: it reads it from here on)
                continue;
            }
            const int prev = g_last_ctx;
            g_last_ctx = c;
            g_op_done[k] = press(r, k, in.now_ms);
            g_last_ctx = prev;
            if (route_log()) log_edge(c, k, true, r, g_op_done[k]);
            s.down = true; s.dead = false; s.row = r;
            if (kRows[r].kind == 'T') { s.trow = r; s.at = in.now_ms; }   // (its tap / hold clock from now)
        }
        g_queued_n = 0;
    };
    if (c != g_last_ctx) {   // pulses not started are dropped; held keys keep their hold only for the same action, else dead
        // (a started pulse runs its 2 frames: the press that closed a popup changes the context the next frame, and
        // cutting its pulse there left the game one sample of it; menus act on the edge, so the rest does nothing more)
        for (size_t i = 0; i < g_pulses.size();)
            if (!g_pulses[i].started) g_pulses.erase(g_pulses.begin() + (long)i);
            else i++;
        for (auto &run : g_runs)   // (each run carries over only where its key keeps the same action)
            if (run.id >= 0) {
                int16_t now_row = g_t.route[c][run.input];
                if (now_row >= 0 && kRows[now_row].kind == 'T') now_row = hold_of(now_row);   // (a T pair's: the hold row)
                if (same_action(run.row, now_row)) { run.row = now_row; run.ctx = c; }
                else macro_stop(run, true);
            }
        for (int k = 1; k < 256; k++) {
            KeyState &s = g_keys[k];
            const bool d = is_down(k);
            if (!d) { s = KeyState(); continue; }
            const int16_t now_row = g_t.route[c][k];
            if (!s.down) {   // (held before, not routed: a layout key; between two contexts that both fall back to the
                             // layout and do not route it either, it keeps its meaning: L held into the ability wheel)
                // (not down at the last read: pressed on this very read, a fresh press of the new context, routed below
                // LMB on the tick a strip's quickmenu gave back the world was killed here)
                if (!g_phys_prev[k]) continue;
                if (g_last_ctx >= 0 && g_t.layout[g_last_ctx] && g_t.layout[c] && now_row < 0) continue;
                s.down = true; s.row = -1; s.dead = true;
                continue;
            }
            // (a key pressed on a pulse row, P / O / F, carries into nothing: V closing the Scope is SCOPE's 'P' "scope" and
            // WORLD's 'G' "scope", so its hold reopened it ~60 ms later, item 8. Held rows carry.)
            const char was = s.row >= 0 ? kRows[s.row].kind : 0;
            const bool t_now = now_row >= 0 && kRows[now_row].kind == 'T';
            if (was == 'T') {   // (a tap still undecided goes on only onto the same action's T row, its clock kept)
                if (!s.dead && t_now && same_action(s.row, now_row)) s.row = s.trow = now_row;
                else s.dead = true;
                continue;
            }
            const int16_t eff = t_now ? hold_of(now_row) : now_row;   // (a held key onto a T pair: its hold row)
            if (s.dead || eff < 0 || !same_action(s.row, eff) || (kRows[eff].kind != 'H' && kRows[eff].kind != 'G') ||
                was == 'P' || was == 'O' || was == 'F')
                s.dead = true;
            else { s.row = eff; s.trow = t_now ? now_row : -1; }
        }
        // Space tapped while a menu closes jumps once), else dropped. (the same for a quickmenu's close: the presses
        // queued after its opening key let go, played once the strip's macro run is over, below: its release tail,
        // MAC_BOW_STRIP's B + ZR lowering, would cancel a draw started now)
        if (g_last_ctx == PCX_QUICKMENU && g_qm_closing) { g_qm_replay = true; g_qm_replay_at = in.now_ms; }
        else if (g_last_ctx == PCX_TRANSITION && c != PCX_TRANSITION && !g_qm_replay) replay_queued();
        if (c == PCX_QUICKMENU) {   // (the keys holding it open: the strip / wheel key, a row on a d-pad button or L;
            g_qm_has = false;       // W held while picking is not one. None found: no closing rule)
            for (int k = 1; k < 256; k++) {
                const int16_t r = g_keys[k].row;   // (its row before this change: kept on a dead key)
                g_qm_holders[k] = is_down(k) && r >= 0 && (kRows[r].out & (B_DL | B_DR | B_DU | B_DD | B_L));
                g_qm_has |= g_qm_holders[k];
            }
        }
        g_last_ctx = c;
    }
    // A quickmenu whose opening keys are all let go is closing: the game equips / picks on that release and leaves it a
    // few frames later, so a key pressed in between (LMB 0.1 s after picking a bow, and the world never saw the press)
    // is queued as in a transition: still held when the world settles, it counts from that moment; pressed and let go,
    // it is played once there
    if (c != PCX_QUICKMENU) g_qm_closing = false;
    else if (!g_qm_closing && g_qm_has) {
        bool held = false;
        for (int k = 1; k < 256 && !held; k++) held = g_qm_holders[k] && is_down(k);
        g_qm_closing = !held;
        // (an item strip closing: the opener's glyphs from now, not after the game has left it, 4.27)
        if (g_qm_closing && g_qm_sub.load() != 0) pc_glyph_intent(g_last_game.load(std::memory_order_relaxed), 0);
    }
    for (int k = 1; k < 256; k++) g_phys_prev[k] = is_down(k);   // (the context change's "pressed this read" test, next read)
    if (g_qm_replay && c != PCX_TRANSITION && c != PCX_QUICKMENU) {   // (a closed quickmenu's presses, once its key's run ends)
        bool strip_run = false;
        for (auto &r : g_runs) strip_run |= r.id >= 0 && r.input > 0 && r.input < 256 && g_qm_holders[r.input];
        if (!strip_run || in.now_ms - g_qm_replay_at > 1000) { replay_queued(); g_qm_replay = false; }
    }
    // the window back in front: a key already down when the reads see it again (the click or Enter that activated the
    // window, a key held across an alt-tab) is dead until released, as one held into a context change a key capture (the
    // rebinding rows) open or just over: a key going down is dead until released, as the window proc swallows its messages
    // (the polled state would route it otherwise), and the wheel is not routed
    const bool capturing = cap_quiet();
    if (in.refocus || capturing)
        for (int k = 1; k < 256; k++) {
            KeyState &s = g_keys[k];
            if (!s.down && is_down(k)) { s = KeyState(); s.down = true; s.dead = true; }
        }
    // A kind F row's op in the context its pulse opened, tried at every read until taken or 1.5 s are up: the context
    // can read the pause menu (screen_track's guess) before the screen does, and its op is taken only then.
    if (g_follow.op != PCOP_NONE && (c != g_follow.from || in.now_ms >= g_follow.until)) {
        const bool in_time = in.now_ms < g_follow.until;
        bool ran = false;
        if (in_time && c != PCX_TRANSITION) if (auto fn = g_ops.load()) ran = fn(g_follow.op, g_follow.arg);
        if (ran || !in_time) {
            if (route_log())
                printf("[pc] pc_route: %s then op %s %d: %s\n", kCtxNames[c], op_name(g_follow.op), g_follow.arg,
                       ran ? "taken" : "not taken in 1.5 s (the menu stays where + opened it)");
            g_follow = Follow();
        }
    }
    if (c == PCX_TRANSITION || g_qm_closing)   // (the presses that start in it, for the replay above)
        for (int k = 1; k < 256; k++) {
            KeyState &s = g_keys[k];
            const bool d = is_down(k);
            if (s.down) { if (!d) s = KeyState(); continue; }
            if (!d) continue;
            s = KeyState(); s.down = true; s.dead = true;
            if (g_queued_n < 8) g_queued[g_queued_n++] = {k, in.now_ms};
            if (route_log()) { char b[16]; printf("[pc] pc_route: %s %s down -> queued\n", g_qm_closing ? "quickmenu (closing)" : "transition", input_id(k, b, sizeof b)); }
        }
    // a running macro's claimed inputs (4.14): a press is the macro's (dead for the rows), one held with its row lets go
    for (int k = 1; k < 256; k++) {
        if (!g_mclaim[k] || !is_down(k)) continue;
        KeyState &s = g_keys[k];
        if (!s.down) { s = KeyState(); s.down = true; s.dead = true; s.mac = true; }
        else s.dead = true;
    }
    // doc 3.10: in the title's "press a button" phase any key or click continues, one A pulse per press, over the rows
    // (not the modifiers, Win, the F-keys (the overlays, F9's capture), Esc, the lock keys)
    if (c == PCX_TITLE && g_title_press.load())
        for (int k = 1; k < 256; k++) {
            if ((k >= VK_SHIFT && k <= VK_MENU) || (k >= VK_LSHIFT && k <= VK_RMENU) || k == VK_LWIN || k == VK_RWIN ||
                k == VK_APPS || (k >= VK_F1 && k <= VK_F24) || k == VK_ESCAPE || k == VK_CAPITAL || k == VK_NUMLOCK ||
                k == VK_SCROLL || k == VK_PAUSE || k == VK_SNAPSHOT)
                continue;
            KeyState &s = g_keys[k];
            if (s.down || !is_down(k)) continue;
            s = KeyState(); s.down = true; s.dead = true; s.any = true;   // (claimed until released, no row)
            pulse(B_A, in.now_ms);
        }
    // the inputs this context routes, and the ones still held from before (dead until released)
    for (int k : g_t.keys[c]) {
        if (k >= 256) continue;
        KeyState &s = g_keys[k];
        const bool d = is_down(k);
        if (d && !s.down) {
            s = KeyState();
            s.down = true;
            s.row = g_t.route[c][k];
            if (s.row >= 0) g_op_done[k] = press(s.row, k, in.now_ms);
            if (undecided(s)) { s.trow = s.row; s.at = in.now_ms; }
        } else if (!d) {
            // (a T key let go before its hold started: the tap; a read late past kTapMs still counts it, as no hold ran)
            if (s.down && !s.dead && undecided(s) && g_t.route[c][k] == s.row) tap(s.row, k, in.now_ms);
            s = KeyState();
        }
    }
    // a T key held kTapMs: its hold row from now on, pressed as if the key went down now (the strip opens 250 ms
    // after the press)
    for (int k : g_t.keys[c]) {
        if (k >= 256) continue;
        KeyState &s = g_keys[k];
        if (!s.down || s.dead || !undecided(s) || g_t.route[c][k] != s.row || in.now_ms - s.at < kTapMs) continue;
        const int16_t h = hold_of(s.row);
        if (route_log()) {
            char b[16];
            printf("[pc] pc_route: %s %s held %llu ms -> %s %s\n", kCtxNames[c], input_id(k, b, sizeof b),
                   (unsigned long long)(in.now_ms - s.at), kRows[s.row].action, h >= 0 ? "(hold)" : "(no hold row: nothing)");
        }
        s.row = h;
        if (h >= 0) g_op_done[k] = press(h, k, in.now_ms);
    }
    for (int k = 1; k < 256; k++) {
        KeyState &s = g_keys[k];
        if (!s.down) continue;
        if (!is_down(k)) { s = KeyState(); continue; }   // (a dead key's release)
        if (s.dead) { out->claimed[k] = true; continue; }
        if (s.row >= 0 && (g_t.route[c][k] == s.row || (s.trow >= 0 && g_t.route[c][k] == s.trow))) {
            out->claimed[k] = true;
            if (kRows[s.row].kind == 'H') out->bits |= kRows[s.row].out;
            if (kRows[s.row].kind == 'G') {
                if (vk_live()) out->vbtn |= row_vk(kRows[s.row]);
                else out->bits |= kRows[s.row].out;   // (no hook yet: its pad bits, as an H row)
            }
        }
    }
    for (int k : g_t.keys[c]) if (k < 256) out->claimed[k] = true;
    if (ui) memset(out->claimed, 1, sizeof out->claimed);
    // Q / E held in a menu repeat the tab switch: 300 ms, then every 80 ms while the key stays down and the context still
    // routes it to the same tab op. A tab change passes through TRANSITION (every key dead there), so the repeat is its
    // own record, not the key's state: it waits out the gap.
    {
        static int rep_k = 0, rep_op = PCOP_NONE;
        static uint64_t rep_next = 0;
        for (int k : g_t.keys[c]) {   // (a fresh press of a tab row starts it)
            if (k >= 256) continue;
            const int16_t r = g_t.route[c][k];
            if (r < 0 || kRows[r].kind != 'O' || (kRows[r].op != PCOP_TAB_PREV && kRows[r].op != PCOP_TAB_NEXT)) continue;
            const KeyState &s = g_keys[k];
            if (s.down && !s.dead && s.row == r && rep_k != k) { rep_k = k; rep_op = kRows[r].op; rep_next = in.now_ms + 300; }
        }
        if (rep_k && !is_down(rep_k)) rep_k = 0;
        if (rep_k && in.now_ms >= rep_next && c != PCX_TRANSITION) {
            const int16_t r = g_t.route[c][rep_k];
            if (r >= 0 && kRows[r].kind == 'O' && kRows[r].op == rep_op) {
                const bool done = press(r, rep_k, in.now_ms);
                if (route_log()) { char b[16]; printf("[pc] pc_route: %s %s held -> %s (repeat)%s\n", kCtxNames[c],
                                                       input_id(rep_k, b, sizeof b), kRows[r].action, done ? "" : ", pulsed"); }
                rep_next = in.now_ms + 80;
            } else rep_k = 0;   // (left the menu, or the key means something else here)
        }
    }
    // the wheel: each notch a press of its direction's row (none: hle_hid's own wheel use); a macro holding the mouse gets
    // the notches instead (below)
    const bool macro_mouse = macro_mouse_wanted() && c != PCX_QUICKMENU;
    // notches while a T row's key (1 / 3) is held outside the quickmenu (its strip opens 250 ms after the press, then
    // the screen's own open) are HELD for that strip, never weapon_next (whose d-right would collide with the strip's):
    // the quickmenu feed below gets them with its own notches once the strip takes the read. Dropped once the key is
    // let go before a strip took them
    static int early_notches;
    bool strip_key = false;
    for (int k = 1; k < 256 && !strip_key; k++) strip_key = g_keys[k].down && !g_keys[k].dead && g_keys[k].trow >= 0;
    if (c != PCX_QUICKMENU) {
        if (strip_key && in.wheel && !macro_mouse && !capturing) {
            early_notches += in.wheel;
            out->wheel_taken = true;
            if (route_log()) printf("[pc] pc_route: %s wheel %+d -> held for the strip (%+d held)\n", kCtxNames[c], in.wheel, early_notches);
        } else if (!strip_key && early_notches) {
            if (route_log()) printf("[pc] pc_route: %s %+d held notch(es) dropped (no strip took them)\n", kCtxNames[c], early_notches);
            early_notches = 0;
        }
    }
    for (int n = macro_mouse || c == PCX_QUICKMENU || capturing || strip_key ? 0 : in.wheel; n; n -= n > 0 ? 1 : -1) {
        const int k = n > 0 ? kWheelUp : kWheelDown;
        const int16_t r = g_t.route[c][k];
        if (r < 0 || kRows[r].kind == 'E' || kRows[r].kind == 'K') break;
        out->wheel_taken = true;
        const bool done = press(r, k, in.now_ms);
        if (route_log()) log_edge(c, k, true, r, done);
    }
    if (route_log() && in.wheel && !out->wheel_taken) {   // (one line per notch with its taker; the row ones are log_edge's)
        const int16_t r = g_t.route[c][in.wheel > 0 ? kWheelUp : kWheelDown];
        for (int n = in.wheel; n; n -= n > 0 ? 1 : -1)
            printf("[pc] pc_route: %s %s -> %s\n", kCtxNames[c], n > 0 ? "wheel_up" : "wheel_down",
                   macro_mouse ? "macro (take_mouse)" : c == PCX_QUICKMENU ? "quickmenu feed" : r >= 0 ? "hle_hid (row E: the map / Purah Pad zoom)"
                   : "hle_hid wheel (quick menus, or dropped)");
    }
    if (route_log()) {   // every key's edges in this context: the action it got, or dead (the layout's keys: not logged)
        static bool was[256];
        static int16_t at_down[256];   // what the down said: the row, -1 dead, -2 the layout's
        for (int k = 1; k < 256; k++) {
            const bool d = is_down(k);
            if (d == was[k]) continue;
            was[k] = d;
            if (d) {
                const KeyState &s = g_keys[k];
                if (s.any || s.mac) {
                    char b[16];
                    printf("[pc] pc_route: %s %s down -> %s\n", kCtxNames[c], input_id(k, b, sizeof b),
                           s.any ? "any key (press a button) I A" : "claimed by a macro");
                    at_down[k] = -3;
                    continue;
                }
                at_down[k] = s.down && !s.dead && s.row >= 0 ? s.row : (s.down && s.dead) || ui ? -1 : -2;
                if (at_down[k] != -2) log_edge(c, k, true, at_down[k], g_op_done[k]);
            } else if (at_down[k] >= -1) log_edge(c, k, false, at_down[k], false);
        }
    }
    {   // the running macros' ticks, in the order they started (the later one's sticks win)
        Running *mouse_run = nullptr;   // (the mouse: to the run started last that asked for it)
        for (auto &r : g_runs) if (r.id >= 0 && r.take_mouse && (!mouse_run || r.seq > mouse_run->seq)) mouse_run = &r;
        double mdx = 0, mdy = 0;
        int mw = 0;
        if (mouse_run) {   // (the notch only outside a quickmenu: there it is the feed's)
            pc_mouse_redirect_take(&mdx, &mdy);
            if (c != PCX_QUICKMENU) { mw = in.wheel; out->wheel_taken = true; }
        }
        bool claims[256] = {};
        Running *order[kRuns];
        int n = 0;
        for (auto &r : g_runs) if (r.id >= 0) order[n++] = &r;
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && order[j]->seq < order[j - 1]->seq; j--) std::swap(order[j], order[j - 1]);
        for (int i = 0; i < n; i++) {
            Running &r = *order[i];
            const PcMacroOps *m = g_macros[r.id].load();
            const bool mine = &r == mouse_run;
            PcMacroIn mi = {c, r.input, in.frame, in.now_ms, in.down, in.user, mine ? mdx : 0, mine ? mdy : 0, mine ? mw : 0};
            PcMacroOut mo = {};
            if (!m || !m->tick || !m->tick(&mi, &mo)) { macro_stop(r, false); continue; }
            for (int v : mo.claim) if (v > 0 && v < 256) claims[v] = true;
            out->bits |= mo.bits;
            for (int s = 0; s < 2; s++) if (mo.stick[s]) { out->stick[s] = true; out->sx[s] = mo.sx[s]; out->sy[s] = mo.sy[s]; }
            r.take_mouse = mo.take_mouse;
        }
        memcpy(g_mclaim, claims, sizeof g_mclaim);   // (the next read's)
    }
    // 4.13: in a quickmenu the mouse is neither the camera nor the R stick (unless a macro holds the mouse), the wheel
    // and strips turn by wheel notches only. Its motion is taken (redirected, so the camera stays still) and dropped
    // here; the notches step the strips through hle_hid's quick-menu wheel (its d-pad hold is on), and the ability wheel
    // / Autobuild list through the executor when it takes the read.
    const bool qm = c == PCX_QUICKMENU && !macro_mouse_wanted();
    if (qm) {
        double dx = 0, dy = 0;
        pc_mouse_redirect_take(&dx, &dy);
        // (a strip, sub != 0, also gets the notches held while its key was undecided; kept until the feed takes them)
        const int sub = g_qm_sub.load(), held = sub ? early_notches : 0;
        if (auto fn = g_qm_feed.load())
            if (fn(dx, dy, in.wheel + held, sub)) {
                out->wheel_taken = true;
                if (held && route_log()) printf("[pc] pc_route: quickmenu %+d held notch(es) to the strip\n", held);
                if (held) early_notches = 0;
            }
    }
    {   // the mouse to a macro while its last tick asked (take_mouse) or to a quickmenu, back to the camera after
        static bool redirecting;
        const bool want = macro_mouse_wanted() || qm;
        if (want != redirecting) { pc_mouse_redirect(want); redirecting = want; }
    }
    {
        static uint64_t published;
        if (out->vbtn != published)
            if (auto w = g_vbtn_writer.load()) { w(out->vbtn); published = out->vbtn; }
    }
    for (size_t i = 0; i < g_pulses.size(); i++) {
        Pulse &p = g_pulses[i];
        if (!p.started) {   // after every earlier pulse on its buttons, and 2 frames after the last of them ended
            bool wait = false;
            for (size_t j = 0; j < i; j++)
                if (g_pulses[j].bits & p.bits) wait |= !g_pulses[j].started || in.frame < g_pulses[j].end + kGapFrames;
            if (wait) continue;
            p.started = true; p.start = in.frame; p.end = in.frame + kPulseFrames;
        }
        if (in.frame < p.end) out->bits |= p.bits;
    }
    for (size_t i = 0; i < g_pulses.size();) {   // (kept 2 frames past their end: the gap before the next on their buttons)
        if (g_pulses[i].started && in.frame >= g_pulses[i].end + kGapFrames) g_pulses.erase(g_pulses.begin() + (long)i);
        else i++;
    }
}

// ---- Rebinding. The settings hold one key list per action level (setting_key); a change is checked first on two
// tables built from the settings as they are and as they would be (build_ctx), so nothing is written unless the whole
// result is sound. Callers: the F10 Keys tab (pc_bind_ui.cpp), the in-game Options rows (totk_options_menu.cpp).
namespace {
std::mutex g_bind_mu;   // (one change at a time; the input thread's own build reads the settings, never this)
struct MustKeep { const char *action; int group; };   // an action that can move but never be left with no key
const MustKeep kMustKeep[] = {{"confirm", S_UI}, {"back", S_UI}, {"pause", S_GAMEPLAY}};
bool scope_ok(int s) { return (s >= 0 && s < PCX_COUNT) || (s >= S_ROOT && s <= S_MENUS); }
bool reserved_input(int in) {   // F10 (the overlay: always the way back), F12 (shots), the Windows keys, PrintScreen
    return in == VK_F10 || in == VK_F12 || in == VK_LWIN || in == VK_RWIN || in == VK_SNAPSHOT;
}
// a layout-independent input (pc_input.h) -> the router's code: a key bound by position through the layout in use
int us_to_live(int in) {
    const int scan = in < 256 ? us_scan_of(in) : 0;
    if (!scan) return in;
    const int vk = (int)MapVirtualKeyExW((UINT)scan, MAPVK_VSC_TO_VK, g_hkl ? g_hkl : layout_now());
    return vk ? vk : in;
}
// an input's name in the settings (input_from_name reads it back: letters and digits by position, "vk 0x.." for the
// punctuation keys, by position too)
std::string canon(int in) {
    if (!in) return "none";
    if (in == kWheelUp) return "Wheel Up";
    if (in == kWheelDown) return "Wheel Down";
    if ((in >= 'A' && in <= 'Z') || (in >= '0' && in <= '9')) return std::string(1, (char)tolower(in));
    static const struct { int vk; const char *n; } names[] = {{VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"},
        {VK_TAB, "Tab"}, {VK_BACK, "Backspace"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"},
        {VK_LEFT, "Left"}, {VK_UP, "Up"}, {VK_RIGHT, "Right"}, {VK_DOWN, "Down"}, {VK_LBUTTON, "Mouse Left"},
        {VK_RBUTTON, "Mouse Right"}, {VK_MBUTTON, "Mouse Middle"}, {VK_XBUTTON1, "Mouse X1"}, {VK_XBUTTON2, "Mouse X2"},
        {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_INSERT, "Insert"},
        {VK_DELETE, "Delete"}};
    for (auto &n : names) if (n.vk == in) return n.n;
    char b[16];
    if (in >= VK_F1 && in <= VK_F12) snprintf(b, sizeof b, "F%d", in - VK_F1 + 1);
    else snprintf(b, sizeof b, "vk 0x%02x", in);
    return b;
}
int us_of_name(std::string n) {   // a settings name -> the layout-independent input (0: none or unknown)
    while (!n.empty() && isspace((unsigned char)n.front())) n.erase(0, 1);
    while (!n.empty() && isspace((unsigned char)n.back())) n.pop_back();
    for (auto &c : n) c = (char)tolower((unsigned char)c);
    if (n == "wheel up") return kWheelUp;
    if (n == "wheel down") return kWheelDown;
    if (n.size() == 1 && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    return n.empty() || n == "none" ? 0 : hid_vk_from_name(n.c_str());
}
std::vector<int> us_list(const char *names) {
    std::vector<int> r;
    std::string s(names ? names : "");
    for (size_t a = 0; a <= s.size();) {
        size_t e = s.find(',', a);
        if (e == std::string::npos) e = s.size();
        if (const int in = us_of_name(s.substr(a, e - a))) r.push_back(in);
        a = e + 1;
    }
    return r;
}
std::string canon_list(const std::vector<int> &v) {
    std::string s;
    for (int in : v) s += (s.empty() ? "" : ",") + canon(in);
    return s.empty() ? "none" : s;
}
// where an action's keys come from in a context: the level that has them in the settings (*val: the value), -1 its row
int key_source(int c, const char *action, const Look &look, std::string *val) {
    for (int o = c; o >= 0; o = parent(o)) {
        char v[96];
        if (look.get(setting_key(o, action).c_str(), v, sizeof v)) { if (val) *val = v; return o; }
    }
    return -1;
}
std::vector<int> keys_of(int c, const char *action, int row, const Look &look) {
    std::string v;
    const int src = kRows[row].kind == 'E' ? -1 : key_source(c, action, look, &v);
    return us_list(src >= 0 ? v.c_str() : kRows[row].keys);
}
bool in_gameplay_file(const char *action) {
    for (int r = kStage1Rows; r < kRowCount; r++) if (!strcmp(kRows[r].action, action)) return true;
    return false;
}
// The contexts a binding at scope reaches: the scope's contexts that have the action, with no more specific level of the
// settings holding it (that level wins there). fixed: its row is E / X, which settings never move.
struct Reach { int c, row; bool fixed; };
std::vector<Reach> reach(int scope, const char *action, bool gp, const Look &look) {
    std::vector<Reach> out;
    for (int c = 0; c < PCX_COUNT; c++) {
        if (c == PCX_OVERLAY || c == PCX_TRANSITION || c == PCX_LOADING || !in_scope(c, scope)) continue;
        const int r = row_of(c, action, gp);
        if (r < 0) continue;
        const bool fixed = kRows[r].kind == 'E' || kRows[r].kind == 'X';
        const int src = fixed ? -1 : key_source(c, action, look, nullptr);
        if (src >= 0 && src != scope && in_scope(src, scope)) continue;
        out.push_back({c, r, fixed});
    }
    return out;
}
std::unique_ptr<Table> table_of(bool gp, const Look &look, int *conflicts) {
    auto t = std::make_unique<Table>();
    int overrides = 0;
    *conflicts = 0;
    for (int c = 0; c < PCX_COUNT; c++) build_ctx(*t, c, gp, look, &overrides, conflicts, true);
    return t;
}
int routed_count(const Table &t, int c, const char *action) {
    int n = 0;
    for (int k : t.keys[c]) if (t.route[c][k] >= 0 && !strcmp(kRows[t.route[c][k]].action, action)) n++;
    return n;
}
void note(PcBindChange *out, int max, int *n, int scope, const char *action, int slot, int old_in, int new_in) {
    if (*n < max && out) {
        PcBindChange &ch = out[*n];
        ch.scope = scope;
        snprintf(ch.action, sizeof ch.action, "%s", action);
        ch.slot = slot; ch.old_input = old_in; ch.new_input = new_in;
    }
    ++*n;
}
unsigned flags_of(int scope, const char *action, const std::vector<Reach> &rch, const Look &look) {
    unsigned f = PCB_F_FIXED;
    bool active = false;
    const bool gp = gameplay_pc();
    for (auto &x : rch) {
        if (!x.fixed) f &= ~(unsigned)PCB_F_FIXED;
        active |= row_of(x.c, action, gp) >= 0;
    }
    char v[96];
    if (look.get(setting_key(scope, action).c_str(), v, sizeof v)) f |= PCB_F_OVERRIDDEN;
    if (!active) f |= PCB_F_INACTIVE;
    return f;
}
// One change: the action's list at scope becomes want (reset: its level's key dropped, want ignored). Swaps per 4.9 / pc-
// controls' answer (b); checks: E rows in the way, mixed source levels, the linked pairs, the must-keep actions, no new
// conflict anywhere. Nothing is written unless all hold.
int bind_change(int scope, const char *action, int slot, int input, bool reset, PcBindChange *out, int max) {
    if (!pc_scheme_on()) return PCB_NOT_READY;
    if (!scope_ok(scope) || !action || !*action) return PCB_UNKNOWN;
    std::lock_guard<std::mutex> l(g_bind_mu);
    const bool gpx = gameplay_pc() || in_gameplay_file(action);   // (under Console: checked as if PC, 4.20 point 5)
    const Look look0;
    const auto rch = reach(scope, action, gpx, look0);
    if (rch.empty()) return PCB_UNKNOWN;
    const Reach *first = nullptr;
    for (auto &x : rch) if (!x.fixed) { first = &x; break; }
    int n = 0;
    if (!first) { note(out, max, &n, scope, action, slot, 0, 0); return PCB_FIXED; }
    const std::vector<int> cur = keys_of(first->c, action, first->row, look0);
    std::vector<int> want = cur;
    int old_in = 0;
    std::vector<std::pair<std::string, std::string>> prop;
    struct Owner { int c, row; std::string action; };   // (per prop entry but a reset's: whose keys it holds)
    std::vector<Owner> owners;
    if (reset) {
        prop.push_back({setting_key(scope, action), ""});
        const Look l1{&prop};
        want = keys_of(first->c, action, first->row, l1);
        old_in = cur.empty() ? 0 : cur[0];
    } else {
        if (slot < 0 || slot > 3 || input < 0 || input > kWheelDown) return PCB_UNKNOWN;
        if (reserved_input(input)) return PCB_RESERVED;
        if (input >= 256)   // (the wheel has no release: pulse kinds only)
            for (auto &x : rch)
                if (!x.fixed && !strchr("POF", kRows[x.row].kind)) return PCB_KIND;
        old_in = slot < (int)want.size() ? want[slot] : 0;
        if (input) {
            for (size_t i = want.size(); i-- > 0;) if (want[i] == input && (int)i != slot) want.erase(want.begin() + (long)i);
            if (slot < (int)want.size()) want[slot] = input;
            else if (want.size() < 4) want.push_back(input);
            else return PCB_UNKNOWN;
        } else if (slot < (int)want.size()) want.erase(want.begin() + slot);   // (packed: the next one moves up)
        prop.push_back({setting_key(scope, action), canon_list(want)});
        owners.push_back({first->c, first->row, action});
    }
    int c0 = 0;
    const auto t0 = table_of(gpx, look0, &c0);
    // the inputs it gains, in each context reached: another action that has one gives it the old key (swap) (give: the
    // key the other action gets for it; a set's the slot's old key, a reset's the key the action has now in the gained
    // key's slot, so "Default" swaps as a set does)
    struct Swap { std::string action; int level, c, row, input, give; };
    std::vector<Swap> swaps;
    for (size_t wi = 0; wi < want.size(); wi++) {
        const int in = want[wi];
        if (std::find(cur.begin(), cur.end(), in) != cur.end()) continue;
        int give = old_in;
        if (reset) give = wi < cur.size() && std::find(want.begin(), want.end(), cur[wi]) == want.end() ? cur[wi] : 0;
        const int lk = us_to_live(in);
        for (auto &x : rch) {
            if (x.fixed) continue;
            const int16_t r = t0->route[x.c][lk];
            if (r < 0 || !strcmp(kRows[r].action, action)) continue;
            if (kRows[r].kind == 'E') { note(out, max, &n, kRows[r].scope, kRows[r].action, 0, in, 0); return PCB_FIXED; }
            const int src = key_source(x.c, kRows[r].action, look0, nullptr);
            const int level = src >= 0 ? src : S_ROOT;   // (root, or the scheme's row: written at root)
            bool known = false;
            for (auto &s : swaps)
                if (s.action == kRows[r].action && s.input == in) {
                    known = true;
                    if (s.level != level) { note(out, max, &n, scope, kRows[r].action, 0, in, 0); return PCB_CHAIN; }
                }
            if (!known) swaps.push_back({kRows[r].action, level, x.c, r, in, give});
        }
    }
    for (auto &s : swaps) {
        std::vector<int> b = keys_of(s.c, s.action.c_str(), s.row, look0);
        for (size_t i = 0; i < b.size(); i++)
            if (b[i] == s.input) {
                if (s.give && std::find(b.begin(), b.end(), s.give) == b.end()) b[i] = s.give;
                else b.erase(b.begin() + (long)i);
                break;
            }
        for (auto &m : kMustKeep)
            if (b.empty() && s.action == m.action && in_scope(s.c, m.group)) {
                note(out, max, &n, s.level, m.action, 0, s.input, 0);
                return PCB_LAST_KEY;
            }
        prop.push_back({setting_key(s.level, s.action.c_str()), canon_list(b)});
        owners.push_back({s.c, s.row, s.action});
    }
    // the whole result: no new conflict, the must-keep actions keep a key, the linked pairs stay apart
    const Look look1{&prop};
    int c1 = 0;
    const auto t1 = table_of(gpx, look1, &c1);
    if (c1 > c0) { note(out, max, &n, scope, action, slot, old_in, input); return PCB_CHAIN; }
    for (auto &m : kMustKeep)
        for (int c = 0; c < PCX_COUNT; c++)
            if (in_scope(c, m.group) && routed_count(*t0, c, m.action) > 0 && routed_count(*t1, c, m.action) == 0) {
                note(out, max, &n, scope, m.action, 0, 0, 0);
                return PCB_LAST_KEY;
            }
    int clash_in = 0;
    if (pair_clash(*t0, nullptr) < 0)
        if (const int p = pair_clash(*t1, &clash_in); p >= 0) {
            note(out, max, &n, scope, strcmp(kPairs[p].a, action) ? kPairs[p].a : kPairs[p].b, 0, clash_in, 0);
            return PCB_CHAIN;
        }
    // a list equal to what its level would inherit without it is that level's default: dropped, not written (a swap
    // back, a key set to its default: the row then reads as not overridden, and settings.ini stays clean)
    for (size_t i = reset ? 1 : 0, o = 0; i < prop.size() && o < owners.size(); i++, o++) {
        if (prop[i].second.empty()) continue;
        auto without = prop;
        without[i].second = "";
        const Look lw{&without};
        if (canon_list(keys_of(owners[o].c, owners[o].action.c_str(), owners[o].row, lw)) == prop[i].second) prop[i].second = "";
    }
    for (auto &e : prop) {
        char v[96];
        if (e.second.empty() && !pc_settings_raw(e.first.c_str(), v, sizeof v)) continue;   // (nothing to drop)
        pc_settings_raw_set(e.first.c_str(), e.second.empty() ? nullptr : e.second.c_str());
    }
    g_rebuild = true;   // (the input thread's table at its next read; the glyph epoch follows)
    g_bind_epoch++;
    note(out, max, &n, scope, action, reset ? 0 : slot, old_in, reset ? (want.empty() ? 0 : want[0]) : input);
    for (auto &s : swaps) note(out, max, &n, s.level, s.action.c_str(), 0, s.input, s.give);
    printf("[pc] bind %s %s: %s (%d swap%s)\n", scope_name(scope), action, canon_list(want).c_str(), (int)swaps.size(),
           swaps.size() == 1 ? "" : "s");
    return n;
}
}  // namespace

extern "C" int pc_bind_set(int scope, const char *action, int slot, int input, PcBindChange *out, int max) {
    return bind_change(scope, action, slot, input, false, out, max);
}
extern "C" int pc_bind_get(int scope, const char *action, int *inputs, int max, unsigned *flags) {
    if (flags) *flags = 0;
    if (!pc_scheme_on()) return PCB_NOT_READY;
    if (!scope_ok(scope) || !action || !*action) return PCB_UNKNOWN;
    std::lock_guard<std::mutex> l(g_bind_mu);
    // (the answer per (scope, action) until the next change: a macro's per-tick ask costs a lookup, not a table walk)
    struct Cached { int scope; std::string action; unsigned epoch; int r; unsigned flags; std::vector<int> keys; };
    static std::vector<Cached> cache;
    const unsigned ep = g_bind_epoch.load();
    Cached *hit = nullptr;
    for (auto &e : cache) if (e.scope == scope && e.action == action) hit = &e;
    if (!hit || hit->epoch != ep) {
        if (!hit) { cache.push_back({scope, action, 0, 0, 0, {}}); hit = &cache.back(); }
        const Look look;
        const auto rch = reach(scope, action, gameplay_pc() || in_gameplay_file(action), look);
        hit->epoch = ep;
        hit->keys.clear();
        hit->flags = 0;
        if (rch.empty()) hit->r = PCB_UNKNOWN;
        else {
            const Reach *first = &rch[0];
            for (auto &x : rch) if (!x.fixed) { first = &x; break; }
            hit->keys = keys_of(first->c, action, first->row, look);
            hit->flags = flags_of(scope, action, rch, look);
            hit->r = (int)hit->keys.size();
        }
    }
    if (hit->r < 0) return hit->r;
    for (int i = 0; i < (int)hit->keys.size() && i < max && inputs; i++) inputs[i] = hit->keys[i];
    if (flags) *flags = hit->flags;
    return hit->r;
}
extern "C" int pc_bind_reset(int scope, const char *action, PcBindChange *out, int max) {
    if (action) return bind_change(scope, action, 0, 0, true, out, max);
    // every override at that scope (Restore defaults): no dry run, the defaults are the scheme's own
    if (!pc_scheme_on()) return PCB_NOT_READY;
    if (!scope_ok(scope)) return PCB_UNKNOWN;
    std::lock_guard<std::mutex> l(g_bind_mu);
    int n = 0;
    std::vector<std::string> done;
    for (int r = 0; r < kRowCount; r++) {
        const std::string key = setting_key(scope, kRows[r].action);
        if (std::find(done.begin(), done.end(), key) != done.end()) continue;
        done.push_back(key);
        char v[96];
        if (!pc_settings_raw(key.c_str(), v, sizeof v)) continue;
        pc_settings_raw_set(key.c_str(), nullptr);
        // (the change: the override's first input -> the default's, 0 = "the default" here)
        const std::vector<int> was = parse_keys(v);
        note(out, max, &n, scope, kRows[r].action, 0, was.empty() ? 0 : was[0], 0);
    }
    if (n) { g_rebuild = true; g_bind_epoch++; }
    return n;
}
extern "C" int pc_bind_actions(int scope, void (*fn)(const char *action, unsigned flags, void *user), void *user) {
    if (!pc_scheme_on()) return PCB_NOT_READY;
    if (!scope_ok(scope)) return PCB_UNKNOWN;
    std::lock_guard<std::mutex> l(g_bind_mu);
    const Look look;
    int n = 0;
    std::vector<std::string> done;
    for (int r = 0; r < kRowCount; r++) {
        const char *a = kRows[r].action;
        if (std::find(done.begin(), done.end(), a) != done.end()) continue;
        done.push_back(a);
        const auto rch = reach(scope, a, true, look);
        if (rch.empty()) continue;
        if (fn) fn(a, flags_of(scope, a, rch, look), user);
        n++;
    }
    return n;
}

// ---- Capture (the rebinding rows' "press a key"). The game window's procedure hands every message here first
// (pc_mouse.cpp's subclass, ahead of the overlay and ImGui): while a capture is open the next key / button down (a wheel
// notch with PCC_WHEEL) is the result and swallowed with its release, Esc cancels (unless PCC_ESC), focus loss cancels.
// The router holds any key going down meanwhile (and for 200 ms after) dead until released, so the polled state cannot
// route it either.
namespace {
std::atomic<int> g_cap{0};               // 1: waiting for a key
std::atomic<int> g_cap_out{0};           // the result: > 0 an input, -1 cancelled
std::atomic<unsigned> g_cap_flags{0};
std::atomic<ULONGLONG> g_cap_quiet{0};   // the router's dead-on-press window ends here
std::atomic<ULONGLONG> g_cap_seen{0};    // the last begin / poll: a capture no one polls for 1 s is cancelled
// The capture's end, once (the window thread's key and the input thread's watchdog may race: the first one wins).
// result > 0 an input, -1 cancelled (why)
void cap_end(int result, const char *why = nullptr) {
    int one = 1;
    if (!g_cap.compare_exchange_strong(one, 0)) return;
    g_cap_out.store(result);
    g_cap_quiet.store(GetTickCount64() + 200);
    if (result > 0) printf("[pc] capture: %d, got %s\n", result, canon(result).c_str());
    else printf("[pc] capture: -1, cancelled (%s)\n", why ? why : "?");
}
// a new capture drops a release the window thread still remembers from the last (its static, cleared there)
std::atomic<bool> g_up_drop{false};
// (the router, every read, so a forgotten one cannot leave every key dead)
bool cap_quiet() {
    if (g_cap.load() && GetTickCount64() - g_cap_seen.load() > 1000) cap_end(-1, "not polled for 1 s");
    return g_cap.load() || GetTickCount64() < g_cap_quiet.load();
}
}  // namespace
extern "C" unsigned pc_mouse_buttons(void);   // pc_mouse.cpp (its first call subclasses the game window)
extern "C" bool pc_input_capture_begin(unsigned flags) {
    if (!pc_scheme_on()) return false;
    (void)pc_mouse_buttons();   // (the subclass that hands the messages here)
    g_cap_flags.store(flags);
    g_cap_out.store(0);
    g_cap_seen.store(GetTickCount64());
    g_up_drop.store(true);
    g_cap.store(1);
    printf("[pc] capture: waiting%s%s\n", flags & PCC_WHEEL ? " (wheel too)" : "", flags & PCC_ESC ? " (Esc too)" : "");
    return true;
}
extern "C" int pc_input_capture_poll(void) {
    g_cap_seen.store(GetTickCount64());
    const int o = g_cap_out.load();
    return o ? o : g_cap.load() ? 0 : -1;
}
extern "C" void pc_input_capture_cancel(void) {
    if (g_cap.load()) cap_end(-1, "the caller");
}
extern "C" bool pc_input_capture_wants_wheel(void) { return g_cap.load() && (g_cap_flags.load() & PCC_WHEEL); }
extern "C" bool pc_input_capture_wheel(int notches) {   // pc_mouse.cpp's raw input: true = the notch is the capture's
    if (!notches || !pc_input_capture_wants_wheel()) return false;
    cap_end(notches > 0 ? kWheelUp : kWheelDown);
    return true;
}
// The window thread, every message first. true: swallowed (the procedure returns 0)
extern "C" bool pc_input_capture_msg(void *hwnd, unsigned m, uintptr_t w, intptr_t l) {
    (void)hwnd;
    static unsigned up_msg, up_msg2;   // the captured input's release, swallowed too (window thread only)
    static uintptr_t up_w;
    static ULONGLONG up_at;
    // (a release that never reaches the window, e.g. a button let go outside it or after a focus change, must not stay
    // armed: the next ordinary release would be swallowed and the button read held)
    if (g_up_drop.exchange(false) ||
        (up_msg && (m == WM_KILLFOCUS || m == WM_CAPTURECHANGED || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) ||
                    GetTickCount64() - up_at > 1000)))
        up_msg = up_msg2 = 0;
    if (up_msg && (m == up_msg || (up_msg2 && m == up_msg2)) && (m == WM_KEYUP || m == WM_SYSKEYUP ? w == up_w : true)) {
        up_msg = up_msg2 = 0;
        return true;
    }
    if (!g_cap.load()) return false;
    auto take = [&](int in, unsigned up, unsigned up2, uintptr_t uw) {
        up_msg = up; up_msg2 = up2; up_w = uw; up_at = GetTickCount64();
        cap_end(in);
        return true;
    };
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        if ((l >> 30) & 1) return true;   // (a key held from before the capture: its repeats, swallowed, never the result)
        const int vk = (int)w;
        if (reserved_input(vk) || vk == VK_PROCESSKEY) return false;   // (F10 closes the overlay: its tab cancels)
        if (vk == VK_ESCAPE && !(g_cap_flags.load() & PCC_ESC)) {
            up_msg = WM_KEYUP; up_msg2 = WM_SYSKEYUP; up_w = w; up_at = GetTickCount64();
            cap_end(-1, "Esc");
            return true;
        }
        const int scan = (int)((l >> 16) & 0xFF);
        const bool ext = ((l >> 24) & 1) != 0;
        int in = ext ? 0 : us_vk_of_scan(scan);   // (by position: the US key there)
        if (!in) in = vk == VK_LSHIFT || vk == VK_RSHIFT ? VK_SHIFT : vk == VK_LCONTROL || vk == VK_RCONTROL ? VK_CONTROL
                    : vk == VK_LMENU || vk == VK_RMENU ? VK_MENU : vk;
        return take(in, WM_KEYUP, WM_SYSKEYUP, w);
    }
    case WM_LBUTTONDOWN: return take(VK_LBUTTON, WM_LBUTTONUP, 0, 0);
    case WM_RBUTTONDOWN: return take(VK_RBUTTON, WM_RBUTTONUP, 0, 0);
    case WM_MBUTTONDOWN: return take(VK_MBUTTON, WM_MBUTTONUP, 0, 0);
    case WM_XBUTTONDOWN: return take(HIWORD(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, WM_XBUTTONUP, 0, 0);
    case WM_KILLFOCUS: cap_end(-1, "focus lost"); return false;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) cap_end(-1, "window inactive"); return false;
    }
    return false;
}

// ---- Key names for the player (pc_input.h): named keys translated, character keys as the layout in use prints them
namespace {
enum KeyWord { KW_SPACE, KW_SHIFT, KW_CTRL, KW_ALT, KW_TAB, KW_ENTER, KW_ESC, KW_BACK, KW_UP, KW_DOWN, KW_LEFT, KW_RIGHT,
               KW_LMB, KW_RMB, KW_MMB, KW_X1, KW_X2, KW_WUP, KW_WDOWN, KW_CAPS, KW_INS, KW_DEL, KW_HOME, KW_END, KW_PGUP,
               KW_PGDN, KW_NUM, KW_COUNT };
// en, es, fr, de, it, nl, ru, ja, ko, zh-Hans, zh-Hant (KW_NUM: "%d" for the digit)
const char *const kKeyWords[11][KW_COUNT] = {
    {"Space", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "Backspace", "Up", "Down", "Left", "Right", "Left Click",
     "Right Click", "Middle Click", "Mouse 4", "Mouse 5", "Wheel Up", "Wheel Down", "Caps Lock", "Insert", "Delete", "Home",
     "End", "Page Up", "Page Down", "Numpad %d"},
    {"Espacio", "Mayús", "Ctrl", "Alt", "Tab", "Intro", "Esc", "Retroceso", "Arriba", "Abajo", "Izquierda", "Derecha",
     "Clic izquierdo", "Clic derecho", "Clic central", "Ratón 4", "Ratón 5", "Rueda arriba", "Rueda abajo", "Bloq Mayús",
     "Insert", "Supr", "Inicio", "Fin", "Re Pág", "Av Pág", "Teclado num. %d"},
    {"Espace", "Maj", "Ctrl", "Alt", "Tab", "Entrée", "Échap", "Retour arrière", "Haut", "Bas", "Gauche", "Droite",
     "Clic gauche", "Clic droit", "Clic molette", "Souris 4", "Souris 5", "Molette haut", "Molette bas", "Verr. Maj",
     "Inser", "Suppr", "Début", "Fin", "Page préc.", "Page suiv.", "Pavé num. %d"},
    {"Leertaste", "Umschalt", "Strg", "Alt", "Tab", "Eingabe", "Esc", "Rücktaste", "Oben", "Unten", "Links", "Rechts",
     "Linksklick", "Rechtsklick", "Mittelklick", "Maus 4", "Maus 5", "Mausrad hoch", "Mausrad runter", "Feststell",
     "Einfg", "Entf", "Pos1", "Ende", "Bild auf", "Bild ab", "Num %d"},
    {"Spazio", "Maiusc", "Ctrl", "Alt", "Tab", "Invio", "Esc", "Backspace", "Su", "Giù", "Sinistra", "Destra",
     "Clic sinistro", "Clic destro", "Clic centrale", "Mouse 4", "Mouse 5", "Rotella su", "Rotella giù", "Bloc Maiusc",
     "Ins", "Canc", "Home", "Fine", "Pag su", "Pag giù", "Tast. num. %d"},
    {"Spatie", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "Backspace", "Omhoog", "Omlaag", "Links", "Rechts",
     "Linkermuisknop", "Rechtermuisknop", "Middelste muisknop", "Muis 4", "Muis 5", "Wiel omhoog", "Wiel omlaag",
     "Caps Lock", "Insert", "Delete", "Home", "End", "Page Up", "Page Down", "Numpad %d"},
    {"Пробел", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "Backspace", "Вверх", "Вниз", "Влево", "Вправо", "ЛКМ",
     "ПКМ", "СКМ", "Мышь 4", "Мышь 5", "Колесо вверх", "Колесо вниз", "Caps Lock", "Insert", "Delete", "Home", "End",
     "Page Up", "Page Down", "Num %d"},
    {"スペース", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "Backspace", "上", "下", "左", "右", "左クリック",
     "右クリック", "ホイールクリック", "マウス4", "マウス5", "ホイール上", "ホイール下", "Caps Lock", "Insert", "Delete",
     "Home", "End", "Page Up", "Page Down", "テンキー%d"},
    {"스페이스", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "Backspace", "위", "아래", "왼쪽", "오른쪽", "왼쪽 클릭",
     "오른쪽 클릭", "휠 클릭", "마우스 4", "마우스 5", "휠 위", "휠 아래", "Caps Lock", "Insert", "Delete", "Home", "End",
     "Page Up", "Page Down", "숫자패드 %d"},
    {"空格", "Shift", "Ctrl", "Alt", "Tab", "回车", "Esc", "退格", "上", "下", "左", "右", "鼠标左键", "鼠标右键",
     "鼠标中键", "鼠标4", "鼠标5", "滚轮上", "滚轮下", "大写锁定", "Insert", "Delete", "Home", "End", "Page Up",
     "Page Down", "小键盘%d"},
    {"空白鍵", "Shift", "Ctrl", "Alt", "Tab", "Enter", "Esc", "退格", "上", "下", "左", "右", "滑鼠左鍵", "滑鼠右鍵",
     "滑鼠中鍵", "滑鼠4", "滑鼠5", "滾輪上", "滾輪下", "Caps Lock", "Insert", "Delete", "Home", "End", "Page Up",
     "Page Down", "數字鍵%d"}};
int lang_index(const char *lang) {
    if (!lang) return 0;
    char l[16] = {};
    for (int i = 0; i < 15 && lang[i]; i++) l[i] = (char)tolower((unsigned char)lang[i]);
    if (!strncmp(l, "zh", 2)) return strstr(l, "hant") || strstr(l, "tw") || strstr(l, "hk") ? 10 : 9;
    static const char *const two[] = {"en", "es", "fr", "de", "it", "nl", "ru", "ja", "ko"};
    for (int i = 0; i < 9; i++) if (!strncmp(l, two[i], 2)) return i;
    return 0;
}
}  // namespace
extern "C" const char *pc_key_name(int in, const char *lang) {
    thread_local std::string out;
    const char *const *w = kKeyWords[lang_index(lang)];
    auto word = [&](int kw) -> const char * { out = w[kw]; return out.c_str(); };
    switch (in) {
    case 0: out.clear(); return out.c_str();
    case kWheelUp: return word(KW_WUP);
    case kWheelDown: return word(KW_WDOWN);
    case VK_SPACE: return word(KW_SPACE);
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return word(KW_SHIFT);
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return word(KW_CTRL);
    case VK_MENU: case VK_LMENU: case VK_RMENU: return word(KW_ALT);
    case VK_TAB: return word(KW_TAB);
    case VK_RETURN: return word(KW_ENTER);
    case VK_ESCAPE: return word(KW_ESC);
    case VK_BACK: return word(KW_BACK);
    case VK_UP: return word(KW_UP);
    case VK_DOWN: return word(KW_DOWN);
    case VK_LEFT: return word(KW_LEFT);
    case VK_RIGHT: return word(KW_RIGHT);
    case VK_LBUTTON: return word(KW_LMB);
    case VK_RBUTTON: return word(KW_RMB);
    case VK_MBUTTON: return word(KW_MMB);
    case VK_XBUTTON1: return word(KW_X1);
    case VK_XBUTTON2: return word(KW_X2);
    case VK_CAPITAL: return word(KW_CAPS);
    case VK_INSERT: return word(KW_INS);
    case VK_DELETE: return word(KW_DEL);
    case VK_HOME: return word(KW_HOME);
    case VK_END: return word(KW_END);
    case VK_PRIOR: return word(KW_PGUP);
    case VK_NEXT: return word(KW_PGDN);
    }
    char b[64];
    if (in >= VK_NUMPAD0 && in <= VK_NUMPAD9) { snprintf(b, sizeof b, w[KW_NUM], in - VK_NUMPAD0); out = b; return out.c_str(); }
    if (in >= VK_F1 && in <= VK_F24) { snprintf(b, sizeof b, "F%d", in - VK_F1 + 1); out = b; return out.c_str(); }
    const HKL hkl = g_hkl ? g_hkl : layout_now();
    const int scan = in < 256 ? us_scan_of(in) : 0;
    const UINT sc = scan ? (UINT)scan : MapVirtualKeyExW((UINT)in, MAPVK_VK_TO_VSC, hkl);
    if (scan) {   // a character key: what the layout in use prints there (flag 4: the keyboard state left as it was)
        const UINT live = MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK, hkl);
        BYTE state[256] = {};
        WCHAR ch[4] = {};
        const int got = ToUnicodeEx(live, sc, state, ch, 4, 4, hkl);
        if (got >= 1 && ch[0] > L' ') {
            CharUpperBuffW(ch, 1);
            char u8[16] = {};
            WideCharToMultiByte(CP_UTF8, 0, ch, 1, u8, sizeof u8, nullptr, nullptr);
            out = u8;
            return out.c_str();
        }
    }
    WCHAR nm[64] = {};
    if (sc && GetKeyNameTextW((LONG)(sc << 16), nm, 64) > 0) {
        char u8[192] = {};
        WideCharToMultiByte(CP_UTF8, 0, nm, -1, u8, sizeof u8, nullptr, nullptr);
        out = u8;
        return out.c_str();
    }
    snprintf(b, sizeof b, "0x%02X", in);
    out = b;
    return out.c_str();
}

// TOTK_CTL's rebinding steps (hle_hid.cpp passes them here; boots check pc_bind_* without the menus):
//   bind <scope> <action> <slot> <key|none>   bindreset <scope> [action]   bindget <scope> <action>   capture [wheel] [esc]
// scope: a context's name (world, pause ...), gameplay / ui / menus, or root. Keys by settings name ("f", "space", "mouse
// right", "wheel up", "f10"). Each logs a "[pc] bind" line with the result and the changes.
namespace {
int scope_from_name(const char *n) {
    if (!_stricmp(n, "root")) return S_ROOT;
    for (int s = 0; s < PCX_COUNT; s++) if (!_stricmp(n, kCtxNames[s])) return s;
    if (!_stricmp(n, "gameplay")) return S_GAMEPLAY;
    if (!_stricmp(n, "ui")) return S_UI;
    if (!_stricmp(n, "menus")) return S_MENUS;
    return -1;
}
void log_changes(const char *what, int r, const PcBindChange *ch, int max) {
    std::string s;
    const int shown = r > 0 ? r : ch[0].action[0] ? 1 : 0;   // (an error names the row in the way in ch[0])
    for (int i = 0; i < shown && i < max; i++) {
        char b[160];
        snprintf(b, sizeof b, " [%s %s slot %d: %s -> %s]", scope_name(ch[i].scope) ? scope_name(ch[i].scope) : "?",
                 ch[i].action, ch[i].slot, canon(ch[i].old_input).c_str(), canon(ch[i].new_input).c_str());
        s += b;
    }
    printf("[pc] bind %s -> %d%s\n", what, r, s.c_str());
}
}  // namespace
extern "C" bool pc_bind_ctl(const char *line) {
    char cmd[16] = {}, a1[32] = {}, a2[32] = {};
    int slot = 0, n = sscanf(line, "%15s %31s %31s %d", cmd, a1, a2, &slot);
    PcBindChange ch[8] = {};
    if (!strcmp(cmd, "bind") && n == 4) {
        const char *key = line;   // (the rest after the slot: a name may hold spaces)
        for (int f = 0; f < 4 && key; f++) { key = strchr(key, ' '); while (key && *key == ' ') key++; }
        const int sc = scope_from_name(a1);
        if (sc < 0 || !key) return false;
        const int in = us_of_name(key);
        const int r = pc_bind_set(sc, a2, slot, in, ch, 8);
        log_changes(line, r, ch, 8);
        return true;
    }
    if (!strcmp(cmd, "bindreset") && n >= 2) {
        const int sc = scope_from_name(a1);
        if (sc < 0) return false;
        const int r = pc_bind_reset(sc, n >= 3 ? a2 : nullptr, ch, 8);
        log_changes(line, r, ch, 8);
        return true;
    }
    if (!strcmp(cmd, "bindget") && n == 3) {
        const int sc = scope_from_name(a1);
        if (sc < 0) return false;
        int keys[4] = {};
        unsigned flags = 0;
        const int r = pc_bind_get(sc, a2, keys, 4, &flags);
        std::string s;
        for (int i = 0; i < r && i < 4; i++) {
            s += " " + canon(keys[i]) + " (en " + pc_key_name(keys[i], "en");
            s += ", es " + std::string(pc_key_name(keys[i], "es"));
            s += ", de " + std::string(pc_key_name(keys[i], "de"));
            s += ", ja " + std::string(pc_key_name(keys[i], "ja")) + ")";
        }
        printf("[pc] bindget %s %s -> %d flags %u:%s\n", a1, a2, r, flags, s.c_str());
        return true;
    }
    if (!strcmp(cmd, "capture")) {
        const unsigned f = (strstr(line, "wheel") ? PCC_WHEEL : 0) | (strstr(line, "esc") ? PCC_ESC : 0);
        return pc_input_capture_begin(f);
    }
    return false;
}
#else
// (Linux: the SDL path keeps its fixed table; nothing routes yet)
extern "C" int pc_ctx(void) { return PCX_WORLD; }
extern "C" const char *pc_ctx_name(int) { return "world"; }
extern "C" bool pc_ctx_is_ui(int) { return false; }
extern "C" bool pc_scheme_on(void) { return false; }
extern "C" void pc_menu_ops_register(bool (*)(int, int)) {}
extern "C" int pc_glyph_vk(int) { return 0; }
extern "C" unsigned pc_glyph_sig(void) { return 0; }
extern "C" int pc_quickmenu_sub(void) { return 0; }
extern "C" bool pc_quickmenu_closing(void) { return false; }
extern "C" int pc_glyph_vk_in(int, int) { return 0; }
extern "C" unsigned pc_glyph_sig_in(int) { return 0; }
extern "C" int pc_glyph_game_set(void) { return PCX_WORLD; }
extern "C" void pc_glyph_intent(int, int) {}
extern "C" bool pc_glyph_intent_active(void) { return false; }
extern "C" bool pc_glyph_qm_strip(void) { return false; }
extern "C" void pc_glyph_kick_register(void (*)(void)) {}
extern "C" int pc_glyph_audit(int) { return 0; }
extern "C" uint16_t pc_glyph_gameplay_cp(uint16_t cp) { return cp; }   // (Windows: hle_fs.cpp's, next to the glyph bank)
extern "C" unsigned pc_glyph_gameplay_epoch(void) { return 0; }
extern "C" uint16_t pc_glyph_cp_in(int, uint16_t cp) { return cp; }
extern "C" unsigned pc_glyph_epoch_in(int) { return 0; }
bool pc_ctx_force(const char *) { return false; }
int pc_ctx_decide(bool, bool, uint32_t, int, int) { return PCX_WORLD; }
extern "C" void pc_ctx_ui(uint32_t, uint32_t) {}
extern "C" int pc_ui_sub(void) { return -1; }
extern "C" void pc_input_focus(bool) {}
extern "C" void pc_input_gameplay_set(const char *) {}
extern "C" void pc_quickmenu_feed_register(bool (*)(double, double, int, int)) {}
extern "C" void pc_input_keys_restore(void) {}
extern "C" void pc_ctx_player(uint32_t, uint8_t) {}
extern "C" void pc_ctx_player_map_register(int (*)(uint32_t, uint8_t)) {}
extern "C" uint32_t pc_ctx_player_flags(void) { return 0; }
extern "C" uint8_t pc_ctx_ability(void) { return 0; }
extern "C" void pc_vbtn_writer_register(void (*)(uint64_t), void (*)(uint64_t)) {}
void pc_macro_register(int, const PcMacroOps *) {}
void pc_ctx_player_fake(bool, uint32_t, uint8_t) {}
void pc_route(const PcRouteIn &, PcRouteOut *out) { *out = PcRouteOut(); }
#endif

#ifndef _WIN32
// Rebinding (pc_input.h, stage 3): Windows only so far (the SDL path routes nothing); every call says so.
extern "C" int pc_bind_set(int, const char *, int, int, PcBindChange *, int) { return PCB_NOT_READY; }
extern "C" int pc_bind_get(int, const char *, int *, int, unsigned *flags) {
    if (flags) *flags = 0;
    return PCB_NOT_READY;
}
extern "C" int pc_bind_reset(int, const char *, PcBindChange *, int) { return PCB_NOT_READY; }
extern "C" int pc_bind_actions(int, void (*)(const char *, unsigned, void *), void *) { return PCB_NOT_READY; }
extern "C" bool pc_input_capture_begin(unsigned) { return false; }
extern "C" int pc_input_capture_poll(void) { return -1; }
extern "C" void pc_input_capture_cancel(void) {}
extern "C" const char *pc_key_name(int, const char *) { return ""; }
extern "C" bool pc_bind_ctl(const char *) { return false; }
#endif
