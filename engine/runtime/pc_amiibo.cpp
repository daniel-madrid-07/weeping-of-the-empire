// pc_amiibo.cpp: amiibo for the player's own figures (pc_amiibo.h; rival parity: totk-pc-recomp 0.3.1 reads dumps too,
// its #235). nn::nfp for the 14 functions TotK 1.0.0 imports (main.elf: Initialize... GetModelInfo; no Unmount, Flush or
// application area: the game reads which figure it is and drops its items) over one virtual reader: ListDevices finds
// it; StartDetection makes it search (and a notice points to F10 > Amiibo); the tab's Scan puts a dump on it (TagFound,
// the activate event); Mount, GetTagInfo and GetModelInfo read it (amiibo_dump.h); StopDetection takes it off (the
// deactivate event if a tag was on). Dumps come from the TOTK_AMIIBO_DIR folders (';' between them; the Windows
// launcher's amiibo folder beside the game and %APPDATA%\nxrecomp\TotK-engine\amiibo). Before 2026-11-02 hle_oe.cpp
// answered "no reader"; its Initialize / GetState and the availability event are kept as they were.
#include "pc_amiibo.h"
#include "mods_scan.h"   // (env_u8 / from_u8: UTF-8 paths that never throw, public #94)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../third_party/imgui/imgui.h"
#include "amiibo_dump.h"
#include "pc_amiibo_hold.h"
#include "hle.h"
#include "pc_text.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

void hle_sysevent_init(u64 ev, bool autoclear);   // hle_os_thread.cpp
void hle_sysevent_signal(u64 ev);
extern "C" void osd_toast(const char *text, int ms);   // hle_osd.cpp

namespace {
namespace fs = std::filesystem;
constexpr u32 kDeviceNotFound = 0x8073;   // nfp module 115, description 64
constexpr u32 kTagNotFound = 0xC273;      // description 97
constexpr u64 kHandle = 0x4F4249494D41;   // our reader's DeviceHandle (opaque to the game)
enum { DevInitialized = 0, DevSearching = 1, DevTagFound = 2, DevTagRemoved = 3, DevTagMounted = 4 };

std::atomic<u32> g_nfp_state{0};   // the library: 0 NonInitialized, 1 Initialized
std::mutex g_mtx;                  // the reader below (guest threads and the F10 tab)
int g_dev = DevInitialized;
u64 g_activate, g_deactivate;      // the game's SystemEventType for each (0: not attached)
amiibo_tag g_tag;
std::string g_scanned;             // the name of the dump on the reader, for the tab
AmiiboHold g_hold;                 // the search between a stop and the next start (pc_amiibo_hold.h)
amiibo_tag g_pending_tag;          // a Scan in g_hold's hold: goes on at the next start
std::string g_pending;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ours(u64 handle) { return handle && *(const u64 *)handle == kHandle; }
bool tag_on() { return g_dev == DevTagFound || g_dev == DevTagMounted; }

std::vector<std::string> folders() {
    std::vector<std::string> v;
    std::stringstream ss(mods_scan::env_u8("TOTK_AMIIBO_DIR"));   // (UTF-8, not getenv's ANSI code page: #94)
    for (std::string d; std::getline(ss, d, ';');)
        if (!d.empty()) v.push_back(d);
    return v;
}
fs::path from_u8(const std::string &s) { return mods_scan::from_u8(s); }
std::string to_u8(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct Dump {
    std::string name, path;
    int problem;   // amiibo_parse's AMIIBO_* (AMIIBO_OK: usable)
    amiibo_tag tag;
};
// The dumps of the folders, read again at most once a second while the tab is drawn (a few small files)
const std::vector<Dump> &dumps() {
    static std::vector<Dump> list;
    static std::chrono::steady_clock::time_point last;
    static bool read;
    const auto now = std::chrono::steady_clock::now();
    if (read && now - last < std::chrono::seconds(1)) return list;
    read = true, last = now;
    list.clear();
    for (const std::string &dir : folders()) {
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(from_u8(dir), ec)) {
            std::string ext = to_u8(e.path().extension());
            for (char &ch : ext) ch = (char)tolower((unsigned char)ch);
            if (!e.is_regular_file(ec) || (ext != ".bin" && ext != ".nfc")) continue;
            Dump d{to_u8(e.path().stem()), to_u8(e.path()), AMIIBO_UNREADABLE, {}};
            if (e.file_size(ec) <= 64 * 1024) {
                std::ifstream f(e.path(), std::ios::binary);
                std::ostringstream body;
                body << f.rdbuf();
                if (f) d.problem = amiibo_parse(amiibo_image(body.str()), &d.tag);
            }
            list.push_back(d);
        }
    }
    return list;
}
const char *problem_text(int p) {
    switch (p) {
    case AMIIBO_SIZE: return pc_tr("Not an amiibo dump: an amiibo image is 540 bytes.");
    case AMIIBO_NO_CC: return pc_tr("Not an amiibo dump: the amiibo marker is missing.");
    case AMIIBO_FORMAT: return pc_tr("Not an amiibo dump: unknown figure data.");
    case AMIIBO_UNREADABLE: return pc_tr("The file cannot be read.");
    default: return nullptr;
    }
}
// Scan: the dump goes on the reader while the game searches for one (or at its next start, just after a stop)
bool scan(const Dump &d) {
    u64 ev = 0;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (d.problem) return false;
        if (g_dev != DevSearching) {
            if (g_dev != DevInitialized || !g_hold.queue(now_ms())) return false;
            g_pending_tag = d.tag;
            g_pending = d.name;
            printf("[nfp] amiibo %s waits for the game's next search\n", d.name.c_str());
            return true;
        }
        g_tag = d.tag;
        g_scanned = d.name;
        g_dev = DevTagFound;
        ev = g_activate;
    }
    if (ev) hle_sysevent_signal(ev);
    printf("[nfp] amiibo %s on the reader (id %02x%02x%02x%02x%02x%02x%02x%02x)\n", d.name.c_str(), d.tag.id[0], d.tag.id[1],
           d.tag.id[2], d.tag.id[3], d.tag.id[4], d.tag.id[5], d.tag.id[6], d.tag.id[7]);
    return true;
}
bool open_folder() {
    const std::vector<std::string> dirs = folders();
    if (dirs.empty()) return false;
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(from_u8(dirs[0]), ec);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // (pc_mods_ui.cpp)
    const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", from_u8(dirs[0]).c_str(), nullptr, nullptr,
                                                            SW_SHOWNORMAL)) > 32;
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
#else
    return false;
#endif
}
}   // namespace

// ---- nn::nfp. Arguments as declared (free functions: x0, x1, x2); the result in x0
HLE_AS(nfp_Initialize, "_ZN2nn3nfp10InitializeEv") {
    g_nfp_state = 1;
    std::lock_guard<std::mutex> l(g_mtx);
    g_dev = DevInitialized;
    g_hold.reset();
    c->x[0] = 0;
}
HLE_AS(nfp_Finalize, "_ZN2nn3nfp8FinalizeEv") {
    g_nfp_state = 0;
    std::lock_guard<std::mutex> l(g_mtx);
    g_dev = DevInitialized;
    g_hold.reset();
    g_activate = g_deactivate = 0;
    c->x[0] = 0;
}
HLE_AS(nfp_GetState, "_ZN2nn3nfp8GetStateEv") { c->x[0] = g_nfp_state.load(); }   // 0 NonInitialized, 1 Initialized
HLE_AS(nfp_ListDevices, "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii") {   // (DeviceHandle* out, int* count, int max)
    const bool one = g_nfp_state.load() && c->x[0] && (s32)c->x[2] >= 1;
    if (one) *(u64 *)c->x[0] = kHandle;
    if (c->x[1]) *(s32 *)c->x[1] = one ? 1 : 0;
    c->x[0] = one ? 0 : kDeviceNotFound;
}
HLE_AS(nfp_AttachAvailabilityChangeEvent, "_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE") {
    if (c->x[0]) hle_sysevent_init(c->x[0], false);   // (the reader never comes or goes)
}
HLE_AS(nfp_GetDeviceState, "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE") {
    std::lock_guard<std::mutex> l(g_mtx);
    c->x[0] = ours(c->x[0]) ? (u64)g_dev : (u64)DevInitialized;
}
HLE_AS(nfp_GetNpadId, "_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE") {   // (u32* out, handle): the first player's pad
    if (!ours(c->x[1])) { c->x[0] = kDeviceNotFound; return; }
    if (c->x[0]) *(u32 *)c->x[0] = 0;
    c->x[0] = 0;
}
HLE_AS(nfp_AttachActivateEvent, "_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE") {
    if (!ours(c->x[1]) || !c->x[0]) { c->x[0] = kDeviceNotFound; return; }
    hle_sysevent_init(c->x[0], true);   // (autoclear: the game's wait takes the signal)
    std::lock_guard<std::mutex> l(g_mtx);
    g_activate = c->x[0];
    c->x[0] = 0;
}
HLE_AS(nfp_AttachDeactivateEvent, "_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE") {
    if (!ours(c->x[1]) || !c->x[0]) { c->x[0] = kDeviceNotFound; return; }
    hle_sysevent_init(c->x[0], true);
    std::lock_guard<std::mutex> l(g_mtx);
    g_deactivate = c->x[0];
    c->x[0] = 0;
}
HLE_AS(nfp_StartDetection, "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE") {
    if (!ours(c->x[0])) { c->x[0] = kDeviceNotFound; return; }
    bool fresh, deliver;
    int restarts;
    u64 ev = 0;
    std::string name;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        fresh = g_hold.start(now_ms(), &deliver);
        restarts = g_hold.restarts;
        g_dev = DevSearching;
        g_scanned.clear();
        if (deliver) {   // (a Scan clicked between the last stop and this start)
            g_tag = g_pending_tag;
            g_scanned = name = g_pending;
            g_dev = DevTagFound;
            ev = g_activate;
        }
    }
    if (fresh) {
        osd_toast(pc_tr("amiibo: pick one in F10 (or L3 + R3) > Amiibo"), 6000);
        printf("[nfp] the game searches for an amiibo\n");
    } else if (restarts == 1) {
        printf("[nfp] the game restarts its search (the same one: no more lines until it ends)\n");
    }
    if (deliver) printf("[nfp] amiibo %s on the reader (clicked between the game's stop and start)\n", name.c_str());
    if (ev) hle_sysevent_signal(ev);
    c->x[0] = 0;
}
HLE_AS(nfp_StopDetection, "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE") {
    if (!ours(c->x[0])) { c->x[0] = kDeviceNotFound; return; }
    u64 ev = 0;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (tag_on()) ev = g_deactivate;
        g_hold.stop(now_ms(), g_dev == DevSearching, tag_on());
        g_dev = DevInitialized;
    }
    if (ev) hle_sysevent_signal(ev);
    c->x[0] = 0;
}
HLE_AS(nfp_Mount, "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE") {
    if (!ours(c->x[0])) { c->x[0] = kDeviceNotFound; return; }
    std::lock_guard<std::mutex> l(g_mtx);
    if (!tag_on()) { c->x[0] = kTagNotFound; return; }
    g_dev = DevTagMounted;
    c->x[0] = 0;
}
HLE_AS(nfp_GetTagInfo, "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE") {   // (TagInfo* out, handle)
    if (!ours(c->x[1]) || !c->x[0]) { c->x[0] = kDeviceNotFound; return; }
    std::lock_guard<std::mutex> l(g_mtx);
    if (!tag_on()) { c->x[0] = kTagNotFound; return; }
    amiibo_tag_info(g_tag, (uint8_t *)c->x[0]);
    c->x[0] = 0;
}
HLE_AS(nfp_GetModelInfo, "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE") {   // (ModelInfo* out, handle)
    if (!ours(c->x[1]) || !c->x[0]) { c->x[0] = kDeviceNotFound; return; }
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_dev != DevTagMounted) { c->x[0] = kTagNotFound; return; }
    amiibo_model_info(g_tag, (uint8_t *)c->x[0]);
    c->x[0] = 0;
}

// ---- the F10 tab
bool pc_amiibo_tab_available() { return !folders().empty(); }

void pc_amiibo_tab() {
    ImGui::TextWrapped("%s", pc_tr("Your own amiibo dumps (.bin, 540 bytes, or Flipper .nfc) in the amiibo folder. In the "
                                   "game, open its amiibo screen; when it waits for a figure, press Scan on one here."));
    if (ImGui::Button(pc_tr("Open amiibo folder"))) open_folder();
    const std::vector<std::string> dirs = folders();
    if (!dirs.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dirs[0].c_str());
    }
    int dev;
    std::string scanned;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        dev = g_dev;
        scanned = g_scanned;
        const long long now = now_ms();
        if (dev == DevInitialized && g_hold.waiting(now)) {
            scanned = g_pending;   // (goes on at the game's next start)
            dev = DevTagFound;
        } else if (dev == DevInitialized && g_hold.held(now)) {
            dev = DevSearching;   // (the game stopped its search a moment ago and starts it again)
        }
    }
    ImGui::Spacing();
    if (dev == DevSearching) ImGui::TextUnformatted(pc_tr("The game is waiting for an amiibo."));
    else if (!scanned.empty() && (dev == DevTagFound || dev == DevTagMounted))
        ImGui::Text(pc_tr("On the reader: %s"), scanned.c_str());
    else ImGui::TextDisabled("%s", pc_tr("Open the game's amiibo screen first: Scan works while it waits for a figure."));
    ImGui::Spacing();
    const std::vector<Dump> &list = dumps();
    if (list.empty()) {
        ImGui::TextWrapped("%s", pc_tr("No dumps yet: put .bin or .nfc files in the amiibo folder."));
        return;
    }
    for (int i = 0; i < (int)list.size(); i++) {
        const Dump &d = list[i];
        ImGui::PushID(i);
        ImGui::BeginDisabled(d.problem || dev != DevSearching);
        if (ImGui::Button(pc_tr("Scan"))) scan(d);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(d.name.c_str());
        if (d.problem) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", problem_text(d.problem));
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}
