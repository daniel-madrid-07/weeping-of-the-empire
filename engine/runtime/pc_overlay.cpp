// pc_overlay.cpp: nxrecomp's PC overlay on the engine, UI side. Threads: window messages arrive on the UI thread
// (shim_win.c -> pc_wndproc) and are queued; pad buttons arrive on the HID path (hle_hid.cpp -> pc_overlay_pad) as
// one atomic word; the ImGui frame is built on the present thread (pc_overlay_vk.c -> pc_overlay_build), which is the
// only thread that touches the ImGui context. Textures: ImGuiBackendFlags_RendererHasTextures. Every create or update
// request becomes a whole-image upload (the font atlas is the only texture and changes rarely); pc_overlay_vk.c
// carries the requests out before the draws.
#include "pc_overlay.h"
#include "pc_bind_ui.h"
#include "pc_display_caps.h"
#include "pc_settings.h"
#include "pc_restart.h"
#include "pc_text.h"
#include "../third_party/imgui/imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <float.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

std::atomic<bool> g_open{false}, g_perf{false};
std::atomic<bool> g_open_keys{false};   // pc_overlay_open_keys: select the Keys tab at the next frame
std::atomic<bool> g_welcome{false};   // the first-run screen is up (counts as open: no game input)
char g_gpu_name[256];
std::atomic<uint32_t> g_gpu_vram{0};   // MB, device-local (pc_overlay_note_gpu)
std::atomic<uint64_t> g_pad{0};
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<uint32_t> g_vram_used{0}, g_vram_budget{0};   // MB (pc_overlay_vk.c)

struct Ev { uint32_t msg; uintptr_t wp; intptr_t lp; uint8_t mods; };   // mods: 1 ctrl, 2 shift, 4 alt, 8 super
std::mutex g_q_mu;
std::vector<Ev> g_q;   // window messages for ImGui, drained by the present thread

// The F10 footer's "This PC" lines. The CPU, cores and RAM once; the display from the window's monitor,
// pc_display_caps.cpp's cached read (never on this thread: a new monitor, a display change or 10 s read it again on
// a thread of its own).
void hardware_lines(char *cpu, size_t cn, char *gpu, size_t gn, char *disp, size_t dn) {
    static char once[256];
    if (!once[0]) {
        int r[4];
        char brand[49] = {};
        __cpuid(r, 0x80000000);
        if ((unsigned)r[0] >= 0x80000004u)
            for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
        for (int i = 47; i >= 0 && (brand[i] == ' ' || !brand[i]); i--) brand[i] = 0;   // (padded with spaces)
        const char *b = brand;
        while (*b == ' ') b++;
        int cores = 0;
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> buf(len);
        if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf.data(), &len))
            for (DWORD at = 0; at < len; at += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf.data() + at))->Size) cores++;
        const DWORD threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        ULONGLONG kb = 0;
        MEMORYSTATUSEX ms = {sizeof ms};
        const double gb = GetPhysicallyInstalledSystemMemory(&kb) && kb ? kb / (1024.0 * 1024.0)
                          : GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0) : 0;
        snprintf(once, sizeof once, pc_tr("Processor: %s, %d cores, %lu threads. Memory: %.0f GB."), *b ? b : pc_tr("unknown"), cores,
                 threads, gb);
    }
    snprintf(cpu, cn, "%s", once);
    snprintf(gpu, gn, pc_tr("Graphics card: %s, %.0f GB."), g_gpu_name[0] ? g_gpu_name : pc_tr("unknown"), g_gpu_vram.load() / 1024.0f);
    disp[0] = 0;
    pcd_caps c;
    char t[352];
    if (HWND hw = g_hwnd.load(); hw && pcd_caps_cached(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &c)) {
        pcd_caps_text(c, t, sizeof t);
        snprintf(disp, dn, pc_tr("Display: %s"), t);
    }
}

// Restart the game. The window closes as with its X (shim_win.c saves a pending settings change, device.c's exit path
// waits for the GPU and saves the pipeline cache: progress since the last save is lost, as at any close), and that
// path's last step starts the game again: pc_restart.cpp (under the launcher, the launcher does it; the settings file's
// values replace the environment at the new start: pc_settings.cpp). Windows only.
std::atomic<bool> g_restart{false};
#ifdef _WIN32
extern "C" void (*volatile g_vk_exit_hook)(void);
void relaunch_at_exit() {
    if (g_restart.load()) pc_restart_process(nullptr);
}
void restart_now() {
    HWND hw = g_hwnd.load();
    if (!hw || g_restart.exchange(true)) return;
    g_vk_exit_hook = relaunch_at_exit;
    printf("[pc-overlay] restart: closing the game to start it again\n");
    PostMessageW(hw, WM_CLOSE, 0, 0);
}
#endif

void set_open(bool open) {
    if (g_open.exchange(open) != open) printf("[pc-overlay] settings %s\n", open ? "opened" : "closed");
}

uint8_t mods_now() {   // UI thread: GetKeyState follows this thread's message queue
    return (GetKeyState(VK_CONTROL) < 0 ? 1 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 2 : 0) | (GetKeyState(VK_MENU) < 0 ? 4 : 0) |
           ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? 8 : 0);
}

ImGuiKey vk_key(uintptr_t vk, intptr_t lp) {
    const bool ext = (lp >> 24) & 1;
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ext ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_SHIFT: return ((lp >> 16) & 0xff) == 0x36 ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
    case VK_CONTROL: return ext ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
    case VK_MENU: return ext ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

// ---- frame timing (every present while the overlay is enabled, so F9 shows history at once)
constexpr int kHist = 512;
float g_ms[kHist];
int g_ms_n, g_ms_at;
LARGE_INTEGER g_qpf, g_last;

void note_frame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_last.QuadPart) {
        g_ms[g_ms_at] = (float)((now.QuadPart - g_last.QuadPart) * 1000.0 / g_qpf.QuadPart);
        g_ms_at = (g_ms_at + 1) % kHist;
        if (g_ms_n < kHist) g_ms_n++;
    }
    g_last = now;
}
float recent_ms(int i) { return g_ms[(g_ms_at - 1 - i + kHist * 2) % kHist]; }   // i = 0: the last frame

// ---- ImGui state (present thread)
bool g_init;
float g_scale = 0;
ImGuiStyle g_base_style;
int g_tex_slot_used[PC_TEX_SLOTS];

std::vector<pc_vtx> g_vtx;
std::vector<uint16_t> g_idx;
std::vector<pc_cmd> g_cmd;
std::vector<pc_tex_req> g_tex;
pc_draw g_draw;

void init_imgui() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // window positions are not saved (settings go to settings.ini, O2)
    io.LogFilename = nullptr;
    io.BackendPlatformName = "nxrecomp-engine-win32";
    io.BackendRendererName = "nxrecomp-engine-vk";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavSwapGamepadButtons = true;   // the game's own convention: A (right face) confirms, B (bottom) goes back
    const char *fonts[] = {"C:\\Windows\\Fonts\\bahnschrift.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"};
    bool font = false;
    for (const char *f : fonts)
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(f, 18.0f)) { font = true; break; }
    if (!font) io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    g_base_style = st;
    g_init = true;
}

// 1080p is the reference size; the whole style follows the window height
void apply_scale(uint32_t h) {
    float k = h ? h / 1080.0f : 1.0f;
    if (k < 0.75f) k = 0.75f;
    if (k > 3.0f) k = 3.0f;
    if (k == g_scale) return;
    g_scale = k;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(k);
    st.FontScaleMain = k;
}

void drain_input(uint32_t w, uint32_t h) {
    std::vector<Ev> q;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        q.swap(g_q);
    }
    ImGuiIO& io = ImGui::GetIO();
    float sx = 1, sy = 1;   // window client pixels -> window image pixels (the same unless a resize is in flight)
    if (HWND hw = g_hwnd.load()) {
        RECT rc;
        if (GetClientRect(hw, &rc) && rc.right > 0 && rc.bottom > 0) { sx = (float)w / rc.right; sy = (float)h / rc.bottom; }
    }
    for (const Ev& e : q) {
        io.AddKeyEvent(ImGuiMod_Ctrl, e.mods & 1);
        io.AddKeyEvent(ImGuiMod_Shift, e.mods & 2);
        io.AddKeyEvent(ImGuiMod_Alt, e.mods & 4);
        io.AddKeyEvent(ImGuiMod_Super, e.mods & 8);
        switch (e.msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            ImGuiKey k = vk_key(e.wp, e.lp);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN);
            break;
        }
        case WM_CHAR:
            if (e.wp > 0 && e.wp < 0x10000) io.AddInputCharacterUTF16((unsigned short)e.wp);
            break;
        case WM_MOUSEMOVE:
            io.AddMousePosEvent((short)LOWORD(e.lp) * sx, (short)HIWORD(e.lp) * sy);
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: io.AddMouseButtonEvent(2, true); break;
        case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); break;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(e.wp) / WHEEL_DELTA); break;
        case WM_MOUSEHWHEEL: io.AddMouseWheelEvent(-(float)GET_WHEEL_DELTA_WPARAM(e.wp) / WHEEL_DELTA, 0); break;
        case WM_KILLFOCUS: io.AddFocusEvent(false); break;
        case WM_SETFOCUS: io.AddFocusEvent(true); break;
        }
    }
}

// nn::hid NpadButton bits -> ImGui gamepad keys. Positional, as the pad is laid out: B is the bottom face button,
// A the right one (ConfigNavSwapGamepadButtons makes A activate).
const char *g_capture;   // the key of the row waiting for a key (keys_widget, pad_widget)
bool g_capture_pad;      // g_capture is a pad row (pad_widget)
bool g_pad_armed;        // every pad button was up since the capture began
double g_capture_t;      // ImGui::GetTime() when it began
void feed_pad() {
    static const struct { int bit; ImGuiKey key; } map[] = {
        {0, ImGuiKey_GamepadFaceRight}, {1, ImGuiKey_GamepadFaceDown}, {2, ImGuiKey_GamepadFaceUp}, {3, ImGuiKey_GamepadFaceLeft},
        {6, ImGuiKey_GamepadL1}, {7, ImGuiKey_GamepadR1}, {8, ImGuiKey_GamepadL2}, {9, ImGuiKey_GamepadR2},
        {10, ImGuiKey_GamepadStart}, {11, ImGuiKey_GamepadBack},
        {12, ImGuiKey_GamepadDpadLeft}, {13, ImGuiKey_GamepadDpadUp}, {14, ImGuiKey_GamepadDpadRight}, {15, ImGuiKey_GamepadDpadDown},
        {16, ImGuiKey_GamepadLStickLeft}, {17, ImGuiKey_GamepadLStickUp}, {18, ImGuiKey_GamepadLStickRight}, {19, ImGuiKey_GamepadLStickDown},
    };
    if (g_capture_pad && (!g_capture || ImGui::GetTime() - g_capture_t > 6.0)) { g_capture = nullptr; g_capture_pad = false; }
    uint64_t b = g_capture_pad ? 0 : g_pad.load(std::memory_order_relaxed);   // (a pad capture: its press is not navigation)
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& m : map) io.AddKeyEvent(m.key, (b >> m.bit) & 1);
}

void perf_window() {
    if (!g_perf.load()) return;
    const int n = g_ms_n;
    float avg = 0, worst_1pct = 0;
    int na = n < 120 ? n : 120;
    for (int i = 0; i < na; i++) avg += recent_ms(i);
    avg = na ? avg / na : 0;
    if (n >= 100) {   // 1% low: the mean of the slowest 1% of the last n frames
        std::vector<float> s(n);
        for (int i = 0; i < n; i++) s[i] = recent_ms(i);
        int k = n / 100;
        std::nth_element(s.begin(), s.begin() + k, s.end(), [](float a, float b) { return a > b; });
        for (int i = 0; i < k; i++) worst_1pct += s[i];
        worst_1pct /= k;
    }
    static float graph[240];
    int ng = n < 240 ? n : 240;
    for (int i = 0; i < ng; i++) graph[ng - 1 - i] = recent_ms(i);
    const float pad = 10.0f * g_scale;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##perf", nullptr, fl)) {
        ImGui::Text("%.1f fps  %.2f ms", avg > 0 ? 1000.0f / avg : 0.0f, avg);
        if (worst_1pct > 0) ImGui::Text(pc_tr("1%% low %.1f fps (%.1f ms)"), 1000.0f / worst_1pct, worst_1pct);
        else ImGui::TextDisabled("%s", pc_tr("1%% low: collecting"));
        if (ng > 1) ImGui::PlotLines("##ft", graph, ng, 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f * g_scale, 48.0f * g_scale));
        const uint32_t vu = g_vram_used.load(std::memory_order_relaxed), vb = g_vram_budget.load(std::memory_order_relaxed);
        if (vb) ImGui::Text("VRAM %.1f / %.1f GB", vu / 1024.0f, vb / 1024.0f);
        ImGui::TextDisabled("%s", pc_tr("frame time 0-50 ms  (F9)"));
    }
    ImGui::End();
}

// ImGui key -> hle_hid.cpp vk_from_name()'s name (NULL: not bindable by name)
const char *bind_name(ImGuiKey k) {
    static char b[8];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) { b[0] = (char)('A' + (k - ImGuiKey_A)); b[1] = 0; return b; }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) { b[0] = (char)('0' + (k - ImGuiKey_0)); b[1] = 0; return b; }
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) { snprintf(b, sizeof b, "F%d", 1 + (k - ImGuiKey_F1)); return b; }
    switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: case ImGuiKey_KeypadEnter: return "Enter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_LeftShift: return "LShift";
    case ImGuiKey_RightShift: return "RShift";
    case ImGuiKey_LeftCtrl: return "LCtrl";
    case ImGuiKey_RightCtrl: return "RCtrl";
    case ImGuiKey_LeftAlt: return "LAlt";
    case ImGuiKey_RightAlt: return "RAlt";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_DownArrow: return "Down";
    default: return nullptr;
    }
}

// A key-list row (PC_SET_KEYS): "Add key" waits for the next key or mouse button (Esc cancels), "Clear" empties it
bool g_esc_eaten;        // this frame's Esc cancelled a capture: it does not also close the window
int g_capture_wait;      // frames before input counts (the click on "Add key" itself)
void keys_widget(const pc_setting *d, const char *value) {
    const bool mine = g_capture && !strcmp(g_capture, d->key);
    if (mine) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("Press a key or mouse button (Esc: cancel)"));
        if (g_capture_wait > 0) { g_capture_wait--; return; }
        const char *got = nullptr;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_capture = nullptr; g_esc_eaten = true; return; }
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !got; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) got = bind_name((ImGuiKey)k);
        static const char *const mouse[] = {"Mouse Left", "Mouse Right", "Mouse Middle"};
        for (int m = 0; m < 3 && !got; m++) if (ImGui::IsMouseClicked(m)) got = mouse[m];
        if (!got) return;
        std::string v = value;
        int n = v.empty() ? 0 : 1 + (int)std::count(v.begin(), v.end(), ',');
        if (n < 4 && ("," + v + ",").find("," + std::string(got) + ",") == std::string::npos) v += (v.empty() ? "" : ",") + std::string(got);
        pc_settings_set(d->key, v.c_str());
        g_capture = nullptr;
        return;
    }
    if (*value) ImGui::TextUnformatted(value);
    else ImGui::TextDisabled("%s", pc_tr("(the layout's keys)"));
    ImGui::SameLine();
    if (ImGui::SmallButton(pc_tr("Add key"))) { g_capture = d->key; g_capture_pad = false; g_capture_wait = 2; }
    if (*value) {
        ImGui::SameLine();
        if (ImGui::SmallButton(pc_tr("Clear"))) pc_settings_set(d->key, "");
    }
}

// A pad-button row (PC_SET_PAD, hle_hid.cpp pad_remap): "Add button" waits for every pad button to be released, then
// takes the next one pressed (Esc or 6 s cancels; meanwhile feed_pad gives ImGui no pad, so that press does not also
// navigate), "None" unbinds the action, "Clear" gives it back its own button. Buttons by position, by TOTK_PRESS's names.
const char *pad_label(const std::string &t) {
    static const char *const n[][2] = {{"A", PC_T("A (right)")}, {"B", PC_T("B (bottom)")}, {"X", PC_T("X (top)")},
        {"Y", PC_T("Y (left)")}, {"LS", PC_T("L stick")}, {"RS", PC_T("R stick")}, {"L", "L"}, {"R", "R"}, {"ZL", "ZL"},
        {"ZR", "ZR"}, {"PLUS", "+"}, {"MINUS", "-"}, {"LEFT", PC_T("D-pad left")}, {"UP", PC_T("D-pad up")},
        {"RIGHT", PC_T("D-pad right")}, {"DOWN", PC_T("D-pad down")}, {"none", PC_T("none")}};
    for (const auto &e : n) if (t == e[0]) return pc_tr(e[1]);
    return t.c_str();
}
// The button an action has while unbound (controls.pad.<action> -> TOTK_PRESS's name: hle_hid.cpp pad_remap's default)
const char *pad_own(const char *key) {
    static const char *const n[][2] = {{"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"lstick", "LS"}, {"rstick", "RS"},
        {"l", "L"}, {"r", "R"}, {"zl", "ZL"}, {"zr", "ZR"}, {"plus", "PLUS"}, {"minus", "MINUS"}, {"dpad_left", "LEFT"},
        {"dpad_up", "UP"}, {"dpad_right", "RIGHT"}, {"dpad_down", "DOWN"}};
    const char *act = strrchr(key, '.');
    for (const auto &e : n) if (act && !strcmp(act + 1, e[0])) return e[1];
    return "";
}
void pad_widget(const pc_setting *d, const char *value) {
    static const char *const names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    const bool mine = g_capture && g_capture_pad && !strcmp(g_capture, d->key);
    if (mine) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("Press a pad button (Esc: cancel)"));
        if (g_capture_wait > 0) { g_capture_wait--; return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_capture = nullptr; g_capture_pad = false; g_esc_eaten = true; return; }
        const uint64_t b = g_pad.load(std::memory_order_relaxed) & 0xFFFF;
        if (!g_pad_armed) { g_pad_armed = b == 0; return; }
        if (!b) return;
        int bit = 0;
        while (!(b >> bit & 1)) bit++;
        // an action on its own button keeps it: "Add button" adds a second one (Clear gives the default back, None unbinds)
        std::string v = !strcmp(value, "none") ? "" : *value ? value : pad_own(d->key);
        int n = v.empty() ? 0 : 1 + (int)std::count(v.begin(), v.end(), ',');
        if (n < 4 && ("," + v + ",").find("," + std::string(names[bit]) + ",") == std::string::npos) v += (v.empty() ? "" : ",") + std::string(names[bit]);
        pc_settings_set(d->key, v.c_str());
        g_capture = nullptr;
        g_capture_pad = false;
        return;
    }
    if (!*value) ImGui::TextDisabled("%s", pc_tr("(its own button)"));
    else {
        std::string shown, v = value;
        for (size_t p = 0; p <= v.size();) {
            size_t e = v.find(',', p);
            if (e == std::string::npos) e = v.size();
            shown += (shown.empty() ? "" : ", ") + std::string(pad_label(v.substr(p, e - p)));
            p = e + 1;
        }
        ImGui::TextUnformatted(shown.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(pc_tr("Add button"))) {
        g_capture = d->key; g_capture_pad = true; g_pad_armed = false; g_capture_wait = 2; g_capture_t = ImGui::GetTime();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(pc_tr("None"))) pc_settings_set(d->key, "none");
    if (*value) {
        ImGui::SameLine();
        if (ImGui::SmallButton(pc_tr("Clear"))) pc_settings_set(d->key, "");
    }
}

// One registry row (pc_settings.h): label, then a combo (choice rows) or a slider, then its state
void setting_row(const pc_setting_view &s) {
    const pc_setting *d = s.def;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(pc_tr(d->label));
    if (d->help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pc_tr(d->help));
    ImGui::TableNextColumn();
    ImGui::PushID(d->key);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (d->flags & PC_SET_KEYS) {
        keys_widget(d, s.value);
    } else if (d->flags & PC_SET_PAD) {
        pad_widget(d, s.value);
    } else if (d->values) {
        int cur = -1, n = 0;
        for (; d->values[n]; n++) if (!strcmp(d->values[n], s.value)) cur = n;
        if (ImGui::BeginCombo("##v", cur >= 0 ? pc_tr(d->names[cur]) : s.value)) {
            for (int i = 0; i < n; i++) {
                if (ImGui::Selectable(pc_tr(d->names[i]), i == cur)) pc_settings_set(d->key, d->values[i]);
                if (i == cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        const bool pct = d->min >= 0 && d->max <= 1;   // 0..1 rows read as percentages (volume)
        float v = (float)atof(s.value), shown = pct ? v * 100 : v;
        const char *fmt = d->format ? d->format(shown) : pct ? "%.0f %%" : "%.2f";
        if (ImGui::SliderFloat("##v", &shown, pct ? d->min * 100 : d->min, pct ? d->max * 100 : d->max, fmt)) {
            float x = pct ? shown / 100 : shown;
            if (d->step > 0) x = d->min + d->step * (float)(int)((x - d->min) / d->step + 0.5f);
            if (x < d->min) x = d->min;
            if (x > d->max) x = d->max;
            char b[32];
            snprintf(b, sizeof b, "%g", x);
            pc_settings_set(d->key, b);
        }
    }
    if (d->help && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", pc_tr(d->help));
    ImGui::TableNextColumn();
    if (s.overridden) {   // (short: the column fits "at next start"; "this run (NX_SET_)" was cut off at 1080p)
        ImGui::TextDisabled("%s", pc_tr("this run"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pc_tr("Set by NX_SET_ for this run only; not saved."));
    } else if (s.restart_pending) ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("at next start"));
    else if (d->flags & PC_SET_RESTART) ImGui::TextDisabled("%s", pc_tr("restart"));
    ImGui::PopID();
}

void settings_page(const char *page) {
    const int n = pc_settings_count();
    std::vector<const char *> sections;   // in the order they first appear (rows of several tables share a section)
    for (int i = 0; i < n; i++) {
        pc_setting_view s = pc_settings_at(i);
        if (strcmp(s.def->page, page)) continue;
        bool seen = false;
        for (const char *q : sections) seen |= !strcmp(q, s.def->section);
        if (!seen) sections.push_back(s.def->section);
    }
    for (const char *section : sections) {
        ImGui::SeparatorText(pc_tr(section));
        if (!ImGui::BeginTable(section, 3, ImGuiTableFlags_SizingStretchProp)) continue;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.43f);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        for (int i = 0; i < n; i++) {
            pc_setting_view s = pc_settings_at(i);
            if (!strcmp(s.def->page, page) && !strcmp(s.def->section, section)) setting_row(s);
        }
        ImGui::EndTable();
    }
    if (sections.empty()) { ImGui::TextDisabled("%s", pc_tr("No settings on this page yet.")); return; }
    ImGui::Spacing();
    if (ImGui::Button(pc_tr("Reset this page to defaults")))
        for (int i = 0; i < n; i++) {
            pc_setting_view s = pc_settings_at(i);
            if (!strcmp(s.def->page, page) && s.def->def) pc_settings_set(s.def->key, s.def->def);
        }
}

void settings_window(uint32_t w, uint32_t h) {
    if (!g_open.load()) return;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820.0f * g_scale, 560.0f * g_scale), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin((std::string(pc_tr("Settings  (F10)")) + "###settings").c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("##pages")) {
            std::vector<const char *> pages = {"Graphics", "Controls", "Audio", "Game"};   // then any other page a row names
            for (int i = 0, n = pc_settings_count(); i < n; i++) {
                const char *p = pc_settings_at(i).def->page;
                bool seen = false;
                for (const char *q : pages) seen |= !strcmp(p, q);
                if (!seen && *p) pages.push_back(p);   // (no page: a hidden row)
            }
            // The pad's L / R switch pages; not while an item is active, where L / R are ImGui's slower / faster value
            // steps
            static int want = -1;
            if (g_open_keys.exchange(false)) want = (int)pages.size();   // pc_overlay_open_keys: the Keys tab
            int cur = 0;
            for (int i = 0; i < (int)pages.size(); i++)
                if (ImGui::BeginTabItem((std::string(pc_tr(pages[i])) + "###" + pages[i]).c_str(), nullptr,
                                        i == want ? ImGuiTabItemFlags_SetSelected : 0)) {
                    cur = i;
                    settings_page(pages[i]);
                    ImGui::EndTabItem();
                }
            // the rebinding pages: the last tab, once the rebinding API answers
            const bool with_keys = pc_bind_keys_available();
            const int keys = (int)pages.size(), tabs = keys + (with_keys ? 1 : 0);
            if (with_keys && ImGui::BeginTabItem((std::string(pc_tr("Keys")) + "###Keys").c_str(), nullptr, want == keys ? ImGuiTabItemFlags_SetSelected : 0)) {
                cur = keys;
                pc_bind_keys_tab();
                ImGui::EndTabItem();
            } else {
                pc_bind_keys_closed();
            }
            ImGui::EndTabBar();
            want = -1;
            const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
            if (step && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
                want = (cur + step + tabs) % tabs;
        }
        ImGui::Separator();
        bool pending = false;
        for (int i = 0, n = pc_settings_count(); i < n; i++) pending |= pc_settings_at(i).restart_pending;
        if (pending) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", pc_tr("Some changes apply when the game starts again."));
#ifdef _WIN32
            ImGui::SameLine();
            if (ImGui::SmallButton(pc_tr("Restart the game now"))) ImGui::OpenPopup("##restart");
            if (ImGui::BeginPopup("##restart")) {
                ImGui::TextUnformatted(pc_tr("The game closes and starts again with the new settings. Progress since your "
                                             "last save is lost."));
                if (ImGui::Button(pc_tr("Restart"))) { restart_now(); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::Button(pc_tr("Cancel"))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
#endif
        }
        char cpu[256], gpu[320], disp[384];
        hardware_lines(cpu, sizeof cpu, gpu, sizeof gpu, disp, sizeof disp);
        ImGui::TextDisabled("%s", cpu);
        ImGui::TextDisabled("%s", gpu);
        if (disp[0]) ImGui::TextDisabled("%s", disp);
        if (const char *f = pc_settings_file()) ImGui::TextDisabled(pc_tr("Saved to %s"), f);
        else ImGui::TextDisabled("%s", pc_tr("Not saved (TOTK_SETTINGS=0)"));
        ImGui::TextDisabled("%s", pc_tr("F10, Esc, B or L3 + R3 closes. A: select. L / R: pages. F9: perf overlay."));
    }
    ImGui::End();
    // Esc closes when no widget holds the keyboard; the pad's B (ImGui's cancel, swapped: feed_pad) when nothing is active
    // and no popup is up, nor was in the frame before: the B that closes a combo or ends a slider's tweak is that press.
    const bool esc_eaten = g_esc_eaten || g_capture;
    g_esc_eaten = false;
    static bool popup_was, active_was;   // (the same for an item that press deactivated: a slider being tweaked)
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId), active = ImGui::IsAnyItemActive();
    if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !active && !esc_eaten) ||
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) && !active && !active_was && !popup && !popup_was))
        set_open(false);
    popup_was = popup;
    active_was = active;
}

// The preset to suggest: full quality, Ultra, on any card from 8 GB at any display size. Under 8 GB (or not known) High:
// still the native resolution and every knob at or above the game's own. Never Medium / Low: those draw less than the game,
// the player's choice only.
int recommend_preset(uint32_t vram_mb, int disp_h) {
    (void)disp_h;
    return vram_mb >= 8 * 1024 - 512 ? 3 : 2;   // (an "8 GB" card reports a little under 8192 MiB device-local)
}

// First-run screen: no settings file yet, or the package's (pc_settings_first_run), so offer a preset for this PC (pc.welcome_done stops it coming back)
void welcome_window(uint32_t w, uint32_t h) {
    if (!g_welcome.load()) return;
    char cpu[256], gpu[320], disp[384];   // (F10's lines: the same detection, the same words)
    hardware_lines(cpu, sizeof cpu, gpu, sizeof gpu, disp, sizeof disp);
    const uint32_t vram = g_gpu_vram.load();
    const int rec = recommend_preset(vram, 0);
    static const char *const names[] = {PC_T("Low"), PC_T("Medium"), PC_T("High"), "Ultra"}, *const keys[] = {"low", "medium", "high", "ultra"};
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620.0f * g_scale, 0), ImGuiCond_Always);
    int chose = -2;   // -1: keep the current settings
    if (ImGui::Begin((std::string(pc_tr("Welcome")) + "###welcome").c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", pc_tr("Pick graphics settings for this PC. Everything can be changed later with F10 (or L3 + R3)."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", gpu);
        if (disp[0]) ImGui::TextWrapped("%s", disp);
        // what the first start set for this display (pc_settings.cpp boot)
        const char *fps = pc_settings_get("display.fps_cap"), *vrr = pc_settings_get("display.vrr");
        pcd_caps wc;
        const HWND whw = g_hwnd.load();
        const bool c_hdr_on = whw && pcd_caps_cached(MonitorFromWindow(whw, MONITOR_DEFAULTTONEAREST), &wc) && wc.hdr_on;
        ImGui::TextWrapped(pc_tr("Set for this display: frame rate %s%s.%s"),
                           fps && !strcmp(fps, "unlocked") ? pc_tr("Unlocked (up to the refresh)") : fps ? fps : "?",
                           vrr && !strcmp(vrr, "on") ? pc_tr(", VRR on") : "",
                           c_hdr_on ? pc_tr(" HDR output (off) is in F10 > Graphics > HDR.") : "");
        ImGui::Spacing();
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            const bool r = i == rec;
            if (r) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            char label[48];
            snprintf(label, sizeof label, r ? pc_tr("%s (recommended)") : "%s", pc_tr(names[i]));
            if (ImGui::Button(label)) chose = i;
            if (r) {
                ImGui::PopStyleColor();
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
            }
        }
        ImGui::Spacing();
        if (ImGui::Button(pc_tr("Keep the current settings"))) chose = -1;
        ImGui::TextDisabled("%s", pc_tr("Some choices apply when the game starts again. Esc keeps the current settings."));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !g_open.load()) chose = -1;
    }
    ImGui::End();
    if (chose == -2) return;
    if (chose >= 0) pc_settings_set("graphics.preset", keys[chose]);
    pc_settings_set("pc.welcome_done", "1");
    pc_settings_tick(true);
    g_welcome = false;
    printf("[pc-overlay] first-run screen: %s\n", chose >= 0 ? keys[chose] : "kept the current settings");
}

void handle_textures(ImDrawData *dd) {
    g_tex.clear();
    if (!dd->Textures) return;
    for (ImTextureData *t : *dd->Textures) {
        if (t->Status == ImTextureStatus_OK || t->Status == ImTextureStatus_Destroyed) continue;
        if (t->Status == ImTextureStatus_WantDestroy) {
            if (t->UnusedFrames < 4) continue;   // still referenced by a frame in flight; pc_overlay_vk.c also defers
            int slot = (int)t->TexID - 1;
            if (slot >= 0 && slot < PC_TEX_SLOTS) {
                g_tex.push_back({(uint32_t)slot, PC_TEX_DESTROY, 0, 0, nullptr});
                g_tex_slot_used[slot] = 0;
            }
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
            continue;
        }
        int slot = (int)t->TexID - 1;   // WantCreate or WantUpdates: upload the whole image
        if (t->Status == ImTextureStatus_WantCreate || slot < 0 || slot >= PC_TEX_SLOTS) {
            slot = -1;
            for (int i = 0; i < PC_TEX_SLOTS; i++) if (!g_tex_slot_used[i]) { slot = i; break; }
            if (slot < 0) continue;   // no slot: ImGui keeps asking; never with only the font atlas
            g_tex_slot_used[slot] = 1;
        }
        if (t->Format != ImTextureFormat_RGBA32) continue;   // TexDesiredFormat stays RGBA32
        g_tex.push_back({(uint32_t)slot, PC_TEX_UPLOAD, (uint32_t)t->Width, (uint32_t)t->Height, t->GetPixels()});
        t->SetTexID((ImTextureID)(slot + 1));
        t->SetStatus(ImTextureStatus_OK);
    }
}

void flatten(ImDrawData *dd, uint32_t w, uint32_t h) {
    g_vtx.clear(); g_idx.clear(); g_cmd.clear();
    const ImVec2 off = dd->DisplayPos, fs = dd->FramebufferScale;
    for (const ImDrawList *l : dd->CmdLists) {
        const uint32_t v0 = (uint32_t)g_vtx.size(), i0 = (uint32_t)g_idx.size();
        static_assert(sizeof(ImDrawVert) == sizeof(pc_vtx), "pc_vtx must match ImDrawVert");
        g_vtx.insert(g_vtx.end(), (const pc_vtx *)l->VtxBuffer.Data, (const pc_vtx *)l->VtxBuffer.Data + l->VtxBuffer.Size);
        g_idx.insert(g_idx.end(), l->IdxBuffer.Data, l->IdxBuffer.Data + l->IdxBuffer.Size);
        for (const ImDrawCmd &c : l->CmdBuffer) {
            if (c.UserCallback) continue;   // none are used
            float x0 = (c.ClipRect.x - off.x) * fs.x, y0 = (c.ClipRect.y - off.y) * fs.y;
            float x1 = (c.ClipRect.z - off.x) * fs.x, y1 = (c.ClipRect.w - off.y) * fs.y;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = (float)w;
            if (y1 > h) y1 = (float)h;
            if (x1 <= x0 || y1 <= y0 || !c.ElemCount) continue;
            int slot = (int)c.GetTexID() - 1;
            if (slot < 0 || slot >= PC_TEX_SLOTS) continue;
            g_cmd.push_back({{x0, y0, x1, y1}, (uint32_t)slot, i0 + c.IdxOffset, c.ElemCount, v0 + c.VtxOffset});
        }
    }
}

}  // namespace

extern "C" bool pc_overlay_enabled(void) {
    static int on = -1;
    if (on < 0) {
        // NXR_PC_OVERLAY=0: hle_osd.cpp's own menu and recomp_settings.ini instead
        const char *e = getenv("NXR_PC_OVERLAY");
        on = !(e && *e == '0');
        if (on) printf("[pc-overlay] on (NXR_PC_OVERLAY=0 turns it off): F10 or L3 + R3 settings, F9 perf overlay\n");
    }
    return on;
}

extern "C" bool pc_overlay_open(void) { return g_open.load() || g_welcome.load(); }
extern "C" void pc_overlay_open_keys(void) {
    g_open_keys = true;
    set_open(true);
}

extern "C" void *pc_overlay_hwnd(void) { return g_hwnd.load(std::memory_order_relaxed); }
extern "C" bool pc_overlay_perf(void) { return g_perf.load(); }
extern "C" void pc_overlay_note_gpu(const char *name, uint32_t vram_mb) {
    snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : "");
    g_gpu_vram.store(vram_mb);
}
extern "C" void pc_overlay_note_vram(uint32_t used_mb, uint32_t budget_mb) {
    g_vram_used.store(used_mb, std::memory_order_relaxed);
    g_vram_budget.store(budget_mb, std::memory_order_relaxed);
}
extern "C" void pc_overlay_set_perf(bool on) { g_perf.store(on); }
extern "C" void osd_toast(const char *text, int ms);   // hle_osd.cpp
// The game's Options screen changed a restart row (totk_options_menu.cpp set_choice): a notice over the game for 6 s
extern "C" void pc_overlay_restart_notice(void) {
    osd_toast(pc_tr("Applies when the game starts again: F10 > Restart the game now"), 6000);
}

extern "C" bool pc_wndproc(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
    if (!pc_overlay_enabled()) return false;
    g_hwnd.store((HWND)hwnd, std::memory_order_relaxed);
    if (msg == WM_DISPLAYCHANGE) pcd_caps_display_changed();   // (F10's Display line reads the monitor again)
    const bool open = g_open.load() || g_welcome.load();
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:   // (F10 is a system key: WM_SYSKEYDOWN, and it would open the window menu)
        if (wp == VK_F9 && GetKeyState(VK_SHIFT) < 0) return false;   // Shift+F9: hle_hid.cpp's frame capture
        if (wp == VK_F10 || wp == VK_F9) {
            if (!(lp & (1 << 30))) {   // first press, not auto-repeat
                if (wp == VK_F10) set_open(!open);
                else g_perf.store(!g_perf.load());
            }
            return true;
        }
        // F11 / Alt+Enter / F12 (screenshot) stay with shim_win.c, also while the settings or the Welcome window is open
        if (!open || wp == VK_F11 || wp == VK_F12 || (msg == WM_SYSKEYDOWN && wp == VK_RETURN)) return false;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (wp == VK_F10 || wp == VK_F9) return true;
        if (!open) return false;
        break;
    case WM_CHAR: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        if (!open) return false;
        break;
    case WM_SETCURSOR:   // shim_win.c hides the cursor in fullscreen; show it while the settings window is open
        if (!open || LOWORD(lp) != HTCLIENT) return false;
        SetCursor(LoadCursorA(nullptr, (LPCSTR)IDC_ARROW));
        return true;
    case WM_SETFOCUS: case WM_KILLFOCUS: {   // queued, not consumed
        std::lock_guard<std::mutex> lk(g_q_mu);
        g_q.push_back({msg, wp, lp, 0});
        return false;
    }
    default:
        return false;
    }
    std::lock_guard<std::mutex> lk(g_q_mu);
    if (g_q.size() < 4096) g_q.push_back({msg, wp, lp, mods_now()});
    return true;
}

extern "C" bool pc_overlay_pad(uint64_t buttons) {
    const uint64_t LS = 1u << 4, RS = 1u << 5;
    static uint64_t was;
    if ((buttons & (LS | RS)) == (LS | RS) && (was & (LS | RS)) != (LS | RS)) set_open(!g_open.load());
    was = buttons;
    g_pad.store(buttons, std::memory_order_relaxed);
    return g_open.load() || g_welcome.load();
}

extern "C" const pc_draw *pc_overlay_build(uint32_t w, uint32_t h) {
    note_frame();
    static bool was_visible, was_open;
    const bool open_now = g_open.load();
    if (was_open && !open_now) {   // closed: save what changed now, not 0.5 s later; a key capture of the Keys tab ends
        pc_settings_tick(true);
        pc_bind_keys_closed();
    }
    was_open = open_now;
    static bool welcome_checked;
    if (!welcome_checked) {   // first start with a settings file: offer a preset (NXR_PC_WELCOME=0: never)
        welcome_checked = true;
        // Not in scripted boots (TOTK_PRESS / TOTK_CTL): their presses pass the overlay to the game anyway, and the
        // window would sit in every harness screenshot (each boot has fresh userdata, so each is a first run)
        const char *done = pc_settings_get("pc.welcome_done"), *off = getenv("NXR_PC_WELCOME");
        const bool scripted = getenv("TOTK_PRESS") || getenv("TOTK_CTL");
        if (pc_settings_first_run() && done && !strcmp(done, "0") && !(off && *off == '0') && !scripted) g_welcome = true;
    }
    const bool visible = open_now || g_perf.load() || g_welcome.load();
    if (!visible) {
        if (was_visible) {   // drop what was queued while closing, so nothing replays at the next open
            std::lock_guard<std::mutex> lk(g_q_mu);
            g_q.clear();
        }
        was_visible = false;
        pc_settings_tick_save();   // (a binding changed from the game's Options rows: saved without the overlay)
        return nullptr;
    }
    if (!g_init) init_imgui();
    ImGuiIO& io = ImGui::GetIO();
    if (!was_visible) { io.ClearInputKeys(); io.ClearInputMouse(); }
    was_visible = true;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = g_ms_n ? (recent_ms(0) > 0 ? recent_ms(0) / 1000.0f : 1.0f / 60) : 1.0f / 60;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    io.MouseDrawCursor = false;
    apply_scale(h);
    drain_input(w, h);
    // (the perf overlay alone takes no pad input; the first-run screen does: i2ov3 left it up, A did nothing)
    if (g_open.load() || g_welcome.load()) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    feed_pad();
    pc_settings_tick(false);
    ImGui::NewFrame();
    welcome_window(w, h);
    settings_window(w, h);
    perf_window();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    handle_textures(dd);
    flatten(dd, w, h);
    g_draw = {g_vtx.data(), (uint32_t)g_vtx.size(), g_idx.data(), (uint32_t)g_idx.size(), g_cmd.data(), (uint32_t)g_cmd.size(),
              g_tex.data(), (uint32_t)g_tex.size()};
    return &g_draw;
}
