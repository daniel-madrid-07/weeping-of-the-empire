// totk_menu_cursor.cpp: the mouse in TotK's menus.
//
// While a menu screen with a focus box (eui's BoxCursor: the frame the d-pad moves) is open, the Windows pointer is shown
// (pc_mouse_cursor_mode: the pc_mouse.cpp releases it from the camera) and drives that focus: the focusable item under it becomes
// the focused one, as if the d-pad had moved there (sound, highlight and the item's description included); a left click on an
// item is A on it. The engine draws nothing (the cursor is the Windows arrow) and calls no guest code: on the game thread, at
// each pad read (GetNpadStates, hle_hid.cpp), the open screens are read and the focus is moved with the writes
// eui::BoxCursorMgr::moveBoxCursor (main+0x2230100) itself makes, so the game's own BoxCursorControl moves the box at its next
// update.
//
// The pieces:
//  - eui::ScreenMgr::sInstance (main+0x4640010) -> mpBoxCursorMgr (+0x3f8) -> mBoxCursorControlArray[2] (+0x28),
//    one per draw target; a control's mpActiveNode (+0x58) is the focused BoxCursorNode, whose mpScreen (+0x20) is
//    the screen that has the focus.
//  - that screen's mBoxCursorList (+0x80, an OffsetList) holds its focusable nodes; a node's mpAnimButton (+0x18)
//    has the hit pane (+0x58) that AnimButton::HitTest tests a touch against.
//  - the hit test (main+0x222ea40, +0x223b090): the pane's centre is its global matrix's translation, moved by half
//    its size for a left / right / top / bottom base position, its half size |size * scale| / 2; the pane and its
//    parents must be shown (AnimButton::HitTest's rule). Coordinates: the screen's layout (nn::ui2d::Layout size,
//    1920 x 1080 for the pause menu), origin at the centre, y up.
//  - moveBoxCursor: control->mpReservedActiveNode = node, screen->mpReservedActiveBoxCursorNode = node,
//    screen->mRequestFlags |= 4.
// The focus moves only when the pointer moves (or clicks), so the keys and the pad keep theirs while it rests. eui's
// own pointer path (UIController's pointer, ButtonGroup hit tests) is dead in retail and stays unused: its keyboard /
// mouse device and controller are dummies, and a hover there would not move the BoxCursor. controls.menu_cursor
// (TOTK_MENU_CURSOR, live; default on): 0 = the menus are the pad's and the keys' only. With the map mouse on
// (TOTK_MOUSE_MAP: the map drags with the pointer captured) the Purah Pad stays theirs.
#include "rt.h"
#include "totk_version.h"
#include "pc_lang.h"
#include "guest/guest.h"   // (design A's hook: guest calls on eui's thread)
#include "pc_settings.h"
#include "totk_menu_cursor.h"
#if __has_include("pc_input.h")
#include "totk_player_state.h"   // (TPS_HORSE: totk_controls_page_set)
#include "totk_globals.h"        // (totk_global: the controls page's riding fields)
#include "pc_input.h"  // the router: pc_ctx_ui, pc_menu_ops_register, over_item
#define MC_ROUTER 1
#else
#pragma message("totk_menu_cursor.cpp: no pc_input.h, the router hooks are left out")
#endif
extern "C" {
#include "nvn/output_scale.h"
}

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void pc_mouse_cursor_mode(bool on);              // pc_mouse.cpp
extern "C" bool pc_mouse_client_pos(float* nx, float* ny);  // 0..1 of the client area
extern "C" unsigned pc_mouse_buttons(void);                 // bit 0 left, 1 right (the window's button messages)
extern "C" unsigned pc_mouse_presses(int b);                // presses of button b (0 left, 1 right) since start
extern "C" bool pc_mouse_enabled(void);                     // the mouse camera (pc_mouse.cpp's window hook) is on
extern "C" bool pc_mouse_pointer_hidden(void);  // a pad in use hides the released pointer
extern "C" int hid_screen_now(void);   // hle_hid.cpp screen_track: 0 game, 1 Purah Pad, 2 pause menu, 3 other
extern "C" bool hid_mouse_map_on(void);   // hle_hid.cpp: controls.mouse_map (TOTK_MOUSE_MAP), any thread
extern "C" bool pc_overlay_open(void);  // pc_overlay.cpp: the F10 settings window (or the welcome) has the input
extern "C" int osd_menu_is_open(void);  // hle_osd.cpp: the Graphics menu has the input

namespace {

std::atomic<int> g_on{-1};   // controls.menu_cursor: -1 not read yet

const char* const kOffOn[] = {"0", "1", nullptr};
const char* const kOffOnNames[] = {"Off", "On", nullptr};
void apply(const char* v) { g_on.store(!v || strcmp(v, "0") != 0); }
const pc_setting kRows[] = {
    {"controls.menu_cursor", "Controls", "Keyboard and mouse", "Mouse cursor in menus",
     "TOTK_MENU_CURSOR: in a menu the pointer shows and selects what it points at; left click is A (back is Esc).",
     "1", kOffOn, kOffOnNames, 0, 0, 0, 0, "TOTK_MENU_CURSOR", nullptr, apply, nullptr, nullptr},
};
[[maybe_unused]] const bool g_rows = (pc_settings_add(kRows, 1), true);

bool enabled() {
    int on = g_on.load();
    if (on < 0) {
        const char* e = getenv("TOTK_MENU_CURSOR");
        on = !e || strcmp(e, "0") != 0;
        int unset = -1;
        g_on.compare_exchange_strong(unset, on);
        on = g_on.load();
    }
    return on != 0;
}

#ifdef _WIN32
constexpr u64 kMain = 0x7100000000ull;          // main's base in the engine (gen.py IMGBASE)
constexpr u64 kScreenMgr = totk_main(0x4640010);   // eui::ScreenMgr::sInstance

// eui::ScreenMgr, BoxCursorMgr, BoxCursorControl
constexpr u64 kSmBoxCursorMgr = 0x3f8, kBmControls = 0x28, kCtlActive = 0x58, kCtlReserved = 0x60;
// eui::Screen
constexpr u64 kScrDrawInfo = 0x60, kScrLayout = 0x28, kScrNodes = 0x80, kScrActive = 0x148, kScrReserved = 0x150, kScrState = 0x16e,
              kScrRequest = 0x175;
constexpr u8 kScrOpened = 2;
// eui::BoxCursorNode, AnimButton (ButtonBase)
constexpr u64 kNodeButton = 0x18, kNodeScreen = 0x20, kBtnLayout = 0x20, kBtnFlags = 0x40, kBtnHitPane = 0x58,
              kBtnCursorPane = 0x60;
constexpr u32 kBtnUsable = 0x20010;   // both set: active and enabled (ButtonGroup::Update's test)
// nn::ui2d::DrawInfo (eui::DrawInfoEx): the projection (4 x 4) and view (3 x 4) matrices the screen is drawn with
constexpr u64 kDiProj = 0x10, kDiView = 0x50;
// nn::ui2d::Layout (eui::LayoutEx): its size (the bflyt's) and name
constexpr u64 kLytRoot = 0x18, kLytSize = 0x28, kLytName = 0x30;
// nn::ui2d::Pane
constexpr u64 kPaneParent = 0x18, kPaneChildren = 0x20, kPaneSize = 0x50, kPaneFlag = 0x58, kPaneAlpha = 0x59,
              kPaneBase = 0x5b, kPaneMtx = 0x70, kPaneName = 0xb0;

template <class T> T rd(u64 a) {
    T v;
    memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), sizeof v);
    return v;
}
template <class T> void wr(u64 a, T v) { memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, sizeof v); }
// A pointer the game keeps in a field (heap or image, 8-aligned); only keeps a garbage value from being followed
bool ptr_ok(u64 p) { return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0; }

// AnimButton::HitTest's visibility: the pane shown with some alpha, each parent shown and not hidden by an alpha of 0
// it passes on (flag bit 1: influenced alpha)
bool pane_shown(u64 pane) {
    if (!(rd<u8>(pane + kPaneFlag) & 1) || !rd<u8>(pane + kPaneAlpha)) return false;
    int depth = 0;
    for (u64 p = rd<u64>(pane + kPaneParent); p; p = rd<u64>(p + kPaneParent)) {
        if (!ptr_ok(p) || ++depth > 64) return false;
        const u8 f = rd<u8>(p + kPaneFlag);
        if (!(f & 1) || (!rd<u8>(p + kPaneAlpha) && (f & 2))) return false;
    }
    return true;
}
// main+0x223b090: the pane's box in layout coordinates, its centre and half size
void pane_box(u64 pane, float* cx, float* cy, float* hw, float* hh) {
    const float w = rd<float>(pane + kPaneSize), h = rd<float>(pane + kPaneSize + 4);
    const float m00 = rd<float>(pane + kPaneMtx), m11 = rd<float>(pane + kPaneMtx + 0x14);
    const u8 base = rd<u8>(pane + kPaneBase), bh = base & 3, bv = base >> 2 & 3;
    *hw = fabsf(w * m00) * 0.5f; *hh = fabsf(h * m11) * 0.5f;
    *cx = rd<float>(pane + kPaneMtx + 0xc); *cy = rd<float>(pane + kPaneMtx + 0x1c);
    if (bh == 1) *cx += *hw; else if (bh == 2) *cx -= *hw;   // the translation is the left / right edge
    if (bv == 1) *cy -= *hh; else if (bv == 2) *cy += *hh;   // ... the top / bottom edge
}
// main+0x222ea40: the point (layout coordinates) inside the pane's box
bool pane_hit(u64 pane, float x, float y) {
    float cx, cy, hw, hh;
    pane_box(pane, &cx, &cy, &hw, &hh);
    return x >= cx - hw && x <= cx + hw && y >= cy - hh && y <= cy + hh;
}

// An object of the game: its vtable in main's image (the offsets above are this main's: the exe is recompiled from
// it, so they cannot drift without a new exe; this only keeps a stray value from being written through)
bool game_object(u64 p) {
    if (!ptr_ok(p)) return false;
    const u64 vt = rd<u64>(p);
    return vt >= kMain && vt < kMain + 0x6000000;
}
// The screen's draw transform (DrawInfo: projection x view), for a screen drawn away from its centred layout space.
// No such screen is known (the dialogue choices, first suspected, have a plain transform and stale hit panes:
// mc_dial6); kept, gated to non-plain transforms, for one that does
struct Xf {
    bool ok;
    float v[12], p[16];
    bool plain(float lw, float lh) const;   // it maps the layout as the layout test does (then it adds nothing)
};
Xf screen_xf(u64 scr) {
    Xf x{};
    const u64 di = rd<u64>(scr + kScrDrawInfo);
    if (!ptr_ok(di)) return x;
    memcpy(x.p, reinterpret_cast<const void*>(static_cast<uintptr_t>(di + kDiProj)), sizeof x.p);
    memcpy(x.v, reinterpret_cast<const void*>(static_cast<uintptr_t>(di + kDiView)), sizeof x.v);
    for (float f : x.p) if (!std::isfinite(f)) return x;
    for (float f : x.v) if (!std::isfinite(f)) return x;
    // scale and translation only (pane_hit_ndc bounds two corners: exact for those, wrong under a rotation or shear)
    x.ok = x.p[0] != 0 && x.p[5] != 0 && fabsf(x.p[1]) < 1e-5f && fabsf(x.p[4]) < 1e-5f && fabsf(x.v[1]) < 1e-4f &&
           fabsf(x.v[4]) < 1e-4f;
    return x;
}
// A point of the layout (z 0) to normalised device coordinates (-1..1, y up), rows of row-major matrices
bool to_ndc(const Xf& t, float x, float y, float* nx, float* ny) {
    const float vx = t.v[0] * x + t.v[1] * y + t.v[3], vy = t.v[4] * x + t.v[5] * y + t.v[7],
                vz = t.v[8] * x + t.v[9] * y + t.v[11];
    const float cx = t.p[0] * vx + t.p[1] * vy + t.p[2] * vz + t.p[3];
    const float cy = t.p[4] * vx + t.p[5] * vy + t.p[6] * vz + t.p[7];
    const float cw = t.p[12] * vx + t.p[13] * vy + t.p[14] * vz + t.p[15];
    if (!(fabsf(cw) > 1e-6f)) return false;
    *nx = cx / cw; *ny = cy / cw;
    return true;
}
// The layout's centre and corner land where the layout test puts them (device 0,0 and 1,1, y up): the transform is
// the plain one and the fallback would only repeat that test; a misread or odd transform then never hits empty space
bool Xf::plain(float lw, float lh) const {
    float ax, ay, bx, by;
    if (!to_ndc(*this, 0, 0, &ax, &ay) || !to_ndc(*this, lw / 2, lh / 2, &bx, &by)) return true;
    return fabsf(ax) < 0.01f && fabsf(ay) < 0.01f && fabsf(bx - 1) < 0.01f && fabsf(by - 1) < 0.01f;
}
// pane_hit through the draw transform: the pane's box (as pane_hit makes it) to device coordinates, against the
// pointer's (ndx, ndy)
bool pane_hit_ndc(u64 pane, const Xf& t, float ndx, float ndy) {
    float cx, cy, hw, hh;
    pane_box(pane, &cx, &cy, &hw, &hh);
    float ax, ay, bx, by;
    if (!to_ndc(t, cx - hw, cy - hh, &ax, &ay) || !to_ndc(t, cx + hw, cy + hh, &bx, &by)) return false;
    return ndx >= fminf(ax, bx) && ndx <= fmaxf(ax, bx) && ndy >= fminf(ay, by) && ndy <= fmaxf(ay, by);
}

struct Focus { u64 control, node, screen; int target; };   // (target: the draw target, 0 or 1)
// The focused node of an open screen, from the draw targets' box cursors (all 0: none, as in gameplay)
Focus focus_now() {
    const u64 sm = rd<u64>(kScreenMgr);
    if (!game_object(sm)) return {};
    const u64 bm = rd<u64>(sm + kSmBoxCursorMgr);
    if (!ptr_ok(bm)) return {};
    for (int i = 0; i < 2; i++) {
        const u64 ctl = rd<u64>(bm + kBmControls + 8ull * i);
        if (!ptr_ok(ctl)) continue;
        const u64 node = rd<u64>(ctl + kCtlActive);
        if (!ptr_ok(node)) continue;
        const u64 scr = rd<u64>(node + kNodeScreen);
        if (!game_object(scr) || rd<u8>(scr + kScrState) != kScrOpened) continue;
        return {ctl, node, scr, i};
    }
    return {};
}

// The screen's usable node under (x, y); 0 if none. The list is an sead OffsetList: head {prev, next} at +0x80,
// count +0x90, node-to-link offset +0x94. any: also a node whose button is not usable now (a focused item's button
// can be in its focus animation: mc_real8 / mc_real9 lost clicks 0.5 s after a hover). Which pane stands for a node
// is chosen per screen: the buttons' hit panes (the touch areas eui tests), as verified on the inventory and
// Options; or, on a screen where every usable node's hit pane lies off the visible layout (stale: TotK never uses
// touch; the dialogue choices' sit at x 1425 of a +-960 layout while the choices are drawn at x ~730, mc_dial6),
// the buttons' own parts panes (their layouts' roots: the items as drawn), the nearest centre among those under the
// pointer (parts panes can be several items wide: mc_tabs1). (t: x, y are device coordinates, tested through the
// screen's draw transform; lw, lh: the layout's size.)
template <class F> void for_nodes(u64 scr, int* count, F&& f) {
    const u64 head = scr + kScrNodes;
    const s32 off = rd<s32>(head + 0x14);
    *count = 0;
    if (off < 0 || off > 0x40) return;
    for (u64 link = rd<u64>(head + 8); link != head; link = rd<u64>(link + 8)) {
        if (!ptr_ok(link) || ++*count > 512) return;
        const u64 node = link - (u64)off, btn = rd<u64>(node + kNodeButton);
        if (ptr_ok(btn) && !f(node, btn)) return;
    }
}
u64 root_pane(u64 btn) {
    const u64 blyt = rd<u64>(btn + kBtnLayout);
    return ptr_ok(blyt) ? rd<u64>(blyt + kLytRoot) : 0;
}
// Every usable node's shown hit pane has its centre off the visible layout (and there is at least one)
bool stale_hit_panes(u64 scr, float lw, float lh, const Xf* t) {
    int n = 0, seen = 0;
    bool off_all = true;
    for_nodes(scr, &n, [&](u64, u64 btn) {
        if (~rd<u32>(btn + kBtnFlags) & kBtnUsable) return true;
        const u64 pane = rd<u64>(btn + kBtnHitPane);
        if (!ptr_ok(pane) || !pane_shown(pane)) return true;
        float cx, cy, hw, hh;
        pane_box(pane, &cx, &cy, &hw, &hh);
        float vx = cx, vy = cy;
        const bool in = t ? to_ndc(*t, cx, cy, &vx, &vy) && fabsf(vx) <= 1 && fabsf(vy) <= 1
                          : fabsf(cx) <= lw / 2 && fabsf(cy) <= lh / 2;
        seen++;
        if (in) { off_all = false; return false; }
        return true;
    });
    return seen && off_all;
}
u64 node_at(u64 scr, float x, float y, int* count, float lw, float lh, bool any = false, const Xf* t = nullptr) {
    auto hit = [&](u64 pane) {
        return ptr_ok(pane) && pane_shown(pane) && (t ? pane_hit_ndc(pane, *t, x, y) : pane_hit(pane, x, y));
    };
    auto usable = [&](u64 btn) { return any || !(~rd<u32>(btn + kBtnFlags) & kBtnUsable); };
    u64 found = 0;
    if (!stale_hit_panes(scr, lw, lh, t)) {
        for_nodes(scr, count, [&](u64 node, u64 btn) {
            if (usable(btn) && hit(rd<u64>(btn + kBtnHitPane))) { found = node; return false; }
            return true;
        });
        return found;
    }
    float best = 1e30f;
    for_nodes(scr, count, [&](u64 node, u64 btn) {
        const u64 pane = root_pane(btn);
        if (!usable(btn) || !hit(pane)) return true;
        float cx, cy, hw, hh;
        pane_box(pane, &cx, &cy, &hw, &hh);
        float d;
        if (t) {
            float nx, ny;
            d = to_ndc(*t, cx, cy, &nx, &ny) ? (nx - x) * (nx - x) + (ny - y) * (ny - y) : 1e29f;
        } else d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (d < best) best = d, found = node;
        return true;
    });
    return found;
}

// ---- the scrolling lists A list that scrolls draws its rows through a scissor pane (nn::ui2d scr1, S_Scissor_00: the
// Options list's 960 x 940 window, the recipe log's 610 x 870; Common's SystemOption_00 / PaRecipeLogSlideList_00). Rows
// past its edges stay shown and hit-testable (Options hides only those over three row heights out), and the game scrolls
// a focused row into the window: a hover on a row cut by the edge, or on one hidden under a header, scrolled the list,
// and the next move did it again. So: a pointer outside the window hits nothing of its list; a hover focuses a row only
// when its whole box is inside the window (less a margin learnt where that still scrolled: hover_watch); a click still
// takes a cut row (its scroll is the click's); the wheel steps the window (scroll_target).
void layout_name(u64 scr, char* out, size_t n);
// The lists' scissors in Common (every scr1 pane, lyt_dump): S_Scissor_* (Options, the save list, the recipe log,
// shops, Character Profiles, the horse list) and the Adventure Log's S_SlideList_00; the S_Deco* / gauge / text ones
// clip no list
u64 scissor_of(u64 pane) {   // the pane's nearest list scissor ancestor, 0: none
    int depth = 0;
    for (u64 p = ptr_ok(pane) ? rd<u64>(pane + kPaneParent) : 0; ptr_ok(p) && ++depth <= 32; p = rd<u64>(p + kPaneParent)) {
        const char* nm = reinterpret_cast<const char*>(static_cast<uintptr_t>(p + kPaneName));
        if (!memcmp(nm, "S_Scissor", 9) || !memcmp(nm, "S_SlideList_00", 15)) return p;   // (the Log's: that name exactly)
    }
    return 0;
}
// The pane node_at tests for a node (its button's hit pane, or the parts pane on a stale-hit-panes screen)
u64 node_pane(u64 node, bool stale) {
    const u64 btn = rd<u64>(node + kNodeButton);
    return !ptr_ok(btn) ? 0 : stale ? root_pane(btn) : rd<u64>(btn + kBtnHitPane);
}
// Margins learnt per list (by the screen's layout name; a scissor's address does not outlive its screen)
struct ListMargin { char layout[40]; float mx, my; };
ListMargin g_margins[8];
int g_nmargins;
ListMargin* list_margin(u64 scr, bool add) {
    char nm[40];
    layout_name(scr, nm, sizeof nm);
    for (int i = 0; i < g_nmargins; i++) if (!strcmp(g_margins[i].layout, nm)) return &g_margins[i];
    if (!add || g_nmargins >= 8) return nullptr;
    ListMargin* m = &g_margins[g_nmargins++];
    *m = ListMargin{};
    snprintf(m->layout, sizeof m->layout, "%s", nm);
    return m;
}
TotkBox box_of(u64 pane) {
    TotkBox b;
    pane_box(pane, &b.cx, &b.cy, &b.hw, &b.hh);
    return b;
}
// The screen's layout size (the bflyt's; 1920 x 1080 when it cannot be read)
void layout_size(u64 scr, float* lw, float* lh) {
    *lw = 1920, *lh = 1080;
    const u64 lyt = rd<u64>(scr + kScrLayout);
    if (!ptr_ok(lyt)) return;
    const float w = rd<float>(lyt + kLytSize), h = rd<float>(lyt + kLytSize + 4);
    if (w >= 64 && w <= 8192 && h >= 64 && h <= 8192) *lw = w, *lh = h;
}
// A row a hover focused, watched for 600 ms: if it moves while its window stays (the list scrolled for it after all:
// the game keeps a margin there), that list's margin on that axis grows past the gap the row had, so such rows take no
// hover again (at most 0.4 of the window's half size). A margin lasts one opening of its screen (it starts again at 0
// when the screen opens), and no hover is watched in a screen's first 500 ms: its slide-in moves the rows too
struct HoverWatch { u64 screen = 0, node = 0, pane = 0, sc = 0; float cx = 0, cy = 0, sx = 0, sy = 0; ULONGLONG at = 0; };
HoverWatch g_hw;
ULONGLONG list_screen_age(u64 scr, ULONGLONG now) {   // ms since the focus screen became scr (each frame and hover)
    static u64 last;
    static ULONGLONG since;
    if (scr != last) {
        last = scr, since = now;
        if (ListMargin* m = scr ? list_margin(scr, false) : nullptr) m->mx = m->my = 0;
    }
    return now - since;
}
void hover_note(u64 scr, u64 node, bool stale, ULONGLONG now) {
    g_hw = HoverWatch{};
    if (list_screen_age(scr, now) < 500) return;
    const u64 p = node_pane(node, stale), sc = ptr_ok(p) ? scissor_of(p) : 0;
    if (!sc) return;
    float hw, hh, sw, sh;
    g_hw.screen = scr, g_hw.node = node, g_hw.pane = p, g_hw.sc = sc, g_hw.at = now;
    pane_box(p, &g_hw.cx, &g_hw.cy, &hw, &hh);
    pane_box(sc, &g_hw.sx, &g_hw.sy, &sw, &sh);
}
void hover_watch(u64 scr, u64 focused, ULONGLONG now) {
    list_screen_age(scr, now);
    if (!g_hw.node) return;
    if (scr != g_hw.screen || now - g_hw.at > 600) { g_hw.node = 0; return; }
    if (focused != g_hw.node) return;   // (the focus not there yet)
    float cx, cy, hw, hh, sx, sy, sw, sh;
    pane_box(g_hw.pane, &cx, &cy, &hw, &hh);
    pane_box(g_hw.sc, &sx, &sy, &sw, &sh);
    if (fabsf(sx - g_hw.sx) > 0.5f || fabsf(sy - g_hw.sy) > 0.5f) { g_hw.node = 0; return; }   // (the window moved too)
    const float ddx = cx - g_hw.cx, ddy = cy - g_hw.cy;
    if (fabsf(ddx) < 8 && fabsf(ddy) < 8) return;
    g_hw.node = 0;
    const bool vert = fabsf(ddy) >= fabsf(ddx);
    const float c = vert ? g_hw.cy : g_hw.cx, h = vert ? hh : hw, sc = vert ? sy : sx, w = vert ? sh : sw;
    const float gap = fminf((c - h) - (sc - w), (sc + w) - (c + h));
    ListMargin* m = list_margin(scr, true);
    if (!m) return;
    float& mm = vert ? m->my : m->mx;
    const float want = fminf(fmaxf(gap, 0.f) + 2, 0.4f * w);
    if (want <= mm) return;
    mm = want;
    printf("[menu-cursor] %s: a hover scrolled its list (%s %+.0f): no hover within %.0f of the window's edge now\n",
           m->layout, vert ? "y" : "x", vert ? ddy : ddx, mm);
}
// The wheel's step (dir -1 up, 1 down) in the focused row's list (totk_scroll_pick over the usable shown rows under the
// same scissor); 0 when the focus is not in a scrolling list or there is nothing to step to
bool is_options(u64 scr);
constexpr float kOptListTop = 435.5f, kOptListBottom = -400.0f;   // Options' rows whole on screen (options_row_cut)
// A list's rows: the usable shown nodes under scissor sc but `except` (at most 128), and its window (Options' less its
// masks, the band options_row_cut uses)
int list_rows(u64 scr, u64 sc, u64 except, bool stale, u64* nodes, TotkBox* boxes) {
    int k = 0, cnt = 0;
    for_nodes(scr, &cnt, [&](u64 node, u64 btn) {
        if (node == except || (~rd<u32>(btn + kBtnFlags) & kBtnUsable)) return true;
        const u64 p = node_pane(node, stale);
        if (!ptr_ok(p) || !pane_shown(p) || scissor_of(p) != sc) return true;
        nodes[k] = node, boxes[k] = box_of(p);
        return ++k < 128;
    });
    return k;
}
TotkBox list_window(u64 scr, u64 sc) {
    TotkBox w = box_of(sc);
    if (is_options(scr)) w.cy = (kOptListTop + kOptListBottom) / 2, w.hh = (kOptListTop - kOptListBottom) / 2;   // (its masks)
    return w;
}
u64 scroll_target(u64 scr, u64 focused, int dir, bool stale) {
    const u64 fp = node_pane(focused, stale), sc = ptr_ok(fp) ? scissor_of(fp) : 0;
    if (!sc) return 0;
    const ListMargin* lm = list_margin(scr, false);
    u64 nodes[128];
    TotkBox boxes[128];
    const int k = list_rows(scr, sc, focused, stale, nodes, boxes);
    const TotkBox fb = box_of(fp), w = list_window(scr, sc);
    const int i = totk_scroll_pick(&fb, boxes, k, &w, lm ? lm->mx : 0, lm ? lm->my : 0, dir);
    return i >= 0 ? nodes[i] : 0;
}
// A hover may focus this row of a scrolling list: whole inside the window (less the margins learnt) and not the last
// whole row before a cut one on either side (totk_row_at_edge: a list that keeps a margin scrolls for it). 0: yes;
// 1: cut; 2: at the edge
int hover_blocked(u64 scr, u64 node, u64 sc, bool stale) {
    const ListMargin* lm = list_margin(scr, false);
    const float mx = lm ? lm->mx : 0, my = lm ? lm->my : 0;
    const TotkBox b = box_of(node_pane(node, stale)), w = list_window(scr, sc);
    if (!totk_box_in_window(&b, &w, mx, my)) return 1;
    u64 nodes[128];
    TotkBox boxes[128];
    const int k = list_rows(scr, sc, node, stale, nodes, boxes);
    return totk_row_at_edge(&b, boxes, k, &w, mx, my) ? 2 : 0;
}

// Tests (verbose): each node of the screen with its button's flags and hit pane (centre, half size, shown), once per
// screen, when a pointer finds no item there
void dump_nodes(u64 scr) {
    static u64 dumped[16];
    for (u64 d : dumped) if (d == scr) return;
    for (u64& d : dumped) if (!d) { d = scr; break; }
    const Xf t = screen_xf(scr);
    if (t.ok)
        printf("[menu-cursor]   draw: proj %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / w %.4f %.4f %.4f %.4f; view %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f\n",
               t.p[0], t.p[1], t.p[2], t.p[3], t.p[4], t.p[5], t.p[6], t.p[7], t.p[12], t.p[13], t.p[14], t.p[15], t.v[0], t.v[1],
               t.v[2], t.v[3], t.v[4], t.v[5], t.v[6], t.v[7]);
    const u64 head = scr + kScrNodes;
    const s32 off = rd<s32>(head + 0x14);
    if (off < 0 || off > 0x40) return;
    int i = 0;
    for (u64 link = rd<u64>(head + 8); link != head && ptr_ok(link) && i < 24; link = rd<u64>(link + 8), i++) {
        const u64 node = link - (u64)off, btn = rd<u64>(node + kNodeButton);
        const u64 pane = ptr_ok(btn) ? rd<u64>(btn + kBtnHitPane) : 0;
        if (!ptr_ok(pane)) { printf("[menu-cursor]   node %d %#llx: button %#llx, no hit pane\n", i, (unsigned long long)node, (unsigned long long)btn); continue; }
        const float w = rd<float>(pane + kPaneSize), h = rd<float>(pane + kPaneSize + 4);
        const float m00 = rd<float>(pane + kPaneMtx), m03 = rd<float>(pane + kPaneMtx + 0xc);
        const float m11 = rd<float>(pane + kPaneMtx + 0x14), m13 = rd<float>(pane + kPaneMtx + 0x1c);
        printf("[menu-cursor]   node %d %#llx: flags %#x, pane at %.0f,%.0f half %.0f x %.0f (scale %.2f %.2f, base %#x) %s\n", i,
               (unsigned long long)node, rd<u32>(btn + kBtnFlags), m03, m13, fabsf(w * m00) / 2, fabsf(h * m11) / 2, m00, m11,
               rd<u8>(pane + kPaneBase), pane_shown(pane) ? "shown" : "hidden");
        const u64 blyt = rd<u64>(btn + kBtnLayout), root = ptr_ok(blyt) ? rd<u64>(blyt + kLytRoot) : 0;
        const u64 cur = rd<u64>(btn + kBtnCursorPane);
        for (u64 q : {root, cur})
            if (ptr_ok(q))
                printf("[menu-cursor]     %s pane at %.0f,%.0f half %.0f x %.0f %s\n", q == root ? "button" : "cursor",
                       rd<float>(q + kPaneMtx + 0xc), rd<float>(q + kPaneMtx + 0x1c),
                       fabsf(rd<float>(q + kPaneSize) * rd<float>(q + kPaneMtx)) / 2,
                       fabsf(rd<float>(q + kPaneSize + 4) * rd<float>(q + kPaneMtx + 0x14)) / 2, pane_shown(q) ? "shown" : "hidden");
    }
}

// The screen's layout name, for the log
void layout_name(u64 scr, char* out, size_t n) {
    snprintf(out, n, "?");
    const u64 lyt = rd<u64>(scr + kScrLayout), name = ptr_ok(lyt) ? rd<u64>(lyt + kLytName) : 0;
    if (name < 0x10000 || name >= (1ull << 47)) return;
    memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(name)), n - 1);
    out[n - 1] = 0;
    for (char* q = out; *q; q++) if (*q < 0x20 || *q > 0x7e) { *q = 0; break; }
}

// The pause menu's tab icons: not focus items but 9 parts panes Pa_Category_00..08 of the Pa_SlideList_00 parts,
// one game::ui::PaneScrollElement each. UIPouchScreen +0x1e0 -> the slide list parts screen, +0x58 its 9 elements
// (index = the tab: Armor, Bow, Shield, Weapon, Material, Food, SpecialParts, KeyItem, System), each +0x20 the
// icon's parts layout, whose root pane (nn::ui2d::Layout +0x18) is the Pa_Category pane. The current tab: PouchMgr
// (through the GOT entry main+0x455db58) +0x18c.
constexpr u64 kPouchSlideList = 0x1e0, kSlideElements = 0x58, kElemLayout = 0x20;
constexpr u64 kPouchMgrGot = totk_main(0x455db58), kPouchActiveTab = 0x18c;
bool is_pouch(u64 scr) {   // (cached by the screen and its layout: a freed screen's address can come back as another)
    static u64 yes, yes_lyt, no, no_lyt;
    const u64 lyt = rd<u64>(scr + kScrLayout);
    if (scr == yes && lyt == yes_lyt) return true;
    if (scr == no && lyt == no_lyt) return false;
    char nm[16];
    layout_name(scr, nm, sizeof nm);
    const bool p = !strcmp(nm, "Pouch_00");
    if (p) yes = scr, yes_lyt = lyt;
    else no = scr, no_lyt = lyt;
    return p;
}
bool is_options(u64 scr) {   // System > Options (the same cache as is_pouch)
    static u64 yes, yes_lyt, no, no_lyt;
    const u64 lyt = rd<u64>(scr + kScrLayout);
    if (scr == yes && lyt == yes_lyt) return true;
    if (scr == no && lyt == no_lyt) return false;
    char nm[24];
    layout_name(scr, nm, sizeof nm);
    const bool p = !strcmp(nm, "SystemOption_00");
    if (p) yes = scr, yes_lyt = lyt;
    else no = scr, no_lyt = lyt;
    return p;
}
// Options' list is drawn inside S_Scissor_00 (SystemOption_00.bflyt, a 1920 x 1080 layout, y up: -470.. 470) under the
// masks P_MaskUp_00 (435.5 and up) and P_MaskBottom_00 (-400 and down): a row is whole on screen inside this band. A
// hover on a row the band cuts would move the focus there, and the game scrolls the list to show its focus: moving the
// pointer along the edge scrolled it, and that fought the wheel. So a hover leaves such a row alone; a click still
// takes it (and the game scrolls it in). (kOptListTop / kOptListBottom: above scroll_target, which uses them too)
bool options_row_cut(u64 node) {
    const u64 btn = rd<u64>(node + kNodeButton), pane = ptr_ok(btn) ? rd<u64>(btn + kBtnHitPane) : 0;
    if (!ptr_ok(pane)) return false;
    float cx, cy, hw, hh;
    pane_box(pane, &cx, &cy, &hw, &hh);
    return cy + hh > kOptListTop + 1 || cy - hh < kOptListBottom - 1;
}
// A click on a selection row's < or >: the node whose row has an arrow pane under (x, y) (layout coordinates), its
// direction -1 / 1, else 0. The arrow panes are the row's OptionSelect's (totk_options_menu.cpp, Pa_ArrowL_00 /
// Pa_ArrowR_00 of PaOptionBtnLevel_00); every node is asked, an arrow may lie outside its row's hit pane.
int options_arrow_at(u64 scr, float x, float y, u64* at) {
    int n = 0, dir = 0;
    for_nodes(scr, &n, [&](u64 node, u64 btn) {
        uint64_t prev = 0, next = 0;
        if (!totk_options_row_arrows(rd<u64>(btn + kBtnLayout), &prev, &next)) return true;
        if (ptr_ok(prev) && pane_shown(prev) && pane_hit(prev, x, y)) dir = -1;
        else if (ptr_ok(next) && pane_shown(next) && pane_hit(next, x, y)) dir = 1;
        if (dir) *at = node;
        return !dir;
    });
    return dir;
}
// The tab whose icon is under (x, y) (the pouch screen's layout coordinates), -1 if none. The icons' parts panes
// are 316 wide, 80 apart (mc_tabs1: they overlap), so the nearest centre wins, within half the spacing.
int tab_at(u64 pouch, float x, float y) {
    const u64 slide = rd<u64>(pouch + kPouchSlideList);
    if (!ptr_ok(slide)) return -1;
    float cx[9], cy[9], hh[9];
    bool ok[9];
    for (int i = 0; i < 9; i++) {
        const u64 el = rd<u64>(slide + kSlideElements + 8ull * i), lyt = ptr_ok(el) ? rd<u64>(el + kElemLayout) : 0;
        const u64 pane = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
        ok[i] = ptr_ok(pane) && pane_shown(pane);
        if (!ok[i]) continue;
        float hw;
        pane_box(pane, &cx[i], &cy[i], &hw, &hh[i]);   // (the base position corrected, as pane_hit does)
    }
    int best = -1;
    for (int i = 0; i < 9; i++) {
        if (!ok[i] || fabsf(y - cy[i]) > hh[i]) continue;
        float half = 60;   // half the spacing to the nearest other icon (at most 60)
        for (int j = 0; j < 9; j++)
            if (j != i && ok[j]) half = fminf(half, fabsf(cx[j] - cx[i]) / 2);
        if (fabsf(x - cx[i]) <= half && (best < 0 || fabsf(x - cx[i]) < fabsf(x - cx[best]))) best = i;
    }
    return best;
}
// Tests (verbose): the tab icons' boxes, once per pouch screen
void dump_tabs(u64 pouch) {
    static u64 done;
    if (done == pouch) return;
    done = pouch;
    const u64 slide = rd<u64>(pouch + kPouchSlideList);
    printf("[menu-cursor]   pouch tabs (slide list %#llx):\n", (unsigned long long)slide);
    if (!ptr_ok(slide)) return;
    for (int i = 0; i < 9; i++) {
        const u64 el = rd<u64>(slide + kSlideElements + 8ull * i), lyt = ptr_ok(el) ? rd<u64>(el + kElemLayout) : 0;
        const u64 pane = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
        if (!ptr_ok(pane)) { printf("[menu-cursor]   tab %d: no pane\n", i); continue; }
        printf("[menu-cursor]   tab %d: at %.0f,%.0f half %.0f x %.0f %s\n", i, rd<float>(pane + kPaneMtx + 0xc),
               rd<float>(pane + kPaneMtx + 0x1c), fabsf(rd<float>(pane + kPaneSize) * rd<float>(pane + kPaneMtx)) / 2,
               fabsf(rd<float>(pane + kPaneSize + 4) * rd<float>(pane + kPaneMtx + 0x14)) / 2,
               pane_shown(pane) ? "shown" : "hidden");
    }
}
// The stored current tab (PouchMgr::getActiveTab also skips invalid tabs upward; a press moves to a valid one), -1
// if PouchMgr is not there
int active_tab() {
    const u64 got = rd<u64>(kPouchMgrGot);
    if (!ptr_ok(got)) return -1;
    u64 mgr = rd<u64>(got);
    if (!game_object(mgr)) mgr = game_object(got) ? got : 0;
    if (!mgr) return -1;
    const s32 t = rd<s32>(mgr + kPouchActiveTab);
    return t >= 0 && t <= 8 ? t : -1;
}

// The pouch's item popup (the action window) is open: UIPouchScreen +0x1f0 mpPa_ActionWindow_00 -> its layout
// (+0x20) -> LayoutEx +0x91 mAnimState non-zero, the game's own test
bool pouch_popup_open(u64 pouch) {
    const u64 win = rd<u64>(pouch + 0x1f0), lyt = ptr_ok(win) ? rd<u64>(win + 0x20) : 0;
    return ptr_ok(lyt) && rd<u8>(lyt + 0x91) != 0;
}

// ---- the menu screen in front (totk_menu_cursor.h), for the context
std::atomic<uint32_t> g_menu_screen{TMS_NONE | 0xff00u};
// A layout's screen. The names come from the boots' logs (focus screens, the census below); a focus screen not
// listed is TMS_OTHER.
struct NamedScreen { const char* layout; TotkMenuScreen screen; };
const NamedScreen kNamed[] = {
    {"Title_00", TMS_TITLE},           {"SystemLoadList_00", TMS_SAVE_LIST}, {"SystemSaveLoad_00", TMS_SAVE_LIST},
    {"Pouch_00", TMS_PAUSE},           {"SystemOption_00", TMS_OPTIONS},     {"MessageDialog_00", TMS_CHOICE},
    {"AppAlbum_00", TMS_PURAH_PAD},    {"AppPictureBook_00", TMS_PURAH_PAD}, {"AppMap_00", TMS_MAP},
    {"CharaDirectory_00", TMS_PURAH_PAD}, {"ChallengeLog_00", TMS_PURAH_PAD},
    {"SystemWindow_00", TMS_CHOICE},   // (the system yes / no box: the title's New Game, saving, ...; mc_title3)
    {"ShopMenu_00", TMS_SHOP},         {"DyeShopMenu_00", TMS_SHOP},         {"HouseShopMenu_00", TMS_SHOP},
};
// The Purah Pad's page (the sub value), in its tab bar's order: 0 Character Profiles, 1 Adventure Log, 2 Map, 3
// Album, 4 Hyrule Compendium; 0xff another
uint32_t pad_page(const char* nm) {
    static const char* const pages[] = {"CharaDirectory_00", "ChallengeLog_00", "AppMap_00", "AppAlbum_00",
                                        "AppPictureBook_00"};
    for (uint32_t i = 0; i < 5; i++) if (!strcmp(nm, pages[i])) return i;
    return 0xff;
}
TotkMenuScreen named(const char* nm) {
    for (const NamedScreen& n : kNamed) if (!strcmp(n.layout, nm)) return n.screen;
    return TMS_OTHER;
}
// The open screens (ScreenMgr.mScreenArray: count +0x8, pointers +0x10), scanned at every read for the flags below,
// their layout names logged whenever the set changes (at most every 250 ms, 200 lines): how the screens without a
// focus box (the talk window, the map, the Purah Pad's pages) are found and named The Purah Pad from the open screens
// (mc_pad1): its frame (AppMenuOverlay_00) is open, and the map page (AppMap_00, also the gameplay minimap: it counts
// only inside the frame). Refreshed by the census at every read. The camera (CameraPointer_00) opens inside the same
// frame with no page: it is gameplay with the Pad's camera, not a Pad page, so it reads as TMS_CAMERA (the router's
// PCX_CAMERA); a page opened from it (the Album) is the Pad again. The Scope opens the frame over AppMap_00 with
// CameraPointer_00 shown and the HUD up, without the menu's AppMenuBG_00: TMS_SCOPE, not the map page (4.18 (b)).
std::atomic<bool> g_pad_open{false}, g_pad_map{false}, g_pad_camera{false}, g_pad_scope{false};
std::atomic<int> g_pad_page{-1};   // the Pad page the census saw open and shown (pad_page), -1 none, -2 a slide
// The talk window: MessageDialog_00 open (with MessageLog_01); without a focus box it is a line of text, with one its
// choices (mc_hookA2)
std::atomic<bool> g_dialog_open{false};
// The quickmenus: the L ability wheel ShortCutMainItem_00 (UIShortCutMainItemScreen) and the D-pad strips
// ShortCut_00 (UIShortCutScreen, +0x1b8 mCategory: 0 weapon, 1 shield, 2 bow and arrows, 3 material). Gameplay
// class: the cursor stays off there. g_quick: -1 none, else the TMS_QM_* sub value. A screen counts only with its
// layout's root pane shown: HUD-like screens can stay Opened while hidden.
std::atomic<int> g_quick{-1};
// The controls diagrams' screens (glyph_pass): CtrlGuide_00, SystemActionGuide_00 when Opened (scr 0: not), and
// whether the root is shown. Written by the census, read by the hook, both under g_frame_lock.
struct Guide { u64 scr; bool shown; } g_guide[3];
int quick_of(u64 scr, const char* nm) {
    const bool wheel = !strcmp(nm, "ShortCutMainItem_00"), strip = !strcmp(nm, "ShortCut_00"),
               build = !strcmp(nm, "AutoBuilder_00");
    if (!wheel && !strip && !build) return -1;
    const u64 lyt = rd<u64>(scr + kScrLayout), root = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
    if (!ptr_ok(root) || !pane_shown(root)) return -1;
    if (wheel) return TMS_QM_WHEEL;
    if (build) return TMS_QM_AUTOBUILD;
    const s32 cat = rd<s32>(scr + 0x1b8);
    return cat == 1 ? TMS_QM_SHIELD : cat == 2 ? TMS_QM_ARROW : cat == 3 ? TMS_QM_MATERIAL : TMS_QM_WEAPON;
}
void census(u64 now) {
    static u64 last_at, last_hash;
    static int lines;
    const u64 sm = rd<u64>(kScreenMgr);
    if (!game_object(sm)) return;
    const s32 n = rd<s32>(sm + 0x8);
    const u64 arr = rd<u64>(sm + 0x10);
    if (n <= 0 || n > 1024 || !ptr_ok(arr)) return;
    char list[1024];
    size_t len = 0;
    u64 h = 1469598103934665603ull;
    bool pad = false, map = false, dialog = false, camera = false, cam_open = false, menu_bg = false;
    u64 guide[3] = {};
    bool guide_shown[3] = {};
    int page = -1;
    int quick = -1;
    for (s32 i = 0; i < n && len < sizeof list - 64; i++) {
        const u64 scr = rd<u64>(arr + 8ull * i);
        if (!game_object(scr) || rd<u8>(scr + kScrState) != kScrOpened) continue;
        char nm[40];
        layout_name(scr, nm, sizeof nm);
        pad |= !strcmp(nm, "AppMenuOverlay_00");
        map |= !strcmp(nm, "AppMap_00");
        dialog |= !strcmp(nm, "MessageDialog_00");
        menu_bg |= !strcmp(nm, "AppMenuBG_00");
        if (const int gi = !strcmp(nm, "CtrlGuide_00") ? 0 : !strcmp(nm, "SystemActionGuide_00") ? 1
                         : !strcmp(nm, "Pouch_00") ? 2 : -1; gi >= 0) {
            const u64 lyt = rd<u64>(scr + kScrLayout), root = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
            guide[gi] = scr;
            guide_shown[gi] = ptr_ok(root) && pane_shown(root);
        }
        if (!strcmp(nm, "CameraPointer_00")) {   // (shown only: it may stay Opened while hidden, as the quickmenus do)
            const u64 lyt = rd<u64>(scr + kScrLayout), root = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
            cam_open = true;
            camera |= ptr_ok(root) && pane_shown(root);
        }
        if (const uint32_t pg = pad_page(nm); pg < 5 && page != -2) {
            const u64 lyt = rd<u64>(scr + kScrLayout), root = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
            // (two pages shown at once, a slide: neither, -2 -> -1 below)
            if (ptr_ok(root) && pane_shown(root)) page = page < 0 ? (int)pg : page == (int)pg ? page : -2;
        }
        if (const int q = quick_of(scr, nm); q >= 0 && (quick < 0 || q == TMS_QM_WHEEL)) quick = q;
        for (const char* c = nm; *c; c++) h = (h ^ (u8)*c) * 1099511628211ull;
        h = (h ^ ',') * 1099511628211ull;
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", len ? " " : "", nm);
    }
    g_pad_open.store(pad);
    g_pad_map.store(pad && map);
    g_pad_page.store(pad ? page : -1);   // (-2: a slide, two pages shown)
    g_pad_camera.store(pad && camera && page == -1);
    g_pad_scope.store(pad && camera && page == 2 && !menu_bg);
    // The Scope opens the Pad's frame over AppMap_00 with CameraPointer_00 and no menu background: log
    // CameraPointer_00's root against the page on every change, in the Scope and the real map
    {
        static int last = -1, cam_lines;
        const int k = (cam_open ? 1 : 0) | (camera ? 2 : 0) | (menu_bg ? 4 : 0) | (pad ? 8 : 0) | (page + 2) << 4;
        if (k != last && (cam_open || (pad && page == 2)) && cam_lines < 100) {
            cam_lines++;
            printf("[menu-cursor] camera pointer: open %d, root shown %d; pad %d, page %d, menu bg %d\n", cam_open,
                   camera, pad, page, menu_bg);
        }
        last = k;
    }
    g_dialog_open.store(dialog);
    g_quick.store(quick);
    for (int i = 0; i < 3; i++) g_guide[i] = {guide[i], guide_shown[i]};
    // (the flags at every read, about 30 screens; the log line on a change, at most every 250 ms)
    if (h == last_hash || lines >= 200 || now - last_at < 250) return;
    last_at = now;
    last_hash = h;
    lines++;
    printf("[menu-cursor] open screens: %s\n", len ? list : "(none)");
}
// Updated at every pad read (under the frame's lock), with or without the cursor. A menu's focus gap (a popup
// opening, a tab switch: no focus for a frame or two) reads TMS_TRANSITION for up to 300 ms, as long as the cursor
// stays, so the context never flicks to gameplay
#if defined(MC_ROUTER) && defined(PC_HAS_GLYPH_GAMEPLAY)
bool verbose();
// The controls diagrams. System > Basic Controls (CtrlGuide_00, also the world's controls overlay) and Special
// Controls (SystemActionGuide_00) draw each button as a font glyph in text panes inside their diagram parts:
// CtrlGuide_00's Pa_Guide_00..04 / Pa_GuideSpecial_00 (T_Icon_00 / _01), SystemActionGuide_00's Pa_ActionGuide_00
// (mc_glyph3's pane tree: a part's panes are its own children). The game writes a game code point (U+E040..E068),
// which the font remap draws in the CURRENT set: the menu's keys on that page. The pc_glyph_gameplay_cp names the
// gameplay set's glyph (U+F1xx a key, U+F0xx the pad's own). At every UI update (design A's hook, eui's thread) a
// game code point there is swapped in place: the character, the text bits' "lay out again" (+0x114 |= 4) and +0x5c's
// bit 0x10 cleared, as TextBox::SetString does; SetString itself would cut the string after the slot. The original is
// kept per pane and slot, so an epoch change (a rebind, a mode switch) maps from it again; a game rewrite shows up as
// U+E0xx again. When the game's write comes does not matter. The vptrs in this build's panes (mc_glyph3: every Pa_*
// part 0x44fcd00, every T_* text box 0x44fd0b0, and Pa_GuideSpecial_00's T_Icon_00 / T_Guide_00 0x44fc2c8, a second
// text box class; mc-ui2d.md's 0x40ea488 / 0x40e9e90 matched no pane)
constexpr u64 kVtParts = totk_main(0x44fcd00), kVtTextBox = totk_main(0x44fd0b0), kVtTextBox2 = totk_main(0x44fc2c8);
constexpr u64 kTbBuf = 0xd8, kTbCap = 0x110, kTbLen = 0x112, kTbBits = 0x114, kTbFlagEx = 0x5c;
constexpr u64 kTbRegState = 0x168;   // eui::ScalableFontTextBoxEx (the 0x44fd0b0 class): its glyph registration state
// One record per slot written: the game's original, what was written and at which epoch. A record is trusted only
// while its slot still holds what was written, and a screen's records go when the census no longer sees it
// Opened. No record (the table full): written, but not redone. (epoch: its set's; tick: the pass it was written
// on; stages: the relayouts done, TOTK_MC_GLYPH_RELAYOUT)
struct GlyphRec { u64 tb; u16 slot, orig, wrote; int set; unsigned epoch; int gi; unsigned tick; u8 stages; };
unsigned g_glyph_tick;   // glyph_pass calls
GlyphRec g_glyph_recs[96];   // eui's thread, under g_frame_lock
int g_glyph_n;
struct GlyphPass { int gi; unsigned epoch; int budget, panes, swapped; u16 first_from, first_to; int type = -1;
                   u16 force = 0; };
// A Special Controls callout that describes another context than the world draws that context's key: {the
// ActionGuide type the diagram shows, the callout} -> its set, through the pc_glyph_cp_in; every other callout
// keeps the world's. The type is the diagram part's (screen +0x1b8 -> +0x68), written before the callouts' texts on
// the focus frame. The callout is the pane name's last number + 1 (T_Icon2_01, T_Icon3_01, T_Icon3NoIn_01: callout
// 2), so every callout layout of a page maps alike.
constexpr u64 kSagDiagram = 0x1b8, kDiagramType = 0x68;
#ifdef PC_HAS_GLYPH_CP_IN
struct GlyphCtxRow { s32 type; int callout; int set; };
constexpr GlyphCtxRow kGlyphCtx[] = {
    {11, 2, PCX_SCOPE},   // The Scope and Pins (UseCameraPointer), 2 "Place a pin": the Scope's A, LMB (1 "Press" is V)
};
#endif
int glyph_callout(u64 pane) {
    char nm[25];
    memcpy(nm, reinterpret_cast<const void*>(static_cast<uintptr_t>(pane + kPaneName)), 24);
    nm[24] = 0;
    const char* u = strrchr(nm, '_');
    return u && u[1] >= '0' && u[1] <= '9' ? atoi(u + 1) + 1 : 0;
}
GlyphRec* glyph_rec(u64 tb, u16 slot) {
    for (int i = 0; i < g_glyph_n; i++)
        if (g_glyph_recs[i].tb == tb && g_glyph_recs[i].slot == slot) return &g_glyph_recs[i];
    return nullptr;
}
void glyph_relayout(u64 tb);
void glyph_textbox(u64 tb, GlyphPass& gp) {
    const u64 buf = rd<u64>(tb + kTbBuf);
    const u16 cap = rd<u16>(tb + kTbCap), len = rd<u16>(tb + kTbLen);
    if (!ptr_ok(buf) || len >= cap || cap > 2048) return;
    gp.panes++;
    // The set this pane draws from, and that set's epoch; a record is current only for the same set and epoch, so a
    // page whose callout maps to another set re-maps a pane the game did not rewrite
    int set = 0;   // (PCX_WORLD: pc_glyph_gameplay_cp)
#ifdef PC_HAS_GLYPH_CP_IN
    if (gp.gi == 1 && gp.type >= 0)
        for (const GlyphCtxRow& row : kGlyphCtx)
            if (row.type == gp.type && row.callout == glyph_callout(tb)) set = row.set;
    // The world's guide (gi 0, the HUD's prompt bar) draws the gameplay context in force, so riding draws Go! /
    // Soothe in HORSE's keys and gliding Close in GLIDE's (a quickmenu keeps its opener's). The System tab's
    // diagram (gi 2): totk_controls_page_set.
#ifdef PC_HAS_GLYPH_GAME_SET
    if (gp.gi == 2) {  // (the fields the page's own refresh 0x1001a84a4c reads)
        const u64 g = totk_global(0x4558440);
        const int ride = ptr_ok(g) ? rd<u8>(g + 0x67a) : -1, kind = ptr_ok(g) ? rd<s32>(g + 0x6b8) : 0;
        set = totk_controls_page_set(ride, kind, pc_glyph_game_set(), totk_player_state());
    }
    // (debounced: a context held under 100 ms, ~3 frames, is not drawn, so a 1-frame GLIDE at a jump or an AIM edge
    // does not rewrite and re-register the bar's boxes at that rate)
    if (gp.gi == 0) {
        static int shown = -1, cand = -1;
        static ULONGLONG since;
        const int now_set = pc_glyph_game_set();
        const ULONGLONG now = GetTickCount64();
        if (shown < 0) shown = cand = now_set;
#ifdef PC_HAS_GLYPH_INTENT
        // (a set a key press named, pc_glyph_intent: drawn at once; the hold is for the game's own flicker, 4.27)
        if (now_set != shown && pc_glyph_intent_active()) shown = cand = now_set;
#endif
        if (now_set != cand) cand = now_set, since = now;
        else if (cand != shown && now - since >= 100) shown = cand;
        set = shown;
    }
#endif
    const unsigned epoch = set ? pc_glyph_epoch_in(set) : gp.epoch;
#else
    const unsigned epoch = gp.epoch;
#endif
    bool dirty = false;
    for (u16 i = 0; i < len; i++) {
        // (one code unit for one: E040..E068 and F000..F1FF are BMP, never half a surrogate pair, so the length, the
        // terminator and the other slots stay; only the glyph's width changes, which the relayout bit covers)
        const u16 c = rd<u16>(buf + 2ull * i);
        GlyphRec* rec = glyph_rec(tb, i);
        u16 orig;
        if (c >= 0xe040 && c <= 0xe068) {
            if (rec && rec->orig == c && rec->wrote == c && rec->set == set && rec->epoch == epoch)
                continue;   // (left: a pad style)
            orig = c;   // the game's write (first, or again)
        } else if (rec && c == rec->wrote && (rec->set != set || rec->epoch != epoch)) {
            orig = rec->orig;   // ours, from an older epoch: map the original again
        } else {
            // TOTK_MC_GLYPH_RELAYOUT=1 (an experiment, mc_glyph8: some written key codes draw nothing in a pane that
            // draws the game's code; the lead, the bank's font / glyph cache not ready when the pane laid it out): the
            // pane is laid out again 3, 15 and 60 passes after the write, the characters untouched
            static const char* const rl = getenv("TOTK_MC_GLYPH_RELAYOUT");
            static const bool relayout = rl && !strcmp(rl, "1");
            if (relayout && rec && c == rec->wrote && c != rec->orig) {
                // (at the first visit past each age: a pane is visited only while its screen is shown and the walk is
                // not cut, so an exact age could be missed; each stage once, logged)
                const unsigned age = g_glyph_tick - rec->tick;
                const int stage = age >= 60 ? 2 : age >= 15 ? 1 : age >= 3 ? 0 : -1;
                if (stage >= 0 && !(rec->stages & (1u << stage))) {
                    rec->stages |= (u8)((1u << (stage + 1)) - 1);   // (a later stage covers the earlier ones)
                    dirty = true;
                    static int told;
                    if (told < 20 && ++told)
                        printf("[menu-cursor] glyph relayout tb %#llx slot %u U+%04X age %u\n",
                               (unsigned long long)tb, i, c, age);
                }
            }
            continue;
        }
#ifdef PC_HAS_GLYPH_CP_IN
        u16 r = set ? pc_glyph_cp_in(set, orig) : pc_glyph_gameplay_cp(orig);
#else
        u16 r = pc_glyph_gameplay_cp(orig);
#endif
        // TOTK_MC_BASIC_PAD=1: System > Basic Controls (gi 2) draws the pad's own buttons instead of keys (the package
        // fallback: a pad diagram is incomplete, not wrong). Every font keeps the original pad glyph of U+E0xx at
        // U+F0xx
        static const bool basic_pad = getenv("TOTK_MC_BASIC_PAD") && !strcmp(getenv("TOTK_MC_BASIC_PAD"), "1");
        if (basic_pad && gp.gi == 2) r = (u16)(orig - 0xe000 + 0xf000);
        if (gp.force) r = gp.force;   // (tests: TOTK_MC_GLYPH_FORCE, below)
        if (!rec && g_glyph_n < (int)(sizeof g_glyph_recs / sizeof *g_glyph_recs)) rec = &g_glyph_recs[g_glyph_n++];
        static bool told_full;
        if (!rec && !told_full) {  // (written, but not re-mapped on an epoch change)
            told_full = true;
            printf("[menu-cursor] controls diagram: the glyph record table is full (%d), later slots are not re-mapped\n",
                   g_glyph_n);
        }
        if (rec) *rec = {tb, i, orig, r, set, epoch, gp.gi, g_glyph_tick, 0};
        if (r == c) continue;   // (a pad style: the remap is right as it is)
        wr<u16>(buf + 2ull * i, r);
        dirty = true;
        if (!gp.swapped++) gp.first_from = orig, gp.first_to = r;
    }
    if (dirty) glyph_relayout(tb);
}
// A text box written in place: laid out again (as TextBox::SetString), and registered again (below)
void glyph_relayout(u64 tb) {
    wr<u16>(tb + kTbBits, (u16)(rd<u16>(tb + kTbBits) | 4));
    wr<u8>(tb + kTbFlagEx, (u8)(rd<u8>(tb + kTbFlagEx) & ~0x10));
    // A scalable glyph draws only once the box registered it in the font's texture cache: an
    // eui::ScalableFontTextBoxEx keeps its registration state at +0x168 (0 none, 1 reserved and pending in the
    // manager's list via +0x170, 2 registered), and in state 2 it registers again only when the manager's
    // generation moves. So a code written into a registered box drew only if another box of the same size had
    // cached it (mc_glyph7's blank callouts). From state 2 only (never 1), the box goes back to 0 and its next
    // Calculate registers the new codes. TOTK_MC_GLYPH_REREG=0: not. (2 -> 0 as one compare-exchange: a Calculate
    // of the box on another thread that pushed it (state 1) between a read and a write would otherwise be undone,
    // and the box pushed twice)
    static const bool rereg = !(getenv("TOTK_MC_GLYPH_REREG") && !strcmp(getenv("TOTK_MC_GLYPH_REREG"), "0"));
    u8 two = 2;
    if (rereg && rd<u64>(tb) == kVtTextBox &&
        __atomic_compare_exchange_n(reinterpret_cast<u8*>(static_cast<uintptr_t>(tb + kTbRegState)), &two, (u8)0,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        static int told;
        if (told < 20 && ++told)
            printf("[menu-cursor] glyph register again: tb %#llx (state 2 -> 0)\n", (unsigned long long)tb);
    }
}
bool pane_name_is(u64 pane, const char* prefix) {
    char nm[25];
    memcpy(nm, reinterpret_cast<const void*>(static_cast<uintptr_t>(pane + kPaneName)), 24);
    nm[24] = 0;
    return !strncmp(nm, prefix, strlen(prefix));
}
#ifdef PC_HAS_GLYPH_CP_IN
// Basic Controls' second lines (the composite item): "(Press) Crouch", "(Hold) Change Ability", "(Press) Scope" are
// callout parts of their own (PaControllerKey_00) showing the N_GuideOne* variant, a label with no icon; on the Switch
// the line above's button covers them, on PC each has its own key. The icon can't be shown there:
// PaControllerKey_00_Pattern drives N_IconGuide* / N_GuideOne*'s visibility (the romfs's bflan), so a write would be
// undone, and the label's font has no External face for a key glyph in the text. So the key goes in as text, in the
// label's own bracket: "(Ctrl) Crouch" (totk_mc_sub_label). The parts: PaSystemController_00's alignment panes stack
// their children in order, so the lower line is Pa_BtnL_01, Pa_BtnLStick_00 (A_LStick_00 holds _01 first: mc_pouch1's
// LS glyph sat in _01) and Pa_BtnRStick_01; only on the page the game titles Basic Controls (ride +0x67a == 0,
// mc-sysctl.md 2), only while the keyboard style draws keys (a pad style keeps the game's text), in on foot's keys
// (PCX_WORLD), rebinds included (pc_bind_get). The game's text is kept per box and mapped again when the epoch moves;
// TOTK_MC_SUBLABEL=0: off.
struct SubLabel { const char* part; const char* action; };
constexpr SubLabel kSubLabel[] = {
    {"Pa_BtnLStick_00", "crouch"},          // (Press) Crouch: the left stick's press
    {"Pa_BtnRStick_01", "scope"},           // (Press) Scope: the right stick's press
    {"Pa_BtnL_01", "ability_wheel"},        // (Hold) Change Ability: L held, the wheel
};
struct SubRec { u64 tb; u16 orig[64], wrote[64]; int olen, wlen; unsigned epoch; };
SubRec g_sub_recs[16];   // (3 parts, a language variant or more each) eui's thread, under g_frame_lock; a screen's go when it closes
int g_sub_n;
void sub_label_textbox(u64 tb, const char* part) {
    static const bool off = getenv("TOTK_MC_SUBLABEL") && !strcmp(getenv("TOTK_MC_SUBLABEL"), "0");
    const SubLabel* sl = nullptr;
    for (const SubLabel& s : kSubLabel)
        if (!strcmp(part, s.part)) sl = &s;
    // (the language's own variant: the box and its N_GuideOne* parent both visible)
    const u64 parent = rd<u64>(tb + kPaneParent);
    if (off || !sl || !(rd<u8>(tb + kPaneFlag) & 1) || !ptr_ok(parent) || !(rd<u8>(parent + kPaneFlag) & 1)) return;
    const u64 buf = rd<u64>(tb + kTbBuf);
    const u16 cap = rd<u16>(tb + kTbCap), len = rd<u16>(tb + kTbLen);
    if (!ptr_ok(buf) || len >= cap || cap > 2048 || len >= 64) return;
    const u64 g = totk_global(0x4558440);
    if (!ptr_ok(g) || rd<u8>(g + 0x67a) != 0) return;   // (another page, or unreadable: the game's text)
    u16 cur[64];
    for (u16 i = 0; i < len; i++) cur[i] = rd<u16>(buf + 2ull * i);
    const unsigned epoch = pc_glyph_epoch_in(PCX_WORLD);
    SubRec* rec = nullptr;
    for (int i = 0; i < g_sub_n; i++)
        if (g_sub_recs[i].tb == tb) rec = &g_sub_recs[i];
    const bool ours = rec && rec->wlen == len && !memcmp(cur, rec->wrote, 2ull * len);
    if (ours && rec->epoch == epoch) return;
    if (!rec) {
        if (g_sub_n == (int)(sizeof g_sub_recs / sizeof *g_sub_recs)) return;
        rec = &g_sub_recs[g_sub_n++];
    }
    if (!ours) {   // the game's text (first, or written again)
        memcpy(rec->orig, cur, 2ull * len);
        rec->olen = len;
    }
    rec->tb = tb;
    rec->epoch = epoch;
    // the key: only while keys' glyphs are drawn (U+F1xx for the A button or the left stick's press, either one
    // bound); else the game's text again
    static const bool basic_pad = getenv("TOTK_MC_BASIC_PAD") && !strcmp(getenv("TOTK_MC_BASIC_PAD"), "1");
    const u16 pa = pc_glyph_cp_in(PCX_WORLD, 0xe042), pls = pc_glyph_cp_in(PCX_WORLD, 0xe056);
    const bool keys = (pa >= 0xf100 && pa <= 0xf1ff) || (pls >= 0xf100 && pls <= 0xf1ff);
    int in[4];
    const int n = basic_pad || !keys ? 0 : pc_bind_get(PCX_WORLD, sl->action, in, 4, nullptr);
    u16 out[64];
    int w = n > 0 ? totk_mc_sub_label(rec->orig, rec->olen, pc_key_name(in[0], pc_lang_key()), out, cap < 64 ? cap : 64) : -1;
    if (w < 0) memcpy(out, rec->orig, 2ull * rec->olen), w = rec->olen;
    if (w + 1 > cap) return;
    memcpy(rec->wrote, out, 2ull * w);
    rec->wlen = w;
    if (w == len && !memcmp(out, cur, 2ull * len)) return;
    for (int i = 0; i < w; i++) wr<u16>(buf + 2ull * i, out[i]);
    wr<u16>(buf + 2ull * w, 0);
    wr<u16>(tb + kTbLen, (u16)w);
    glyph_relayout(tb);
    static int told;
    if (told < 20 && ++told) {
        char a[65], b[65];
        for (int i = 0; i <= 64; i++) a[i] = i < len ? (cur[i] >= 0x20 && cur[i] < 0x7f ? (char)cur[i] : '?') : 0;
        for (int i = 0; i <= 64; i++) b[i] = i < w ? (out[i] >= 0x20 && out[i] < 0x7f ? (char)out[i] : '?') : 0;
        printf("[menu-cursor] Basic Controls: %s's '%s' -> '%s' (epoch %u)\n", part, a, b, epoch);
    }
}
#endif
// gi 0: CtrlGuide_00, its parts Pa_Guide* (Pa_Guide_00..04, Pa_GuideSpecial_00), only their T_Icon* panes; 1:
// SystemActionGuide_00, its part Pa_ActionGuide*, any text box there that holds a game code point; 2: Pouch_00 on the
// System tab: its part Pa_SystemController_00 (layout PaSystemController_00), whose callouts are Pa_Btn*_00 parts
// (PaControllerKey_00) with the glyph in T_IconL_00 / T_IconR_00 (the romfs's layouts, Common.Product.100 blarc), so
// T_Icon* only; the footer hints are outside it
int g_glyph_diag;   // TOTK_MC_GLYPH_DIAG=1: each diagram screen's text boxes, twice per open (lines left)
void glyph_walk(u64 pane, int gi, bool in_part, int depth, GlyphPass& gp, const char* part = "") {
    if (!ptr_ok(pane) || depth > 16 || --gp.budget < 0) return;
    const u64 vt = rd<u64>(pane);
    const bool text = vt == kVtTextBox || vt == kVtTextBox2;
    char pn[25];   // (the nearest part's name, for the dump)
    if (vt == kVtParts) {
        memcpy(pn, reinterpret_cast<const void*>(static_cast<uintptr_t>(pane + kPaneName)), 24);
        pn[24] = 0;
        part = pn;
        static const char* const kPart[3] = {"Pa_Guide", "Pa_ActionGuide", "Pa_SystemController"};
        if (gi < 3 && pane_name_is(pane, kPart[gi])) in_part = true;
    }
    // (name, where, length and the first code units: every text box on the dump's passes; besides, with the dump on,
    // each text box as it gets a fresh game code point, so every Special Controls entry's callouts show as it opens)
    static int fresh_lines;
    bool fresh = false;
    if (text && g_glyph_diag == 0 && fresh_lines < 200) {
        static const bool diag_on = getenv("TOTK_MC_GLYPH_DIAG") && atoi(getenv("TOTK_MC_GLYPH_DIAG")) > 0;
        const u64 buf = rd<u64>(pane + kTbBuf);
        const u16 cap = rd<u16>(pane + kTbCap), len = rd<u16>(pane + kTbLen);
        for (u16 i = 0; diag_on && ptr_ok(buf) && len < cap && i < len && i < 64 && !fresh; i++) {
            const u16 c = rd<u16>(buf + 2ull * i);
            const GlyphRec* rec = glyph_rec(pane, i);
            fresh = gi == 3 ? (c >= 0xe040 && c <= 0xe068) || (c >= 0xf000 && c <= 0xf1ff)
                            : c >= 0xe040 && c <= 0xe068 && !(rec && rec->wrote == c && rec->orig == c);
        }
        if (fresh && gi >= 2) {   // (Pouch_00's many text boxes outside the diagram, a dumped screen's: each once)
            static u64 seen[256];
            static int nseen;
            for (int i = 0; i < nseen && fresh; i++) fresh = seen[i] != pane;
            if (fresh && nseen < 256) seen[nseen++] = pane;
        }
        if (fresh) fresh_lines++;
    }
    if (text && (g_glyph_diag > 0 || fresh)) {
        if (g_glyph_diag > 0) g_glyph_diag--;
        char nm[25];
        memcpy(nm, reinterpret_cast<const void*>(static_cast<uintptr_t>(pane + kPaneName)), 24);
        nm[24] = 0;
        const u64 buf = rd<u64>(pane + kTbBuf);
        const u16 cap = rd<u16>(pane + kTbCap), len = rd<u16>(pane + kTbLen);
        printf("[menu-cursor]   text %s (vt main+%#llx, depth %d, part %s%s, shown %d): len %u cap %u:", nm,
               (unsigned long long)(vt - kMain), depth, *part ? part : "-", in_part ? ", in the part" : "",
               pane_shown(pane), len, cap);
        for (u16 i = 0; ptr_ok(buf) && i < len && i < 12 && len < cap; i++) printf(" %04X", rd<u16>(buf + 2ull * i));
        printf("\n");
    }
    if (in_part && text && (gi == 1 || pane_name_is(pane, "T_Icon"))) glyph_textbox(pane, gp);
#ifdef PC_HAS_GLYPH_CP_IN
    else if (in_part && text && gi == 2 && pane_name_is(pane, "T_GuideOne")) sub_label_textbox(pane, part);
#endif
    for (u64 n = rd<u64>(pane + kPaneChildren + 8); ptr_ok(n) && n != pane + kPaneChildren && gp.budget > 0;
         n = rd<u64>(n + 8))
        glyph_walk(n - 8, gi, in_part, depth + 1, gp, part);
}
void glyph_pass() {
    // TOTK_MC_GLYPH=0: the pass off (it writes the diagrams' text boxes in gameplay too, CtrlGuide_00: the fight-crash
    // A/B)
    static const bool off = getenv("TOTK_MC_GLYPH") && !strcmp(getenv("TOTK_MC_GLYPH"), "0");
    if (off) return;
    g_glyph_tick++;
    static int lines;
    static unsigned logged_epoch = ~0u;
    static int logged[3] = {-1, -1, -1};
    // (a screen gone: its records go, kept order irrelevant)
    for (int i = 0; i < g_glyph_n;)
        if (!g_guide[g_glyph_recs[i].gi].scr) g_glyph_recs[i] = g_glyph_recs[--g_glyph_n]; else i++;
    static const bool diag = getenv("TOTK_MC_GLYPH_DIAG") && atoi(getenv("TOTK_MC_GLYPH_DIAG")) > 0;
    static int passes[3];  // (passes since the screen opened: the dump at the 1st and the 30th)
    static const char* const kGuideName[3] = {"CtrlGuide_00", "SystemActionGuide_00", "Pouch_00 (System)"};
    for (int gi = 0; gi < 3; gi++) if (!g_guide[gi].scr) logged[gi] = -1, passes[gi] = 0;
#ifdef PC_HAS_GLYPH_CP_IN
    if (!g_guide[2].scr) g_sub_n = 0;
#endif
    // Tests: TOTK_MC_GLYPH_DIAG_SCREEN=<layout>[,<layout>...] or * (every open screen, every 30th pass), with
    // TOTK_MC_GLYPH_DIAG=1: those screens are walked read-only and each text box holding a button code (the game's
    // U+E040..E068, the pad's U+F0xx, a bank key U+F1xx) is logged once with its part, to find which panes a screen's
    // hints are in. Nothing is written there.
    if (static const char* const ds = getenv("TOTK_MC_GLYPH_DIAG_SCREEN"); ds && *ds && diag) {
        static unsigned n;
        const bool all = !strcmp(ds, "*");
        if (!all || n++ % 30 == 0) {
            const u64 sm = rd<u64>(kScreenMgr);
            const s32 cnt = game_object(sm) ? rd<s32>(sm + 0x8) : 0;
            const u64 arr = game_object(sm) ? rd<u64>(sm + 0x10) : 0;
            for (s32 i = 0; i < cnt && i < 1024 && ptr_ok(arr); i++) {
                const u64 scr = rd<u64>(arr + 8ull * i);
                if (!game_object(scr) || rd<u8>(scr + kScrState) != kScrOpened) continue;
                char nm[40];
                layout_name(scr, nm, sizeof nm);
                bool want = all;
                for (const char* q = ds; !want && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") {
                    const size_t l = strchr(q, ',') ? (size_t)(strchr(q, ',') - q) : strlen(q);
                    want = l == strlen(nm) && !strncmp(q, nm, l);
                }
                const u64 lyt = rd<u64>(scr + kScrLayout);
                if (!want || !ptr_ok(lyt)) continue;
                GlyphPass gp{3, 0, 12000, 0, 0, 0, 0};
                glyph_walk(rd<u64>(lyt + kLytRoot), 3, false, 0, gp, nm);
            }
        }
    }
    // (Pouch_00 only on its System tab, where the diagram shows: the walk is not paid on the item tabs)
    const bool pouch = g_guide[2].scr && active_tab() == 8;
    if (!g_guide[0].scr && !g_guide[1].scr && !pouch) return;
    const unsigned e = pc_glyph_gameplay_epoch();
    for (int gi = 0; gi < 3; gi++) {
        if (!g_guide[gi].scr || !g_guide[gi].shown || (gi == 2 && !pouch)) continue;
        const u64 lyt = rd<u64>(g_guide[gi].scr + kScrLayout);
        if (diag && gi < 2 && (++passes[gi] == 1 || passes[gi] == 30)) {   // (Pouch_00: its fresh lines only)
            g_glyph_diag = 300;
            printf("[menu-cursor] controls diagram %s: its text boxes, pass %d since open\n", kGuideName[gi],
                   passes[gi]);
        }
        GlyphPass gp{gi, e, gi == 2 ? 12000 : 4000, 0, 0, 0, 0};
        // Tests: TOTK_MC_GLYPH_FORCE=<hex cp>,<hex cp>,...: the k-th time Special Controls opens, every pane the pass
        // writes there gets the k-th code instead of the lookup's
        if (gi == 1) {
            static u16 force[8];
            static int nforce = -1, opens;
            static unsigned seen_tick;   // (a gap in the UI updates seen: the screen was closed in between)
            if (nforce < 0) {
                nforce = 0;
                if (const char* f = getenv("TOTK_MC_GLYPH_FORCE"))
                    for (const char* q = f; *q && nforce < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : "")
                        force[nforce++] = (u16)strtoul(q, nullptr, 16);
            }
            const bool fresh_open = !seen_tick || g_glyph_tick - seen_tick > 1;
            seen_tick = g_glyph_tick;
            if (fresh_open) {
                opens++;
                if (nforce && verbose())
                    printf("[menu-cursor] controls diagram SystemActionGuide_00: open %d forces U+%04X\n", opens,
                           force[opens - 1 < nforce ? opens - 1 : nforce - 1]);
            }
            if (nforce) gp.force = force[opens - 1 < nforce ? opens - 1 : nforce - 1];
        }
        if (gi == 1) {   // (the entry Special Controls shows: its ActionGuide type, logged as it changes)
            const u64 diagram = rd<u64>(g_guide[gi].scr + kSagDiagram);
            gp.type = ptr_ok(diagram) ? rd<s32>(diagram + kDiagramType) : -1;
            static s32 told = -2;
            if (gp.type != told && verbose() && lines < 60) {
                told = gp.type;
                lines++;
                printf("[menu-cursor] controls diagram SystemActionGuide_00: ActionGuide type %d\n", gp.type);
            }
        }
        if (ptr_ok(lyt)) glyph_walk(rd<u64>(lyt + kLytRoot), gi, false, 0, gp);
        g_glyph_diag = 0;   // (the dump: this pass only)
        // (one line per page when its icon count or the epoch changes, and on a pass that swapped: at most 60)
        if ((gp.panes != logged[gi] || e != logged_epoch || gp.swapped) && lines < 60) {
            lines++;
            logged[gi] = gp.panes;
            logged_epoch = e;
            printf("[menu-cursor] controls diagram %s: %d icon panes, %d glyphs swapped (first U+%04X -> U+%04X), "
                   "epoch %u%s\n", kGuideName[gi], gp.panes, gp.swapped, gp.first_from, gp.first_to, e,
                   gp.budget < 0 ? ", walk cut (budget)" : "");
        }
    }
}
#endif

// Stores the value and pushes it to the router; logs each change (at most 200 lines), as the router's own pc_ctx line
// shows only its decision, not which input made it (mc_cam1: the camera came from the player read first)
void publish_screen(uint32_t v) {
    static uint32_t last = ~0u;
    static int lines;
    g_menu_screen.store(v, std::memory_order_relaxed);
    if (v != last && lines < 200) {
        lines++;
        printf("[menu-cursor] menu screen %u sub %u\n", v & 0xff, v >> 8 & 0xff);
    }
    last = v;
#ifdef MC_ROUTER
    pc_ctx_ui(v & 0xff, v >> 8 & 0xff);
#endif
}
u64 open_screen(const char* name);
// The recipe book: RecipeCard_00 open with its cards or its history part shown, -1 if not. UIRecipeCardScreen: +0x1a0
// the cards' PartsScreen, +0x1a8 the history's (PaRecipeLogSlideList_00); a part's LayoutEx at +0x20. Its close hides
// the part's root pane (flag bit 0, 0x1001ac8d68), so a shown root is that part up. Its update (0x1001ac8e30): in the
// Food tab the UI controller's Y opens the history, B there goes back to the cards.
[[maybe_unused]] int recipe_state() {
    const u64 scr = open_screen("RecipeCard_00");
    if (!scr) return -1;
    auto part_up = [&](u64 off) {
        const u64 part = rd<u64>(scr + off), lyt = ptr_ok(part) ? rd<u64>(part + 0x20) : 0,
                  root = ptr_ok(lyt) ? rd<u64>(lyt + kLytRoot) : 0;
        return ptr_ok(root) && (rd<u8>(root + kPaneFlag) & 1) && rd<u8>(root + kPaneAlpha);
    };
    if (part_up(0x1a8)) return TMS_RECIPE_HISTORY;
    return part_up(0x1a0) ? TMS_RECIPE_CARDS : -1;
}
void update_menu_screen(const u64 focus_screen) {
    static ULONGLONG menu_seen;
    const ULONGLONG now = GetTickCount64();
    // A quickmenu: the focused screen itself, or, with no menu focused (a quickmenu opens from gameplay), one
    // the census saw shown
    int q = -1;
    if (focus_screen) {
        char nm[40];
        layout_name(focus_screen, nm, sizeof nm);
        q = quick_of(focus_screen, nm);
    } else {
        q = g_quick.load();
    }
    if (q >= 0) {
        menu_seen = 0;
        publish_screen(TMS_QUICKMENU | (uint32_t)q << 8);
        return;
    }
    // The Scope is gameplay class too: a focus box in it (AppMap_00's) must not start a menu's focus gap, or its close
    // would read TRANSITION for 300 ms. Not taken while it is up, so the close goes straight to NONE.
    const bool scope = g_pad_scope.load();
    if (scope) menu_seen = 0;
    if (!focus_screen && menu_seen && now - menu_seen < 300) {
        publish_screen(TMS_TRANSITION | 0xff00u);
        return;
    }
    if (focus_screen && !scope) menu_seen = now;
#ifdef PC_HAS_PCX_RECIPE
    // the recipe book over the pause menu, whichever of its parts or the pouch has the focus box (its own context)
    if (const int r = recipe_state(); r >= 0) {
        menu_seen = now;
        publish_screen(TMS_RECIPE | (uint32_t)r << 8);
        return;
    }
#endif
    uint32_t v = TMS_NONE | 0xff00u;
    if (focus_screen) {
        char nm[40];
        layout_name(focus_screen, nm, sizeof nm);
        TotkMenuScreen sc = named(nm);
        uint32_t sub = 0xff;
        if (sc == TMS_PAUSE) { const int t = active_tab(); if (t >= 0) sub = (uint32_t)t; }
        if (sc == TMS_PURAH_PAD || sc == TMS_MAP) sub = g_pad_page.load() == -2 ? 0xff : pad_page(nm);   // (a slide: unknown)
        else if (sc == TMS_OTHER && g_pad_open.load()) sc = TMS_PURAH_PAD;   // another page with a focus box
        // AppMap_00 takes a focus box: under the Scope's census it is still the Scope; the map opened from inside it
        // has AppMenuBG_00, so g_pad_scope is off there
        if (sc == TMS_MAP && g_pad_scope.load()) { sc = TMS_SCOPE; sub = 0xff; }
        v = (uint32_t)(sc == TMS_PAUSE && pouch_popup_open(focus_screen) ? TMS_ITEM_POPUP : sc) | sub << 8;
        // A sub-screen over a menu's top level (TOTK_MENU_F_SUB, for the router's menu hotkeys: Esc goes back one
        // level there, and jumps to System only at a top level): the pause menu or the Purah Pad is open and the
        // focus is not its own page (the pouch's tab, a Pad page, the map), e.g. an item's popup, Options, the save
        // list, a yes / no choice, Special Controls
        const uint32_t kind = v & 0xff;
        const bool top = kind == TMS_PAUSE || kind == TMS_PURAH_PAD || kind == TMS_MAP || kind == TMS_SCOPE;
        if (!top && (g_pad_open.load() || open_screen("Pouch_00"))) v |= TOTK_MENU_F_SUB;
    } else if (g_dialog_open.load()) {
        v = TMS_DIALOGUE | 0xff00u;         // a line of the talk window (its choices have a focus box: TMS_CHOICE)
    } else if (g_pad_camera.load()) {  // the Pad's camera: gameplay class (the PCX_CAMERA, 8e9df44)
        v = TMS_CAMERA | 0xff00u;
    } else if (g_pad_scope.load()) {    // the Scope, before the Pad branch would name it the map (4.18 (b))
        v = TMS_SCOPE | 0xff00u;
    } else if (g_pad_open.load()) {  // a Purah Pad page without a focus box: named from the census
        const int pg = g_pad_page.load();
        v = pg == 2 || (pg == -1 && g_pad_map.load()) ? (TMS_MAP | 2u << 8)
          : pg >= 0 ? (TMS_PURAH_PAD | (uint32_t)pg << 8) : (TMS_PURAH_PAD | 0xff00u);
    }
    publish_screen(v);
}

// Design A: eui's thread runs the cursor through a hook on UIController::updateState when the key has it.
// g_in_hook: this call is that hook's (guest calls allowed); g_hook_at: when it last ran.
thread_local bool g_in_hook;
std::atomic<ULONGLONG> g_hook_at{0};
std::atomic<u64> g_pad_walk{0};            // the hook's Purah Pad walk: pad L / R bits for the pad-read path
std::atomic<uint32_t> g_ext_hold{0};       // totk_menu_pad_hold's pad buttons (the Options sections' B / A)
std::atomic<uint32_t> g_arrow_pad{0};      // a click on an Options row's < / >: D-pad left / right for 100 ms
ULONGLONG g_arrow_until;                   // ... until then (ms; frame() ends it, whatever path it takes)
bool hook_alive(ULONGLONG now) { const ULONGLONG h = g_hook_at.load(); return h && now - h < 250; }
// The inventory model's drag (totk_menu_cursor_rstick): the right stick's x while it is live
std::atomic<int32_t> g_drag_rx{0};
std::atomic<bool> g_drag_live{false};
// The construct's attach screen (totk_attach_screen_open): AttachTargetSelectCommand_00 Opened, set each UI update
std::atomic<int> g_attach_open{0};
GUEST_FN(MoveBoxCursor, 1002230100, void(u64 box_cursor_mgr, u64 node));   // eui::BoxCursorMgr::moveBoxCursor
// The D-pad strips' selection: game::ui::UIShortCutSlideListPartsScreen::setPage(parts, index), the jump the game
// itself makes to land on the equipped item, and eui::SlideListControl::playSlideSound
GUEST_FN(StripSetPage, 1001a829fc, void(u64 parts, u32 index));
GUEST_FN(StripSlideSound, 10019eaaf8, void(u64 ctrl, u32 next));
// The Autobuild list's selection: game::specialpower:: GetActiveAutoBuilderModeAndIndex(s32* index, s32* mode) (bit 0:
// a draft is active), and the screen's own select (old mode, old index, new mode, new index, flags), which
// UIAutoBuilderScreen::updateImpl_ calls on an up / down with flags 0: it lights the new row and unlights the old,
// sets the active draft, loads its blueprint and plays the move sound; the scroll target (+0x254) is the caller's
// (updateImpl_ writes getDraftSlotHeight's value just before)
GUEST_FN(AbActiveDraft, 10017333b0, u64(u64 index, u64 mode));
GUEST_FN(AbSelect, 1001a90b2c, void(u64 screen, s32 old_mode, s32 old_index, s32 new_mode, s32 new_index,
                                           u32 flags));

// The n-th (0-based) usable node in on-screen order, top to bottom (by its pane's centre; the pane kind as node_at
// chooses it), for choice_pick: the list order need not be the screen's
u64 nth_node(u64 scr, int n, float lw, float lh) {
    const bool stale = stale_hit_panes(scr, lw, lh, nullptr);
    u64 nodes[16];
    float ys[16], xs[16];
    int k = 0, cnt = 0;
    for_nodes(scr, &cnt, [&](u64 node, u64 btn) {
        if (~rd<u32>(btn + kBtnFlags) & kBtnUsable) return true;
        const u64 pane = stale ? root_pane(btn) : rd<u64>(btn + kBtnHitPane);
        if (!ptr_ok(pane) || !pane_shown(pane)) return true;
        float cx, cy, hw, hh;
        pane_box(pane, &cx, &cy, &hw, &hh);
        if (k < 16) nodes[k] = node, ys[k] = cy, xs[k] = cx, k++;
        return true;
    });
    auto before = [&](int a, int b) { return ys[a] > ys[b] + 1 || (fabsf(ys[a] - ys[b]) <= 1 && xs[a] < xs[b]); };
    for (int i = 1; i < k; i++)   // reading order: top to bottom (y up), then left to right
        for (int j = i; j > 0 && before(j, j - 1); j--)
            std::swap(ys[j], ys[j - 1]), std::swap(xs[j], xs[j - 1]), std::swap(nodes[j], nodes[j - 1]);
    return n >= 0 && n < k ? nodes[n] : 0;
}

// Ops from the router (pc_menu_ops_register, called on the input thread): taken here only where this file has its
// own way today; the rest (confirm, back, tab prev / next, scroll) are declined, so the router pulses the pad
// button once, until design A runs them on eui's thread A request: the op's argument, the focus screen and its
// layout when it came (it runs only on that screen), and when (it is dropped after 500 ms). Input thread ->
// pad-read frame, one slot.
struct Req { int arg = -1; u64 screen = 0, layout = 0; ULONGLONG at = 0; };
std::mutex g_req_lock;
Req g_req_tab, g_req_choice, g_req_page;
std::atomic<u64> g_focus_screen{0};   // the focus screen of the last pad read (for the requests' tag)
// The wheel in a scrolling list (scroll_target): the frame publishes which ways have a row to step to on its focus
// screen (bit 0 up, 1 down); the router's notches there are taken and summed, one row per UI update. Elsewhere, or
// with nothing to step to, PCOP_SCROLL is declined and the router pulses the d-pad as before.
std::atomic<u64> g_scroll_scr{0};
std::atomic<uint32_t> g_scroll_can{0};
std::atomic<int> g_wheel_n{0};
std::atomic<ULONGLONG> g_wheel_at{0};
#ifdef MC_ROUTER
#ifdef PC_HAS_MENU_GOTO
bool menu_goto(int arg);
#endif
bool menu_op(int op, int arg) {
    if (!enabled()) return false;   // (the cursor off: the router pulses the pad instead)
#ifdef PC_HAS_MENU_GOTO
    if (op == PCOP_MENU_GOTO) return menu_goto(arg);
#endif
    const uint32_t v = g_menu_screen.load(std::memory_order_relaxed);
    const int scr = (int)(v & 0xff);
    // Q / E in Settings: the previous / next section, as the section row's own step; with no sections, declined
    // (the router pulses L / R, as before)
    if ((op == PCOP_TAB_PREV || op == PCOP_TAB_NEXT) && scr == TMS_OPTIONS)
        return totk_options_section_step(op == PCOP_TAB_NEXT ? 1 : -1);
    if (op == PCOP_TAB_GOTO && (scr == TMS_PURAH_PAD || scr == TMS_MAP) && arg >= 0 && arg <= 4) {
        // a Purah Pad page: the pad's frame is the screen (its map page has no focus box), so no screen tag
        std::lock_guard<std::mutex> l(g_req_lock);
        g_req_page = Req{arg, 0, 0, GetTickCount64()};
        return true;
    }
    const u64 fs = g_focus_screen.load();
    if (!fs) return false;
    if (op == PCOP_SCROLL) {
        if (fs != g_scroll_scr.load() || !(g_scroll_can.load() & (arg < 0 ? 1u : 2u))) return false;
        if (GetTickCount64() - g_wheel_at.load() > 300) g_wheel_n.store(0);   // (old notches the frame never took)
        g_wheel_n.fetch_add(arg < 0 ? -1 : 1);
        g_wheel_at.store(GetTickCount64());
        return true;
    }
    Req r{arg, fs, rd<u64>(fs + kScrLayout), GetTickCount64()};
    std::lock_guard<std::mutex> l(g_req_lock);
    if (op == PCOP_TAB_GOTO && scr == TMS_PAUSE && arg >= 0 && arg <= 8) { g_req_tab = r; return true; }
    if (op == PCOP_CHOICE_PICK && scr == TMS_CHOICE && arg >= 1 && arg <= 6) { r.arg = arg - 1; g_req_choice = r; return true; }
    return false;
}
#endif
#ifdef MC_ROUTER
// The menu hotkeys' chain: the router's PCOP_MENU_GOTO sets the target (input thread), the hook steps it
std::mutex g_chain_lock;
TotkChain g_chain = [] { TotkChain c; totk_menu_chain_init(&c); return c; }();
std::atomic<uint32_t> g_chain_buttons{0};   // the chain's pad buttons (totk_menu_cursor_frame)
#ifdef PC_HAS_MENU_GOTO
bool menu_goto(int arg) {
    const uint32_t v = g_menu_screen.load(std::memory_order_relaxed);
    // (Esc's System from a sub-screen is "back one level": declined, the router's B does it)
    const int t = arg < 0 ? arg : arg & ~TOTK_MC_GOTO_NO_CLOSE;
    if (t == 8 && (v & TOTK_MENU_F_SUB)) return false;
    const bool ok = t == TOTK_MC_GOTO_CANCEL || t == TOTK_MC_GOTO_INVENTORY || t == TOTK_MC_GOTO_LAST || (t >= 0 && t <= 8) ||
                    (t >= TOTK_MC_GOTO_PAGE && t <= TOTK_MC_GOTO_PAGE + 4);
    if (!ok) return false;
    std::lock_guard<std::mutex> l(g_chain_lock);
    totk_menu_chain_goto(&g_chain, arg, GetTickCount64());
    printf("[menu-cursor] menu chain: target %#x\n", (unsigned)arg);
    return true;
}
#endif
void walk_start(int kind, int to, ULONGLONG now);
// Each UI update, on eui's thread
void chain_apply(ULONGLONG now) {
    TotkChainIn in{now, g_menu_screen.load(std::memory_order_relaxed), g_pad_open.load(), open_screen("Pouch_00") != 0};
    TotkChainOut o;
    {
        std::lock_guard<std::mutex> l(g_chain_lock);
        const int was = g_chain.target;
        o = totk_menu_chain_step(&g_chain, &in);
        if (was >= 0 && g_chain.target < 0 && verbose()) printf("[menu-cursor] menu chain: done\n");
    }
    g_chain_buttons.store(o.buttons, std::memory_order_relaxed);
    if (o.walk_to >= 0) walk_start(o.walk_kind, o.walk_to, now);
}
#endif
// A request for this read's focus screen, taken (cleared) if it is due; stale or other-screen ones are dropped
int take_req(Req& slot, u64 screen, ULONGLONG now) {
    std::lock_guard<std::mutex> l(g_req_lock);
    if (slot.arg < 0) return -1;
    const Req r = slot;
    slot = Req{};
    if (now - r.at > 500) return -1;
    if (r.screen && (r.screen != screen || !screen || rd<u64>(screen + kScrLayout) != r.layout)) return -1;
    return r.arg;
}

// ---- the quickmenus' mouse feed the router hands each pad read's raw counts and wheel notches while the context is
// QUICKMENU to qm_feed (its input thread). Applied on eui's thread by the hook (qm_apply), at UI level only:
//  - the L wheel reads only the UI controller's right stick (+0x128): a notch picks the next slot that holds an
//    ability, and the mouse's counts build a direction that picks the nearest such slot; the slot's direction is the
//    stick;
//  - the D-pad strips: every notch of an update in one jump of the strip's selection (setPage), N notches = N items;
//  - the Autobuild list reads the UI controller's up / down (bits 16 / 17): a notch is one press.
// The commit stays the game's: the quickmenu closes on its key's release (the hold carry), or on LMB (QUICKMENU's
// confirm row).
std::mutex g_qm_lock;
int g_qm_notches;
double g_qm_dx, g_qm_dy;   // the ability wheel's mouse counts (the wheel only)
// The open strip's SlideListControl is usable for the direct set (qm_apply, each UI update a strip is open): only then
// does qm_feed take a strip's notches, else they stay hle_hid's paced flicks, never dropped
std::atomic<bool> g_qm_strip_usable{false};
// TOTK_MC_QM=0: the feed off, nothing taken or written; the notches then do nothing in the wheel and the
// Autobuild list
bool qm_off() {
    static const bool off = getenv("TOTK_MC_QM") && !strcmp(getenv("TOTK_MC_QM"), "0");
    return off;
}
#ifdef MC_ROUTER
// The UI contexts' "click" 'E' row (pc_scheme.inc) is this file's: asked by the router at a left click there
// (pc_external_register, the prompt audit's runtime half). Answered while the cursor feature is on and its hook alive;
// whether the pointer is over an item at that moment is the player's aim, not a dead key (the map's click is hle_hid's)
bool mc_answers_click(int ctx, int vk) {
    return vk == VK_LBUTTON && pc_ctx_is_ui(ctx) && ctx != PCX_MAP && enabled() && hook_alive(GetTickCount64());
}
// (at static init, not at the first frame: a menu click before that would read "no part registered")
[[maybe_unused]] const bool g_click_registered = (pc_external_register("menu-cursor (its clicks)", mc_answers_click), true);
bool qm_feed(double dx, double dy, int notches, int sub) {
    if (qm_off() || !enabled() || !hook_alive(GetTickCount64()) || sub < TMS_QM_WHEEL || sub > TMS_QM_AUTOBUILD)
        return false;
    if (sub != TMS_QM_WHEEL && sub != TMS_QM_AUTOBUILD && !g_qm_strip_usable.load(std::memory_order_relaxed))
        return false;   // (a strip whose control the direct set cannot use: hle_hid's flicks step it)
    std::lock_guard<std::mutex> l(g_qm_lock);
    g_qm_notches += notches;
    if (sub == TMS_QM_WHEEL) g_qm_dx += dx, g_qm_dy += dy;   // (motion turns the ability wheel only: the rule above)
    return true;
}
#endif

// Tasks other files post to eui's thread (totk_ui_post): run in the hook, after the cursor's frame, outside its lock
struct UiTask { TotkUiTask fn; void* arg; ULONGLONG at; };
std::mutex g_task_lock;
UiTask g_tasks[16];
int g_ntasks;
// Takes the queued tasks: those under 250 ms old into run[] (ran true), older ones into drop[] (ran false: the
// poster frees arg and falls back). Under the queue's lock; the calls are made after it.
void take_tasks(ULONGLONG now, UiTask* run, int* nrun, UiTask* drop, int* ndrop) {
    std::lock_guard<std::mutex> l(g_task_lock);
    *nrun = *ndrop = 0;
    for (int i = 0; i < g_ntasks; i++) {
        if (now - g_tasks[i].at < 250) run[(*nrun)++] = g_tasks[i];
        else drop[(*ndrop)++] = g_tasks[i];
    }
    g_ntasks = 0;
}

// Confirm on node, on eui's thread: its button's own Down (AnimButton vtable +0x38), a complete press: the button
// queues it, plays it and calls the screen's doButtonDownStart_ / doButtonDownEnd_, where the screens decide. The
// BoxCursor's own decide reads the main custom controller, not the UI controller (mc-uicontroller.md 2c), so an A on
// the UI controller would not reach it. Down needs the button's flags 0x14 (active and shown); false when they are
// not set (the caller falls back to the UI controller's A). The game's own gates in front of Down: no screen has
// grabbed the cursor (BoxCursorMgr.mScreenIndex +0x14 == -1: the Pouch does during a category change), and the node
// is decidable for its draw target (BoxCursorNode vtable +0x30, isDecidable(node, target, repeat) = main+0x11cd0fc:
// the screen's state, top input layer, panes shown, no other button mid-Down). Then Down. 1: pressed; 0: not now
// (the caller keeps waiting, within its 400 ms); -1: the button cannot take Down (its flags).
int press_down(u64 node, int target) {
    const u64 btn = rd<u64>(node + kNodeButton);
    if (!ptr_ok(btn) || (rd<u32>(btn + kBtnFlags) & 0x14) != 0x14) return -1;
    const u64 sm = rd<u64>(kScreenMgr), bm = game_object(sm) ? rd<u64>(sm + kSmBoxCursorMgr) : 0;
    if (!ptr_ok(bm) || rd<s32>(bm + 0x14) != -1) return 0;
    Ctx* const c = guest_bridge::tl_ctx;
    const u64 nvt = rd<u64>(node), dec = ptr_ok(nvt) ? rd<u64>(nvt + 0x30) : 0;
    const u64 bvt = rd<u64>(btn), down = ptr_ok(bvt) ? rd<u64>(bvt + 0x38) : 0;
    auto in_main = [](u64 f) { return f >= kMain && f < kMain + 0x6000000; };
    if (!c || !in_main(dec) || !in_main(down)) return -1;
    // isDecidable must be BoxCursorNode's own (main+0x11cd0fc) or an override the boots have seen: anything else (a
    // wrong slot, an unknown override) is not called, and the UI controller's A is the fallback
    static const u64 kDecidable[] = {totk_main(0x11cd0fc)};
    bool known = false;
    for (u64 k : kDecidable) known |= dec == k;
    if (!known) {
        static u64 told[8];
        static int nt;
        bool seen = false;
        for (int i = 0; i < nt; i++) seen |= told[i] == dec;
        if (!seen && nt < 8) {
            told[nt++] = dec;
            printf("[menu-cursor] isDecidable of node vtable %#llx is main+%#llx, not the known one: no Down, UI A\n",
                   (unsigned long long)nvt, (unsigned long long)(dec - kMain));
        }
        return -1;
    }
    c->x[0] = node; c->x[1] = (u64)(u32)target; c->x[2] = 0;
    call_ind(c, dec);
    if (!(c->x[0] & 1)) return 0;
    c->x[0] = btn;
    call_ind(c, down);
    return 1;
}

// The focus to node: on eui's thread the game's own moveBoxCursor; elsewhere (no hook in this key) the three writes
// it makes. True when it wrote them (the caller then re-raises the request bit until the focus is there).
bool move_focus(const Focus& f, u64 node) {
    if (g_in_hook) {
        const u64 sm = rd<u64>(kScreenMgr), bm = game_object(sm) ? rd<u64>(sm + kSmBoxCursorMgr) : 0;
        if (ptr_ok(bm)) MoveBoxCursor(bm, node);
        return false;
    }
    wr<u64>(f.control + kCtlReserved, node);   // = BoxCursorMgr::moveBoxCursor
    wr<u64>(f.screen + kScrReserved, node);
    // (a locked OR keeps a bit eui sets at the same moment; eui's own plain read-modify-write of the byte can still
    // drop ours, so the bit is raised again until the focus is there)
    __atomic_fetch_or(reinterpret_cast<u8*>(static_cast<uintptr_t>(f.screen + kScrRequest)), (u8)4, __ATOMIC_SEQ_CST);
    return true;
}

// A tab walk: L / R presses until a tab bar shows tab `to`. kind 0: the pause menu's pouch tabs (PouchMgr's tab);
// kind 1: the Purah Pad's pages (the context's page, through a page switch's focus gap). Started by a tab icon's
// click or the router's tab_goto; run by walk_step at every frame, also with the cursor off (the map page).
struct Walk {
    int to = -1, kind = 0, phase = 0, presses = 0, last = -1, dead = 0;
    ULONGLONG at_ms = 0, t = 0, press_at = 0;
};
Walk g_walk;
int walk_cur(int kind) {
    if (kind == 0) return active_tab();
    const uint32_t v = g_menu_screen.load(std::memory_order_relaxed), sc = v & 0xff, sub = v >> 8 & 0xff;
    return (sc == TMS_PURAH_PAD || sc == TMS_MAP) && sub < 5 ? (int)sub : -1;
}
void walk_start(int kind, int to, ULONGLONG now) {
    g_walk = Walk{};
    g_walk.to = to; g_walk.kind = kind; g_walk.at_ms = now; g_walk.last = walk_cur(kind);
    printf("[menu-cursor] walk to %s %d (now %d)\n", kind ? "page" : "tab", to, g_walk.last);
}
// The walk's L / R bits for this frame (HidNpadButton L 6 / R 7). One press (50 ms, then 50 ms released: over a frame
// each at 30 fps; a tab bar steps one valid tab per press, on its trigger frame), the next as soon as the tab has
// changed. It stops at the tab, after 12 presses or 4 s, when the tab bar is gone, or when two presses in a row changed
// nothing in 400 ms each (an invalid tab: greyed, the game skips it; at a low frame rate a 50 ms press can fall between
// two samples, hence one retry). The pouch's tabs do not wrap.
u64 walk_step(const Focus& f, ULONGLONG now) {
    Walk& w = g_walk;
    if (w.to < 0) return 0;
    const int cur = walk_cur(w.kind);
    const bool there = w.kind == 0 ? is_pouch(f.screen) : g_pad_open.load();
    // (the Purah Pad's page reads -1 in a page switch's focus gap: wait, within the time limits)
    const bool unknown = cur < 0 && w.kind == 1;
    bool waiting = w.phase == 0 && w.presses > 0 && (cur == w.last || unknown);
    if (waiting && !unknown && now - w.press_at >= 400 && w.dead < 1) { w.dead++; waiting = false; }
    else if (!waiting && w.phase == 0) w.dead = 0;
    if ((cur < 0 && !unknown) || !there || now - w.at_ms > 4000 || w.presses > 12 ||
        (waiting && now - w.press_at >= (unknown ? 1000u : 400u))) {
        if (cur != w.to) printf("[menu-cursor] walk: %s %d not reached (at %d)\n", w.kind ? "page" : "tab", w.to, cur);
        w.to = -1;
        return 0;
    }
    if (cur == w.to && w.phase != 1) { w.to = -1; return 0; }
    if (w.phase == 0 && !waiting) { w.last = cur; w.phase = 1; w.t = w.press_at = now; w.presses++; }
    u64 out = 0;
    if (w.phase == 1) {
        out = w.to > w.last ? (1u << 7) : (1u << 6);
        if (now - w.t >= 50) { w.phase = 2; w.t = now; }
    } else if (w.phase == 2 && now - w.t >= 50) {
        w.phase = 0;
    }
    return out;
}

bool verbose();
// An open screen by its layout's name (0: none)
u64 open_screen(const char* name) {
    const u64 sm = rd<u64>(kScreenMgr);
    if (!game_object(sm)) return 0;
    const s32 n = rd<s32>(sm + 0x8);
    const u64 arr = rd<u64>(sm + 0x10);
    for (s32 i = 0; i < n && i < 1024 && ptr_ok(arr); i++) {
        const u64 scr = rd<u64>(arr + 8ull * i);
        if (!game_object(scr) || rd<u8>(scr + kScrState) != kScrOpened) continue;
        char nm[40];
        layout_name(scr, nm, sizeof nm);
        if (!strcmp(nm, name)) return scr;
    }
    return 0;
}
// The quickmenu feed on eui's thread, after updateState's original (ui: the UI controller). Returns the UI bits to
// press this frame (the Autobuild list's up / down).
u32 qm_apply(u64 ui, ULONGLONG now) {
    if (qm_off()) return 0;
    static int target = -1;         // the wheel's slot the notches chose (-1: none, the game's own)
    static int pending;             // steps not yet made (one at a time: 70 ms and 2 UI updates apart)
    static ULONGLONG last_step;
    static unsigned tick, step_tick, closed_tick;  // this hook's UI updates
    static int open_sub = -1;       // the quickmenu open last frame (-1: none)
    static float wx, wy;            // the wheel's direction written last (0, 0: none)
    const uint32_t v = g_menu_screen.load(std::memory_order_relaxed);
    tick++;
    int notches;
    double dx, dy;
    {
        std::lock_guard<std::mutex> l(g_qm_lock);
        notches = g_qm_notches, dx = g_qm_dx, dy = g_qm_dy;
        g_qm_notches = 0, g_qm_dx = g_qm_dy = 0;
    }
    static double mvx, mvy;         // the ability wheel's mouse direction (counts, y up)
    static bool strip_told;         // this open's "not usable" line was printed
    static int strip_age;           // UI updates since the strip opened
    // The strip's slide: the item the notches chose, the native step still to make (1: once setPage's snap is
    // consumed, 2: made, waiting for the index), its direction, the updates waited, the control it is for
    static s32 st_target = -1;
    static int st_phase, st_dir, st_wait;
    static u64 st_ctrl;
    static int ab_age;              // UI updates since the Autobuild list opened
    const bool quick = (v & 0xff) == TMS_QUICKMENU;
    const int sub = quick ? (int)(v >> 8 & 0xff) : -1;
    if (sub != open_sub) {   // a quickmenu closed, or another one opened
        if (open_sub == TMS_QM_WHEEL) closed_tick = tick;
        else wx = wy = 0;
        target = -1, pending = 0, mvx = mvy = 0, strip_told = false, strip_age = 0, ab_age = 0, st_phase = 0, st_target = -1;
        open_sub = sub;
    }
    // The wheel commits as it closes (doOutBefore, on L's release, copies the stick too): its direction is kept for 2
    // UI updates past the close, also when another quickmenu opens straight after it
    if (sub != TMS_QM_WHEEL && (wx || wy)) {
        if (tick - closed_tick < 2) {
            wr<float>(ui + 0x128, wx);
            wr<float>(ui + 0x12c, wy);
        } else wx = wy = 0;
    }
    if (!quick || sub == TMS_QM_WHEEL || sub == TMS_QM_AUTOBUILD)
        g_qm_strip_usable.store(false, std::memory_order_relaxed);
    if (!quick) return 0;
    // (a notch is one item; on the wheel Wheel Down is the next slot, as QUICKMENU's Wheel Down = weapon_next)
    static const char* const test_qm = getenv("TOTK_MC_TEST_QM");
    if (const char* t = test_qm) {   // tests: "<notches>" once per quickmenu kind, 1 s after it opened, no router
        static ULONGLONG opened;
        static int seen_sub = -1;
        if (sub != seen_sub) seen_sub = sub, opened = now;
        else if (opened && now - opened > 1000) notches += atoi(t), opened = 0;
    }
    if (sub == TMS_QM_WHEEL) {
        const u64 scr = open_screen("ShortCutMainItem_00");
        // A pad's right stick steers the wheel too: while it is out (the original copied it in), the notches' slot
        // yields and the next notch starts from the game's slot
        const float px = rd<float>(ui + 0x128), py = rd<float>(ui + 0x12c);
        if (px * px + py * py > 0.09f) target = -1, notches = 0, wx = wy = 0, mvx = mvy = 0, dx = dy = 0;
        // tests: TOTK_MC_TEST_QM_ABILITY=<wheel id, 6 Autobuild> picks its slot (mCircleAbilityMap +0x270)
        static const char* const test_ability = getenv("TOTK_MC_TEST_QM_ABILITY");
        if (const char* t = test_ability; t && verbose() && scr) {
            static bool told;
            if (!told) {
                told = true;
                printf("[menu-cursor] quickmenu wheel: abilities by slot");
                for (int i = 0; i < 8; i++) printf(" %d", rd<s32>(scr + 0x270 + 4ull * i));
                printf("\n");
            }
            for (int i = 0; i < 8; i++) if (rd<s32>(scr + 0x270 + 4ull * i) == atoi(t)) target = i;
        }
        // A notch steps from the slot chosen last, or from the game's (mCurrentCircleIndex +0x23c), past the empty
        // slots; slot i points at pi/2 - i*pi/4, so the next slot is clockwise. A slot holds its fixed id 1..8 once
        // the ability is got, else 0, so only 1..8 count. The game picks a slot only for a direction within ~22 deg
        // of it, averaged over 2 frames, and keeps the last pick otherwise (mc-wheel.md 2): the slot's exact
        // direction is held on every update, so a notch lands on the 2nd frame. At most 8 notches are played, each
        // looks at most 8 slots, and with no valid slot the notch does nothing.
        auto valid = [&](int i) { const s32 id = rd<s32>(scr + 0x270 + 4ull * i); return id >= 1 && id <= 8; };
        if (notches && scr) {
            mvx = mvy = 0;   // (a notch starts the mouse's direction afresh)
            const int dir = notches < 0 ? 1 : -1;   // (a notch up is +1: counter-clockwise)
            // The game's index is -1 at open until the equipped ability's slot is found, and can name a locked
            // slot: then the first notch lands on slot 0 going clockwise, 7 the other way
            int s = target >= 0 ? target : rd<s32>(scr + 0x23c);
            const bool from_valid = s >= 0 && s <= 7 && valid(s);
            if (!from_valid) s = dir > 0 ? 7 : 0;
            int n = notches < 0 ? -notches : notches, skipped = 0;
            bool any = false;
            // (each notch is one ability: a cycle is the number of slots that hold one, so n notches are n mod that
            // many steps, all in this update; mod 8 matched only with 1, 2, 4 or 8 abilities)
            int nvalid = 0;
            for (int i = 0; i < 8; i++) nvalid += valid(i);
            // (from no valid slot the first notch only lands on the first one, then the cycle starts: a burst of a
            // multiple of nvalid must still land on an ability)
            for (n = !nvalid ? 0 : from_valid ? n % nvalid : 1 + (n - 1) % nvalid; n > 0; n--) {
                any = false;
                for (int k = 0; k < 8 && !any; k++) {
                    s = (s + dir + 8) % 8;
                    any = valid(s);
                    skipped += !any;
                }
                if (!any) break;
            }
            static bool told_skip;
            if (skipped && !told_skip) {
                told_skip = true;
                printf("[menu-cursor] quickmenu wheel: %d empty slot(s) skipped (ids", skipped);
                for (int i = 0; i < 8; i++) printf(" %d", rd<s32>(scr + 0x270 + 4ull * i));
                printf(")\n");
            }
            if (any) target = s;
        } else if ((dx || dy) && scr) {
            // The mouse: its counts build a direction (y up), clamped to 200 counts; past 40 it picks the slot nearest
            // that direction that holds an ability (nearest first, then alternately either side), so the pick never
            // lands on a locked slot and the game's own ~22 deg window is always met
            mvx += dx, mvy -= dy;
            const double m = sqrt(mvx * mvx + mvy * mvy);
            if (m > 200) mvx *= 200 / m, mvy *= 200 / m;
            if (m > 40) {
                const double a = atan2(mvy, mvx);
                int s = ((int)lround((1.5707963 - a) / 0.78539816) % 8 + 8) % 8;
                for (int k = 0; k < 8 && !valid(s); k++) s = ((s + ((k & 1) ? -(k + 1) : (k + 1))) % 8 + 8) % 8;
                if (valid(s)) target = s;
            }
        }
        if (target >= 0) {   // the slot's direction, full deflection, on the UI controller's right stick, every update
            const double a = 1.5707963 - target * 0.78539816;
            wx = (float)cos(a), wy = (float)sin(a);
            wr<float>(ui + 0x128, wx);
            wr<float>(ui + 0x12c, wy);
        }
        if (verbose()) {   // (the slot the wheel picked: UIShortCutMainItemScreen mCurrentCircleIndex +0x23c)
            static s32 slot = -2;
            const u64 scr = open_screen("ShortCutMainItem_00");
            const s32 now_slot = scr ? rd<s32>(scr + 0x23c) : -2;
            if (now_slot != slot)
                printf("[menu-cursor] quickmenu wheel: stick (%.2f, %.2f) -> slot %d\n", wx, wy, now_slot);
            slot = now_slot;
        }
        return 0;
    }
    // One step a notch; a change of direction drops the steps still queued the old way, not played first Wheel Down (a
    // notch < 0) is the next item on every kind, as the scheme's Wheel Down = weapon_next (a right flick): the list's
    // row below, the strip's item to the right, the wheel's slot clockwise
    const int fresh = -notches;   // (down / right +)
    if (sub != TMS_QM_AUTOBUILD) {
        // The D-pad strips: the parts at the screen's +0x1a8, its SlideListControl at +0x50 (the index +0x8c, the item
        // count +0x88). N notches = N items, wrapping on the strip's own count; the release commits +0x8c
        // (onOutBefore). With the game's own slide: a single notch is the game's own step, the control's request byte
        // +0xc3 = dir << 1 with +0xbe |= 8 (what eui::Screen::update writes for a d-pad press: the slide animates and
        // its sound plays); a burst jumps at once with setPage (silent, no easing) to the item before the target, and
        // the next update, once that snap (+0x98) is consumed (a pending snap clears the frame's step: mc-strip.md 3),
        // steps the last item natively, so the eye sees the slide and the strip never lags. A step the control does not
        // make within 3 updates (a clamp, a wrap) falls back to setPage onto the target, so the index is always the
        // chosen item. The strips that show all their items (the shields', the bows': their scroll window [+0x50, +0x40
        // - +0x50 + bit 4] empty) move a box cursor, not a slide, and keep setPage; so does a step that wraps or leaves
        // that window.
        const u64 scr = open_screen("ShortCut_00");
        const u64 parts = scr ? rd<u64>(scr + 0x1a8) : 0, ctrl = ptr_ok(parts) ? rd<u64>(parts + 0x50) : 0;
        // (the strip's items are 0.. mPageCount +0x88 - 1: setPage snaps to any of them and the release commits that
        // entry. [+0x50, +0x40 - +0x50 + mFlags bit 4] is only slide_step's clamp for a page slide, the scroll window:
        // a strip that shows all its items, the shields' or the bows' (6 with a "none" slot), walks below it, mc_qm9,
        // mc-strip.md's correction to section 2)
        const s32 lo = 0, cur = ptr_ok(ctrl) ? rd<s32>(ctrl + 0x8c) : -1;
        const s32 hi = ptr_ok(ctrl) ? rd<s32>(ctrl + 0x88) - 1 : -1;
        const s32 count = hi - lo + 1;
        const bool usable = ptr_ok(ctrl) && lo >= 0 && count >= 1 && count <= 1024 && cur >= lo && cur <= hi;
        g_qm_strip_usable.store(usable, std::memory_order_relaxed);
        // (once an open, and only if still unusable on its 3rd UI update: the first frames of an open, before
        // ShortCut_00 and its parts are built, are a normal transient and must not spend the budget)
        static int told_unusable;
        if (!usable && ++strip_age >= 3 && !strip_told && told_unusable < 20 && ++told_unusable && (strip_told = true))
            printf("[menu-cursor] quickmenu strip %d: its control is not usable for the direct set (screen %#llx, "
                   "control %#llx, range %d..%d, index %d)\n", sub, (unsigned long long)scr, (unsigned long long)ctrl,
                   lo, hi, cur);
        if (usable && ctrl != st_ctrl) st_phase = 0, st_target = -1, st_ctrl = ctrl;
        auto step = [&](int dir) {   // (the game's own one-item slide)
            wr<u8>(ctrl + 0xc3, (u8)(dir << 1));
            wr<u8>(ctrl + 0xbe, (u8)(rd<u8>(ctrl + 0xbe) | 8));
        };
        auto snap = [&](const char* why) {   // (setPage onto the target, with the slide's sound)
            StripSetPage(parts, (u32)st_target);
            StripSlideSound(ctrl, st_dir ? 1u : 0u);
            st_phase = 0;
            if (verbose()) printf("[menu-cursor] quickmenu strip %d: setPage %d (%s)\n", sub, st_target, why);
        };
#ifdef PC_HAS_QM_CLOSING
        // (the strip's key let go with a step still to make: the release must equip the item shown, so the target is
        // set at once, not one UI update later. The order against the game's own onOutBefore is not guaranteed: this
        // shrinks the window from a UI update to the release's read)
        if (st_phase && usable && pc_quickmenu_closing()) snap("closing");
#endif
        if (fresh && usable) {
            const s32 base = st_phase ? st_target : cur;   // (notches on top of a pending step go on from its target)
            const s32 wlo = rd<s32>(ctrl + 0x50);
            const s32 whi = rd<s32>(ctrl + 0x40) - wlo + (s32)((rd<u32>(ctrl + 0xbc) >> 4) & 1);
            int to, pre;
            const int plan = totk_strip_plan(cur, base, fresh, count, wlo, whi, rd<s32>(ctrl + 0x98) == -1, &to, &pre);
            st_target = to, st_dir = fresh > 0 ? 1 : 0, st_wait = 0;
            if (plan == TOTK_STRIP_NONE) st_phase = 0;
            else if (plan == TOTK_STRIP_SET) snap("no slide here");
            else if (plan == TOTK_STRIP_STEP) { step(st_dir); st_phase = 2; }   // one item: the native step
            else { StripSetPage(parts, (u32)pre); st_phase = 1; }   // a burst: to the item before, the step next
            if (verbose())
                printf("[menu-cursor] quickmenu strip %d: %+d notch(es), index %d -> %d of %d%s\n", sub, fresh, cur, to,
                       count, st_phase == 2 ? " (native step)" : st_phase == 1 ? " (setPage, then the native step)" : "");
        } else if (st_phase == 1 && usable) {
            if (rd<s32>(ctrl + 0x98) == -1) step(st_dir), st_phase = 2, st_wait = 0;
            else if (++st_wait > 3) snap("the snap still pending");
        } else if (st_phase == 2 && usable) {
            if (cur == st_target) st_phase = 0;
            else if (++st_wait > 3) snap("the step was not made");
        }
        if (scr && getenv("TOTK_MC_STRIP_DIAG")) {   // tests: where the strip keeps its focus (logged as it changes)
            const u64 sm = rd<u64>(kScreenMgr), bm = game_object(sm) ? rd<u64>(sm + kSmBoxCursorMgr) : 0;
            const u64 slide = rd<u64>(scr + 0x1a8), slc = ptr_ok(slide) ? rd<u64>(slide + 0x50) : 0;
            u64 c[2] = {}, cs[2] = {};
            for (int i = 0; i < 2 && ptr_ok(bm); i++) {
                const u64 ctl = rd<u64>(bm + kBmControls + 8ull * i);
                c[i] = ptr_ok(ctl) ? rd<u64>(ctl + kCtlActive) : 0;
                cs[i] = ptr_ok(c[i]) ? rd<u64>(c[i] + kNodeScreen) : 0;
            }
            static u64 last[8];
            const u64 now_v[8] = {rd<u64>(scr + kScrActive), rd<u64>(scr + kScrReserved), c[0], cs[0], c[1], cs[1],
                                  ptr_ok(slide) ? rd<u64>(slide + kScrActive) : 0, slc};
            if (memcmp(last, now_v, sizeof last)) {
                memcpy(last, now_v, sizeof last);
                printf("[menu-cursor] strip diag: scr active %#llx reserved %#llx | ctl0 %#llx (screen %#llx) ctl1 %#llx "
                       "(screen %#llx) | slide %#llx +0x148 %#llx control %#llx\n",
                       (unsigned long long)now_v[0], (unsigned long long)now_v[1], (unsigned long long)c[0],
                       (unsigned long long)cs[0], (unsigned long long)c[1], (unsigned long long)cs[1],
                       (unsigned long long)slide, (unsigned long long)now_v[6], (unsigned long long)slc);
            }
            static s32 words[64];   // the SlideListControl's first 0x100 bytes, as words: the ones that change
            static u64 seen;
            if (ptr_ok(slc)) {
                for (int i = 0; i < 64; i++) {
                    const s32 w = rd<s32>(slc + 4ull * i);
                    if (seen == slc && w != words[i] && abs(w) < 1000 && abs(words[i]) < 1000)   // (small ints only)
                        printf("[menu-cursor] strip diag: control +%#x %d -> %d\n", i * 4, words[i], w);
                    words[i] = w;
                }
                seen = slc;
            }
        }
        return 0;
    }
    // The Autobuild list: all the queued notches in one jump. Its rows are the screen's array (+0x248, 0x14 each: +8 the
    // mode, +0xc the draft's index, < 0 a category header; the count +0x250 within the capacity +0x240), in the order up
    // / down walks them; headers are never selected and the walk clamps at either end, no wrap (updateImpl_). The jump
    // is what updateImpl_ does for one step: the scroll target +0x254 = getDraftSlotHeight (computed here: -header/2,
    // plus each row's height up to the target's, minus half of it; header rows mPaddingHeaderHeight +0x234, the others
    // mPaddingBtnHeight +0x230), then the select call, once per burst. Only while the list takes input: its info panel
    // closed (+0x264 == 0, updateImpl_'s gate) and its born animation done (updateImpl_'s other gate); the notches wait
    // meanwhile. The row heights mPaddingBtnHeight +0x230 and mPaddingHeaderHeight +0x234 are confirmed in the raw code.
    // Anything unreadable falls back to the paced presses below. TOTK_MC_AB_DIRECT=0: the presses only.
    pending = (long long)fresh * pending < 0 ? fresh : pending + fresh;
    pending = pending > 200 ? 200 : pending < -200 ? -200 : pending;
    ab_age++;
    static const bool ab_direct = !(getenv("TOTK_MC_AB_DIRECT") && !strcmp(getenv("TOTK_MC_AB_DIRECT"), "0"));
    const u64 ab_scr = open_screen("AutoBuilder_00");
    const u64 rows = ab_scr ? rd<u64>(ab_scr + 0x248) : 0;
    const s32 count = ab_scr ? rd<s32>(ab_scr + 0x250) : 0, cap = ab_scr ? rd<s32>(ab_scr + 0x240) : 0;
    const float btn = ab_scr ? rd<float>(ab_scr + 0x230) : 0, hdr = ab_scr ? rd<float>(ab_scr + 0x234) : 0;
    const bool rows_ok = ptr_ok(rows) && count >= 1 && count <= cap && cap <= 4096 && btn > 0 && btn < 1000 &&
                         hdr > 0 && hdr < 1000;
    auto row_mode = [&](int i) { return rd<s32>(rows + 0x14ull * i + 8); };
    auto row_index = [&](int i) { return rd<s32>(rows + 0x14ull * i + 0xc); };   // (< 0: a header)
    auto row_scroll = [&](int to) { return totk_ab_row_scroll(rows, to, btn, hdr); };
    s32 index = -1, mode = 0;   // (updateImpl_ pre-fills the mode 0)
    int at = -1;
    if (rows_ok && ((ab_direct && pending) || verbose()) &&
        (AbActiveDraft((u64)(uintptr_t)&index, (u64)(uintptr_t)&mode) & 1))
        for (int i = 0; i < count && at < 0; i++)
            if (row_mode(i) == mode && row_index(i) == index) at = i;
    if (verbose() && ab_scr) {   // tests: the game's own scroll target against this formula's, as the row changes
        static int seen = -2;
        if (at != seen)
            printf("[menu-cursor] quickmenu autobuild: row %d (mode %d, draft %d), scroll target %.2f, formula %.2f "
                   "(heights %.2f / %.2f)\n", at, mode, index, rd<float>(ab_scr + 0x254), at >= 0 ? row_scroll(at) : 0.f,
                   btn, hdr);
        seen = at;
    }
    if (ab_direct && pending && ab_scr) {
        if (rd<u8>(ab_scr + 0x264)) return pending = 0;   // (the info panel takes the input, as the game's own)
        // (the notches wait for the list: updateImpl_ takes input only once its born animation is at frame 0,
        // *(f32*)(*(screen + 0x1b8) + 0x20); unreadable, 10 UI updates. A gate that never clears (a frame that never
        // reads 0) gives way after 30 UI updates, so the list never stops answering)
        const u64 born = rd<u64>(ab_scr + 0x1b8);
        if (ptr_ok(born) ? rd<float>(born + 0x20) != 0.f : ab_age < 10) {
            if (ab_age < 30) return 0;
            static bool told;
            if (!told && (told = true)) printf("[menu-cursor] autobuild: born gate not cleared, proceeding\n");
        }
        if (at >= 0) {
            const int to = totk_ab_target_row(rows, count, at, pending);
            const s32 to_mode = row_mode(to), to_index = row_index(to);
            if (to != at) {
                // (the scroll target only: the list eases +0x258 to it with the game's own animation, a burst too; the
                // selection is the new row at once)
                wr<float>(ab_scr + 0x254, row_scroll(to));
                AbSelect(ab_scr, mode, index, to_mode, to_index, 0);
            }
            if (verbose())
                printf("[menu-cursor] quickmenu autobuild: %+d notch(es), row %d (mode %d, draft %d) -> %d (mode %d, "
                       "draft %d) of %d\n", pending, at, mode, index, to, to_mode, to_index, count);
            pending = 0;
            return 0;
        }
        static int told;
        if (told < 5 && ++told)   // (then the presses step it)
            printf("[menu-cursor] quickmenu autobuild: no direct set (rows %#llx, count %d of %d, heights %.1f / %.1f, "
                   "draft %d mode %d)\n", (unsigned long long)rows, count, cap, btn, hdr, index, mode);
    }
    // The paced fallback: one press of up / down (bits 16 / 17) per step, 70 ms and 2 UI updates apart, every notch
    // counted (up to 200 queued)
    if (!pending || now - last_step < 70 || tick - step_tick < 2) return 0;
    const int dir = pending > 0 ? 1 : -1;
    last_step = now, step_tick = tick;
    pending -= dir;
    if (verbose()) printf("[menu-cursor] quickmenu autobuild: %s\n", dir > 0 ? "down" : "up");
    return dir > 0 ? (1u << 17) : (1u << 16);
}

HWND game_window() {   // class TotKRecomp, this process
    static HWND wnd;
    if (wnd && IsWindow(wnd)) return wnd;
    wnd = NULL;
    for (HWND w = NULL; (w = FindWindowExW(NULL, w, L"TotKRecomp", NULL));) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) return wnd = w;
    }
    return NULL;
}

// Tests (TOTK_MC_SWEEP=1, unattended boots): the pointer visits a grid of the picture, TOTK_MC_GRID=x0,y0,dx,dy,nx,ny
// (fractions, default 0.2,0.25,0.08,0.15,8,4), one point every TOTK_MC_DWELL ms (3000) from TOTK_MC_AT s (75) after
// the first pad read; TOTK_MC_CLICK_STEP=<n>[,<n>...] click left 1 s into those points.
// (TOTK_CTL "cursor" / "click", pc_mouse.cpp, drive the real path instead.)
ULONGLONG g_t0;
bool sweeping() {
    static const bool on = getenv("TOTK_MC_SWEEP") != nullptr;
    return on;
}
bool sweep_step(int* step, ULONGLONG* in = nullptr) {
    static const double at = getenv("TOTK_MC_AT") ? atof(getenv("TOTK_MC_AT")) : 75;
    static const ULONGLONG dwell = getenv("TOTK_MC_DWELL") ? (ULONGLONG)atoi(getenv("TOTK_MC_DWELL")) : 3000;
    const ULONGLONG now = GetTickCount64(), from = g_t0 + (ULONGLONG)(at * 1000);
    if (!g_t0 || now < from || !dwell) return false;
    *step = (int)((now - from) / dwell);
    if (in) *in = (now - from) % dwell;
    return true;
}
// Tests: each hover logged (with the sweep, or TOTK_MC_LOG=1)
bool verbose() {
    static const bool on = sweeping() || (getenv("TOTK_MC_LOG") && strcmp(getenv("TOTK_MC_LOG"), "0") != 0);
    return on;
}
bool step_listed(const char* list, int step) {
    for (const char* p = list; p && *p;) {
        if (atoi(p) == step) return true;
        p = strchr(p, ',');
        if (p) p++;
    }
    return false;
}

// The left mouse button (bit 0; the right one is not back any more, 4.22 rule 1): pc_mouse.cpp's (from the window's messages, TOTK_CTL "click" too);
// without the mouse camera it has no window hook, then the buttons' state while the game window is in front.
unsigned buttons() {
    unsigned b = pc_mouse_buttons() & 1;
    if (!pc_mouse_enabled() && GetForegroundWindow() == game_window() && game_window())
        b |= GetAsyncKeyState(VK_LBUTTON) & 0x8000 ? 1u : 0u;
    int step;
    ULONGLONG t;
    if (sweeping() && sweep_step(&step, &t) && t >= 1000 && t < 1150) {
        if (step_listed(getenv("TOTK_MC_CLICK_STEP"), step)) b |= 1;
    }
    return b;
}

// Where the pointer is on the game's picture, 0..1 from its top left: the rect the output pass draws it in
// (output_scale + output_fit_image, as device.c places it: a 16:9 fit, centred, or the image's own aspect when it is
// not 16:9). False off the picture or without a window.
bool picture(float* ppx, float* ppy) {
    float px, py;
    if (sweeping()) {
        int step;
        if (!sweep_step(&step)) return false;
        static float g[6] = {0.2f, 0.25f, 0.08f, 0.15f, 8, 4};
        static const bool parsed = [] {
            if (const char* e = getenv("TOTK_MC_GRID")) sscanf(e, "%f,%f,%f,%f,%f,%f", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5]);
            return true;
        }();
        (void)parsed;
        const int gx = (int)g[4] > 0 ? (int)g[4] : 1, gy = (int)g[5] > 0 ? (int)g[5] : 1;
        px = g[0] + (float)(step % gx) * g[2];
        py = g[1] + (float)((step / gx) % gy) * g[3];
    } else {
        const HWND wnd = game_window();
        RECT r;
        if (!wnd || !GetClientRect(wnd, &r) || r.right < 1 || r.bottom < 1) return false;
        float nx, ny;
        if (!pc_mouse_client_pos(&nx, &ny)) {   // (no window hook without the mouse camera: the pointer itself)
            POINT p;
            if (pc_mouse_enabled() || !GetCursorPos(&p) || !ScreenToClient(wnd, &p)) return false;
            nx = (float)p.x / (float)r.right; ny = (float)p.y / (float)r.bottom;
        }
        OutScale o;
        output_scale((double)r.right, (double)r.bottom, false, 0, -1, &o);
        output_fit_image((double)r.right, (double)r.bottom, &o);  // a 21:9 / 32:9 canvas's own rect
        if (o.vw < 1 || o.vh < 1) return false;
        px = (float)((nx * r.right - o.vx) / o.vw);
        py = (float)((ny * r.bottom - o.vy) / o.vh);
    }
    if (px < 0 || px > 1 || py < 0 || py > 1) return false;
    *ppx = px; *ppy = py;
    return true;
}
#endif

}  // namespace

#ifdef _WIN32
namespace {
u64 frame(const Focus& f);
std::mutex g_frame_lock;   // the frame's state: one caller at a time (the pad reads, or design A's hook)
}
#endif
extern "C" bool totk_ui_post(TotkUiTask fn, void* arg) {
#ifdef _WIN32
    if (!fn || !hook_alive(GetTickCount64())) return false;
    std::lock_guard<std::mutex> l(g_task_lock);
    if (g_ntasks >= 16) return false;
    g_tasks[g_ntasks++] = UiTask{fn, arg, GetTickCount64()};
    return true;
#else
    (void)fn; (void)arg;
    return false;
#endif
}

extern "C" int totk_strip_plan(int cur, int base, int notches, int count, int wlo, int whi, bool snap_free, int* to,
                               int* pre) {
    const int d = notches > 0 ? 1 : -1;
    *to = ((base + notches % count) % count + count) % count;
    *pre = ((*to - d) % count + count) % count;
    if (*to == cur) return TOTK_STRIP_NONE;
    // (the slide's own window: a step inside it animates; outside it, or across the wrap, the control would not make it)
    const bool slides = whi >= wlo && *pre >= wlo && *pre <= whi && *to >= wlo && *to <= whi && *pre + d == *to;
    if (!slides) return TOTK_STRIP_SET;
    return *pre == cur && snap_free ? TOTK_STRIP_STEP : TOTK_STRIP_SET_THEN_STEP;
}

// ---- the scrolling lists' geometry (totk_menu_cursor.h)
extern "C" bool totk_box_in_window(const TotkBox* b, const TotkBox* w, float mx, float my) {
    auto axis = [](float c, float h, float s, float sh, float m) {
        return h >= sh || (c - h >= s - sh + m - 0.5f && c + h <= s + sh - m + 0.5f);
    };
    return axis(b->cx, b->hw, w->cx, w->hw, mx) && axis(b->cy, b->hh, w->cy, w->hh, my);
}
extern "C" bool totk_row_at_edge(const TotkBox* r, const TotkBox* rows, int n, const TotkBox* w, float mx, float my) {
    for (int dir = -1; dir <= 1; dir += 2) {
        bool whole = false, cut = false;
        for (int i = 0; i < n; i++) {
            const float d = (r->cy - rows[i].cy) * (float)dir;
            if (d <= 1 || fabsf(rows[i].cx - r->cx) >= fmaxf(rows[i].hw, r->hw)) continue;   // (not that side, another column)
            (totk_box_in_window(&rows[i], w, mx, my) ? whole : cut) = true;
        }
        if (cut && !whole) return true;
    }
    return false;
}
extern "C" int totk_scroll_pick(const TotkBox* focused, const TotkBox* rows, int n, const TotkBox* w, float mx, float my,
                                int dir) {
    int out_i = -1, in_i = -1;
    float out_d = 1e30f, in_d = -1;
    for (int i = 0; i < n; i++) {
        const TotkBox& r = rows[i];
        const float d = (focused->cy - r.cy) * (float)dir;   // (layout y is up: down the list is a smaller y)
        if (d <= 1 || fabsf(r.cx - focused->cx) >= fmaxf(r.hw, focused->hw)) continue;   // (not that way, another column)
        if (!totk_box_in_window(&r, w, mx, my)) {
            if (d < out_d) out_d = d, out_i = i;
        } else if (d > in_d) {
            in_d = d, in_i = i;
        }
    }
    return out_i >= 0 ? out_i : in_i;
}

// ---- the menu hotkeys' chain
extern "C" void totk_menu_chain_init(TotkChain* c) {
    *c = TotkChain{};
    c->target = -1;
    c->last_item_tab = 3;
    c->last_tab = -1;
}
extern "C" void totk_menu_chain_goto(TotkChain* c, int target, unsigned long long now) {
    if (target < 0) { c->target = -1; return; }
    // (the same target again while its chain runs, e.g. The router's follow retried: the running chain goes on, not
    // restarted, so nothing is pressed twice)
    if (c->target >= 0 && c->target == (target & ~TOTK_MC_GOTO_NO_CLOSE)) return;
    const bool no_close = (target & TOTK_MC_GOTO_NO_CLOSE) != 0;
    c->target = target & ~TOTK_MC_GOTO_NO_CLOSE, c->phase = 0, c->first = no_close ? 0 : 1, c->backs = 0, c->button = 0;
    c->at = c->t = now;
}
extern "C" TotkChainOut totk_menu_chain_step(TotkChain* c, const TotkChainIn* in) {
    constexpr uint32_t kB = 1u << 1, kPlus = 1u << 10, kMinus = 1u << 11;
    TotkChainOut o{0, 0, -1};
    const int kind = (int)(in->screen & 0xff), sub = (int)(in->screen >> 8 & 0xff);
    const bool subscreen = (in->screen & TOTK_MENU_F_SUB) != 0;
    if (kind == TMS_PAUSE && !subscreen && sub <= 7) c->last_item_tab = sub;   // (the inventory's target)
    // (LAST's target: not while a chain runs, whose Plus opens the menu on the game's own tab first)
    if (kind == TMS_PAUSE && !subscreen && sub <= 8 && c->target < 0) c->last_tab = sub;
    if (c->target < 0) return o;
    const unsigned long long now = in->now;
    if (now - c->at > 3000) { c->target = -1; return o; }
    // the families: 1 the pause menu, 2 the Purah Pad (its map too), 0 no menu, -1 another screen
    const bool pause = in->pouch_open || kind == TMS_PAUSE || kind == TMS_ITEM_POPUP;
    const bool pad = !pause && (in->pad_open || kind == TMS_PURAH_PAD || kind == TMS_MAP);
    const int fam = pause ? 1 : pad ? 2 : kind == TMS_NONE ? 0 : kind == TMS_TRANSITION ? -2 : -1;
    const int want_fam = c->target >= TOTK_MC_GOTO_PAGE && c->target < TOTK_MC_GOTO_INVENTORY ? 2 : 1;
    const bool last = c->target == TOTK_MC_GOTO_LAST;
    const int want = c->target == TOTK_MC_GOTO_INVENTORY ? c->last_item_tab : last ? c->last_tab
                   : want_fam == 2 ? c->target - TOTK_MC_GOTO_PAGE : c->target;
    auto press = [&](uint32_t b, int after) { c->button = b, c->after = after, c->phase = 1, c->t = now; };
    switch (c->phase) {
    case 1:   // pressing
        o.buttons = c->button;
        if (now - c->t >= 60) c->phase = 2, c->t = now;
        return o;
    case 2:   // released
        if (now - c->t >= 60) c->phase = c->after, c->t = now;
        return o;
    case 3:   // waiting for no menu (a close)
        if (fam == 0) {
            if (c->first < 0) { c->target = -1; return o; }   // (the toggle: closed, done)
            press(want_fam == 1 ? kPlus : kMinus, 4);
        } else if (fam > 0 && now - c->t >= 400) {
            if (++c->backs > 6) { c->target = -1; return o; }
            press(kB, 3);   // (still open: B again)
        }
        return o;
    case 4:   // waiting for the target's menu
        if (fam == want_fam && !subscreen && sub != 0xff) c->phase = 0;
        return o;
    default:
        break;
    }
    // decide
    if (fam == -2 || (fam > 0 && sub == 0xff && !subscreen)) return o;   // (a focus gap: wait)
    if (fam == -1) { c->target = -1; return o; }   // (a dialogue, a shop, the title: not ours to leave)
    const bool first = c->first > 0;
    c->first = 0;
    if (fam > 0 && subscreen) {
        if (++c->backs > 4) { c->target = -1; return o; }
        press(kB, 0);
        return o;
    }
    if (fam == want_fam) {
        const bool on = c->target == TOTK_MC_GOTO_INVENTORY ? sub <= 7 : last ? sub <= 8 && (want < 0 || sub == want)
                                                                         : sub == want;
        if ((last ? sub <= 8 : on) && first) { c->first = -1; press(kB, 3); return o; }   // the toggle: close (LAST: any tab)
        if (!on) { o.walk_kind = want_fam == 1 ? 0 : 1; o.walk_to = want; }
        c->target = -1;
        return o;
    }
    if (fam == 0) { press(want_fam == 1 ? kPlus : kMinus, 4); return o; }
    press(kB, 3);   // (the other menu: close it first)
    return o;
}

// The Autobuild list's rows (UIAutoBuilderScreen +0x248: 0x14 each, +0xc the draft's index, < 0 a category header)
extern "C" int totk_ab_target_row(uint64_t rows, int count, int at, int notches) {
    int to = at;
    for (int n = notches, i = at; n;) {   // (n rows the notches' way, past the headers, stopping at an end)
        i += n > 0 ? 1 : -1;
        if (i < 0 || i >= count) break;
        if (rd<s32>(rows + 0x14ull * i + 0xc) < 0) continue;
        to = i, n -= n > 0 ? 1 : -1;
    }
    return to;
}
extern "C" float totk_ab_row_scroll(uint64_t rows, int to, float btn, float hdr) {   // (getDraftSlotHeight)
    float h = -hdr / 2, p = 0;
    for (int i = 0; i <= to; i++) h += p = rd<s32>(rows + 0x14ull * i + 0xc) < 0 ? hdr : btn;
    return h - p / 2;
}

// The page's refresh 0x1001a84a4c titles it from the global at main's slot 0x4558440: ride +0x67a == 0 Basic
// Controls; else kind +0x6b8 in {0, 1, 2, 3, 4, 12} Riding Controls, 6 the Sages' (SoulSageControls), 8 / 10
// SunazarashiControls, any other Basic; the game is paused, so the fields still describe the page shown. The other
// pages keep on foot's keys. Fallback, the global unreadable (ride < 0): in the saddle at the pause, by the last
// context or TPS_HORSE, set while mounted, a bow drawn or not
extern "C" int totk_controls_page_set(int ride, int kind, int game_set, uint32_t player_state) {
#ifdef PC_HAS_GLYPH_CP_IN
    if (ride >= 0) {
        const bool riding_page = ride && (kind == 0 || kind == 1 || kind == 2 || kind == 3 || kind == 4 || kind == 12);
        return riding_page ? PCX_HORSE : PCX_WORLD;
    }
    const bool riding = (player_state & TPS_VALID) && (player_state & TPS_HORSE);
    return game_set == PCX_HORSE || riding ? PCX_HORSE : PCX_WORLD;
#else
    (void)ride, (void)kind, (void)game_set, (void)player_state;
    return 0;
#endif
}

extern "C" int totk_mc_sub_label(const uint16_t* in, int len, const char* key, uint16_t* out, int cap) {
    if (!in || !key || !*key || !out || len < 0) return -1;
    int open = -1, close = -1;
    uint16_t want = 0;
    for (int i = 0; i < len && close < 0; i++) {
        const uint16_t c = in[i];
        if (c == 0x0e) {   // (a tag: 0x0E group type size, then size bytes of payload)
            i += i + 3 < len ? 3 + (in[i + 3] + 1) / 2 : len;
            continue;
        }
        if (c == 0x0f) {   // (a tag's end: 0x0F group type)
            i += 2;
            continue;
        }
        if (open < 0) {
            want = c == '(' ? ')' : c == '[' ? ']' : c == 0xff08 ? 0xff09 : c == 0x3010 ? 0x3011 : 0;
            if (want) open = i;
        } else if (c == want) {
            close = i;
        }
    }
    if (close < 0) return -1;
    uint16_t k[32];   // (the key's name, UTF-8 to UTF-16: BMP only, a name is a key cap's text)
    int kn = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(key);
    while (*p) {
        if (kn == 32) return -1;
        unsigned c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more == 3 || (c >= 0x80 && c < 0xc0)) return -1;   // (outside the BMP, or a stray continuation)
        c &= more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
        for (; more; more--, p++) {
            if ((*p & 0xc0) != 0x80) return -1;
            c = c << 6 | (*p & 0x3f);
        }
        k[kn++] = (uint16_t)c;
    }
    const int n = open + 1 + kn + (len - close);
    if (n + 1 > cap) return -1;
    int o = 0;
    for (int i = 0; i <= open; i++) out[o++] = in[i];
    for (int i = 0; i < kn; i++) out[o++] = k[i];
    for (int i = close; i < len; i++) out[o++] = in[i];
    out[o] = 0;
    return o;
}

extern "C" uint32_t totk_menu_screen(void) {
#ifdef _WIN32
    return g_menu_screen.load(std::memory_order_relaxed);
#else
    return TMS_NONE | 0xff00u;
#endif
}

extern "C" bool totk_menu_cursor_rstick(int32_t* rx) {
#ifdef _WIN32
    if (!g_drag_live.load(std::memory_order_relaxed)) return false;
    *rx = g_drag_rx.load(std::memory_order_relaxed);
    return true;
#else
    (void)rx;
    return false;
#endif
}

extern "C" int totk_attach_screen_open(void) {
#ifdef _WIN32
    if (!hook_alive(GetTickCount64())) return -1;   // (no hook in this key, or eui not updating: unknown)
    return g_attach_open.load(std::memory_order_relaxed);
#else
    return -1;
#endif
}

extern "C" uint64_t totk_menu_focus_node(void) {
#ifdef _WIN32
    const u64 scr = g_focus_screen.load();
    return game_object(scr) ? rd<u64>(scr + kScrActive) : 0;
#else
    return 0;
#endif
}

extern "C" uint64_t totk_menu_layout_focus(const char* layout) {
#ifdef _WIN32
    const u64 scr = open_screen(layout);
    return scr ? rd<u64>(scr + kScrActive) : 0;
#else
    (void)layout;
    return 0;
#endif
}

extern "C" void totk_menu_pad_hold(uint32_t buttons) {
#ifdef _WIN32
    g_ext_hold.store(buttons, std::memory_order_relaxed);
#else
    (void)buttons;
#endif
}

// hle_hid.cpp GetNpadStates (once per npad read, several times a frame, from 4 threads: mc_dial / mc_opts logged them):
// moves the focus to the item under the pointer; returns the pad buttons the clicks give (HidNpadButton A / B).
// One read at a time (its state is shared); a read that finds the lock taken gets the last answer.
extern "C" u64 totk_menu_cursor_frame(void) {
#ifdef _WIN32
    static std::atomic<u64> last{0};
    // Design A: eui's thread runs it all; the pad gets only the hook's Purah Pad page walk (L / R)
    if (const ULONGLONG now = GetTickCount64(); hook_alive(now))
        return (g_pad_open.load() ? g_pad_walk.load() : 0)
#ifdef MC_ROUTER
               | g_chain_buttons.load(std::memory_order_relaxed)   // (the menu hotkeys' chain: B, Plus, Minus)
#endif
               | g_ext_hold.load(std::memory_order_relaxed)        // (the Options sections' reopen: B, A)
               | g_arrow_pad.load(std::memory_order_relaxed);      // (a click on an Options arrow)
    {   // tasks posted just before the hook stopped (a load): told they will not run once 250 ms old
        UiTask run[16], drop[16];
        int nrun, ndrop;
        take_tasks(GetTickCount64(), run, &nrun, drop, &ndrop);
        if (nrun) {   // (still young: back in the queue for the hook)
            std::lock_guard<std::mutex> q(g_task_lock);
            for (int i = 0; i < nrun && g_ntasks < 16; i++) g_tasks[g_ntasks++] = run[i];
        }
        for (int i = 0; i < ndrop; i++) drop[i].fn(drop[i].arg, false);
    }
    std::unique_lock<std::mutex> l(g_frame_lock, std::try_to_lock);
    if (!l.owns_lock()) return last.load();
    const Focus f = focus_now();   // (once per read: the context and the cursor share it)
    g_focus_screen.store(f.screen);
#ifdef MC_ROUTER
    static const bool registered = (pc_menu_ops_register(menu_op), pc_quickmenu_feed_register(qm_feed), true);
    (void)registered;
#endif
    census(GetTickCount64());       // (first: the flags the context reads)
    update_menu_screen(f.screen);   // the context, also with the cursor off
    const u64 out = frame(f) | g_ext_hold.load(std::memory_order_relaxed) | g_arrow_pad.load(std::memory_order_relaxed);
    last.store(out);
    return out;
#else
    return 0;
#endif
}

#ifdef _WIN32
namespace {
// Link's model in the inventory turned by an LMB drag: a press on the model's area, not on an item or a tab, starts it;
// while the button stays down the pointer's horizontal speed is the right stick's x (the pad's Rotate: R stick left /
// right), full at half the picture's width per second, eased over 25 ms as pc_mouse's camera; let go and it ends. The
// press is no click (no A, nothing on release); the pointer stays free; no hover while it lasts. The model is drawn by
// the game behind Pouch_00, no pane marks it (lyt_dump of Pouch_00 / PlayerStatus3D_00 / PlayerStatus_00): its box is
// taken from the inventory as drawn (os4_1440_on's inv shot, layout units, y up): right of the item grid, below the
// hearts, over the description panel and the Rotate prompt's row; only on an item tab (System has no model) with no
// item popup open. x from the layout's right edge (lw / 2: 960 on 16:9, so 330..900 there), as the right-hand panel
// sits on a wider layout (the nit: an ultrawide canvas)
constexpr float kModelFromRightL = 630, kModelFromRightR = 60, kModelB = -450, kModelT = 380;
bool pouch_model_at(u64 scr, float x, float y, float lw) {
    if (!is_pouch(scr) || pouch_popup_open(scr)) return false;
    const int tab = active_tab();
    return tab >= 0 && tab <= 7 && x >= lw / 2 - kModelFromRightL && x <= lw / 2 - kModelFromRightR && y >= kModelB &&
           y <= kModelT;
}
struct ModelDrag { bool on = false; float lx = 0; double t = 0, v = 0; };
ModelDrag g_drag;
void drag_stop() {
    g_drag = ModelDrag{};
    g_drag_live.store(false, std::memory_order_relaxed);
    g_drag_rx.store(0, std::memory_order_relaxed);
}
double now_s() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}
u64 frame(const Focus& f) {
    static bool was;
    if (g_arrow_until && GetTickCount64() >= g_arrow_until)   // (first: every path below ends the arrow's D-pad press)
        g_arrow_until = 0, g_arrow_pad.store(0, std::memory_order_relaxed);
    if (!enabled()) {
        if (was) { pc_mouse_cursor_mode(false); was = false; }
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_t0) g_t0 = now;
    {
        // the pads are logged (the first 4), so a second one shows
        static DWORD tids[4];
        const DWORD me = GetCurrentThreadId();
        for (int i = 0; i < 4; i++) {
            if (tids[i] == me) break;
            if (!tids[i]) { tids[i] = me; printf("[menu-cursor] pad reads on thread %lu (%d)\n", me, i + 1); break; }
        }
    }
    static u64 last_screen;
    static int logs;
    if (f.screen != last_screen && f.screen && logs < 400) {
        logs++;
        char nm[48];
        layout_name(f.screen, nm, sizeof nm);
        const u64 lyt = rd<u64>(f.screen + kScrLayout);
        printf("[menu-cursor] focus in screen %#llx, layout %s (%.0f x %.0f)\n", (unsigned long long)f.screen, nm,
               ptr_ok(lyt) ? rd<float>(lyt + kLytSize) : 0.0f, ptr_ok(lyt) ? rd<float>(lyt + kLytSize + 4) : 0.0f);
    }
    last_screen = f.screen;
    // The cursor stays 300 ms after the focus is gone: a screen changing its focus (a popup opening, a tab switch)
    // has none for a frame or two, and the pointer must not flick back to the camera then.
    static ULONGLONG seen;
    if (f.screen) seen = now;
    // On the map the map mouse (controls.mouse_map, TOTK_MOUSE_MAP: the mouse moves it as the left stick, a click pins
    // at the centre) needs the pointer captured: no cursor there while it is on. The map by this census's own screen
    // (TMS_MAP), not only screen_track's Pad guess, which M's menu chain never trips
    const uint32_t ms = g_menu_screen.load(std::memory_order_relaxed) & 0xff;
    const bool pad = (hid_screen_now() == 1 || ms == TMS_MAP) && hid_mouse_map_on();
    // (a quickmenu or the Scope, as the context says: no cursor, the mouse stays the router's)
    const bool quick = ms == TMS_QUICKMENU || ms == TMS_SCOPE;
    const bool cursor = seen && now - seen < 300 && !pad && !quick;
    if (cursor != was && logs < 400) {
        logs++;
        printf("[menu-cursor] cursor %s\n", cursor ? "on" : pad ? "off (Purah Pad: the map's mouse)" : "off");
    }
    static ULONGLONG on_since;   // when the cursor last came on
    if (cursor && !was) on_since = now;
    was = cursor;
    pc_mouse_cursor_mode(cursor);
    // Clicks: pc_mouse.cpp's press counts, so a click whose press and release both fall between two pad reads (a
    // quick click during a frame hitch: mc_real8 lost one) still counts; plus the buttons' state for the presses it
    // does not see (no window hook without the mouse camera, the tests' sweep clicks). Followed while the cursor is
    // off too, so a press in gameplay is not taken as a click when a menu opens.
    static unsigned prev, presses;
    static bool counted;
    const unsigned btn = buttons(), pl = pc_mouse_presses(0);
    // A click is kept until a read that finds a menu's focus takes it (a read can find none for a moment while a
    // screen changes its focus: mc_opts lost every click that way), for 300 ms at most
    static unsigned pending;
    static ULONGLONG pending_at;
    unsigned fresh = btn & ~prev;
    if (counted) fresh |= pl != presses ? 1u : 0u;
    counted = true;
    prev = btn; presses = pl;
    if (fresh) { pending |= fresh; pending_at = now; }
    if (fresh && verbose() && logs < 400) {   // (tests: a press as this read counts it, and what gates it here)
        logs++;
        printf("[menu-cursor] press counted (buttons %u, presses %u): cursor %d, focus screen %d, overlay %d\n", btn,
               pl, cursor, f.screen != 0, pc_overlay_open() || osd_menu_is_open());
    }
    if (pending && now - pending_at > 300) {
        if (verbose() && logs < 400) {
            logs++;
            printf("[menu-cursor] click dropped: 300 ms without a focus screen (cursor %d, screen %d)\n", cursor,
                   f.screen != 0);
        }
        pending = 0;
    }
    if (const int rp = take_req(g_req_page, 0, now); rp >= 0 && g_pad_open.load() && !g_pad_camera.load() &&
                                                       !g_pad_scope.load())
        walk_start(1, rp, now);
    {   // Tests: TOTK_MC_TEST_PAGE=<n> walks to Purah Pad page n once, 1 s after the Pad first opens (no router needed)
        static const char* tp = getenv("TOTK_MC_TEST_PAGE");
        static ULONGLONG pad_since;
        static bool done;
        if (tp && !done) {
            if (!g_pad_open.load()) pad_since = 0;
            else if (!pad_since) pad_since = now;
            else if (now - pad_since > 1000) { done = true; walk_start(1, atoi(tp), now); }
        }
    }
    if (!cursor) {
        g_scroll_can.store(0);
        if (g_drag.on) drag_stop();
        if (pending && verbose() && logs < 400) {
            logs++;
            printf("[menu-cursor] click dropped: the cursor is off (screen %d)\n", f.screen != 0);
        }
        pending = 0;
        return walk_step(f, now);
    }
    // (an Options section change reopening Options: no hover, no click until it is done)
    const bool reopening = totk_options_reopening();
    const unsigned edge = f.screen && !reopening ? pending : 0;
    if (f.screen) pending = 0;

    static u64 a_node;                            // a click's item, A once the focus is on it (0: none)
    static int a_dir;                             // ... or D-pad left (-1) / right (1): a click on its < / > (Options)
    static ULONGLONG a_at, a_from, a_until;       // clicked at; A from / until (a_from 0: not yet) (ms)
    // A click on a pause-menu tab icon: the tab it asks for, when that started, the press / release phase (0 a press
    // is due, 1 pressing, 2 releasing), the presses made, and the tab seen before the last one
    static bool over;  // a focus-box item or a tab icon is under the pointer (the map: its clicks are ours)
    static u64 want;                              // the node the last hover asked the focus to move to
    static u64 mine;                              // the node this cursor last moved the focus to (any path)
    static ULONGLONG want_at;
    if (pc_overlay_open() || osd_menu_is_open()) {   // the engine's own menus have the mouse: the game none of it
        a_node = 0; a_from = 0;
        g_scroll_can.store(0);
        if (g_drag.on) drag_stop();
        return 0;
    }
    // Hover follows the pointer's motion on the picture. For 200 ms after the cursor comes on the pointer is taken as
    // it is: pc_mouse.cpp centres it then (on its window thread, a moment later), and that jump is not a hover; the
    // menu's own first focus stays.
    float px = 0, py = 0;
    const bool on = f.screen && picture(&px, &py);
    if ((edge & 1) && !on && verbose() && logs < 400) { logs++; printf("[menu-cursor] click: the pointer is off the picture\n"); }
    static float lx = -1, ly = -1;
    const bool settling = now - on_since < 200;
    // While a pad is in use pc_mouse hides the released pointer until a deliberate move, a click or an absolute move
    // (the automatic device switching): a hidden pointer nudged a few counts does not hover, so it never takes the focus
    // from the pad's d-pad. Its position is still followed, and a click still acts (it shows the pointer).
    const bool hidden = pc_mouse_pointer_hidden();
    bool moved = on && !settling && !hidden && !reopening && (fabsf(px - lx) > 0.0005f || fabsf(py - ly) > 0.0005f);
    if (on) { lx = px; ly = py; }
    {  // Options: the focus moved by the game's own input
        // (the wheel's steps, the D-pad, the keys: the list scrolled to it, another row is now under the still
        // pointer) is kept until the pointer really moves, 2 % of the picture from where it was then; a hand's
        // jitter no longer takes the focus back and drags the list against the wheel
        static u64 seen_active;
        static float ax = -1, ay = -1;   // the pointer when the game moved the focus (ax < 0: no hold)
        const u64 act = f.screen ? rd<u64>(f.screen + kScrActive) : 0;
        if (act != seen_active) {
            if (act && seen_active && act != mine && on && is_options(f.screen)) ax = px, ay = py;
            seen_active = act;
        }
        if (ax >= 0 && (!on || !is_options(f.screen) || fabsf(px - ax) > 0.02f || fabsf(py - ay) > 0.02f)) ax = ay = -1;
        if (ax >= 0) moved = false;
    }
    if (on && !g_drag.on && (moved || (edge & 1))) {   // (no hover or click while the model's drag lasts)
        float lw = 1920, lh = 1080;   // the screen's layout size
        const u64 lyt = rd<u64>(f.screen + kScrLayout);
        if (ptr_ok(lyt)) {
            const float w = rd<float>(lyt + kLytSize), h = rd<float>(lyt + kLytSize + 4);
            if (w >= 64 && w <= 8192 && h >= 64 && h <= 8192) lw = w, lh = h;
        }
        const float x = (px - 0.5f) * lw, y = (0.5f - py) * lh;
        // One test per screen: through the screen's draw transform where it maps the layout elsewhere than the
        // layout test assumes (no such screen known yet), else in the layout's own space (every screen seen so far).
        // Never both: on a moved screen the layout test would hit items where nothing is drawn.
        const Xf t = screen_xf(f.screen);
        const bool use_xf = t.ok && !t.plain(lw, lh);
        {  // which test a screen gets, once per screen
            static u64 told[32];
            static int nt;
            bool seen_scr = false;
            for (int i = 0; i < nt; i++) seen_scr |= told[i] == f.screen;
            if (!seen_scr && logs < 400) {
                if (nt < 32) told[nt++] = f.screen;
                logs++;
                char nm[40];
                layout_name(f.screen, nm, sizeof nm);
                printf("[menu-cursor] %s: hit test in %s, on the %s\n", nm,
                       use_xf ? "device space (the draw transform)" : "layout space",
                       stale_hit_panes(f.screen, lw, lh, use_xf ? &t : nullptr) ? "buttons' parts panes (stale hit panes)"
                                                                                   : "hit panes");
            }
        }
        const float hx = use_xf ? px * 2 - 1 : x, hy = use_xf ? 1 - py * 2 : y;
        int n = 0;
        u64 node = node_at(f.screen, hx, hy, &n, lw, lh, false, use_xf ? &t : nullptr);
        if (node && use_xf && verbose() && logs < 400) { logs++; printf("[menu-cursor] (hit through the draw transform)\n"); }
        // A scrolling list's row (scissor_of): none with the pointer outside its window; a hover only on a whole one
        // (a click takes a cut one: the scroll is the click's)
        const bool stale = !use_xf && stale_hit_panes(f.screen, lw, lh, nullptr);
        if (node && !use_xf) {
            const u64 np = node_pane(node, stale), sc = ptr_ok(np) ? scissor_of(np) : 0;
            const bool outside = sc && !pane_hit(sc, x, y);
            const int blocked = sc && !outside && !(edge & 1) ? hover_blocked(f.screen, node, sc, stale) : 0;
            if (outside || blocked) {
                static int told;
                if (verbose() && told < 40) {
                    told++;
                    printf("[menu-cursor] pointer %.0f,%.0f: node %#llx %s: no hover\n", x, y, (unsigned long long)node,
                           outside ? "outside its list's window" : blocked == 1 ? "cut by its list's window"
                                                                            : "the last whole row before a cut one");
                }
                node = 0;
            }
        }
        // (Options: a hover leaves a row the list's edge cuts alone, options_row_cut; a click takes it)
        const bool cut = node && !(edge & 1) && is_options(f.screen) && options_row_cut(node);
        if (node && !cut && node != rd<u64>(f.screen + kScrActive) && node != rd<u64>(f.screen + kScrReserved)) {
            mine = node;
            if (move_focus(f, node)) want = node, want_at = now;
            if (!(edge & 1) && !use_xf) hover_note(f.screen, node, stale, now);
            if (verbose() && logs < 400) {
                logs++;
                printf("[menu-cursor] pointer %.0f,%.0f: focus -> node %#llx (%d of the list)\n", x, y,
                       (unsigned long long)node, n);
            }
        }
        // Tests: why the pointer has no item: no shown pane under it, or the node there is not usable (flags;
        // ButtonGroup's 0x20010), with the screen's active / reserved nodes
        char why[160] = "";
        if (!node && verbose()) {
            int m = 0;
            const u64 under = node_at(f.screen, hx, hy, &m, lw, lh, true, use_xf ? &t : nullptr);
            const u64 act = rd<u64>(f.screen + kScrActive), res = rd<u64>(f.screen + kScrReserved);
            if (!under) snprintf(why, sizeof why, " (no shown pane under it; active %#llx, reserved %#llx)",
                                 (unsigned long long)act, (unsigned long long)res);
            else snprintf(why, sizeof why, " (node %#llx under it not usable, flags %#x; active %#llx, reserved %#llx)",
                          (unsigned long long)under, rd<u32>(rd<u64>(under + kNodeButton) + kBtnFlags),
                          (unsigned long long)act, (unsigned long long)res);
        }
        if (!(node && node != rd<u64>(f.screen + kScrActive) && node != rd<u64>(f.screen + kScrReserved)) && verbose() &&
            logs < 400) {
            logs++;
            printf("[menu-cursor] pointer %.0f,%.0f: %s%s\n", x, y, node ? "focused" : "no item", why);
            if (!node) dump_nodes(f.screen);
            if (!node && is_pouch(f.screen)) dump_tabs(f.screen);
        }
        // A left click on an item is A on that item: sent once the game's focus is on it (it moves the box at its
        // next UI update), held at least 100 ms so a quick click is seen; dropped if the focus has not got there
        // in 400 ms. Off the items a click does nothing. A focused item's button may be in its focus animation and
        // not "usable" for a moment: a click on the focused (or reserved) item counts all the same
        u64 hit = node;
        int arrow = 0;   // a click on an Options row's < (-1) or > (1): that row, D-pad left / right once it has the focus
        if ((edge & 1) && g_in_hook && !use_xf && is_options(f.screen)) {   // (eui's thread: the rows' list is its)
            u64 an = 0;
            if ((arrow = options_arrow_at(f.screen, hx, hy, &an)) != 0) {
                hit = an;
                if (an != rd<u64>(f.screen + kScrActive) && an != rd<u64>(f.screen + kScrReserved) && (mine = an, move_focus(f, an)))
                    want = an, want_at = now;
            }
        }
        if ((edge & 1) && !hit) {
            int m = 0;
            const u64 any = node_at(f.screen, hx, hy, &m, lw, lh, true, use_xf ? &t : nullptr);
            if (any && (any == rd<u64>(f.screen + kScrActive) || any == rd<u64>(f.screen + kScrReserved))) hit = any;
        }
        int tab = -1;
        over = node || hit || (is_pouch(f.screen) && tab_at(f.screen, x, y) >= 0);
        if ((edge & 1) && hit && g_walk.to >= 0) {
            // (a tab walk is under way: the list changes under the pointer between its presses, so a click on an
            // item would land on whatever the list holds then; dropped)
            if (logs < 400) { logs++; printf("[menu-cursor] click during a tab walk: dropped\n"); }
        } else if ((edge & 1) && hit) {
            a_node = hit; a_at = now; a_from = 0; a_dir = arrow;
            if (logs < 400) {
                logs++;
                printf("[menu-cursor] click: %s\n", arrow < 0   ? "D-pad left on the row under the pointer (its <)"
                                                    : arrow > 0 ? "D-pad right on the row under the pointer (its >)"
                                                                : "A on the item under the pointer");
            }
        } else if ((edge & 1) && is_pouch(f.screen) && (tab = tab_at(f.screen, x, y)) >= 0) {
            // a tab icon: L / R presses until the pouch shows that tab (tab_walk below)
            walk_start(0, tab, now);
        } else if ((edge & 1) && !use_xf && pouch_model_at(f.screen, x, y, lw)) {
            g_drag = ModelDrag{true, px, now_s(), 0};
            if (logs < 400) { logs++; printf("[menu-cursor] drag on the model (%.0f,%.0f): the right stick turns it\n", x, y); }
        } else if ((edge & 1) && logs < 400) {
            logs++;
            printf("[menu-cursor] click off the items (%.0f,%.0f): nothing%s\n", x, y, why);
        }
    }
    if (!on) over = false;
#ifdef MC_ROUTER
    pc_mouse_cursor_over_item(over);
#endif
    if (g_drag.on) {   // the model's drag (pouch_model_at): the pointer's horizontal speed, until the button is let go
        if (!(btn & 1) || !f.screen || !is_pouch(f.screen) || pouch_popup_open(f.screen)) {
            drag_stop();
        } else {
            const double t = now_s(), dt = t - g_drag.t;
            if (dt > 0.0005) {
                const double vx = on ? (px - g_drag.lx) / dt : 0;
                g_drag.v += (vx - g_drag.v) * (1 - exp(-dt / 0.025));
                if (on) g_drag.lx = px;
                g_drag.t = t;
            }
            double s = g_drag.v / 0.5;
            s = s > 1 ? 1 : s < -1 ? -1 : fabs(s) < 0.02 ? 0 : s;
            g_drag_rx.store((int32_t)lrint(s * 32767), std::memory_order_relaxed);
            g_drag_live.store(true, std::memory_order_relaxed);
        }
    }
    // The wheel in a scrolling list (scroll_target), not in the pouch (its pages slide by their own control): the ways
    // with a row to step to, for menu_op; then one pending notch per UI update, the focus there before the next
    if (f.screen && f.node && !is_pouch(f.screen)) {
        hover_watch(f.screen, f.node, now);
        float lw, lh;
        layout_size(f.screen, &lw, &lh);
        const Xf t = screen_xf(f.screen);
        const bool plain = !t.ok || t.plain(lw, lh);
        const bool stale = plain && stale_hit_panes(f.screen, lw, lh, nullptr);
        const u64 up = plain ? scroll_target(f.screen, f.node, -1, stale) : 0;
        const u64 down = plain ? scroll_target(f.screen, f.node, 1, stale) : 0;
        g_scroll_scr.store(f.screen);
        g_scroll_can.store((up ? 1u : 0u) | (down ? 2u : 0u));
        static u64 stepped;
        static ULONGLONG stepped_at;
        const int wn = g_wheel_n.load();
        if (wn && now - g_wheel_at.load() > 300) {
            g_wheel_n.store(0);
        } else if (wn && (f.node == stepped || now - stepped_at > 300)) {
            g_wheel_n.fetch_sub(wn < 0 ? -1 : 1);
            if (const u64 to = wn < 0 ? up : down) {
                if (move_focus(f, to)) want = to, want_at = now;
                stepped = to, stepped_at = now;
                g_hw.node = 0;
                if (verbose() && logs < 400) {
                    logs++;
                    printf("[menu-cursor] wheel %s: focus -> node %#llx\n", wn < 0 ? "up" : "down", (unsigned long long)to);
                }
            }
        }
    } else {
        g_scroll_can.store(0);
    }
    // The router's ops this file takes (menu_op): a tab by number (the same walk as a tab icon's click), a choice by
    // number (its node focused, then A once the focus is there, as a click)
    if (const int rt = f.screen ? take_req(g_req_tab, f.screen, now) : -1; rt >= 0 && is_pouch(f.screen))
        walk_start(0, rt, now);
    if (const int rc = f.screen ? take_req(g_req_choice, f.screen, now) : -1; rc >= 0) {
        float lw = 1920, lh = 1080;
        const u64 lyt = rd<u64>(f.screen + kScrLayout);
        if (ptr_ok(lyt)) {
            const float w = rd<float>(lyt + kLytSize), h = rd<float>(lyt + kLytSize + 4);
            if (w >= 64 && w <= 8192 && h >= 64 && h <= 8192) lw = w, lh = h;
        }
        if (const u64 node = nth_node(f.screen, rc, lw, lh)) {
            if (node != rd<u64>(f.screen + kScrActive)) {
                mine = node;
                if (move_focus(f, node)) want = node, want_at = now;
            }
            a_node = node; a_at = now; a_from = 0; a_dir = 0;
            if (logs < 400) { logs++; printf("[menu-cursor] op: choice %d\n", rc + 1); }
        }
    }
    // The move's request bit again while the focus has not got there, for 300 ms
    if (want && f.screen && now - want_at < 300 && rd<u64>(f.screen + kScrReserved) == want &&
        rd<u64>(f.screen + kScrActive) != want && !(rd<u8>(f.screen + kScrRequest) & 4))
        __atomic_fetch_or(reinterpret_cast<u8*>(static_cast<uintptr_t>(f.screen + kScrRequest)), (u8)4, __ATOMIC_SEQ_CST);
    else if (want && now - want_at >= 300) want = 0;
    if (a_node && !a_from) {
        if (f.screen && a_dir && (rd<u64>(f.control + kCtlActive) == a_node || rd<u64>(f.screen + kScrActive) == a_node)) {
            // An arrow's click: the pad's own D-pad press on the focused row (HidNpadButton Left / Right) for 100 ms,
            // so the screen steps the value as for a real press (its arrow animation, its sound); 100 ms is one step
            g_arrow_pad.store(a_dir < 0 ? 1u << 12 : 1u << 14, std::memory_order_relaxed);
            g_arrow_until = now + 100;
            a_node = 0; a_dir = 0;
            if (logs < 400) { logs++; printf("[menu-cursor] arrow: D-pad pressed\n"); }
        } else if (f.screen && (rd<u64>(f.control + kCtlActive) == a_node || rd<u64>(f.screen + kScrActive) == a_node)) {
            // On eui's thread the button's own Down; else (or if Down refuses) an A press
            const int d = g_in_hook ? press_down(a_node, f.target) : -1;
            if (d > 0) {
                if (logs < 400) { logs++; printf("[menu-cursor] confirm: Down sent\n"); }
                a_node = 0;
                // (the decide may have closed the screen: nothing more of this frame reads f)
                return 0;
            } else if (d < 0) {
                a_from = now; a_until = now + 100;
            } else if (now - a_at > 400) {   // (0: not decidable now; the wait goes on for 400 ms)
                a_node = 0;
                if (logs < 400) {
                    logs++;
                    printf("[menu-cursor] click: the item was not decidable in 400 ms, no confirm\n");
                }
            }
        } else if (now - a_at > 400) {
            a_node = 0;
            if (logs < 400) { logs++; printf("[menu-cursor] click: the focus never reached the item, no A\n"); }
        }
    }
    // The right button is not back in the menus, so a right click does nothing here. A only while a menu has the
    // focus. A moment without focus (a submenu closing, a popup opening) ends a pulse: carried into the next screen
    // it would be a press there. A pending A that has not started keeps its 400 ms wait.
    if (!f.screen) {
        if (a_from) a_node = 0, a_from = 0;
        return 0;
    }
    u64 out = 0;
    out |= walk_step(f, now);
    // One edge per click: A is a 100 ms pulse, never held with the button, so a long press is still one confirm
    if (a_from && now < a_until) out |= 1;
    else if (a_from) a_node = 0, a_from = 0;
    return out;
}
}  // namespace
#endif

#ifdef _WIN32
// ---- design A: the hook on eui::UIController::updateState (main+0x83e3f8; gen.py --hook 83e3f8 in GEN_MAIN) eui's UI
// module calls it once a frame on eui's thread, for its one shared UIController, before the screens read their input;
// gameplay never reads that controller. After the original: the cursor's frame runs here, its focus moves call
// moveBoxCursor itself, and its A / B / L / R come out as whole presses on the UI controller's own fields (trigger
// +0x08, release +0x0c, repeat +0x10, hold +0x114; sead bits A 0, B 1, ZL 2, ZR 5, L 13, R 14: the pouch's tab masks
// 0x2004 / 0x4020), never on the hid pad. A key without the hook never calls hook_f_83e3f8 and has no f_83e3f8_orig:
// the linker then takes mc_orig_stub (/alternatename), so one runtime links against keys with and without it.
void f_83e3f8_orig(Ctx *__restrict c);
void mc_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_83e3f8_orig@@YAXPEIAUCtx@@@Z=?mc_orig_stub@@YAXPEIAUCtx@@@Z")
void hook_f_83e3f8(Ctx *__restrict c) {   // UIController::updateState(this, src)
    const u64 self = c->x[0], src = c->x[1];
    f_83e3f8_orig(c);
    if (!ptr_ok(self)) return;
    // The shared UIController, by address: UIModule (main+0x4563120, the instance or the slot that holds it) +0x198
    // (the object, or a pointer to it). Another controller is left alone (logged once); if the module cannot be
    // read, the first controller seen.
    static u64 first;
    u64 ui = 0;
    {
        u64 mod = rd<u64>(totk_main(0x4563120));
        if (!game_object(mod) && ptr_ok(mod)) mod = rd<u64>(mod);
        if (game_object(mod)) ui = self == mod + 0x198 || self == rd<u64>(mod + 0x198) ? self : 0;
        else { if (!first) first = self; ui = self == first ? self : 0; }
    }
    if (!ui) {
        static bool told;
        if (!told) {
            told = true;
            printf("[menu-cursor] updateState on another UIController %#llx: left alone\n", (unsigned long long)self);
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG before = g_hook_at.load();
    if (!before) {
        // (U2: the controller the BoxCursorMgr reads is getCustomController(module, port, "UI"), a sead controller, not
        // this one; logged so a boot shows them. Writing into a sead controller would reach gameplay, so only the UI
        // controller gets our presses: the screens read it for decide / back / tabs.)
        printf("[menu-cursor] design A: the hook on UIController::updateState is alive (UI controller %#llx, its "
               "source %#llx)\n", (unsigned long long)self, (unsigned long long)src);
        // ControllerModule's custom controllers (sInstance; main +0xa0, all players +0xa8, combined +0xb0), to tell
        // which one is the BoxCursor's "UI" controller and which one gameplay reads
        const u64 cm = rd<u64>(totk_main(0x46490c0));
        if (ptr_ok(cm))
            printf("[menu-cursor]   ControllerModule (sInstance) controllers: main %#llx, all players %#llx, "
                   "combined %#llx\n",
                   (unsigned long long)rd<u64>(cm + 0xa0), (unsigned long long)rd<u64>(cm + 0xa8),
                   (unsigned long long)rd<u64>(cm + 0xb0));
    }
    g_hook_at.store(now);
    guest_bridge::Enter e(c);
    u64 out;
    {
        std::lock_guard<std::mutex> l(g_frame_lock);
        const Focus f = focus_now();
        g_focus_screen.store(f.screen);
#ifdef MC_ROUTER
        static const bool registered = (pc_menu_ops_register(menu_op), pc_quickmenu_feed_register(qm_feed), true);
        (void)registered;
#endif
        census(now);
        g_attach_open.store(open_screen("AttachTargetSelectCommand_00") ? 1 : 0, std::memory_order_relaxed);
        update_menu_screen(f.screen);
#if defined(MC_ROUTER) && defined(PC_HAS_GLYPH_GAMEPLAY)
        glyph_pass();   // (eui's thread only: the panes are drawn from it)
#endif
        g_in_hook = true;
        out = frame(f);
        g_in_hook = false;
    }
    // Posted tasks (totk_ui_post), on this thread, inside the bridge, no lock of ours held
    UiTask run[16], drop[16];
    int nrun, ndrop;
    take_tasks(now, run, &nrun, drop, &ndrop);
    for (int i = 0; i < ndrop; i++) drop[i].fn(drop[i].arg, false);
    for (int i = 0; i < nrun; i++) run[i].fn(run[i].arg, true);
    // The Purah Pad's page walk: its L / R on the pad (whether the page bar reads the UI controller is not known; the
    // pad reaches both, the UI controller through its source), only while the Pad is open
    const u64 lr = out & ((1u << 6) | (1u << 7));
    const bool pad_walk = g_walk.to >= 0 && g_walk.kind == 1;
    g_pad_walk.store(pad_walk && g_pad_open.load() ? lr : 0);
    if (pad_walk) out &= ~((1ull << 6) | (1ull << 7));
    // A, L, R: presses of the UI controller, held while on, triggered on the frame a press starts, released on the
    // frame it ends. Not repeated: the repeat bit is a second decide on repeatable buttons (mAction 2).
    u32 want = (out & 1 ? 0x1u : 0) | (out & (1u << 6) ? 0x2004u : 0) | (out & (1u << 7) ? 0x4020u : 0);
    {   // the quickmenus' mouse feed (its right stick is written directly; the Autobuild list's steps are presses)
        std::lock_guard<std::mutex> l(g_frame_lock);
        want |= qm_apply(self, now);
#ifdef MC_ROUTER
        chain_apply(now);
#endif
    }
    static u32 prev;
    if (!before || now - before >= 250) prev = 0;  // (the path just switched: a fresh start)
    const u32 trig = want & ~prev, rel = prev & ~want;
    prev = want;
    if (!want && !rel) return;
    wr<u32>(self + 0x114, rd<u32>(self + 0x114) | want);
    wr<u32>(self + 0x08, rd<u32>(self + 0x08) | trig);
    // The original derives release from last frame's hold, which held our bits: while we still hold them, that is
    // not a release; ours is the frame the press ends
    wr<u32>(self + 0x0c, (rd<u32>(self + 0x0c) & ~want) | rel);
}
#else
// Elsewhere (Linux / Deck) the cursor is off, but a key with 83e3f8 in GEN_MAIN calls this hook: it only runs the
// original (weak, so keys without the hook link too).
__attribute__((weak)) void f_83e3f8_orig(Ctx *__restrict c);
void hook_f_83e3f8(Ctx *__restrict c) {
    if (f_83e3f8_orig) f_83e3f8_orig(c);
}
#endif
