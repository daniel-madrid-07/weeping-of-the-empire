// Mouse camera. TOTK_MOUSE_CAMERA=<x> (a multiplier, default 1, 0 = off; reload() below): raw mouse motion (WM_INPUT)
// turns the camera. Direct (the patched key's `mouse` veneer, games/totk/patches/sites_v100.py mouse): the counts, times
// TOTK_MOUSE_SENS (default 1) and the Y inversion (TOTK_MOUSE_INVERT=1), go as running totals (doubles: no motion lost,
// however slow) to the PC page of the game patches' data page; inside the PlayerCamera's stick step the veneer adds the
// motion since its last step to the processed stick, after the game's dead zone, clamp and response curve, scaled by the
// frame ratio: linear, 1:1, the same turn per count at any frame rate, no direction snapping, no speed cap. While the
// veneer applies the mouse it claims it (PC page +0x64) and the stick emulation below stops. Elsewhere (the map's cursor,
// menus, an unpatched key) the mouse drives the right stick: velocity smoothed over ~25 ms, full deflection at
// TOTK_MOUSE_STICK_SPEED counts per second (default 1500) / TOTK_MOUSE_SENS. Capture (the pointer hidden and confined):
// whenever the camera is on, the game window is active and the settings overlay is closed; in a window, after a
// click-activation only once the click lands inside it (a title-bar drag stays a drag). It is checked at every input
// read, so it comes back by itself after a menu, the overlay or an Alt+Tab; Windows dropping the confinement (another
// window, a notification) is redone. Each capture and release is logged with why. Mouse buttons bound to actions
// (TOTK_KEYBOARD_LAYOUT=pc / custom, hle_hid.cpp) count only while captured, not the capturing click. The game window
// (shim_win.c) is subclassed from here, not edited. Windows only.
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pc_settings.h"
#include "pc_input.h"   // the rebinding capture (pc_input_capture_msg / _wheel)
#include "pc.h"   // pc_fault_guard (cam_floats)
#include "totk_player_state.h"   // the late latch's aim bit (latch_aim)

extern "C" uint64_t hid_game_buttons(void);   // hle_hid.cpp: the Npad buttons the game was given last (bit 9 ZR)
#include "pc_device_switch.h"   // the released pointer's "moved on purpose" (MotionGate)
#include "pc_clip.h"            // when the pointer is confined to the window (every context in fullscreen)

extern "C" bool shim_window_fullscreen(void);   // shim_win.c (borderless or exclusive)
extern "C" bool pc_overlay_open(void);          // pc_overlay.cpp: the settings window (or the Welcome one) is open
extern "C" void* totk_data_page_current(void);  // games/totk/runtime/totk_patches.c: the data page, or NULL
#define WM_APP_CAPTURE (WM_APP + 50)            // wParam 1 / 0, lParam the reason (const char*); shim_win.c: WM_APP + 1 .. 3
#define WM_APP_RECLIP (WM_APP + 51)
#define WM_APP_CURSOR (WM_APP + 52)
#define WM_APP_POINTER (WM_APP + 53)

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
double g_dx, g_dy;                     // g_lock: raw counts since the last stick read
double g_tx, g_ty;                     // g_lock: running totals for the veneer (sensitivity and inversion applied)
double g_wheel;                        // g_lock: wheel notches (+ = away from the user) not yet taken
double g_vx, g_vy;                     // stick reader: smoothed velocity, counts/s
LARGE_INTEGER g_last;                  // stick reader: time of the last read
std::atomic<bool> g_captured{false};
std::atomic<bool> g_armed{false};      // in a window: activated by the keyboard, or clicked inside since a click-activation
std::atomic<bool> g_pending{false};    // a capture change was posted to the window thread
std::atomic<unsigned> g_swallow{0};    // mouse buttons held when the pointer was captured (bits 1 left, 2 right, 4 middle):
                                       // ignored until released (the capturing click; a right click that closed a menu)
std::atomic<ULONGLONG> g_injected_until{0};   // TOTK_CTL "mouse dx dy": counts as captured until this tick (ms)
HWND g_hwnd;
WNDPROC g_orig;
std::atomic<bool> g_on{false};
std::atomic<bool> g_menu{false};   // a menu screen has the mouse (pc_mouse_menu)
std::atomic<unsigned> g_presses[3];   // DOWN messages so far, per button (left, right, middle): pc_mouse_presses
std::atomic<unsigned> g_buttons{0};   // left / right / middle down, from the window's button messages (posted ones too)
std::atomic<bool> g_cursor{false};   // a menu shows the pointer as a cursor (pc_mouse_cursor_mode, totk_menu_cursor.cpp)
std::atomic<bool> g_ui{false};       // the PC scheme's context is a menu, the title or a dialogue (pc_mouse_ui, hle_hid.cpp)
// A pad is the device in use (hle_hid.cpp's prompts, pc_mouse_pad_in_use): the released pointer is hidden, as console
// menus show none, until the mouse moves on purpose (pcdev::MotionGate, here on the window thread: it shows at once and
// g_pointer_moved tells hle_hid, whose prompts then follow through their own dwell).
std::atomic<bool> g_pad_hide{false};
std::atomic<bool> g_pointer_moved{false};
pcdev::MotionGate g_pointer_gate;    // window thread
HCURSOR pointer() { return g_pad_hide.load() ? NULL : LoadCursor(NULL, IDC_ARROW); }
// The pointer is released for a menu: the cursor mode, or a UI context (the capture policy is fed the context class
// and stays the only writer of capture and clip)
bool released() { return g_cursor.load() || g_ui.load(); }
// A PC scheme macro has the mouse (pc_input.cpp: a tick that sets take_mouse, e.g. the Tab wheel, the strips, Ultrahand's
// rotation): the counts go here, raw, and the camera's totals and the stick's counts get none, so when it ends the
// camera continues from where it stopped (no catch-up turn). g_lock.
std::atomic<bool> g_redirect{false};
double g_rx, g_ry;
double g_sens = 1, g_full = 1500, g_inv = 1, g_scale = 1;   // g_lock (written by reload, the settings overlay's thread)
// The data page's PC page (games/totk/include/totk/data_page.h TOTK_DP_PC_OFFSET; platform's PlatPcPage layout): the
// mouse totals (f64 +0x08 / +0x10), captured (u32 +0x20); +0x64 the veneer's claim. And the multiplier (f32 at 80).
const unsigned kPcPage = 0x100, kMouseX = 0x08, kMouseY = 0x10, kCaptured = 0x20, kClaimed = 0x64, kMouseScale = 80;
// Pitch is not linear in the processed stick the veneer adds to: the camera turns 51 * y * |y| deg/s for a step value
// y, against yaw's 70 * x (54.7 from y 0.08 to 4.95). So the y total is published once per camera step (the veneer
// stamps DP_MOUSE_BEAT at each), as the motion since the last one squared back: a step's dy counts become
// sqrt(kPitchGain * |dy| / k) with the veneer's own k = rate * multiplier / frame ratio, and pitch turns as many
// degrees a count as yaw, at any speed and frame rate (one camera step later than yaw: the step it is for). Keys and
// runtimes ship apart and the runtime cannot tell the veneer otherwise, so pitch_feed shapes pitch with g = 0 until it
// sees the ack, then writes g = kPitchGain and stops at that same step; from then on the y total is published raw with
// x (no step of lag, R18).
const double kPitchGain = 70.0 / 51.0;
const unsigned kRatio = 0, kMouseRate = 84, kBeat = 92, kPitchG = 128, kPitchAck = 132;
std::atomic<bool> g_pitch_veneer{false};   // the veneer shapes pitch (its ack seen): y published raw by page_totals
double g_ty_off;                           // g_lock: published y = g_ty + this (continuous across the switch)
uint8_t* page() { return (uint8_t*)totk_data_page_current(); }
// The abilities' camera (4.25 3b): the veneer's step runs there too, but ExecutePlayerCameraSpecialPower (0x1001ce6eb0)
// turns the stored stick its own way: dPitch = y * LatStickScale 1.5 * (PlayerCamera +0xc2c), LINEAR (no y|y|), and dYaw =
// x * LngStickScale 0.8 * speed. So in the router's ABILITY the veneer gets pitch linear (g 0 at +128: its linear path,
// not the on-foot sqrt shaping, which made small moves the fastest) and the counts going to the totals are scaled to on
// foot's degrees a count: x / 0.8 (yaw's speed as on foot), y * 70 / (1.5 * 51) (on foot pitch turns 51 y|y| deg/s, 70
// deg/s a unit once shaped: +0xc2c taken as that same 51). Aiming keeps the game's own slower bow camera.
// TOTK_MOUSE_ABILITY_GAIN=<x>,<y> overrides the two factors.
const double kAbilityGainX = 1 / 0.8, kAbilityGainY = 70.0 / (1.5 * 51.0);
float pitch_gain_now() { return pc_ctx() == PCX_ABILITY ? 0.0f : (float)kPitchGain; }
void pitch_gain_apply() {   // any thread, once the veneer shapes pitch: g at +128 as the context wants it
    if (!g_pitch_veneer.load(std::memory_order_acquire)) return;
    uint8_t* p = page();
    if (!p) return;
    const float g = pitch_gain_now();
    uint32_t gw;
    memcpy(&gw, &g, 4);
    if (__atomic_load_n((uint32_t*)(p + kPitchG), __ATOMIC_ACQUIRE) != gw) __atomic_store_n((uint32_t*)(p + kPitchG), gw, __ATOMIC_RELEASE);
}
// The bow drawn: the PC bow macro's AIM, or the game's own ZR held (hid_game_buttons bit 9: on foot, on a horse, gliding;
// any device) unless the hands read shows no bow (ZR with no bow is the empty aim stance, not a bow).
bool bow_aim() {
    if (pc_ctx() == PCX_AIM) return true;
    if (!(hid_game_buttons() & (1ull << 9))) return false;
    const uint32_t st = totk_player_state();
    const bool hands_read = (st & TPS_VALID) && (st & (TPS_WEAPON_EQUIPPED | TPS_BOW_EQUIPPED | TPS_SHIELD_EQUIPPED));
    return !hands_read || (st & TPS_BOW_EQUIPPED);
}
// controls.mouse_aim_scale (TOTK_MOUSE_AIM_SCALE, 0.1 .. 3): the mouse's camera motion times this while the bow is drawn,
// on every path it takes (the veneer's totals, so the late latch's warp too; the stick; the mouse gyro's counts, before
// mouse_gyro_speed). g_lock.
double g_aim_scale = 1;
double aim_gain() { return g_aim_scale != 1 && bow_aim() ? g_aim_scale : 1; }
void mode_gain(double* gx, double* gy) {   // (each count: the window thread, the ctl injection)
    static double ax = -1, ay = -1;
    if (ax < 0) {
        double x = kAbilityGainX, y = kAbilityGainY;
        if (const char* e = getenv("TOTK_MOUSE_ABILITY_GAIN"); e && sscanf(e, "%lf,%lf", &x, &y) != 2) x = kAbilityGainX, y = kAbilityGainY;
        ay = y; ax = x;
    }
    const bool ability = pc_ctx() == PCX_ABILITY;
    const double aim = aim_gain();
    *gx = (ability ? ax : 1) * aim; *gy = (ability ? ay : 1) * aim;
    pitch_gain_apply();
}
void page_totals() {   // g_lock held
    if (uint8_t* p = page()) {
        uint64_t x;
        memcpy(&x, &g_tx, 8);
        __atomic_store_n((uint64_t*)(p + kPcPage + kMouseX), x, __ATOMIC_RELEASE);
        if (g_pitch_veneer.load(std::memory_order_acquire)) {   // (else y: pitch_feed)
            const double yv = g_ty + g_ty_off;
            uint64_t y;
            memcpy(&y, &yv, 8);
            __atomic_store_n((uint64_t*)(p + kPcPage + kMouseY), y, __ATOMIC_RELEASE);
        }
    }
}
void page_captured(bool on) {
    if (uint8_t* p = page()) __atomic_store_n((uint32_t*)(p + kPcPage + kCaptured), on ? 1u : 0u, __ATOMIC_RELEASE);
}
bool page_claimed() {
    uint8_t* p = page();
    return p && __atomic_load_n((uint32_t*)(p + kPcPage + kClaimed), __ATOMIC_ACQUIRE) != 0;
}

// TOTK_MOUSE_CAMERA is a multiplier (the controls.mouse_camera_scale row writes it 0..4): unset = 1 (on by default since
// the 2026-10-23 all-on pass), 0 = off, above 0 scales the turn (the veneer's multiplier; the stick's full speed / it).
// TOTK_MOUSE_SENS 0.1.. 20 (the controls.mouse_sensitivity row).
void reload() {
    const char* e = getenv("TOTK_MOUSE_CAMERA");
    double scale = e && *e ? atof(e) : 1;
    double sens = 1, full = 1500, inv = 1;
    if (const char* s = getenv("TOTK_MOUSE_SENS"); s && *s) sens = fmin(20.0, fmax(0.1, atof(s)));
    if (const char* s = getenv("TOTK_MOUSE_STICK_SPEED")) full = fmax(50.0, atof(s));
    if (const char* s = getenv("TOTK_MOUSE_INVERT")) inv = *s && strcmp(s, "0") ? -1 : 1;
    double aim = 1;
    if (const char* s = getenv("TOTK_MOUSE_AIM_SCALE"); s && *s && atof(s) > 0) aim = fmin(3.0, fmax(0.1, atof(s)));
    AcquireSRWLockExclusive(&g_lock);
    g_sens = sens; g_full = full; g_inv = inv; g_scale = scale > 0 ? scale : 1; g_aim_scale = aim;
    ReleaseSRWLockExclusive(&g_lock);
    if (uint8_t* p = page()) {   // (totk_patches.c wrote it at boot; the row changes it live)
        float f = scale > 0 ? (float)fmin(scale, 4.0) : 0.0f;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        __atomic_store_n((uint32_t*)(p + kMouseScale), bits, __ATOMIC_RELEASE);
    }
    bool was = g_on.exchange(scale > 0);
    if (scale > 0)
        printf("[mouse] camera%s: the pointer is captured while the window is active and the overlay closed; x%.2f, "
               "sensitivity %.2f\n", was ? " settings" : "", scale, sens);
    else if (was) printf("[mouse] camera off\n");
}
// Once per settings file: a sensitivity saved for the old stick path (where it only reached the stick's top turn rate
// sooner, so players set 3-4) is far too fast for the direct path (1 = about 6650 counts a turn): it goes back to 1.
// controls.mouse_look (a hidden row, hle_hid.cpp) records that it was done.
void migrate_sensitivity() {
    const char* done = pc_settings_get("controls.mouse_look");
    if (!done || !strcmp(done, "1")) return;
    const char* s = pc_settings_get("controls.mouse_sensitivity");
    if (s && *s && atof(s) != 1.0 && !getenv("NXR_MOUSE_KEEP_SENS")) {
        printf("[mouse] sensitivity %s was for the old stick camera; the mouse now turns the camera directly: set to 1 "
               "(controls.mouse_sensitivity, 0.1 .. 20)\n", s);
        pc_settings_set("controls.mouse_sensitivity", "1");
    }
    pc_settings_set("controls.mouse_look", "1");
}
bool enabled() {
    static const bool once = (migrate_sensitivity(), reload(), true);
    (void)once;
    return g_on.load();
}

bool client_rect(HWND h, RECT* s) {   // the client area on screen
    RECT r;
    if (!GetClientRect(h, &r)) return false;
    POINT a = {r.left, r.top}, b = {r.right, r.bottom};
    ClientToScreen(h, &a); ClientToScreen(h, &b);
    *s = {a.x, a.y, b.x, b.y};
    return s->right > s->left && s->bottom > s->top;
}
// The confinement (pc_clip.h's rule: every context while in front; windowed once armed, never in a move / size loop):
// clip_apply is the only ClipCursor caller, on the window thread, at every event that can change it (capture, focus,
// size / move / display change, a mode change, the menu cursor) and when the input thread's clip_check finds Windows'
// confinement differs (at every read: Windows drops it on its own, and a fast flick crossed the old 500 ms gap).
std::atomic<bool> g_clip_ours{false};       // the last ClipCursor was ours, a confinement (window thread writes)
std::atomic<bool> g_reclip_posted{false};   // a WM_APP_RECLIP is on its way
bool clip_target(HWND h, RECT* want) {   // the client area as Windows keeps a confinement: cut to the virtual screen
    RECT scr = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
    scr.right = scr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); scr.bottom = scr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return client_rect(h, want) && IntersectRect(want, want, &scr);
}
std::atomic<bool> g_moving{false};   // Windows' move / size loop (window thread writes)
pcclip::Act clip_decide(HWND h, bool force_free, RECT* want_r) {
    const pcclip::State st = {GetForegroundWindow() == h, IsIconic(h) != 0, shim_window_fullscreen(), g_captured.load(),
                              g_armed.load(), g_moving.load()};
    const bool wanted = !force_free && pcclip::want(st) && clip_target(h, want_r);
    RECT have = {};
    GetClipCursor(&have);
    return pcclip::check(wanted, {want_r->left, want_r->top, want_r->right, want_r->bottom},
                         {have.left, have.top, have.right, have.bottom}, g_clip_ours.load());
}
RECT g_clip_rect;   // window thread: the rectangle of the game's last ClipCursor (while g_clip_ours)
pcclip::Act clip_apply(HWND h, bool force_free = false) {   // window thread
    RECT want_r = {};
    const pcclip::Act a = clip_decide(h, force_free, &want_r);
    if (a == pcclip::CLIP) { ClipCursor(&want_r); g_clip_rect = want_r; g_clip_ours.store(true); }
    else if (a == pcclip::FREE) { ClipCursor(NULL); g_clip_ours.store(false); }
    return a;
}
// A confinement of ours that Windows dropped, redone (capped): what Windows had instead (none: the whole virtual screen;
// else another program's rectangle) and which window was in front.
void clip_again_log(HWND h, const RECT& had, const char* by) {
    static int logged;
    if (logged >= 20) return;
    const RECT scr = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    char rect[64] = "no confinement";
    if (!EqualRect(&had, &scr)) snprintf(rect, sizeof rect, "%ld,%ld .. %ld,%ld", had.left, had.top, had.right, had.bottom);
    printf("[mouse] pointer confined again (Windows had dropped the confinement; %s): it had %s, in front %s (pid %lu)%s\n",
           by, rect, fg == h ? "the game" : fg ? "another window" : "none", pid, ++logged == 20 ? "; not logged again" : "");
}
// Window thread, at every raw mouse event (WM_INPUT: 1000 and more a second while the mouse moves): a dropped
// confinement is redone before the pointer travels, not at the input thread's next read (clip_check), which is a game
// frame later (33 ms at 30 fps).
void clip_guard(HWND h) {
    RECT have;
    if (!g_clip_ours.load() || !GetClipCursor(&have) || EqualRect(&have, &g_clip_rect)) return;
    if (clip_apply(h) == pcclip::CLIP) clip_again_log(h, have, "at a mouse event");
}
// Window thread, captured (the pointer hidden): back to the client centre past a quarter of the way to an edge (pc_clip.h
// recentre), so a flick in a dropped-confinement gap can't reach another monitor. The box is the confinement's (the
// client cut to the virtual screen: a client larger than the desktop, the 4K canvas, would centre past the clip), the
// client area only while there is none of ours.
void recentre(HWND h) {
    RECT s;
    POINT p;
    if (g_clip_ours.load()) s = g_clip_rect;
    else if (!client_rect(h, &s)) return;
    if (!GetCursorPos(&p) || !pcclip::recentre({s.left, s.top, s.right, s.bottom}, p.x, p.y)) return;
    SetCursorPos((s.left + s.right) / 2, (s.top + s.bottom) / 2);
}
// The physical buttons down now (bit 1 left, 2 right, 4 middle): the device (GetAsyncKeyState reads them physically, as
// keyboard_read's bindings do, so pc_mouse_button_ok's VK is physical too), or the window's messages (posted ones too),
// which are logical: mapped back through SM_SWAPBUTTON. One space for the swallow mask.
unsigned held_buttons() {
    unsigned msg = g_buttons.load() & 7;
    if (GetSystemMetrics(SM_SWAPBUTTON)) msg = (msg & 4) | (msg & 1) << 1 | (msg & 2) >> 1;
    return msg | ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0) |
           ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4 : 0);
}
void set_capture(HWND h, bool on, const char* why) {   // window thread
    if (on == g_captured.load()) return;
    if (on) SetCursor(NULL);
    AcquireSRWLockExclusive(&g_lock); g_dx = g_dy = 0; ReleaseSRWLockExclusive(&g_lock);
    if (on) g_swallow.store(held_buttons());   // (posted clicks too: g_buttons)
    g_captured.store(on);
    clip_apply(h);  // (released: still confined; windowed too)
    page_captured(on);
    printf("[mouse] pointer %s (%s)\n", on ? "captured" : "released", why);
}
void want(bool on, const char* why) {   // any thread: the window thread applies it
    if (on == g_captured.load() || !g_hwnd || g_pending.exchange(true)) return;
    if (!PostMessageW(g_hwnd, WM_APP_CAPTURE, on, (LPARAM)why)) g_pending.store(false);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // a rebinding capture (pc_input.cpp) sees every message first: the key it takes reaches nobody else, not the overlay
    if (pc_input_capture_msg(h, m, (uintptr_t)w, (intptr_t)l)) return 0;
    switch (m) {
    case WM_INPUT: {
        RAWINPUT ri; UINT size = sizeof ri;
        const bool cap = g_captured.load(), cur = released();   // (the menu cursor: the wheel only)
        clip_guard(h);   // (every context: the confinement first, before the pointer moves on)
        if (cap && !g_moving.load()) recentre(h);  // (Alt+Space Move / Size follows the pointer)
        const bool capw = pc_input_capture_wants_wheel();         // (a capture that takes a wheel notch)
        if ((cap || cur || capw) && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            if (capw && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && (SHORT)ri.data.mouse.usButtonData &&
                pc_input_capture_wheel((SHORT)ri.data.mouse.usButtonData > 0 ? 1 : -1))
                ri.data.mouse.usButtonFlags &= ~RI_MOUSE_WHEEL;   // (the capture's notch: not the game's)
            if (!cap && !cur) break;
            if (!cap) {   // (the released pointer: moved on purpose or clicked; absolute devices, e.g. a remote desktop's
                const auto& mo = ri.data.mouse;   // pointer, have no counts: a position that changed by over 64 of 65535
                static LONG ax = -1, ay = -1;     // per axis; VM mouse integration and pen drivers repeat the same one)
                bool abs_move = false;
                if (mo.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    abs_move = ax >= 0 && abs(mo.lLastX - ax) + abs(mo.lLastY - ay) > 64;
                    ax = mo.lLastX; ay = mo.lLastY;
                }
                const bool click = mo.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN);
                const bool moved = !(mo.usFlags & MOUSE_MOVE_ABSOLUTE) &&
                                   g_pointer_gate.add(abs(mo.lLastX) + abs(mo.lLastY), (double)GetTickCount64());
                if (abs_move || click || moved) {
                    g_pointer_moved.store(true);
                    if (g_pad_hide.exchange(false)) SetCursor(LoadCursor(NULL, IDC_ARROW));
                }
            }
            AcquireSRWLockExclusive(&g_lock);
            if (cap && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_redirect.load()) {
                g_rx += ri.data.mouse.lLastX; g_ry += ri.data.mouse.lLastY;
            } else if (cap && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_dx += ri.data.mouse.lLastX; g_dy += ri.data.mouse.lLastY;
                // (a menu screen's mouse, the map's: its stick only, never the camera's totals, or the camera turns by
                // all the map's scrolling when it closes)
                if (!g_menu.load()) {
                    double gx, gy;
                    mode_gain(&gx, &gy);
                    g_tx += ri.data.mouse.lLastX * g_sens * gx; g_ty += ri.data.mouse.lLastY * g_sens * g_inv * gy;
                    page_totals();
                }
            }
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) g_wheel += (SHORT)ri.data.mouse.usButtonData / (double)WHEEL_DELTA;
            ReleaseSRWLockExclusive(&g_lock);
        }
        break;   // the original procedure's DefWindowProc cleans the input up
    }
    // (the window takes the mouse while a button is down, so its UP comes here even released outside the client area;
    // a lost capture clears them, a bit left set swallowed the button for gameplay)
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        const unsigned bit = m == WM_LBUTTONUP ? 1u : m == WM_RBUTTONUP ? 2u : 4u;
        if (!(g_buttons.fetch_and(~bit) & ~bit & 7) && GetCapture() == h) ReleaseCapture();
        break;
    }
    case WM_CAPTURECHANGED: g_buttons.store(0); break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:   // (client area only)
        if (GetCapture() != h) SetCapture(h);
        g_buttons.fetch_or(m == WM_LBUTTONDOWN ? 1u : m == WM_RBUTTONDOWN ? 2u : 4u);
        g_presses[m == WM_LBUTTONDOWN ? 0 : m == WM_RBUTTONDOWN ? 1 : 2].fetch_add(1);
        g_armed.store(true);
        if (!g_captured.load() && g_on.load() && !released() && GetForegroundWindow() == h && !pc_overlay_open()) {
            set_capture(h, true, "click");
        }
        clip_apply(h);   // (windowed, a menu: armed by this click inside, confined from it; pc_clip.h)
        break;
    case WM_ENTERSIZEMOVE: g_moving.store(true); clip_apply(h, true); break;   // (a title-bar drag, Alt+Space Move / Size)
    case WM_EXITSIZEMOVE: g_moving.store(false); clip_apply(h); break;
    case WM_APP_CAPTURE:   // (a capture posted just before an alt-tab lands after it: not while another window is in front)
        g_pending.store(false);
        if (!w || GetForegroundWindow() == h) set_capture(h, w != 0, (const char*)l);
        return 0;
    case WM_APP_RECLIP: {   // (clip_check found it differs, or a window mode change: wParam 1, expected, not logged)
        g_reclip_posted.store(false);
        const bool was_ours = g_clip_ours.load();
        RECT had = {};
        GetClipCursor(&had);
        const RECT old = g_clip_rect;
        // (a move / resize also lands here, the client area changed: Windows still had ours then, not a drop)
        if (clip_apply(h) == pcclip::CLIP && was_ours && !w && !EqualRect(&had, &old)) clip_again_log(h, had, "at an input read");
        return 0;
    }
    case WM_ACTIVATE:
        // (a click-activation of a window arms on the click inside it: WM_LBUTTONDOWN, a title-bar drag stays a drag)
        if (LOWORD(w) == WA_INACTIVE) {  // (and no clip outlives the focus, the menu cursor's included)
            g_armed.store(false); g_buttons.store(0); set_capture(h, false, "window inactive");
            clip_apply(h, true);
        } else {
            if (LOWORD(w) == WA_ACTIVE || shim_window_fullscreen()) g_armed.store(true);
            clip_apply(h);   // (fullscreen: confined again at once, before the first read)
        }
        break;
    case WM_KILLFOCUS: set_capture(h, false, "focus lost"); clip_apply(h, true); break;
    case WM_SETFOCUS: clip_apply(h); break;
    case WM_SIZE: case WM_MOVE: case WM_DISPLAYCHANGE: clip_apply(h); break;   // (the new client area; a resolution change)
    case WM_SETCURSOR:
        if (g_captured.load() && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        if (released() && LOWORD(l) == HTCLIENT) { SetCursor(pointer()); return TRUE; }
        break;
    case WM_APP_CURSOR: {   // the menu cursor on / off: the pointer stays confined, shown; from a capture,
        RECT s;             // at the client centre (else where Windows last had it)
        if (w && l && client_rect(h, &s)) SetCursorPos((s.left + s.right) / 2, (s.top + s.bottom) / 2);
        clip_apply(h);
        SetCursor(w ? pointer() : NULL);
        return 0;
    }
    case WM_APP_POINTER: {   // the device in use changed (pc_mouse_pad_in_use): the released pointer, if over the client area
        RECT s; POINT p;
        if (released() && !g_captured.load() && client_rect(h, &s) && GetCursorPos(&p) && PtInRect(&s, p)) SetCursor(pointer());
        return 0;
    }
    }
    return CallWindowProcW(g_orig, h, m, w, l);
}

// TOTK_CAMLOG=<ms>[:<hex offset>[i],...] (tests): every <ms>, the PlayerCamera's (data page +136) processed stick +0x840 /
// +0x844 (where the veneer adds the mouse) and the floats at the offsets given (with i: the 32-bit word in hex; +0x88 ..
// +0x90 is the look direction), with the mouse totals: "[cam] ..." lines.
bool cam_floats_try(uint64_t cam, const unsigned* off, int n, float* out) {
    __try {
        for (int i = 0; i < n; i++) out[i] = *(const volatile float*)(cam + off[i]);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool cam_floats(uint64_t cam, const unsigned* off, int n, float* out) {   // (on_crash is vectored: pc.h)
    pc_fault_guard(1);
    const bool r = cam_floats_try(cam, off, n, out);
    pc_fault_guard(0);
    return r;
}
float page_f32(uint8_t* p, unsigned off);   // (below)
DWORD WINAPI camlog(void* arg) {
    unsigned off[16] = {0x840, 0x844};
    bool word[16] = {};
    int n = 2;
    const char* e = (const char*)arg;
    int ms = atoi(e);
    for (const char* c = strchr(e, ':'); c && n < 16; c = strchr(c + 1, ',')) {
        char* end;
        off[n] = (unsigned)strtoul(c + 1, &end, 16);
        word[n++] = *end == 'i';
    }
    if (ms < 5) ms = 5;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(ms);
        uint8_t* p = page();
        uint64_t cam = p ? __atomic_load_n((uint64_t*)(p + 136), __ATOMIC_ACQUIRE) : 0;
        float v[16];
        if (!cam || !cam_floats(cam, off, n, v)) continue;
        AcquireSRWLockShared(&g_lock); double tx = g_tx, ty = g_ty; ReleaseSRWLockShared(&g_lock);
        QueryPerformanceCounter(&t);
        char line[512];
        // (+ the pitch handshake: the veneer's ack at data page +132 and the gain g at +128)
        uint8_t* dp = page();
        const uint32_t ack = dp ? __atomic_load_n((uint32_t*)(dp + kPitchAck), __ATOMIC_ACQUIRE) : 0;
        const float gain = dp ? page_f32(dp, kPitchG) : 0.0f;
        int k = snprintf(line, sizeof line, "[cam] t=%.3f mouse %.1f %.1f claimed %d ack 0x%08x g %.3f", (double)(t.QuadPart - t0.QuadPart) / f.QuadPart,
                         tx, ty, page_claimed() ? 1 : 0, ack, gain);
        for (int i = 0; i < n && k < (int)sizeof line - 24; i++) {
            uint32_t w;
            memcpy(&w, &v[i], 4);
            k += word[i] ? snprintf(line + k, sizeof line - k, " %x=%08x", off[i], w)
                         : snprintf(line + k, sizeof line - k, " %x=%.5f", off[i], v[i]);
        }
        printf("%s\n", line);
    }
}
float page_f32(uint8_t* p, unsigned off) {
    uint32_t w = __atomic_load_n((uint32_t*)(p + off), __ATOMIC_ACQUIRE);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
// (a 1 ms high-resolution waitable timer: Sleep(1) is 15.6 ms without the process-wide timeBeginPeriod, so pitch came in uneven chunks squared with the
// wrong step's ratio; idle while the camera is off. Keys from 985ad43 on square in the veneer, which holds dy and k of the step it applies: there this
// thread hands pitch over at the ack, and takes it back if the ack drops.)
DWORD WINAPI pitch_feed(void*) {   // (see kPitchGain)
    uint32_t last_beat = 0;
    double raw_pub = 0, pub = 0;
    HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    for (;;) {
        if (!g_on.load()) { Sleep(100); continue; }
        LARGE_INTEGER due; due.QuadPart = -10000;   // 1 ms
        if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tm, 5);
        else Sleep(1);
        uint8_t* p = page();
        if (!p) continue;
        const uint32_t beat = __atomic_load_n((uint32_t*)(p + kBeat), __ATOMIC_ACQUIRE);
        if (beat == last_beat) continue;
        {  // (the handshake's reader, for the check: the page, the first beat, then the ack it sees every 5 s)
            static bool first = true;
            static ULONGLONG next_log;
            static int logs;
            const uint32_t a = __atomic_load_n((uint32_t*)(p + kPitchAck), __ATOMIC_ACQUIRE);
            if (first) { first = false; printf("[mouse] pitch_feed: data page %p, first beat %u, ack %u\n", (void*)p, beat, a); }
            else if (GetTickCount64() >= next_log && logs < 12) {
                logs++; next_log = GetTickCount64() + 5000;
                printf("[mouse] pitch_feed: beat %u, ack (+132) 0x%08x, g (+128) 0x%08x\n", beat, a, __atomic_load_n((uint32_t*)(p + kPitchG), __ATOMIC_ACQUIRE));
            }
        }
        if (__atomic_load_n((uint32_t*)(p + kPitchAck), __ATOMIC_ACQUIRE) >= 1) {   // the veneer shapes pitch from here on
            const float g = (float)kPitchGain;
            uint32_t gw;
            memcpy(&gw, &g, 4);
            AcquireSRWLockExclusive(&g_lock);
            g_ty_off = pub - raw_pub;   // (motion since the last publish goes to the veneer, shaped there)
            __atomic_store_n((uint32_t*)(p + kPitchG), gw, __ATOMIC_RELEASE);
            g_pitch_veneer.store(true, std::memory_order_release);
            page_totals();
            ReleaseSRWLockExclusive(&g_lock);
            printf("[mouse] pitch: the veneer shapes it (ack at +%u), g %.4f at +%u; pitch_feed off\n", kPitchAck, kPitchGain, kPitchG);
            // (then a look at the ack every 100 ms: should a load set the page up again, ack and g back to 0, pitch_feed
            // takes pitch back from the value published now, and the switch above runs again at the next ack)
            for (;;) {
                Sleep(100);
                uint8_t* q = page();
                if (!q) continue;
                if (__atomic_load_n((uint32_t*)(q + kPitchAck), __ATOMIC_ACQUIRE) >= 1) {
                    // (a set-up page whose next camera step stored the ack again within this poll: ack 1, g 0; g is all
                    // that case needs. The g the context wants: 0, linear, in an ability, mode_gain's)
                    const float want = pitch_gain_now();
                    uint32_t ww;
                    memcpy(&ww, &want, 4);
                    if (__atomic_load_n((uint32_t*)(q + kPitchG), __ATOMIC_ACQUIRE) != ww) {
                        __atomic_store_n((uint32_t*)(q + kPitchG), ww, __ATOMIC_RELEASE);
                        static int told;
                        if (told++ < 8) printf("[mouse] pitch: g at +%u set to %.4f (%s)\n", kPitchG, want,
                                               want == 0 ? "an ability: linear" : "on foot, or the page set up again");
                    }
                    continue;
                }
                AcquireSRWLockExclusive(&g_lock);
                g_pitch_veneer.store(false, std::memory_order_release);
                raw_pub = g_ty;
                pub = g_ty + g_ty_off;
                ReleaseSRWLockExclusive(&g_lock);
                last_beat = __atomic_load_n((uint32_t*)(q + kBeat), __ATOMIC_ACQUIRE);
                printf("[mouse] pitch: the veneer's ack at +%u went back to 0 (the page set up again): pitch_feed on\n", kPitchAck);
                break;
            }
            continue;
        }
        const bool stale = beat - last_beat > 3;  // TOTK_MOUSE_STALE_PRESENTS: the veneer drops that motion
        last_beat = beat;
        AcquireSRWLockShared(&g_lock); const double raw = g_ty; ReleaseSRWLockShared(&g_lock);
        const double d = raw - raw_pub;
        raw_pub = raw;
        if (d == 0 || stale) continue;   // (stale: no camera steps meanwhile, the map, menus, a cutscene: resynced)
        const float ratio = page_f32(p, kRatio), rate = page_f32(p, kMouseRate), mult = page_f32(p, kMouseScale);
        double k = ratio > 0.01f && ratio < 100 && rate > 0 && mult > 0 ? (double)rate * mult / ratio : 0.04;
        pub += copysign(sqrt(kPitchGain * fabs(d) / k), d);
        uint64_t y;
        memcpy(&y, &pub, 8);
        __atomic_store_n((uint64_t*)(p + kPcPage + kMouseY), y, __ATOMIC_RELEASE);
    }
}
// TOTK_LATE_LATCH=1 (the mouse late-latch prototype): the live block at data page +144..+175 (data_page.h
// TOTK_DP_LATCH_*), at 1 kHz on its own high-resolution timer: the PC page's mouse totals (the counter the veneer's
// TOTK_DP_MOUSE_LAST holds as consumed) with the QPC they were read at, under a seqlock, and bit 0 of the flags when the
// camera is the aim / first-person kind the prototype latches, whatever drew it (latch_aim). Off: no thread, no writes.
// (pitch_feed cannot carry it: it idles at a 100 ms poll once the veneer shapes pitch.)
const unsigned kLatchSeq = 144, kLatchFlags = 148, kLatchTotals = 152, kLatchQpc = 168;
// Bit 0 from the player, not the router's context alone: the bow in hand (TPS_AIM) with the game's own ZR held (the
// buttons it was given last, any device, after the pad bindings), on foot, in the air or swimming (from_player's order:
// the saddle, a vehicle, the glider and a wall stay out); or the Purah Pad camera / the Scope (TPS_CAMERA); or the
// router's AIM / CAMERA / SCOPE (the PC bow macro draws through the virtual keys, not ZR).
bool latch_aim() {
    const int c = pc_ctx();
    if (c == PCX_AIM || c == PCX_CAMERA || c == PCX_SCOPE) return true;
    const uint32_t st = totk_player_state();
    if (!(st & TPS_VALID)) return false;
    if (st & TPS_CAMERA) return true;
    // (ZR with no bow equipped is the empty aim stance's false 59 for ~1 s, the lat55r on a bowless save: not an
    // aim. As totk_player_state.cpp's hand latch, only where the hands read works: an _EQUIPPED bit shows)
    const bool hands_read = (st & (TPS_WEAPON_EQUIPPED | TPS_BOW_EQUIPPED | TPS_SHIELD_EQUIPPED)) != 0;
    if (hands_read && !(st & TPS_BOW_EQUIPPED)) return false;
    return (st & TPS_AIM) && !(st & (TPS_HORSE | TPS_VEHICLE | TPS_GLIDE | TPS_CLIMB)) && (hid_game_buttons() & (1ull << 9));
}
DWORD WINAPI latch_feed(void*) {
    HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    uint32_t seq = 0;
    printf("[mouse] late latch: the live block at +%u..+%u, 1 kHz (TOTK_LATE_LATCH)\n", kLatchSeq, kLatchQpc + 7);
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -10000;   // 1 ms
        if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tm, 5);
        else Sleep(1);
        uint8_t* p = page();
        if (!p) continue;
        const uint64_t x = __atomic_load_n((uint64_t*)(p + kPcPage + kMouseX), __ATOMIC_ACQUIRE);
        const uint64_t y = __atomic_load_n((uint64_t*)(p + kPcPage + kMouseY), __ATOMIC_ACQUIRE);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const uint32_t flags = latch_aim() ? 1u : 0u;
        __atomic_store_n((uint32_t*)(p + kLatchSeq), ++seq, __ATOMIC_RELEASE);   // (odd: being written)
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n((uint32_t*)(p + kLatchFlags), flags, __ATOMIC_RELAXED);
        __atomic_store_n((uint64_t*)(p + kLatchTotals), x, __ATOMIC_RELAXED);
        __atomic_store_n((uint64_t*)(p + kLatchTotals + 8), y, __ATOMIC_RELAXED);
        __atomic_store_n((int64_t*)(p + kLatchQpc), (int64_t)now.QuadPart, __ATOMIC_RELAXED);
        __atomic_store_n((uint32_t*)(p + kLatchSeq), ++seq, __ATOMIC_RELEASE);   // (even: done)
    }
}
// Input thread: finds the game window (class TotKRecomp, this process) once it exists and subclasses it.
// (from the input thread and, through the menu cursor's calls, the game thread: one at a time, or the window could be
// subclassed twice and its procedure call itself)
void attach() {
    static std::atomic<bool> busy{false};
    if (g_hwnd || busy.exchange(true)) return;
    struct Done { ~Done() { busy.store(false); } } done;
    static bool feed_started;
    if (!feed_started) {
        feed_started = true;
        CreateThread(nullptr, 0, pitch_feed, nullptr, 0, nullptr);
        if (const char* e = getenv("TOTK_LATE_LATCH"); e && !strcmp(e, "1")) CreateThread(nullptr, 0, latch_feed, nullptr, 0, nullptr);
    }
    static bool cam_started;
    if (!cam_started) {
        cam_started = true;
        if (const char* e = getenv("TOTK_CAMLOG"); e && *e) CreateThread(nullptr, 0, camlog, (void*)e, 0, nullptr);
    }
    static ULONGLONG next;
    if (g_hwnd || GetTickCount64() < next) return;
    next = GetTickCount64() + 500;
    HWND h = NULL;
    while ((h = FindWindowExW(NULL, h, L"TotKRecomp", NULL))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) break;
    }
    if (!h) return;
    RAWINPUTDEVICE d = {0x01, 0x02, 0, h};   // generic desktop / mouse, to this window while it has focus
    if (!RegisterRawInputDevices(&d, 1, sizeof d)) { printf("[mouse] RegisterRawInputDevices failed (%lu): camera off\n", GetLastError()); next = ~0ull; return; }
    g_orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)proc);
    g_hwnd = h;
    if (GetForegroundWindow() == h) g_armed.store(true);
}
// Input thread, every read: the pointer captured exactly when it should be, and still confined.
void policy() {
    if (!g_hwnd) return;
    // (the buttons held at a capture: cleared here, at every read, as each is released; pc_mouse_button_ok is only asked
    // while a button is down, so it would never see the release)
    if (g_swallow.load()) g_swallow.fetch_and(held_buttons());
    const bool active = GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd);
    const bool overlay = pc_overlay_open();
    const bool cursor = released();
    const bool on = g_on.load() && active && !overlay && !cursor && (shim_window_fullscreen() || g_armed.load());
    if (on != g_captured.load())
        want(on, on ? "window active, overlay closed" : !g_on.load() ? "camera off" : overlay ? "settings overlay open"
                 : cursor ? (g_ui.load() ? "menu context" : "menu cursor") : !active ? "window inactive" : "window not clicked yet");
}
// Input thread, every read (pc_mouse_ui: the camera on or off): the confinement as pc_clip.h wants it, else the window
// thread redoes it (Windows drops it on an app switch, and on its own). A few calls per read (GetForegroundWindow,
// GetClientRect, GetClipCursor); one fix posted at a time.
void clip_check() {
    if (!g_hwnd) return;
    RECT want_r = {};
    if (clip_decide(g_hwnd, false, &want_r) != pcclip::KEEP && !g_reclip_posted.exchange(true) &&
        !PostMessageW(g_hwnd, WM_APP_RECLIP, 0, 0))
        g_reclip_posted.store(false);
}
bool captured_now() { return g_captured.load() || GetTickCount64() < g_injected_until.load(); }
}   // namespace

// The settings overlay (hle_hid.cpp's controls rows): the TOTK_MOUSE_* variables changed, read them again.
extern "C" void pc_mouse_reload(void) {
    reload();
    if (!g_on.load()) want(false, "camera off");
}
// hle_hid.cpp TOTK_CTL "mouse <dx> <dy>": raw counts as if the mouse had moved while captured (unattended tests of the
// mouse camera and the mouse gyro); for 1 s after it the stick / gyro paths and the veneer treat the mouse as captured.
extern "C" void pc_mouse_inject(double dx, double dy) {
    if (!enabled()) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_redirect.load()) { g_rx += dx; g_ry += dy; }   // (a macro has the mouse)
    else {
        g_dx += dx; g_dy += dy;
        double gx, gy;
        mode_gain(&gx, &gy);
        g_tx += dx * g_sens * gx; g_ty += dy * g_sens * g_inv * gy;
        page_totals();
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_injected_until.store(GetTickCount64() + 1000);
    page_captured(true);
}
extern "C" bool pc_mouse_injected(void) { return GetTickCount64() < g_injected_until.load(); }
// TOTK_CTL "wheel <n>": notches as if scrolled while captured.
extern "C" void pc_mouse_inject_wheel(double n) {
    if (n && pc_input_capture_wheel(n > 0 ? 1 : -1)) return;   // (a capture waiting for a notch takes it, as raw input's)
    if (!enabled()) return;
    AcquireSRWLockExclusive(&g_lock); g_wheel += n; ReleaseSRWLockExclusive(&g_lock);
    g_injected_until.store(GetTickCount64() + 1000);
}
// hle_hid.cpp: whole wheel notches since the last take (the fraction stays), while captured; 0 otherwise.
extern "C" int pc_mouse_wheel_take(void) {
    if (!enabled()) return 0;
    AcquireSRWLockExclusive(&g_lock);
    int n = (int)g_wheel;   // toward zero
    g_wheel -= n;
    if (!captured_now() && !released()) { n = 0; g_wheel = 0; }
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}
// hle_hid.cpp keyboard_read: whether a mouse button bound to an action counts now. Without TOTK_MOUSE_CAMERA always (the
// window is in front); with it only while the pointer is captured, and not a button that was already held when it was
// captured, until it is released (the capturing click must not also attack, nor the closing right click).
extern "C" bool pc_mouse_button_ok(int vk) {
    if (!enabled()) return true;
    if (!g_captured.load()) return false;
    const unsigned bit = vk == VK_LBUTTON ? 1 : vk == VK_RBUTTON ? 2 : vk == VK_MBUTTON ? 4 : 0;
    return !(g_swallow.load() & bit);   // (cleared by policy() once released)
}
// totk_menu_cursor.cpp, at each look on the game thread: the pointer is over one of a menu's focus-box items (the
// Purah Pad's tabs and lists), which its own hover / clicks serve. hle_hid.cpp's map clicks and drags in cursor mode
// take only presses that start off them, so a click on a tab is not also a pin on the map.
extern "C" bool pc_mouse_client_pos(float* nx, float* ny);   // (below)
static std::atomic<bool> g_over_item{false};
extern "C" void pc_mouse_cursor_over_item(bool over) { g_over_item.store(over); }
extern "C" bool pc_mouse_cursor_on_item(void) { return g_over_item.load(); }
// hle_hid.cpp mouse_read, the Purah Pad's map in cursor mode (the pointer released and shown there): the left stick from
// the pointer's own motion in client pixels, smoothed and scaled as pc_mouse_stick_counts does raw counts while captured
// (not inverted: it is a cursor); cdx / cdy the pixels moved since the last call. Input thread only.
extern "C" void pc_mouse_cursor_stick(int32_t* rx, int32_t* ry, double* cdx, double* cdy) {
    static bool have;
    static double px, py, vx, vy;
    static LARGE_INTEGER last;
    *cdx = *cdy = 0;
    float nx, ny;
    RECT r;
    if (!released() || !g_hwnd || !GetClientRect(g_hwnd, &r) || r.right <= 0 || r.bottom <= 0 || !pc_mouse_client_pos(&nx, &ny)) {
        have = false; vx = vy = 0;
        return;
    }
    const double x = nx * r.right, y = ny * r.bottom;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    const double dt = have ? (double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart : 0;
    const double dx = have ? x - px : 0, dy = have ? y - py : 0;
    px = x; py = y; last = now; have = true;
    *cdx = dx; *cdy = dy;
    if (dt > 0 && dt < 0.25) {
        const double a = 1 - exp(-dt / 0.025);
        vx += (dx / dt - vx) * a; vy += (dy / dt - vy) * a;
    } else vx = vy = 0;
    const double full = g_full / (g_sens * g_scale);
    const double sx = vx / full, sy = -vy / full, m = sqrt(sx * sx + sy * sy);
    if (m < 0.01) return;
    const double k = (0.2 + 0.8 * fmin(m, 1.0)) / m;
    *rx = (int32_t)(fmax(-1.0, fmin(1.0, sx * k)) * 32767); *ry = (int32_t)(fmax(-1.0, fmin(1.0, sy * k)) * 32767);
}
// hle_hid.cpp mouse-as-gyro: the counts since the last read, scaled by the sensitivity and Y inversion (dy > 0 = down).
// False while the pointer is not captured (nothing taken). The stick's velocity restarts from zero afterwards.
extern "C" bool pc_mouse_take(double* dx, double* dy) {
    *dx = *dy = 0;
    if (!enabled()) return false;
    attach();
    policy();
    AcquireSRWLockExclusive(&g_lock);
    double x = g_dx, y = g_dy; g_dx = g_dy = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_vx = g_vy = 0; g_last.QuadPart = 0;
    if (!captured_now()) return false;
    const double aim = aim_gain();   // (the bow drawn: controls.mouse_aim_scale, before mouse_gyro_speed)
    *dx = x * g_sens * aim; *dy = y * g_sens * g_inv * aim;
    return true;
}
extern "C" bool pc_mouse_enabled(void) { return enabled(); }
// totk_menu_cursor.cpp: a menu that takes the mouse as a cursor is open (on) or closed (off). While on the pointer is
// released for the camera (shown, still confined; no camera counts, mouse buttons bound to actions do not count), and
// the capture policy takes it back by itself when it goes off.
extern "C" void pc_mouse_cursor_mode(bool on) {
    attach();  // (the menu cursor must not depend on the camera path having run first: the unattended boots)
    const bool was = g_captured.load();
    if (g_cursor.exchange(on) == on) return;
    printf("[mouse] menu cursor %s\n", on ? "on" : "off");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CURSOR, released(), was);
}
// The mouse buttons down in the client area (bit 0 left, 1 right, 2 middle), as the window's button messages say,
// TOTK_CTL "click" included (GetAsyncKeyState does not see posted ones). Cleared when the window goes inactive.
extern "C" unsigned pc_mouse_buttons(void) { attach(); return g_buttons.load(); }
// Presses of button b (0 left, 1 right, 2 middle) since the start, wrapping: a reader that diffs it sees a click that was
// already released by its next read (a 100 ms click inside a frame hitch, the mc_real8).
extern "C" unsigned pc_mouse_presses(int b) { attach(); return b >= 0 && b < 3 ? g_presses[b].load() : 0; }
extern "C" bool pc_mouse_cursor_on(void) { return released(); }  // (the mode or a UI context)
// pc_input.cpp (input thread): a macro takes the mouse (on) or gives it back; what was not taken is dropped either way
extern "C" void pc_mouse_redirect(bool on) {
    if (g_redirect.exchange(on) == on) return;
    AcquireSRWLockExclusive(&g_lock); g_rx = g_ry = 0; ReleaseSRWLockExclusive(&g_lock);
    static int logs;
    if (logs++ < 100) printf("[mouse] %s\n", on ? "a PC controls macro takes the mouse (the camera gets none)" : "the camera has the mouse again");
}
// the raw counts since the last take, while redirected (sensitivity not applied)
extern "C" void pc_mouse_redirect_take(double* dx, double* dy) {
    AcquireSRWLockExclusive(&g_lock);
    *dx = g_rx; *dy = g_ry; g_rx = g_ry = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
// hle_hid.cpp, once per read: the PC scheme's context is a menu, the title or a dialogue (pc_input.cpp pc_ctx_is_ui)
extern "C" void pc_mouse_ui(bool on) {
    attach();
    clip_check();
    const bool was = g_captured.load();
    if (g_ui.exchange(on) == on) return;
    static int logs;
    if (logs++ < 100) printf("[mouse] %s context: pointer %s\n", on ? "menu" : "gameplay", on ? "released" : "back to the policy");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_CURSOR, released(), was);
}
// hle_hid.cpp (input thread): the device the prompts show changed, a pad (on) or the keyboard and mouse
extern "C" void pc_mouse_pad_in_use(bool on) {
    if (g_pad_hide.exchange(on) == on) return;
    static int logs;
    if (logs++ < 100) printf("[mouse] %s\n", on ? "a pad is in use: menus hide the pointer until the mouse moves" : "the pointer shows in menus");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_POINTER, 0, 0);
}
// shim_win.c (window thread), around a window mode change (F11, Alt+Enter, the settings' window mode / monitor): the
// capture is kept. The style and size changes re-clip through WM_SIZE / WM_MOVE; after them the confinement is redone
// once more (an exclusive display mode moves the desktop under it), and a capture that a deactivation in between dropped
// comes back: the window counts as armed, so the policy takes the pointer again at the next read (no click needed).
extern "C" void pc_mouse_mode_change(bool after) {
    static bool had;
    if (!after) { had = g_captured.load(); return; }
    if (!g_hwnd) return;
    PostMessageW(g_hwnd, WM_APP_RECLIP, 1, 0);   // (captured or not: a released pointer is confined too)
    if (!g_captured.load() && had && GetForegroundWindow() == g_hwnd) {
        g_armed.store(true);
        printf("[mouse] window mode changed: the capture comes back\n");
    }
}
// hle_hid.cpp host_input: the released pointer moved on purpose (or clicked) since the last call
extern "C" bool pc_mouse_pointer_moved(void) { return g_pointer_moved.exchange(false); }
// totk_menu_cursor.cpp: the released pointer is hidden for a pad (its small moves, under the gate, are not hovers)
extern "C" bool pc_mouse_pointer_hidden(void) { return g_pad_hide.load(); }
// TOTK_CTL "cursor <nx> <ny> [ms]" (the unattended tests): pc_mouse_client_pos gives this for 1 s; with ms, a glide
// there from the last injected point over ms (read by read, as a hand drags: the map's drag needs motion in every read,
// which jumps a poll apart never give), then the 1 s.
static std::atomic<ULONGLONG> g_cur_until{0}, g_cur_t0{0}, g_cur_ms{0};
static std::atomic<float> g_cur_x{0.5f}, g_cur_y{0.5f}, g_cur_x0{0.5f}, g_cur_y0{0.5f};
extern "C" void pc_mouse_inject_cursor(double nx, double ny, double ms) {
    const ULONGLONG now = GetTickCount64();
    const bool live = now < g_cur_until.load();
    g_cur_x0.store(live ? g_cur_x.load() : (float)nx); g_cur_y0.store(live ? g_cur_y.load() : (float)ny);
    g_cur_x.store((float)nx); g_cur_y.store((float)ny);
    g_cur_ms.store(ms > 0 ? (ULONGLONG)ms : 0); g_cur_t0.store(now);
    g_cur_until.store(now + (ms > 0 ? (ULONGLONG)ms : 0) + 1000);
}
// The pointer in the client area, 0..1 (false outside it or with no window).
extern "C" bool pc_mouse_client_pos(float* nx, float* ny) {
    const ULONGLONG now = GetTickCount64();
    if (now < g_cur_until.load()) {
        const ULONGLONG ms = g_cur_ms.load(), t = now - g_cur_t0.load();
        const float k = ms && t < ms ? (float)t / (float)ms : 1.0f;
        *nx = g_cur_x0.load() + (g_cur_x.load() - g_cur_x0.load()) * k;
        *ny = g_cur_y0.load() + (g_cur_y.load() - g_cur_y0.load()) * k;
        return *nx >= 0 && *ny >= 0 && *nx < 1 && *ny < 1;
    }
    POINT p;
    RECT r;
    if (!g_hwnd || !GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r) || r.right <= 0 || r.bottom <= 0)
        return false;
    *nx = (float)p.x / (float)r.right; *ny = (float)p.y / (float)r.bottom;
    return p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
}
// hle_hid.cpp keyboard_read: a menu screen has the mouse (the map's cursor, the menu keys): the stick from the mouse
// (the cursor) ignores the camera's Y inversion there.
extern "C" void pc_mouse_menu(bool on) { g_menu.store(on); }
// hle_hid.cpp keyboard_read (input refresh, game window in front): right-stick deflection from the mouse while captured
// and the veneer does not apply it (the map's cursor, menus, an unpatched key). pc_mouse_stick_counts also gives the raw
// counts this read took (0 while not captured): the map's drag tells a click from a drag by them.
extern "C" void pc_mouse_stick_counts(int32_t* rx, int32_t* ry, double* cdx, double* cdy) {
    *cdx = *cdy = 0;
    if (!enabled()) return;
    attach();
    policy();
    if (pc_mouse_injected()) page_captured(true);
    else if (!g_captured.load()) page_captured(false);
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    double dt = g_last.QuadPart ? (double)(now.QuadPart - g_last.QuadPart) / (double)f.QuadPart : 0;
    g_last = now;
    AcquireSRWLockExclusive(&g_lock);
    double dx = g_dx, dy = g_dy; g_dx = g_dy = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (!captured_now()) { g_vx = g_vy = 0; return; }
    *cdx = dx; *cdy = dy;
    if (page_claimed() && !g_menu.load()) { g_vx = g_vy = 0; return; }   // the veneer turns the camera by the totals
    if (dt > 0 && dt < 0.25) {
        double a = 1 - exp(-dt / 0.025);
        g_vx += (dx / dt - g_vx) * a; g_vy += (dy / dt - g_vy) * a;
    } else g_vx = g_vy = 0;
    double full = g_full / (g_sens * g_scale * (g_menu.load() ? 1 : aim_gain()));   // (the bow drawn: controls.mouse_aim_scale)
    double sx = g_vx / full, sy = -g_vy / full * (g_menu.load() ? 1 : g_inv);   // stick +y = up (a menu's cursor: not inverted)
    double m = sqrt(sx * sx + sy * sy);
    if (m < 0.01) return;   // still: the pad's or the keys' stick stays
    double k = (0.2 + 0.8 * fmin(m, 1.0)) / m;
    *rx = (int32_t)(fmax(-1.0, fmin(1.0, sx * k)) * 32767); *ry = (int32_t)(fmax(-1.0, fmin(1.0, sy * k)) * 32767);
}
extern "C" void pc_mouse_stick(int32_t* rx, int32_t* ry) { double x, y; pc_mouse_stick_counts(rx, ry, &x, &y); }
#else
extern "C" void pc_mouse_stick(int32_t*, int32_t*) {}
extern "C" void pc_mouse_stick_counts(int32_t*, int32_t*, double* cdx, double* cdy) { *cdx = *cdy = 0; }
extern "C" bool pc_mouse_button_ok(int) { return true; }
extern "C" bool pc_mouse_take(double* dx, double* dy) { *dx = *dy = 0; return false; }
extern "C" bool pc_mouse_enabled(void) { return false; }
extern "C" void pc_mouse_menu(bool) {}
extern "C" void pc_mouse_inject(double, double) {}
extern "C" void pc_mouse_inject_wheel(double) {}
extern "C" int pc_mouse_wheel_take(void) { return 0; }
extern "C" bool pc_mouse_injected(void) { return false; }
extern "C" void pc_mouse_cursor_mode(bool) {}
extern "C" bool pc_mouse_client_pos(float*, float*) { return false; }
extern "C" void pc_mouse_inject_cursor(double, double, double) {}
extern "C" unsigned pc_mouse_buttons(void) { return 0; }
extern "C" unsigned pc_mouse_presses(int) { return 0; }
extern "C" bool pc_mouse_cursor_on(void) { return false; }
extern "C" void pc_mouse_redirect(bool) {}
extern "C" void pc_mouse_redirect_take(double* dx, double* dy) { *dx = *dy = 0; }
extern "C" void pc_mouse_ui(bool) {}
extern "C" void pc_mouse_pad_in_use(bool) {}
extern "C" bool pc_mouse_pointer_moved(void) { return false; }
extern "C" bool pc_mouse_pointer_hidden(void) { return false; }
extern "C" void pc_mouse_mode_change(bool) {}
extern "C" void pc_mouse_cursor_over_item(bool) {}
extern "C" bool pc_mouse_cursor_on_item(void) { return false; }
extern "C" void pc_mouse_cursor_stick(int32_t*, int32_t*, double* cdx, double* cdy) { *cdx = *cdy = 0; }
#endif
