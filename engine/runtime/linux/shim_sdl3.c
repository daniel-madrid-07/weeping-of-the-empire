// TotK Linux port: the window / event glue of the LASRecomp Vulkan backend on REAL SDL3 (counterpart of nvn/vulkan/shim_win.c, which
// implements the declare-only SDL3 subset on Win32), plus the same LASRecomp app-layer stand-ins (settings, menu, input, texpatch).
// Linux build only (mkninja_linux.py); nothing globs runtime/linux on Windows.
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wctype.h>

#include "../nvn/shim/las.h"
// compat_posix.h (force-included) renamed these three; the wrappers below call the real ones
#undef SDL_WaitEventTimeout
#undef SDL_SetWindowTitle
#undef SDL_SavePNG
// (SDL.h was read with the renames in force, so its own declarations went to the totk_ names: declare the real ones)
extern SDL_DECLSPEC bool SDLCALL SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS);
extern SDL_DECLSPEC bool SDLCALL SDL_SetWindowTitle(SDL_Window *window, const char *title);

extern void totk_hid_sensor(int type, const float *data, Uint64 ts_ns);   // runtime/hle_hid.cpp
// the same receivers as shim_win.c's window procedure
extern bool pc_wndproc(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);   // pc_overlay.cpp: the F10 window
extern bool pc_overlay_open(void);
extern void pc_settings_save_pending(void);   // pc_settings.cpp
extern void pc_shot_request(void);            // pc_shot.cpp: F12
extern void oe_focus_update(bool focused);    // hle_oe.cpp: TOTK_PAUSE_UNFOCUSED
extern void pc_input_focus(bool focused);     // pc_input.cpp
extern void hid_key_event(int vk, bool down); // hle_hid.cpp: the key snapshot the PC layer reads
extern void hid_key_focus(bool front);
extern bool pc_input_capture_msg(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);   // pc_input.cpp: a key to bind
// pc_mouse.cpp: the mouse camera's events and its posted requests (what its window procedure takes on Windows)
extern void pc_mouse_sdl_motion(float xrel, float yrel);
extern void pc_mouse_sdl_button(int b, bool down);
extern void pc_mouse_sdl_wheel(float notches);
extern void pc_mouse_sdl_focus(bool on);
extern void pc_mouse_sdl_msg(unsigned m, uintptr_t w, intptr_t l);
extern int pc_mouse_sdl_cursor(void);

// ------------------------------------------------------------------ state read on the pump thread, copied out anywhere
// SDL's window and display calls belong to the thread that pumps its events; the runtime's other threads (the F10
// drawing side, the input thread) read these copies instead. The displays (pc_display_caps.cpp): name, desktop size in
// pixels, its refresh and the fastest at that size; read at the start and at every display event.
typedef struct { SDL_DisplayID id; char name[96]; int w, h, hz, top_hz; } DispInfo;
static pthread_mutex_t g_disp_lock = PTHREAD_MUTEX_INITIALIZER;
static DispInfo g_disp[8];
static int g_ndisp = -1;   // -1: not read yet
static SDL_DisplayID g_disp_primary;
static int px_of(int points, float density) { return (int)(points * density + 0.5f); }
static void displays_read(void) {
    DispInfo d[8];
    int nd = 0, n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    for (int i = 0; ids && i < n && nd < 8; i++) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(ids[i]);
        if (!m) continue;
        DispInfo *o = &d[nd++];
        memset(o, 0, sizeof *o);
        o->id = ids[i];
        snprintf(o->name, sizeof o->name, "%s", SDL_GetDisplayName(ids[i]) ? SDL_GetDisplayName(ids[i]) : "?");
        o->w = px_of(m->w, m->pixel_density), o->h = px_of(m->h, m->pixel_density);
        o->hz = o->top_hz = (int)(m->refresh_rate + 0.5f);
        int k = 0;
        SDL_DisplayMode **all = SDL_GetFullscreenDisplayModes(ids[i], &k);
        for (int j = 0; all && j < k; j++)
            if (px_of(all[j]->w, all[j]->pixel_density) == o->w && px_of(all[j]->h, all[j]->pixel_density) == o->h &&
                (int)(all[j]->refresh_rate + 0.5f) > o->top_hz)
                o->top_hz = (int)(all[j]->refresh_rate + 0.5f);
        SDL_free(all);
    }
    SDL_free(ids);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    pthread_mutex_lock(&g_disp_lock);
    memcpy(g_disp, d, sizeof d);
    g_ndisp = nd;
    g_disp_primary = primary;
    pthread_mutex_unlock(&g_disp_lock);
}
int shim_display_info(void *monitor, char *name, size_t name_n, int *w, int *h, int *hz, int *top_hz) {
    pthread_mutex_lock(&g_disp_lock);
    const bool unread = g_ndisp < 0;
    pthread_mutex_unlock(&g_disp_lock);
    // (none read yet: pc_settings.cpp's first start, a static initializer on the main thread before any window or pump:
    // read here once, video up first; SDL counts the inits)
    if (unread && (SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO))) displays_read();
    int found = 0;
    pthread_mutex_lock(&g_disp_lock);
    const SDL_DisplayID id = monitor ? (SDL_DisplayID)(uintptr_t)monitor : g_disp_primary;
    for (int i = 0; i < g_ndisp && !found; i++)
        if (g_disp[i].id == id) {
            snprintf(name, name_n, "%s", g_disp[i].name);
            *w = g_disp[i].w, *h = g_disp[i].h, *hz = g_disp[i].hz, *top_hz = g_disp[i].top_hz;
            found = 1;
        }
    pthread_mutex_unlock(&g_disp_lock);
    return found;
}
// The pointer (pc_mouse.cpp's input-thread reads): its window position, the window's size in the same units, the
// buttons held and whether it is over the window; the window minimised (IsIconic)
static pthread_mutex_t g_ptr_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_ptr_x, g_ptr_y;
static int g_ptr_ww, g_ptr_wh, g_ptr_over;
static unsigned g_ptr_buttons;   // bit 0 left, 1 right, 2 middle
static atomic_int g_minimized;
static void window_size_note(void) {
    int ww = 0, hh = 0;
    if (ui_window()) SDL_GetWindowSize(ui_window(), &ww, &hh);
    pthread_mutex_lock(&g_ptr_lock);
    g_ptr_ww = ww, g_ptr_wh = hh;
    pthread_mutex_unlock(&g_ptr_lock);
}
// the pointer in the window, 0..1, and the buttons held; 0 while it is not over the window
int shim_mouse_state(float *nx, float *ny, unsigned *buttons) {
    pthread_mutex_lock(&g_ptr_lock);
    const int over = g_ptr_over && g_ptr_ww > 0 && g_ptr_wh > 0;
    if (nx) *nx = over ? g_ptr_x / (float)g_ptr_ww : 0;
    if (ny) *ny = over ? g_ptr_y / (float)g_ptr_wh : 0;
    if (buttons) *buttons = g_ptr_buttons;
    pthread_mutex_unlock(&g_ptr_lock);
    return over;
}

// PostMessageW to the game window from any thread: pc_mouse.cpp's WM_APP + 50 .. 55 wait here for the UI thread
#define MOUSE_MSG_FIRST (0x8000 + 50)
#define MOUSE_MSG_LAST (0x8000 + 55)
static pthread_mutex_t g_msg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned m; uintptr_t w; intptr_t l; } g_msgs[64];
static int g_nmsgs;

// ------------------------------------------------------------------ window requests from other threads
// device.c calls SDL_SetWindowTitle from the present thread every 0.5 s: the text goes through g_title and is applied by the UI
// thread (the one that pumps events) inside totk_wait_event.
static pthread_mutex_t g_title_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_title[256];
static atomic_int g_title_dirty;
static atomic_int g_fs_req = -1;    // -1 none, 0 windowed, 1 fullscreen (SDL_SetWindowFullscreen from another thread)
// window modes as shim_win.c's: 0 windowed, 1 borderless (SDL's desktop fullscreen: the compositor / gamescope scans the
// window out directly), 2 exclusive (SDL's fullscreen with a display mode; Wayland has no mode switch, SDL scales that
// mode's picture to the output). Requests from other threads are applied by the thread that pumps events.
static atomic_int g_mode = 0, g_mode_req = -1, g_mon_req = -1, g_focus;
static _Atomic uint64_t g_size_req;   // a window size request, w << 32 | h, taken whole (0: none)
static int g_excl_w, g_excl_h, g_excl_hz;   // display.exclusive_size / _refresh (0: the desktop's)
static void apply_mode(int mode, int monitor);
static atomic_int g_fps_osd = -2, g_fps_osd_last = 1;   // the fps readout as shim_win.c's: 0 off, 1 minimal, 2 detailed

bool totk_set_window_title(SDL_Window *w, const char *title) {
    if (!w) return false;
    pthread_mutex_lock(&g_title_lock);
    snprintf(g_title, sizeof g_title, "%s", title);
    pthread_mutex_unlock(&g_title_lock);
    atomic_store(&g_title_dirty, 1);
    return true;
}

// Fullscreen = SDL's borderless desktop mode (the window covers the display at its native size; the compositor / gamescope can
// scan it out directly). The cursor hides over the game while fullscreen.
bool shim_window_fullscreen(void) {
    SDL_Window *w = ui_window();
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
}
// device.c ui_window_init, after its SDL_SetWindowFullscreen (or none): the start's mode (TOTK_WINDOW_MODE, TOTK_MONITOR,
// the exclusive size / rate) as shim_win.c applies it, in effect once SDL_SyncWindow returns (the compositor may apply it
// later otherwise)
static atomic_int g_started;
void shim_window_start_done(void) {
    SDL_Window *w = ui_window();
    if (w) {
        const char *wm = getenv("TOTK_WINDOW_MODE"), *mon = getenv("TOTK_MONITOR");
        const char *xs = getenv("TOTK_EXCLUSIVE_SIZE"), *xh = getenv("TOTK_EXCLUSIVE_HZ");
        if (xs && sscanf(xs, "%dx%d", &g_excl_w, &g_excl_h) != 2) g_excl_w = g_excl_h = 0;
        g_excl_hz = xh ? atoi(xh) : 0;
        const bool fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
        const int mode = !fs ? 0 : wm && !strcmp(wm, "exclusive") ? 2 : 1;
        if (mode == 2 || (mon && atoi(mon) > 0)) apply_mode(mode, mon ? atoi(mon) : -1);
        else atomic_store(&g_mode, mode);
        if (mode) SDL_HideCursor();
        SDL_SyncWindow(w);
        atomic_store(&g_focus, (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0);
        atomic_store(&g_minimized, (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) != 0);
        displays_read();   // (the copies the other threads read: this thread pumps SDL from here on)
        window_size_note();
        hid_key_focus(atomic_load(&g_focus) != 0);
    }
    atomic_store(&g_started, 1);
}
// The size the window's picture is meant to have: the display's desktop mode in fullscreen, else the window's pixels
// (textures.c, TOTK_OUT_SCALE's choice at start; shim_win.c has the Windows one). False until the start's mode is in
// effect (waits up to wait_ms).
bool shim_window_target_size(int *w, int *h, int wait_ms) {
    SDL_Window *win = ui_window();
    if (!win) return false;
    for (int t = 0; !atomic_load(&g_started) && t < wait_ms; t += 5) usleep(5000);
    if (!atomic_load(&g_started)) return false;
    if (shim_window_fullscreen()) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win));
        if (m && m->w > 0 && m->h > 0) {
            *w = (int)(m->w * m->pixel_density), *h = (int)(m->h * m->pixel_density);
            return true;
        }
    }
    return SDL_GetWindowSizeInPixels(win, w, h) && *w > 0 && *h > 0;
}
// The fps readout (as shim_win.c): 0 off, 1 minimal, 2 detailed; F3 shows the last style chosen or hides it
int shim_fps_overlay_mode(void) {
    if (atomic_load(&g_fps_osd) == -2) {  // (from -2 only: a set racing the first read wins)
        const char *e = getenv("TOTK_FPS_OVERLAY");
        int want = -2;
        atomic_compare_exchange_strong(&g_fps_osd, &want,
                                       !e ? 0 : !strcmp(e, "detailed") ? 2 : !strcmp(e, "minimal") || !strcmp(e, "1") ? 1 : 0);
    }
    return atomic_load(&g_fps_osd);
}
bool shim_fps_overlay(void) { return shim_fps_overlay_mode() != 0; }
// display.fps_readout, live (pc_settings.cpp fps_readout_apply / fps_readout_now)
void shim_set_fps_overlay(int mode) {
    const int m = mode == 2 ? 2 : mode == 1 ? 1 : 0;
    if (m) atomic_store(&g_fps_osd_last, m);
    atomic_store(&g_fps_osd, m);
}
// the display for a TOTK_MONITOR / display.monitor index (1..N in SDL's order), 0: the window's own
static SDL_DisplayID display_of(int monitor) {
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n), id = 0;
    if (ids && monitor >= 1 && monitor <= n) id = ids[monitor - 1];
    SDL_free(ids);
    return id ? id : ui_window() ? SDL_GetDisplayForWindow(ui_window()) : SDL_GetPrimaryDisplay();
}
extern void pc_mouse_mode_change(bool after);   // pc_mouse.cpp: the capture kept across a mode change (as shim_win.c)
static void apply_mode(int mode, int monitor) {
    SDL_Window *w = ui_window();
    if (!w) return;
    pc_mouse_mode_change(false);
    const SDL_DisplayID d = display_of(monitor > 0 ? monitor : 0);
    if (monitor > 0 && d != SDL_GetDisplayForWindow(w)) {   // (on the other display first: fullscreen takes the one it is on)
        if (atomic_load(&g_mode)) SDL_SetWindowFullscreen(w, false);
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(d), SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    }
    SDL_DisplayMode m;
    bool have = false;
    if (mode == 2) {   // the display's mode for the chosen size / rate (none chosen: the desktop's), in pixels
        const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(d);
        const int ww = g_excl_w ? g_excl_w : desk ? px_of(desk->w, desk->pixel_density) : 0;   // (rounded as the list)
        const int hh = g_excl_h ? g_excl_h : desk ? px_of(desk->h, desk->pixel_density) : 0;
        const float hz = g_excl_hz ? (float)g_excl_hz : desk ? desk->refresh_rate : 0.0f;
        // (SDL's modes are in points, w * pixel_density pixels: at 150-200 % scaling SDL_GetClosestFullscreenDisplayMode
        // given pixels picks another size. So the list, by pixels; of that size the fastest rate at or under the wanted one,
        // else the slowest over it: the row's own rule, "the fastest it has under it")
        int n = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
        const SDL_DisplayMode *best = NULL;
        for (int i = 0; modes && i < n; i++) {
            const SDL_DisplayMode *c = modes[i];
            if ((int)(c->w * c->pixel_density + 0.5f) != ww || (int)(c->h * c->pixel_density + 0.5f) != hh) continue;
            const bool under = c->refresh_rate <= hz + 0.5f;
            if (!best) { best = c; continue; }
            const bool best_under = best->refresh_rate <= hz + 0.5f;
            if (under ? (!best_under || c->refresh_rate > best->refresh_rate) : (!best_under && c->refresh_rate < best->refresh_rate))
                best = c;
        }
        if (best) { m = *best; have = true; }
        SDL_free(modes);
    }
    SDL_SetWindowFullscreenMode(w, have ? &m : NULL);
    SDL_SetWindowFullscreen(w, mode != 0);
    atomic_store(&g_mode, mode);
    if (mode) SDL_HideCursor(); else SDL_ShowCursor();
    pc_mouse_mode_change(true);
    if (have)
        fprintf(stderr, "[win] exclusive %dx%d pixels at %.2f Hz\n", (int)(m.w * m.pixel_density + 0.5f),
                (int)(m.h * m.pixel_density + 0.5f), m.refresh_rate);
    else if (mode == 2) fprintf(stderr, "[win] exclusive: the display has no mode of that size: its desktop's\n");
    else fprintf(stderr, "[win] %s\n", mode ? "borderless fullscreen" : "windowed");
}
// F11 / Alt+Enter: between windowed and the fullscreen kind of TOTK_WINDOW_MODE (borderless unless exclusive)
static void apply_fullscreen(bool fs) {
    SDL_Window *w = ui_window();
    if (!w || fs == shim_window_fullscreen()) return;
    const char *wm = getenv("TOTK_WINDOW_MODE");
    apply_mode(!fs ? 0 : wm && !strcmp(wm, "exclusive") ? 2 : 1, -1);
}
int shim_window_mode(void) { return atomic_load(&g_mode); }
void shim_set_window_mode(int mode, int monitor) {   // (pc_window.cpp's rows; monitor: -1 keep, 0 the window's own, 1..N)
    atomic_store(&g_mon_req, monitor);
    atomic_store(&g_mode_req, mode < 0 ? 0 : mode > 2 ? 2 : mode);
}
void shim_set_exclusive_mode(int w, int h, int hz) {
    g_excl_w = w, g_excl_h = h, g_excl_hz = hz;
    if (atomic_load(&g_mode) == 2) shim_set_window_mode(2, -1);
}
void shim_set_window_size(int w, int h) {
    if (w > 0 && h > 0) atomic_store(&g_size_req, (uint64_t)(uint32_t)w << 32 | (uint32_t)h);
}
void *shim_window_hwnd(void) { return ui_window(); }   // (the "window handle" pc_overlay / hle_vi pass back to this file)
int shim_window_active(void) { return atomic_load(&g_focus); }
int shim_monitor_count(void) { int n = 0; SDL_free(SDL_GetDisplays(&n)); return n; }
int shim_window_refresh_hz(void) {
    SDL_Window *w = ui_window();
    const SDL_DisplayMode *m = w ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)) : NULL;
    return m && m->refresh_rate > 1.0f ? (int)(m->refresh_rate + 0.5f) : 0;
}
// the hook device.c does not have: SDL_SetWindowFullscreen from the present thread goes through here (not called today)
void shim_request_fullscreen(bool fs) { atomic_store(&g_fs_req, fs ? 1 : 0); }

static void apply_requests(void) {
    for (;;) {   // pc_mouse.cpp's posted requests, in order (capture, menu cursor, pointer, confinement)
        pthread_mutex_lock(&g_msg_lock);
        const bool any = g_nmsgs > 0;
        unsigned m = 0; uintptr_t w = 0; intptr_t l = 0;
        if (any) {
            m = g_msgs[0].m, w = g_msgs[0].w, l = g_msgs[0].l;
            memmove(&g_msgs[0], &g_msgs[1], (size_t)(--g_nmsgs) * sizeof g_msgs[0]);
        }
        pthread_mutex_unlock(&g_msg_lock);
        if (!any) break;
        pc_mouse_sdl_msg(m, w, l);
    }
    int fs = atomic_exchange(&g_fs_req, -1);
    if (fs >= 0) apply_fullscreen(fs != 0);
    const int mode = atomic_exchange(&g_mode_req, -1);
    if (mode >= 0) apply_mode(mode, atomic_exchange(&g_mon_req, -1));
    const uint64_t req = atomic_exchange(&g_size_req, 0);
    const int sw = (int)(req >> 32), sh = (int)(uint32_t)req;
    if (sw > 0 && sh > 0 && ui_window() && !atomic_load(&g_mode)) {   // (windowed only; the size in pixels, as Windows')
        const float k = SDL_GetWindowPixelDensity(ui_window());
        SDL_SetWindowSize(ui_window(), (int)(sw / (k > 0 ? k : 1.0f) + 0.5f), (int)(sh / (k > 0 ? k : 1.0f) + 0.5f));
    }
    if (atomic_exchange(&g_title_dirty, 0)) {
        char t[256];
        pthread_mutex_lock(&g_title_lock);
        memcpy(t, g_title, sizeof t);
        pthread_mutex_unlock(&g_title_lock);
        SDL_SetWindowTitle(ui_window(), t);
    }
}

// ------------------------------------------------------------------ keys: SDL -> Windows virtual-key codes
// Letters follow the layout (SDL's key code, as Windows' VK_A..VK_Z do); everything else by position (scancode), which is
// what Windows' VK codes are for the digit row, the punctuation (VK_OEM_*), the numpad and the function keys.
static int vk_of(const SDL_KeyboardEvent *k) {
    if (k->key >= 'a' && k->key <= 'z') return (int)(k->key - 'a' + 'A');
    const SDL_Scancode s = k->scancode;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24) return 0x7C + (s - SDL_SCANCODE_F13);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSHASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0xE2;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    default: return 0;
    }
}
// ------------------------------------------------------------------ the keyboard layout in Win32's shapes (pc_input.cpp)
// pc_input binds letters, digits and punctuation by position: a set-1 scan code (a US keyboard's place) read as the VK
// the layout in use puts there. Here the place is an SDL scancode and the layout SDL's keymap.
static const SDL_Scancode kSet1[0x59] = {
    [0x01] = SDL_SCANCODE_ESCAPE, [0x02] = SDL_SCANCODE_1, [0x03] = SDL_SCANCODE_2, [0x04] = SDL_SCANCODE_3,
    [0x05] = SDL_SCANCODE_4, [0x06] = SDL_SCANCODE_5, [0x07] = SDL_SCANCODE_6, [0x08] = SDL_SCANCODE_7,
    [0x09] = SDL_SCANCODE_8, [0x0A] = SDL_SCANCODE_9, [0x0B] = SDL_SCANCODE_0, [0x0C] = SDL_SCANCODE_MINUS,
    [0x0D] = SDL_SCANCODE_EQUALS, [0x0E] = SDL_SCANCODE_BACKSPACE, [0x0F] = SDL_SCANCODE_TAB, [0x10] = SDL_SCANCODE_Q,
    [0x11] = SDL_SCANCODE_W, [0x12] = SDL_SCANCODE_E, [0x13] = SDL_SCANCODE_R, [0x14] = SDL_SCANCODE_T,
    [0x15] = SDL_SCANCODE_Y, [0x16] = SDL_SCANCODE_U, [0x17] = SDL_SCANCODE_I, [0x18] = SDL_SCANCODE_O,
    [0x19] = SDL_SCANCODE_P, [0x1A] = SDL_SCANCODE_LEFTBRACKET, [0x1B] = SDL_SCANCODE_RIGHTBRACKET,
    [0x1C] = SDL_SCANCODE_RETURN, [0x1D] = SDL_SCANCODE_LCTRL, [0x1E] = SDL_SCANCODE_A, [0x1F] = SDL_SCANCODE_S,
    [0x20] = SDL_SCANCODE_D, [0x21] = SDL_SCANCODE_F, [0x22] = SDL_SCANCODE_G, [0x23] = SDL_SCANCODE_H,
    [0x24] = SDL_SCANCODE_J, [0x25] = SDL_SCANCODE_K, [0x26] = SDL_SCANCODE_L, [0x27] = SDL_SCANCODE_SEMICOLON,
    [0x28] = SDL_SCANCODE_APOSTROPHE, [0x29] = SDL_SCANCODE_GRAVE, [0x2A] = SDL_SCANCODE_LSHIFT,
    [0x2B] = SDL_SCANCODE_BACKSLASH, [0x2C] = SDL_SCANCODE_Z, [0x2D] = SDL_SCANCODE_X, [0x2E] = SDL_SCANCODE_C,
    [0x2F] = SDL_SCANCODE_V, [0x30] = SDL_SCANCODE_B, [0x31] = SDL_SCANCODE_N, [0x32] = SDL_SCANCODE_M,
    [0x33] = SDL_SCANCODE_COMMA, [0x34] = SDL_SCANCODE_PERIOD, [0x35] = SDL_SCANCODE_SLASH, [0x36] = SDL_SCANCODE_RSHIFT,
    [0x37] = SDL_SCANCODE_KP_MULTIPLY, [0x38] = SDL_SCANCODE_LALT, [0x39] = SDL_SCANCODE_SPACE,
    [0x3A] = SDL_SCANCODE_CAPSLOCK, [0x3B] = SDL_SCANCODE_F1, [0x3C] = SDL_SCANCODE_F2, [0x3D] = SDL_SCANCODE_F3,
    [0x3E] = SDL_SCANCODE_F4, [0x3F] = SDL_SCANCODE_F5, [0x40] = SDL_SCANCODE_F6, [0x41] = SDL_SCANCODE_F7,
    [0x42] = SDL_SCANCODE_F8, [0x43] = SDL_SCANCODE_F9, [0x44] = SDL_SCANCODE_F10, [0x45] = SDL_SCANCODE_NUMLOCKCLEAR,
    [0x46] = SDL_SCANCODE_SCROLLLOCK, [0x47] = SDL_SCANCODE_KP_7, [0x48] = SDL_SCANCODE_KP_8, [0x49] = SDL_SCANCODE_KP_9,
    [0x4A] = SDL_SCANCODE_KP_MINUS, [0x4B] = SDL_SCANCODE_KP_4, [0x4C] = SDL_SCANCODE_KP_5, [0x4D] = SDL_SCANCODE_KP_6,
    [0x4E] = SDL_SCANCODE_KP_PLUS, [0x4F] = SDL_SCANCODE_KP_1, [0x50] = SDL_SCANCODE_KP_2, [0x51] = SDL_SCANCODE_KP_3,
    [0x52] = SDL_SCANCODE_KP_0, [0x53] = SDL_SCANCODE_KP_PERIOD, [0x56] = SDL_SCANCODE_NONUSBACKSLASH,
    [0x57] = SDL_SCANCODE_F11, [0x58] = SDL_SCANCODE_F12};
static atomic_int g_keymap_gen;   // SDL_EVENT_KEYMAP_CHANGED: pc_input rebuilds its table for the new layout
static SDL_Scancode sdl_of_set1(unsigned sc) { return sc < sizeof kSet1 / sizeof kSet1[0] ? kSet1[sc] : SDL_SCANCODE_UNKNOWN; }
static int vk_of(const SDL_KeyboardEvent *k);
static int vk_of_sdl(SDL_Scancode s) {   // the VK the layout in use gives that place (as a key event there would)
    SDL_KeyboardEvent k = {0};
    k.scancode = s;
    k.key = SDL_GetKeyFromScancode(s, SDL_KMOD_NONE, false);
    return vk_of(&k);
}
HKL GetKeyboardLayout(DWORD tid) { (void)tid; return (HKL)(uintptr_t)(atomic_load(&g_keymap_gen) + 1); }
UINT MapVirtualKeyExW(UINT code, UINT type, HKL layout) {
    (void)layout;
    if (type == MAPVK_VSC_TO_VK) return sdl_of_set1(code) ? (UINT)vk_of_sdl(sdl_of_set1(code)) : 0;
    if (type == MAPVK_VK_TO_VSC)
        for (unsigned sc = 1; sc < sizeof kSet1 / sizeof kSet1[0]; sc++)
            if (kSet1[sc] && vk_of_sdl(kSet1[sc]) == (int)code) return sc;
    return 0;
}
int ToUnicodeEx(UINT vk, UINT scan, const BYTE *state, WCHAR *buf, int n, UINT flags, HKL layout) {
    (void)vk, (void)state, (void)flags, (void)layout;
    const SDL_Scancode s = sdl_of_set1(scan);
    const SDL_Keycode k = s ? SDL_GetKeyFromScancode(s, SDL_KMOD_NONE, false) : SDLK_UNKNOWN;
    // (a printable key's SDL key code is its character; the extended ones carry SDLK_SCANCODE_MASK)
    if (n < 1 || !k || (k & SDLK_SCANCODE_MASK) || k < 0x20 || k >= 0x110000) return 0;
    buf[0] = (WCHAR)k;
    return 1;
}
int GetKeyNameTextW(LONG lparam, WCHAR *buf, int n) {
    const SDL_Scancode s = sdl_of_set1((unsigned)(lparam >> 16) & 0xFF);
    const char *name = s ? SDL_GetScancodeName(s) : "";
    int k = name && *name && n > 0 ? MultiByteToWideChar(CP_UTF8, 0, name, -1, buf, n) : 0;
    return k > 0 ? k - 1 : 0;
}
DWORD CharUpperBuffW(WCHAR *s, DWORD n) {
    for (DWORD i = 0; i < n; i++) s[i] = (WCHAR)towupper((wint_t)s[i]);
    return n;
}
HWND GetForegroundWindow(void) { return atomic_load(&g_focus) ? (HWND)ui_window() : NULL; }
int GetSystemMetrics(int index) {   // (SM_CXSCREEN / SM_CYSCREEN; video started here if no window yet: SDL counts the inits)
    if ((index != SM_CXSCREEN && index != SM_CYSCREEN) || (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)))
        return 0;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return m ? (int)((index == SM_CYSCREEN ? m->h : m->w) * m->pixel_density) : 0;
}

// every VK held now (the UI thread's view, for GetKeyState), the generic Shift / Ctrl / Alt of either side included
static unsigned char g_vk_down[256];
static void key_note(int vk, bool down) {
    if (vk <= 0 || vk >= 256) return;
    g_vk_down[vk] = down;
    hid_key_event(vk, down);
    static const int pairs[3][3] = {{0x10, 0xA0, 0xA1}, {0x11, 0xA2, 0xA3}, {0x12, 0xA4, 0xA5}};
    for (int i = 0; i < 3; i++)
        if (vk == pairs[i][1] || vk == pairs[i][2]) {
            const bool any = g_vk_down[pairs[i][1]] || g_vk_down[pairs[i][2]];
            g_vk_down[pairs[i][0]] = any;
            hid_key_event(pairs[i][0], any);
        }
}
static void keys_all_up(void) {
    memset(g_vk_down, 0, sizeof g_vk_down);
    hid_key_focus(false);
}
// user32's answers for the code that speaks Win32 (linux/windows.h)
SHORT GetKeyState(int vk) {
    if (vk == 0x01 || vk == 0x02 || vk == 0x04) {   // (the mouse buttons: SDL's state)
        const SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
        return (b & (vk == 0x01 ? SDL_BUTTON_LMASK : vk == 0x02 ? SDL_BUTTON_RMASK : SDL_BUTTON_MMASK)) ? (short)0x8000 : 0;
    }
    return vk > 0 && vk < 256 && g_vk_down[vk] ? (short)0x8000 : 0;
}
BOOL GetClientRect(HWND h, RECT *r) {
    int w = 0, hh = 0;
    (void)h;
    if (!ui_window() || !SDL_GetWindowSizeInPixels(ui_window(), &w, &hh)) return false;
    r->left = r->top = 0, r->right = w, r->bottom = hh;
    return true;
}
BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)h, (void)lp;
    if (msg == 0x0010) {   // WM_CLOSE: as the window's X
        SDL_Event q = {.type = SDL_EVENT_QUIT};
        return SDL_PushEvent(&q);
    }
    if (msg == 0x8000 + 2) { shim_request_fullscreen(wp != 0); return true; }   // (shim_win.c's WM_TOTK_FULLSCREEN)
    if (msg >= MOUSE_MSG_FIRST && msg <= MOUSE_MSG_LAST) {   // (pc_mouse.cpp's requests: apply_requests delivers them)
        pthread_mutex_lock(&g_msg_lock);
        const bool room = g_nmsgs < (int)(sizeof g_msgs / sizeof g_msgs[0]);
        if (room) { g_msgs[g_nmsgs].m = msg; g_msgs[g_nmsgs].w = wp; g_msgs[g_nmsgs].l = lp; g_nmsgs++; }
        pthread_mutex_unlock(&g_msg_lock);
        SDL_Event wake = {.type = SDL_EVENT_USER};   // (the UI thread may sit in SDL_WaitEventTimeout: as a posted message wakes it)
        if (room) SDL_PushEvent(&wake);
        return room;
    }
    return false;
}
static atomic_int g_cursor_want = 1;   // the overlay's SetCursor in WM_SETCURSOR (below): the pointer shows
HCURSOR SetCursor(HCURSOR c) { atomic_store(&g_cursor_want, c != NULL); return NULL; }
BOOL IsIconic(HWND h) { return h && atomic_load(&g_minimized); }   // (the window events' flag: any thread)
HMONITOR MonitorFromWindow(HWND h, DWORD flags) {
    (void)flags;
    return (void *)(uintptr_t)(h ? SDL_GetDisplayForWindow((SDL_Window *)h) : SDL_GetPrimaryDisplay());
}

// a message to the receivers that see it first on Windows: the rebinding capture (pc_mouse.cpp's window procedure
// asks it before anything), then the F10 window (shim_win.c's); true: one of them took it
static bool to_overlay(uint32_t msg, uintptr_t wp, intptr_t lp) {
    return pc_input_capture_msg(ui_window(), msg, wp, lp) || pc_wndproc(ui_window(), msg, wp, lp);
}
static intptr_t mouse_lp(float x, float y) {   // (client pixels, as Windows' messages carry them)
    const float k = ui_window() ? SDL_GetWindowPixelDensity(ui_window()) : 1.0f;
    const int px = (int)(x * k), py = (int)(y * k);
    return (intptr_t)(uint32_t)(((uint32_t)(uint16_t)py << 16) | (uint16_t)px);
}
static void cursor_update(void) {   // (WM_SETCURSOR's rule: fullscreen hides the pointer unless the overlay shows it)
    const int mouse = pc_mouse_sdl_cursor();   // (pc_mouse.cpp's window procedure answers first: captured / a menu's)
    if (mouse >= 0 && !pc_overlay_open()) {
        if (mouse) SDL_ShowCursor(); else SDL_HideCursor();
        return;
    }
    atomic_store(&g_cursor_want, 0);
    const bool shown = to_overlay(0x0020, 0, 1 /* HTCLIENT */) && atomic_load(&g_cursor_want);
    if (shown || !atomic_load(&g_mode)) SDL_ShowCursor(); else SDL_HideCursor();
}

// The UI loop's event wait (device.c ui_run): pumps SDL (windows, keyboard, gamepads - hle_hid.cpp reads their state through the SDL
// API) and hands what shim_win.c's window procedure handles to the same receivers in its words: the F10 window
// (pc_wndproc) first, then F11 / Alt+Enter (fullscreen), F3 (fps readout), F12 (screenshot); the key snapshot of the PC
// controls (hid_key_event); focus to the pause-on-focus-loss and the accessibility hotkeys. Returns only a quit event.
bool totk_wait_event(SDL_Event *out, Sint32 ms) {
    apply_requests();
    Uint64 end = ms < 0 ? 0 : SDL_GetTicks() + (Uint64)ms;
    for (;;) {
        SDL_Event e;
        Sint32 left = -1;
        if (ms >= 0) { Uint64 now = SDL_GetTicks(); left = now >= end ? 0 : (Sint32)(end - now); }
        if (!SDL_WaitEventTimeout(&e, left)) { apply_requests(); return false; }
        static bool text_on;   // (text input only while the F10 window is open: no on-screen keyboard or IME otherwise)
        if (ui_window() && text_on != pc_overlay_open()) {
            text_on = !text_on;
            if (text_on) SDL_StartTextInput(ui_window()); else SDL_StopTextInput(ui_window());
        }
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            pc_settings_save_pending();   // (shim_win.c's WM_CLOSE: a pending settings change is saved now)
            out->type = SDL_EVENT_QUIT;
            return true;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:   // accelerometer / gyro of the open gamepad -> hle_hid.cpp's motion fusion
            totk_hid_sensor((int)e.gsensor.sensor, e.gsensor.data, e.gsensor.sensor_timestamp);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = e.type == SDL_EVENT_KEY_DOWN, alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            const int vk = vk_of(&e.key);
            if (!e.key.repeat) key_note(vk, down);
            if (!vk) break;
            // (Windows' lParam: the scan code, bit 29 Alt held, bit 30 the previous state, bit 31 a release; F10 and
            // Alt+key come as WM_SYSKEY*, as Windows sends them)
            const intptr_t lp = (intptr_t)(uint32_t)(((uint32_t)(e.key.scancode & 0xFF) << 16) | (alt ? 1u << 29 : 0u) |
                                                     (e.key.repeat || !down ? 1u << 30 : 0u) | (down ? 0u : 1u << 31) | 1u);
            const bool sys = alt || vk == 0x79;
            if (to_overlay(down ? (sys ? 0x0104 : 0x0100) : (sys ? 0x0105 : 0x0101), (uintptr_t)vk, lp) || !down || e.key.repeat) break;
            if (vk == 0x7A || (vk == 0x0D && alt)) apply_fullscreen(!shim_window_fullscreen());   // F11, Alt+Enter
            else if (vk == 0x72) shim_set_fps_overlay(shim_fps_overlay_mode() ? 0 : atomic_load(&g_fps_osd_last));   // F3
            else if (vk == 0x7B) pc_shot_request();   // F12
            break;
        }
        case SDL_EVENT_TEXT_INPUT:   // WM_CHAR per UTF-16 unit
            for (const unsigned char *t = (const unsigned char *)e.text.text; t && *t;) {
                uint32_t c = *t++;
                int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
                while (more-- > 0 && (*t & 0xC0) == 0x80) c = c << 6 | (*t++ & 0x3F);
                if (c >= 0x10000) {
                    to_overlay(0x0102, 0xD800 + ((c - 0x10000) >> 10), 1);
                    to_overlay(0x0102, 0xDC00 + ((c - 0x10000) & 0x3FF), 1);
                } else to_overlay(0x0102, c, 1);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:   // (the mouse camera first, as its window procedure sees WM_INPUT before anyone)
            pthread_mutex_lock(&g_ptr_lock);
            g_ptr_x = e.motion.x, g_ptr_y = e.motion.y, g_ptr_over = 1;   // (motion comes over the window, or captured)
            pthread_mutex_unlock(&g_ptr_lock);
            pc_mouse_sdl_motion(e.motion.xrel, e.motion.yrel);
            to_overlay(0x0200, 0, mouse_lp(e.motion.x, e.motion.y));
            cursor_update();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const intptr_t lp = mouse_lp(e.button.x, e.button.y);
            const int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 :
                          e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b >= 0) {
                pthread_mutex_lock(&g_ptr_lock);
                g_ptr_buttons = down ? g_ptr_buttons | 1u << b : g_ptr_buttons & ~(1u << b);
                pthread_mutex_unlock(&g_ptr_lock);
                pc_mouse_sdl_button(b, down);
            }
            switch (e.button.button) {
            case SDL_BUTTON_LEFT: to_overlay(down ? 0x0201 : 0x0202, 0, lp); break;
            case SDL_BUTTON_RIGHT: to_overlay(down ? 0x0204 : 0x0205, 0, lp); break;
            case SDL_BUTTON_MIDDLE: to_overlay(down ? 0x0207 : 0x0208, 0, lp); break;
            case SDL_BUTTON_X1: case SDL_BUTTON_X2:
                to_overlay(down ? 0x020B : 0x020C, (uintptr_t)(e.button.button == SDL_BUTTON_X1 ? 1u : 2u) << 16, lp);
                key_note(e.button.button == SDL_BUTTON_X1 ? 0x05 : 0x06, down);   // (VK_XBUTTON1 / 2: bindable keys)
                break;
            default: break;
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const float dir = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            const intptr_t lp = mouse_lp(e.wheel.mouse_x, e.wheel.mouse_y);
            if (e.wheel.y) pc_mouse_sdl_wheel(e.wheel.y * dir);
            if (e.wheel.y) to_overlay(0x020A, (uintptr_t)(uint16_t)(int16_t)(e.wheel.y * dir * 120) << 16, lp);
            if (e.wheel.x) to_overlay(0x020E, (uintptr_t)(uint16_t)(int16_t)(e.wheel.x * dir * 120) << 16, lp);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            const bool on = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            atomic_store(&g_focus, on);
            if (!on) {   // (the buttons go with the focus, as the keys do)
                keys_all_up();
                pthread_mutex_lock(&g_ptr_lock);
                g_ptr_buttons = 0;
                pthread_mutex_unlock(&g_ptr_lock);
            }
            hid_key_focus(on);
            pc_mouse_sdl_focus(on);
            to_overlay(on ? 0x0007 : 0x0008, 0, 0);
            oe_focus_update(on);
            pc_input_focus(on);
            break;
        }
        case SDL_EVENT_KEYMAP_CHANGED:   // (a layout switch: the letters' VKs and the key names follow)
            atomic_fetch_add(&g_keymap_gen, 1);
            break;
        case SDL_EVENT_DISPLAY_ADDED:
        case SDL_EVENT_DISPLAY_REMOVED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            displays_read();   // (the copies first: F10's Display line and uneven-rate hint read them)
            to_overlay(0x007E, 0, 0);   // WM_DISPLAYCHANGE
            break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            pthread_mutex_lock(&g_ptr_lock);
            g_ptr_over = e.type == SDL_EVENT_WINDOW_MOUSE_ENTER;
            pthread_mutex_unlock(&g_ptr_lock);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            window_size_note();
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
            atomic_store(&g_minimized, e.type == SDL_EVENT_WINDOW_MINIMIZED);
            break;
        default: break;
        }
        apply_requests();
    }
}

// ------------------------------------------------------------------ PNG (only SDL >= 3.4 has SDL_SavePNG)
// Surfaces only carry RGBA8 pixels (SDL_PIXELFORMAT_ABGR8888 = R,G,B,A bytes) for the PNG writer below.
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void png_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t b[4]; png_be32(b, (uint32_t)n); fwrite(b, 1, 4, f);
    uint32_t c = png_crc(0, (const uint8_t *)type, 4); c = png_crc(c, data, n);
    fwrite(type, 1, 4, f); fwrite(data, 1, n, f); png_be32(b, c); fwrite(b, 1, 4, f);
}
bool totk_save_png(SDL_Surface *s, const char *path) {
    if (!s) return false;
    size_t row = (size_t)s->w * 4 + 1, raw_n = row * s->h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < s->h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, (const uint8_t *)s->pixels + (size_t)y * s->pitch, row - 1); }
    size_t blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = off + n == raw_n; *q++ = n & 0xFF; *q++ = n >> 8; *q++ = ~n & 0xFF; *q++ = (~n >> 8) & 0xFF;
        memcpy(q, raw + off, n); q += n;
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
    }
    png_be32(q, (b << 16) | a); q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return false; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13]; png_be32(ihdr, s->w); png_be32(ihdr + 4, s->h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite(sig, 1, 8, f); png_chunk(f, "IHDR", ihdr, 13); png_chunk(f, "IDAT", z, (size_t)(q - z)); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return true;
}

// ------------------------------------------------------------------ LASRecomp app layer (same stand-ins as shim_win.c)
// Fullscreen is the default on Linux (the Deck, a TV); TOTK_FULLSCREEN=0 or LARECOMP_WINDOW=WxH start windowed.
Settings g_settings = {.aspect = ASPECT_ORIGINAL, .fullscreen = true, .res_mode = RES_MATCH, .upscaler = UPSCALE_BILINEAR,
                       .antialias = AA_OFF};
void settings_load(void) {
    const char *f = getenv("TOTK_FULLSCREEN");
    g_settings.fullscreen = f ? (*f && strcmp(f, "0") != 0) : true;
    // TOTK_AA=smaa / TOTK_UPSCALE=fsr: as on Windows (shim_win.c settings_load)
    const char *aa = getenv("TOTK_AA"), *up = getenv("TOTK_UPSCALE");
    if (aa && !strcmp(aa, "smaa")) g_settings.antialias = AA_SMAA;
    if (up && !strcmp(up, "fsr")) g_settings.upscaler = UPSCALE_FSR1;
}
void plat_terminate(void) { _exit(0); }
int rt_reboot_requested(void) { return 0; }
void rt_window_icon(SDL_Window *w) { (void)w; }
const char *game_level_name(void) { return "-"; }
void menu_render(void *cb, uint32_t w, uint32_t h) { (void)cb, (void)w, (void)h; }
bool menu_handle_event(const SDL_Event *e) { (void)e; return false; }
void menu_apply_window_requests(SDL_Window *w) { (void)w; }
void menu_test_open(void) {}
void input_handle_event(const SDL_Event *e) { (void)e; }
void input_frame(uint64_t frame) { (void)frame; }
bool input_user_blocked(const SDL_Event *e) { (void)e; return false; }
void ui_size(int *w, int *h) { int a = 0, b = 0; SDL_GetWindowSizeInPixels(ui_window(), &a, &b); *w = a; *h = b; }
int texpatch_identify(const NvnTexture *t) { (void)t; return -1; }
int texpatch_bottle_kind(int id) { (void)id; return 0; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
uint32_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, uint32_t lv, uint8_t *b, uint32_t w, uint32_t h, uint32_t row) { (void)id, (void)lv, (void)b, (void)w, (void)h, (void)row; }
int texpatch_heart_kind(int id) { (void)id; return 0; }  // (was -1: truthy, so any texture with tp > 0 tinted its draw)
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind; for (int i = 0; i < 8; i++) tint[i] = 1.0f; }
#endif
