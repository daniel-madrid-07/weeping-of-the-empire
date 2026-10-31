// TotK: Win32 implementation of the SDL3 subset in shim/SDL3/SDL.h, plus the LASRecomp app-layer stand-ins
// (settings, menu, input, texpatch, kernel heap) that runtime/nvn/vulkan/*.c call.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>   // (shim_host_line's thread count)
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../shim/las.h"
#include "../../pc.h"
#include "../../pc_overlay.h"
#include "../../pc_settings.h"        // display.last_monitor / display.window_pos: where the game was played last
#include "../../pc_display_modes.h"   // exclusive: the monitor's own mode closest to the choice

struct SDL_Window { HWND hwnd; };
static SDL_Window g_win;
static volatile LONG g_quit;
static char g_err[256] = "";

// SDL_SetWindowTitle from the present thread: the text goes through g_title and a posted message, so the window
// thread sets it while pumping (SetWindowTextW from another thread would block on it)
#define WM_TOTK_TITLE (WM_APP + 1)
static SRWLOCK g_title_lock = SRWLOCK_INIT;
static wchar_t g_title[256];

// Window modes, all changed on the window thread:
// - windowed: resizable; the swapchain follows the client size (vk_frame_begin).
// - borderless (user 2026-10-15: "add a fulscreen option"): a popup covering the monitor, so the driver flips the
//   swapchain directly (no desktop composition step = less latency). Alt-tab works as with any window.
// - exclusive: the same popup, the monitor switched to TOTK_EXCLUSIVE_SIZE / TOTK_EXCLUSIVE_HZ when set
//   (ChangeDisplaySettingsEx, CDS_FULLSCREEN: Windows restores it when the process ends), and the swapchain made with
//   VK_EXT_full_screen_exclusive, application-controlled (present.c). Losing focus (alt-tab) gives the desktop mode
//   back and minimises; coming back re-enters.
// F11 / Alt+Enter toggle between windowed and the fullscreen kind (TOTK_WINDOW_MODE, default borderless).
// TOTK_FULLSCREEN=1 (old) = start borderless. TOTK_MONITOR=<n> (1 = first in EnumDisplayMonitors order): the monitor the
// fullscreen modes use, and where a windowed start is centred; 0 / unset: the window's own, and the game remembers it:
// the monitor the window was on last (display.last_monitor, its GDI name) and the windowed position (display.window_pos,
// the window's top-left) are saved when a move ends, the mode changes and the game closes, and the next start opens
// there (if that monitor is still connected and the position on one). The cursor hides over the game while fullscreen.
#define WM_TOTK_FULLSCREEN (WM_APP + 2)   // wParam: fullscreen (of the configured kind) or windowed
#define WM_TOTK_MODE (WM_APP + 3)         // wParam: SHIM_WINDOWED / BORDERLESS / EXCLUSIVE, lParam: monitor + 1 (0 keep)
#define WM_TOTK_RECOVER (WM_APP + 4)      // fullscreen, resized off its monitor by someone else: cover it again
#define WM_TOTK_STARTED (WM_APP + 5)      // behind the start's mode request (shim_window_start_done): it is applied
enum { SHIM_WINDOWED, SHIM_BORDERLESS, SHIM_EXCLUSIVE };
static volatile LONG g_fs, g_mode, g_fs_kind = SHIM_BORDERLESS, g_monitor, g_excl_w, g_excl_h, g_excl_hz, g_excl_away;
static WINDOWPLACEMENT g_wp = {sizeof g_wp};
static wchar_t g_dm_dev[32];   // monitor whose display mode we changed ("" none)
void vk_present_set_exclusive(void *hmonitor);   // present.c: the swapchain's full-screen-exclusive monitor (NULL: none)

typedef struct { int want, n; HMONITOR found; } MonPick;
static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM p) {
    (void)dc, (void)r;
    MonPick *k = (MonPick *)p;
    if (++k->n == k->want) { k->found = m; return FALSE; }
    return TRUE;
}
static HMONITOR pick_monitor(HWND h) {
    MonPick k = {(int)g_monitor, 0, NULL};
    if (k.want > 0) EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&k);
    return k.found ? k.found : MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}
int shim_monitor_count(void) { MonPick k = {0, 0, NULL}; EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&k); return k.n; }
typedef struct { const wchar_t *dev; HMONITOR found; } MonByName;
static BOOL CALLBACK mon_by_name_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM p) {
    (void)dc, (void)r;
    MonByName *k = (MonByName *)p;
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (GetMonitorInfoW(m, (MONITORINFO *)&mi) && !wcscmp(mi.szDevice, k->dev)) { k->found = m; return FALSE; }
    return TRUE;
}
static HMONITOR monitor_by_name(const wchar_t *dev) {   // ("\\.\DISPLAY2"; NULL: not connected now)
    MonByName k = {dev, NULL};
    if (dev[0]) EnumDisplayMonitors(NULL, NULL, mon_by_name_enum, (LPARAM)&k);
    return k.found;
}
// The monitor and the windowed position to open on next time (only while the monitor follows the window: TOTK_MONITOR
// unset). Written only when changed (pc_settings_raw_set saves the file 0.5 s later).
static void remember_place(HWND h) {
    char v[64], was[64];
    if (g_monitor <= 0) {
        MONITORINFOEXW mi = {.cbSize = sizeof mi};
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
            WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, v, sizeof v, NULL, NULL) > 0 &&
            (!pc_settings_raw("display.last_monitor", was, sizeof was) || strcmp(was, v))) {
            pc_settings_raw_set("display.last_monitor", v);
            fprintf(stderr, "[win] monitor remembered: %s\n", v);
        }
    }
    RECT r;
    if (g_mode != SHIM_WINDOWED || IsIconic(h) || IsZoomed(h) || !GetWindowRect(h, &r)) return;
    snprintf(v, sizeof v, "%ld,%ld", r.left, r.top);
    if (!pc_settings_raw("display.window_pos", was, sizeof was) || strcmp(was, v)) pc_settings_raw_set("display.window_pos", v);
}
void pc_mouse_mode_change(bool after);   // pc_mouse.cpp: a window mode change keeps the pointer's capture

static void display_mode_restore(void) {
    if (!g_dm_dev[0]) return;
    ChangeDisplaySettingsExW(g_dm_dev, NULL, NULL, 0, NULL);
    fprintf(stderr, "[win] display mode of %ls restored\n", g_dm_dev);
    g_dm_dev[0] = 0;
}
// exclusive: the monitor to TOTK_EXCLUSIVE_SIZE / _HZ (fields left 0 keep the desktop's), as the closest mode the
// monitor has (pc_display_modes.h pcd_pick: a rate it lacks at that size gives the fastest under it; a size it lacks
// keeps the desktop mode, as the settings' lists come from the start monitor and the other may differ)
static void display_mode_set(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if ((!g_excl_w && !g_excl_hz) || !GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return;
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return;
    static pcd_mode modes[512];
    int n = 0;
    DEVMODEW e = {.dmSize = sizeof e};
    for (DWORD i = 0; n < 512 && EnumDisplaySettingsW(mi.szDevice, i, &e); i++)
        modes[n++] = (pcd_mode){(int)e.dmPelsWidth, (int)e.dmPelsHeight, (int)e.dmDisplayFrequency};
    const pcd_mode desk = {(int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency};
    pcd_mode to;
    if (!pcd_pick(modes, n, desk, (int)g_excl_w, (int)g_excl_h, (int)g_excl_hz, &to)) {
        fprintf(stderr, "[win] exclusive display mode %ldx%ld: %ls has no such mode (the desktop mode stays)\n", g_excl_w,
                g_excl_h, mi.szDevice);
        return;
    }
    dm.dmPelsWidth = (DWORD)to.w; dm.dmPelsHeight = (DWORD)to.h; dm.dmDisplayFrequency = (DWORD)to.hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
    fprintf(stderr, "[win] exclusive display mode %lux%lu @ %lu Hz on %ls: %s\n", dm.dmPelsWidth, dm.dmPelsHeight,
            dm.dmDisplayFrequency, mi.szDevice, r == DISP_CHANGE_SUCCESSFUL ? "set" : "refused (the desktop mode stays)");
    if (r == DISP_CHANGE_SUCCESSFUL) wcscpy_s(g_dm_dev, 32, mi.szDevice);
}

static void cover_monitor(HWND h, HMONITOR mon) {
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(mon, &mi)) return;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    // (SWP_NOACTIVATE: covering the monitor never takes the foreground; and only the window in front is raised: a display
    // change or a recovery while the player is in another window, keeps its place in the z-order instead of a
    // monitor-sized popup over his windows. The in-game paths, a mode switch from the options and exclusive's way back on
    // activation, are in front: HWND_TOP)
    const bool front = GetForegroundWindow() == h;
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE | (front ? 0 : SWP_NOZORDER));
}

static bool g_applying;   // (window thread) apply_mode is moving / restyling the window: no recovery meanwhile
static bool g_recover_posted;   // (window thread) a WM_TOTK_RECOVER is queued
static HMONITOR g_excl_mon;     // the monitor exclusive's display mode and present were set on
// The monitor a fullscreen window must cover: in exclusive the one its display mode and present are on; borderless
// follows the window (Win+Shift+arrow onto another monitor)
static HMONITOR recover_monitor(HWND h) {
    return g_mode == SHIM_EXCLUSIVE && g_excl_mon ? g_excl_mon : MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}
static void apply_mode(HWND h, int mode, int monitor) {
    if (monitor > 0) InterlockedExchange(&g_monitor, monitor - 1);
    int cur = (int)g_mode;
    pc_mouse_mode_change(false);
    if (cur == SHIM_WINDOWED && mode != SHIM_WINDOWED) { remember_place(h); GetWindowPlacement(h, &g_wp); }
    HMONITOR mon = pick_monitor(h);
    g_applying = true;
    // The new mode before the window changes: the WM_DPICHANGED that cover_monitor's move to a monitor of another DPI
    // sends must see fullscreen (and leave the popup's size alone), the windowed restore must see windowed
    InterlockedExchange(&g_mode, mode);
    InterlockedExchange(&g_fs, mode != SHIM_WINDOWED);
    if (cur == SHIM_EXCLUSIVE) { vk_present_set_exclusive(NULL); display_mode_restore(); }
    if (mode == SHIM_WINDOWED) {
        if (cur != SHIM_WINDOWED) {
            LONG style = GetWindowLongW(h, GWL_STYLE);
            SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(h, &g_wp);
            SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else if (monitor > 0 && MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != mon) {   // centred on the chosen one
            MONITORINFO mi = {sizeof mi};
            RECT r;
            if (GetMonitorInfoW(mon, &mi) && GetWindowRect(h, &r))
                SetWindowPos(h, NULL, (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2,
                             (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        }
    } else {
        if (mode == SHIM_EXCLUSIVE) display_mode_set(mon);
        cover_monitor(h, mon);   // (after the mode change: rcMonitor has the new size)
        if (mode == SHIM_EXCLUSIVE) { vk_present_set_exclusive(mon); g_excl_mon = mon; }
    }
    InterlockedExchange(&g_excl_away, 0);
    g_applying = false;
    if (mode != SHIM_WINDOWED) InterlockedExchange(&g_fs_kind, mode);
    fprintf(stderr, "[win] %s\n", mode == SHIM_EXCLUSIVE ? "exclusive fullscreen" : mode == SHIM_BORDERLESS ? "fullscreen" : "windowed");
    remember_place(h);
    pc_mouse_mode_change(true);
}
static void apply_fullscreen(HWND h, bool fs) {
    int mode = fs ? (int)g_fs_kind : SHIM_WINDOWED;
    if (mode != (int)g_mode) apply_mode(h, mode, 0);
}
// exclusive and alt-tab: away = desktop mode back + minimised; back = the mode again
void vk_present_exclusive_retry(void);   // present.c: exclusive again after a refused acquire
static void exclusive_focus(HWND h, bool active) {
    // (g_applying: an activation inside apply_mode sees the new mode before its display mode / exclusive present are
    // set, either way)
    if (g_mode != SHIM_EXCLUSIVE || g_applying) return;
    if (active && !g_excl_away) vk_present_exclusive_retry();   // (started or left behind another window: borderless till now)
    if (!active && !g_excl_away) {
        InterlockedExchange(&g_excl_away, 1);
        vk_present_set_exclusive(NULL);
        display_mode_restore();
        ShowWindow(h, SW_MINIMIZE);
        fprintf(stderr, "[win] exclusive fullscreen: focus lost, minimised\n");
    } else if (active && g_excl_away) {
        g_applying = true;   // (our own move: the restore's rect is not "off its monitor")
        InterlockedExchange(&g_excl_away, 0);
        ShowWindow(h, SW_RESTORE);
        HMONITOR mon = pick_monitor(h);
        display_mode_set(mon);
        cover_monitor(h, mon);
        vk_present_set_exclusive(mon);
        g_excl_mon = mon;
        g_applying = false;
        fprintf(stderr, "[win] exclusive fullscreen: back\n");
    }
}
bool shim_window_fullscreen(void) { return g_fs != 0; }
int shim_window_mode(void) { return (int)g_mode; }
// The size the window's picture is meant to have: its monitor's in borderless / exclusive, else the client area.
// (textures.c, TOTK_OUT_SCALE's choice at start: the first swapchain can still have the windowed size, 1280 x 720 with
// a blank window size, for a few frames after the switch to borderless, p1810o_kak_120 / mtr_auto1440)
static volatile LONG g_started;   // the start's window mode is applied (WM_TOTK_STARTED, after its WM_TOTK_FULLSCREEN)
// device.c ui_window_init, after its SDL_SetWindowFullscreen (or none: a windowed start): a marker behind that request
void shim_window_start_done(void) {
    if (!g_win.hwnd || !PostMessageW(g_win.hwnd, WM_TOTK_STARTED, 0, 0)) InterlockedExchange(&g_started, 1);
}
// false until the start's mode is applied (waits up to wait_ms for it; never on the window thread, which applies it):
// before that a borderless start still reads as the blank 1280 x 720 window, and an exclusive one's g_mode flips before
// its display mode is set
bool shim_window_target_size(int *w, int *h, int wait_ms) {
    HWND hw = g_win.hwnd;
    RECT r;
    if (!hw) return false;
    if (!g_started && GetWindowThreadProcessId(hw, NULL) != GetCurrentThreadId())
        for (int t = 0; !g_started && t < wait_ms; t += 5) Sleep(5);
    if (!g_started) return false;
    if (g_mode != SHIM_WINDOWED) {
        MONITORINFO mi = {sizeof mi};
        if (!GetMonitorInfoW(pick_monitor(hw), &mi)) return false;
        r = mi.rcMonitor;
    } else if (!GetClientRect(hw, &r)) return false;
    *w = r.right - r.left, *h = r.bottom - r.top;
    return *w > 0 && *h > 0;
}
int shim_window_active(void) { return GetForegroundWindow() == g_win.hwnd; }
// From any thread (the overlay): the window thread applies it. monitor: -1 keep, 0 the window's own, 1..N that one.
void shim_set_window_mode(int mode, int monitor) {
    if (g_win.hwnd) PostMessageW(g_win.hwnd, WM_TOTK_MODE, (WPARAM)mode, (LPARAM)(monitor + 1));
}
// Exclusive display mode (0 = the desktop's); applied at once when exclusive is on.
void shim_set_exclusive_mode(int w, int h, int hz) {
    InterlockedExchange(&g_excl_w, w); InterlockedExchange(&g_excl_h, h); InterlockedExchange(&g_excl_hz, hz);
    if (g_mode == SHIM_EXCLUSIVE) shim_set_window_mode(SHIM_EXCLUSIVE, 0);
}
void *shim_window_hwnd(void) { return g_win.hwnd; }   // (hle_vi.cpp: the monitor whose vblank TOTK_VSYNC=display follows)

// device.c's stats window: the "[host]" line. Free RAM, the system's commit against its limit, ours (commit, working
// set) and page faults per second (soft and hard). Test boots with <= 7 GB available showed game-side stalls of 100-900
// ms with the game at ~1 core and no render work behind them; a player's log had the same stalls and nothing to tell
// RAM pressure apart. (Not GetPerformanceInfo: it walks every process, 5.6-26 ms with ~1,070 of them on the present
// thread, review V21. The commit charge and limit come from GlobalMemoryStatusEx's page-file fields.) Then our handle,
// thread, GDI and USER object counts, so a long session shows those leaks (integration-2's soak, 2026-10-27). Handles
// and GDI / USER are per-process calls (microseconds). Windows has no per-process thread count: a thread snapshot walks
// every thread on the host (6,940 here, milliseconds), so it runs every 10 s on a thread of its own at the lowest
// priority and the line prints the last count ("threads -" until the first one).
static volatile LONG g_thread_count = -1;
static DWORD WINAPI thread_count_loop(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const DWORD me = GetCurrentProcessId();
    for (;;) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (s != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te = {.dwSize = sizeof te};
            LONG cnt = 0;
            for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) cnt += te.th32OwnerProcessID == me;
            CloseHandle(s);
            InterlockedExchange(&g_thread_count, cnt);
        }
        Sleep(10000);
    }
}
void shim_host_line(char *out, size_t n) {
    static ULONGLONG last_ms;
    static DWORD last_faults;
    static volatile LONG counter_started;
    if (!InterlockedExchange(&counter_started, 1)) {
        HANDLE t = CreateThread(NULL, 64 << 10, thread_count_loop, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (t) CloseHandle(t);
    }
    MEMORYSTATUSEX ms = {sizeof ms};
    PROCESS_MEMORY_COUNTERS_EX pc = {sizeof pc};
    GlobalMemoryStatusEx(&ms);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    const LONG threads = InterlockedCompareExchange(&g_thread_count, 0, 0);
    char th[16] = "-";
    if (threads >= 0) snprintf(th, sizeof th, "%ld", (long)threads);
    ULONGLONG now = GetTickCount64();
    double rate = last_ms && now > last_ms ? (pc.PageFaultCount - last_faults) * 1000.0 / (double)(now - last_ms) : 0;
    last_ms = now, last_faults = pc.PageFaultCount;
    const double gb = 1024.0 * 1024 * 1024;
    snprintf(out, n, "RAM free %.1f of %.1f GB; system commit %.1f of %.1f GB; ours: commit %.1f GB, working set %.1f GB, "
             "%.0f page faults/s; handles %lu, threads %s, GDI %lu, USER %lu", ms.ullAvailPhys / gb, ms.ullTotalPhys / gb,
             (ms.ullTotalPageFile - ms.ullAvailPageFile) / gb, ms.ullTotalPageFile / gb, pc.PrivateUsage / gb,
             pc.WorkingSetSize / gb, rate, (unsigned long)handles, th, (unsigned long)gdi, (unsigned long)user);
}

// textures.c VRAM residency: the system's commit headroom (bytes before the commit limit) and the limit
void shim_commit_room(uint64_t *avail, uint64_t *limit) {
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    *avail = ms.ullAvailPageFile;
    *limit = ms.ullTotalPageFile;
}

// device.c's pipeline-cache file: src over dst in one step (0 = done), and the tmp files of killed boots
int shim_file_replace(const char *src, const char *dst) { return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1; }
void shim_remove_old_files(const char *pattern, int min_age_s) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const char *slash = strrchr(pattern, '/'), *bs = strrchr(pattern, '\\');
    if (bs > slash) slash = bs;
    size_t dl = slash ? (size_t)(slash - pattern + 1) : 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER n = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
    do {
        ULARGE_INTEGER w = {.LowPart = fd.ftLastWriteTime.dwLowDateTime, .HighPart = fd.ftLastWriteTime.dwHighDateTime};
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || w.QuadPart > n.QuadPart ||
            n.QuadPart - w.QuadPart < (ULONGLONG)min_age_s * 10000000) continue;
        char path[MAX_PATH];
        if (dl + strlen(fd.cFileName) < sizeof path) {
            memcpy(path, pattern, dl);
            strcpy(path + dl, fd.cFileName);
            DeleteFileA(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// on-screen fps readout (device.c): shown while fullscreen (no title bar there), F3 toggles it in either mode
static volatile LONG g_fps_osd = -2;   // -1 = automatic (fullscreen only), 0 off, 1 on (TOTK_FPS_OVERLAY=1 / 0 at start)
bool shim_fps_overlay(void) {
    if (g_fps_osd == -2) {  // (from -2 only: a set racing the first read wins)
        const char *e = getenv("TOTK_FPS_OVERLAY");
        InterlockedCompareExchange(&g_fps_osd, e ? (*e != '0') : -1, -2);
    }
    return g_fps_osd < 0 ? g_fs != 0 : g_fps_osd != 0;
}
// display.fps_readout, live (pc_settings.cpp fps_readout_apply / fps_readout_now): "" automatic, "1" on, "0" off
void shim_set_fps_overlay(int mode) { InterlockedExchange(&g_fps_osd, mode < 0 ? -1 : mode != 0); }
int shim_fps_overlay_mode(void) { shim_fps_overlay(); return (int)g_fps_osd; }

// Windows blanks the display and may sleep after its idle timeout, and pad input does not count as activity (a whole
// session on a controller went dark: totk-pc-recomp #7). While the window is up and not minimised it holds the display
// and the system on, as games do. The request belongs to the calling thread, so only the window's thread calls this
// (SDL_CreateWindow, wndproc); its exit or a minimise releases it. TOTK_KEEP_AWAKE=0: never held.
static void keep_awake(bool on) {
    static int cur = -1, off = -1;
    if (off < 0) { const char *e = getenv("TOTK_KEEP_AWAKE"); off = e && *e == '0'; }
    if (off || (int)on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

void oe_focus_update(bool focused);   // hle_oe.cpp: TOTK_PAUSE_UNFOCUSED (review N20: from activation, not a poll)
void hid_devices_changed(void);       // hle_hid.cpp: the pad readers look for pads at once (hot-plug)
void pc_input_focus(bool focused);    // pc_input.cpp: Sticky / Toggle / Filter Keys hotkeys off while the game is in front
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (pc_wndproc(h, m, (uintptr_t)w, (intptr_t)l)) return m == WM_SETCURSOR ? TRUE : 0;   // nxrecomp overlay (pc_overlay.h)
    switch (m) {
    case WM_DEVICECHANGE: if (w == 0x0007 /*DBT_DEVNODES_CHANGED*/) hid_devices_changed(); break;   // (sent to every top-level window)
    case WM_TOTK_FULLSCREEN: apply_fullscreen(h, w != 0); return 0;
    case WM_TOTK_MODE: apply_mode(h, (int)w <= SHIM_EXCLUSIVE ? (int)w : SHIM_WINDOWED, (int)l); return 0;
    case WM_TOTK_STARTED: InterlockedExchange(&g_started, 1); return 0;
    case WM_ACTIVATEAPP:   // (+ the game's focus state, hle_oe.cpp; the accessibility hotkeys off while in front, pc_input.cpp)
        exclusive_focus(h, w != 0); oe_focus_update(w != 0); pc_input_focus(w != 0); break;
    case WM_DISPLAYCHANGE:   // a desktop resolution / layout change: borderless covers its monitor again
        if (g_mode == SHIM_BORDERLESS) cover_monitor(h, pick_monitor(h));
        break;
    case WM_WINDOWPOSCHANGING: {   // fullscreen: a move or resize by anything but apply_mode lands on the monitor rect
        // before it applies, so the swapchain never follows a wrong size (osc4kw_a, after 9f079c0c: the popup went back
        // to 1316x823, the windowed outer size at the TV's DPI, after "[win] fullscreen"; WM_WINDOWPOSCHANGED's recovery
        // below stays the backstop). Borderless takes the monitor the proposed rect starts on (Win+Shift+arrow, a monitor
        // unplugged), exclusive its own, as recover_monitor. The first few are logged with their flags (the source of
        // that resize is not known yet).
        WINDOWPOS *p = (WINDOWPOS *)l;
        RECT cur, r;
        if (!g_fs || g_applying || g_excl_away || IsIconic(h) || ((p->flags & SWP_NOMOVE) && (p->flags & SWP_NOSIZE)) ||
            !GetWindowRect(h, &cur))
            break;
        r.left = p->flags & SWP_NOMOVE ? cur.left : p->x;
        r.top = p->flags & SWP_NOMOVE ? cur.top : p->y;
        r.right = r.left + (p->flags & SWP_NOSIZE ? cur.right - cur.left : p->cx);
        r.bottom = r.top + (p->flags & SWP_NOSIZE ? cur.bottom - cur.top : p->cy);
        if (r.left <= -32000 || r.top <= -32000) break;   // (minimising)
        // (borderless: the monitor under the proposed top-left, not the largest overlap: a 4K popup moved with its size
        // kept onto a smaller monitor overlaps the 4K one most and would be thrown back)
        MONITORINFO mi = {sizeof mi};
        const POINT tl = {r.left, r.top};
        HMONITOR mon = g_mode == SHIM_EXCLUSIVE && g_excl_mon ? g_excl_mon : MonitorFromPoint(tl, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(mon, &mi) || EqualRect(&r, &mi.rcMonitor)) break;
        static int kept;
        if (kept++ < 8)
            fprintf(stderr, "[win] fullscreen: a move to %ldx%ld at %ld,%ld (flags 0x%x) kept on the monitor\n",
                    r.right - r.left, r.bottom - r.top, r.left, r.top, p->flags);
        p->x = mi.rcMonitor.left;
        p->y = mi.rcMonitor.top;
        p->cx = mi.rcMonitor.right - mi.rcMonitor.left;
        p->cy = mi.rcMonitor.bottom - mi.rcMonitor.top;
        p->flags &= ~(UINT)(SWP_NOMOVE | SWP_NOSIZE);
        break;
    }
    case WM_WINDOWPOSCHANGED:   // fullscreen resized or moved off its monitor by anything but apply_mode (Windows restoring
        // the pre-popup rect, a late DPI suggestion, a queued SWP_ASYNCWINDOWPOS, 1316x823 and 3876x2263, the swapchain
        // following): cover the monitor again, posted (not from inside this change), at most 20 times a boot (a size
        // Windows keeps refusing would loop)
        if (g_fs && !g_applying && !g_excl_away && !IsIconic(h) && !g_recover_posted) {
            MONITORINFO mi = {sizeof mi};
            RECT r;
            if (GetWindowRect(h, &r) && GetMonitorInfoW(recover_monitor(h), &mi) && !EqualRect(&r, &mi.rcMonitor)) {
                g_recover_posted = true;
                PostMessageW(h, WM_TOTK_RECOVER, 0, 0);
            }
        }
        break;
    case WM_TOTK_RECOVER: {   // (checked again here: our own fix-up may have run in between, e.g. exclusive's way back)
        g_recover_posted = false;
        static int recovered;
        MONITORINFO mi = {sizeof mi};
        RECT r;
        HMONITOR mon = recover_monitor(h);
        if (g_fs && !g_applying && !g_excl_away && !IsIconic(h) && recovered < 20 && GetWindowRect(h, &r) &&
            GetMonitorInfoW(mon, &mi) && !EqualRect(&r, &mi.rcMonitor)) {
            recovered++;
            fprintf(stderr, "[win] fullscreen: the window was %ldx%ld at %ld,%ld, off its monitor: covering it again\n",
                    r.right - r.left, r.bottom - r.top, r.left, r.top);
            cover_monitor(h, mon);
        }
        return 0;
    }
    case WM_DPICHANGED: {   // (per-monitor DPI v2) keep the client size in pixels: the swapchain does not change size
        if (g_fs) return 0;  // (fullscreen: the popup is placed on the monitor rect)
        RECT c, *s = (RECT *)l;
        GetClientRect(h, &c);
        RECT r = {0, 0, c.right, c.bottom};
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE), HIWORD(w));
        SetWindowPos(h, NULL, s->left, s->top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return 0;
    }
    case WM_SIZE: keep_awake(w != SIZE_MINIMIZED); break;   // (minimised: the display may sleep again)
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize.x = 320, ((MINMAXINFO *)l)->ptMinTrackSize.y = 200; return 0;
    case WM_EXITSIZEMOVE: remember_place(h); break;   // (a drag to the other monitor, a resize)
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) { apply_fullscreen(h, !g_fs); return 0; }
        if (w == VK_F3 && !(l & (1 << 30))) { InterlockedExchange(&g_fps_osd, shim_fps_overlay() ? 0 : 1); return 0; }
        if (w == VK_F12 && !(l & (1 << 30))) { pc_shot_request(); return 0; }   // nxrecomp: screenshot (pc_shot.cpp)
        break;
    case WM_SYSKEYDOWN: {   // Alt+Enter
        // (Alt is also the walk, held while moving. Alt+Enter toggles only as a chord, Alt pressed under 1 s before
        // Enter, so Enter confirming a dialogue while walking keeps the window mode)
        static ULONGLONG alt_at;
        if (w == VK_MENU && !(l & (1 << 30))) alt_at = GetTickCount64();
        if (w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)) && GetTickCount64() - alt_at < 1000) {
            apply_fullscreen(h, !g_fs);
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
        // (every Alt+key but Alt+Space, the window menu: no SC_KEYMENU, which on a window with no menu bar beeps. With Alt
        // held to walk, each W A S D press is one)
        if (w != ' ') return 0;
        break;
    case WM_SYSCOMMAND:  // a lone Alt, F10 (the PC overlay) or Alt+a key: no window
                          // menu, which would hold the keyboard in windowed mode, nor its beep; Alt+Space still opens it
        if ((w & 0xFFF0) == SC_KEYMENU && l != ' ') return 0;
        break;
    case WM_SETCURSOR:
        if (g_fs && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_TOTK_TITLE: {
        wchar_t t[256];
        AcquireSRWLockShared(&g_title_lock);
        memcpy(t, g_title, sizeof t);
        ReleaseSRWLockShared(&g_title_lock);
        SetWindowTextW(h, t);
        return 0;
    }
    case WM_CLOSE:   // (Win+Shift+arrow moves: no size-move end; saved now, not 500 ms later: the game may be gone)
        remember_place(h); pc_settings_save_pending(); InterlockedExchange(&g_quit, 1); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool SDL_Init(uint32_t flags) { (void)flags; SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); return true; }
bool SDL_SetHint(const char *n, const char *v) { (void)n, (void)v; return true; }
const char *SDL_GetError(void) { return g_err; }

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {sizeof wc, CS_HREDRAW | CS_VREDRAW, wndproc, 0, 0, inst, LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION),
                      LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL, L"TotKRecomp", NULL};
    RegisterClassExW(&wc);
    {   // window modes (above): the fullscreen kind, the monitor and the exclusive display mode
        const char *m = getenv("TOTK_WINDOW_MODE"), *mon = getenv("TOTK_MONITOR"), *es = getenv("TOTK_EXCLUSIVE_SIZE"),
                   *ehz = getenv("TOTK_EXCLUSIVE_HZ");
        if (m && !strcmp(m, "exclusive")) g_fs_kind = SHIM_EXCLUSIVE;
        if (mon) g_monitor = atoi(mon) > 0 ? atoi(mon) : 0;
        int ew = 0, eh = 0;
        if (es && sscanf(es, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { g_excl_w = ew; g_excl_h = eh; }
        if (ehz && atoi(ehz) > 0) g_excl_hz = atoi(ehz);
    }
    // client size w x h in pixels (per-monitor DPI aware: SDL_Init), the frame measured at the DPI of the monitor the
    // window opens on: TOTK_MONITOR's; else the remembered one (remember_place): at the saved position if that is still
    // on it, else centred in its work area; else the primary's at the default position
    DWORD style = WS_OVERLAPPEDWINDOW;
    HMONITOR mon = NULL;
    if (g_monitor > 0) { MonPick k = {(int)g_monitor, 0, NULL}; EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&k); mon = k.found; }
    POINT saved = {0, 0};
    bool have_pos = false;
    {
        char v[64];
        wchar_t dev[32];
        if (pc_settings_raw("display.window_pos", v, sizeof v) && sscanf(v, "%ld,%ld", &saved.x, &saved.y) == 2) have_pos = true;
        if (!mon && pc_settings_raw("display.last_monitor", v, sizeof v) && MultiByteToWideChar(CP_UTF8, 0, v, -1, dev, 32) > 0)
            mon = monitor_by_name(dev);
        // (the window's corner, 8 px in, on a connected monitor, the chosen one if any: else the saved place is gone)
        POINT in = {saved.x + 8, saved.y + 8};
        HMONITOR pm = have_pos ? MonitorFromPoint(in, MONITOR_DEFAULTTONULL) : NULL;
        if (!pm || (mon && pm != mon)) have_pos = false;
        else if (!mon) mon = pm;
    }
    MONITORINFO mi = {sizeof mi};
    if (!mon || !GetMonitorInfoW(mon, &mi)) mon = NULL;
    UINT dpi = GetDpiForSystem();
    {
        typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        PFN_GetDpiForMonitor gdm = sh ? (PFN_GetDpiForMonitor)GetProcAddress(sh, "GetDpiForMonitor") : NULL;
        UINT dx, dy;
        HMONITOR dm = mon ? mon : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (gdm && gdm(dm, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy) == S_OK) dpi = dx;
    }
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (mon && have_pos) {   // (kept inside the work area: a size larger than last time must not hang off the edge)
        x = saved.x + (r.right - r.left) > mi.rcWork.right ? mi.rcWork.right - (r.right - r.left) : saved.x;
        y = saved.y + (r.bottom - r.top) > mi.rcWork.bottom ? mi.rcWork.bottom - (r.bottom - r.top) : saved.y;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    } else if (mon) {
        x = (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2;
        y = (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2;
    }
    wchar_t wt[128];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 128);
    g_win.hwnd = CreateWindowExW(0, L"TotKRecomp", wt, style, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_win.hwnd) { snprintf(g_err, sizeof g_err, "CreateWindowExW failed (%lu)", GetLastError()); return NULL; }
    {   // (CW_USEDEFAULT may have put it on a monitor of another DPI: the client size again, in pixels)
        RECT c;
        UINT wd = GetDpiForWindow(g_win.hwnd);
        if (wd && wd != dpi && GetClientRect(g_win.hwnd, &c) && (c.right != w || c.bottom != h)) {
            RECT a = {0, 0, w, h};
            AdjustWindowRectExForDpi(&a, style, FALSE, 0, wd);
            SetWindowPos(g_win.hwnd, NULL, 0, 0, a.right - a.left, a.bottom - a.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // TOTK_NO_FOCUS=1 (tools/run_test.sh; user 2026-10-18 "make sure they open up without focus"): shown behind the other
    // windows, not activated (not minimized: a minimized window has no swapchain extent, so nothing is presented)
    const char *nf = getenv("TOTK_NO_FOCUS");
    if (flags & SDL_WINDOW_HIDDEN) {}
    else if (nf && *nf && strcmp(nf, "0")) SetWindowPos(g_win.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else ShowWindow(g_win.hwnd, SW_SHOW);
    if (!(flags & SDL_WINDOW_HIDDEN)) keep_awake(true);
    return &g_win;
}
bool SDL_SetWindowFullscreen(SDL_Window *w, bool fs) {
    return w && w->hwnd && PostMessageW(w->hwnd, WM_TOTK_FULLSCREEN, fs, 0) != 0;
}
bool SDL_SetWindowTitle(SDL_Window *w, const char *title) {
    if (!w || !w->hwnd) return false;
    AcquireSRWLockExclusive(&g_title_lock);
    MultiByteToWideChar(CP_UTF8, 0, title, -1, g_title, 256);
    g_title[255] = 0;
    ReleaseSRWLockExclusive(&g_title_lock);
    return PostMessageW(w->hwnd, WM_TOTK_TITLE, 0, 0) != 0;
}
bool SDL_HideWindow(SDL_Window *w) { return w && w->hwnd && ShowWindowAsync(w->hwnd, SW_HIDE); }   // (the window's own thread hides it)
// (UTF-8 text; device.c gpu_unsupported: no parent, so the box stands on its own above a hidden or fullscreen window)
bool SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, SDL_Window *w) {
    (void)w;
    wchar_t *t = calloc(strlen(title) + 1, sizeof *t), *m = calloc(strlen(message) + 1, sizeof *m);
    if (!t || !m) { free(t); free(m); return false; }
    MultiByteToWideChar(CP_UTF8, 0, title, -1, t, (int)strlen(title) + 1);
    MultiByteToWideChar(CP_UTF8, 0, message, -1, m, (int)strlen(message) + 1);
    UINT icon = flags & SDL_MESSAGEBOX_ERROR ? MB_ICONERROR : flags & SDL_MESSAGEBOX_WARNING ? MB_ICONWARNING : MB_ICONINFORMATION;
    int r = MessageBoxW(NULL, m, t, MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST);
    free(t); free(m);
    return r != 0;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *pw, int *ph) {
    RECT r; GetClientRect(w->hwnd, &r);
    *pw = r.right - r.left; *ph = r.bottom - r.top;
    return true;
}
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *const ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    *count = 2;
    return ext;
}
bool SDL_Vulkan_CreateSurface(SDL_Window *w, VkInstance inst, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    VkWin32SurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = GetModuleHandleW(NULL),
                                      .hwnd = w->hwnd};
    VkResult r = vkCreateWin32SurfaceKHR(inst, &ci, a, out);
    if (r != VK_SUCCESS) snprintf(g_err, sizeof g_err, "vkCreateWin32SurfaceKHR %d", r);
    return r == VK_SUCCESS;
}
// pumps this thread's window messages; returns one event at a time (only quit for now)
bool SDL_WaitEventTimeout(SDL_Event *e, int32_t ms) {
    if (InterlockedExchange(&g_quit, 0)) { e->type = SDL_EVENT_QUIT; return true; }
    MsgWaitForMultipleObjects(0, NULL, FALSE, ms < 0 ? INFINITE : (DWORD)ms, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (InterlockedExchange(&g_quit, 0)) { e->type = SDL_EVENT_QUIT; return true; }
    return false;
}
Uint64 SDL_GetTicksNS(void) { return plat_now_ns(); }
Uint64 SDL_GetTicks(void) { return plat_now_ns() / 1000000u; }
void SDL_Delay(Uint32 ms) { Sleep(ms); }
// The CPUs this process may run on, not every CPU in the box: the pools sized from this (exec.c, textures.c) then fit a
// process limited by an affinity mask (a player's, or the 4c/8t tier boots) the way they fit a CPU that size.
// Unrestricted it is dwNumberOfProcessors, as before.
int SDL_GetNumLogicalCPUCores(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD_PTR pm = 0, sm = 0;
    int n = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm))
        for (; pm; pm &= pm - 1) n++;
    return n > 0 && n < (int)si.dwNumberOfProcessors ? n : (int)si.dwNumberOfProcessors;
}
// Surfaces only carry RGBA8 pixels (SDL_PIXELFORMAT_ABGR8888 = R,G,B,A bytes) for the PNG writer below.
struct SDL_Surface { int w, h, pitch; const uint8_t *px; };
SDL_Surface *SDL_CreateSurfaceFrom(int w, int h, uint32_t f, void *p, int pitch) {
    (void)f;
    SDL_Surface *s = malloc(sizeof *s);
    s->w = w; s->h = h; s->pitch = pitch; s->px = p;
    return s;
}
// Minimal PNG: 8-bit RGBA, zlib with stored (uncompressed) deflate blocks. Big files, no dependencies.
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
bool SDL_SavePNG(SDL_Surface *s, const char *path) {
    if (!s) { snprintf(g_err, sizeof g_err, "no surface"); return false; }
    size_t row = (size_t)s->w * 4 + 1, raw_n = row * s->h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < s->h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, s->px + (size_t)y * s->pitch, row - 1); }
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
    if (!f) { snprintf(g_err, sizeof g_err, "cannot open %s", path); free(raw); free(z); return false; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13]; png_be32(ihdr, s->w); png_be32(ihdr + 4, s->h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite(sig, 1, 8, f); png_chunk(f, "IHDR", ihdr, 13); png_chunk(f, "IDAT", z, (size_t)(q - z)); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return true;
}
void SDL_DestroySurface(SDL_Surface *s) { free(s); }

// ---- LASRecomp app layer
Settings g_settings = {.aspect = ASPECT_ORIGINAL, .fullscreen = false, .res_mode = RES_MATCH, .upscaler = UPSCALE_BILINEAR,
                       .antialias = AA_OFF};
void settings_load(void) {
    // (window modes, above) Borderless by default, the bare exe included: the overlay is on unless NXR_PC_OVERLAY=0
    // and its display.window_mode row defaults to borderless. Test boots stay windowed: boot.py always sets
    // LARECOMP_WINDOW, which device.c ui_window_init takes as windowed unless TOTK_WINDOW_MODE names a mode.
    const char *f = getenv("TOTK_FULLSCREEN"), *m = getenv("TOTK_WINDOW_MODE");
    g_settings.fullscreen = m && *m ? strcmp(m, "windowed") != 0 : f && *f ? strcmp(f, "0") != 0 : pc_overlay_enabled();
    // TOTK_AA=smaa: SMAA 1x on the presented image (exec.c post_process); TOTK_UPSCALE=fsr: FSR 1 (EASU + RCAS) instead
    // of bilinear when the window is larger than the image
    const char *aa = getenv("TOTK_AA"), *up = getenv("TOTK_UPSCALE");
    if (aa && !strcmp(aa, "smaa")) g_settings.antialias = AA_SMAA;
    if (up && !strcmp(up, "fsr")) g_settings.upscaler = UPSCALE_FSR1;
}
void pc_input_keys_restore(void);   // pc_input.cpp: the accessibility hotkeys back (no atexit on this path)
void plat_terminate(void) { pc_input_keys_restore(); TerminateProcess(GetCurrentProcess(), 0); }
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
void ui_size(int *w, int *h) { int a, b; SDL_GetWindowSizeInPixels(&g_win, &a, &b); *w = a; *h = b; }
int texpatch_identify(const NvnTexture *t) { (void)t; return -1; }
int texpatch_bottle_kind(int id) { (void)id; return 0; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
uint32_t texpatch_generation(void) { return 0; }
void texpatch_apply(int id, uint32_t lv, uint8_t *b, uint32_t w, uint32_t h, uint32_t row) { (void)id, (void)lv, (void)b, (void)w, (void)h, (void)row; }
int texpatch_heart_kind(int id) { (void)id; return 0; }  // (was -1: truthy, so any texture with tp > 0 tinted its draw)
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind; for (int i = 0; i < 8; i++) tint[i] = 1.0f; }
