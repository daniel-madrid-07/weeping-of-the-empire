// pc_display_caps.cpp: a monitor's modes, HDR and VRR (pc_display_caps.h). Windows only.
// Sources: the desktop mode and the fastest at its size (EnumDisplaySettings); its name, its EDID's registry path and
// whether Windows offers HDR for it (QueryDisplayConfig); HDR on and its peak (DXGI's IDXGIOutput6: the colour space is
// ST 2084 only with Windows HDR on); VRR from NVIDIA's driver when it drives the monitor (NVAPI, loaded at run time:
// nvapi64.dll is in every NVIDIA driver), else from the EDID (an AMD FreeSync block or a DisplayID Adaptive-Sync one).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "pc_display_caps.h"
#include "pc_text.h"

namespace {

std::string utf8(const wchar_t *w) {
    char b[256];
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr) > 0 ? b : "";
}

// QueryDisplayConfig's view of the GDI device: its EDID name, its device path and Windows' HDR state
struct Target { std::string name; std::wstring path; bool hdr_capable = false; };
Target target_of(const wchar_t *gdi) {
    Target t;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return t;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return t;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME s = {};
        s.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof s, paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&s.header) != ERROR_SUCCESS || wcscmp(gdi, s.viewGdiDeviceName)) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME n = {};
        n.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof n, paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&n.header) == ERROR_SUCCESS) {
            t.name = utf8(n.monitorFriendlyDeviceName);
            t.path = n.monitorDevicePath;
        }
        // (wideColorEnforced: Windows 11's colour management on an SDR panel, not HDR)
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO a = {};
        a.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof a, paths[i].targetInfo.adapterId,
                    paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&a.header) == ERROR_SUCCESS)
            t.hdr_capable = a.advancedColorSupported && !a.wideColorEnforced;
        break;
    }
    return t;
}

// The EDID Windows keeps for the monitor: \\?\DISPLAY#DEL4105#5&2b5e9b4&0&UID4352#{guid} is the device
// HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\DEL4105\5&2b5e9b4&0&UID4352, value "Device Parameters\EDID"
std::vector<uint8_t> edid_of(std::wstring path) {
    std::vector<uint8_t> e;
    if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    const size_t g = path.rfind(L"#{");
    if (path.empty() || g == std::wstring::npos) return e;
    path.resize(g);
    for (wchar_t &c : path) if (c == L'#') c = L'\\';
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + path + L"\\Device Parameters";
    DWORD n = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, nullptr, &n) != ERROR_SUCCESS || !n)
        return e;
    e.resize(n);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, e.data(), &n) != ERROR_SUCCESS)
        e.clear();
    else
        e.resize(n);
    return e;
}

// DXGI's output for the monitor: HDR on (ST 2084 colour space) and its peak. dxgi.dll loaded here (the Vulkan loader
// loads it anyway); the runtime does not link it.
bool dxgi_hdr(HMONITOR m, bool *on, int *peak) {
    static HMODULE dll = LoadLibraryW(L"dxgi.dll");
    using create_t = HRESULT(WINAPI *)(REFIID, void **);
    static auto create = dll ? (create_t)(void *)GetProcAddress(dll, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1 *f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void **)&f))) return false;
    bool found = false;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; !found && f->EnumAdapters1(i, &a) == S_OK; i++) {
        IDXGIOutput *o = nullptr;
        for (UINT j = 0; !found && a->EnumOutputs(j, &o) == S_OK; j++) {
            IDXGIOutput6 *o6 = nullptr;
            DXGI_OUTPUT_DESC1 d = {};
            if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void **)&o6)) && SUCCEEDED(o6->GetDesc1(&d)) &&
                d.Monitor == m) {
                found = true;
                *on = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                *peak = d.MaxLuminance > 0 && d.MaxLuminance < 20000 ? (int)(d.MaxLuminance + 0.5f) : 0;
            }
            if (o6) o6->Release();
            o->Release();
        }
        a->Release();
    }
    f->Release();
    return found;
}

// NVIDIA's VRR state for a GDI device -1: not an NVIDIA display (or no NVAPI). nvapi_QueryInterface ids from NVIDIA's
// nvapi_interface.h NV_GET_VRR_INFO_V1 from nvapi.h (a version mismatch is an error code, not a crash). Bits: 0
// bIsVRREnabled, 1 bIsVRRPossible, 2 bIsVRRRequested, 3 bIsVRRIndicatorEnabled, 4 bIsDisplayInVRRMode (newer drivers;
// older ones fill bit 0 only). Only bit 4 counts: bits 0 / 2 may be set under "full screen only" G-SYNC, where a
// borderless window gets no VRR, and no sample shows them yet (the first start logs the bits). Requiring bit 0 would
// never see G-SYNC on outside a VRR game (0 here with it on).
struct NvVrrInfo {
    uint32_t version;
    uint32_t bits;
    uint32_t reserved_ex[4];
};
int nv_vrr(const wchar_t *gdi, int *bits) {
    using qi_t = void *(__cdecl *)(uint32_t);
    using init_t = int(__cdecl *)();
    using id_t = int(__cdecl *)(const char *, uint32_t *);
    using vrr_t = int(__cdecl *)(uint32_t, NvVrrInfo *);
    static id_t by_name;
    static vrr_t vrr_info;
    static bool once = [] {
        HMODULE dll = LoadLibraryW(L"nvapi64.dll");
        auto qi = dll ? (qi_t)(void *)GetProcAddress(dll, "nvapi_QueryInterface") : nullptr;
        auto init = qi ? (init_t)qi(0x0150e828u) : nullptr;   // NvAPI_Initialize
        if (!init || init() != 0) return true;
        by_name = (id_t)qi(0xae457190u);                       // NvAPI_DISP_GetDisplayIdByDisplayName
        vrr_info = (vrr_t)qi(0xdf8fda57u);                     // NvAPI_Disp_GetVRRInfo
        return true;
    }();
    (void)once;
    uint32_t id = 0;
    const std::string name = utf8(gdi);
    if (!by_name || !vrr_info || by_name(name.c_str(), &id) != 0) return -1;
    NvVrrInfo v = {};
    v.version = (uint32_t)sizeof v | 1u << 16;
    if (vrr_info(id, &v) != 0) return -1;
    *bits = (int)v.bits;
    return v.bits & 16 ? PCD_VRR_ON : v.bits & 2 ? PCD_VRR_OFF : PCD_VRR_NONE;
}

// Per monitor (its device path; the GDI name where QueryDisplayConfig has none: RDP, indirect displays): its EDID,
// read once (it does not change while the monitor stays plugged in)
struct Known { std::wstring key; pcd_edid edid; bool edid_ok; };
std::mutex g_mu;
std::vector<Known> g_known;

// The overlay's copy (pcd_caps_cached): read on a thread of its own, never on the present thread, whose frame times
// F10 / F9 show
std::mutex g_cache_mu;
HMONITOR g_cache_mon;
pcd_caps g_cache;
bool g_cache_ok;
ULONGLONG g_cache_at;
unsigned g_cache_gen;
std::atomic<unsigned> g_gen{0};
std::atomic<bool> g_busy{false};

}  // namespace

int pcd_caps_of(void *monitor, pcd_caps *out) {
    HMONITOR m = monitor ? (HMONITOR)monitor : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    DEVMODEW cur = {};
    cur.dmSize = sizeof cur;
    if (!GetMonitorInfoW(m, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return 0;
    pcd_caps c = {};
    c.w = (int)cur.dmPelsWidth;
    c.h = (int)cur.dmPelsHeight;
    c.hz = c.top_hz = cur.dmDisplayFrequency > 1 ? (int)cur.dmDisplayFrequency : 0;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        if ((int)dm.dmPelsWidth == c.w && (int)dm.dmPelsHeight == c.h && (int)dm.dmDisplayFrequency > c.top_hz)
            c.top_hz = (int)dm.dmDisplayFrequency;
    const Target t = target_of(mi.szDevice);
    snprintf(c.name, sizeof c.name, "%s", t.name.empty() ? utf8(mi.szDevice).c_str() : t.name.c_str());
    c.hdr_capable = t.hdr_capable;
    bool hdr_on = false;
    int peak = 0;
    if (dxgi_hdr(m, &hdr_on, &peak)) { c.hdr_on = hdr_on; c.peak_nits = peak; }
    Known k = {};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const std::wstring key = t.path.empty() ? std::wstring(mi.szDevice) : t.path;
        bool have = false;
        for (const Known &e : g_known)
            if (e.key == key) { k = e; have = true; }
        if (!have) {
            const std::vector<uint8_t> e = edid_of(t.path);
            k.key = key;
            k.edid_ok = !e.empty() && pcd_edid_parse(e.data(), e.size(), &k.edid);
            g_known.push_back(k);
        }
    }
    if (k.edid_ok) {
        c.native_w = k.edid.native_w;
        c.native_h = k.edid.native_h;
    }
    // NVIDIA's driver decides where it drives the monitor (a FreeSync monitor it has not enabled G-SYNC for: off);
    // elsewhere the EDID
    c.nv_bits = -1;
    const int nv = nv_vrr(mi.szDevice, &c.nv_bits);
    const bool declared = k.edid_ok && (k.edid.freesync || k.edid.adaptive_sync);
    const char *edid_kind = k.edid_ok && k.edid.freesync ? "FreeSync" : "Adaptive-Sync";
    if (nv >= 0) {
        c.vrr = nv == PCD_VRR_NONE && declared ? PCD_VRR_OFF : nv;
        c.vrr_kind = nv != PCD_VRR_NONE ? "G-SYNC" : declared ? edid_kind : nullptr;
    } else if (declared) {
        c.vrr = PCD_VRR_MONITOR;
        c.vrr_kind = edid_kind;
    }
    if (c.vrr != PCD_VRR_NONE && k.edid_ok) {
        c.vrr_lo = k.edid.freesync_lo ? k.edid.freesync_lo : k.edid.range_lo;
        c.vrr_hi = k.edid.freesync_hi ? k.edid.freesync_hi : k.edid.range_hi;
        if (c.vrr_lo <= 0 || c.vrr_hi <= c.vrr_lo) c.vrr_lo = c.vrr_hi = 0;
    }
    *out = c;
    return 1;
}

// The reader thread: one at a time (g_busy), so the monitor and generation it reads can sit in globals, set before
// CreateThread (a Win32 thread: no std::thread, whose failure under -fno-exceptions ends the process)
static HMONITOR g_read_mon;
static unsigned g_read_gen;
static DWORD WINAPI cache_read(void *) {
    pcd_caps c;
    const bool ok = pcd_caps_of(g_read_mon, &c) != 0;
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        g_cache_mon = g_read_mon;
        g_cache = c;
        g_cache_ok = ok;
        g_cache_at = GetTickCount64();
        g_cache_gen = g_read_gen;
    }
    g_busy = false;
    return 0;
}

// The last read of the monitor asked for; a new read starts (on its own thread, one at a time) for another monitor,
// after a display change (pcd_caps_display_changed) or 10 s (Windows HDR turned on / off)
int pcd_caps_cached(void *monitor, pcd_caps *out) {
    HMONITOR m = monitor ? (HMONITOR)monitor : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const unsigned gen = g_gen.load();
    bool have, stale;
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        have = g_cache_ok && g_cache_mon == m;
        if (have) *out = g_cache;
        stale = !have || g_cache_gen != gen || GetTickCount64() - g_cache_at >= 10000;
    }
    if (stale && !g_busy.exchange(true)) {
        g_read_mon = m;
        g_read_gen = gen;
        if (HANDLE t = CreateThread(nullptr, 0, cache_read, nullptr, 0, nullptr)) CloseHandle(t);
        else g_busy = false;   // (no thread: the next call tries again)
    }
    return have;
}

void pcd_caps_display_changed() { g_gen++; }

// (whole fragments through pc_tr, so the Spanish reads as a sentence: pc_text_es.inc)
void pcd_caps_text(const pcd_caps &c, char *buf, size_t n) {
    int at = snprintf(buf, n, pc_tr("%s, %d x %d at %d Hz"), c.name, c.w, c.h, c.hz);
    auto add = [&](const char *fmt, auto... a) {
        if (at >= 0 && (size_t)at < n) at += snprintf(buf + at, n - at, fmt, a...);
    };
    if (c.top_hz > c.hz) add(pc_tr(" (it can do %d Hz: Windows display settings)"), c.top_hz);
    if (c.native_w && c.native_h && (c.native_w != c.w || c.native_h != c.h)) add(pc_tr(" (native %d x %d)"), c.native_w, c.native_h);
    add("%s", c.hdr_on ? pc_tr(". HDR on in Windows") : c.hdr_capable ? pc_tr(". HDR off in Windows (the monitor can)")
                                                       : pc_tr(". No HDR"));
    if (c.hdr_on && c.peak_nits) add(pc_tr(", %d nits"), c.peak_nits);
    switch (c.vrr) {
        case PCD_VRR_ON: add(pc_tr(". VRR on (%s"), c.vrr_kind); break;
        case PCD_VRR_OFF: add(pc_tr(". VRR off in the driver (%s monitor"), c.vrr_kind); break;
        case PCD_VRR_MONITOR:
            add(pc_tr(". VRR: a %s monitor, not confirmed by the driver (Options: VRR on if it is on there"), c.vrr_kind);
            break;
        default: add("%s", pc_tr(". No VRR")); break;
    }
    if (c.vrr != PCD_VRR_NONE) {
        if (c.vrr_lo) add(", %d-%d Hz", c.vrr_lo, c.vrr_hi);
        add("%s", ")");
    }
    add("%s", ".");
}

#endif  // _WIN32
