// HLE: nn::hid. One Pro Controller (FullKey) on Npad id 0 (No1). Layouts follow Eden's hid_core/hid_types.h
// (NpadFullKeyState etc. = 0x28 bytes; SixAxisSensorState 0x60; MouseState 0x28; KeyboardState 0x30).
// Input = host pads (XInput: Xbox; raw HID: DualSense, DualShock 4, Pro Controller, Joy-Cons) | keyboard (while the game window has focus) |
// TOTK_PRESS script; see host_input().
#include "hle.h"
#include "totk_version.h"
#include "pc_input.h"      // the PC scheme: context and routing table (pc_input.cpp)
#include "pc_overlay.h"
#include "pc_restart.h"     // skip_restart (Windows): through the launcher when it started us
#include "pc_settings.h"
#include "pc_device_switch.h"   // the device the prompts follow: what counts as a use, the anti-flicker guards
#include "pc_key_hold.h"        // the move keys' minimum hold (keyboard_read)
#include "totk_menu_cursor.h"
#include "totk_player_state.h"
#include "totk_ability.h"
#include "totk_pcvk.h"
#include <chrono>
#include <windows.h>
#ifndef _WIN32
#include <SDL3/SDL.h>      // Linux: pads, keyboard and motion come from SDL3 (Steam Input shows up as an SDL gamepad)
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <time.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include "linux/plat_posix.h"
#endif
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <cwchar>
#include <cwctype>
#include <vector>
#include <set>
#include <unordered_map>
#include <utility>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>

enum : u32 { StyleFullKey = 1u << 0, StyleHandheld = 1u << 1, StyleJoyDual = 1u << 2 };
static std::atomic<s64> g_sample{1};

struct NpadState {          // NpadFullKeyState / JoyDual / Handheld / JoyLeft / JoyRight share this layout
    s64 sampling_number; u64 buttons; s32 lx, ly, rx, ry; u32 attributes; u32 reserved;
};
static_assert(sizeof(NpadState) == 0x28);
static bool npad_connected(u32 id) { return id == 0; }

// Scripted input for unattended test runs (user 2026-10-14: after the autosave, boots stop at the title screen):
// TOTK_PRESS="A@600,A@700,PLUS@900" holds each button for 6 presented frames starting at that frame (present count);
// "A@300+30x40" repeats: 40 presses, 30 frames apart, from frame 300.
// Npad button bits (Eden hid_types.h NpadButton): A 0, B 1, X 2, Y 3, LS 4, RS 5, L 6, R 7, ZL 8, ZR 9, PLUS 10,
// MINUS 11, LEFT 12, UP 13, RIGHT 14, DOWN 15.
extern "C" uint64_t nvn_present_count(void);
// Stick pseudo-buttons LUP/LDOWN/LLEFT/LRIGHT/RUP/RDOWN/RLEFT/RRIGHT = full deflection (bits 32-39, never sent as
// buttons) and an optional hold length: "LUP@2100:150" pushes the left stick up for 150 frames (default hold 6).
enum { kScriptStick = 32 };
struct Press { u64 frame, bits, hold; bool logged; };
// Button names of TOTK_PRESS and TOTK_CTL
static const struct { const char* n; int bit; } kPressNames[] = {{"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LS", 4}, {"RS", 5},
    {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"LEFT", 12}, {"UP", 13}, {"RIGHT", 14}, {"DOWN", 15},
    {"LUP", 32}, {"LDOWN", 33}, {"LLEFT", 34}, {"LRIGHT", 35}, {"RUP", 36}, {"RDOWN", 37}, {"RLEFT", 38}, {"RRIGHT", 39}, {"DUMP", 40}};
static std::vector<Press> parse_presses() {
    const auto& names = kPressNames;
    std::vector<Press> v;
    const char* e = getenv("TOTK_PRESS");
    // TOTK_AUTO_NEWGAME=1 (set by the in-game skip restart): title menu Continue -> New Game, A, "Overwrite previous
    // save data?" Cancel -> Start, A
    if (!e && getenv("TOTK_AUTO_NEWGAME")) e = "DOWN@350,A@420,UP@480,A@540";
    for (const char* p = e; p && *p;) {
        char name[16] = {}; unsigned long long f = 0, hold = 6; int n = 0;
        if (sscanf(p, "%15[A-Z]@%llu%n", name, &f, &n) != 2) { printf("[hid] TOTK_PRESS: cannot parse at \"%s\"\n", p); break; }
        int bit = -1;
        for (auto& nm : names) if (!strcmp(nm.n, name)) bit = nm.bit;
        p += n;
        int k = 0;
        if (*p == ':' && sscanf(p, ":%llu%n", &hold, &k) == 1) p += k;
        // repeat form "A@300+30x40": from frame 300, every 30 frames, 40 presses (user: tap through the title as soon
        // as it shows instead of guessing its frame)
        unsigned long long every = 0, count = 1; int m = 0;
        if (*p == '+' && sscanf(p, "+%llux%llu%n", &every, &count, &m) == 2) p += m; else every = 0, count = 1;
        if (bit < 0) printf("[hid] TOTK_PRESS: unknown button %s\n", name);
        else for (unsigned long long i = 0; i < count; i++) v.push_back({f + i * every, 1ull << bit, hold, false});
        if (*p == ',') p++;
    }
    return v;
}
extern "C" volatile int g_debug_frame_request;   // nvn/vulkan/totk_hooks.c (device.c dumps the next frame)
// "DUMP@N" (bit 40, never sent): request a frame capture at tick N, as F9 does (with LARECOMP_DEBUG_FRAMES=n: n frames)
static u64 scripted_buttons() {
    static std::vector<Press> presses = parse_presses();
    if (presses.empty()) return 0;
    u64 f = nvn_present_count(), b = 0;
    // TOTK_PRESS_CLOCK=wall: frame numbers are 1/30 s ticks of wall time since the first presented frame, so a script
    // covers the same real time at any frame rate (speed A/B runs at 30 vs 60 fps)
    static const bool wall = getenv("TOTK_PRESS_CLOCK") && !strcmp(getenv("TOTK_PRESS_CLOCK"), "wall");
    if (wall) {
        static auto t0 = std::chrono::steady_clock::now();
        f = (u64)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 30.0);
    }
    for (auto& p : presses)
        if (f >= p.frame && f < p.frame + p.hold) {
            b |= p.bits & ~(1ull << 40);
            if (!p.logged && (p.bits >> 40 & 1)) g_debug_frame_request = 1;
            if (!p.logged) { p.logged = true; printf("[hid] scripted press %llx at frame %llu\n", (unsigned long long)p.bits, (unsigned long long)f); }
        }
    return b;
}
// ------------------------------------------------------------------ host input
// Pad: XInput (Xbox controllers; a DualSense / Pro Controller shows up through Steam Input or DS4Windows). Face buttons
// map by POSITION like Eden's SDL default (pad B = right = Switch A, pad A = bottom = Switch B, Y = top = X, X = left
// = Y), so on-screen prompts match where the button sits; TOTK_PAD_LABELS=1 maps by label instead (pad A -> Switch A).
// Keyboard (only while the game window is in front): WASD move (Ctrl = walk), arrows / IJKL camera, Space X (jump),
// Shift B (sprint / back), E or Enter A (action / confirm), F Y (attack), Q L (abilities), R R (throw / use),
// Z ZL (target / shield), C ZR (bow), V LS (crouch), G RS (look), Esc PLUS (menu), M MINUS (map), 1-4 D-pad L U R D,
// Backspace B.
enum : u64 { BtnA = 1u << 0, BtnB = 1u << 1, BtnX = 1u << 2, BtnY = 1u << 3, BtnLS = 1u << 4, BtnRS = 1u << 5,
             BtnL = 1u << 6, BtnR = 1u << 7, BtnZL = 1u << 8, BtnZR = 1u << 9, BtnPlus = 1u << 10, BtnMinus = 1u << 11,
             BtnLeft = 1u << 12, BtnUp = 1u << 13, BtnRight = 1u << 14, BtnDown = 1u << 15 };
struct HostInput { u64 buttons; s32 lx, ly, rx, ry; };
static std::atomic<int> g_padmap_gen{1};   // the pad bindings changed (overlay rows): pad_remap rebuilds

#ifdef _WIN32
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };     // XINPUT_GAMEPAD
struct XState { DWORD packet; XPad pad; };                          // XINPUT_STATE
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XState*);
typedef struct { WORD l, r; } XVib;                                 // XINPUT_VIBRATION
typedef DWORD(WINAPI* XInputSetState_t)(DWORD, XVib*);
static XInputGetState_t xinput_load() {
    for (const char* n : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(n))
            if (auto f = (XInputGetState_t)GetProcAddress(m, "XInputGetState")) { printf("[hid] host pads via %s\n", n); return f; }
    printf("[hid] XInput not found: keyboard only\n");
    return nullptr;
}
#endif

// TOTK_STICK_DEADZONE_L / _R (controls.deadzone_left / _right, live): each stick's radial dead zone in percent of its
// travel, in place of the pad's own (XInput 24 / 26.5 %, DualSense / DS4 15 %, Pro Controller / Joy-Con 12 %); 0 or
// unset = the pad's own. Raised for a stick that drifts at rest, lowered for finer small movements (the other public
// port's players asked for it: drift, their #12 #99 #111 #115).
static std::atomic<int> g_dead_zone[2] = {{-1}, {-1}};   // Npad units (0..32767), 0 = the pad's own, -1 = not read yet
static int dead_zone_units(const char* v) { double p = v && *v ? atof(v) : 0; return p > 0 ? (int)lrint(fmin(p, 90.0) * 327.67) : 0; }
static int dead_zone(int right, int own) {
    int d = g_dead_zone[right].load(std::memory_order_relaxed);
    if (d < 0) g_dead_zone[right] = d = dead_zone_units(getenv(right ? "TOTK_STICK_DEADZONE_R" : "TOTK_STICK_DEADZONE_L"));
    return d ? d : own;
}

// radial dead zone, rescaled so the edge of the dead zone is 0 and full deflection stays 32767 (Npad stick range)
static void stick(SHORT x, SHORT y, int dz, s32* ox, s32* oy) {
    float fx = x, fy = y, m = sqrtf(fx * fx + fy * fy);
    if (m <= dz) { *ox = *oy = 0; return; }
    float k = fminf((m - dz) / (32767.0f - dz), 1.0f) * 32767.0f / m;
    *ox = (s32)lrintf(fx * k); *oy = (s32)lrintf(fy * k);
}

static std::atomic<int> g_xslot{-1};   // XInput slot in use (rumble_thread: XInputSetState)
static std::atomic<u64> g_cheat_keys{0};   // hle_cheat_vm.inc: the buttons the game saw last (HidNpadButton bits, like hid::GetKeysHeld)
// The game's Jump / Sprint option: 1 when the game swaps B and X (jump_option_poll, below the npad state). Until
// the virtual-key hook (stage 2b) the keyboard's gameplay keys are Npad bits, so with it set the router sends B for
// Space and X for Shift (keyboard_read) and the prompts swap with them (kbm_glyph).
static std::atomic<int> g_jump_swapped{0};
// The glyph style in use (the glyph bank, below host_input's readers)
enum { GlyphGame = 0, GlyphKbm = 1, GlyphXbox = 2, GlyphPs = 3 };
static void glyph_unplugged(int style);   // (below hid_glyph_active)
extern "C" void pc_mouse_pad_in_use(bool on);   // pc_mouse.cpp: a pad is the device in use (menus hide the pointer)
extern "C" bool pc_mouse_pointer_moved(void);   // pc_mouse.cpp: the released pointer moved on purpose since the last call
static std::atomic<int> g_pads_scanned{0};   // bit 0: pad_read's first XInput scan, bit 1: the raw-HID scan's first pass
// Hot-plug: the game window's WM_DEVICECHANGE (shim_win.c; DBT_DEVNODES_CHANGED reaches every top-level window) bumps
// this, and the readers look for pads at once instead of at their next poll (XInput 1 s, raw HID 1.5 s; the polls stay
// for a device whose interface comes up after the notification).
static std::atomic<unsigned> g_dev_gen{0};
#ifdef _WIN32
static HANDLE dev_event() { static HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); return e; }   // raw HID's scan
static HANDLE xscan_event() { static HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); return e; }  // XInput's scan
extern "C" void hid_devices_changed(void) {   // (the window's thread: only wakes the two scan threads)
    static std::atomic<int> logs{0};
    if (logs++ < 20) printf("[hid] device change: looking for pads now\n");
    g_dev_gen++;
    SetEvent(dev_event());
    SetEvent(xscan_event());
}
#else
extern "C" void hid_devices_changed(void) { g_dev_gen++; }
#endif

#ifdef _WIN32
extern "C" void* shim_window_hwnd(void);   // shim_win.c: the game window (NULL before it exists)
// TOTK_CTL "vpad xbox": a virtual XInput pad on slot 3 behind XInputGetState / XInputSetState (test boots: plug,
// buttons, sticks, unplug and the rumble the game sends, through pad_read and rumble_thread unchanged)
static struct { std::mutex mu; bool on = false; XPad pad = {}; double btn_until = 0; WORD btn_timed = 0; DWORD packet = 0;
                WORD rl = 0, rr = 0; u64 sets = 0; } g_vx;
static const DWORD kVxSlot = 3;
static double vpad_now() { static const auto t0 = std::chrono::steady_clock::now(); return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
static DWORD xget(XInputGetState_t get, DWORD i, XState* st) {
    if (i == kVxSlot) {
        std::lock_guard<std::mutex> l(g_vx.mu);
        if (g_vx.on) {
            if (g_vx.btn_timed && vpad_now() >= g_vx.btn_until) { g_vx.pad.buttons &= ~g_vx.btn_timed; g_vx.btn_timed = 0; }
            st->packet = ++g_vx.packet; st->pad = g_vx.pad;
            return ERROR_SUCCESS;
        }
    }
    return get(i, st);
}
// The empty slots are looked at on a thread of their own: XInputGetState on an empty slot costs ~1 ms, and while USB /
// HID devices change it can take seconds. Inline in pad_read (host_input: a guest thread, holding the mutex every npad
// poll waits on) that stopped the game, 0.5-3.6 s each after "[hid] device change". The scan runs once a second while no
// pad is in use, at once on a device change (at most one scan per 250 ms: a hub or a USB drive sends a burst of them);
// pad_read reads only the slot in use and takes the one the scan found.
static std::atomic<int> g_xfound{-1};   // a slot the scan found a pad on, for pad_read to take
static void xinput_scan_thread(XInputGetState_t get) {
    for (;;) {
        if (g_xslot.load() < 0 && g_xfound.load() < 0) {
            XState st;
            for (int i = 0; i < 4; i++)
                if (xget(get, i, &st) == ERROR_SUCCESS) { g_xfound = i; break; }
        }
        g_pads_scanned |= 1;
        Sleep(250);
        WaitForSingleObject(xscan_event(), 750);
    }
}
static void pad_read(HostInput* in) {
    static XInputGetState_t get = xinput_load();
    static const bool labels = getenv("TOTK_PAD_LABELS") != nullptr;
    static int slot = -1;
    if (!get) { g_pads_scanned |= 1; return; }  // (no XInput DLL: nothing to wait for)
    static const bool scanning = (std::thread(xinput_scan_thread, get).detach(), true);
    (void)scanning;
    XState st;
    if (slot < 0) {
        const int found = g_xfound.load();
        if (found < 0) return;
        slot = found;
        g_xslot = slot;     // (before g_xfound clears: the scan thread never sees both at -1 with a pad being taken)
        g_xfound = -1;
        printf("[hid] pad connected (XInput slot %d)\n", slot);
    }
    if (xget(get, slot, &st) != ERROR_SUCCESS) {
        printf("[hid] pad on XInput slot %d disconnected\n", slot);
        slot = -1; g_xslot = -1;
        glyph_unplugged(GlyphXbox);
        return;
    }
    const XPad& p = st.pad;
    static const struct { WORD x; u64 pos, lab; } map[] = {
        {0x1000 /*A*/, BtnB, BtnA}, {0x2000 /*B*/, BtnA, BtnB}, {0x4000 /*X*/, BtnY, BtnX}, {0x8000 /*Y*/, BtnX, BtnY},
        {0x0100 /*LB*/, BtnL, BtnL}, {0x0200 /*RB*/, BtnR, BtnR}, {0x0040 /*LTHUMB*/, BtnLS, BtnLS},
        {0x0080 /*RTHUMB*/, BtnRS, BtnRS}, {0x0010 /*START*/, BtnPlus, BtnPlus}, {0x0020 /*BACK*/, BtnMinus, BtnMinus},
        {0x0001, BtnUp, BtnUp}, {0x0002, BtnDown, BtnDown}, {0x0004, BtnLeft, BtnLeft}, {0x0008, BtnRight, BtnRight}};
    for (auto& m : map) if (p.buttons & m.x) in->buttons |= labels ? m.lab : m.pos;
    if (p.lt > 30) in->buttons |= BtnZL;      // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    if (p.rt > 30) in->buttons |= BtnZR;
    stick(p.lx, p.ly, dead_zone(0, 7849), &in->lx, &in->ly);   // XINPUT_GAMEPAD_LEFT / RIGHT_THUMB_DEADZONE
    stick(p.rx, p.ry, dead_zone(1, 8689), &in->rx, &in->ry);
}
#else
static void pad_read(HostInput* in);   // SDL gamepad, below the pad state
#endif

// ---- Pads over raw HID (no XInput, no DS4Windows / BetterJoy needed), USB or Bluetooth, up to 4 at once (two Joy-Cons
// make one pad: their buttons and sticks are merged):
//  Sony DualSense / DualShock 4 (VID 054C). Report layouts as in SDL's hidapi PS4 / PS5 drivers: DS4 order LX LY RX RY
//   b0 b1 b2 L2 R2 (USB 0x01 at +1, BT 0x11 at +3, BT "simple" 0x01 at +1, also the DualSense BT simple report);
//   DualSense full order LX LY RX RY L2 R2 counter b0 b1 b2 (USB 0x01 at +1, BT 0x31 at +2). b0 = hat (low nibble,
//   8 = centred) | Square 0x10 Cross 0x20 Circle 0x40 Triangle 0x80; b1 = L1 R1 L2 R2 Create Options L3 R3; b2 = PS 1,
//   touchpad 2. Sticks 0..255, 128 centre, y down. Face buttons by position like the Xbox mapping.
//  Nintendo Pro Controller / Joy-Con L / Joy-Con R (VID 057E, PID 2009 / 2006 / 2007). Switched to the standard full
//   report 0x30 with subcommand 0x03 (USB first gets the 0x80 02/03/02/04 handshake), player light 1 via subcommand
//   0x30 (dekuNukem Nintendo_Switch_Reverse_Engineering, SDL hidapi_switch). Report: [3] Y X B A SR SL R ZR,
//   [4] - + RS LS Home Capture, [5] Down Up Right Left SR SL L ZL, [6..8] left stick, [9..11] right stick (12-bit x, y).
//   Buttons by label (Switch layout). Stick centre = first full report (sticks at rest), range grows from 1300 to
//   what the stick reaches (no SPI factory calibration read).
enum PadKind { PadNone, PadDualSense, PadDS4, PadPro, PadJoyL, PadJoyR };
// Motion in the Switch SixAxisSensor frame as Eden/yuzu feed it (x right, y forward, z up out of the face; accel in g,
// -1 on z lying flat face up; angular velocity in revolutions/s), integrated by the pad thread (motion_fuse).
struct Motion { float acc[3] = {0, 0, -1}, gyro[3] = {}, angle[3] = {}, dir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; s64 dt_ns = 5000000; u64 n = 0; };
struct HidPad {
    std::atomic<u64> buttons{0}; std::atomic<s32> lx{0}, ly{0}, rx{0}, ry{0}; std::atomic<bool> live{false};
    std::mutex mmu; Motion m; std::atomic<bool> has_imu{false};
    // rumble output: a second handle (synchronous ReadFile on the read handle would hold WriteFile until a report lands)
    std::atomic<HANDLE> wr{INVALID_HANDLE_VALUE}; std::atomic<int> kind{PadNone}; bool bt = false; int outlen = 64; u8 seq = 0;
};
static HidPad g_hidpad[4];
static HidPad g_mousepad;   // the mouse as the gyro (keyboard_read, TOTK_MOUSE_GYRO): motion only, never in g_hidpad loops
static std::atomic<u64> g_ctl_held{0};   // TOTK_CTL's buttons at the last refresh (ctl_read; the mouse gyro's aim)
static std::atomic<u64> g_ctl_pad{0};    // TOTK_CTL "pad": buttons as from a pad, at the last refresh (host_input)
// TOTK_CTL "sixaxis": the motion fields hid_GetSixAxisSensorStates passes on (the others as a pad lying still), to find
// which ones TotK's motion aiming reads (unattended A/Bs with the mouse gyro); all by default
enum { kSixAcc = 1, kSixGyro = 2, kSixAngle = 4, kSixDir = 8, kSixAll = 15, kSixDirT = 16,   // (dirT: dir transposed, A/B)
       kSixDt33 = 32, kSixLatest = 64 };                                                    // (dt33: delta_time 33 ms; latest: six_rate off)
// TOTK_CTL "gyro <x> <y> <z> <s>": a virtual pad turning at a constant rate (rev/s, Switch frame) for s seconds, then
// still (gone 10 s later): measured turns for the motion-aim A/Bs (ctl_read integrates it; imu_pad after real pads)
static HidPad g_synthpad;
static std::atomic<int> g_sixaxis_mask{kSixAll};
#ifdef _WIN32
static const char* glyph_probe();   // (below the raw-HID scan)
// glyph_probe opens every HID device (CreateFileW) and reads the XInput slots: never on the caller's thread, which is
// hle_fs's glyph patching on the game's loader thread (ARes_Loading_0). A device whose open blocks in the kernel (a
// sleeping Bluetooth pad, a filter driver, a device being installed) hung the boot before the title with every thread
// idle, and it ran 33 times in one boot. Now it runs once, on a thread of its own; its answer is cached; the first
// ask waits for it up to 250 ms, then (and until it answers) the prompts are the keyboard's.
static std::atomic<int> g_probe{-1};   // -1 not answered yet; else 0 kbm, 1 ps, 2 the game's own (Nintendo), 3 xbox
static const char* probe_style() {
    static std::atomic<bool> started{false}, waited{false};
    if (!started.exchange(true))
        std::thread([] {
            const char* s = glyph_probe();
            g_probe = !s ? 2 : !strcmp(s, "ps") ? 1 : !strcmp(s, "xbox") ? 3 : 0;
        }).detach();
    if (!waited.exchange(true))
        for (int i = 0; i < 50 && g_probe.load() < 0; i++) Sleep(5);
    static const char* const kStyles[] = {"kbm", "ps", nullptr, "xbox"};
    const int p = g_probe.load();
    if (p < 0) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) printf("[hid] glyph probe: no answer in 250 ms (a device slow to open): keyboard prompts until it answers\n");
        return "kbm";
    }
    return kStyles[p];
}
#endif
// hle_fs.cpp TOTK_GLYPHS=auto: the prompt style for the pads connected now (raw-HID pads first, then XInput). The first
// prompt archive opens at present 0: "glyphs kbm (auto)" at present 0), so with no pad seen yet the pads are looked for
// here, until both readers have looked themselves (g_pads_scanned: the probe enumerates every HID interface, and
// hle_fs.cpp's live thread asks 4 times a second).
extern "C" const char* hid_glyph_style(void) {
    for (auto& p : g_hidpad)
        if (p.live) {
            int k = p.kind;
            if (k == PadDualSense || k == PadDS4) return "ps";
            if (k == PadPro || k == PadJoyL || k == PadJoyR) return nullptr;   // the game's own prompts
        }
#ifdef _WIN32
    return g_xslot >= 0 ? "xbox" : g_pads_scanned.load() == 3 ? "kbm" : probe_style();
#else
    return g_xslot >= 0 ? "xbox" : "kbm";
#endif
}
static std::atomic<int> g_glyph_last{-1};     // the style of the input used last; -1 none yet
static std::atomic<int> g_glyph_forced{-2};   // -2: not read yet, -1: follow the input
static int glyph_code(const char* s) {
    return !s ? GlyphGame : !strcmp(s, "kbm") ? GlyphKbm : !strcmp(s, "xbox") ? GlyphXbox : !strcmp(s, "ps") ? GlyphPs
         : !strcmp(s, "switch") ? GlyphGame : -1;
}
extern "C" int hid_glyph_active(void) {
    if (g_glyph_forced.load() == -2) {
        const char* e = getenv("TOTK_GLYPHS");
        int f = e && *e && strcmp(e, "auto") ? glyph_code(e) : -1;
        int want = -2;
        g_glyph_forced.compare_exchange_strong(want, f);
    }
    int f = g_glyph_forced.load();
    if (f >= 0) return f;
    int l = g_glyph_last.load();
    return l >= 0 ? l : glyph_code(hid_glyph_style());
}
// A pad was unplugged (style: its prompts): if the prompts showed them, they follow what is still connected (another
// pad, else the keyboard and mouse) instead of staying on the gone pad's until something else is used.
static void glyph_unplugged(int style) {
    int was = g_glyph_last.load();
    if (was != style) return;
    int to = GlyphKbm;
    for (auto& p : g_hidpad)
        if (p.live) { int k = p.kind; to = k == PadDualSense || k == PadDS4 ? GlyphPs : GlyphGame; break; }
    if (to == GlyphKbm && g_xslot >= 0) to = GlyphXbox;
    if (g_glyph_last.compare_exchange_strong(was, to)) {
        printf("[hid] glyphs: the pad in use was unplugged, %s now\n", to == GlyphKbm ? "keyboard / mouse" : to == GlyphXbox ? "XInput pad"
                                                                       : to == GlyphPs ? "PlayStation pad" : "Nintendo pad");
        pc_mouse_pad_in_use(to != GlyphKbm);
    }
}
static std::mutex g_hidpad_mtx;
static std::wstring g_hidpad_path[4];   // guarded by g_hidpad_mtx; empty = free slot

#ifdef _WIN32
typedef BOOLEAN(WINAPI* HidD_GetAttributes_t)(HANDLE, void*);
typedef BOOLEAN(WINAPI* HidD_GetPreparsedData_t)(HANDLE, void**);
typedef BOOLEAN(WINAPI* HidD_FreePreparsedData_t)(void*);
typedef BOOLEAN(WINAPI* HidD_GetFeature_t)(HANDLE, void*, ULONG);
typedef LONG(WINAPI* HidP_GetCaps_t)(void*, void*);
typedef DWORD(WINAPI* CM_List_Size_t)(PULONG, LPGUID, void*, ULONG);
typedef DWORD(WINAPI* CM_List_t)(LPGUID, void*, wchar_t*, ULONG, ULONG);
static struct {
    HidD_GetAttributes_t attr; HidD_GetPreparsedData_t prep; HidD_FreePreparsedData_t freep; HidP_GetCaps_t caps; HidD_GetFeature_t getfeat;
    CM_List_Size_t list_size; CM_List_t list;
} hidapi;
#endif

static void sony_decode(const u8* r, PadKind kind, bool bt, HidPad* pad) {
    const u8* p; bool ds4_order;
    if (bt && r[0] == 0x01) { p = r + 1; ds4_order = true; }                 // BT simple report (both pads)
    else if (!bt && r[0] == 0x01) { p = r + 1; ds4_order = kind == PadDS4; }
    else if (bt && kind == PadDualSense && r[0] == 0x31) { p = r + 2; ds4_order = false; }
    else if (bt && kind == PadDS4 && r[0] >= 0x11 && r[0] <= 0x19) { p = r + 3; ds4_order = true; }
    else return;
    u8 b0 = ds4_order ? p[4] : p[7], b1 = ds4_order ? p[5] : p[8], b2 = ds4_order ? p[6] : p[9];
    u8 l2 = ds4_order ? p[7] : p[4], r2 = ds4_order ? p[8] : p[5];
    u64 b = 0;
    if (b0 & 0x40) b |= BtnA;   // Circle (right)
    if (b0 & 0x20) b |= BtnB;   // Cross (bottom)
    if (b0 & 0x80) b |= BtnX;   // Triangle (top)
    if (b0 & 0x10) b |= BtnY;   // Square (left)
    static const u64 hat[8] = {BtnUp, BtnUp | BtnRight, BtnRight, BtnRight | BtnDown, BtnDown, BtnDown | BtnLeft, BtnLeft, BtnLeft | BtnUp};
    if ((b0 & 15) < 8) b |= hat[b0 & 15];
    if (b1 & 0x01) b |= BtnL;
    if (b1 & 0x02) b |= BtnR;
    if ((b1 & 0x04) || l2 > 30) b |= BtnZL;
    if ((b1 & 0x08) || r2 > 30) b |= BtnZR;
    if (b1 & 0x10) b |= BtnMinus;
    if (b1 & 0x20) b |= BtnPlus;
    if (b1 & 0x40) b |= BtnLS;
    if (b1 & 0x80) b |= BtnRS;
    if (b2 & 0x02) b |= BtnMinus;   // touchpad click = map too
    auto ax = [](u8 v) { int d = (int)v - 128; return d < 0 ? d * 32767 / 128 : d * 32767 / 127; };   // +-32767
    s32 x, y;
    stick((SHORT)ax(p[0]), (SHORT)-ax(p[1]), dead_zone(0, 5000), &x, &y); pad->lx = x; pad->ly = y;   // y down on the pad
    stick((SHORT)ax(p[2]), (SHORT)-ax(p[3]), dead_zone(1, 5000), &x, &y); pad->rx = x; pad->ry = y;
    pad->buttons = b;
}

struct NinCal { int c[4], r[4]; bool init; };
static void nin_decode(const u8* r, PadKind kind, NinCal* k, HidPad* pad) {
    if (r[0] != 0x30 && r[0] != 0x21 && r[0] != 0x31) return;   // full report / subcommand reply / NFC-IR: same header
    int raw[4] = {r[6] | (r[7] & 0xF) << 8, r[7] >> 4 | r[8] << 4, r[9] | (r[10] & 0xF) << 8, r[10] >> 4 | r[11] << 4};
    if (!k->init) {
        k->init = true;
        for (int i = 0; i < 4; i++) k->c[i] = raw[i], k->r[i] = 1300;
        printf("[hid] Nintendo pad stick centres %d,%d %d,%d (taken at rest)\n", raw[0], raw[1], raw[2], raw[3]);
    }
    s32 v[4];
    for (int i = 0; i < 4; i++) {
        int d = raw[i] - k->c[i], a = d < 0 ? -d : d;
        if (a > k->r[i]) k->r[i] = a;
        v[i] = (s32)((long long)d * 32767 / k->r[i]);
    }
    u8 b3 = r[3], b4 = r[4], b5 = r[5];
    u64 b = 0;
    if (b3 & 0x01) b |= BtnY;
    if (b3 & 0x02) b |= BtnX;
    if (b3 & 0x04) b |= BtnB;
    if (b3 & 0x08) b |= BtnA;
    if (b3 & 0x40) b |= BtnR;
    if (b3 & 0x80) b |= BtnZR;
    if (b4 & 0x01) b |= BtnMinus;
    if (b4 & 0x02) b |= BtnPlus;
    if (b4 & 0x04) b |= BtnRS;
    if (b4 & 0x08) b |= BtnLS;
    if (b5 & 0x01) b |= BtnDown;
    if (b5 & 0x02) b |= BtnUp;
    if (b5 & 0x04) b |= BtnRight;
    if (b5 & 0x08) b |= BtnLeft;
    if (b5 & 0x40) b |= BtnL;
    if (b5 & 0x80) b |= BtnZL;
    s32 x, y;
    if (kind != PadJoyR) { stick((SHORT)v[0], (SHORT)v[1], dead_zone(0, 4000), &x, &y); pad->lx = x; pad->ly = y; }   // y up already
    if (kind != PadJoyL) { stick((SHORT)v[2], (SHORT)v[3], dead_zone(1, 4000), &x, &y); pad->rx = x; pad->ry = y; }
    pad->buttons = b;
}

// ---- motion (user 2026-10-15: "add motion control support for dualsense and switch pro controllers") Pad thread
// side, w31: Eden/yuzu SDL mapping accel (-x, z, -y) / g, gyro (x, -z, y) / 2pi from SDL's frame x right, y up, z
// toward the player) -> fusion like Eden's motion_input.cpp (gyro integration + accelerometer tilt correction,
// Mahony gain 0.5; gyro bias averaged while the pad lies still; 1.3 dps dead zone). TOTK_GYRO=0: no motion (the
// game sees a pad lying still).
struct Fusion {
    float q[4] = {1, 0, 0, 0}, bias[3] = {}; double still_s = 0, steady_s = 0; u64 logged = 0;
    double win_s = 0, win_still = 0, win_steady = 0, win_out[3] = {}; int win_logs = 0;   // [hid] gyro log window
};
static void motion_fuse(HidPad* pad, Fusion* f, const float acc[3], const float gyro_in[3], float dt) {
    if (!(dt > 0 && dt < 0.1f)) dt = 0.005f;
    float an = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    float g[3] = {gyro_in[0] - f->bias[0], gyro_in[1] - f->bias[1], gyro_in[2] - f->bias[2]};
    float gn = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    // Gyro bias (user 2026-10-15: "theres a rightward drift on the gyro... its contant"): the old rule learned it only
    // while the pad LAY still (< 4 dps, 1 g +- 0.05 for 0.5 s); held in the hands it never did, tremor kept |gyro| over
    // the dead zone and the residual yaw bias went to the game. Now three rates: lying still 0.5 s: 0.02 per sample (as
    // before); held steady (< 2 dps for 1 s, 0.9..1.1 g): time constant 5 s (ours); not moving fast (< 0.05 rev/s = 18
    // dps, 0.9..1.1 g): time constant 40 s = Eden motion_input.cpp SetGyroscope (gyro_bias * 0.9999 + gyroscope *
    // 0.0001 per sample at ~250 Hz while !IsMoving(IsAtRestRelaxed)).
    bool g1 = an > 0.9f && an < 1.1f;
    bool still = gn < 0.011f && fabsf(an - 1.0f) < 0.05f;           // < ~4 dps, ~1 g
    f->still_s = still ? f->still_s + dt : 0;
    f->steady_s = gn < 0.0055f && g1 ? f->steady_s + dt : 0;
    float rate = f->still_s > 0.5 ? 0.02f : f->steady_s > 1.0 ? dt / 5.0f : gn < 0.05f && g1 ? dt / 40.0f : 0.0f;
    if (rate > 0) for (int i = 0; i < 3; i++) f->bias[i] += (gyro_in[i] - f->bias[i]) * rate;
    if (gn < 0.0035f) g[0] = g[1] = g[2] = 0;
    f->win_s += dt; f->win_still += f->still_s > 0.5 ? dt : 0; f->win_steady += f->steady_s > 1.0 ? dt : 0;
    for (int i = 0; i < 3; i++) f->win_out[i] += g[i] * dt;
    if (f->win_s >= 30.0) {   // measurement for the next report: bias + what reached the game (mean dps) per 30 s
        if (f->win_logs++ < 60)
            printf("[hid] gyro 30 s: bias %.2f %.2f %.2f dps, game mean %.2f %.2f %.2f dps, lying still %.0f %%, held steady %.0f %%\n",
                   f->bias[0] * 360, f->bias[1] * 360, f->bias[2] * 360, f->win_out[0] / f->win_s * 360, f->win_out[1] / f->win_s * 360,
                   f->win_out[2] / f->win_s * 360, f->win_still / f->win_s * 100, f->win_steady / f->win_s * 100);
        f->win_s = f->win_still = f->win_steady = 0; f->win_out[0] = f->win_out[1] = f->win_out[2] = 0;
    }
    float w[3] = {g[0] * 6.2831853f, g[1] * 6.2831853f, g[2] * 6.2831853f};   // rad/s
    float* q = f->q;
    if (an > 0.75f && an < 1.25f) {   // expected "down" in the pad frame = R^T (0, 0, -1); error = measured x expected
        float vx = -2 * (q[1] * q[3] - q[0] * q[2]), vy = -2 * (q[0] * q[1] + q[2] * q[3]);
        float vz = -(q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3]);
        float ax = acc[0] / an, ay = acc[1] / an, az = acc[2] / an;
        const float kp = 0.5f;
        w[0] += kp * (ay * vz - az * vy); w[1] += kp * (az * vx - ax * vz); w[2] += kp * (ax * vy - ay * vx);
    }
    float h = 0.5f * dt;
    float q0 = q[0] + (-q[1] * w[0] - q[2] * w[1] - q[3] * w[2]) * h, q1 = q[1] + (q[0] * w[0] + q[2] * w[2] - q[3] * w[1]) * h;
    float q2 = q[2] + (q[0] * w[1] - q[1] * w[2] + q[3] * w[0]) * h, q3 = q[3] + (q[0] * w[2] + q[1] * w[1] - q[2] * w[0]) * h;
    float qn = 1.0f / sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q[0] = q0 * qn; q[1] = q1 * qn; q[2] = q2 * qn; q[3] = q3 * qn;
    std::lock_guard<std::mutex> l(pad->mmu);
    Motion& m = pad->m;
    for (int i = 0; i < 3; i++) { m.acc[i] = acc[i]; m.gyro[i] = g[i]; m.angle[i] += g[i] * dt; }
    // direction: the pad's x / y / z axes in the world frame (columns of R; identity lying flat, pointing forward)
    float xx = q[1] * q[1], yy = q[2] * q[2], zz = q[3] * q[3], xy = q[1] * q[2], xz = q[1] * q[3], yz = q[2] * q[3];
    float wx = q[0] * q[1], wy = q[0] * q[2], wz = q[0] * q[3];
    float r[9] = {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                  2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)};
    for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) m.dir[c * 3 + k] = r[k * 3 + c];
    m.dt_ns = (s64)(dt * 1e9f);
    m.n++;
    pad->has_imu = true;
    if (f->logged < 3 && m.n % 2000 == 1) {
        f->logged++;
        printf("[hid] motion: accel %.2f %.2f %.2f g, gyro %.3f %.3f %.3f rev/s, bias %.4f %.4f %.4f\n", acc[0], acc[1], acc[2], g[0], g[1],
               g[2], f->bias[0], f->bias[1], f->bias[2]);
    }
}
static bool ds_haptics_on() { static const bool on = getenv("TOTK_DS_HAPTICS") && atoi(getenv("TOTK_DS_HAPTICS")) != 0; return on; }
// TOTK_GYRO=0: off; live (the controls.gyro row stores 0 / 1 here: gyro_set)
static std::atomic<int> g_gyro{-1};
static bool gyro_on() {
    int on = g_gyro.load(std::memory_order_relaxed);
    if (on < 0) { const char* e = getenv("TOTK_GYRO"); g_gyro = on = !e || strcmp(e, "0") != 0; }
    return on != 0;
}
static void gyro_set(const char* v) { g_gyro = !v || strcmp(v, "0") != 0; }
// TOTK_GYRO_SCALE (the controls.gyro_scale row, live): the angular velocity the game gets, times this. TotK's motion aim
// turns by the angular velocity in the frame of the direction matrix, so the scaled rate with the true orientation
// scales the aim and keeps which way is up.
static std::atomic<float> g_gyro_scale{-1.0f};
static float gyro_scale() {
    float k = g_gyro_scale.load(std::memory_order_relaxed);
    if (k < 0) { const char* e = getenv("TOTK_GYRO_SCALE"); g_gyro_scale = k = e && *e && atof(e) > 0 ? (float)atof(e) : 1.0f; }
    return k;
}
static void gyro_scale_set(const char* v) { g_gyro_scale = v && *v && atof(v) > 0 ? (float)atof(v) : 1.0f; }
static s16 le16(const u8* p) { return (s16)(p[0] | p[1] << 8); }

// DualSense calibration (feature report 0x05; w30: SDL formulas): gyro dps = (raw - gb) * gs, accel g = (raw - ab) * as.
struct DsCal { float gs[3] = {1 / 16.0f, 1 / 16.0f, 1 / 16.0f}, gb[3] = {}, as[3] = {1 / 8192.0f, 1 / 8192.0f, 1 / 8192.0f}, ab[3] = {}; };
static void ds_imu(const u8* r, bool bt, const DsCal& k, HidPad* pad, Fusion* f, double dt) {
    const u8* p = bt ? r + 2 : r + 1;   // state struct: gyro x y z at +15, accel x y z at +21 (int16)
    float gd[3], ag[3];
    for (int i = 0; i < 3; i++) { gd[i] = (le16(p + 15 + 2 * i) - k.gb[i]) * k.gs[i]; ag[i] = (le16(p + 21 + 2 * i) - k.ab[i]) * k.as[i]; }
    const float acc[3] = {-ag[0], ag[2], -ag[1]};
    const float gyro[3] = {gd[0] / 360.0f, -gd[2] / 360.0f, gd[1] / 360.0f};
    motion_fuse(pad, f, acc, gyro, (float)dt);
}
// DualShock 4 (SDL hidapi_ps4 PS4StatePacket_t: gyro x y z at +12, accel at +18 of the state, USB 0x01 at +1, BT 0x11
// at +3; the frame as the DualSense's), calibrated by ds4_cal_read (below), else SDL's uncalibrated scales
// (PS4_GYRO_RES_PER_DEGREE 16, PS4_ACCEL_RES_PER_G 8192: the DualSense defaults). Untested with a real DS4.
static void ds4_imu(const u8* r, bool bt, const DsCal& k, HidPad* pad, Fusion* f, double dt) {
    const u8* p = bt ? r + 3 : r + 1;
    float gd[3], ag[3];
    for (int i = 0; i < 3; i++) { gd[i] = (le16(p + 12 + 2 * i) - k.gb[i]) * k.gs[i]; ag[i] = (le16(p + 18 + 2 * i) - k.ab[i]) * k.as[i]; }
    const float acc[3] = {-ag[0], ag[2], -ag[1]};
    const float gyro[3] = {gd[0] / 360.0f, -gd[2] / 360.0f, gd[1] / 360.0f};
    motion_fuse(pad, f, acc, gyro, (float)dt);
}
// Pro Controller / Joy-Con report 0x30: 3 IMU frames (accel x y z, gyro x y z) at 13 + 12 i, 5 ms apart (w31:
// 4096 LSB per g, 0.070 dps per LSB; SDL frame (-y, z, -x) for both, then the Eden mapping above).
static void nin_imu(const u8* r, HidPad* pad, Fusion* f) {
    if (r[0] != 0x30) return;
    for (int s = 0; s < 3; s++) {
        const u8* p = r + 13 + 12 * s;
        float a[3] = {le16(p) / 4096.0f, le16(p + 2) / 4096.0f, le16(p + 4) / 4096.0f};
        float gd[3] = {le16(p + 6) * 0.0700f, le16(p + 8) * 0.0700f, le16(p + 10) * 0.0700f};
        const float acc[3] = {a[1], -a[0], -a[2]};
        const float gyro[3] = {-gd[1] / 360.0f, gd[0] / 360.0f, gd[2] / 360.0f};
        motion_fuse(pad, f, acc, gyro, 0.005f);
    }
}

#ifdef _WIN32
static bool hid_write(HANDLE h, const u8* d, int n, int outlen) {
    u8 buf[1024] = {};
    if (n > (int)sizeof buf) n = sizeof buf;
    if (outlen > (int)sizeof buf) outlen = sizeof buf;
    memcpy(buf, d, n);
    DWORD w = 0;
    return WriteFile(h, buf, (DWORD)(outlen > n ? outlen : n), &w, nullptr) != 0;
}
static void nin_subcmd(HANDLE h, int outlen, u8* pkt, u8 cmd, u8 arg) {
    u8 b[12] = {0x01, (u8)((*pkt)++ & 15), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, cmd, arg};   // neutral rumble
    if (!hid_write(h, b, 12, outlen)) printf("[hid] Nintendo subcommand %02x failed (%lu)\n", cmd, GetLastError());
    Sleep(60);
}

static u32 crc32_buf(const u8* d, size_t n) {   // CRC-32 (reflected 0xEDB88320), as SDL_crc32 / crc32_le
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void ds_cal_read(HANDLE h, int featlen, DsCal* k) {
    u8 b[256] = {0x05};
    if (!hidapi.getfeat || featlen < 41 || featlen > (int)sizeof b || !hidapi.getfeat(h, b, (ULONG)featlen)) {
        printf("[hid] DualSense calibration (feature 0x05) not read (%lu): default scales\n", GetLastError());
        return;
    }
    auto v = [&](int o) { return (float)le16(b + o); };
    float speed = v(19) + v(21);
    for (int i = 0; i < 3; i++) {
        float plus = v(7 + 4 * i), minus = v(9 + 4 * i), den = plus - minus;
        float gs = den != 0 ? speed / den : 0;
        if (gs > 1 / 40.0f && gs < 1 / 8.0f) { k->gs[i] = gs; k->gb[i] = v(1 + 2 * i); }
        float ap = v(23 + 4 * i), am = v(25 + 4 * i), range = ap - am;
        if (range > 8192 && range < 32768) { k->as[i] = 2.0f / range; k->ab[i] = ap - range / 2; }
    }
    printf("[hid] DualSense calibration: gyro %.4f %.4f %.4f dps/LSB bias %.0f %.0f %.0f, accel %.6f %.6f %.6f g/LSB\n", k->gs[0], k->gs[1],
           k->gs[2], k->gb[0], k->gb[1], k->gb[2], k->as[0], k->as[1], k->as[2]);
}
// DualShock 4 calibration: feature report 0x02 over USB, 0x05 over Bluetooth (SDL hidapi_ps4 / DS4Windows): gyro bias
// pitch / yaw / roll at 1 / 3 / 5; the plus / minus references at 7..18, pairs per axis over USB (pitch+ pitch- yaw+ yaw-
// roll+ roll-), all plus then all minus over Bluetooth; speed plus / minus at 19 / 21; accel x+ x- y+ y- z+ z- at 23..33.
// Scales outside the DualSense's sanity range keep the defaults (the bias is still taken: it is what stops the drift).
static void ds4_cal_read(HANDLE h, int featlen, bool bt, DsCal* k) {
    u8 b[256] = {(u8)(bt ? 0x05 : 0x02)};
    const int need = bt ? 41 : 37;
    const int len = featlen >= need && featlen <= (int)sizeof b ? featlen : need;
    if (!hidapi.getfeat || !hidapi.getfeat(h, b, (ULONG)len)) {
        printf("[hid] DualShock 4 calibration (feature %02x) not read (%lu): default scales, no bias\n", b[0], GetLastError());
        return;
    }
    auto v = [&](int o) { return (float)le16(b + o); };
    const float speed = v(19) + v(21);
    for (int i = 0; i < 3; i++) {
        k->gb[i] = v(1 + 2 * i);
        const float plus = bt ? v(7 + 2 * i) : v(7 + 4 * i), minus = bt ? v(13 + 2 * i) : v(9 + 4 * i), den = plus - minus;
        const float gs = den != 0 ? speed / den : 0;
        if (gs > 1 / 40.0f && gs < 1 / 8.0f) k->gs[i] = gs;
        const float ap = v(23 + 4 * i), am = v(25 + 4 * i), range = ap - am;
        if (range > 8192 && range < 32768) { k->as[i] = 2.0f / range; k->ab[i] = ap - range / 2; }
    }
    printf("[hid] DualShock 4 calibration: gyro %.4f %.4f %.4f dps/LSB bias %.0f %.0f %.0f, accel %.6f %.6f %.6f g/LSB\n", k->gs[0],
           k->gs[1], k->gs[2], k->gb[0], k->gb[1], k->gb[2], k->as[0], k->as[1], k->as[2]);
}
// DualSense output report (w30, SDL DS5EffectsState_t): USB 0x02 (effects at [1], 48 bytes); Bluetooth 0x31 (78 bytes:
// [1] sequence << 4, [2] 0x10, effects at [3], CRC-32 of 0xA2 + bytes 0..73 at 74..77). Sending 0x31 is also what
// switches a Bluetooth DualSense from the simple 0x01 report (no motion data) to the full 0x31 report.
// effects [0] 0x01 rumble emulation | 0x02 classic rumble (audio haptics off), [2] right motor, [3] left motor,
// [38] 0x04 improved rumble emulation (firmware >= 2.24: full-range motor values).
static void ds_effects(HidPad* p, HANDLE w, u8 left, u8 right, bool haptics) {
    u8 b[80] = {};
    int off = p->bt ? 3 : 1, size = p->bt ? 78 : 48;
    if (p->bt) { b[0] = 0x31; b[1] = (u8)(p->seq++ << 4); b[2] = 0x10; } else b[0] = 0x02;
    u8* e = b + off;
    e[0] = haptics ? 0x00 : 0x03;
    e[2] = right; e[3] = left;
    e[38] = 0x04;
    if (p->bt) {
        u8 t[80]; t[0] = 0xA2; memcpy(t + 1, b, 74);
        u32 c = crc32_buf(t, 75);
        b[74] = (u8)c; b[75] = (u8)(c >> 8); b[76] = (u8)(c >> 16); b[77] = (u8)(c >> 24);
    }
    if (!hid_write(w, b, size, p->outlen)) {
        static std::atomic<int> told{0};
        if (told++ < 3) printf("[hid] DualSense output report failed (%lu)\n", GetLastError());
    }
}

static void hidpad_thread(int slot, std::wstring path, PadKind kind, bool bt, HANDLE h) {
    HidPad* pad = &g_hidpad[slot];
    int outlen = 64, featlen = 0;
    void* pd = nullptr;
    if (hidapi.prep(h, &pd)) {
        USHORT caps[32] = {};   // HIDP_CAPS: Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength
        if (hidapi.caps(pd, caps) >= 0 && caps[3]) { outlen = caps[3]; featlen = caps[4]; }
        hidapi.freep(pd);
    }
    // rumble writes go through their own handle (rumble_thread); motion/rumble state on the pad
    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    pad->bt = bt; pad->outlen = outlen; pad->seq = 0; pad->has_imu = false;
    { std::lock_guard<std::mutex> l(pad->mmu); pad->m = Motion{}; }
    if (kind == PadPro || kind == PadJoyL || kind == PadJoyR) {
        if (!bt) {   // USB: handshake, 3 Mbit, handshake, HID only (no timeout) before Bluetooth-style subcommands work
            for (u8 c : {0x02, 0x03, 0x02, 0x04}) { u8 m[2] = {0x80, c}; hid_write(h, m, 2, outlen); Sleep(60); }
        }
        u8 pkt = 0;
        nin_subcmd(h, outlen, &pkt, 0x03, 0x30);   // input report mode: standard full (0x30)
        nin_subcmd(h, outlen, &pkt, 0x30, 0x01);   // player light 1
        nin_subcmd(h, outlen, &pkt, 0x40, 0x01);   // IMU on (w31)
        nin_subcmd(h, outlen, &pkt, 0x48, 0x01);   // vibration on (HD rumble)
        pad->seq = pkt;
    }
    DsCal dscal;
    if (kind == PadDS4) ds4_cal_read(h, featlen, bt, &dscal);   // (Bluetooth: reading it also starts the full 0x11 reports)
    if (kind == PadDualSense) {
        ds_cal_read(h, featlen, &dscal);
        if (w != INVALID_HANDLE_VALUE) ds_effects(pad, w, 0, 0, bt && ds_haptics_on());   // (Bluetooth: full 0x31 reports from here on)
    }
    pad->wr = w;
    pad->kind = kind;
    Fusion fus;
    auto imu_t = std::chrono::steady_clock::now();
    NinCal cal = {};
    u8 rep[1024];
    DWORD n = 0;
    int nlog = 0;
    u64 last = 0;
    bool first = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // input latency: decode as soon as a report lands
    u64 rate_n = 0, rate_logs = 0;
    auto rate_t0 = std::chrono::steady_clock::now();
    while (ReadFile(h, rep, sizeof rep, &n, nullptr) && n > 0) {
        if (kind == PadDualSense || kind == PadDS4) sony_decode(rep, kind, bt, pad);
        else nin_decode(rep, kind, &cal, pad);
        if (gyro_on()) {
            if (kind == PadDualSense && ((bt && rep[0] == 0x31) || (!bt && rep[0] == 0x01 && n >= 64))) {
                auto t = std::chrono::steady_clock::now();
                ds_imu(rep, bt, dscal, pad, &fus, std::chrono::duration<double>(t - imu_t).count());
                imu_t = t;
            } else if (kind == PadDS4 && ((bt && rep[0] == 0x11 && n >= 78) || (!bt && rep[0] == 0x01 && n >= 64))) {
                auto t = std::chrono::steady_clock::now();
                ds4_imu(rep, bt, dscal, pad, &fus, std::chrono::duration<double>(t - imu_t).count());
                imu_t = t;
            } else if (kind == PadPro || kind == PadJoyL || kind == PadJoyR) nin_imu(rep, pad, &fus);
        }
        // report rate (input latency, user 2026-10-15): one line per 10 s for the first minute
        rate_n++;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_t0).count();
        if (el >= 10.0 && rate_logs < 6) {
            printf("[hid] slot %d: %.0f reports/s (report %02x, %lu bytes)\n", slot, rate_n / el, rep[0], n);
            rate_logs++; rate_n = 0; rate_t0 = std::chrono::steady_clock::now();
        }
        if (first) {
            first = false;
            printf("[hid] slot %d first report %02x (%lu bytes): sticks %d,%d %d,%d buttons %llx\n", slot, rep[0], n, pad->lx.load(),
                   pad->ly.load(), pad->rx.load(), pad->ry.load(), (unsigned long long)pad->buttons.load());
        }
        pad->live = true;
        u64 b = pad->buttons.load();
        if (b != last && b && nlog < 12) { nlog++; printf("[hid] slot %d buttons %llx (report %02x)\n", slot, (unsigned long long)b, rep[0]); }
        last = b;
    }
    printf("[hid] slot %d disconnected (%lu)\n", slot, GetLastError());
    pad->live = false; pad->has_imu = false; pad->buttons = 0; pad->lx = pad->ly = pad->rx = pad->ry = 0;
    pad->kind = PadNone;
    glyph_unplugged(kind == PadDualSense || kind == PadDS4 ? GlyphPs : GlyphGame);
    HANDLE ow = pad->wr.exchange(INVALID_HANDLE_VALUE);
    Sleep(20);   // (rumble_thread may be mid-write on the old handle)
    if (ow != INVALID_HANDLE_VALUE) CloseHandle(ow);
    CloseHandle(h);
    std::lock_guard<std::mutex> l(g_hidpad_mtx);
    g_hidpad_path[slot].clear();
}

static bool hidapi_load() {   // hid.dll + cfgmgr32.dll entry points, once (the scan thread, glyph_probe)
    static std::once_flag once;
    static bool ok;
    std::call_once(once, [] {
        HMODULE hid = LoadLibraryA("hid.dll"), cfg = LoadLibraryA("cfgmgr32.dll");
        if (hid) {
            hidapi.attr = (HidD_GetAttributes_t)GetProcAddress(hid, "HidD_GetAttributes");
            hidapi.prep = (HidD_GetPreparsedData_t)GetProcAddress(hid, "HidD_GetPreparsedData");
            hidapi.freep = (HidD_FreePreparsedData_t)GetProcAddress(hid, "HidD_FreePreparsedData");
            hidapi.caps = (HidP_GetCaps_t)GetProcAddress(hid, "HidP_GetCaps");
            hidapi.getfeat = (HidD_GetFeature_t)GetProcAddress(hid, "HidD_GetFeature");
        }
        if (cfg) {
            hidapi.list_size = (CM_List_Size_t)GetProcAddress(cfg, "CM_Get_Device_Interface_List_SizeW");
            hidapi.list = (CM_List_t)GetProcAddress(cfg, "CM_Get_Device_Interface_ListW");
        }
        ok = hidapi.attr && hidapi.prep && hidapi.freep && hidapi.caps && hidapi.list_size && hidapi.list;
    });
    return ok;
}
static const GUID kHidGuid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};   // GUID_DEVINTERFACE_HID
static std::vector<wchar_t> hid_paths() {   // the HID interfaces present now, as CM_Get_Device_Interface_List's list
    ULONG len = 0;
    GUID g = kHidGuid;
    if (hidapi.list_size(&len, &g, nullptr, 0) != 0 || len <= 1) return {};
    std::vector<wchar_t> buf(len);
    if (hidapi.list(&g, nullptr, buf.data(), len, 0) != 0) return {};
    return buf;
}
static PadKind hid_kind(USHORT vid, USHORT pid) {
    if (vid == 0x054C) return (pid == 0x0CE6 || pid == 0x0DF2) ? PadDualSense : (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0) ? PadDS4 : PadNone;
    if (vid == 0x057E) return pid == 0x2009 ? PadPro : pid == 0x2006 ? PadJoyL : pid == 0x2007 ? PadJoyR : PadNone;
    return PadNone;
}
static PadKind hid_path_kind(const wchar_t* path, USHORT* pid_out = nullptr) {   // by VID / PID, without opening it for I/O
    HANDLE q = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (q == INVALID_HANDLE_VALUE) return PadNone;
    struct { ULONG size; USHORT vid, pid, ver; } at = {sizeof at, 0, 0, 0};
    bool ok = hidapi.attr(q, &at);
    CloseHandle(q);
    if (pid_out) *pid_out = at.pid;
    return ok ? hid_kind(at.vid, at.pid) : PadNone;
}
// hid_glyph_style before any pad was read: the raw-HID pads present (Sony: ps, Nintendo: the game's own), then the
// XInput slots (xbox), else kbm
static const char* glyph_probe() {
    const char* style = "kbm";
    if (hidapi_load()) {
        std::vector<wchar_t> buf = hid_paths();
        for (const wchar_t* path = buf.data(); !buf.empty() && *path; path += wcslen(path) + 1) {
            PadKind k = hid_path_kind(path);
            if (k == PadDualSense || k == PadDS4) { style = "ps"; break; }
            if (k != PadNone) { style = nullptr; break; }
        }
    }
    if (style && !strcmp(style, "kbm"))
        for (const char* n : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(n)) {
                if (auto get = (XInputGetState_t)GetProcAddress(m, "XInputGetState")) {
                    XState st;
                    for (int i = 0; i < 4; i++) if (get(i, &st) == ERROR_SUCCESS) { style = "xbox"; break; }
                    break;
                }
            }
    printf("[hid] glyph probe (no pad read yet): %s\n", style ? style : "Nintendo pad");
    return style;
}

static void hidpad_scan_thread() {
    if (!hidapi_load()) {
        printf("[hid] raw HID unavailable: no DualSense / DS4 / Pro Controller / Joy-Con support\n");
        g_pads_scanned |= 2;   // (the glyph probe has nothing to wait for)
        return;
    }
    for (;; WaitForSingleObject(dev_event(), 1500)) {   // (hid_devices_changed sets it: a plug is seen at once)
        std::vector<wchar_t> buf = hid_paths();
        if (buf.empty()) { g_pads_scanned |= 2; continue; }
        for (const wchar_t* path = buf.data(); *path; path += wcslen(path) + 1) {
            int slot = -1;
            {
                std::lock_guard<std::mutex> l(g_hidpad_mtx);
                bool open = false;
                for (int i = 0; i < 4; i++) open |= g_hidpad_path[i] == path;
                for (int i = 0; i < 4 && slot < 0 && !open; i++) if (g_hidpad_path[i].empty()) slot = i;
            }
            if (slot < 0) continue;
            USHORT pid = 0;
            PadKind kind = hid_path_kind(path, &pid);
            if (kind == PadNone) continue;
            HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            std::wstring lp(path);
            for (auto& ch : lp) ch = towlower(ch);
            bool bt = lp.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
            static const char* const names[] = {"", "DualSense", "DualShock 4", "Pro Controller", "Joy-Con (L)", "Joy-Con (R)"};
            printf("[hid] %s connected over %s (pid %04x) -> slot %d\n", names[kind], bt ? "Bluetooth" : "USB", pid, slot);
            {
                std::lock_guard<std::mutex> l(g_hidpad_mtx);
                g_hidpad_path[slot] = path;
            }
            std::thread(hidpad_thread, slot, std::wstring(path), kind, bt, h).detach();
        }
        g_pads_scanned |= 2;
    }
}

// TOTK_CTL "vpad ds": a virtual DualSense for test boots. Its input reports go through a message-mode named pipe to the
// real reader (hidpad_thread: sony_decode, ds_imu, the disconnect path), and the output reports the runtime sends it
// (the Bluetooth switch to 0x31, rumble, haptics) come back through a second one, where their CRC and motors are checked
// and logged ("[vpad]"). Over Bluetooth it sends the simple 0x01 report (no motion) until the runtime's first 0x31
// output report, as a real one does. No feature report on a pipe: the reader keeps the default calibration (1/16 dps,
// 1/8192 g per LSB), which is what the sensor values are encoded with.
static struct {
    std::mutex mu; bool on = false, bt = false, full = false; int slot = -1; unsigned gen = 0;
    u64 btn = 0, btn_timed = 0; double btn_until = 0, gyro_until = 0;   // Switch buttons by position (BtnA = Circle)
    float stick[4] = {}, trig[2] = {}, gyro[3] = {}, acc[3] = {0, 0, -1};   // sticks -1..1 y up; Switch frame rev/s, g
    u8 motor[2] = {}; u64 outs = 0, rumbles = 0, haptics = 0, bad = 0;
} g_vds;
static void vds_report(u8* r, DWORD* len, u8 counter) {   // g_vds.mu held
    if (g_vds.btn_timed && vpad_now() >= g_vds.btn_until) { g_vds.btn &= ~g_vds.btn_timed; g_vds.btn_timed = 0; }
    if (g_vds.gyro_until > 0 && vpad_now() >= g_vds.gyro_until) { g_vds.gyro[0] = g_vds.gyro[1] = g_vds.gyro[2] = 0; g_vds.gyro_until = 0; }
    const u64 b = g_vds.btn;
    const bool u = b & BtnUp, d = b & BtnDown, l = b & BtnLeft, rt = b & BtnRight;
    u8 b0 = u && !d ? (rt ? 1 : l ? 7 : 0) : d && !u ? (rt ? 3 : l ? 5 : 4) : rt ? 2 : l ? 6 : 8;   // hat, 8 = centred
    if (b & BtnA) b0 |= 0x40;   // Circle
    if (b & BtnB) b0 |= 0x20;   // Cross
    if (b & BtnX) b0 |= 0x80;   // Triangle
    if (b & BtnY) b0 |= 0x10;   // Square
    u8 b1 = (b & BtnL ? 0x01 : 0) | (b & BtnR ? 0x02 : 0) | (b & BtnZL ? 0x04 : 0) | (b & BtnZR ? 0x08 : 0) |
            (b & BtnMinus ? 0x10 : 0) | (b & BtnPlus ? 0x20 : 0) | (b & BtnLS ? 0x40 : 0) | (b & BtnRS ? 0x80 : 0);
    auto axis = [](float v, bool flip) { v = fmaxf(-1.0f, fminf(1.0f, flip ? -v : v)); return (u8)lrintf(128 + v * 127); };
    const u8 sticks[4] = {axis(g_vds.stick[0], false), axis(g_vds.stick[1], true), axis(g_vds.stick[2], false), axis(g_vds.stick[3], true)};
    const u8 l2 = b & BtnZL ? 255 : (u8)lrintf(fmaxf(0, fminf(1, g_vds.trig[0])) * 255);
    const u8 r2 = b & BtnZR ? 255 : (u8)lrintf(fmaxf(0, fminf(1, g_vds.trig[1])) * 255);
    if (g_vds.bt && !g_vds.full) {   // Bluetooth simple report: DS4 order, no motion
        r[0] = 0x01; memcpy(r + 1, sticks, 4); r[5] = b0; r[6] = b1; r[7] = 0; r[8] = l2; r[9] = r2;
        *len = 10;
        return;
    }
    u8* p = g_vds.bt ? r + 2 : r + 1;
    r[0] = g_vds.bt ? 0x31 : 0x01;
    if (g_vds.bt) r[1] = (u8)(counter << 4);
    memcpy(p, sticks, 4); p[4] = l2; p[5] = r2; p[6] = counter; p[7] = b0; p[8] = b1; p[9] = 0;
    // the reader's frame change inverted (ds_imu): Switch gyro (x, y, z) = (gx, -gz, gy) / 360, accel = (-ax, az, -ay)
    const float gd[3] = {g_vds.gyro[0] * 360, g_vds.gyro[2] * 360, -g_vds.gyro[1] * 360};
    const float ag[3] = {-g_vds.acc[0], -g_vds.acc[2], g_vds.acc[1]};
    for (int i = 0; i < 3; i++) {
        const s16 gr = (s16)lrintf(fmaxf(-32768, fminf(32767, gd[i] * 16))), ar = (s16)lrintf(fmaxf(-32768, fminf(32767, ag[i] * 8192)));
        p[15 + 2 * i] = (u8)gr; p[16 + 2 * i] = (u8)(gr >> 8); p[21 + 2 * i] = (u8)ar; p[22 + 2 * i] = (u8)(ar >> 8);
    }
    *len = g_vds.bt ? 78 : 64;
}
static void vds_feed(unsigned gen, HANDLE srv) {   // 250 reports/s, as a DualSense over USB
    if (!ConnectNamedPipe(srv, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) printf("[vpad] ds input pipe: no reader (%lu)\n", GetLastError());
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (u8 counter = 0;; counter++) {
        u8 r[80] = {};
        DWORD len = 0, w = 0;
        {
            std::lock_guard<std::mutex> l(g_vds.mu);
            if (!g_vds.on || g_vds.gen != gen) break;
            vds_report(r, &len, counter);
        }
        if (!WriteFile(srv, r, len, &w, nullptr)) { printf("[vpad] ds input pipe closed (%lu)\n", GetLastError()); break; }
        LARGE_INTEGER due; due.QuadPart = -40000;   // 4 ms
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 20);
        else Sleep(4);
    }
    if (timer) CloseHandle(timer);
    DisconnectNamedPipe(srv);   // the reader's ReadFile fails, as on a real unplug
    CloseHandle(srv);
}
static void vds_out(const u8* b, DWORD n) {   // an output report the runtime wrote to the virtual DualSense
    std::lock_guard<std::mutex> l(g_vds.mu);
    g_vds.outs++;
    const u8* e = nullptr;
    bool crc_ok = true;
    auto crc_at = [&](DWORD end) { u8 t[160]; t[0] = 0xA2; memcpy(t + 1, b, end); u32 c = crc32_buf(t, end + 1);
                                   return n >= end + 4 && b[end] == (u8)c && b[end + 1] == (u8)(c >> 8) && b[end + 2] == (u8)(c >> 16) && b[end + 3] == (u8)(c >> 24); };
    if (b[0] == 0x02 && n >= 48) e = b + 1;
    else if (b[0] == 0x31 && n >= 78) { e = b + 3; crc_ok = crc_at(74); }
    else if (b[0] == 0x32 && n >= 142) {
        g_vds.haptics++;
        if (!crc_at(138)) g_vds.bad++;
        if (g_vds.haptics == 1) printf("[vpad] ds: first haptics report 0x32 (CRC %s)\n", crc_at(138) ? "ok" : "BAD");
        return;
    } else { if (g_vds.bad++ < 5) printf("[vpad] ds: unexpected output report %02x (%lu bytes)\n", b[0], n); return; }
    if (!crc_ok) { if (g_vds.bad++ < 5) printf("[vpad] ds: output report 31 with a bad CRC\n"); return; }
    if (b[0] == 0x31 && !g_vds.full) { g_vds.full = true; printf("[vpad] ds: 0x31 output report (CRC ok): full input reports from now on\n"); }
    if (e[3] != g_vds.motor[0] || e[2] != g_vds.motor[1]) {
        g_vds.rumbles++;
        if (g_vds.rumbles <= 40) printf("[vpad] ds rumble: left %u right %u (report %02x, flags %02x %02x)\n", e[3], e[2], b[0], e[0], e[38]);
        g_vds.motor[0] = e[3]; g_vds.motor[1] = e[2];
    }
}
static void vds_drain(HANDLE srv) {
    if (!ConnectNamedPipe(srv, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) printf("[vpad] ds output pipe: no writer (%lu)\n", GetLastError());
    u8 b[1024];
    DWORD n = 0;
    while (ReadFile(srv, b, sizeof b, &n, nullptr) && n > 0) vds_out(b, n);
    CloseHandle(srv);
}
static bool vds_plug(bool bt) {
    if (!hidapi_load()) return false;   // (the reader's HidD calls)
    std::lock_guard<std::mutex> l(g_vds.mu);
    if (g_vds.on) return false;
    int slot = -1;
    {
        std::lock_guard<std::mutex> m(g_hidpad_mtx);
        for (int i = 0; i < 4 && slot < 0; i++) if (g_hidpad_path[i].empty()) slot = i;
    }
    if (slot < 0) return false;
    const unsigned gen = ++g_vds.gen;
    wchar_t in_name[96], out_name[96];
    swprintf(in_name, 96, L"\\\\.\\pipe\\totk-vpad-%lu-%u-in", GetCurrentProcessId(), gen);
    swprintf(out_name, 96, L"\\\\.\\pipe\\totk-vpad-%lu-%u-out", GetCurrentProcessId(), gen);
    HANDLE in_srv = CreateNamedPipeW(in_name, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE out_srv = CreateNamedPipeW(out_name, PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE cli = in_srv == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE
               : CreateFileW(in_name, GENERIC_READ | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD mode = PIPE_READMODE_MESSAGE;   // one ReadFile, one report
    if (out_srv == INVALID_HANDLE_VALUE || cli == INVALID_HANDLE_VALUE || !SetNamedPipeHandleState(cli, &mode, nullptr, nullptr)) {
        printf("[vpad] ds: pipes failed (%lu)\n", GetLastError());
        for (HANDLE h : {in_srv, out_srv, cli}) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return false;
    }
    g_vds.btn = g_vds.btn_timed = 0;
    for (float& v : g_vds.stick) v = 0;
    g_vds.trig[0] = g_vds.trig[1] = 0;
    for (int i = 0; i < 3; i++) g_vds.gyro[i] = 0, g_vds.acc[i] = i == 2 ? -1.0f : 0.0f;
    g_vds.motor[0] = g_vds.motor[1] = 0;
    g_vds.on = true; g_vds.bt = bt; g_vds.full = false; g_vds.slot = slot;
    {
        std::lock_guard<std::mutex> m(g_hidpad_mtx);
        g_hidpad_path[slot] = out_name;   // (the reader opens its rumble handle on this path)
    }
    printf("[vpad] DualSense plugged over %s -> slot %d\n", bt ? "Bluetooth" : "USB", slot);
    std::thread(vds_drain, out_srv).detach();
    std::thread(vds_feed, gen, in_srv).detach();
    std::thread(hidpad_thread, slot, std::wstring(out_name), PadDualSense, bt, cli).detach();
    return true;
}
static void xset_any(XInputSetState_t xset, DWORD slot, XVib* v);   // (below: the virtual XInput pad's rumble)
// "vpad ds usb|bt|off|btn <A+B..|none> [s]|stick L|R x y|trig L|R v|gyro x y z|acc x y z|stat" and
// "vpad xbox on|off|btn ..|stick ..|trig ..|stat"; buttons by the Switch names, placed by position as the readers map them
template <class Bits> static bool vpad_cmd(const char* line, Bits bits_of) {
    char dev[8] = {}, op[8] = {}, a1[64] = {};
    float f[3] = {};
    const int n = sscanf(line, "%*s %7s %7s %63s %f %f %f", dev, op, a1, &f[0], &f[1], &f[2]);
    const bool ds = !strcmp(dev, "ds"), xb = !strcmp(dev, "xbox");
    if (n < 2 || (!ds && !xb)) return false;
    const double now = vpad_now();
    if (!strcmp(op, "stat")) {
        if (ds) { std::lock_guard<std::mutex> l(g_vds.mu);
                  printf("[vpad] ds stat: %s, %llu output reports, %llu rumble changes (now left %u right %u), %llu haptics, %llu bad\n",
                         g_vds.on ? "plugged" : "unplugged", (unsigned long long)g_vds.outs, (unsigned long long)g_vds.rumbles,
                         g_vds.motor[0], g_vds.motor[1], (unsigned long long)g_vds.haptics, (unsigned long long)g_vds.bad); }
        else { std::lock_guard<std::mutex> l(g_vx.mu);
               printf("[vpad] xbox stat: %s, %llu XInputSetState calls (now left %u right %u)\n", g_vx.on ? "plugged" : "unplugged",
                      (unsigned long long)g_vx.sets, g_vx.rl, g_vx.rr); }
        return true;
    }
    if (ds && (!strcmp(op, "usb") || !strcmp(op, "bt"))) return vds_plug(op[0] == 'b');
    if (xb && !strcmp(op, "on")) {
        { std::lock_guard<std::mutex> l(g_vx.mu); if (g_vx.on) return false; g_vx.on = true; g_vx.pad = {}; g_vx.btn_timed = 0; g_vx.rl = g_vx.rr = 0; }
        printf("[vpad] XInput pad plugged on slot %lu\n", kVxSlot);
        if (HWND h = (HWND)shim_window_hwnd()) PostMessageW(h, WM_DEVICECHANGE, 0x0007 /*DBT_DEVNODES_CHANGED*/, 0);   // as Windows sends it
        return true;
    }
    if (!strcmp(op, "off")) {
        if (ds) { std::lock_guard<std::mutex> l(g_vds.mu); if (!g_vds.on) return false; g_vds.on = false; }
        else { std::lock_guard<std::mutex> l(g_vx.mu); if (!g_vx.on) return false; g_vx.on = false; }
        printf("[vpad] %s unplugged\n", ds ? "DualSense" : "XInput pad");
        if (HWND h = (HWND)shim_window_hwnd()) PostMessageW(h, WM_DEVICECHANGE, 0x0007, 0);
        return true;
    }
    if (!strcmp(op, "btn") && n >= 3) {
        u64 b = 0;
        if (strcmp(a1, "none") && !bits_of(a1, &b)) return false;
        if (b & ~0xFFFFull) return false;
        const double secs = n >= 4 && f[0] > 0 ? f[0] : 0.3;
        const bool timed = !!strcmp(a1, "none") && !(n >= 4 && f[0] < 0);   // (a negative time: held until "btn none")
        if (ds) { std::lock_guard<std::mutex> l(g_vds.mu); g_vds.btn = b; g_vds.btn_timed = timed ? b : 0; g_vds.btn_until = now + secs; }
        else {
            // Switch bit -> XInput button by position (pad_read's map: XInput B = right = Switch A, A = B, Y = X, X = Y)
            static const struct { u64 s; WORD x; } m[] = {{BtnA, 0x2000}, {BtnB, 0x1000}, {BtnX, 0x8000}, {BtnY, 0x4000}, {BtnL, 0x0100},
                {BtnR, 0x0200}, {BtnLS, 0x0040}, {BtnRS, 0x0080}, {BtnPlus, 0x0010}, {BtnMinus, 0x0020}, {BtnUp, 0x0001},
                {BtnDown, 0x0002}, {BtnLeft, 0x0004}, {BtnRight, 0x0008}};
            WORD x = 0;
            for (auto& e : m) if (b & e.s) x |= e.x;
            std::lock_guard<std::mutex> l(g_vx.mu);
            g_vx.pad.buttons = x; g_vx.pad.lt = b & BtnZL ? 255 : 0; g_vx.pad.rt = b & BtnZR ? 255 : 0;
            g_vx.btn_timed = timed ? x : 0; g_vx.btn_until = now + secs;
        }
        return true;
    }
    if ((!strcmp(op, "stick") && n >= 5) || (!strcmp(op, "trig") && n >= 4)) {
        const int k = toupper((unsigned char)a1[0]) == 'R';
        if (toupper((unsigned char)a1[0]) != 'L' && !k) return false;
        const bool st = op[0] == 's';
        if (ds) {
            std::lock_guard<std::mutex> l(g_vds.mu);
            if (st) { g_vds.stick[2 * k] = f[0]; g_vds.stick[2 * k + 1] = f[1]; } else g_vds.trig[k] = f[0];
        } else {
            auto s16v = [](float v) { return (SHORT)lrintf(fmaxf(-1.0f, fminf(1.0f, v)) * 32767); };
            std::lock_guard<std::mutex> l(g_vx.mu);
            if (st) { if (k) { g_vx.pad.rx = s16v(f[0]); g_vx.pad.ry = s16v(f[1]); } else { g_vx.pad.lx = s16v(f[0]); g_vx.pad.ly = s16v(f[1]); } }
            else (k ? g_vx.pad.rt : g_vx.pad.lt) = (BYTE)lrintf(fmaxf(0, fminf(1, f[0])) * 255);
        }
        return true;
    }
    if (ds && (!strcmp(op, "gyro") || !strcmp(op, "acc"))) {   // gyro x y z [s] (rev/s; s: then still) | acc x y z (g): Switch frame
        float v[3], secs = 0;
        if (sscanf(line, "%*s %*s %*s %f %f %f %f", &v[0], &v[1], &v[2], &secs) < 3) return false;
        std::lock_guard<std::mutex> l(g_vds.mu);
        memcpy(op[0] == 'g' ? g_vds.gyro : g_vds.acc, v, sizeof v);
        if (op[0] == 'g') g_vds.gyro_until = secs > 0 ? now + secs : 0;
        return true;
    }
    return false;
}

#else
// ---- Linux: ONE SDL3 gamepad (the first one found; Steam Input presents the Deck's controls as a virtual Xbox 360 / DualSense /
// Switch Pro pad that SDL reads like any other). Buttons / sticks are polled through the SDL gamepad API (state is updated by
// the event pump on the UI thread, device.c ui_run -> shim_sdl3.c totk_wait_event); motion arrives as sensor events on that thread
// (totk_hid_sensor below) and feeds the same fusion as the Windows raw-HID pads; rumble goes out through SDL_RumbleGamepad.
// Face buttons map by POSITION like the Windows XInput path (TOTK_PAD_LABELS=1: by label).
static std::mutex g_gp_mu;
static SDL_Gamepad* g_gp;
static std::atomic<bool> g_gp_rumble{false};
static float g_gp_acc[3] = {0, 0, -1};   // latest accelerometer sample in the Switch frame (g)
static Fusion g_gp_fus;
static Uint64 g_gp_gyro_ts;
static std::atomic<bool> g_gp_has_gyro{false};   // the open SDL gamepad has its own gyro (then the Deck IMU reader stays out)

static void gp_open_first() {   // g_gp_mu held; called at most once a second while no pad is open
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && !g_gp; i++) {
        SDL_Gamepad* g = SDL_OpenGamepad(ids[i]);
        if (!g) continue;
        g_gp = g;
        HidPad* pad = &g_hidpad[0];
        bool acc = SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL), gyr = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO);
        if (acc) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
        if (gyr) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        SDL_PropertiesID pr = SDL_GetGamepadProperties(g);
        g_gp_rumble = SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        { std::lock_guard<std::mutex> l(pad->mmu); pad->m = Motion{}; }
        g_gp_fus = Fusion{}; g_gp_gyro_ts = 0;
        g_gp_has_gyro = gyr;
        pad->has_imu = false; pad->live = true; pad->kind = PadPro;
        printf("[hid] SDL gamepad \"%s\" (type %d, rumble %s, accel %s, gyro %s)\n", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?",
               (int)SDL_GetGamepadType(g), g_gp_rumble ? "yes" : "no", acc ? "yes" : "no", gyr ? "yes" : "no");
    }
    SDL_free(ids);
}
static void gp_close_locked() {
    if (!g_gp) return;
    printf("[hid] SDL gamepad disconnected\n");
    SDL_CloseGamepad(g_gp);
    g_gp = nullptr; g_gp_rumble = false; g_gp_has_gyro = false;
    HidPad* pad = &g_hidpad[0];
    pad->live = false; pad->has_imu = false; pad->buttons = 0;
}
static void pad_read(HostInput* in) {
    static const bool labels = getenv("TOTK_PAD_LABELS") != nullptr;
    static bool inited, init_ok;
    static u64 next_scan;
    std::lock_guard<std::mutex> l(g_gp_mu);
    if (!inited) {
        inited = true;
        init_ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!init_ok) printf("[hid] SDL gamepad init failed (%s): keyboard only\n", SDL_GetError());
    }
    if (!init_ok) return;
    if (g_gp && !SDL_GamepadConnected(g_gp)) gp_close_locked();
    if (!g_gp) {   // look for a new pad once a second only
        u64 now = GetTickCount64();
        if (now < next_scan) return;
        next_scan = now + 1000;
        gp_open_first();
        if (!g_gp) return;
    }
    SDL_Gamepad* g = g_gp;
    auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
    auto lab = [&](SDL_GamepadButton b, u64 pos) -> u64 {   // TOTK_PAD_LABELS: the Switch button with the face button's printed letter
        switch (SDL_GetGamepadButtonLabel(g, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return BtnA; case SDL_GAMEPAD_BUTTON_LABEL_B: return BtnB;
        case SDL_GAMEPAD_BUTTON_LABEL_X: return BtnX; case SDL_GAMEPAD_BUTTON_LABEL_Y: return BtnY;
        default: return pos;
        }
    };
    struct { SDL_GamepadButton b; u64 pos; } face[] = {{SDL_GAMEPAD_BUTTON_EAST, BtnA}, {SDL_GAMEPAD_BUTTON_SOUTH, BtnB},
                                                        {SDL_GAMEPAD_BUTTON_NORTH, BtnX}, {SDL_GAMEPAD_BUTTON_WEST, BtnY}};
    for (auto& f : face) if (btn(f.b)) in->buttons |= labels ? lab(f.b, f.pos) : f.pos;
    static const struct { SDL_GamepadButton b; u64 sw; } map[] = {
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BtnL}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BtnR}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, BtnLS},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, BtnRS}, {SDL_GAMEPAD_BUTTON_START, BtnPlus}, {SDL_GAMEPAD_BUTTON_BACK, BtnMinus},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, BtnUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BtnDown}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BtnLeft},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BtnRight}};
    for (auto& m : map) if (btn(m.b)) in->buttons |= m.sw;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 3855) in->buttons |= BtnZL;    // 30 / 255 of the 0..32767 range (XInput threshold)
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 3855) in->buttons |= BtnZR;
    auto ax = [&](SDL_GamepadAxis a, bool flip) { int v = SDL_GetGamepadAxis(g, a); if (flip) v = -v; return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); };
    stick(ax(SDL_GAMEPAD_AXIS_LEFTX, false), ax(SDL_GAMEPAD_AXIS_LEFTY, true), dead_zone(0, 7849), &in->lx, &in->ly);      // SDL y is down, Npad y up
    stick(ax(SDL_GAMEPAD_AXIS_RIGHTX, false), ax(SDL_GAMEPAD_AXIS_RIGHTY, true), dead_zone(1, 8689), &in->rx, &in->ry);   // (XInput dead zones)
}
// Sensor events from the UI thread (shim_sdl3.c): accelerometer samples are kept, each gyroscope sample is fused with the latest one
// using the sensor's own timestamps. SDL frame (x right, y up, z toward the player; accel m/s^2, gyro rad/s) -> Switch frame as
// Eden/yuzu do: accel (-x, z, -y) / g, gyro (x, -z, y) / 2pi (rev/s).
extern "C" void totk_hid_sensor(int type, const float* d, Uint64 ts_ns) {
    if (!gyro_on()) return;
    if (type == SDL_SENSOR_ACCEL) {
        const float g = SDL_STANDARD_GRAVITY;
        g_gp_acc[0] = -d[0] / g; g_gp_acc[1] = d[2] / g; g_gp_acc[2] = -d[1] / g;
    } else if (type == SDL_SENSOR_GYRO) {
        float dt = g_gp_gyro_ts && ts_ns > g_gp_gyro_ts ? (float)((ts_ns - g_gp_gyro_ts) * 1e-9) : 0.005f;
        g_gp_gyro_ts = ts_ns;
        const float k = 1.0f / 6.2831853f;
        const float gyro[3] = {d[0] * k, -d[2] * k, d[1] * k};
        motion_fuse(&g_hidpad[0], &g_gp_fus, g_gp_acc, gyro, dt);
    }
}
// Steam Deck built-in IMU (user 2026-10-17: "add gyro for the steam deck"). Under Steam Input the game sees only Steam's virtual
// gamepad (SDL: "Steam Deck Controller", accel no, gyro no), but the controller's vendor HID interface (28de:1205, input2) keeps
// streaming its 64-byte deck state reports (01 00 09 40..., ~250 Hz) with raw accel / gyro while Steam runs (measured 2026-10-17
// over SSH: /dev/hidraw3, 124 reports in 0.5 s, accel 388 -1012 16439 lying flat; Steam's uaccess udev rule makes the node
// readable by the logged-in user). But while a game runs Steam switches the IMU off (w3 probe 11:35: 250 reports in 1 s, accel /
// gyro / quaternion all 0) unless the Steam layout uses gyro, so the reader turns it back on with the setting Steam and SDL use
// (feature report 0x87 ID_SET_SETTINGS_VALUES: SETTING_IMU_MODE 48 = SEND_RAW_ACCEL | SEND_RAW_GYRO | SEND_ORIENTATION, a
// runtime mode the controller forgets; SDL_hidapi_steam.c SetSensorsEnabled), again whenever the data stays all-zero for 0.5 s
// (at most every 2 s). TOTK_DECK_IMU=ro: never write (gyro then needs a gyro binding in the Steam layout). Scale and axes as
// SDL's HIDAPI Steam Deck driver (accel +-2 g, gyro +-2000 dps; SDL frame = (x, z, -y)) followed by totk_hid_sensor's SDL ->
// Switch conversion: the raw gyro axes land unchanged on the Switch axes, the accelerometer negated. Feeds pad 0 unless the open
// SDL gamepad has a gyro of its own. TOTK_DECK_IMU=0: off.
static bool deck_imu_enable(int fd) {
    unsigned char buf[65] = {};   // report id 0, then the 64-byte message (USB path of SDL's SetFeatureReport: always 65 bytes)
    buf[1] = 0x87; buf[2] = 3; buf[3] = 48; buf[4] = 0x08 | 0x10 | 0x04; buf[5] = 0;
    return ioctl(fd, HIDIOCSFEATURE(sizeof buf), buf) >= 0;
}
static int deck_imu_open(bool quiet) {
    DIR* d = opendir("/sys/class/hidraw");
    if (!d) return -1;
    int fd = -1;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "hidraw", 6) != 0) continue;
        char path[320], buf[1024];
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (!strstr(buf, "HID_ID=0003:000028DE:00001205")) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        static const bool ro = getenv("TOTK_DECK_IMU") && !strcmp(getenv("TOTK_DECK_IMU"), "ro");
        int h = ro ? -1 : open(path, O_RDWR | O_CLOEXEC);
        if (h < 0) h = open(path, O_RDONLY | O_CLOEXEC);
        if (h < 0) {
            if (!quiet) printf("[hid] Steam Deck IMU: %s: %s\n", path, strerror(errno));
            continue;
        }
        bool ok = false;   // the vendor interface is the one sending deck state reports
        for (int i = 0; i < 20 && !ok; i++) {
            pollfd p = {h, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) break;
            unsigned char r[64];
            ssize_t k = read(h, r, sizeof r);
            ok = k >= 36 && r[2] == 0x09;
        }
        if (ok) {
            fd = h;
            printf("[hid] Steam Deck IMU: %s (controller HID, read-only)\n", path);
            break;
        }
        close(h);
    }
    closedir(d);
    return fd;
}
static void deck_imu_thread() {
    Fusion fus;
    bool quiet = false;
    for (;;) {
        int fd = deck_imu_open(quiet);
        if (fd < 0) {
            if (!quiet) printf("[hid] Steam Deck IMU: no readable Steam Deck controller interface; looking again every 5 s\n");
            quiet = true;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        quiet = false;
        u64 last = 0, n = 0, zeros = 0, last_on = 0, ons = 0;
        unsigned char r[64];
        for (;;) {
            pollfd p = {fd, POLLIN, 0};
            int pr = poll(&p, 1, 1000);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;   // gone, or silent for 1 s (suspend): look again
            ssize_t k = read(fd, r, sizeof r);
            if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
            if (k < 36 || r[2] != 0x09) continue;
            timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            u64 now = (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
            float dt = last ? (float)((now - last) * 1e-9) : 0.004f;
            last = now;
            if (dt < 0.0005f || dt > 0.02f) dt = 0.004f;   // (reports come every ~4 ms; after a stall use the nominal step)
            if (g_gp_has_gyro) continue;
            s16 v[6];
            memcpy(v, r + 24, sizeof v);   // accel x y z, gyro x y z (int16) after the 4-byte header + packet number + buttons + pads
            if (!(v[0] | v[1] | v[2] | v[3] | v[4] | v[5])) {   // IMU off (a live accelerometer is never exactly 0 on all axes)
                if (++zeros >= 125 && now - last_on > 2000000000ull) {
                    last_on = now; zeros = 0;
                    bool ok = deck_imu_enable(fd);
                    if (++ons <= 3 || !(ons & (ons - 1)))
                        printf("[hid] Steam Deck IMU: data is off, switching it on (%llu): %s\n", (unsigned long long)ons, ok ? "ok" : strerror(errno));
                }
                continue;
            }
            zeros = 0;
            const float ka = 2.0f / 32768.0f, kg = 2000.0f / 32768.0f / 360.0f;
            const float acc[3] = {-v[0] * ka, -v[1] * ka, -v[2] * ka}, gyro[3] = {v[3] * kg, v[4] * kg, v[5] * kg};
            motion_fuse(&g_hidpad[0], &fus, acc, gyro, dt);
            if (++n == 1) printf("[hid] Steam Deck IMU: first sample accel %.2f %.2f %.2f g, gyro %.3f %.3f %.3f rev/s\n", acc[0], acc[1],
                                 acc[2], gyro[0], gyro[1], gyro[2]);
        }
        close(fd);
        printf("[hid] Steam Deck IMU: reports stopped after %llu samples; looking again\n", (unsigned long long)n);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
static void deck_imu_start() {
    const char* e = getenv("TOTK_DECK_IMU");
    if (!gyro_on() || (e && *e == '0')) return;
    if (access("/sys/class/hidraw", F_OK) != 0) return;
    std::thread(deck_imu_thread).detach();
}
#endif
static void rumble_start();   // below the vibration graph
static void hidpad_read(HostInput* in) {
#ifdef _WIN32
    static bool started = [] { std::thread(hidpad_scan_thread).detach(); rumble_start(); return true; }();
#else
    static bool started = [] { rumble_start(); deck_imu_start(); return true; }();
#endif
    (void)started;
    bool lset = in->lx || in->ly, rset = in->rx || in->ry;
    for (auto& p : g_hidpad) {
        if (!p.live) continue;
        in->buttons |= p.buttons.load();
        s32 lx = p.lx, ly = p.ly, rx = p.rx, ry = p.ry;
        if (!lset && (lx || ly)) { in->lx = lx; in->ly = ly; lset = true; }
        if (!rset && (rx || ry)) { in->rx = rx; in->ry = ry; rset = true; }
    }
}

extern "C" volatile int g_debug_frame_request;   // nvn/vulkan/totk_hooks.c (device.c dumps the next frame)
extern "C" volatile int g_skip_opening, g_title_phase, g_in_opening;   // hle_fs.cpp (skip-opening overlay; banner in hle_osd.cpp)
extern "C" volatile int g_title_stage;                                 // hle_fs.cpp: the title's stage is up
extern "C" { volatile uint64_t g_title_burst_end = 0; }                // TOTK_TITLE_FPS until this present (hle_vi.cpp)
static const uint64_t kTitleBurst = 240;
extern "C" { volatile int g_skip_hold_ms = 0; }                          // Minus held in the opening (hle_osd.cpp countdown)
static bool g_f2_down, g_f10_down;
extern "C" bool osd_menu_input(uint64_t buttons, bool key);   // hle_osd.cpp Graphics menu
extern "C" int osd_menu_is_open(void);
extern "C" int vk_precompile_warmup_active(void);   // nvn/vulkan/exec.c (TOTK_WARMUP until "play now")
extern "C" void vk_precompile_play_now(void);
// Restart this exe with TOTK_SKIP_OPENING=1 TOTK_AUTO_NEWGAME=1 (same command line, std handles and other env), then
// end this process at once (the normal exit path is not needed: the new process owns the window from here).
#ifdef _WIN32
static void skip_restart() {
    static bool done;
    if (done) return;
    done = true;
    printf("[hid] in-game skip: restarting into New Game with the opening skipped\n");
    // (pc_restart.cpp: under the launcher it starts the package again, so the new game stays watched; else this exe)
    static const char *const env[] = {"TOTK_SKIP_OPENING=1", "TOTK_AUTO_NEWGAME=1", "TOTK_PRESS=", nullptr};
    pc_restart_process(env);
    done = false;   // (still here: the new process could not be started)
}
#else
static void skip_restart() {
    static bool done;
    if (done) return;
    done = true;
    printf("[hid] in-game skip: restarting into New Game with the opening skipped\n");
    fflush(stdout); fflush(stderr);
    // same command line, std handles and other env: exec in place (the window and GPU state die with the old image)
    std::vector<std::string> args;
    if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
        std::string cur; int ch;
        while ((ch = fgetc(f)) != EOF) { if (ch) cur += (char)ch; else { args.push_back(cur); cur.clear(); } }
        fclose(f);
    }
    std::string exe = std::string(rt_posix_exe_dir()) + "/totk";
    char self[4096]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) { self[n] = 0; exe = self; }
    setenv("TOTK_SKIP_OPENING", "1", 1);
    setenv("TOTK_AUTO_NEWGAME", "1", 1);
    unsetenv("TOTK_PRESS");
    std::vector<char*> av;
    for (auto& a : args) av.push_back((char*)a.c_str());
    if (av.empty()) av.push_back((char*)exe.c_str());
    av.push_back(nullptr);
    execv(exe.c_str(), av.data());
    printf("[hid] in-game skip: exec failed (%d)\n", errno);
    done = false;
}
#endif
#ifdef _WIN32
extern "C" void pc_mouse_stick(int32_t* rx, int32_t* ry);   // pc_mouse.cpp
extern "C" void pc_mouse_stick_counts(int32_t* rx, int32_t* ry, double* cdx, double* cdy);
extern "C" bool pc_mouse_button_ok(int vk);                   // pc_mouse.cpp: a mouse button counts for the game now
// Key bindings. 24 actions: the 16 Npad buttons in bit order, then move left/up/right/down and camera left/up/right/down; each
// up to 4 keys or mouse buttons (Windows VK codes, so letters follow the keyboard layout). TOTK_KEYBOARD_LAYOUT: classic: the
// keys above (the engine's original table); Ctrl walks. pc (default): one key per button: E interact, Shift sprint / back,
// Space jump, left mouse attack, Ctrl crouch, V look, F abilities, R throw, Q target, right mouse bow (TOTK_MOUSE_AIM=1, the
// default: while it is held the left button shoots instead of attacking), Esc menu, M map, 2 / 1 / 3 / X d-pad left / up /
// right / down, WASD move, arrows camera. custom: pc, then TOTK_KBM_<ACTION>=Key,Key per action (A B X Y LSTICK RSTICK L R ZL
// ZR PLUS MINUS DPAD_LEFT DPAD_UP DPAD_RIGHT DPAD_DOWN MOVE_LEFT MOVE_UP MOVE_RIGHT MOVE_DOWN CAMERA_LEFT CAMERA_UP
// CAMERA_RIGHT CAMERA_DOWN, WALK); names: a letter or digit, Space Enter Esc Tab Backspace Shift LShift RShift Ctrl LCtrl
// RCtrl Alt LAlt RAlt Left Up Right Down F1..F12, Mouse Left / Right / Middle / X1 / X2, "vk 0x41", or none. Mouse buttons
// count only while TOTK_MOUSE_CAMERA has the pointer captured (pc_mouse_button_ok), so the click that captures it does not
// attack.
enum { kActions = 24, kBindMax = 4, kWalk = kActions };   // (kWalk: the walk modifier's slot, not an action bit)
struct Bindings { int vk[kActions + 1][kBindMax]; bool aim; };
static const char* const kActionNames[kActions + 1] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
    "MINUS", "DPAD_LEFT", "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "MOVE_LEFT", "MOVE_UP", "MOVE_RIGHT", "MOVE_DOWN", "CAMERA_LEFT",
    "CAMERA_UP", "CAMERA_RIGHT", "CAMERA_DOWN", "WALK"};
static int vk_from_name(const char* s) {
    static const struct { const char* n; int vk; } names[] = {{"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"tab", VK_TAB}, {"backspace", VK_BACK}, {"shift", VK_SHIFT},
        {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT}, {"ctrl", VK_CONTROL}, {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL},
        {"alt", VK_MENU}, {"lalt", VK_LMENU}, {"ralt", VK_RMENU}, {"left", VK_LEFT}, {"up", VK_UP}, {"right", VK_RIGHT},
        {"down", VK_DOWN}, {"mouse left", VK_LBUTTON}, {"mouse right", VK_RBUTTON}, {"mouse middle", VK_MBUTTON},
        {"mouse x1", VK_XBUTTON1}, {"mouse x2", VK_XBUTTON2}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"home", VK_HOME},
        {"end", VK_END}, {"insert", VK_INSERT}, {"delete", VK_DELETE},
#ifdef _WIN32
        {"comma", VK_OEM_COMMA}, {"period", VK_OEM_PERIOD},   // (by position: pc_input.cpp's kUsOem)
#endif
    };
    for (auto& n : names) if (!_stricmp(s, n.n)) return n.vk;
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    unsigned v = 0;
    if (!_strnicmp(s, "vk ", 3) && sscanf(s + 3, "%i", (int*)&v) == 1 && v > 0 && v < 256) return (int)v;
    return 0;
}
extern "C" int hid_vk_from_name(const char* s) { return vk_from_name(s); }   // pc_input.cpp: the scheme's key names
// Bumped when a controls setting changes at run time (the settings overlay's rows below); the input thread rebuilds its
// tables (bindings, mouse gyro, aim buttons, wheel) at its next read.
static std::atomic<int> g_ctl_gen{1};
// The tables from the environment (quiet: no log line; the glyph bank's lookups on the fs thread use their own copy)
static Bindings build_bindings(bool quiet) {
    Bindings r = {};
    auto set = [&](int a, std::initializer_list<int> vks) { int i = 0; for (int v : vks) if (i < kBindMax) r.vk[a][i++] = v; };
    const char* lay = getenv("TOTK_KEYBOARD_LAYOUT");
    if (!lay || !*lay) lay = "pc";   // default since the 2026-10-23 all-on pass; classic = the engine's original keys
    bool pc = !_stricmp(lay, "pc") || !_stricmp(lay, "custom"), custom = !_stricmp(lay, "custom");
    if (!pc) {   // classic: the fixed table this file had
        set(0, {'E', VK_RETURN}); set(1, {VK_SHIFT, VK_BACK}); set(2, {VK_SPACE}); set(3, {'F'}); set(4, {'V'}); set(5, {'G'});
        set(6, {'Q'}); set(7, {'R'}); set(8, {'Z'}); set(9, {'C'}); set(10, {VK_ESCAPE}); set(11, {'M'});
        set(12, {'1'}); set(13, {'2'}); set(14, {'3'}); set(15, {'4'});
        set(16, {'A'}); set(17, {'W'}); set(18, {'D'}); set(19, {'S'});
        set(20, {VK_LEFT, 'J'}); set(21, {VK_UP, 'I'}); set(22, {VK_RIGHT, 'L'}); set(23, {VK_DOWN, 'K'});
        set(kWalk, {VK_CONTROL});
    } else {
        // (one key per button: Backspace, C, middle mouse, Tab and 4 dropped)
        set(0, {'E'}); set(1, {VK_LSHIFT}); set(2, {VK_SPACE}); set(3, {VK_LBUTTON}); set(4, {VK_LCONTROL});
        set(5, {'V'}); set(6, {'F'}); set(7, {'R'}); set(8, {'Q'}); set(9, {VK_RBUTTON}); set(10, {VK_ESCAPE});
        set(11, {'M'}); set(12, {'2'}); set(13, {'1'}); set(14, {'3'}); set(15, {'X'});
        set(16, {'A'}); set(17, {'W'}); set(18, {'D'}); set(19, {'S'});
        set(20, {VK_LEFT}); set(21, {VK_UP}); set(22, {VK_RIGHT}); set(23, {VK_DOWN});
    }
    for (int a = 0; custom && a <= kActions; a++) {
        char var[48]; snprintf(var, sizeof var, "TOTK_KBM_%s", kActionNames[a]);
        const char* v = getenv(var);
        if (!v) continue;
        int n = 0, vks[kBindMax] = {};
        for (const char* p = v; *p;) {
            const char* e = strchr(p, ',');
            std::string tok(p, e ? (size_t)(e - p) : strlen(p));
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (!tok.empty() && _stricmp(tok.c_str(), "none")) {
                int vk = vk_from_name(tok.c_str());
                if (!vk) printf("[hid] %s: unknown key \"%s\"\n", var, tok.c_str());
                else if (n < kBindMax) vks[n++] = vk;
            }
            if (!e) break;
            p = e + 1;
        }
        memcpy(r.vk[a], vks, sizeof vks);
    }
    const char* aim = getenv("TOTK_MOUSE_AIM");
    r.aim = !aim || strcmp(aim, "0") != 0;
    if (!quiet) printf("[hid] keyboard layout %s%s\n", custom ? "custom" : pc ? "pc" : "classic", pc && r.aim ? " (mouse aim on)" : "");
    return r;
}
static const Bindings& bindings() {
    static Bindings b;
    static int built;
    if (built != g_ctl_gen.load()) b = build_bindings(false);
    built = g_ctl_gen.load();
    return b;
}
// The mouse as the gyro. With TOTK_MOUSE_CAMERA and no pad that has motion, TOTK_MOUSE_GYRO=aim (off, the default until a boot with a
// bow checks it; always) makes the mouse turn a virtual controller while an aim button is held (aim_mask: the bow drawn, a throw), so
// TotK's motion aiming gets the mouse's precision: x turns it about its z axis (yaw), y about its x axis (pitch), TOTK_MOUSE_GYRO_SPEED
// (default 1) x 0.002 rad per count x TOTK_MOUSE_SENS. The orientation is integrated here and the acceleration fed with it is gravity in
// that orientation, so motion_fuse's tilt correction agrees and an aim stays put. Meanwhile the mouse does not drive the right stick.
// Released, the virtual controller is gone (lying still) and starts level again at the next aim.
extern "C" bool pc_mouse_take(double* dx, double* dy);   // pc_mouse.cpp
extern "C" bool pc_mouse_enabled(void);
// The mouse settings the input thread reads, rebuilt when g_ctl_gen moves (the settings overlay).
struct MouseCfg { int gyro_mode = 0; u64 aim = BtnZR | BtnR; double gyro_speed = 1; bool wheel = true, map = false, menus = false; };
static std::atomic<bool> g_mouse_map{false};   // mouse_cfg().map, for other threads (hid_mouse_map_on)
static const MouseCfg& mouse_cfg() {
    static MouseCfg m;
    static int built;
    if (built == g_ctl_gen.load()) return m;
    built = g_ctl_gen.load();
    const char* e = getenv("TOTK_MOUSE_GYRO");
    m.gyro_mode = !e || !*e ? 0 : !_stricmp(e, "always") ? 2 : !_stricmp(e, "off") || !strcmp(e, "0") ? 0 : 1;   // 0 off, 1 aim, 2 always
    // The aim states, as the buttons that hold them: TOTK_MOUSE_GYRO_ON=ZR,R (default: ZR draws the bow, R held aims a
    // throw; TotK's motion aiming covers both). Ultrahand and the other abilities aim with the camera (the stick path).
    m.aim = BtnZR | BtnR;
    if (const char* on = getenv("TOTK_MOUSE_GYRO_ON")) {
        m.aim = 0;
        for (const char* p = on; *p;) {
            const char* c = strchr(p, ',');
            std::string t(p, c ? (size_t)(c - p) : strlen(p));
            for (auto& ch : t) ch = (char)toupper((unsigned char)ch);
            for (auto& nm : kPressNames) if (t == nm.n && nm.bit < 16) m.aim |= 1ull << nm.bit;
            if (!c) break;
            p = c + 1;
        }
        printf("[hid] mouse gyro aims while %s is held\n", on);
    }
    const char* s = getenv("TOTK_MOUSE_GYRO_SPEED");
    m.gyro_speed = s && *s ? fmax(0.05, atof(s)) : 1.0;
    const char* w = getenv("TOTK_MOUSE_WHEEL");
    m.wheel = !w || strcmp(w, "0") != 0;
    const char* mp = getenv("TOTK_MOUSE_MAP");  // the map with the mouse (screen_track below); on unless 0
    m.map = !mp || strcmp(mp, "0") != 0;
    g_mouse_map.store(m.map);
    const char* mk = getenv("TOTK_MENU_KEYS");   // tabs on the wheel / PageUp / PageDown, Esc = back; off unless 1
    m.menus = mk && *mk && strcmp(mk, "0") != 0;
    return m;
}
static int mouse_gyro_mode() { return mouse_cfg().gyro_mode; }
static u64 aim_mask() { return mouse_cfg().aim; }
static bool real_imu_live() { for (auto& p : g_hidpad) if (p.live && p.has_imu) return true; return false; }
static void mouse_gyro_stop() {
    if (!g_mousepad.live) return;
    g_mousepad.live = false; g_mousepad.has_imu = false;
    printf("[hid] mouse gyro off\n");
}
static void mouse_gyro_feed(double dx, double dy) {
    static double R[3][3];   // rows: the world's reference axes seen in controller coordinates
    static Fusion fusion;
    static LARGE_INTEGER last;
    const double speed = mouse_cfg().gyro_speed;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (!g_mousepad.live) {   // start lying face up, level
        memset(R, 0, sizeof R); R[0][0] = R[1][1] = R[2][2] = 1;
        fusion = Fusion();
        { std::lock_guard<std::mutex> l(g_mousepad.mmu); g_mousepad.m = Motion(); }
        last.QuadPart = 0;
        g_mousepad.live = true;
        printf("[hid] mouse gyro on (aiming with the mouse)\n");
    }
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart : 0;
    last = now;
    if (!(dt > 0) || dt > 0.05) dt = 0.004;
    double k = 0.002 * speed;   // rad per count (sensitivity applied by pc_mouse_take)
    double w[3] = {-dy * k / dt, 0, -dx * k / dt};   // rad/s: right = yaw right (-z), down = pitch down (-x)
    for (int r = 0; r < 3; r++) {   // fixed reference axes seen from the turning controller: r' = r - (w x r) dt
        double* v = R[r], cr[3] = {w[1] * v[2] - w[2] * v[1], w[2] * v[0] - w[0] * v[2], w[0] * v[1] - w[1] * v[0]};
        for (int i = 0; i < 3; i++) v[i] -= cr[i] * dt;
        double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; i++) v[i] /= n;
    }
    const float acc[3] = {(float)-R[2][0], (float)-R[2][1], (float)-R[2][2]};   // gravity, read as -up (face up: z = -1 g)
    const float gyro[3] = {(float)(w[0] / 6.2831853), (float)(w[1] / 6.2831853), (float)(w[2] / 6.2831853)};   // rev/s
    motion_fuse(&g_mousepad, &fusion, acc, gyro, (float)dt);
}
// TOTK_CTL "key <name> down|up|<seconds>": keys and mouse buttons by the bindings' names, held as if pressed (ctl_apply;
// read in keyboard_read even with the window behind others).
static std::atomic<u64> g_key_inj[4];   // bit per VK code
static ULONGLONG g_key_until[256];      // ctl thread (host_input's lock): timed presses' release ticks, 0 = held / none
static bool key_injected(int vk) { return vk > 0 && vk < 256 && (g_key_inj[vk >> 6].load() >> (vk & 63) & 1); }
static bool key_injected_any() { return g_key_inj[0].load() | g_key_inj[1].load() | g_key_inj[2].load() | g_key_inj[3].load(); }
static void key_inject(int vk, bool on) {
    if (on) g_key_inj[vk >> 6].fetch_or(1ull << (vk & 63)); else g_key_inj[vk >> 6].fetch_and(~(1ull << (vk & 63)));
}
static void key_inject_tick() {   // timed presses that ended
    ULONGLONG now = GetTickCount64();
    for (int vk = 1; vk < 256; vk++) if (g_key_until[vk] && now >= g_key_until[vk]) { g_key_until[vk] = 0; key_inject(vk, false); }
}
// The screen the game shows, for the map with the mouse and the menu keys. A pause menu or the Purah Pad holds the
// picture: no 3D scene is drawn (exec.c vk_scene_idle_frames >= 3; gameplay shows 0-1, dpad_gate reads the same), and
// the press just before tells which: Minus the Purah Pad (it opens on the map), Plus the pause menu (inventory), neither
// something else (a quick menu: wheel_read's own case, a loading screen). While one is open, Plus / Minus switch to the
// other. The scene drawn again for 250 ms = back in the game, so a wrong guess lasts only until the screen closes. Fed
// the buttons the game saw (host_input), read by keyboard_read at the next refresh. When the world goes through the
// docked upscale (exec.c vk_upscale_idle_frames, under 30 presents old as the screen is left) only that upscale counts
// as the scene until the game is back: a busy menu also passes exec.c's 1000-draw test (the zoomed-out Surface map with
// its icons, an inventory tab switch). Handheld or wherever the upscale never ran: the plain scene test.
enum Screen { ScrGame, ScrPad, ScrMenu, ScrOther };
static std::atomic<int> g_screen{ScrGame};
// totk_menu_cursor.cpp: what screen_track says the game shows: 0 game, 1 Purah Pad (the map), 2 pause menu, 3 other (its
// cursor stays off on the map, where the mouse drags it)
extern "C" int hid_screen_now(void) { return g_screen.load(); }
// TOTK_MOUSE_MAP in force (the map's mouse drag), from any thread: the input thread's last read of it (no getenv racing
// the settings thread's _putenv)
extern "C" bool hid_mouse_map_on(void) { return g_mouse_map.load(); }
extern "C" uint64_t vk_scene_idle_frames(void);   // nvn/vulkan/exec.c: presents since the 3D scene was last drawn
extern "C" uint64_t vk_upscale_idle_frames(void);   // since the docked upscale last drew (~0: never)
static void screen_track(u64 buttons) {
    static u64 prev, minus_t, plus_t, drawn_since;
    static bool by_up;   // the world goes through the upscale (decided while in the game)
    static int logs;
    static const char* const names[] = {"game", "Purah Pad", "pause menu", "other"};
    const u64 now = GetTickCount64(), edge = buttons & ~prev;
    prev = buttons;
    if (edge & BtnMinus) minus_t = now;
    if (edge & BtnPlus) plus_t = now;
    const int s = g_screen.load();
    int to = s;
    const u64 up = vk_upscale_idle_frames();
    if (s == ScrGame) by_up = up < 30;
    const u64 idle = by_up ? up : vk_scene_idle_frames();
    if (idle >= 3) {
        drawn_since = 0;
        if (s == ScrGame) {
            const bool m = minus_t && now - minus_t < 2000, p = plus_t && now - plus_t < 2000;
            to = m && (!p || minus_t > plus_t) ? ScrPad : p ? ScrMenu : ScrOther;
        } else if (s == ScrPad && (edge & BtnPlus)) to = ScrMenu;
        else if (s == ScrMenu && (edge & BtnMinus)) to = ScrPad;
    } else if (s != ScrGame) {
        if (!drawn_since) drawn_since = now;
        if (now - drawn_since >= 250) to = ScrGame;
    }
    if (to == s) return;
    g_screen = to;
    if (logs++ < 200)
        printf("[hid] screen: %s -> %s (%s idle %llu frames)\n", names[s], names[to], by_up ? "upscale" : "scene",
               (unsigned long long)idle);
}
// The mouse's share of the pad: the gyro while aiming (TOTK_MOUSE_GYRO), else right-stick deflection (TOTK_MOUSE_CAMERA);
// on the map with TOTK_MOUSE_MAP the left stick from its motion (4.29: as WASD, the reticle stays at the centre).
// aiming: an aim button (aim_mask) held on the keys / mouse, a pad without motion (in->buttons) or TOTK_CTL.
extern "C" bool pc_mouse_injected(void);   // pc_mouse.cpp: TOTK_CTL "mouse" motion in the last second
extern "C" int pc_mouse_wheel_take(void);
extern "C" void pc_mouse_menu(bool on);
// The mouse wheel, one step per notch: 70 ms of input, then 70 ms of none so the game sees separate steps.
// - In a quick menu (TOTK_MOUSE_WHEEL, on unless 0): while one is held open (d-pad right: weapons, left: shields, up:
//   special items; ZR + right: bows), a right-stick flick through the list, down = right = next (4.22: Wheel Down =
//   weapon_next on every kind). The L ability wheel steps by notch in the executor.
// - In the Purah Pad (TOTK_MOUSE_MAP): the map's zoom, a right-stick flick, up = in (120 ms: the zoom moves while held).
// - In the pause menu (TOTK_MENU_KEYS): the tabs, L / R, up = the previous one.
// Elsewhere notches are dropped.
extern "C" bool pc_mouse_cursor_on(void);  // pc_mouse.cpp: the cursor mode
extern "C" bool pc_mouse_cursor_on_item(void);  // pc_mouse.cpp: the pointer is over one of the items
extern "C" void pc_mouse_cursor_mode(bool on);
extern "C" unsigned pc_mouse_buttons(void);   // pc_mouse.cpp: logical buttons down in the client area (bit 0 left)
extern "C" void pc_mouse_ui(bool on);         // pc_mouse.cpp: a UI context: the pointer released as for the menu cursor
extern "C" void pc_mouse_cursor_stick(int32_t* rx, int32_t* ry, double* cdx, double* cdy);   // the pointer's motion as a stick
extern "C" bool pc_mouse_client_pos(float* nx, float* ny);
// The map page in front, for its mouse (the wheel's zoom, the drag, the click): with the PC scheme, its context, which
// the census names however the map was opened; else screen_track's Purah Pad guess. The guess needs the game to see a
// - press, and M's menu chain (PCOP_MENU_GOTO, 4.25 3c) presses it through the own npad buttons, which screen_track
// never sees: the map read "other" and its zoom, 1536: "[hid] screen: game -> other" with "pc_ctx: world -> map
// (menu)"). The Scope and the Purah Pad camera also read as the Pad: the mouse is their camera's look there, not the
// map's.
static bool map_screen() {
    if (pc_scheme_on()) {
        const int c = pc_ctx();
        if (c == PCX_MAP) return true;
        if (c == PCX_SCOPE || c == PCX_CAMERA) return false;
    }
    return g_screen.load() == ScrPad;
}
// (the PC scheme's router takes the notches first in keyboard_read and leaves here what its table does not use)
static int g_wheel_left;
static bool g_wheel_given;
static void wheel_read(HostInput* in, u64 buttons) {
    static int pending, dir, phase, mode;   // phase 0 idle, 1 input, 2 rest; mode 1 quick, 2 zoom, 3 tabs
    static ULONGLONG t;
    const int n = g_wheel_given ? g_wheel_left : pc_mouse_wheel_take(), scr = g_screen.load();
    g_wheel_given = false;
    g_wheel_left = 0;
    const MouseCfg& mc = mouse_cfg();
    // (the pause menu's tabs also while the menu cursor is on: its wheel, whatever TOTK_MENU_KEYS says)
    const int m = map_screen() && mc.map ? 2 : scr == ScrMenu && (mc.menus || pc_mouse_cursor_on()) ? 3
                : mc.wheel && (buttons & (BtnLeft | BtnRight | BtnUp)) ? 1 : 0;
    if (m != mode) { pending = 0; phase = 0; mode = m; }
    if (!m) return;
    pending += n;
    const ULONGLONG now = GetTickCount64(), len = m == 2 ? 120 : 70;
    if (phase == 0 && pending) { dir = pending > 0 ? 1 : -1; pending -= dir; phase = 1; t = now; }
    if (phase == 1) {
        if (m == 1) { in->rx = -dir * 32767; in->ry = 0; }   // (a notch down = right = next, as the world's weapon_next)
        else if (m == 2) { in->rx = 0; in->ry = dir * 32767; }
        else in->buttons |= dir > 0 ? BtnL : BtnR;
        if (now - t >= len) { phase = 2; t = now; }
    } else if (phase == 2) { if (m != 3) in->rx = in->ry = 0; if (now - t >= 70) phase = 0; }
}
// The map's left button (TOTK_MOUSE_MAP): held and moved past 20 counts it drags the map, grab-style (the map follows
// the pointer: the left stick, which moves the view, the other way); released without that it is a click, A for 100 ms
// (a stamp / pin at the cursor, which stays at the map's centre, or the choice in a popup). The mouse without the button
// does nothing there. keyboard_read follows the button, mouse_read the motion (both on the input thread).
static struct { bool down, dragged; double moved; ULONGLONG a_until; int reads; s32 peak; } g_map_btn;   // (reads, peak: the log)
// The mouse moved on purpose (pc_device_switch.h MotionGate: over 40 counts within 0.25 s of the first, while captured;
// released, pc_mouse.cpp's pointer test): the prompts go to keyboard / mouse. The veneer turns the camera from the counts
// and leaves the stick at rest, so host_input's look at the sample never sees it.
static bool g_mouse_moved;   // (input thread) set here, taken by host_input
static void mouse_motion_note(double cx, double cy) {
    static pcdev::MotionGate gate;
    if (gate.add(fabs(cx) + fabs(cy), (double)GetTickCount64())) g_mouse_moved = true;
}
static void mouse_read(HostInput* in, bool aiming) {
    wheel_read(in, g_ctl_held.load() | in->buttons);
    // The map (not the Scope's nor the Purah Pad camera's look: map_screen): the mouse moves it as WASD does. The game's
    // reticle stays at the centre; the mouse's motion is the left stick, its speed the deflection (pc_mouse_stick_counts:
    // the mouse sensitivity row, the menus' uninverted Y, its small-motion floor over the stick's dead zone), so the map
    // scrolls as far as the mouse moved. The pointer stays captured there (keyboard_read: no UI release on the map), so it
    // never leaves the window nor turns the camera. Still for 25 ms: nothing, so no smoothing tail scrolls on after the
    // mouse stops; WASD (set before) is kept while the mouse is still. No free cursor, no drag (doc 4.29).
    if (map_screen() && mouse_cfg().map) {
        mouse_gyro_stop();
        int32_t x = 0, y = 0;
        double cx = 0, cy = 0;
        if (!pc_mouse_cursor_on()) pc_mouse_stick_counts(&x, &y, &cx, &cy);   // (released: no counts, the map stays)
        mouse_motion_note(cx, cy);
        static ULONGLONG moved_at;
        const ULONGLONG now = GetTickCount64();
        if (cx != 0 || cy != 0) moved_at = now;
        if ((x || y) && now - moved_at <= 25) {
            in->lx = x; in->ly = y;   // (right = D, up = W: stick +y is up, as pc_mouse_stick_counts gives it)
            static int logs;
            if (logs++ < 20) printf("[hid] map: the mouse moves it as the stick (%d, %d)\n", (int)x, (int)y);
        }
        return;
    }
    aiming = aiming || ((g_ctl_held.load() | in->buttons) & aim_mask());
    if (pc_mouse_enabled() && gyro_on() && mouse_gyro_mode() && (mouse_gyro_mode() == 2 || aiming) && !real_imu_live()) {
        double dx, dy;
        if (pc_mouse_take(&dx, &dy)) { mouse_gyro_feed(dx, dy); mouse_motion_note(dx, dy); }   // TOTK_MOUSE_GYRO: the mouse turns the controller
    } else {
        mouse_gyro_stop();
        double cx, cy;
        pc_mouse_stick_counts(&in->rx, &in->ry, &cx, &cy);   // TOTK_MOUSE_CAMERA (pc_mouse.cpp): a moving mouse wins over the stick
        mouse_motion_note(cx, cy);
    }
}
static u64 jump_swap(u64 held) {   // (P5, the PC scheme: Space jumps, Shift sprints; the game's Jump/Sprint option)
    if (!g_jump_swapped.load() || pc_ctx_is_ui(pc_ctx()) || pc_ctx() == PCX_OVERLAY) return held;
    const u64 b = held >> 1 & 1, x = held >> 2 & 1;
    return (held & ~(BtnB | BtnX)) | x << 1 | b << 2;
}
// The move keys' minimum hold (pc_key_hold.h: a quick W / A / D tap holds its direction so Link's walk start takes a
// step instead of sliding; the walkstart.md): TOTK_KEY_MIN_HOLD_MS (controls.key_min_hold_ms), default 150, 0 = off,
// at most 1000; read again when g_ctl_gen moves (the settings rows).
static unsigned key_min_hold_ms() {
    static unsigned ms = 150;
    static int built;
    if (built == g_ctl_gen.load()) return ms;
    built = g_ctl_gen.load();
    const char* e = getenv("TOTK_KEY_MIN_HOLD_MS");
    ms = e && *e ? (unsigned)fmin(fmax(atof(e), 0.0), 1000.0) : 150;
    return ms;
}
static pckh::Hold g_key_hold;   // input thread (keyboard_read)
// The keyboard's state for the input reads, sampled once a millisecond on a thread of its own (keysnap_thread): every
// input read used to ask Windows key by key (GetAsyncKeyState), and the PC scheme's router walks every key several times
// a read, so ~800-1300 calls into win32k per read, 1000 reads a second, on the game's own threads (every npad poll waits
// on a read). Behind a mouse at 4000-8000 Hz, whose raw input holds win32k's lock that often, each call waited, game
// threads 'other' 150-350 ms a frame, with Razer's software closed (the mouse at its own rate; 60 fps with it open), as
// the r/decomps reporter's. Now the reads only look at the snapshot; the sampler alone waits on win32k. The mouse
// buttons stay the window's messages (pc_mouse_buttons). TOTK_KEYSNAP=0: each read asks Windows itself, as before (A/B).
static std::atomic<u64> g_keysnap[4];
static std::atomic<bool> g_keysnap_front{false};
static bool keysnap_on() {
    static const bool on = [] {
        const char* e = getenv("TOTK_KEYSNAP");
        return !(e && !strcmp(e, "0"));
    }();
    return on;
}
// TOTK_KEYS_FRONT=1 (tests): the keys are read as if the game were in front, the cost an A/B of TOTK_KEYSNAP needs to
// see (behind other windows a read asks nothing)
static bool game_in_front() {
    static const bool forced = getenv("TOTK_KEYS_FRONT") && !strcmp(getenv("TOTK_KEYS_FRONT"), "1");
    if (forced) return true;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}
static void keysnap_thread() {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    printf("[hid] keyboard sampled every 1 ms on its own thread (%s timer); TOTK_KEYSNAP=0: in each input read\n",
           t ? "high-resolution" : "Sleep");
    double busy = 0, worst = 0;
    unsigned n = 0, lines = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (t) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;   // 1 ms, relative
            if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t, INFINITE);
            else Sleep(1);
        } else {
            Sleep(1);
        }
        const auto s0 = std::chrono::steady_clock::now();
        const bool front = game_in_front();
        u64 w[4] = {};
        if (front)
            for (int vk = 1; vk < 255; vk++) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;   // (the window's messages)
                if (GetAsyncKeyState(vk) & 0x8000) w[vk >> 6] |= 1ull << (vk & 63);
            }
        for (int i = 0; i < 4; i++) g_keysnap[i].store(w[i], std::memory_order_relaxed);
        g_keysnap_front.store(front, std::memory_order_release);
        const auto s1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(s1 - s0).count();
        busy += us; n++; if (us > worst) worst = us;
        if (std::chrono::duration<double>(s1 - t0).count() >= 10.0) {   // (the first minute, and any 10 s with a 5 ms sample)
            if (lines < 6 || (worst >= 5000 && lines < 100)) {
                lines++;
                printf("[hid] keyboard sampler: %u samples in 10 s, mean %.0f us, slowest %.1f ms%s\n", n, busy / n,
                       worst / 1000.0, front ? "" : " (behind other windows: not asked)");
            }
            busy = worst = 0; n = 0; t0 = s1;
        }
    }
}
static bool keysnap_down(int vk) {
    return vk > 0 && vk < 256 && (g_keysnap[vk >> 6].load(std::memory_order_relaxed) >> (vk & 63) & 1);
}
static void keyboard_read(HostInput* in) {
    const Bindings& bd = bindings();   // (first: its layout / unknown-key lines reach the log of any boot)
    static const bool snap = keysnap_on() && (std::thread(keysnap_thread).detach(), true);
    const bool front = snap ? g_keysnap_front.load(std::memory_order_acquire) : game_in_front();
    const bool injected = key_injected_any();   // TOTK_CTL "key": unattended tests of the bindings
    // the PC scheme's context, once per read: the overlay, the title, the screen, else screen_track's guess (the pause
    // menu / the Purah Pad), else the world
    {
        const int scr = g_screen.load();
        const u64 seen = g_cheat_keys.load();   // (L held: the ability wheel; d-pad left / up / right held: a strip)
        const int quick = seen & BtnL ? 0 : seen & (BtnLeft | BtnUp | BtnRight) ? 1 : -1;
        const int c = pc_ctx_decide(pc_overlay_open(), g_title_phase != 0, totk_menu_screen(), scr == ScrPad ? 1 : scr == ScrMenu ? 2 : 0, quick);
        // (the capture policy's input: menus show the pointer; not the map with its mouse on: the mouse moves it as the
        // stick there, captured, 4.29)
        pc_mouse_ui(pc_scheme_on() && pc_ctx_is_ui(c) && !(c == PCX_MAP && mouse_cfg().map));
    }
    // game window not in front: only TOTK_CTL's keys and mouse (unattended tests; the real keyboard is the front window's)
    // (and while the map's button is held: its release must be seen) (the PC scheme's router still reads, every key up, so
    // it never saw Enter go up nor the title come, held the first Enter as carried into the title (dead) and every later
    // one as still held; its pulses and macros also play out from here)
    const bool idle = !front && !injected && !pc_mouse_injected() && !g_map_btn.down;
    if (idle && !pc_scheme_on()) return;
    // The left / right / middle buttons logical (the window's button messages: Windows' swap applied once GetAsyncKeyState
    // reads the physical ones); the side buttons are not swapped.
    auto down = [front](int vk) {
        if (key_injected(vk)) return true;
        if (!front) return false;
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON)
            return (pc_mouse_buttons() & (vk == VK_LBUTTON ? 1u : vk == VK_RBUTTON ? 2u : 4u)) != 0;
        if (snap) return keysnap_down(vk);   // (the sampler's snapshot: no win32k call in the read)
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    auto mouse_vk = [](int vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2; };
    auto key = [&](int vk) { return vk && down(vk) && (!mouse_vk(vk) || key_injected(vk) || pc_mouse_button_ok(vk)); };
    // The PC scheme: the context's table first; the layout reads only what it leaves
    PcRouteOut ro;
    const bool pcs = pc_scheme_on();
    // the map's 'E' rows are read below: the router asks at their press whether they act (pc_external_register, the
    // prompt audit's runtime half; 4.29's map click was one that did not)
    static const bool ext_registered = (pc_external_register("hle_hid (the map's mouse)", [](int c, int vk) {
        return c == PCX_MAP && vk == VK_LBUTTON && mouse_cfg().map && map_screen();
    }), true);
    (void)ext_registered;
    if (pcs) {
        struct Ctx { decltype(key)* key; const Bindings* bd; } kc = {&key, &bd};
        PcRouteIn ri;
        ri.ctx = pc_ctx();
        ri.now_ms = GetTickCount64();
        ri.user = &kc;
        ri.layout_bits = [](int vk, void* u) {   // (the layout's actions bound to vk: a transition's replayed tap)
            const Bindings& b = *((Ctx*)u)->bd;
            u64 bits = 0;
            for (int a = 0; a < kActions; a++) for (int s = 0; s < kBindMax; s++) if (b.vk[a][s] == vk) bits |= 1ull << a;
            return (uint64_t)bits;
        };
        ri.down = [](int vk, void* u) {
            auto& k = *((Ctx*)u)->key;
            return k(vk) || ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) && pc_mouse_cursor_on() &&
                             (pc_mouse_buttons() & (vk == VK_LBUTTON ? 1u : vk == VK_RBUTTON ? 2u : 4u)) != 0);
        };
        ri.wheel = pc_mouse_wheel_take();
        ri.gen = g_ctl_gen.load();
        ri.frame = nvn_present_count();
        static bool behind;   // (the last read was with the window behind others)
        ri.refocus = front && behind;
        behind = !front;
        pc_route(ri, &ro);
        g_wheel_left = ro.wheel_taken ? 0 : ri.wheel;   // (not the table's: wheel_read's own uses, the map's zoom)
        g_wheel_given = true;
    }
    if (idle) {   // (behind other windows: only the router's outputs, no key or mouse reads)
        pckh::cancel(g_key_hold);   // (no move key held past a focus loss)
        in->buttons |= jump_swap(ro.bits) & 0xFFFFull;
        if (ro.stick[0]) { in->lx = ro.sx[0]; in->ly = ro.sy[0]; }
        if (ro.stick[1]) { in->rx = ro.sx[1]; in->ry = ro.sy[1]; }
        return;
    }
    u64 held = 0;
    for (int a = 0; a <= kActions; a++)
        for (int s = 0; s < kBindMax; s++) {
            const int vk = bd.vk[a][s];
            if (key(vk) && !(pcs && vk < 256 && ro.claimed[vk])) { held |= 1ull << a; break; }
        }
    {   // Menu screens (screen_track): TOTK_MOUSE_MAP in the Purah Pad, TOTK_MENU_KEYS there and in the pause menu (the
        // console layout; the PC scheme has Esc / Q / E in its table)
        const int scr = g_screen.load();
        const MouseCfg& mc = mouse_cfg();
        // (the Scope and the Purah Pad camera read as ScrPad too: their mouse is the camera's, veneer and inversion kept)
        // (map_screen: the map by the PC scheme's context, or the Pad guess; never the Scope's / the camera's look)
        const bool map = mc.map && map_screen(), menu = !pcs && mc.menus && (scr == ScrPad || scr == ScrMenu);
        pc_mouse_menu(map || menu || (pcs && pc_ctx_is_ui(pc_ctx())));
        auto drop = [&](int vk) {   // vk's bindings' actions give way
            for (int a = 0; a < kActions; a++) for (int s = 0; s < kBindMax; s++) if (bd.vk[a][s] == vk) held &= ~(1ull << a);
        };
        // A key keeps the meaning it was pressed with: held while the screen changes it does nothing until released (Esc
        // held as + while the pause menu opens must not close it again as B, nor reopen it as + once it has closed).
        static int8_t pressed_as[256];   // 0 up, 1 its bindings, 2 the menu's button
        auto steady = [&](int vk, bool as_menu) {   // vk down with the meaning it has now (else its bindings dropped)
            if (!key(vk)) { pressed_as[vk] = 0; return false; }
            const int8_t as = as_menu ? 2 : 1;
            if (!pressed_as[vk]) pressed_as[vk] = as;
            if (pressed_as[vk] == as) return true;
            drop(vk);
            return false;
        };
        auto instead = [&](int vk, bool on, u64 btn) { if (steady(vk, on) && on) { drop(vk); held |= btn; } };
        const bool clicks = map || (menu && scr == ScrMenu);
        bool l;
        if (map && pc_mouse_cursor_on()) {
            // Cursor mode: the left button is read here, the bindings do not see it while the pointer is released. A
            // press that starts on one of the items (the Pad's tabs and lists: its click) or outside the window is not
            // the map's; its right click stays the B.
            static bool was, ignored;
            float nx, ny;
            // (logical: the window's button messages, Windows' swap applied GetAsyncKeyState is physical)
            const bool dn = key_injected(VK_LBUTTON) || (front && (pc_mouse_buttons() & 1) != 0);
            if (dn && !was) ignored = pc_mouse_cursor_on_item() || !pc_mouse_client_pos(&nx, &ny);
            was = dn;
            l = dn && !ignored;
        } else l = steady(VK_LBUTTON, clicks) && clicks;
        // the map: the left button is A at the centre's reticle while held, as the pad's (4.29: no click-to-place nor drag,
        // the mouse's motion moves the map, mouse_read); g_map_btn.down keeps its release read behind other windows
        g_map_btn.down = map && l;
        if (l) { drop(VK_LBUTTON); held |= BtnA; }
        // right click = B, unless the PC scheme routed it
        if (!(pcs && ro.claimed[VK_RBUTTON])) instead(VK_RBUTTON, clicks, BtnB);
        // Esc backs out (B; Tab stays +), PageUp / PageDown the previous / next tab (L / R)
        instead(VK_ESCAPE, menu, BtnB);
        instead(VK_PRIOR, menu, BtnL);
        instead(VK_NEXT, menu, BtnR);
    }
    if (pcs) held |= ro.bits;
    if (pcs) held = jump_swap(held);
    const bool aiming = (held & aim_mask()) != 0;   // an aim binding held (the mouse gyro's "aim" mode)
    // Aim: a ZR key or button held draws the bow; the left mouse button then shoots (ZR released) instead of doing its
    // own action (not where the PC scheme's table has the left button: the aim macro owns ZR and fires on its edges)
    if (bd.aim && (held & BtnZR) && key(VK_LBUTTON) && !(pcs && ro.claimed[VK_LBUTTON])) {
        held &= ~BtnZR;
        for (int a = 0; a < kActions; a++) for (int s = 0; s < kBindMax; s++) if (bd.vk[a][s] == VK_LBUTTON) held &= ~(1ull << a);
    }
    in->buttons |= held & 0xFFFFull;
    // F9: capture the next frame (pass dumps + [draw] log, as LARECOMP_DEBUG_FRAME does), for bugs seen while playing
    static bool f9_was = false;
    bool f9 = down(VK_F9) && (!pc_overlay_enabled() || down(VK_SHIFT));   // F9 is the perf overlay with NXR_PC_OVERLAY=1: Shift+F9
    if (f9 && !f9_was) {
        g_debug_frame_request = 1;
        printf("[hid] F9: frame capture requested\n");
    }
    f9_was = f9;
    g_f2_down = down(VK_F2);   // skip-opening toggle (host_input)
    g_f10_down = down(VK_F10);   // Graphics menu (hle_osd.cpp)
    auto axis = [&](int neg, int pos) { return (int)(held >> pos & 1) - (int)(held >> neg & 1); };
    int mx = axis(16, 18), my = axis(19, 17);
    int cx = axis(20, 22), cy = axis(23, 21);
    float walk = held >> kWalk & 1 ? 0.45f : 1.0f;
    {   // a quick tap's direction held for key_min_hold_ms (pc_key_hold.h); not into a menu, the map, the overlay or the
        // title (their keys are not the walk's: the hold ends at once there) (the console layout's map too by
        // map_screen: g_screen is a button-edge guess)
        const int scr = g_screen.load();
        const bool menu = pc_overlay_open() || g_title_phase != 0 ||
                          (pcs ? pc_ctx_is_ui(pc_ctx()) : (scr == ScrPad || scr == ScrMenu || map_screen()));
        auto bit = [&](int b) { return (held >> b & 1) != 0; };
        if (menu) pckh::cancel(g_key_hold);   // (a focus loss: the idle return above)
        else pckh::move(g_key_hold, bit(16), bit(18), bit(19), bit(17), &mx, &my, &walk, GetTickCount64(), key_min_hold_ms());
    }
    if (mx || my) {   // keys win over the pad stick while held
        float k = 32767.0f * walk / sqrtf((float)(mx * mx + my * my));
        in->lx = (s32)lrintf(mx * k); in->ly = (s32)lrintf(my * k);
    }
    if (cx || cy) {
        float k = 32767.0f / sqrtf((float)(cx * cx + cy * cy));
        in->rx = (s32)lrintf(cx * k); in->ry = (s32)lrintf(cy * k);
    }
    if (pcs && ro.stick[0]) { in->lx = ro.sx[0]; in->ly = ro.sy[0]; }   // (a PC scheme macro's sticks: over keys and pads)
    if (pcs && ro.stick[1]) { in->rx = ro.sx[1]; in->ry = ro.sy[1]; }
    mouse_read(in, aiming);
}

// ---- the controls rows of nxrecomp's settings overlay (pc_settings.h, NXR_PC_OVERLAY=1): the own rows, replacing the
// overlay's restart-only stand-ins under the same keys and stored values, and applied live: a change sets the variable
// and bumps g_ctl_gen, and the input thread rebuilds its tables at its next read (bindings(), mouse_cfg()); the mouse's
// own (sensitivity, stick speed, inversion) go to pc_mouse_reload, the focus pause to oe_settings_reload. Editing a key
// binding switches the layout to Custom (the bindings apply there; empty = the PC layout's keys). The button-prompt row
// is a restart row (the prompt archive is chosen when the game first loads it).
extern "C" void pc_mouse_reload(void);     // pc_mouse.cpp
extern "C" void oe_settings_reload(void);  // hle_oe.cpp
static void rumble_reload();              // (the rumble output, below)
static void ctl_env(const char* var, const char* v) { _putenv_s(var, v ? v : ""); g_ctl_gen++; }
#define CTL_APPLY(fn, var, then) static void fn(const char* v) { ctl_env(var, v); then; }
CTL_APPLY(ap_layout, "TOTK_KEYBOARD_LAYOUT", (void)0)
CTL_APPLY(ap_mouse_aim, "TOTK_MOUSE_AIM", (void)0)
CTL_APPLY(ap_sens, "TOTK_MOUSE_SENS", pc_mouse_reload())
CTL_APPLY(ap_camera, "TOTK_MOUSE_CAMERA", pc_mouse_reload())
CTL_APPLY(ap_stick, "TOTK_MOUSE_STICK_SPEED", pc_mouse_reload())
CTL_APPLY(ap_invert, "TOTK_MOUSE_INVERT", pc_mouse_reload())
CTL_APPLY(ap_gyro, "TOTK_MOUSE_GYRO", (void)0)
CTL_APPLY(ap_gyro_speed, "TOTK_MOUSE_GYRO_SPEED", (void)0)
CTL_APPLY(ap_aim_scale, "TOTK_MOUSE_AIM_SCALE", pc_mouse_reload())
CTL_APPLY(ap_wheel, "TOTK_MOUSE_WHEEL", (void)0)
CTL_APPLY(ap_pause, "TOTK_PAUSE_UNFOCUSED", oe_settings_reload())
CTL_APPLY(ap_glyphs, "TOTK_GLYPHS", g_glyph_forced = v && *v && strcmp(v, "auto") ? glyph_code(v) : -1)
CTL_APPLY(ap_map, "TOTK_MOUSE_MAP", (void)0)
CTL_APPLY(ap_menus, "TOTK_MENU_KEYS", (void)0)
CTL_APPLY(ap_rumble, "TOTK_RUMBLE", rumble_reload())
CTL_APPLY(ap_pad_motion, "TOTK_GYRO", gyro_set(v))
CTL_APPLY(ap_gyro_scale, "TOTK_GYRO_SCALE", gyro_scale_set(v))
CTL_APPLY(ap_dead_zone_l, "TOTK_STICK_DEADZONE_L", g_dead_zone[0] = dead_zone_units(v))
CTL_APPLY(ap_dead_zone_r, "TOTK_STICK_DEADZONE_R", g_dead_zone[1] = dead_zone_units(v))
CTL_APPLY(ap_key_hold, "TOTK_KEY_MIN_HOLD_MS", (void)0)
static void kbm_custom() {
    const char* l = pc_settings_get("controls.keyboard_layout");
    if (!l || strcmp(l, "custom")) pc_settings_set("controls.keyboard_layout", "custom");
}
#define KBM_APPLY(act) static void ap_kbm_##act(const char* v) { ctl_env("TOTK_KBM_" #act, v); kbm_custom(); }
KBM_APPLY(A) KBM_APPLY(B) KBM_APPLY(X) KBM_APPLY(Y) KBM_APPLY(LSTICK) KBM_APPLY(RSTICK) KBM_APPLY(L) KBM_APPLY(R)
KBM_APPLY(ZL) KBM_APPLY(ZR) KBM_APPLY(PLUS) KBM_APPLY(MINUS) KBM_APPLY(DPAD_LEFT) KBM_APPLY(DPAD_UP) KBM_APPLY(DPAD_RIGHT)
KBM_APPLY(DPAD_DOWN) KBM_APPLY(MOVE_LEFT) KBM_APPLY(MOVE_UP) KBM_APPLY(MOVE_RIGHT) KBM_APPLY(MOVE_DOWN)
KBM_APPLY(CAMERA_LEFT) KBM_APPLY(CAMERA_UP) KBM_APPLY(CAMERA_RIGHT) KBM_APPLY(CAMERA_DOWN) KBM_APPLY(WALK)
#undef KBM_APPLY
#define PAD_APPLY(act) static void ap_pad_##act(const char* v) { _putenv_s("TOTK_PADBIND_" #act, v ? v : ""); g_padmap_gen++; }
PAD_APPLY(A) PAD_APPLY(B) PAD_APPLY(X) PAD_APPLY(Y) PAD_APPLY(LSTICK) PAD_APPLY(RSTICK) PAD_APPLY(L) PAD_APPLY(R)
PAD_APPLY(ZL) PAD_APPLY(ZR) PAD_APPLY(PLUS) PAD_APPLY(MINUS) PAD_APPLY(DPAD_LEFT) PAD_APPLY(DPAD_UP) PAD_APPLY(DPAD_RIGHT)
PAD_APPLY(DPAD_DOWN)
#undef PAD_APPLY
static const char* const kCtlLayoutV[] = {"classic", "pc", "custom", nullptr};
static const char* const kCtlLayoutN[] = {"Classic (the engine's keys)", "PC (WASD, mouse)", "Custom (the bindings below)", nullptr};
static const char* const kCtlOnV[] = {"", "0", nullptr};     // on unless "0"
static const char* const kCtlOnN[] = {"On", "Off", nullptr};
static const char* const kMouseLookV[] = {"0", "1", nullptr};   // controls.mouse_look (hidden)
static const char* const kMouseLookN[] = {"Stick", "Direct", nullptr};
static const char* const kCtlOffV[] = {"", "1", nullptr};    // off unless "1"
static const char* const kCtlOffN[] = {"Off", "On", nullptr};
static const char* const kCtlGyroV[] = {"aim", "always", "off", nullptr};
static const char* const kCtlGyroN[] = {"While aiming (bow, throw)", "Always", "Off", nullptr};
// controls.mouse_aim_scale: 0.1.. 1.5 by 0.05, then 1.6.. 2, 2.25.. 3
static const char* const kAimScaleV[] = {"0.1", "0.15", "0.2", "0.25", "0.3", "0.35", "0.4", "0.45", "0.5", "0.55", "0.6", "0.65", "0.7", "0.75", "0.8", "0.85", "0.9", "0.95", "1", "1.05", "1.1", "1.15", "1.2", "1.25", "1.3", "1.35", "1.4", "1.45", "1.5", "1.6", "1.7", "1.8", "1.9", "2", "2.25", "2.5", "2.75", "3", nullptr};
static const char* const kCtlGlyphV[] = {"auto", "switch", "xbox", "ps", "kbm", nullptr};
static const char* const kCtlGlyphN[] = {"Automatic (the device used last)", "Switch (the game's)", "Xbox", "PlayStation",
                                          "Keyboard and mouse", nullptr};
#define KM "Keyboard and mouse"
static const pc_setting k_ctl_rows[] = {
    {"controls.keyboard_layout", "Controls", KM, "Keyboard layout", "For the console scheme, and with the PC scheme for "
     "the world while Gameplay keys is Console (menus use the PC keys). Classic: Space jump, Shift sprint, E interact, F attack, "
     "WASD, arrows camera. PC: E interact, Shift sprint, Space jump, left mouse attack, right mouse bow, Q target, F "
     "abilities. Custom: the PC keys changed by the bindings below.", "pc", kCtlLayoutV, kCtlLayoutN, 0, 0, 0, 0,
     "TOTK_KEYBOARD_LAYOUT", nullptr, ap_layout, nullptr},
    {"controls.key_min_hold_ms", "Controls", KM, "Minimum movement key press", "Milliseconds a quick tap of a movement "
     "key (WASD) keeps Link moving, so he takes a step instead of sliding with his legs still. 0 = off (a tap as short as "
     "the key press).", "150", nullptr, nullptr, 0.0f, 250.0f, 10.0f, 0, "TOTK_KEY_MIN_HOLD_MS", nullptr, ap_key_hold, nullptr},
    {"controls.mouse_camera_scale", "Controls", KM, "Mouse camera", "The mouse turns the camera (click the window to "
     "capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.", "1", nullptr, nullptr, 0.0f, 4.0f,
     0.1f, 0, "TOTK_MOUSE_CAMERA", nullptr, ap_camera, nullptr},
    {"controls.glyphs", "Controls", "Pad", "Button prompts", "The button pictures in every prompt and menu, for the keys "
     "and buttons bound. Automatic follows the device used last: keyboard and mouse, an Xbox (XInput) pad, a DualSense / "
     "DualShock 4, or the game's own with a Nintendo pad. A change shows at once, in every prompt and label; with a pad "
     "in use, menus hide the mouse pointer until the mouse moves.", "auto", kCtlGlyphV,
     kCtlGlyphN, 0, 0, 0, 0, "TOTK_GLYPHS", nullptr, ap_glyphs, nullptr},
    {"controls.mouse_aim", "Controls", KM, "Left mouse shoots while aiming", "With the bow drawn (a ZR key or button held), "
     "the left button shoots instead of attacking.", "", kCtlOnV, kCtlOnN, 0, 0, 0, 0, "TOTK_MOUSE_AIM", nullptr, ap_mouse_aim, nullptr},
    // (no page: not shown) 1 once the saved mouse sensitivity is the direct mouse look's (pc_mouse.cpp migrate_sensitivity)
    {"controls.mouse_look", "", "", "Mouse look version", nullptr, "0", kMouseLookV, kMouseLookN, 0, 0, 0, 0, nullptr,
     nullptr, nullptr, nullptr},
    {"controls.mouse_sensitivity", "Controls", KM, "Mouse sensitivity", "Scales the camera and the motion aim.", "1", nullptr,
     nullptr, 0.1f, 20.0f, 0.1f, 0, "TOTK_MOUSE_SENS", nullptr, ap_sens, nullptr},
    {"controls.mouse_stick_speed", "Controls", KM, "Mouse speed for a full camera turn", "Mouse counts per second that turn "
     "the camera at full stick speed.", "1500", nullptr, nullptr, 50.0f, 5000.0f, 50.0f, 0, "TOTK_MOUSE_STICK_SPEED", nullptr,
     ap_stick, nullptr},
    {"controls.mouse_invert_y", "Controls", KM, "Invert mouse vertical", nullptr, "", kCtlOffV, kCtlOffN, 0, 0, 0, 0,
     "TOTK_MOUSE_INVERT", nullptr, ap_invert, nullptr},
    {"controls.mouse_gyro", "Controls", KM, "Mouse as motion aim", "While aiming, the mouse turns a virtual controller, so "
     "the game's motion aiming gets the mouse's precision (when no pad with motion is connected).", "off", kCtlGyroV,
     kCtlGyroN, 0, 0, 0, 0, "TOTK_MOUSE_GYRO", nullptr, ap_gyro, nullptr},
    {"controls.mouse_gyro_speed", "Controls", KM, "Motion aim speed", nullptr, "1", nullptr, nullptr, 0.05f, 4.0f, 0.05f, 0,
     "TOTK_MOUSE_GYRO_SPEED", nullptr, ap_gyro_speed, nullptr},
    {"controls.mouse_aim_scale", "Controls", KM, "Aiming sensitivity (bow)", "With the bow drawn: the mouse's turn "
     "times this (1 = as when not aiming), with or without the mouse as motion aim. A pad's motion sensitivity is "
     "separate.", "1", kAimScaleV, kAimScaleV, 0, 0, 0, 0, "TOTK_MOUSE_AIM_SCALE", nullptr, ap_aim_scale, nullptr},
    {"controls.mouse_wheel", "Controls", KM, "Mouse wheel scrolls the quick menus", "While a D-pad quick menu is held open "
     "(weapons, shields, items, bows), each notch moves one entry.", "", kCtlOnV, kCtlOnN, 0, 0, 0, 0, "TOTK_MOUSE_WHEEL",
     nullptr, ap_wheel, nullptr},
    {"controls.mouse_map", "Controls", KM, "Map with the mouse", "On the map the mouse moves it as the left stick does "
     "(the reticle stays at the centre), the wheel zooms, a left click places a pin or stamp and a right click goes back.", "", kCtlOnV, kCtlOnN,
     0, 0, 0, 0, "TOTK_MOUSE_MAP", nullptr, ap_map, nullptr},
    {"controls.menu_keys", "Controls", KM, "Menu keys", "In the pause menu and the Purah Pad: Esc goes back, PageUp / "
     "PageDown (and the wheel in the pause menu) switch tabs, and in the pause menu clicks select (left) and go back "
     "(right).", "", kCtlOffV, kCtlOffN, 0, 0, 0, 0, "TOTK_MENU_KEYS", nullptr, ap_menus, nullptr},
    {"controls.rumble", "Controls", "Pad", "Rumble strength", "The pad's motors (Xbox, PlayStation and Switch pads): 0 "
     "= off, 1 = the game's own.", "1", nullptr, nullptr, 0.0f, 2.0f, 0.1f, 0, "TOTK_RUMBLE", nullptr, ap_rumble, nullptr},
    {"controls.gyro", "Controls", "Pad", "Motion (gyro)", "The motion sensors of a DualSense, DualShock 4, Pro "
     "Controller or Joy-Con, for the game's motion aiming (the game's own Motion Controls option turns it on there). Off: the pad "
     "lies still for the game.", "", kCtlOnV, kCtlOnN, 0, 0, 0, 0, "TOTK_GYRO", nullptr, ap_pad_motion, nullptr},
    {"controls.gyro_scale", "Controls", "Pad", "Motion sensitivity", "How far the aim turns for a turn of the pad: 1 = the "
     "pad's own, 2 = twice as far.", "1", nullptr, nullptr, 0.25f, 3.0f, 0.05f, 0, "TOTK_GYRO_SCALE", nullptr,
     ap_gyro_scale, nullptr},
    {"controls.deadzone_left", "Controls", "Pad", "Left stick dead zone", "How far the left stick must move before it "
     "counts, in percent of its travel. Raise it if Link moves with the stick at rest (a worn stick); lower it for finer "
     "small steps. 0 = the pad's own (Xbox 24, PlayStation 15, Switch 12).", "0", nullptr, nullptr, 0.0f, 50.0f, 1.0f,
     0, "TOTK_STICK_DEADZONE_L", nullptr, ap_dead_zone_l, nullptr},
    {"controls.deadzone_right", "Controls", "Pad", "Right stick dead zone", "How far the right stick must move before it "
     "counts, in percent of its travel. Raise it if the camera turns with the stick at rest; lower it for finer aiming. "
     "0 = the pad's own (Xbox 27, PlayStation 15, Switch 12).", "0", nullptr, nullptr, 0.0f, 50.0f, 1.0f, 0,
     "TOTK_STICK_DEADZONE_R", nullptr, ap_dead_zone_r, nullptr},
    {"game.pause_unfocused", "Game", "Window", "Pause when the window loses focus", "TOTK_PAUSE_UNFOCUSED: while the "
     "game window is not in front the game stops, picture, sound and its clock, and goes on where it was "
     "(hle_oe.cpp oe_host_paused).", "", kCtlOffV, kCtlOffN, 0, 0, 0,
     0, "TOTK_PAUSE_UNFOCUSED", nullptr, ap_pause, nullptr},
#define KBM_ROW(key, label, act) {"controls.kbm." key, "Controls", "Key bindings", label, "Up to 4 keys or mouse buttons. "  \
     "Editing one switches the layout to Custom; empty = the PC layout's.", "", nullptr, nullptr, 0, 0, 0, PC_SET_KEYS,  \
     "TOTK_KBM_" #act, nullptr, ap_kbm_##act, nullptr}
    KBM_ROW("a", "A (interact)", A), KBM_ROW("b", "B (sprint / back)", B), KBM_ROW("x", "X (jump)", X),
    KBM_ROW("y", "Y (attack)", Y), KBM_ROW("lstick", "Left stick click (crouch)", LSTICK),
    KBM_ROW("rstick", "Right stick click (look)", RSTICK), KBM_ROW("l", "L (abilities)", L), KBM_ROW("r", "R (throw)", R),
    KBM_ROW("zl", "ZL (target / shield)", ZL), KBM_ROW("zr", "ZR (bow)", ZR), KBM_ROW("plus", "+ (menu)", PLUS),
    KBM_ROW("minus", "- (map)", MINUS), KBM_ROW("dpad_left", "D-pad left (shields)", DPAD_LEFT),
    KBM_ROW("dpad_up", "D-pad up (items)", DPAD_UP), KBM_ROW("dpad_right", "D-pad right (weapons)", DPAD_RIGHT),
    KBM_ROW("dpad_down", "D-pad down", DPAD_DOWN), KBM_ROW("move_left", "Move left", MOVE_LEFT),
    KBM_ROW("move_up", "Move forward", MOVE_UP), KBM_ROW("move_right", "Move right", MOVE_RIGHT),
    KBM_ROW("move_down", "Move back", MOVE_DOWN), KBM_ROW("camera_left", "Camera left", CAMERA_LEFT),
    KBM_ROW("camera_up", "Camera up", CAMERA_UP), KBM_ROW("camera_right", "Camera right", CAMERA_RIGHT),
    KBM_ROW("camera_down", "Camera down", CAMERA_DOWN), KBM_ROW("walk", "Walk (hold)", WALK),
#undef KBM_ROW
#define PAD_ROW(key, label, act) {"controls.pad." key, "Controls", "Pad buttons", label, "Up to 4 pad buttons, by position: "  \
     "A = the right face button (Xbox B, PlayStation Circle), B = bottom (Xbox A, Cross), X = top (Xbox Y, Triangle), "     \
     "Y = left (Xbox X, Square). Empty = its own button; None = unbound.", "", nullptr, nullptr, 0, 0, 0, PC_SET_PAD,       \
     "TOTK_PADBIND_" #act, nullptr, ap_pad_##act, nullptr}
    PAD_ROW("a", "A (interact)", A), PAD_ROW("b", "B (sprint / back)", B), PAD_ROW("x", "X (jump)", X),
    PAD_ROW("y", "Y (attack)", Y), PAD_ROW("lstick", "Left stick click (crouch)", LSTICK),
    PAD_ROW("rstick", "Right stick click (look)", RSTICK), PAD_ROW("l", "L (abilities)", L), PAD_ROW("r", "R (throw)", R),
    PAD_ROW("zl", "ZL (target / shield)", ZL), PAD_ROW("zr", "ZR (bow)", ZR), PAD_ROW("plus", "+ (menu)", PLUS),
    PAD_ROW("minus", "- (map)", MINUS), PAD_ROW("dpad_left", "D-pad left (shields)", DPAD_LEFT),
    PAD_ROW("dpad_up", "D-pad up (items)", DPAD_UP), PAD_ROW("dpad_right", "D-pad right (weapons)", DPAD_RIGHT),
    PAD_ROW("dpad_down", "D-pad down", DPAD_DOWN),
#undef PAD_ROW
};
#undef KM
static struct CtlRows { CtlRows() { pc_settings_add(k_ctl_rows, (int)(sizeof k_ctl_rows / sizeof k_ctl_rows[0])); } } g_ctl_rows;
#else
// Keyboard (only while the game window has focus): SDL scancodes = physical key positions (QWERTY), same keys as above.
static void keyboard_read(HostInput* in) {
    if (!SDL_GetKeyboardFocus()) return;                        // game window not in front (or no window: headless)
    int nk = 0;
    const bool* ks = SDL_GetKeyboardState(&nk);
    if (!ks) return;
    auto down = [&](SDL_Scancode sc) { return sc != SDL_SCANCODE_UNKNOWN && (int)sc < nk && ks[sc]; };
    const SDL_Scancode U = SDL_SCANCODE_UNKNOWN;
    static const struct { SDL_Scancode a, b; u64 btn; } keys[] = {
        {SDL_SCANCODE_SPACE, SDL_SCANCODE_UNKNOWN, BtnX}, {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, BtnB}, {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UNKNOWN, BtnB},
        {SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN, BtnA}, {SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, BtnA}, {SDL_SCANCODE_F, SDL_SCANCODE_UNKNOWN, BtnY},
        {SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN, BtnL}, {SDL_SCANCODE_R, SDL_SCANCODE_UNKNOWN, BtnR}, {SDL_SCANCODE_Z, SDL_SCANCODE_UNKNOWN, BtnZL},
        {SDL_SCANCODE_C, SDL_SCANCODE_UNKNOWN, BtnZR}, {SDL_SCANCODE_V, SDL_SCANCODE_UNKNOWN, BtnLS}, {SDL_SCANCODE_G, SDL_SCANCODE_UNKNOWN, BtnRS},
        {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UNKNOWN, BtnPlus}, {SDL_SCANCODE_M, SDL_SCANCODE_UNKNOWN, BtnMinus}, {SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN, BtnLeft},
        {SDL_SCANCODE_2, SDL_SCANCODE_UNKNOWN, BtnUp}, {SDL_SCANCODE_3, SDL_SCANCODE_UNKNOWN, BtnRight}, {SDL_SCANCODE_4, SDL_SCANCODE_UNKNOWN, BtnDown}};
    for (auto& k : keys) if (down(k.a) || down(k.b)) in->buttons |= k.btn;
    static bool f9_was = false;
    bool f9 = down(SDL_SCANCODE_F9);
    if (f9 && !f9_was) {
        g_debug_frame_request = 1;
        printf("[hid] F9: frame capture requested\n");
    }
    f9_was = f9;
    g_f2_down = down(SDL_SCANCODE_F2);   // skip-opening toggle (host_input)
    g_f10_down = down(SDL_SCANCODE_F10);   // Graphics menu (hle_osd.cpp)
    auto axis = [&](SDL_Scancode neg, SDL_Scancode pos, SDL_Scancode neg2, SDL_Scancode pos2) { return (down(pos) || down(pos2) ? 1 : 0) - (down(neg) || down(neg2) ? 1 : 0); };
    int mx = axis(SDL_SCANCODE_A, SDL_SCANCODE_D, U, U), my = axis(SDL_SCANCODE_S, SDL_SCANCODE_W, U, U);
    int cx = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_J, SDL_SCANCODE_L), cy = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, SDL_SCANCODE_K, SDL_SCANCODE_I);
    float walk = down(SDL_SCANCODE_LCTRL) || down(SDL_SCANCODE_RCTRL) ? 0.45f : 1.0f;
    if (mx || my) {   // keys win over the pad stick while held
        float k = 32767.0f * walk / sqrtf((float)(mx * mx + my * my));
        in->lx = (s32)lrintf(mx * k); in->ly = (s32)lrintf(my * k);
    }
    if (cx || cy) {
        float k = 32767.0f / sqrtf((float)(cx * cx + cy * cy));
        in->rx = (s32)lrintf(cx * k); in->ry = (s32)lrintf(cy * k);
    }
}
#endif

// TOTK_CTL=<file>: a live pad driven by a command file that a drive or scene tool appends to (off without the variable).
// One command per line, read as whole lines at most every 50 ms (file size checked first; a file that shrinks was
// truncated by its writer and is read again from the start):
//   press A+B [s]       hold for s seconds (default 0.3)        down ZL / up ZL / up all
//   stick L <x> <y>     left or right stick at -1..1 (y up); stick L 0 0 releases it
//   dump                capture the next frame, as F9
//   glyphmap            log each button's glyph now / in the gameplay set, and pc_glyph_gameplay_cp (4.15)
//   bind <scope> <action> <slot> <key|none> / bindreset <scope> [action] / bindget <scope> <action> / capture
//                       [wheel] [esc]: the rebinding API (pc_input.h), logged as "[pc] bind" / "[pc] capture"
//   mouse <dx> <dy>     raw mouse counts, as if moved while captured (needs TOTK_MOUSE_CAMERA=1: the camera turns, or
//                       with ZR held the mouse gyro aims); for 1 s the mouse paths run with the window behind others
//   wheel <n>           mouse wheel notches (+ = away), as scrolled while captured (TOTK_MOUSE_WHEEL: the quick menus)
//   key <name> down|up|<s>   a key or mouse button by the bindings' names ("W", "Space", "Mouse Left"), held as if pressed:
//                       it goes through TOTK_KEYBOARD_LAYOUT's bindings like a real key (Windows; window in front or not)
//   pad A+B [s]         pad buttons held as if pressed on a pad (default 0.3 s): through the pad bindings (pad_remap)
//                       and to the overlay like a real pad's, from the next refresh (unattended tests of the rebinding)
//   sixaxis all|none|acc+gyro+angle+dir[+dirT|dt33|latest]   the motion fields the game gets (the rest as lying still): motion aiming A/Bs
//   set <key> [value]   a settings row set as the overlay sets it (Windows; live rows apply at once)
//   winkey F11 | alt+enter   a key press posted to the game window: the window procedure's keys (F11 / Alt+Enter window
//                       modes, F10 overlay), which `key` does not reach (Windows)
//   focus out | in      the activation messages of an alt-tab away / back, posted to the window (Windows)
//   cursor <nx> <ny> [ms] the pointer at nx, ny (0..1 of the client area) for pc_mouse_client_pos, for 1 s (the tests);
//   with ms, a glide there from the last point over ms (a drag: motion in every read)
//   click left|right [s] [lost]   a mouse button press posted to the game window at that pointer, held s seconds (default
//                       0.1); lost: no release posted (let go outside the window)
//   gyro <x> <y> <z> <s>   a virtual pad turning at x y z rev/s (Switch frame) for s seconds (g_synthpad)
// Names are TOTK_PRESS's (A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN; LUP..RRIGHT = full stick deflection).
// Its buttons and sticks are applied after the Graphics menu chord (L3+R3) and the skip-opening Minus logic, so they
// reach the game only; nothing while the Graphics menu is open. Logged: the file opened, each command, each ignored line.
extern "C" void pc_mouse_inject(double dx, double dy);   // pc_mouse.cpp (TOTK_MOUSE_CAMERA)
extern "C" void pc_mouse_inject_wheel(double notches);
extern "C" void pc_mouse_inject_cursor(double nx, double ny, double ms);   // pc_mouse.cpp: pc_mouse_client_pos's for 1 s
extern "C" bool pc_mouse_client_pos(float* nx, float* ny);
struct CtlState { u64 held = 0; std::vector<std::pair<double, u64>> releases; bool stick[2] = {}; s32 sx[2] = {}, sy[2] = {}; };
// TOTK_CTL "gyro": the virtual pad's state (input thread only). R's rows are the world's reference axes seen from the pad,
// turned as mouse_gyro_feed turns them; gravity is fed with it so motion_fuse's tilt correction agrees.
static struct { float rate[3]; double until, off_at, last = -1, R[3][3]; Fusion fus; } g_synth;
static void synth_start(const float rate[3], double secs, double now) {
    if (!g_synthpad.live) {
        memset(g_synth.R, 0, sizeof g_synth.R);
        g_synth.R[0][0] = g_synth.R[1][1] = g_synth.R[2][2] = 1;
        g_synth.fus = Fusion();
        { std::lock_guard<std::mutex> l(g_synthpad.mmu); g_synthpad.m = Motion(); }
        g_synth.last = now;
        g_synthpad.live = true;
    }
    memcpy(g_synth.rate, rate, sizeof g_synth.rate);
    g_synth.until = now + secs;
    g_synth.off_at = g_synth.until + 10.0;
}
static void synth_tick(double now) {
    if (!g_synthpad.live) return;
    double dt = now - g_synth.last;
    if (!(dt > 0)) return;
    g_synth.last = now;
    if (dt > 0.05) dt = 0.05;
    if (now >= g_synth.off_at) { g_synthpad.live = false; g_synthpad.has_imu = false; return; }
    const bool on = now < g_synth.until;
    const float gyro[3] = {on ? g_synth.rate[0] : 0, on ? g_synth.rate[1] : 0, on ? g_synth.rate[2] : 0};
    const double w[3] = {gyro[0] * 6.2831853, gyro[1] * 6.2831853, gyro[2] * 6.2831853};
    for (int r = 0; r < 3; r++) {
        double* v = g_synth.R[r], cr[3] = {w[1] * v[2] - w[2] * v[1], w[2] * v[0] - w[0] * v[2], w[0] * v[1] - w[1] * v[0]};
        for (int i = 0; i < 3; i++) v[i] -= cr[i] * dt;
        double nv = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; i++) v[i] /= nv;
    }
    const float acc[3] = {(float)-g_synth.R[2][0], (float)-g_synth.R[2][1], (float)-g_synth.R[2][2]};
    motion_fuse(&g_synthpad, &g_synth.fus, acc, gyro, (float)dt);
}
extern "C" void pc_shot_request(void);   // pc_shot.cpp: the F12 screenshot
extern "C" const char* hid_glyph_for(int style, const char* control);              // (below: TOTK_CTL "glyphmap")
extern "C" const char* hid_glyph_for_set(int style, const char* control, int set);
extern "C" int hid_glyph_active(void);
extern "C" bool pc_bind_ctl(const char* line);   // pc_input.cpp: bind / bindreset / bindget / capture
static void vib_test(float lo, float hi, double secs);   // (below the vibration graph)
static void ctl_apply(CtlState& s, const char* line, double now) {
    char cmd[16] = {}, arg[128] = {}; double v1 = 0, v2 = 0;
    int n = sscanf(line, "%15s %127s %lf %lf", cmd, arg, &v1, &v2);
    if (n <= 0) return;   // blank line
    auto bits_of = [](const char* list, u64* out) {   // "A+B+ZL" -> bits; false on an unknown name
        u64 b = 0;
        for (const char* p = list; *p;) {
            const char* e = strchr(p, '+');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char t[16] = {};
            if (!len || len >= sizeof t) return false;
            for (size_t i = 0; i < len; i++) t[i] = (char)toupper((unsigned char)p[i]);
            int bit = -1;
            for (auto& nm : kPressNames) if (!strcmp(nm.n, t)) bit = nm.bit;
            if (bit < 0) return false;
            b |= 1ull << bit;
            p += len + (e ? 1 : 0);
        }
        *out = b;
        return b != 0;
    };
    u64 b = 0;
    bool ok = true;
    const u64 kDump = 1ull << 40;
    if ((!strcmp(cmd, "press") || !strcmp(cmd, "down")) && n >= 2 && bits_of(arg, &b)) {
        if (b & kDump) { g_debug_frame_request = 1; b &= ~kDump; }   // DUMP: once, not held
        s.held |= b;
        if (cmd[0] == 'p' && b) s.releases.push_back({now + (n >= 3 && v1 > 0 ? v1 : 0.3), b});
    }
    else if (!strcmp(cmd, "pad") && n >= 2 && bits_of(arg, &b) && !(b & ~0xFFFFull))   // (held at bits 48-63: g_ctl_pad)
        s.releases.push_back({now + (n >= 3 && v1 > 0 ? v1 : 0.3), b << 48});
    else if (!strcmp(cmd, "up") && n >= 2 && !strcmp(arg, "all")) { s.held = 0; s.releases.clear(); s.stick[0] = s.stick[1] = false; }
    else if (!strcmp(cmd, "up") && n >= 2 && bits_of(arg, &b)) { s.held &= ~b; for (auto& r : s.releases) r.second &= ~b; }
    else if (!strcmp(cmd, "stick") && n == 4 && (!strcmp(arg, "L") || !strcmp(arg, "R") || !strcmp(arg, "l") || !strcmp(arg, "r"))) {
        int k = toupper((unsigned char)arg[0]) == 'R';
        double x = fmax(-1.0, fmin(1.0, v1)), y = fmax(-1.0, fmin(1.0, v2));
        s.sx[k] = (s32)lrint(x * 32767); s.sy[k] = (s32)lrint(y * 32767); s.stick[k] = s.sx[k] || s.sy[k];
    } else if (!strcmp(cmd, "dump") && n == 1) { g_debug_frame_request = 1; }
    else if (!strncmp(cmd, "bind", 4) || !strcmp(cmd, "capture")) ok = pc_bind_ctl(line);   // (pc_input.cpp)
    else if (!strcmp(cmd, "glyphmap") && n == 1) {   // 4.15: each button character's gameplay-set code point and the
        // current set's control glyph name, one line (the diagram's answer, checkable without the panes)
        static const char* const ctl[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS"};
        std::string out;
        for (const char* c : ctl) {
            const char* now = hid_glyph_for(hid_glyph_active(), c);
            const char* gp = hid_glyph_for_set(hid_glyph_active(), c, PCX_WORLD);
            out += std::string(" ") + c + "=" + (now ? now : "-") + "/" + (gp ? gp : "-");
        }
        char cps[160] = {};
        for (unsigned cp = 0xE040, o = 0; cp <= 0xE048 && o < sizeof cps - 8; cp++)
            o += (unsigned)snprintf(cps + o, sizeof cps - o, " %X>%X", cp, pc_glyph_gameplay_cp((uint16_t)cp));
        char scps[160] = {};   // (the Scope's set: a Special Controls page about it, pc_glyph_cp_in)
        for (unsigned cp = 0xE040, o = 0; cp <= 0xE048 && o < sizeof scps - 8; cp++)
            o += (unsigned)snprintf(scps + o, sizeof scps - o, " %X>%X", cp, pc_glyph_cp_in(PCX_SCOPE, (uint16_t)cp));
        printf("[hid] glyphmap (now/gameplay):%s; cp:%s; scope cp:%s\n", out.c_str(), cps, scps);
    }
    else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %lf %lf", &v1, &v2) == 2) pc_mouse_inject(v1, v2);
    else if (!strcmp(cmd, "wheel") && sscanf(line, "%*s %lf", &v1) == 1) pc_mouse_inject_wheel(v1);
    else if (!strcmp(cmd, "cursor") && sscanf(line, "%*s %lf %lf", &v1, &v2) == 2) {
        double ms = 0;
        sscanf(line, "%*s %*f %*f %lf", &ms);
        pc_mouse_inject_cursor(v1, v2, ms);
    }
    else if (!strcmp(cmd, "cursormode") && n >= 2 && (!strcmp(arg, "on") || !strcmp(arg, "off")))
        pc_mouse_cursor_mode(arg[1] == 'n');
    else if (!strcmp(cmd, "ctx") && n >= 2) ok = pc_ctx_force(arg);   // ctx <world|pause|pad|map|dialogue|...|auto>: the PC scheme's
    else if (!strcmp(cmd, "player") && n >= 2) {   // player <flags: hex or climb+air+...> [ability 0-6] | off: the faked push
        if (!strcmp(arg, "off")) pc_ctx_player_fake(false, 0, 0);
        else {
            static const struct { const char* n; uint32_t f; } fl[] = {{"climb", PCP_CLIMB}, {"air", PCP_AIR}, {"glide", PCP_GLIDE},
                {"swim", PCP_SWIM}, {"aim", PCP_AIM}, {"guard", PCP_GUARD}, {"ability", PCP_ABILITY}, {"horse", PCP_HORSE},
                {"vehicle", PCP_VEHICLE}, {"event", PCP_EVENT}, {"loading", PCP_LOADING}, {"scope", PCP_SCOPE}, {"construct", PCP_CONSTRUCT},
                {"foot", 0}};
            uint32_t flags = 0;
            char* end = nullptr;
            flags = (uint32_t)strtoul(arg, &end, 16);
            if (!end || *end) {   // names
                flags = 0;
                for (const char* p = arg; *p && ok;) {
                    const char* e = strchr(p, '+');
                    std::string t(p, e ? (size_t)(e - p) : strlen(p));
                    bool found = false;
                    for (auto& x : fl) if (t == x.n) { flags |= x.f; found = true; }
                    ok = found;
                    p = e ? e + 1 : p + t.size();
                }
            }
            if (ok) pc_ctx_player_fake(true, flags, (uint8_t)(n >= 3 ? v1 : 0));
        }
    }
#ifdef _WIN32
    else if (!strcmp(cmd, "click") && n >= 2 && (!strcmp(arg, "left") || !strcmp(arg, "right"))) {
        HWND h = (HWND)shim_window_hwnd();
        float nx = 0.5f, ny = 0.5f;
        pc_mouse_client_pos(&nx, &ny);
        RECT r;
        ok = h && GetClientRect(h, &r);
        if (ok) {
            const bool left = arg[0] == 'l';
            const LPARAM at = MAKELPARAM((int)(nx * r.right), (int)(ny * r.bottom));
            const DWORD hold = (DWORD)((n >= 3 && v1 > 0 ? v1 : 0.1) * 1000);
            PostMessageW(h, left ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, left ? MK_LBUTTON : MK_RBUTTON, at);
            // ("lost": no release reaches the window, as when the button is let go outside it)
            if (!strstr(line, " lost"))
                std::thread([h, left, at, hold] { Sleep(hold); PostMessageW(h, left ? WM_LBUTTONUP : WM_RBUTTONUP, 0, at); }).detach();
        }
    }
#endif
    else if (!strcmp(cmd, "sixaxis") && n >= 2) {   // sixaxis all | none | acc+gyro+angle+dir
        int m = 0;
        for (const char* p = arg; *p && ok;) {
            const char* e = strchr(p, '+');
            std::string t(p, e ? (size_t)(e - p) : strlen(p));
            if (t == "all") m |= kSixAll;
            else if (t == "acc") m |= kSixAcc;
            else if (t == "gyro") m |= kSixGyro;
            else if (t == "angle") m |= kSixAngle;
            else if (t == "dir") m |= kSixDir;
            else if (t == "dirT") m |= kSixDir | kSixDirT;
            else if (t == "dt33") m |= kSixDt33;
            else if (t == "latest") m |= kSixLatest;
            else if (t != "none") ok = false;
            p = e ? e + 1 : p + t.size();
        }
        if (ok) g_sixaxis_mask = m;
    }
    else if (!strcmp(cmd, "vk") && n >= 2) {   // vk <hex mask> | vk tap <hex mask>: the player's virtual keys held / tapped
        const bool tap = !strcmp(arg, "tap");   // as the PC rows would (totk_pcvk.h; the VK hook's tests), routing on
        unsigned long long m = 0;
        ok = sscanf(line, tap ? "%*s %*s %llx" : "%*s %llx", &m) == 1;
        if (ok) { totk_pcvk_set_active(true); if (tap) totk_pcvk_tap(m); else totk_pcvk_hold(m); }
    }
    else if (!strcmp(cmd, "ability") && n >= 2) {   // ability <1-6>: select it, the wheel's way (G2) else SpecialPower +0x68
        const bool game = totk_ability_request(atoi(arg));
        ok = game || totk_player_request_ability(atoi(arg));
        printf("[ctl] ability %s: %s\n", arg, game ? "posted to the UI thread" : ok ? "direct +0x68 write" : "refused");
    }
    else if (!strcmp(cmd, "shot")) {   // shot [name]: an F12 screenshot (TOTK_PC_SHOT_DIR), as the key would; the name
        if (n >= 2) printf("[ctl] shot %s\n", arg);  // only labels it in the log
        pc_shot_request();
    }
    else if (!strcmp(cmd, "glyphs") && n >= 2) {   // glyphs kbm|xbox|ps|switch|auto: the glyph style (auto: the input used)
        int f = strcmp(arg, "auto") ? glyph_code(arg) : -1;
        ok = f >= 0 || !strcmp(arg, "auto");
        if (ok) { hid_glyph_active(); g_glyph_forced = f; }
    }
    else if (!strcmp(cmd, "gyro")) {   // gyro <x> <y> <z> <seconds>: rev/s
        float r[3]; double secs;
        ok = sscanf(line, "%*s %f %f %f %lf", &r[0], &r[1], &r[2], &secs) == 4 && secs > 0;
        if (ok) synth_start(r, secs, now);
    }
    else if (!strcmp(cmd, "rumble")) {   // rumble <low band amp> <high band amp> <seconds>: as a SendVibrationValue to both sides
        float lo, hi; double secs;
        ok = sscanf(line, "%*s %f %f %lf", &lo, &hi, &secs) == 3 && secs > 0 && lo >= 0 && hi >= 0;
        if (ok) vib_test(lo, hi, secs);
    }
#ifdef _WIN32
    else if (!strcmp(cmd, "vpad")) ok = vpad_cmd(line, bits_of);   // virtual pads at the device boundary (above the scan)
#endif
#ifdef _WIN32
    else if (!strcmp(cmd, "set") && n >= 2) {   // set <settings key> [value]: as the overlay row would (live rows apply now)
        const char* v = strstr(line + 3, arg) + strlen(arg);   // (the rest of the line: words, numbers or nothing)
        while (*v == ' ') v++;
        ok = pc_settings_get(arg) != nullptr;
        if (ok) pc_settings_set(arg, v);
    }
#endif
#ifdef _WIN32
    else if (!strcmp(cmd, "key") && n >= 2) {   // key <name, may have spaces> down|up|<seconds> (n counts "Mouse" only)
        std::string rest(line + 3);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r')) rest.pop_back();
        size_t sp = rest.find_last_of(' ');
        std::string name = sp == std::string::npos ? "" : rest.substr(0, sp), act = sp == std::string::npos ? "" : rest.substr(sp + 1);
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        int vk = vk_from_name(name.c_str());
        double secs = atof(act.c_str());
        if (!vk || (act != "down" && act != "up" && !(secs > 0))) ok = false;
        else if (act == "up") { key_inject(vk, false); g_key_until[vk] = 0; }
        else { key_inject(vk, true); g_key_until[vk] = act == "down" ? 0 : GetTickCount64() + (ULONGLONG)(secs * 1000); }
    }
    // winkey <name> | alt+<name>: a press posted to the game window, for what its window procedure handles (F11 and
    // Alt+Enter: shim_win.c's window modes; F10: the overlay) and `key` cannot reach (it only feeds the HID poll)
    else if (!strcmp(cmd, "winkey") && n >= 2) {
        HWND h = (HWND)shim_window_hwnd();
        bool alt = !_strnicmp(arg, "alt+", 4);
        int vk = vk_from_name(alt ? arg + 4 : arg);
        ok = h && vk;
        if (ok) {
            LPARAM l = 1 | (LPARAM)(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16) | (alt ? 1 << 29 : 0);
            PostMessageW(h, alt ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, l);
            PostMessageW(h, alt ? WM_SYSKEYUP : WM_KEYUP, vk, l | 3u << 30);
        }
    }
    // focus out | in: the messages an alt-tab away / back sends the game window (the deactivation without another
    // window taking the foreground: test boots must not move the desktop's focus)
    else if (!strcmp(cmd, "focus") && n >= 2 && (!strcmp(arg, "out") || !strcmp(arg, "in"))) {
        HWND h = (HWND)shim_window_hwnd();
        bool in = arg[0] == 'i';
        ok = h != nullptr;
        if (ok) {
            PostMessageW(h, WM_ACTIVATEAPP, in, 0);
            PostMessageW(h, WM_ACTIVATE, in ? WA_ACTIVE : WA_INACTIVE, 0);
            PostMessageW(h, in ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        }
    }
#endif
    else ok = false;
    printf(ok ? "[hid] TOTK_CTL: %s\n" : "[hid] TOTK_CTL: ignored line \"%s\"\n", line);
}
// Called from host_input()'s refresh (holding its lock): ORs the file's pad into *in. The file is read on a thread of
// its own (every 50 ms), never on the input path: an open of it waited up to 2.8 s at a time while a save commit
// renamed its directories. The drives' "menu stutter bursts" (the soak, d184a4a) were those waits, a harness cost a
// player without TOTK_CTL does not have.
static std::mutex g_ctl_mtx;
static std::vector<std::string> g_ctl_lines;   // read, not yet applied (g_ctl_mtx)
static void ctl_reader(std::string path) {
    long long pos = 0;
    bool opened = false;
    for (;; std::this_thread::sleep_for(std::chrono::milliseconds(50))) {
        FILE* f = fopen(path.c_str(), "rb");   // shared read: the writer keeps appending
        if (!f) continue;
        if (!opened) { opened = true; printf("[hid] TOTK_CTL: reading commands from %s\n", path.c_str()); }
        fseek(f, 0, SEEK_END);
        long long size = ftell(f);
        if (size < pos) { printf("[hid] TOTK_CTL: file truncated, reading from the start\n"); pos = 0; }
        if (size > pos) {
            std::string buf((size_t)(size - pos), '\0');
            fseek(f, (long)pos, SEEK_SET);
            buf.resize(fread(&buf[0], 1, buf.size(), f));
            size_t end = buf.rfind('\n');   // whole lines only; a partial last line waits for its newline
            if (end != std::string::npos) {
                pos += (long long)end + 1;
                std::lock_guard<std::mutex> l(g_ctl_mtx);
                for (size_t a = 0; a <= end;) {
                    size_t e = buf.find('\n', a);
                    std::string line = buf.substr(a, e - a);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    g_ctl_lines.push_back(std::move(line));
                    a = e + 1;
                }
            }
        }
        fclose(f);
    }
}
static void ctl_read(HostInput* in) {
    static const char* path = getenv("TOTK_CTL");
    if (!path || !*path) return;
    static CtlState s;
    static auto t0 = std::chrono::steady_clock::now();
    static bool started = (std::thread(ctl_reader, std::string(path)).detach(), true);
    (void)started;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<std::string> lines;
    { std::lock_guard<std::mutex> l(g_ctl_mtx); lines.swap(g_ctl_lines); }
    for (const auto& line : lines) ctl_apply(s, line.c_str(), now);
    for (size_t i = 0; i < s.releases.size();)   // press holds that ended
        if (now >= s.releases[i].first) { s.held &= ~s.releases[i].second; s.releases[i] = s.releases.back(); s.releases.pop_back(); }
        else i++;
    for (auto& r : s.releases) s.held |= r.second;   // overlapping presses of one button: the later hold keeps it down
    synth_tick(now);                                  // TOTK_CTL "gyro"
#ifdef _WIN32
    key_inject_tick();   // "key <name> <seconds>" presses that ended
#endif
    in->buttons |= s.held & 0xFFFFull;
    g_ctl_held = s.held;
    g_ctl_pad = s.held >> 48;
    auto dir = [&](int neg, int pos2) { return (s.held >> pos2 & 1 ? 1 : 0) - (s.held >> neg & 1 ? 1 : 0); };
    int lx = dir(34, 35), ly = dir(33, 32), rx = dir(38, 39), ry = dir(37, 36);   // LUP..RRIGHT: full deflection
    if (lx || ly) { float k = 32767.0f / sqrtf((float)(lx * lx + ly * ly)); in->lx = (s32)lrintf(lx * k); in->ly = (s32)lrintf(ly * k); }
    if (rx || ry) { float k = 32767.0f / sqrtf((float)(rx * rx + ry * ry)); in->rx = (s32)lrintf(rx * k); in->ry = (s32)lrintf(ry * k); }
    if (s.stick[0]) { in->lx = s.sx[0]; in->ly = s.sy[0]; }
    if (s.stick[1]) { in->rx = s.sx[1]; in->ry = s.sy[1]; }
}


#ifndef _WIN32
extern "C" void oe_focus_update(bool focused);   // hle_oe.cpp: TOTK_PAUSE_UNFOCUSED
#endif
// Pad button bindings: the readers above give each pad button as the Switch button it is unbound (by position: A = the
// right face button, B = bottom, X = top, Y = left, on Xbox, PlayStation and Nintendo pads alike; TOTK_PAD_LABELS as
// each reader has it); the game gets the actions bound to the buttons held. TOTK_PADBIND_<ACTION> (A B X Y LSTICK RSTICK
// L R ZL ZR PLUS MINUS DPAD_LEFT DPAD_UP DPAD_RIGHT DPAD_DOWN; the overlay's controls.pad.* rows) = up to 4 buttons by
// TOTK_PRESS's names ("B,ZR"), "none" = unbound, unset = its own button; a button may serve several actions. The overlay
// and its L3 + R3 chord read the buttons as the readers give them, so no binding locks it out. per action: the buttons
// that give it (quiet: no log of unknown names); returns how many actions are rebound
static int padbind_sources(u64 src[16], bool quiet) {
    static const char* const acts[16] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                         "DPAD_LEFT", "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN"};
    int custom = 0;
    for (int a = 0; a < 16; a++) {
        src[a] = 1ull << a;
        char var[40];
        snprintf(var, sizeof var, "TOTK_PADBIND_%s", acts[a]);
        const char* v = getenv(var);
        if (!v || !*v) continue;
        custom++;
        src[a] = 0;
        for (const char* p = v; *p;) {
            const char* e = strchr(p, ',');
            std::string tok(p, e ? (size_t)(e - p) : strlen(p));
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            for (auto& c : tok) c = (char)toupper((unsigned char)c);
            bool known = tok.empty() || tok == "NONE";
            for (auto& nm : kPressNames)
                if (nm.bit < 16 && tok == nm.n) { src[a] |= 1ull << nm.bit; known = true; }
            if (!known && !quiet) printf("[hid] %s: unknown pad button \"%s\"\n", var, tok.c_str());
            if (!e) break;
            p = e + 1;
        }
    }
    return custom;
}
static u64 pad_remap(u64 pos) {
    static u64 to[16];   // per button: the actions it gives
    static int built;
    if (built != g_padmap_gen.load()) {
        built = g_padmap_gen.load();
        u64 src[16];     // per action: the buttons that give it
        int custom = padbind_sources(src, false);
        for (int b = 0; b < 16; b++) {
            to[b] = 0;
            for (int a = 0; a < 16; a++) if (src[a] >> b & 1) to[b] |= 1ull << a;
        }
        if (custom || built > 1) printf("[hid] pad buttons: %d action%s rebound\n", custom, custom == 1 ? "" : "s");
    }
    u64 out = pos & ~0xFFFFull;
    for (int b = 0; b < 16; b++) if (pos >> b & 1) out |= to[b];
    return out;
}

// Glyph bank: which bank glyph shows a game control (bank.py's SLOTS / MASKS names: A, L, PLUS, DPAD_UP, LS, LS_UPDOWN,
// RS_PUSH, RS_SHAKE,...) in a style, from the bindings in force: kbm the keyboard and mouse ones (the first key of each
// action; the stick as W A S D or the arrows when bound so, the right stick as the mouse while TOTK_MOUSE_CAMERA turns
// the camera), xbox / ps the pad ones (TOTK_PADBIND_*, face buttons by position or by label as the readers map them).
// The style in use follows the input used last (host_input): keys or the mouse kbm, an XInput pad xbox, a DualSense /
// DualShock 4 ps, a Nintendo pad the game's own; until one is used, the pads connected (hid_glyph_style).
// TOTK_GLYPHS=kbm|xbox|ps fixes it (TOTK_CTL "glyphs <style>|auto" too). Changes whenever a glyph may have changed: the
// style in use or a binding (the fs side re-patches then). With keys and mouse, the PC scheme's prompts follow the
// context: its signature changes only when that context's prompts differ from the last one's (a menu opening does not
// reset the font cache when its keys show the same). (the game's Jump / Sprint swap too: kbm_glyph trades B and X by it,
// and toggling it in Options changes no context, so the System tab's diagram would keep the old keys)
extern "C" unsigned hid_glyph_epoch(void) {
    const int style = hid_glyph_active();
    return ((unsigned)style | (unsigned)g_ctl_gen.load() << 2 | (unsigned)g_padmap_gen.load() << 17) ^
           (style == GlyphKbm ? pc_glyph_sig() << 2 : 0u) ^ (g_jump_swapped.load() ? 1u << 31 : 0u);
}
static const char* glyph_intern(const std::string& s) {
    static std::mutex m;
    static std::set<std::string> pool;
    std::lock_guard<std::mutex> l(m);
    return pool.insert(s).first->c_str();
}
static const char* vk_glyph(int vk) {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n(256);
        for (int c = 'A'; c <= 'Z'; c++) n[c] = std::string("keyboard_") + (char)(c - 'A' + 'a');
        for (int c = '0'; c <= '9'; c++) n[c] = std::string("keyboard_") + (char)c;
        for (int i = 0; i < 24; i++) n[VK_F1 + i] = "keyboard_f" + std::to_string(i + 1);   // (F13..F24: label caps)
        static const struct { int vk; const char* g; } t[] = {{VK_SPACE, "space"}, {VK_RETURN, "enter"},
            {VK_ESCAPE, "escape"}, {VK_TAB, "tab"}, {VK_BACK, "backspace"}, {VK_SHIFT, "shift"}, {VK_LSHIFT, "shift"},
            {VK_RSHIFT, "shift"}, {VK_CONTROL, "ctrl"}, {VK_LCONTROL, "ctrl"}, {VK_RCONTROL, "ctrl"}, {VK_MENU, "alt"},
            {VK_LMENU, "alt"}, {VK_RMENU, "alt"}, {VK_LEFT, "arrow_left"}, {VK_UP, "arrow_up"}, {VK_RIGHT, "arrow_right"},
            {VK_DOWN, "arrow_down"}, {VK_PRIOR, "page_up"}, {VK_NEXT, "page_down"}, {VK_HOME, "home"}, {VK_END, "end"},
            {VK_INSERT, "insert"}, {VK_DELETE, "delete"},
#ifdef _WIN32
            {VK_OEM_MINUS, "minus"}, {VK_OEM_PLUS, "equals"}, {VK_OEM_COMMA, "comma"}, {VK_OEM_PERIOD, "period"},
            {VK_OEM_2, "slash_forward"}, {VK_OEM_5, "slash_back"}, {VK_OEM_1, "semicolon"}, {VK_OEM_7, "apostrophe"},
            {VK_OEM_4, "bracket_open"}, {VK_OEM_6, "bracket_close"}, {VK_OEM_3, "tilde"}, {VK_CAPITAL, "capslock"},
            {VK_LWIN, "win"}, {VK_RWIN, "win"}, {VK_NUMLOCK, "numlock"}, {VK_SNAPSHOT, "printscreen"}, {VK_PAUSE, "pause"},
            {VK_SCROLL, "scroll_lock"}, {VK_ADD, "plus"}, {VK_SUBTRACT, "minus"},
            {VK_MULTIPLY, "asterisk"}, {VK_DIVIDE, "slash_forward"}, {VK_DECIMAL, "period"}, {VK_OEM_102, "bracket_less"},
            {VK_APPS, "menu"},
#endif
        };
        for (auto& e : t) n[e.vk] = std::string("keyboard_") + e.g;
#ifdef _WIN32
        for (int c = 0; c < 10; c++) n[VK_NUMPAD0 + c] = "keyboard_" + std::to_string(c);
#endif
        n[VK_LBUTTON] = "mouse_left"; n[VK_RBUTTON] = "mouse_right"; n[VK_MBUTTON] = "mouse_scroll";
        n[VK_XBUTTON1] = "mouse_side_back"; n[VK_XBUTTON2] = "mouse_side_forward";
        return n;
    }();
    // (the router's two wheel inputs past the 256 virtual keys, pc_input.cpp kWheelUp / kWheelDown: a rebound wheel)
    if (vk == 256) return "mouse_scroll_up";
    if (vk == 257) return "mouse_scroll_down";
    // any other key a binding can hold draws a plain key cap rather than the pad's button
    return vk > 0 && vk < 256 ? (names[vk].empty() ? "keyboard_any" : names[vk].c_str()) : nullptr;
}
#ifdef _WIN32
extern "C" bool pc_mouse_enabled(void);   // pc_mouse.cpp: TOTK_MOUSE_CAMERA turns the camera
#endif
// set: the PC scheme's glyph set to read (a PcCtx: PCX_WORLD the gameplay one), -1 the current context's
static const char* kbm_glyph(const char* c, int set = -1) {
    const Bindings b = build_bindings(true);
    auto key = [&](int a) -> const char* {
        if ((a == 1 || a == 2) && g_jump_swapped.load() && !pc_ctx_is_ui(set >= 0 ? set : pc_ctx())) a = 3 - a;   // (P5: B / X swapped)
        if (a < 16) {   // the PC scheme: the key the context routes to that button's action (0: the layout's; -1: none here)
            const int v = set >= 0 ? pc_glyph_vk_in(set, a) : pc_glyph_vk(a);
            if (v < 0) return nullptr;
            if (v > 0) return vk_glyph(v);
        }
        for (int v : b.vk[a]) if (const char* g = vk_glyph(v)) return g;
        return nullptr;
    };
    // the d-pad, whole or half: its keys as the context's table has them (the PC scheme's rows, else the layout's), drawn
    // as one composite when they form a known set (bank.py COMPOSITES / COMPOSITES_2 / PAIR_LABELS), else one key: the
    // scheme's on-foot d-pad is C / 2 / X / 1, not the layout presets' 1 / 2 / X / 3; the map's Switch is PageUp / PageDown
    const bool dpad = !strcmp(c, "DPAD"), ud = !strcmp(c, "DPAD_UPDOWN"), lr = !strcmp(c, "DPAD_LEFTRIGHT");
    if (dpad || ud || lr) {
        auto dk = [&](int a) -> int {
            const int v = set >= 0 ? pc_glyph_vk_in(set, a) : pc_glyph_vk(a);
            return v > 0 ? v : v < 0 ? 0 : b.vk[a][0];
        };
        const int u = dk(13), l = dk(12), d = dk(15), r = dk(14);
        static const struct { const char* g; int u, l, d, r; } whole[] = {
            {"kbm_dpad_scheme", 'C', '2', 'X', '1'}, {"kbm_dpad_pc", '1', '2', 'X', '3'}, {"kbm_dpad_classic", '2', '1', '4', '3'}};
        static const struct { const char* g; bool ud; int x, y; } half[] = {
            {"kbm_dpad_scheme_ud", true, 'C', 'X'}, {"kbm_dpad_scheme_lr", false, '2', '1'},
            {"kbm_dpad_pc_ud", true, '1', 'X'}, {"kbm_dpad_pc_lr", false, '2', '3'},
            {"kbm_dpad_classic_ud", true, '2', '4'}, {"kbm_dpad_classic_lr", false, '1', '3'},
            {"kbm_pgupdn", true, VK_PRIOR, VK_NEXT}, {"mouse_scroll_vertical", true, 256, 257}};   // (256 / 257: the wheel)
        if (dpad) {
            for (auto& w : whole) if (w.u == u && w.l == l && w.d == d && w.r == r) return w.g;
            return key(13);
        }
        for (auto& h : half) if (h.ud == ud && h.x == (ud ? u : l) && h.y == (ud ? d : r)) return h.g;
        return key(ud ? 13 : 12);
    }
    static const struct { const char* c; int a; } act[] = {{"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LS_PUSH", 4},
        {"RS_PUSH", 5}, {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"DPAD_LEFT", 12},
        {"DPAD_UP", 13}, {"DPAD_RIGHT", 14}, {"DPAD_DOWN", 15}, {"DPAD_UPDOWN", 13}, {"DPAD_LEFTRIGHT", 12},
        {"LS_LEFT", 16}, {"LS_UP", 17}, {"LS_RIGHT", 18}, {"LS_DOWN", 19}};
    for (auto& x : act) if (!strcmp(c, x.c)) return key(x.a);
    // the sticks: actions left, up, right, down at 16 (move) / 20 (camera)
    bool rs = !strncmp(c, "RS", 2);
    if (rs) {   // a right-stick direction the context's rows drive (PAUSE's Left / Right arrows: the inventory's Rotate)
        auto rk = [&](int a) { const int v = set >= 0 ? pc_glyph_vk_in(set, a) : pc_glyph_vk(a); return v > 0 ? v : 0; };
        const int l = rk(20), r = rk(22);
        if (l && r && (!strcmp(c, "RS") || !strcmp(c, "RS_LEFTRIGHT")))
            return l == VK_LEFT && r == VK_RIGHT ? "kbm_arrows_lr" : vk_glyph(l);
        if (l && !strcmp(c, "RS_LEFT")) return vk_glyph(l);
        if (r && !strcmp(c, "RS_RIGHT")) return vk_glyph(r);
    }
#ifdef _WIN32
    if (rs && pc_mouse_enabled()) {
        // (an item strip or the Autobuild list picks with the wheel only, 4.23 C: its "Select" draws the wheel, not the
        // mouse's move; the ability wheel keeps the move, the pointer picks there) (the set in force, pc_glyph_intent
        // 4.27)
        if (set >= 0 ? set == PCX_QUICKMENU && pc_quickmenu_sub() != 0 : pc_glyph_qm_strip()) return "mouse_scroll_vertical";
        if (!strcmp(c, "RS") || !strcmp(c, "RS_SHAKE")) return "mouse_move";
        return strstr(c, "UP") || strstr(c, "DOWN") ? "mouse_vertical" : "mouse_horizontal";
    }
#endif
    int base = rs ? 20 : 16;
    auto is = [&](int a, int vk) { return b.vk[base + a][0] == vk; };
    bool wasd = is(0, 'A') && is(1, 'W') && is(2, 'D') && is(3, 'S');
    bool arrows = is(0, VK_LEFT) && is(1, VK_UP) && is(2, VK_RIGHT) && is(3, VK_DOWN);
    const char* d = c + 2;
    if (!*d || !strcmp(d, "_SHAKE")) return wasd ? "kbm_wasd" : arrows ? "kbm_arrows" : key(base + 1);
    if (!strcmp(d, "_UPDOWN")) return wasd ? "kbm_ws" : arrows ? "kbm_arrows_ud" : key(base + 1);
    if (!strcmp(d, "_LEFTRIGHT")) return wasd ? "kbm_ad" : arrows ? "kbm_arrows_lr" : key(base);
    if (!strcmp(d, "_LEFT")) return key(base);
    if (!strcmp(d, "_UP")) return key(base + 1);
    if (!strcmp(d, "_RIGHT")) return key(base + 2);
    if (!strcmp(d, "_DOWN")) return key(base + 3);
    return nullptr;
}
static const char* pad_glyph(bool ps, const char* c) {
    static const bool labels = getenv("TOTK_PAD_LABELS") != nullptr;   // (as pad_read has it)
    // the reader's button bits -> the button: by position A = right, B = bottom, X = top, Y = left; by label the letter
    static const char* const face[2][2][4] = {
        {{"xbox_button_b", "xbox_button_a", "xbox_button_y", "xbox_button_x"},
         {"xbox_button_a", "xbox_button_b", "xbox_button_x", "xbox_button_y"}},
        {{"playstation_button_circle", "playstation_button_cross", "playstation_button_triangle", "playstation_button_square"},
         {"playstation_button_cross", "playstation_button_circle", "playstation_button_square", "playstation_button_triangle"}}};
    static const char* const rest[2][12] = {
        {"xbox_stick_l_press", "xbox_stick_r_press", "xbox_lb", "xbox_rb", "xbox_lt", "xbox_rt", "xbox_button_menu",
         "xbox_button_view", "xbox_dpad_left", "xbox_dpad_up", "xbox_dpad_right", "xbox_dpad_down"},
        {"playstation_stick_l_press", "playstation_stick_r_press", "playstation_trigger_l1", "playstation_trigger_r1",
         "playstation_trigger_l2", "playstation_trigger_r2", "playstation5_button_options", "playstation5_button_create",
         "playstation_dpad_left", "playstation_dpad_up", "playstation_dpad_right", "playstation_dpad_down"}};
    u64 src[16];
    padbind_sources(src, true);
    auto act = [&](int a) -> const char* {
        for (int bit = 0; bit < 16; bit++)
            if (src[a] >> bit & 1) return bit < 4 ? face[ps][labels][bit] : rest[ps][bit - 4];
        return nullptr;
    };
    static const struct { const char* c; int a; } acts[] = {{"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LS_PUSH", 4},
        {"RS_PUSH", 5}, {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"DPAD_LEFT", 12},
        {"DPAD_UP", 13}, {"DPAD_RIGHT", 14}, {"DPAD_DOWN", 15}};
    for (auto& x : acts) if (!strcmp(c, x.c)) return act(x.a);
    const std::string pre = ps ? "playstation_" : "xbox_";
    if (!strcmp(c, "DPAD")) return glyph_intern(pre + "dpad");
    if (!strcmp(c, "DPAD_UPDOWN")) return glyph_intern(pre + "dpad_vertical");
    if (!strcmp(c, "DPAD_LEFTRIGHT")) return glyph_intern(pre + "dpad_horizontal");
    if (strncmp(c, "LS", 2) && strncmp(c, "RS", 2)) return nullptr;
    std::string s = pre + "stick_" + (c[0] == 'L' ? "l" : "r");
    const char* d = c + 2;
    static const struct { const char* d; const char* g; } dirs[] = {{"", ""}, {"_SHAKE", ""}, {"_UP", "_up"},
        {"_DOWN", "_down"}, {"_LEFT", "_left"}, {"_RIGHT", "_right"}, {"_UPDOWN", "_vertical"}, {"_LEFTRIGHT", "_horizontal"}};
    for (auto& x : dirs) if (!strcmp(d, x.d)) return glyph_intern(s + x.g);
    return nullptr;
}
// The bank glyph for a control in a style (GlyphKbm / GlyphXbox / GlyphPs); null: none (the game's own glyph stays).
extern "C" const char* hid_glyph_for(int style, const char* control) {
    if (style == GlyphKbm) return kbm_glyph(control);
    if (style == GlyphXbox || style == GlyphPs) return pad_glyph(style == GlyphPs, control);
    return nullptr;
}
// The same in the PC scheme's glyph set `set` (pc_glyph_vk_in) for keys and mouse; the pads' do not depend on it
extern "C" const char* hid_glyph_for_set(int style, const char* control, int set) {
    return style == GlyphKbm ? kbm_glyph(control, set) : hid_glyph_for(style, control);
}
// what a reader added to the sample (the buttons it set, the sticks it moved)
static HostInput input_added(const HostInput& before, const HostInput& after) {
    HostInput d = {after.buttons & ~before.buttons, 0, 0, 0, 0};
    if (after.lx != before.lx || after.ly != before.ly) { d.lx = after.lx; d.ly = after.ly; }
    if (after.rx != before.rx || after.ry != before.ry) { d.rx = after.rx; d.ry = after.ry; }
    return d;
}
static int hidpad_glyph_style() {   // the raw-HID pads': PlayStation or Nintendo (the game's own)
    for (auto& p : g_hidpad)
        if (p.live && (p.kind == PadDualSense || p.kind == PadDS4)) return GlyphPs;
    return GlyphGame;
}
// host_input: the style of what was just used: a button going down, or a stick pushed past a third of its travel from
// rest (pc_device_switch.h stick_push: back under a fifth before it counts again, so a worn stick's noise at the edge
// does not). Edges, not levels: a key or button held on one device while another is used no longer flips the prompts
// back every 1-ms refresh (each device's own last state per style).
static void glyph_note(int style);
static bool stick_out(const HostInput& in) {
    const s32 dz = pcdev::kOut;
    return abs(in.lx) > dz || abs(in.ly) > dz || abs(in.rx) > dz || abs(in.ry) > dz;
}
static int stick_max(const HostInput& in) {
    return std::max(std::max(abs(in.lx), abs(in.ly)), std::max(abs(in.rx), abs(in.ry)));
}
static void glyph_note_use(int style, const HostInput& in) {
    static u64 was_b[4];
    static bool held[4];
    const int k = style & 3;
    const u64 down = in.buttons & 0xFFFF & ~was_b[k];
    const bool pushed = pcdev::stick_push(held[k], stick_max(in));
    was_b[k] = in.buttons & 0xFFFF;
    if (down || pushed) glyph_note(style);
}
// The style shown follows the uses through pc_device_switch.h's dwell (two devices used together: at most two changes a
// second). host_input's mutex; g_glyph_last is what the readers see (glyph_unplugged also moves it, from the hot-plug side).
static pcdev::DeviceSwitch g_glyph_dev;
static double glyph_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void glyph_show(int style) {
    if (g_glyph_last.exchange(style) == style) return;
    printf("[hid] glyphs: %s in use\n", style == GlyphKbm ? "keyboard / mouse" : style == GlyphXbox ? "XInput pad"
                                        : style == GlyphPs ? "PlayStation pad" : "Nintendo pad");
    pc_mouse_pad_in_use(style != GlyphKbm);
}
static void glyph_note(int style) {
    const double t = glyph_ms();
    g_glyph_dev.sync(g_glyph_last.load(), t);
    if (g_glyph_dev.note(style, t)) glyph_show(g_glyph_dev.cur);
    else if (style == g_glyph_dev.cur && style != GlyphKbm) pc_mouse_pad_in_use(true);   // (the pointer a nudge showed)
}
static void glyph_settle() {   // every refresh: a device that waited out the dwell takes over
    const double t = glyph_ms();
    g_glyph_dev.sync(g_glyph_last.load(), t);
    if (g_glyph_dev.settle(t)) glyph_show(g_glyph_dev.cur);
}

// Several guest threads poll the npad every frame: read the host at most every 1 ms and share the result.
// The refresh clock is steady_clock (QueryPerformanceCounter): GetTickCount64 advances in 15.6 ms steps, so a read
// could get input up to ~15 ms old (input latency, user 2026-10-15). TOTK_INPUT_TICK=1: the old tick clock (A/B).
// "[hid] input age" logs how old the returned sample was at each read (avg / max per 10 s, first minute).
static HostInput host_input() {
    static std::mutex mtx;
    static HostInput cur;
    static u64 last_ms = ~0ull;
    static const bool tick = getenv("TOTK_INPUT_TICK") != nullptr;
    static std::chrono::steady_clock::time_point last_t, log_t0 = std::chrono::steady_clock::now();
    static double age_sum, age_max;
    static u64 age_n, age_logs;
    std::lock_guard<std::mutex> l(mtx);
    u64 now = GetTickCount64();
    auto now_t = std::chrono::steady_clock::now();
    bool refresh = last_ms == ~0ull ||
        (tick ? now - last_ms >= 2 : std::chrono::duration<double, std::milli>(now_t - last_t).count() >= 1.0);
    if (!refresh) {
        double age = std::chrono::duration<double, std::milli>(now_t - last_t).count();
        age_sum += age; age_n++; if (age > age_max) age_max = age;
    } else age_n++;
    if (age_logs < 6 && std::chrono::duration<double>(now_t - log_t0).count() >= 10.0) {
        printf("[hid] input age at read: avg %.2f ms, max %.2f ms (%llu reads, %s clock)\n", age_n ? age_sum / age_n : 0.0, age_max,
               (unsigned long long)age_n, tick ? "tick" : "1 ms");
        age_sum = age_max = 0; age_n = 0; age_logs++; log_t0 = now_t;
    }
    if (refresh) {
        last_ms = now;
        last_t = now_t;
        HostInput in = {};
        pad_read(&in);
        // The prompts' style by device. Steam Input and DS4Windows also present a DualSense as an XInput pad, so one
        // press can come through both readers: what the raw-HID pads report themselves counts for them, not for XInput.
        HostInput hin = {};
        hidpad_read(&hin);
        // (Steam's XInput copy lags the raw report: a button the raw pad let go a refresh ago can still be down in the
        // copy, and would read as a new XInput press; so the raw pads' buttons and sticks of the last 50 ms are masked)
        static std::chrono::steady_clock::time_point raw_seen[16], raw_stick;
        for (int b = 0; b < 16; b++) if (hin.buttons >> b & 1) raw_seen[b] = now_t;
        if (stick_out(hin)) raw_stick = now_t;
        HostInput xonly = in;
        for (int b = 0; b < 16; b++)
            if (now_t - raw_seen[b] < std::chrono::milliseconds(50)) xonly.buttons &= ~(1ull << b);
        if (now_t - raw_stick < std::chrono::milliseconds(50)) xonly.lx = xonly.ly = xonly.rx = xonly.ry = 0;
        glyph_note_use(GlyphXbox, xonly);
        glyph_note_use(hidpad_glyph_style(), hin);
        hidpad_read(&in);
        in.buttons |= g_ctl_pad.load();
        g_f2_down = false;
        const u64 pad_buttons = in.buttons;   // (before the keyboard's buttons: the overlay has the keys themselves)
        in.buttons = pad_remap(in.buttons);   // (and before the bindings: the overlay stays reachable)
        const HostInput pin = in;
        keyboard_read(&in);
        glyph_note_use(GlyphKbm, input_added(pin, in));
#ifdef _WIN32
        if (pc_mouse_pointer_moved()) g_mouse_moved = true;   // (the released pointer: menus, the title, dialogues)
        if (g_mouse_moved) { g_mouse_moved = false; glyph_note(GlyphKbm); }
#endif
        glyph_settle();
#ifndef _WIN32
        oe_focus_update(SDL_GetKeyboardFocus() != nullptr);   // TOTK_PAUSE_UNFOCUSED (Windows: shim_win.c's WM_ACTIVATEAPP)
#endif
        // Graphics menu (hle_osd.cpp): L3 + R3 or F10; while it is open the game sees no input. With NXR_PC_OVERLAY=1
        // nxrecomp's settings overlay (pc_overlay.h) takes both instead.
        // TOTK_CTL with the overlay: read first, so its buttons reach the overlay too (L3 + R3 opens it from a script, then
        // they navigate it) and a press's release is seen while it is open; the game gets them only while it is closed.
        HostInput ctl_in = {};
        const bool ctl_first = pc_overlay_enabled();
        if (ctl_first) ctl_read(&ctl_in);
        bool osd_open = pc_overlay_enabled() ? pc_overlay_pad(pad_buttons | scripted_buttons() | ctl_in.buttons)
                                             : osd_menu_input(in.buttons | scripted_buttons(), g_f10_down);
        if (osd_open) in = {};
        {   // Shader warm-up screen (TOTK_WARMUP): A / B / + = play now, the warm-up keeps compiling in the background
            // (exec.c vk_precompile_play_now). That press is held back from the game until released, so it doesn't also
            // confirm the title menu.
            static bool held = true, swallow;   // (a button already down when the warm-up starts doesn't count)
            const u64 play = BtnA | BtnB | BtnPlus;
            bool down = ((in.buttons | scripted_buttons()) & play) != 0;
            if (down && !held && vk_precompile_warmup_active()) { vk_precompile_play_now(); swallow = true; }
            if (!down) swallow = false;
            held = down;
            if (swallow) in.buttons &= ~play;
        }
        cur = in;
        // Skip opening toggle while the title screen is up: F2 or Minus (the title menu has no use for Minus)
        static bool was = false;
        bool now_down = g_f2_down || ((in.buttons | scripted_buttons()) & BtnMinus);
        if (now_down && !was && g_title_phase) {
            g_skip_opening = !g_skip_opening;
            printf("[hid] skip opening %s\n", g_skip_opening ? "ON" : "OFF");
        }
        // In-game skip (user 09:5x: "u click a ui button in game and it takes u to when u wake up in the sky island"):
        // inside the opening stage, F2 or Minus held 1.5 s restarts the game into New Game with the skip on
        // (TOTK_AUTO_NEWGAME presses New Game -> Start; the skip overlay then ends the prologue at once).
        static u64 minus_since = 0;
        bool minus = ((in.buttons | scripted_buttons()) & BtnMinus) != 0;
        if (g_in_opening && minus) { if (!minus_since) minus_since = now; } else minus_since = 0;
        g_skip_hold_ms = minus_since ? (int)(now - minus_since) : 0;
        if (g_in_opening && ((g_f2_down && !was) || g_skip_hold_ms >= 1500)) skip_restart();
        was = now_down;
        if (!osd_open && ctl_first) {   // (ctl_read's own rule: its buttons OR-ed in, a stick it holds off centre wins)
            cur.buttons |= ctl_in.buttons;
            if (ctl_in.lx || ctl_in.ly) { cur.lx = ctl_in.lx; cur.ly = ctl_in.ly; }
            if (ctl_in.rx || ctl_in.ry) { cur.rx = ctl_in.rx; cur.ry = ctl_in.ry; }
        } else if (!osd_open) ctl_read(&cur);   // TOTK_CTL: after the chord and Minus logic above, so it reaches the game only
        screen_track(cur.buttons | scripted_buttons());   // (the map and menu keys' screen, for the next refresh)
        {   // TOTK_PC_TRACE=1: the buttons the game gets, at each change, with the PC scheme's context (stage tests)
            static const bool trace = getenv("TOTK_PC_TRACE") && atoi(getenv("TOTK_PC_TRACE")) != 0;
            static u64 last = ~0ull;
            static int lines;
            const u64 b = cur.buttons & 0xFFFF;
            if (trace && b != last && lines++ < 4000) printf("[pc] npad %04llx in %s\n", (unsigned long long)b, pc_ctx_name(pc_ctx()));
            last = b;
        }
        // TOTK_TITLE_FPS (hle_vi.cpp): each A or B once the title's stage is up (a confirm or cancel starts a transition)
        // opens a window of kTitleBurst presents at that rate; a menu or dialog still up after it waits at the cap
        static bool title_ab_was;
        bool title_ab = ((cur.buttons | scripted_buttons()) & (BtnA | BtnB)) != 0;
        if (g_title_phase && g_title_stage && title_ab && !title_ab_was) {
            uint64_t n = nvn_present_count();
            if (!g_title_burst_end) printf("[hid] title: first A/B at present %llu\n", (unsigned long long)n);
            g_title_burst_end = n + kTitleBurst;
        }
        title_ab_was = title_ab;
        // How long a refresh takes (every guest thread that polls the npad waits on mtx meanwhile): per 10 s, the reads,
        // the mean and the slowest, logged when one took 2 ms or more (r/decomps 11-01, v0.1.5: the game slowed down
        // while a fast mouse moved; the pad scan's 0.5-3.6 s stalls were found the same way)
        static double read_sum, read_max;
        static u64 read_n, read_logs;
        static std::chrono::steady_clock::time_point read_t0 = now_t;
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now_t).count();
        read_sum += took; read_n++; if (took > read_max) read_max = took;
        if (std::chrono::duration<double>(now_t - read_t0).count() >= 10.0) {
            if (read_max >= 2.0 && read_logs++ < 100)
                printf("[hid] input refresh: %llu in %.0f s, mean %.3f ms, slowest %.1f ms\n", (unsigned long long)read_n,
                       std::chrono::duration<double>(now_t - read_t0).count(), read_sum / read_n, read_max);
            read_sum = read_max = 0; read_n = 0; read_t0 = now_t;
        }
    }
    return cur;
}

// Quick menus at 60 fps (user 2026-10-17: "the wheels still make it all black"): with TOTK_FPS=60 the speed fix scales
// the game's interval ratio (main+0x7a83e4 this+8) to 0.5, and TotK then skips the 3D scene in the frame that captures
// the weapon / shield / material menu background (black menu; w4 log, A/B w5-w9: 30 fps, TOTK_SPEED_FIX=0 and a kept
// ratio all fix it, but the ratio also drives game speed). Those menus open on a held d-pad direction, so while one is
// held hle_vi.cpp runs Switch timing (interval 2, no scaling), and the game sees a new d-pad press only after it has been
// held 40 ms, so that timing is already in effect when the menu opens. TOTK_DPAD_30=0: off. Only for presses that can
// open a quick menu (user 2026-10-19: "it also occurs when i go into the regular menu and ultrahand. make sure it only
// activates in quick menus"): not while a pause menu or the map already holds the picture (no 3D scene drawn for a few
// frames; the game's time multiplier gives no such signal), not R + Left / Right / Down (Ultrahand rotation; R + Up =
// throw materials), and a hold that opened no menu (the scene keeps being drawn) drops Switch timing.
extern "C" { volatile int g_dpad_switch_timing = 0; }   // read by hle_vi.cpp (pacing + speed fix)
extern "C" uint64_t vk_scene_idle_frames(void);         // nvn/vulkan/exec.c: presents since the 3D scene was last drawn
extern "C" int vi_fps_setting(void), vi_unlock_setting(void);   // hle_vi.cpp: the frame-rate setting, live (display.fps_cap)
static u64 dpad_gate(u64 buttons) {
    static const bool on = !getenv("TOTK_DPAD_30") || strcmp(getenv("TOTK_DPAD_30"), "0");
    // (above the game's 30: a cap or unlocked, which the start-time TOTK_FPS read missed)
    const bool fast = vi_fps_setting() > 30 || vi_unlock_setting() != 0;
    if (!on || !fast) { g_dpad_switch_timing = 0; return buttons; }   // (a cap set to 30 mid-hold drops it too)
    // (Down = whistle, no quick menu: user 2026-10-19)
    const u64 dmask = buttons & BtnR ? BtnUp : BtnUp | BtnLeft | BtnRight;
    static u64 held_since, released_at;
    static int gated, opened;   // this hold: Switch timing on; a menu took the scene away
    u64 now = GetTickCount64();
    if (buttons & dmask) {
        if (!held_since) {
            held_since = now;
            u64 idle = vk_scene_idle_frames();
            gated = g_dpad_switch_timing || idle <= 4;
            opened = 0;
            printf("[dpad] press %llx: scene idle %llu frames -> %s\n", (unsigned long long)(buttons & dmask),
                   (unsigned long long)idle, gated ? "Switch timing" : "no gate (menu holds the picture)");
        }
        released_at = 0;
        // (gameplay itself shows idle 1 between a present and the next scene draw, every press)
        if (gated && !opened && now - held_since > 40 && vk_scene_idle_frames() >= 3) {
            opened = 1;
            printf("[dpad] menu opened %llu ms after the press\n", (unsigned long long)(now - held_since));
        }
        if (gated && !opened && now - held_since > 400) {
            gated = 0;
            printf("[dpad] no menu 400 ms after the press: Switch timing off for this hold\n");
        }
        g_dpad_switch_timing = gated;
        if (gated && now - held_since < 40) buttons &= ~dmask;
    } else {
        held_since = 0;
        if (!released_at) released_at = now;
        if (now - released_at > 150) g_dpad_switch_timing = 0;   // (taps and quick re-presses keep Switch timing briefly)
    }
    return buttons;
}
static void npad_fill(NpadState* s, u32 id, bool style_ok) {
    memset(s, 0, sizeof *s);
    s->sampling_number = g_sample++;
    if (!npad_connected(id) || !style_ok) return;
    HostInput in = host_input();
    u64 sb = osd_menu_is_open() ? 0 : scripted_buttons();
    if (sb >> kScriptStick) {
        const s32 M = 32767;
        auto dir = [&](int b) { return (sb >> (kScriptStick + b)) & 1 ? M : 0; };
        if (dir(0) | dir(1) | dir(2) | dir(3)) { in.ly = dir(0) - dir(1); in.lx = dir(3) - dir(2); }
        if (dir(4) | dir(5) | dir(6) | dir(7)) { in.ry = dir(4) - dir(5); in.rx = dir(7) - dir(6); }
        sb &= (1ull << kScriptStick) - 1;
    }
    s->attributes = 1;   // IsConnected
    s->buttons = dpad_gate(in.buttons | sb);
    s->lx = in.lx; s->ly = in.ly; s->rx = in.rx; s->ry = in.ry;
    // stick-as-direction bits 16-23 (Eden NpadButton StickLLeft..StickRDown), set past half deflection
    const s32 h = 16384;
    s->buttons |= (u64)(in.lx < -h) << 16 | (u64)(in.ly > h) << 17 | (u64)(in.lx > h) << 18 | (u64)(in.ly < -h) << 19 |
                  (u64)(in.rx < -h) << 20 | (u64)(in.ry > h) << 21 | (u64)(in.rx > h) << 22 | (u64)(in.ry < -h) << 23;
    g_cheat_keys = s->buttons;
}
// pc_mouse.cpp's late latch: the buttons the game was given last (any device, after the pad bindings; bit 9 ZR)
extern "C" uint64_t hid_game_buttons(void) { return g_cheat_keys.load(); }

HLE_AS(hid_InitializeNpad, "_ZN2nn3hid14InitializeNpadEv") { }
HLE_AS(hid_SetSupportedNpadIdType, "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm") { }
HLE_AS(hid_SetSupportedNpadStyleSet, "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") { }
HLE_AS(hid_SetNpadJoyHoldType, "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE") { }
HLE_AS(hid_DisconnectNpad, "_ZN2nn3hid14DisconnectNpadERKj") { }
HLE_AS(hid_GetNpadStyleSet, "_ZN2nn3hid15GetNpadStyleSetERKj") {
    c->x[0] = npad_connected(*(u32*)c->x[0]) ? StyleFullKey : 0;
}
// totk_menu_cursor.cpp: on the game thread, moves a menu's focus to the item under the pointer; the A / B its
// clicks give go to every connected npad
extern "C" u64 totk_menu_cursor_frame(void);
extern "C" void dbatch_probe_frame(void);
// GetNpadStates(State* out, int count, const u32& id) -> void in the SDK; fill min(count,1) entries P5: the Jump / Sprint
// option, read with the game's own getter game::ui::IsChangeJumpButton, at most every 500 ms, only with the PC scheme in a
// gameplay context (GameData is up then). The read runs on eui's thread, as a task posted to the UI hook (totk_ui_post:
// inside the guest bridge, where guest calls are verified): after the first load the game reads the pads on other threads
// only, so the earlier main-thread read never ran again and a toggle never reached the flag. A key without the UI hook
// (totk_ui_post false) falls back to the main-thread read where there is one. TOTK_JUMP_OPTION=0: never read (no swap).
#include "guest/guest.h"
namespace {
GUEST_FN(IsChangeJumpButton, 1001a3e83c, bool());
}
extern "C" void guest_call(Ctx* c, u64 addr);         // rt_main.cpp
extern "C" unsigned long os_main_thread_tid(void);    // hle_os_thread.cpp
static std::atomic<bool> g_jump_pending{false};
static void jump_option_set(int v, const char* where) {
    static std::atomic<bool> told{false};
    if (g_jump_swapped.exchange(v) != v || !told.exchange(true))
        printf("[pc] the game's Jump / Sprint option (read on %s): %s\n", where,
               v ? "swapped (the keyboard sends B for Space, X for Shift)" : "as shipped");
}
static void jump_option_task(void*, bool ran) {   // eui's thread inside the bridge when ran, else no guest call
    if (ran) jump_option_set(IsChangeJumpButton() ? 1 : 0, "the UI thread");
    g_jump_pending = false;
}
static void jump_option_poll(Ctx* c) {
    static const bool on = !getenv("TOTK_JUMP_OPTION") || strcmp(getenv("TOTK_JUMP_OPTION"), "0") != 0;
    static std::atomic<u64> next{0};
    static std::atomic<bool> busy{false};
    const int ctx = pc_ctx();
    if (!on || !pc_scheme_on() || pc_ctx_is_ui(ctx) || ctx == PCX_OVERLAY || ctx == PCX_LOADING || g_title_phase) return;
    const u64 now = GetTickCount64();
    if (now < next.load()) return;
    next = now + 500;
    if (!g_jump_pending.exchange(true)) {   // (one task at a time; it clears this when it has run, or not)
        if (totk_ui_post(jump_option_task, nullptr)) return;
        g_jump_pending = false;
    } else return;
#ifdef _WIN32
    // no UI hook: the main thread's own pad read, when the game makes one
    if ((unsigned long)GetCurrentThreadId() != os_main_thread_tid() || busy.exchange(true)) return;
    Ctx saved = *c;
    guest_call(c, totk_main(0x1a3e83c));
    const int v = (c->x[0] & 1) ? 1 : 0;
    *c = saved;
    jump_option_set(v, "the main thread");
    busy = false;
#else
    (void)c; (void)busy;  // (no main thread id here: a guest call from any pad thread is not safe)
#endif
}
#define NPAD_STATES(name, mangled, style)                                                           \
    HLE_AS(name, mangled) {                                                                         \
        jump_option_poll(c);                                                                        \
        NpadState* s = (NpadState*)c->x[0]; s32 n = (s32)c->x[1]; u32 id = *(u32*)c->x[2];          \
        const u64 mc = totk_menu_cursor_frame();                                                    \
        totk_player_state_frame();                                                                  \
        dbatch_probe_frame();                                                                       \
        for (s32 i = 0; i < n; i++) {                                                               \
            npad_fill(&s[i], id, (style) == StyleFullKey);                                          \
            if (s[i].attributes) s[i].buttons |= mc;                                                \
            if (s[i].attributes && totk_menu_cursor_rstick(&s[i].rx)) /* (the inventory model's drag) */ \
                s[i].buttons = (s[i].buttons & ~(1ull << 20 | 1ull << 22)) |                        \
                               (u64)(s[i].rx < -16383) << 20 | (u64)(s[i].rx > 16383) << 22;          \
        }                                                                                           \
    }
NPAD_STATES(hid_GetNpadStatesFullKey, "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", StyleFullKey)
NPAD_STATES(hid_GetNpadStatesJoyDual, "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", StyleJoyDual)
NPAD_STATES(hid_GetNpadStatesJoyLeft, "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", 0)
NPAD_STATES(hid_GetNpadStatesJoyRight, "_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", 0)
NPAD_STATES(hid_GetNpadStatesHandheld, "_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", StyleHandheld)

// handles: u32 {type, npad id, device idx} packed; any stable non-zero value works for our own getters
HLE_AS(hid_GetSixAxisSensorHandles, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    u32* h = (u32*)c->x[0]; s32 n = (s32)c->x[1]; u32 id = *(u32*)c->x[2];
    for (s32 i = 0; i < n; i++) h[i] = 0x10000u | (id << 8) | (u32)i;
    c->x[0] = (u64)n;
}
HLE_AS(hid_GetVibrationDeviceHandles, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    u32* h = (u32*)c->x[0]; s32 n = (s32)c->x[1]; u32 id = *(u32*)c->x[2];
    for (s32 i = 0; i < n; i++) h[i] = 0x20000u | (id << 8) | (u32)i;
    c->x[0] = (u64)n;
}
HLE_AS(hid_StartSixAxisSensor, "_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE") { }
HLE_AS(hid_InitializeVibrationDevice, "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE") { }
// ---- rumble (user 2026-10-15: "rumble support for xbox dualsense and pro, with hd rumble on dual and pro") The game
// drives nn::hid's vibration node graph: VibrationPlayer (.bnvib clips), VibrationWriter (direct values),
// VibrationMixer, VibrationTarget (one device handle), joined by VibrationNodeConnection (with a modulation), plus
// SendVibrationValue. The SDK classes are polymorphic and their constructors set an SDK vtable that the game calls
// through later (its connection pool releases connections with a virtual destructor call, slot 0: a no-op constructor
// left the pool's free-list link there -> "indirect call to unknown target" on the first hit rumble, play.log 22:38).
// Each class gets a host vtable in (identity-mapped) guest memory; all node state stays host-side and the graph is
// evaluated by rumble_thread at 200 Hz (wall clock), not by the guest's VibrationNode::Update(). Value per side (device
// index 0 = left, 1 = right) -> pads: Xbox (XInput): low band -> left (big) motor, high band -> right motor, Eden/yuzu
// SDL curve (a + a^0.35) / 2 with their frequency attenuation. DualSense: the same two motor values as classic rumble
// (output report 0x02 USB / 0x31 Bluetooth). TOTK_DS_HAPTICS=1 (Bluetooth only, EXPERIMENTAL: SAxense report 0x32, not
// confirmed on Windows) streams both bands of each side as a 3 kHz waveform to the voice-coil actuators instead (HD
// rumble). Pro Controller / Joy-Con: HD rumble (output report 0x10) with the game's own frequencies and amplitudes.
// TOTK_RUMBLE=<gain> (default 1, 0 = off). BNVIB samples (observed): 4 bytes [amp lo, freq lo, amp hi, freq hi] at 200
// Hz after a 12-byte header (04 00 00 00 03 00 c8 00 <u32 data bytes>); amp = b / 255, freq = 10 * 2^(b / 32) (0x80 =
// 160 Hz, 0xA0 = 320 Hz = the SDK defaults; inferred from the data, not documented).
namespace vib {
struct Val { float alo = 0, flo = 160, ahi = 0, fhi = 320; };
enum Cls { Conn, Target, Mixer, Player, Writer, NCls };
static const char* const kClsName[NCls] = {"VibrationNodeConnection", "VibrationTarget", "VibrationMixer", "VibrationPlayer", "VibrationWriter"};
constexpr int kSlots = 12;
static u64 g_vt[NCls][kSlots];
struct Conn_ { u64 src = 0, dst = 0; float mod[4] = {1, 1, 1, 1}; };
struct Play_ { u64 data = 0, size = 0, t0 = 0; bool loop = false, playing = false; s32 pos = 0; float speed = 1.0f; };
static std::mutex g_mu;
static std::unordered_map<u64, int> g_cls;       // node -> class (from its constructor)
static std::unordered_map<u64, Conn_> g_conn;
static std::unordered_map<u64, Play_> g_play;
static std::unordered_map<u64, Val> g_writer;    // VibrationWriter -> last written value
static std::unordered_map<u64, u32> g_target;    // VibrationTarget -> device handle
static Val g_direct[2];                          // SendVibrationValue per side
static u64 g_direct_t[2];
static std::atomic<u64> g_updates{0};

static u64 now_us() { return (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void forget(u64 a) { g_cls.erase(a); g_conn.erase(a); g_play.erase(a); g_writer.erase(a); g_target.erase(a); }

template <int C, int S> static void slot(Ctx* c) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) printf("[hid] vibration %s vtable slot %d called (this %llx)\n", kClsName[C], S, (unsigned long long)c->x[0]);
    if (S <= 1) {                                         // D1 / D0 (deleting: the object memory is left to the game's pool/heap)
        std::lock_guard<std::mutex> l(g_mu);
        forget(c->x[0]);
    }
    c->x[0] = 0;
}
template <int C, int... S> static void fill(std::integer_sequence<int, S...>) { ((g_vt[C][S] = rt_host_fn_addr(slot<C, S>)), ...); }
static u64 vtable(int c) {
    static std::once_flag f;
    std::call_once(f, [] {
        fill<Conn>(std::make_integer_sequence<int, kSlots>{}); fill<Target>(std::make_integer_sequence<int, kSlots>{});
        fill<Mixer>(std::make_integer_sequence<int, kSlots>{}); fill<Player>(std::make_integer_sequence<int, kSlots>{});
        fill<Writer>(std::make_integer_sequence<int, kSlots>{});
    });
    return (u64)&g_vt[c][0];
}

struct Clip { const u8* s = nullptr; u32 n = 0, rate = 200; };
static Clip clip(const Play_& p) {
    Clip c;
    if (!p.data || p.size < 12) return c;
    const u8* b = (const u8*)p.data;
    u32 meta = *(const u32*)b;                           // 4 = format + rate (0xC with loop points)
    if (meta < 4 || meta > 64 || 8ull + meta > p.size) return c;
    u16 rate = *(const u16*)(b + 6);
    if (rate >= 50 && rate <= 1000) c.rate = rate;
    u64 bytes = *(const u32*)(b + 4 + meta), avail = p.size - (8 + meta);
    c.s = b + 8 + meta; c.n = (u32)((bytes < avail ? bytes : avail) / 4);
    return c;
}
static double play_pos(const Play_& p, const Clip& k, u64 now) { return p.pos + (double)(now - p.t0) * 1e-6 * k.rate * p.speed; }
static Val player_val(const Play_& p, u64 now) {
    Val v;
    if (!p.playing) return v;
    Clip k = clip(p);
    if (!k.n) return v;
    double i = play_pos(p, k, now);
    if (i < 0) return v;
    u64 n = (u64)i;
    if (n >= k.n) { if (!p.loop) return v; n %= k.n; }
    const u8* q = k.s + 4 * n;
    v.alo = q[0] / 255.0f; v.flo = 10.0f * exp2f(q[1] / 32.0f); v.ahi = q[2] / 255.0f; v.fhi = 10.0f * exp2f(q[3] / 32.0f);
    return v;
}
static void mix(Val& o, const Val& v) {                   // per band: the stronger input wins (amplitude and frequency)
    if (v.alo > o.alo) { o.alo = v.alo; o.flo = v.flo; }
    if (v.ahi > o.ahi) { o.ahi = v.ahi; o.fhi = v.fhi; }
}
static Val eval(u64 node, u64 now, int depth) {
    Val o;
    if (depth > 8) return o;
    auto c = g_cls.find(node);
    int k = c == g_cls.end() ? -1 : c->second;
    if (k == Player) { auto p = g_play.find(node); return p == g_play.end() ? o : player_val(p->second, now); }
    if (k == Writer) { auto w = g_writer.find(node); return w == g_writer.end() ? o : w->second; }
    for (auto& [a, cn] : g_conn) {                        // Mixer / Target: inputs = connections ending here
        if (cn.dst != node || !cn.src || cn.src == node) continue;
        Val v = eval(cn.src, now, depth + 1);
        v.alo *= cn.mod[0]; v.flo *= cn.mod[1]; v.ahi *= cn.mod[2]; v.fhi *= cn.mod[3];
        mix(o, v);
    }
    return o;
}
static void gather(Val out[2], u64 now) {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [t, h] : g_target) mix(out[(h & 0xFF) ? 1 : 0], eval(t, now, 0));
    for (int s = 0; s < 2; s++)
        if (g_direct_t[s] && now - g_direct_t[s] < 2000000) mix(out[s], g_direct[s]);   // (a lost "stop" ends after 2 s)
}
static Val read_val(u64 a) {                              // guest VibrationValue {amp lo, freq lo, amp hi, freq hi}
    const float* f = (const float*)a;
    Val v;
    auto ok = [](float x, float hi) { return x >= 0 && x <= hi; };
    if (ok(f[0], 1.5f)) v.alo = f[0];
    if (ok(f[1], 2000)) v.flo = f[1];
    if (ok(f[2], 1.5f)) v.ahi = f[2];
    if (ok(f[3], 2000)) v.fhi = f[3];
    return v;
}
static void logv(const char* what, u64 self, const float* f, std::atomic<int>& n) {
    if (n++ < 6) printf("[hid] %s %llx: %.3f %.1f %.3f %.1f\n", what, (unsigned long long)self, f[0], f[1], f[2], f[3]);
}
}  // namespace vib

HLE_AS(hid_SendVibrationValue, "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE") {
    static std::atomic<int> n{0};
    vib::logv("SendVibrationValue", *(u32*)c->x[0], (const float*)c->x[1], n);
    int s = (*(u32*)c->x[0] & 0xFF) ? 1 : 0;
    std::lock_guard<std::mutex> l(vib::g_mu);
    vib::g_direct[s] = vib::read_val(c->x[1]); vib::g_direct_t[s] = vib::now_us();
}
#define VIB_CTOR(id, sym, cls) HLE_AS(id, sym) { \
    *(u64*)c->x[0] = vib::vtable(vib::cls); std::lock_guard<std::mutex> l(vib::g_mu); vib::forget(c->x[0]); vib::g_cls[c->x[0]] = vib::cls; }
VIB_CTOR(hid_VibConnC1, "_ZN2nn3hid23VibrationNodeConnectionC1Ev", Conn)
VIB_CTOR(hid_VibTargetC1, "_ZN2nn3hid15VibrationTargetC1Ev", Target)
VIB_CTOR(hid_VibMixerC2, "_ZN2nn3hid14VibrationMixerC2Ev", Mixer)
VIB_CTOR(hid_VibPlayerC2, "_ZN2nn3hid15VibrationPlayerC2Ev", Player)
VIB_CTOR(hid_VibWriterC1, "_ZN2nn3hid15VibrationWriterC1Ev", Writer)
#undef VIB_CTOR
HLE_AS(hid_VibMixerD2, "_ZN2nn3hid14VibrationMixerD2Ev") { std::lock_guard<std::mutex> l(vib::g_mu); vib::forget(c->x[0]); }
HLE_AS(hid_VibPlayerD2, "_ZN2nn3hid15VibrationPlayerD2Ev") { std::lock_guard<std::mutex> l(vib::g_mu); vib::forget(c->x[0]); }
HLE_AS(hid_VibConnConnect, "_ZN2nn3hid23VibrationNodeConnection7ConnectEPNS0_13VibrationNodeES3_") {
    static std::atomic<int> n{0};
    std::lock_guard<std::mutex> l(vib::g_mu);
    auto& cn = vib::g_conn[c->x[0]]; cn.src = c->x[1]; cn.dst = c->x[2];
    if (n++ < 8) {   // measurement: which node classes the game wires, and whether it replaced our vtables (derived classes)
        auto cls = [](u64 a) { auto i = vib::g_cls.find(a); return i == vib::g_cls.end() ? "?" : vib::kClsName[i->second]; };
        auto own = [](u64 a) { auto i = vib::g_cls.find(a); return i != vib::g_cls.end() && *(u64*)a == vib::vtable(i->second); };
        printf("[hid] vibration Connect %llx: %s %llx (%s vtable) -> %s %llx (%s vtable)\n", (unsigned long long)c->x[0], cls(c->x[1]),
               (unsigned long long)c->x[1], own(c->x[1]) ? "our" : "game", cls(c->x[2]), (unsigned long long)c->x[2], own(c->x[2]) ? "our" : "game");
    }
    c->x[0] = 1;
}
HLE_AS(hid_VibConnDisconnect, "_ZN2nn3hid23VibrationNodeConnection10DisconnectEv") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto i = vib::g_conn.find(c->x[0]); if (i != vib::g_conn.end()) i->second.src = i->second.dst = 0;
}
HLE_AS(hid_VibConnSetModulation, "_ZN2nn3hid23VibrationNodeConnection13SetModulationERKNS0_19VibrationModulationE") {
    static std::atomic<int> n{0};
    const float* f = (const float*)c->x[1];
    vib::logv("SetModulation", c->x[0], f, n);
    std::lock_guard<std::mutex> l(vib::g_mu);
    auto& cn = vib::g_conn[c->x[0]];
    for (int i = 0; i < 4; i++) cn.mod[i] = f[i] >= 0 && f[i] <= 16 ? f[i] : 1.0f;
}
HLE_AS(hid_VibNodeUpdate, "_ZN2nn3hid13VibrationNode6UpdateEv") { vib::g_updates++; }
// game classes derived from Mixer / Player call these from their own OnNextSampleRequired: hand them our value
HLE_AS(hid_VibMixerSample, "_ZN2nn3hid14VibrationMixer20OnNextSampleRequiredEPNS0_14VibrationValueEPNS_4util13IntrusiveListINS0_23VibrationNodeConnectionENS4_27IntrusiveListBaseNodeTraitsIS6_S6_EEEE") {
    std::lock_guard<std::mutex> l(vib::g_mu); *(vib::Val*)c->x[1] = vib::eval(c->x[0], vib::now_us(), 0);
}
HLE_AS(hid_VibPlayerSample, "_ZN2nn3hid15VibrationPlayer20OnNextSampleRequiredEPNS0_14VibrationValueEPNS_4util13IntrusiveListINS0_23VibrationNodeConnectionENS4_27IntrusiveListBaseNodeTraitsIS6_S6_EEEE") {
    std::lock_guard<std::mutex> l(vib::g_mu); *(vib::Val*)c->x[1] = vib::eval(c->x[0], vib::now_us(), 0);
}
HLE_AS(hid_VibTargetSetHandle, "_ZN2nn3hid15VibrationTarget24SetVibrationDeviceHandleERKNS0_21VibrationDeviceHandleE") {
    static std::atomic<int> n{0};
    u32 h = *(u32*)c->x[1];
    if (n++ < 4) printf("[hid] VibrationTarget %llx -> handle %x\n", (unsigned long long)c->x[0], h);
    std::lock_guard<std::mutex> l(vib::g_mu); vib::g_target[c->x[0]] = h;
}
HLE_AS(hid_VibTargetUnsetHandle, "_ZN2nn3hid15VibrationTarget26UnsetVibrationDeviceHandleEv") { std::lock_guard<std::mutex> l(vib::g_mu); vib::g_target.erase(c->x[0]); }
HLE_AS(hid_VibWriterWrite, "_ZN2nn3hid15VibrationWriter5WriteERKNS0_14VibrationValueE") {
    static std::atomic<int> n{0};
    vib::logv("VibrationWriter::Write", c->x[0], (const float*)c->x[1], n);
    std::lock_guard<std::mutex> l(vib::g_mu); vib::g_writer[c->x[0]] = vib::read_val(c->x[1]);
}
HLE_AS(hid_VibPlayerLoad, "_ZN2nn3hid15VibrationPlayer4LoadEPKvm") {
    static std::atomic<int> logged{0};
    if (logged++ < 2) {
        const u8* b = (const u8*)c->x[1]; char hex[3 * 32 + 1] = {};
        for (u64 i = 0; i < c->x[2] && i < 32; i++) sprintf(hex + 3 * i, "%02x ", b[i]);
        printf("[hid] VibrationPlayer::Load %llx size %llu: %s\n", (unsigned long long)c->x[0], (unsigned long long)c->x[2], hex);
    }
    std::lock_guard<std::mutex> l(vib::g_mu); auto& p = vib::g_play[c->x[0]]; p.data = c->x[1]; p.size = c->x[2]; p.playing = false; p.pos = 0;
    c->x[0] = 0;                                           // nn::Result success
}
HLE_AS(hid_VibPlayerUnload, "_ZN2nn3hid15VibrationPlayer6UnloadEv") { std::lock_guard<std::mutex> l(vib::g_mu); vib::g_play.erase(c->x[0]); }
HLE_AS(hid_VibPlayerIsLoaded, "_ZNK2nn3hid15VibrationPlayer8IsLoadedEv") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto i = vib::g_play.find(c->x[0]); c->x[0] = i != vib::g_play.end() && i->second.data ? 1 : 0;
}
HLE_AS(hid_VibPlayerPlay, "_ZN2nn3hid15VibrationPlayer4PlayEv") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto& p = vib::g_play[c->x[0]]; p.playing = true; p.t0 = vib::now_us();
}
HLE_AS(hid_VibPlayerStop, "_ZN2nn3hid15VibrationPlayer4StopEv") { std::lock_guard<std::mutex> l(vib::g_mu); vib::g_play[c->x[0]].playing = false; }
HLE_AS(hid_VibPlayerIsPlaying, "_ZNK2nn3hid15VibrationPlayer9IsPlayingEv") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto it = vib::g_play.find(c->x[0]);
    bool on = false;
    if (it != vib::g_play.end() && it->second.playing) {
        vib::Clip k = vib::clip(it->second);
        on = it->second.loop || vib::play_pos(it->second, k, vib::now_us()) < k.n;
    }
    c->x[0] = on ? 1 : 0;
}
HLE_AS(hid_VibPlayerSetLoop, "_ZN2nn3hid15VibrationPlayer7SetLoopEb") { std::lock_guard<std::mutex> l(vib::g_mu); vib::g_play[c->x[0]].loop = (c->x[1] & 1) != 0; }
HLE_AS(hid_VibPlayerIsLoop, "_ZNK2nn3hid15VibrationPlayer6IsLoopEv") { std::lock_guard<std::mutex> l(vib::g_mu); c->x[0] = vib::g_play[c->x[0]].loop ? 1 : 0; }
HLE_AS(hid_VibPlayerSetPos, "_ZN2nn3hid15VibrationPlayer18SetCurrentPositionEi") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto& p = vib::g_play[c->x[0]]; p.pos = (s32)c->x[1]; p.t0 = vib::now_us();
}
HLE_AS(hid_VibPlayerSetSpeed, "_ZN2nn3hid15VibrationPlayer12SetPlaySpeedEf") {
    std::lock_guard<std::mutex> l(vib::g_mu); auto& p = vib::g_play[c->x[0]];
    u64 now = vib::now_us();
    if (p.playing) { p.pos = (s32)vib::play_pos(p, vib::clip(p), now); p.t0 = now; }   // keep the position across the change
    float f = F(c, 0); p.speed = f >= 0 && f <= 16 ? f : 1.0f;
}

// ---- rumble output (200 Hz)
// TOTK_CTL "rumble <lo> <hi> <s>": both sides as one SendVibrationValue (160 / 320 Hz) every 0.5 s for s seconds, then a
// stop: the output path (graph mix, motor curve, the pads' reports) checked without the game playing an effect
static void vib_test(float lo, float hi, double secs) {
    std::thread([=] {
        const u64 end = vib::now_us() + (u64)(secs * 1e6);
        vib::Val v; v.alo = fminf(lo, 1.0f); v.ahi = fminf(hi, 1.0f);
        for (u64 t; (t = vib::now_us()) < end; std::this_thread::sleep_for(std::chrono::milliseconds(500))) {
            std::lock_guard<std::mutex> l(vib::g_mu);
            for (int s = 0; s < 2; s++) { vib::g_direct[s] = v; vib::g_direct_t[s] = t; }
        }
        std::lock_guard<std::mutex> l(vib::g_mu);
        for (int s = 0; s < 2; s++) { vib::g_direct[s] = vib::Val(); vib::g_direct_t[s] = vib::now_us(); }
    }).detach();
}
#ifdef _WIN32
// the virtual XInput pad's rumble (TOTK_CTL "vpad xbox") is recorded and logged instead of sent
static void xset_any(XInputSetState_t xset, DWORD slot, XVib* v) {
    if (slot == kVxSlot) {
        std::lock_guard<std::mutex> l(g_vx.mu);
        if (g_vx.on) {
            static int logs;
            g_vx.sets++;
            if ((v->l != g_vx.rl || v->r != g_vx.rr) && logs++ < 40) printf("[vpad] xbox rumble: left %u right %u\n", v->l, v->r);
            g_vx.rl = v->l; g_vx.rr = v->r;
            return;
        }
    }
    xset(slot, v);
}
static XInputSetState_t xinput_set_load() {
    for (const char* n : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(n))
            if (auto f = (XInputSetState_t)GetProcAddress(m, "XInputSetState")) return f;
    return nullptr;
}
#endif
// TOTK_RUMBLE: strength, 0 = off, 1 = the game's own (default), up to 4. The settings overlay's controls.rumble row
// (k_ctl_rows) sets it live: rumble_reload re-reads it and the rumble thread picks it up at its next 5-ms tick.
static std::atomic<float> g_rumble_gain{-1.0f};
static void rumble_reload() {
    const char* e = getenv("TOTK_RUMBLE");
    float v = e && *e ? (float)atof(e) : 1.0f;
    g_rumble_gain = v < 0 ? 0.0f : v > 4 ? 4.0f : v;
}
static float rumble_gain() {
    if (g_rumble_gain.load() < 0) rumble_reload();
    return g_rumble_gain.load();
}
// Eden/yuzu SDL rumble for 2-motor pads: frequency attenuation, then (a + a^0.35) / 2 (w31)
static float motor_amp(float a, float f, bool high) {
    float att = high ? 1 - (f - 200) / 700 : 1 - (f - 140) / 400;
    att = att < 0.3f ? 0.3f : att > 1 ? 1.0f : att;
    a *= att;
    if (!(a > 0)) return 0;
    a = (a + powf(a, 0.35f)) * 0.5f;
    return a > 1 ? 1.0f : a;
}
// Pro Controller / Joy-Con HD rumble, one actuator (w31: dekuNukem / yuzu encoding). Neutral = 00 01 40 40.
static void pro_encode(u8* b, const vib::Val& v, float gain) {
    float alo = v.alo * gain, ahi = v.ahi * gain, sum = alo + ahi;
    if (sum > 1) { alo /= sum; ahi /= sum; }               // yuzu: keep the two bands from overdriving the actuator
    auto fidx = [](float f) { float i = roundf(log2f(fmaxf(f, 10.0f) / 10.0f) * 32.0f); return i > 255 ? 255 : (int)i; };
    auto aidx = [](float a) -> int {                       // amplitude code 0..100 (101-entry table)
        if (!(a > 0)) return 0;
        float i = a > 0.23f ? log2f(a * 8.7f) * 32 : a > 0.12f ? log2f(a * 17) * 16 : a / 0.12f * 16.46f;
        i = roundf(i);
        return i < 0 ? 0 : i > 100 ? 100 : (int)i;
    };
    int hf = (fidx(v.fhi) - 0x60) * 4, lf = fidx(v.flo) - 0x40;
    hf = hf < 0 ? 0 : hf > 0x1FF ? 0x1FF : hf;
    lf = lf < 0 ? 0 : lf > 0x7F ? 0x7F : lf;
    int ih = aidx(ahi), il = aidx(alo);
    int hf_amp = 2 * ih, lf_amp = ((il & 1) << 15) | (0x40 + (il >> 1));
    b[0] = (u8)(hf & 0xFF); b[1] = (u8)(hf_amp | ((hf >> 8) & 1)); b[2] = (u8)(lf | ((lf_amp >> 8) & 0x80)); b[3] = (u8)(lf_amp & 0xFF);
}
// DualSense Bluetooth haptics (SAxense, egormanga, read 2026-10-15): report 0x32, 142 bytes: [1] tag | seq << 4,
// packet 0x91 len 7 {FE 00 00 00 00 FF n} (n counts reports), packet 0x92 len 64 = 32 stereo int8 frames at 3 kHz
// (one report per 10.67 ms), CRC-32 of 0xA2 + bytes 0..137 at 138..141.
#ifdef _WIN32
struct Haptic { float ph[2][2] = {}; u8 seq = 0, n = 0; u64 next = 0; };
static void ds_haptics(HidPad* p, HANDLE w, const vib::Val v[2], float gain, Haptic* hs) {
    u8 b[142] = {};
    b[0] = 0x32; b[1] = (u8)((hs->seq++ & 15) << 4);
    u8* q = b + 2;
    const u8 p11[9] = {0x91, 7, 0xFE, 0, 0, 0, 0, 0xFF, hs->n++};
    memcpy(q, p11, 9);
    q[9] = 0x92; q[10] = 64;
    s8* smp = (s8*)(q + 11);
    for (int s = 0; s < 2; s++) {
        float alo = v[s].alo * gain, ahi = v[s].ahi * gain, sum = alo + ahi;
        if (sum > 1) { alo /= sum; ahi /= sum; }
        float dlo = 6.2831853f * v[s].flo / 3000.0f, dhi = 6.2831853f * v[s].fhi / 3000.0f;
        for (int i = 0; i < 32; i++) {
            float x = alo * sinf(hs->ph[s][0]) + ahi * sinf(hs->ph[s][1]);
            hs->ph[s][0] = fmodf(hs->ph[s][0] + dlo, 6.2831853f); hs->ph[s][1] = fmodf(hs->ph[s][1] + dhi, 6.2831853f);
            smp[2 * i + s] = (s8)lrintf(fmaxf(-1.0f, fminf(1.0f, x)) * 127.0f);
        }
    }
    u8 t[143]; t[0] = 0xA2; memcpy(t + 1, b, 138);
    u32 c = crc32_buf(t, 139);
    b[138] = (u8)c; b[139] = (u8)(c >> 8); b[140] = (u8)(c >> 16); b[141] = (u8)(c >> 24);
    if (!hid_write(w, b, 142, p->outlen)) {
        static std::atomic<int> told{0};
        if (told++ < 3) printf("[hid] DualSense haptics report 0x32 failed (%lu)\n", GetLastError());
    }
}
#endif
#ifdef _WIN32
static void rumble_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    XInputSetState_t xset = xinput_set_load();
    float gain = rumble_gain();   // (re-read every tick below: the overlay's controls.rumble row)
    const bool haptics = ds_haptics_on();
    printf("[hid] rumble: gain %.2f, XInput %s, DualSense %s\n", gain, xset ? "yes" : "no", haptics ? "HD haptics over Bluetooth (experimental)" : "classic rumble");
    struct Sent { u8 b[8] = {}; u64 t = 0; bool nz = false; } sent[4], xs_sent;
    Haptic hap[4];
    int first = 0;
    u64 upd_t = vib::now_us(), upd_logs = 0, out_n = 0;
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -50000;          // 5 ms
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 20);
        else Sleep(5);
        u64 now = vib::now_us();
        if (upd_logs < 3 && now - upd_t >= 10000000) {     // measurement: how often the game runs VibrationNode::Update
            printf("[hid] rumble: VibrationNode::Update %.1f/s, outputs %.1f/s\n", vib::g_updates.exchange(0) / ((now - upd_t) * 1e-6),
                   out_n / ((now - upd_t) * 1e-6));
            upd_t = now; upd_logs++; out_n = 0;
        }
        vib::Val v[2];
        gain = rumble_gain();
        if (gain > 0) vib::gather(v, now);
        float lo = fminf(1.0f, fmaxf(motor_amp(v[0].alo, v[0].flo, false), motor_amp(v[1].alo, v[1].flo, false)) * gain);
        float hi = fminf(1.0f, fmaxf(motor_amp(v[0].ahi, v[0].fhi, true), motor_amp(v[1].ahi, v[1].fhi, true)) * gain);
        bool any = lo > 0.004f || hi > 0.004f;
        if (any && first < 3) {
            first++;
            printf("[hid] rumble: L %.2f@%.0f/%.2f@%.0f R %.2f@%.0f/%.2f@%.0f -> motors %.2f %.2f\n", v[0].alo, v[0].flo, v[0].ahi, v[0].fhi,
                   v[1].alo, v[1].flo, v[1].ahi, v[1].fhi, lo, hi);
        }
        // Xbox: on change (at most 100/s) and a 1-s refresh while on
        int xs = g_xslot;
        if (xset && xs >= 0) {
            XVib xv = {(WORD)lrintf(lo * 65535), (WORD)lrintf(hi * 65535)};
            bool ch = memcmp(xs_sent.b, &xv, 4) != 0;
            if ((ch && now - xs_sent.t >= 10000) || (any && now - xs_sent.t >= 1000000)) {
                xset_any(xset, (DWORD)xs, &xv); memcpy(xs_sent.b, &xv, 4); xs_sent.t = now; out_n++;
            }
        }
        for (int i = 0; i < 4; i++) {
            HidPad* p = &g_hidpad[i];
            HANDLE w = p->wr;
            int k = p->kind;
            if (!p->live || w == INVALID_HANDLE_VALUE) continue;
            Sent& s = sent[i];
            if (k == PadDualSense && haptics && p->bt) {    // stream continuously (silence = zero samples)
                if (now < hap[i].next) continue;
                hap[i].next = (hap[i].next && now - hap[i].next < 50000 ? hap[i].next : now) + 10667;
                ds_haptics(p, w, v, gain, &hap[i]); out_n++;
            } else if (k == PadDualSense) {
                u8 m[2] = {(u8)lrintf(lo * 255), (u8)lrintf(hi * 255)};
                bool ch = memcmp(s.b, m, 2) != 0;
                if ((ch && now - s.t >= 10000) || (any && now - s.t >= 500000)) {
                    ds_effects(p, w, m[0], m[1], false); memcpy(s.b, m, 2); s.t = now; out_n++;
                }
            } else if (k == PadPro || k == PadJoyL || k == PadJoyR) {
                u8 e[8];
                pro_encode(e, v[0], gain); pro_encode(e + 4, v[1], gain);
                bool nz = (e[1] & 0xFE) || (e[3] != 0x40 && e[3] != 0x00) || (e[2] & 0x80);
                bool ch = memcmp(s.b, e, 8) != 0;
                if ((ch && now - s.t >= 10000) || ((nz || s.nz) && now - s.t >= 40000)) {   // keep-alive while on
                    u8 r[10] = {0x10, (u8)(p->seq++ & 15)};
                    memcpy(r + 2, e, 8);
                    if (!hid_write(w, r, 10, p->outlen)) {
                        static std::atomic<int> told{0};
                        if (told++ < 3) printf("[hid] HD rumble report failed (%lu)\n", GetLastError());
                    }
                    memcpy(s.b, e, 8); s.t = now; s.nz = nz; out_n++;
                }
            }
        }
    }
}
#else
// Linux: the same graph evaluation and motor curve, out through SDL_RumbleGamepad on the open gamepad (two motors; no HD rumble).
static void rumble_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    float gain = rumble_gain();   // (re-read every tick below: the overlay's controls.rumble row)
    printf("[hid] rumble: gain %.2f, SDL_RumbleGamepad\n", gain);
    u64 sent_t = 0; Uint16 sent_l = 0, sent_h = 0;
    int first = 0;
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -50000;          // 5 ms
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 20);
        else Sleep(5);
        u64 now = vib::now_us();
        vib::Val v[2];
        gain = rumble_gain();
        if (gain > 0) vib::gather(v, now);
        float lo = fminf(1.0f, fmaxf(motor_amp(v[0].alo, v[0].flo, false), motor_amp(v[1].alo, v[1].flo, false)) * gain);
        float hi = fminf(1.0f, fmaxf(motor_amp(v[0].ahi, v[0].fhi, true), motor_amp(v[1].ahi, v[1].fhi, true)) * gain);
        bool any = lo > 0.004f || hi > 0.004f;
        if (any && first < 3) {
            first++;
            printf("[hid] rumble: L %.2f@%.0f/%.2f@%.0f R %.2f@%.0f/%.2f@%.0f -> motors %.2f %.2f\n", v[0].alo, v[0].flo, v[0].ahi, v[0].fhi,
                   v[1].alo, v[1].flo, v[1].ahi, v[1].fhi, lo, hi);
        }
        // on change (at most 100/s) and a 0.5-s refresh while on (each call runs the motors for 1 s)
        Uint16 l = (Uint16)lrintf(lo * 65535), h = (Uint16)lrintf(hi * 65535);
        bool ch = l != sent_l || h != sent_h;
        if ((ch && now - sent_t >= 10000) || (any && now - sent_t >= 500000)) {
            std::lock_guard<std::mutex> lk(g_gp_mu);
            if (g_gp && g_gp_rumble) SDL_RumbleGamepad(g_gp, l, h, 1000);
            sent_l = l; sent_h = h; sent_t = now;
        }
    }
}
#endif
static void rumble_start() { std::thread(rumble_thread).detach(); }

struct SixAxisState {       // delta_time, sampling_number, accel, gyro, angle, direction (3x3), attributes
    s64 delta_time, sampling_number; float acc[3], gyro[3], angle[3], dir[9]; u32 attributes, reserved;
};
static_assert(sizeof(SixAxisState) == 0x60);
// Motion from the first pad with an IMU (a Joy-Con pair: device index 1 = the right Joy-Con); none = lying still.
static HidPad* imu_pad(u32 handle) {
    if (!gyro_on()) return nullptr;
    int dev = handle & 0xFF;
    HidPad* any = nullptr;
    for (auto& p : g_hidpad) {
        if (!p.live || !p.has_imu) continue;
        int k = p.kind;
        if ((dev == 1 && k == PadJoyR) || (dev == 0 && k == PadJoyL)) return &p;
        if (!any || (k != PadJoyL && k != PadJoyR)) any = &p;
    }
    if (!any && g_synthpad.live && g_synthpad.has_imu) return &g_synthpad;   // TOTK_CTL "gyro"
    if (!any && g_mousepad.live && g_mousepad.has_imu) return &g_mousepad;   // no motion pad: the mouse while aiming
    return any;
}
// The angular velocity the game gets: the mean since its last read of that handle (the turn since then, from Motion::angle, over the time), not the latest sample. TotK turns its motion aim by
// the angular velocity times its own frame time and ignores delta_time, and it reads one state per frame per handle, so a latest sample stands for the whole frame: the mouse gyro's 1-ms steps
// were missed or, when a read caught one, held for 33 ms, and a real pad's 250 Hz samples gave the game one noisy sample per frame. A steady turn is the same either way (virtual pad: 1.5
// degrees both). TOTK_SIX_RATE=latest or TOTK_CTL "sixaxis all+latest": the latest sample (A/B).
static void six_rate(u32 handle, const HidPad* p, const Motion& m, float out[3], bool latest_ab) {
    static const bool latest_env = getenv("TOTK_SIX_RATE") && !strcmp(getenv("TOTK_SIX_RATE"), "latest");
    const bool latest = latest_env || latest_ab;
    struct Read { u32 handle; const HidPad* pad; float angle[3], rate[3]; std::chrono::steady_clock::time_point t; u64 n; bool used; };
    static Read reads[8];
    static std::mutex mu;
    static auto log_t = std::chrono::steady_clock::now();
    static int logs;
    memcpy(out, m.gyro, sizeof m.gyro);
    if (latest) return;
    std::lock_guard<std::mutex> l(mu);
    const auto now = std::chrono::steady_clock::now();
    Read* r = nullptr;
    for (auto& x : reads) if (x.used && x.handle == handle) r = &x;
    if (!r) for (auto& x : reads) if (!x.used) { r = &x; r->used = true; r->handle = handle; r->pad = nullptr; r->n = 0; break; }
    if (!r) return;
    r->n++;
    const double el = std::chrono::duration<double>(now - r->t).count();
    if (r->pad == p && el < 0.002) { memcpy(out, r->rate, sizeof r->rate); return; }   // (a second read at once: the same)
    if (r->pad == p && el < 0.1)
        for (int a = 0; a < 3; a++) out[a] = (float)((m.angle[a] - r->angle[a]) / el);
    r->pad = p; r->t = now;
    memcpy(r->angle, m.angle, sizeof r->angle); memcpy(r->rate, out, sizeof r->rate);
    const double since = std::chrono::duration<double>(now - log_t).count();
    if (logs < 3 && since >= 10.0) {   // how often the game reads (once a frame per handle is what the mean assumes)
        logs++;
        for (auto& x : reads)
            if (x.used) { printf("[hid] sixaxis reads: handle %x %.1f/s\n", x.handle, x.n / since); x.n = 0; }
        log_t = now;
    }
}
HLE_AS(hid_GetSixAxisSensorStates, "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE") {
    SixAxisState* s = (SixAxisState*)c->x[0]; s32 n = (s32)c->x[1];
    const u32 handle = *(u32*)c->x[2];
    HidPad* p = imu_pad(handle);
    s32 k = n > 0 ? 1 : 0;
    const int mask = g_sixaxis_mask.load(std::memory_order_relaxed);   // TOTK_CTL "sixaxis": which fields the game gets
    for (s32 i = 0; i < k; i++) {
        memset(&s[i], 0, sizeof s[i]);
        s[i].sampling_number = g_sample++; s[i].delta_time = 5000000;   // 5 ms
        s[i].acc[2] = -1.0f; s[i].dir[0] = s[i].dir[4] = s[i].dir[8] = 1.0f;
        if (p) {
            std::lock_guard<std::mutex> l(p->mmu);
            const Motion& m = p->m;
            s[i].delta_time = mask & kSixDt33 ? 33333333 : m.dt_ns;
            if (mask & kSixAcc) memcpy(s[i].acc, m.acc, sizeof m.acc);
            float g[3];
            six_rate(handle, p, m, g, (mask & kSixLatest) != 0);
            // (a real pad's motion only: the mouse as motion aim has its own speed and controls.mouse_aim_scale, so the
            // pad's Motion sensitivity row never stacks on the mouse's)
            const float gk = p == &g_mousepad ? 1.0f : gyro_scale();
            if (mask & kSixGyro) for (int a = 0; a < 3; a++) s[i].gyro[a] = g[a] * gk;
            if (mask & kSixAngle) memcpy(s[i].angle, m.angle, sizeof m.angle);
            if (mask & kSixDir) memcpy(s[i].dir, m.dir, sizeof m.dir);
            if (mask & kSixDirT) for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) s[i].dir[r * 3 + k] = m.dir[k * 3 + r];
        }
        s[i].attributes = 1;                                           // IsConnected
    }
    c->x[0] = (u64)k;
}

HLE_AS(hid_InitializeMouse, "_ZN2nn3hid15InitializeMouseEv") { }
HLE_AS(hid_InitializeKeyboard, "_ZN2nn3hid18InitializeKeyboardEv") { }
HLE_AS(hid_GetMouseStates, "_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi") {   // -> int
    s32 n = (s32)c->x[1]; s32 k = n > 0 ? 1 : 0;
    if (k) { memset((void*)c->x[0], 0, 0x28); *(s64*)c->x[0] = g_sample++; }
    c->x[0] = (u64)k;
}
HLE_AS(hid_GetKeyboardState, "_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE") {
    memset((void*)c->x[0], 0, 0x30); *(s64*)c->x[0] = g_sample++;
}

HLE_AS(hid_ControllerSupportArg_SetDefault, "_ZN2nn3hid20ControllerSupportArg10SetDefaultEv") { }
HLE_AS(hid_ShowControllerSupport, "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE") {
    if (c->x[0]) { s32* r = (s32*)c->x[0]; r[0] = 1; r[1] = 0; }     // {player_count, selected_id}
    c->x[0] = 0;
}

#include "hle_cheat_vm.inc"
