// HLE: nn::oe (applet / operation environment). We present a docked console in boost performance mode, in focus
// (TOTK_PAUSE_UNFOCUSED: the window's focus, below), in game.language's language (pc_lang.cpp: Windows' by default;
// en-US until 10-27). Setters stay default stubs (logged, return 0).
#include "hle.h"
#include "pc_lang.h"
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

HLE_AS(oe_GetDesiredLanguage, "_ZN2nn2oe18GetDesiredLanguageEv") {   // nn::settings::LanguageCode = char[8] by value
    const char* code = pc_lang_code();
    u64 v = 0; memcpy(&v, code, strlen(code) < 8 ? strlen(code) : 7); c->x[0] = v;
}
// TOTK_HANDHELD=1: report handheld mode (normal performance) instead: the game then renders at its handheld resolution (1280x720
// at most instead of 1600x900) with its handheld settings, the mode for a 1280x800 screen such as the Steam Deck's.
static bool oe_handheld() {
    static const bool h = [] { const char* e = getenv("TOTK_HANDHELD"); bool v = e && *e && strcmp(e, "0") != 0;
                               if (v) fprintf(stderr, "[oe] handheld mode (TOTK_HANDHELD)\n"); return v; }();
    return h;
}
HLE_AS(oe_GetOperationMode, "_ZN2nn2oe16GetOperationModeEv") { c->x[0] = oe_handheld() ? 0 : 1; }      // 0 handheld, 1 console
HLE_AS(oe_GetPerformanceMode, "_ZN2nn2oe18GetPerformanceModeEv") { c->x[0] = oe_handheld() ? 0 : 1; }  // 0 normal, 1 boost
// Pause on focus loss: TOTK_PAUSE_UNFOCUSED=1 (off by default: unattended boots run behind other windows) makes the
// game window's focus the applet's focus. hle_hid.cpp's input refresh reports it (oe_focus_update); a change posts
// AppletMessage 15 (FocusStateChanged) and GetCurrentFocusState answers 2 (out of focus) until the window is in front
// again; TotK pauses on it. Without the variable nothing is ever queued: TryPop finds no message and Pop waits
// forever, as before.
static std::mutex g_msg_lock;
static std::condition_variable g_msg_cv;
static std::deque<u32> g_messages;
static std::atomic<int> g_focused{1};
static std::atomic<int> g_pause{-1};   // -1: read TOTK_PAUSE_UNFOCUSED again (start, oe_settings_reload)
// The window's focus whatever the setting; in focus until the first activation says otherwise
static std::atomic<int> g_window_focused{1};
static int pause_from_env() {
    const char* e = getenv("TOTK_PAUSE_UNFOCUSED");
    const int v = e && *e && strcmp(e, "0") != 0;
    g_pause = v;
    if (v) fprintf(stderr, "[oe] pause on focus loss (TOTK_PAUSE_UNFOCUSED)\n");
    return v;
}
static bool pause_unfocused() {
    int v = g_pause.load();
    if (v < 0) v = pause_from_env();   // (the first ask, at start)
    return v != 0;
}
static void oe_push_message(u32 m);
static void hold_update();
extern "C" bool oe_host_paused(void);
// The settings overlay changed TOTK_PAUSE_UNFOCUSED (hle_hid.cpp's controls rows). Switched off while out of focus, the
// game is told it has focus again.
extern "C" void oe_settings_reload(void) {
    pause_from_env();   // (here, on the settings thread that set the variable: never a getenv on the audio / vsync
    hold_update();  // threads while it _putenv's another row)
    if (!pause_unfocused() && !g_focused.exchange(1)) oe_push_message(15);
}
static void oe_push_message(u32 m) {
    std::lock_guard<std::mutex> l(g_msg_lock);
    if (g_messages.size() < 32) { g_messages.push_back(m); g_msg_cv.notify_all(); }
    else fprintf(stderr, "[oe] notification message %u dropped: queue full\n", m);
}
// From the window's WM_ACTIVATEAPP (shim_win.c), on the window thread: an app switch away or back, also while the game
// is paused and polls nothing (review N20: sampled in the npad refresh, a regained focus could go unseen).
extern "C" void oe_focus_update(bool focused) {
    static std::atomic<bool> seen{false};
    if (seen.load() || focused) {
        seen = true;
        const bool was = oe_host_paused();
        g_window_focused = focused ? 1 : 0;
        if (oe_host_paused() != was)
            fprintf(stderr, "[oe] %s\n", was ? "focus back: the game runs on" : "paused: the window lost focus");
        hold_update();
    }
    static std::atomic<bool> armed{false};   // from the first activation (before it, nothing is "lost")
    if (!pause_unfocused()) return;
    if (!armed.load() && !focused) return;
    armed = true;
    if (g_focused.exchange(focused ? 1 : 0) == (focused ? 1 : 0)) return;
    fprintf(stderr, "[oe] focus %s\n", focused ? "gained" : "lost");
    oe_push_message(15);   // FocusStateChanged
}
extern "C" bool oe_window_focused(void) { return g_window_focused.load() != 0; }
// The host's hold: out of focus with game.pause_unfocused on, the engine stops the game as the Switch's HOME menu
// suspends it. hle_vi.cpp signals no vsync and holds the present (the game's frame loop waits), hle_audio.cpp signals
// no renderer frame and plays silence (its audio threads wait); the system tick runs on, as on the console. What TotK
// itself does with FocusStateChanged was never observed (docs: predictions only), so the hold does not rely on it.
// Scripted boots (TOTK_PRESS / TOTK_CTL) and TOTK_PAUSE_HOLD=0 (boot.py: every harness boot, which runs behind other
// windows) never hold.
extern "C" bool oe_host_paused(void) {
    static const bool never = getenv("TOTK_PRESS") || getenv("TOTK_CTL") ||
                              (getenv("TOTK_PAUSE_HOLD") && !strcmp(getenv("TOTK_PAUSE_HOLD"), "0"));
    return !g_window_focused.load() && !never && pause_unfocused();
}
extern "C" void vk_frames_held(int held);   // device.c: its hang watch (Linux) takes no frames for a pause, not a hang
// Each end of a hold counts (oe_resume_count): hle_vi.cpp's frame-delta hook skips the pause as a time step, which,
// clamped to 1/20 s, would lift its smoothed step ~16 % for ~30 frames
static std::atomic<unsigned> g_resumes{0};
extern "C" unsigned oe_resume_count(void) { return g_resumes.load(); }
static void hold_update() {
    static std::atomic<int> held{0};
    const int now = oe_host_paused() ? 1 : 0;
    if (held.exchange(now) && !now) g_resumes++;
    vk_frames_held(now);
}
HLE_AS(oe_GetCurrentFocusState, "_ZN2nn2oe20GetCurrentFocusStateEv") { c->x[0] = g_focused.load() ? 1 : 2; }  // 1 in focus, 2 out of focus
HLE_AS(oe_GetExpectedVolumeBalance, "_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_") { *(float*)c->x[0] = 1.0f; *(float*)c->x[1] = 1.0f; }
HLE_AS(oe_GetProgramTotalActiveTime, "_ZN2nn2oe25GetProgramTotalActiveTimeEv") { c->x[0] = 0; }
HLE_AS(oe_TryPopNotificationMessage, "_ZN2nn2oe25TryPopNotificationMessageEPj") {   // (u32* out) -> bool
    std::lock_guard<std::mutex> l(g_msg_lock);
    if (g_messages.empty()) { c->x[0] = 0; return; }
    if (c->x[0]) *(u32*)c->x[0] = g_messages.front();
    if (g_messages.front() == 15) fprintf(stderr, "[oe] the game took FocusStateChanged (%s)\n", g_focused.load() ? "in focus" : "out of focus");
    g_messages.pop_front();
    c->x[0] = 1;
}
HLE_AS(oe_PopNotificationMessage, "_ZN2nn2oe22PopNotificationMessageEv") {   // blocks until a message is queued
    std::unique_lock<std::mutex> l(g_msg_lock);
    g_msg_cv.wait(l, [] { return !g_messages.empty(); });
    c->x[0] = g_messages.front();
    if (g_messages.front() == 15) fprintf(stderr, "[oe] the game took FocusStateChanged (%s)\n", g_focused.load() ? "in focus" : "out of focus");
    g_messages.pop_front();
}

// ---- nn::nfp (amiibo): a Switch with no NFC reader found (integration's gap audit, row 5; the frozen runtime's
// hle_oe.c:198-273 is the reference, all 14 symbols main.elf imports). Without them GetState stayed 0 (NonInitialized)
// after Initialize, and Attach left the game's SystemEventType unset: a WaitSystemEvent on it (the amiibo menu) read an
// event never made; and with GetState 1 alone ListDevices / Mount / StartDetection would fall to x0 = 0 ("success") with
// their outputs unwritten. Now Initialize -> Initialized (1), the availability event is a real one never signalled,
// ListDevices finds no device and every device call answers "device not found".
void hle_sysevent_init(u64 ev, bool autoclear);   // hle_os_thread.cpp
static constexpr u32 kNfpDeviceNotFound = 0x8073;   // nfp module 115, description 64
static std::atomic<u32> g_nfp_state{0};
HLE_AS(nfp_Initialize, "_ZN2nn3nfp10InitializeEv") { g_nfp_state = 1; c->x[0] = 0; }
HLE_AS(nfp_Finalize, "_ZN2nn3nfp8FinalizeEv") { g_nfp_state = 0; c->x[0] = 0; }
HLE_AS(nfp_GetState, "_ZN2nn3nfp8GetStateEv") { c->x[0] = g_nfp_state.load(); }   // 0 NonInitialized, 1 Initialized
HLE_AS(nfp_ListDevices, "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii") {   // (DeviceHandle* out, int* count, int max)
    if (c->x[1]) *(s32*)c->x[1] = 0;
    c->x[0] = kNfpDeviceNotFound;
}
HLE_AS(nfp_AttachAvailabilityChangeEvent, "_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE") {
    if (c->x[0]) hle_sysevent_init(c->x[0], false);
    printf("[nfp] availability event %llx: no NFC reader, never signalled\n", (unsigned long long)c->x[0]);
}
HLE_AS(nfp_GetDeviceState, "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE") { c->x[0] = 0; }   // DeviceState_Init
// (no device is ever listed, so a handle reaching these is none of ours)
HLE_AS(nfp_Mount, "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE") { c->x[0] = kNfpDeviceNotFound; }
HLE_AS(nfp_GetNpadId, "_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE") { c->x[0] = kNfpDeviceNotFound; }
HLE_AS(nfp_AttachActivateEvent, "_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE") {
    c->x[0] = kNfpDeviceNotFound;
}
HLE_AS(nfp_AttachDeactivateEvent, "_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE") {
    c->x[0] = kNfpDeviceNotFound;
}
HLE_AS(nfp_StartDetection, "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE") { c->x[0] = kNfpDeviceNotFound; }
HLE_AS(nfp_StopDetection, "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE") { c->x[0] = kNfpDeviceNotFound; }
HLE_AS(nfp_GetTagInfo, "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE") { c->x[0] = kNfpDeviceNotFound; }
HLE_AS(nfp_GetModelInfo, "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE") { c->x[0] = kNfpDeviceNotFound; }
