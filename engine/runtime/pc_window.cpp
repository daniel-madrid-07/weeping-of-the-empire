// pc_window.cpp: the overlay's rows for the present workstream's settings: window mode, monitor, window size,
// exclusive display mode, presentation, pacing, latency and HDR. Rows with the keys of pc_settings.cpp's stand-ins
// (display.window_mode, display.window_size) replace them. Live rows call shim_win.c (the window thread applies
// window changes) and present.h; the others are restart rows.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

#include "pc_settings.h"
#include "pc_display_modes.h"
#include "nvn/vulkan/present.h"

extern "C" {
int shim_window_mode(void);                          // 0 windowed, 1 borderless, 2 exclusive
void shim_set_window_mode(int mode, int monitor);   // monitor: -1 keep, 0 the window's own, 1..N
void shim_set_exclusive_mode(int w, int h, int hz);
void *shim_window_hwnd(void);
}

namespace {

void env_put(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }
int mode_of(const char *v) { return !strcmp(v, "exclusive") ? 2 : !strcmp(v, "borderless") ? 1 : 0; }

const char *window_mode_now() {
    int m = shim_window_mode();
    return m == 2 ? "exclusive" : m == 1 ? "borderless" : "windowed";
}
void window_mode_env(const char *v) {
    env_put("TOTK_WINDOW_MODE", v);
    env_put("TOTK_FULLSCREEN", mode_of(v) ? "1" : "0");
}
void window_mode_apply(const char *v) { shim_set_window_mode(mode_of(v), -1); }

void window_size_env(const char *v) {
    // LARECOMP_WINDOW also keeps the window from starting fullscreen (device.c ui_window_init): windowed only
    const char *m = pc_settings_get("display.window_mode");
    env_put("LARECOMP_WINDOW", m && strcmp(m, "windowed") == 0 ? v : "");
}
// Live, windowed only: the client area to WxH ("" = 1280 x 720), measured at the window's DPI. SWP_ASYNCWINDOWPOS: the
// window thread does it; vk_frame_begin then follows the new size with a new swapchain.
void window_size_apply(const char *v) {
    HWND h = (HWND)shim_window_hwnd();
    int w = 1280, ht = 720;
    if (!h || shim_window_mode() != 0 || (*v && sscanf(v, "%dx%d", &w, &ht) != 2)) return;
    RECT r = {0, 0, w, ht};
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE), GetDpiForWindow(h));
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void monitor_apply(const char *v) { shim_set_window_mode(shim_window_mode(), *v ? atoi(v) : 0); }

void exclusive_apply(const char *) {
    const char *s = pc_settings_get("display.exclusive_size"), *hz = pc_settings_get("display.exclusive_refresh");
    int w = 0, h = 0;
    if (s && *s && sscanf(s, "%dx%d", &w, &h) != 2) w = h = 0;
    shim_set_exclusive_mode(w, h, hz && *hz ? atoi(hz) : 0);
}

// Vsync and VRR, the two choices players know, instead of the raw present modes:
//   vsync on          MAILBOX: no tearing, the engine's default (the game paces itself: device.c present_mode)
//   vsync on + VRR    FIFO: no tearing below the panel's maximum (MAILBOX defeats VRR on NVIDIA: frames replaced at the
//                     panel's fastest rate)
//   vsync off (± VRR) IMMEDIATE: tears (with VRR only above the panel's range)
// LARECOMP_VK_PRESENT (tests, and any mode) stays in charge while settings.ini sets neither row.
int present_of_rows() {
    const char *vs = pc_settings_get("display.vsync"), *vrr = pc_settings_get("display.vrr");
    // (a surface without IMMEDIATE: mailbox, not device.c's fallback FIFO, which would queue frames behind the vblank)
    if (vs && !strcmp(vs, "off")) return vk_present_mode_supported(VKP_IMMEDIATE) == 0 ? VKP_MAILBOX : VKP_IMMEDIATE;
    return vrr && !strcmp(vrr, "on") ? VKP_FIFO : VKP_MAILBOX;
}
void present_env(const char *) {   // (both rows: the later one's start value sees the earlier one's)
    int m = present_of_rows();
    env_put("LARECOMP_VK_PRESENT", m == VKP_IMMEDIATE ? "immediate" : m == VKP_FIFO ? "fifo" : "mailbox");
}
void present_apply(const char *) {   // (not through the environment: getenv runs on other threads meanwhile)
    vk_set_present_mode(present_of_rows());
    vk_request_swapchain_recreate();   // vk_frame_begin, on the present thread: device idle, then the new swapchain
}
void queued_apply(const char *v) { vk_present_set_max_queued(atoi(v)); }
void low_latency_apply(const char *v) {
    vk_present_set_low_latency(!strcmp(v, "boost") ? 2 : !strcmp(v, "on") ? 1 : !strcmp(v, "jit") ? 4 : 0);
}
void fif_apply(const char *v) { vk_present_set_frames_in_flight(*v ? atoi(v) : 2); }
// hle_vi.cpp: where the cap waits and its spin, read per frame (the vsync tick's source stays a start value: its thread
// picks the clock or the monitor's vblank loop once)
extern "C" void vi_set_pace_at(const char *v);
extern "C" void vi_set_pace_spin(const char *v);
void pace_at_apply(const char *v) { vi_set_pace_at(v); }
void pace_spin_apply(const char *v) { vi_set_pace_spin(v); }
void hdr_apply(const char *) {
    const char *m = pc_settings_get("display.hdr"), *pk = pc_settings_get("display.hdr_peak"), *pw = pc_settings_get("display.hdr_paper");
    vk_present_set_hdr(m && !strcmp(m, "hdr10") ? VKP_HDR10 : m && !strcmp(m, "scrgb") ? VKP_HDR_SCRGB : VKP_HDR_OFF,
                       pk ? (float)atof(pk) : 1000.0f, pw ? (float)atof(pw) : 200.0f);
}

const char *const V_WINDOW[] = {"windowed", "borderless", "exclusive", nullptr};
const char *const N_WINDOW[] = {"Windowed", "Borderless fullscreen (default)", "Exclusive fullscreen", nullptr};
// The monitors as they are at start, in shim_win.c's order (EnumDisplayMonitors), named as Windows' display settings
// name them (e.g. "DELL P2422H"), with their desktop mode; and the exclusive lists from the monitor the game starts on
// (shim_win.c's choice: TOTK_MONITOR, else the remembered one, else the primary). A monitor plugged in later shows at
// the next start; on another monitor shim_win.c sets the closest mode it has.
struct MonInfo { std::wstring dev; std::string name; bool primary; pcd_mode desk; };
BOOL CALLBACK mons_enum(HMONITOR m, HDC, LPRECT, LPARAM p) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        ((std::vector<MonInfo> *)p)->push_back({mi.szDevice, "", (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                                                 {(int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency}});
    return TRUE;
}
std::string utf8(const wchar_t *w) {
    char b[256];
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr) > 0 ? b : "";
}
std::string friendly_name(const std::wstring &gdi) {   // (QueryDisplayConfig: the EDID name; "" when it has none)
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return "";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return "";
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME s = {};
        s.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof s, paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&s.header) != ERROR_SUCCESS || gdi != s.viewGdiDeviceName) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME t = {};
        t.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof t, paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&t.header) == ERROR_SUCCESS) return utf8(t.monitorFriendlyDeviceName);
    }
    return "";
}
constexpr int kMaxMon = 8, kMaxSizes = 24, kMaxRates = 16;
std::string g_mon_names[kMaxMon + 1], g_size_v[kMaxSizes + 1], g_size_n[kMaxSizes + 1], g_rate_v[kMaxRates + 1], g_rate_n[kMaxRates + 1];
const char *V_MONITOR[kMaxMon + 2], *N_MONITOR[kMaxMon + 2];
const char *V_XSIZE[kMaxSizes + 3], *N_XSIZE[kMaxSizes + 3], *V_HZ[kMaxRates + 3], *N_HZ[kMaxRates + 3];
const char *const kMonV[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
// (a saved value the list lacks, from another monitor: kept as a choice, so the row shows it and nothing is lost)
std::string g_size_keep_n, g_rate_keep_n, g_size_keep_v, g_rate_keep_v;
void build_display_lists() {
    std::vector<MonInfo> mons;
    EnumDisplayMonitors(nullptr, nullptr, mons_enum, (LPARAM)&mons);
    if ((int)mons.size() > kMaxMon) mons.resize(kMaxMon);
    V_MONITOR[0] = "";
    N_MONITOR[0] = "Remember (the one played on last, default)";
    int start = -1;
    char v[64];
    for (int i = 0; i < (int)mons.size(); i++) {
        std::string f = friendly_name(mons[i].dev);
        char b[192];
        snprintf(b, sizeof b, "Monitor %d: %s, %d x %d%s", i + 1, f.empty() ? utf8(mons[i].dev.c_str()).c_str() : f.c_str(),
                 mons[i].desk.w, mons[i].desk.h, mons[i].primary ? " (main)" : "");
        g_mon_names[i] = b;
        V_MONITOR[i + 1] = kMonV[i];
        N_MONITOR[i + 1] = g_mon_names[i].c_str();
        if (mons[i].primary && start < 0) start = i;
    }
    V_MONITOR[mons.size() + 1] = N_MONITOR[mons.size() + 1] = nullptr;
    // the start monitor, as shim_win.c picks it
    if (pc_settings_raw("display.monitor", v, sizeof v) && atoi(v) >= 1 && atoi(v) <= (int)mons.size()) start = atoi(v) - 1;
    else if (pc_settings_raw("display.last_monitor", v, sizeof v))
        for (int i = 0; i < (int)mons.size(); i++)
            if (utf8(mons[i].dev.c_str()) == v) start = i;
    std::vector<pcd_mode> all;
    if (start >= 0) {
        DEVMODEW e = {};
        e.dmSize = sizeof e;
        for (DWORD i = 0; all.size() < 1024 && EnumDisplaySettingsW(mons[start].dev.c_str(), i, &e); i++)
            all.push_back({(int)e.dmPelsWidth, (int)e.dmPelsHeight, (int)e.dmDisplayFrequency});
    }
    pcd_mode sizes[kMaxSizes];
    int rates[kMaxRates];
    const int ns = pcd_sizes(all.data(), (int)all.size(), sizes, kMaxSizes);
    const int nr = pcd_rates(all.data(), (int)all.size(), rates, kMaxRates);
    int k = 0;
    V_XSIZE[k] = ""; N_XSIZE[k++] = "The desktop's (default)";
    for (int i = 0; i < ns; i++) {
        snprintf(v, sizeof v, "%dx%d", sizes[i].w, sizes[i].h);
        g_size_v[i] = v;
        snprintf(v, sizeof v, "%d x %d", sizes[i].w, sizes[i].h);
        g_size_n[i] = v;
        V_XSIZE[k] = g_size_v[i].c_str(); N_XSIZE[k++] = g_size_n[i].c_str();
    }
    if (pc_settings_raw("display.exclusive_size", v, sizeof v) && *v) {
        bool in = false;
        for (int i = 0; i < ns; i++) in = in || g_size_v[i] == v;
        if (!in) {
            g_size_keep_v = v;
            g_size_keep_n = g_size_keep_v + " (not on this monitor: the desktop's)";
            V_XSIZE[k] = g_size_keep_v.c_str(); N_XSIZE[k++] = g_size_keep_n.c_str();
        }
    }
    V_XSIZE[k] = N_XSIZE[k] = nullptr;
    k = 0;
    V_HZ[k] = ""; N_HZ[k++] = "The desktop's (default)";
    for (int i = 0; i < nr; i++) {
        snprintf(v, sizeof v, "%d", rates[i]);
        g_rate_v[i] = v;
        snprintf(v, sizeof v, "%d Hz", rates[i]);
        g_rate_n[i] = v;
        V_HZ[k] = g_rate_v[i].c_str(); N_HZ[k++] = g_rate_n[i].c_str();
    }
    if (pc_settings_raw("display.exclusive_refresh", v, sizeof v) && *v) {
        bool in = false;
        for (int i = 0; i < nr; i++) in = in || g_rate_v[i] == v;
        if (!in) {
            g_rate_keep_v = v;
            g_rate_keep_n = g_rate_keep_v + " Hz (not on this monitor: the closest it has)";
            V_HZ[k] = g_rate_keep_v.c_str(); N_HZ[k++] = g_rate_keep_n.c_str();
        }
    }
    V_HZ[k] = N_HZ[k] = nullptr;
    fprintf(stderr, "[win] display lists: %d monitor(s); start monitor %d: %d resolutions, %d refresh rates\n", (int)mons.size(),
            start + 1, ns, nr);
}
// (21:9 and 32:9: the ultrawide canvases fill a window of their shape, device.c backend_present; a window larger
// than the desktop is cut to it by Windows, an exclusive mode the monitor lacks falls back to the desktop's)
const char *const V_SIZE[] = {"", "1600x900", "1920x1080", "2560x1440", "3840x2160", "2560x1080", "3440x1440", "3840x1600",
                              "5120x1440", nullptr};
const char *const N_SIZE[] = {"1280 x 720 (default)", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160",
                              "2560 x 1080 (21:9)", "3440 x 1440 (21:9)", "3840 x 1600 (21:9)", "5120 x 1440 (32:9)", nullptr};
const char *const V_VSYNC_ON[] = {"", "off", nullptr};
const char *const N_VSYNC_ON[] = {"On (default, no tearing)", "Off (tearing, lowest latency)", nullptr};
const char *const V_VRR[] = {"", "on", nullptr};
const char *const N_VRR[] = {"Off (default)", "On (G-Sync / FreeSync monitor)", nullptr};
const char *const V_PACE[] = {"", "frame", nullptr};
const char *const N_PACE[] = {"At the present (default, steadier)", "Before the frame (lower latency)", nullptr};
const char *const V_VSYNC[] = {"", "display", nullptr};
const char *const N_VSYNC[] = {"60 Hz clock (default)", "The monitor's vblank (fixed refresh only)", nullptr};
const char *const V_SPIN[] = {"", "300", "500", "1000", nullptr};
const char *const N_SPIN[] = {"Off (default)", "0.3 ms", "0.5 ms", "1 ms", nullptr};
const char *const V_QUEUED[] = {"0", "1", "2", "3", nullptr};
const char *const N_QUEUED[] = {"No limit", "1", "2 (default)", "3", nullptr};
const char *const V_FIF[] = {"", "3", "1", nullptr};
// (1 frame in flight was "lowest latency": measured mouse -> screen it is slower where the CPU and GPU are both busy,
// 30.0 ms and 86.6 fps against 25.0 ms and 113.9 at 2)
const char *const N_FIF[] = {"2 (default)", "3", "1 (lockstep: slower when CPU and GPU are both busy)", nullptr};
const char *const V_LL[] = {"", "on", "boost", "jit", nullptr};
const char *const N_LL[] = {"Off (default)", "On (NVIDIA Reflex; other GPUs: the engine's own)", "On + boost",
                            "On (the engine's own, any GPU)", nullptr};
const char *const V_HDR[] = {"", "hdr10", "scrgb", nullptr};
const char *const N_HDR[] = {"Off (SDR, default)", "HDR10", "scRGB", nullptr};

const unsigned R = PC_SET_RESTART;
const pc_setting k_present[] = {
    // key, page, section, label, help, default, values, names, min, max, step, flags, env, to_env, apply, live_value
    {"display.window_mode", "Graphics", "Display", "Window mode", "TOTK_WINDOW_MODE. F11 or Alt+Enter switch between "
     "windowed and the fullscreen kind chosen here.", "borderless", V_WINDOW, N_WINDOW, 0, 0, 0, 0, nullptr, window_mode_env,
     window_mode_apply, window_mode_now},
    {"display.monitor", "Graphics", "Display", "Monitor", "TOTK_MONITOR: where fullscreen goes and a windowed start opens. "
     "Remember: the monitor the game was on last (drag the window there, or pick it once), at the same window position.", "",
     V_MONITOR, N_MONITOR, 0, 0, 0, 0, "TOTK_MONITOR", nullptr, monitor_apply, nullptr},
    {"display.window_size", "Graphics", "Display", "Window size", "Windowed mode only (LARECOMP_WINDOW at start).", "",
     V_SIZE, N_SIZE, 0, 0, 0, 0, nullptr, window_size_env, window_size_apply, nullptr},
    {"display.exclusive_size", "Graphics", "Display", "Exclusive resolution", "TOTK_EXCLUSIVE_SIZE: the monitor's mode in "
     "exclusive fullscreen; the list is the modes of the monitor the game started on. Borderless and windowed keep the "
     "desktop's.", "", V_XSIZE, N_XSIZE, 0, 0, 0, 0, "TOTK_EXCLUSIVE_SIZE", nullptr, exclusive_apply, nullptr},
    {"display.exclusive_refresh", "Graphics", "Display", "Exclusive refresh rate", "TOTK_EXCLUSIVE_HZ: the refresh rate in "
     "exclusive fullscreen. A rate the resolution lacks gives the fastest it has under it.", "", V_HZ, N_HZ, 0, 0, 0,
     0, "TOTK_EXCLUSIVE_HZ", nullptr, exclusive_apply, nullptr},
    {"display.vsync", "Graphics", "Presentation", "Vsync", "Off: frames go out at once and may tear (LARECOMP_VK_PRESENT="
     "immediate); on: mailbox, or FIFO with VRR.", "", V_VSYNC_ON, N_VSYNC_ON, 0, 0, 0, 0, nullptr, present_env,
     present_apply, nullptr},
    {"display.vrr", "Graphics", "Presentation", "Variable refresh (VRR)", "Only with a G-SYNC / FreeSync monitor (and, "
     "windowed, the driver's windowed VRR on): the panel follows each frame (FIFO). Without one it is plain vsync at a "
     "fixed refresh, and a cap under it repeats frames. Best with a frame cap a little under the refresh (116 at 120 Hz, "
     "138 at 144 Hz); Unlocked and DLSS frame generation keep under it by themselves.", "",
     V_VRR, N_VRR, 0, 0, 0, 0, nullptr, present_env, present_apply, nullptr},
    {"display.pace_at", "Graphics", "Presentation", "Frame pacing", "TOTK_PACE_AT: where the frame cap waits.", "", V_PACE,
     N_PACE, 0, 0, 0, 0, "TOTK_PACE_AT", nullptr, pace_at_apply, nullptr},
    {"display.vsync_source", "Graphics", "Presentation", "Game tick source", "TOTK_VSYNC: what the game's 60 Hz tick follows.", "",
     V_VSYNC, N_VSYNC, 0, 0, 0, R, "TOTK_VSYNC", nullptr, nullptr, nullptr},
    {"display.pace_spin", "Graphics", "Presentation", "Precise pacing", "TOTK_PACE_SPIN_US: the cap's last part spins (CPU).", "",
     V_SPIN, N_SPIN, 0, 0, 0, 0, "TOTK_PACE_SPIN_US", nullptr, pace_spin_apply, nullptr},
    {"display.max_queued", "Graphics", "Presentation", "Queued frames", "TOTK_MAX_QUEUED: frames waiting to be shown (needs "
     "present wait).", "2", V_QUEUED, N_QUEUED, 0, 0, 0, 0, "TOTK_MAX_QUEUED", nullptr, queued_apply, nullptr},
    {"display.frames_in_flight", "Graphics", "Presentation", "GPU frames in flight", "TOTK_FRAMES_IN_FLIGHT.", "", V_FIF, N_FIF,
     0, 0, 0, 0, "TOTK_FRAMES_IN_FLIGHT", nullptr, fif_apply, nullptr},
    {"display.low_latency", "Graphics", "Presentation", "Low latency", "TOTK_REFLEX: each frame is held back "
     "before it reads input, by as much as it would otherwise wait for the GPU, the render thread or the frame cap "
     "later. NVIDIA: Reflex's sleep (VK_NV_low_latency2); other GPUs, or jit: the engine's own. Boost keeps the GPU "
     "clocks up (Reflex).", "", V_LL, N_LL, 0, 0, 0, 0, "TOTK_REFLEX", nullptr, low_latency_apply, nullptr},
    {"display.hdr", "Graphics", "HDR", "HDR output", "TOTK_HDR: needs Windows HDR on for the monitor.", "", V_HDR, N_HDR, 0, 0,
     0, 0, "TOTK_HDR", nullptr, hdr_apply, nullptr},
    {"display.hdr_peak", "Graphics", "HDR", "Peak brightness (nits)", "TOTK_HDR_PEAK: the display's peak.", "1000", nullptr,
     nullptr, 400.0f, 4000.0f, 50.0f, 0, "TOTK_HDR_PEAK", nullptr, hdr_apply, nullptr},
    {"display.hdr_paper", "Graphics", "HDR", "Paper white (nits)", "TOTK_HDR_PAPER: the brightness of SDR white.", "200",
     nullptr, nullptr, 80.0f, 500.0f, 10.0f, 0, "TOTK_HDR_PAPER", nullptr, hdr_apply, nullptr},
};

struct AddPresent {
    AddPresent() {
        build_display_lists();   // (before the rows go in: their choice lists point at these arrays)
        pc_settings_add(k_present, (int)(sizeof k_present / sizeof k_present[0]));
    }
} g_add_present;

}  // namespace
