// totk_pcvk.cpp: the PC keys' player virtual keys, between the router and the ExecutePlayerCalcKey hook.
// TOTK_PLAYER_LOG=1 logs the masks the player then sees ("[pcvk] frame...", tests).
#include "totk_pcvk.h"
#include "totk_player_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<uint64_t> g_held{0}, g_latched{0}, g_frames{0};
// the macros' own held bits (a macro that holds a VK for as long as its key is down: MAC_ATTACK, MAC_SPRINT), apart from the
// router's G-row word, which the router rewrites at every read
std::atomic<uint64_t> g_macro_held{0};
std::atomic<uint64_t> g_pad_mask{0};   // VK bits cleared from the pad's masks (totk_pcvk_pad_mask)
std::atomic<bool> g_active{false};
uint64_t g_prev;      // the PC bits at the last run (game thread only)
uint64_t g_last_ms;   // ... and its time
// A gap between two runs longer than this: the node did not run (a cutscene, a load: the AI graph skips it, unlike
// the input-off case, which runs it with the masks at 0). What was pressed meanwhile gives no press when it comes
// back, as a pad button held through it does not. Above the longest frame hitch (a shader stall), so a tap during
// one still counts.
constexpr uint64_t kGapMs = 300;

uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool verbose() {
    static const bool on = getenv("TOTK_PLAYER_LOG") && strcmp(getenv("TOTK_PLAYER_LOG"), "0") != 0;
    return on;
}
}  // namespace

extern "C" void totk_pcvk_hold(uint64_t mask) { g_held.store(mask, std::memory_order_relaxed); }
extern "C" void totk_pcvk_tap(uint64_t mask) { g_latched.fetch_or(mask, std::memory_order_relaxed); }
extern "C" void totk_pcvk_macro_hold(uint64_t mask, bool on) {
    if (on) g_macro_held.fetch_or(mask, std::memory_order_relaxed);
    else g_macro_held.fetch_and(~mask, std::memory_order_relaxed);
}
extern "C" void totk_pcvk_pad_mask(uint64_t mask, bool on) {
    if (on) g_pad_mask.fetch_or(mask, std::memory_order_relaxed);
    else g_pad_mask.fetch_and(~mask, std::memory_order_relaxed);
}
extern "C" uint64_t totk_pcvk_held(void) { return g_held.load(std::memory_order_relaxed); }
extern "C" void totk_pcvk_set_active(bool on) { g_active.store(on, std::memory_order_relaxed); }
extern "C" bool totk_pcvk_active(void) { return g_active.load(std::memory_order_relaxed); }
extern "C" uint64_t totk_pcvk_frames(void) { return g_frames.load(std::memory_order_relaxed); }
// the hook has not run for kGapMs (a strip / menu: the player's calc stopped): its next frame is a "resumed" one, which
// drops the taps made meanwhile and counts held keys as held before (any thread; one aligned load, a stale read is safe)
extern "C" bool totk_pcvk_stale(void) {
    const uint64_t last = *(volatile uint64_t *)&g_last_ms;
    return !last || now_ms() - last > kGapMs;
}

extern "C" void pc_vk_frame(uint64_t *trig, uint64_t *hold, uint64_t *release, uint64_t keep_trig, uint64_t keep_hold,
                            uint64_t keep_release) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    totk_player_state_game_frame();   // (the player's own update: the actor is alive here)
    const uint64_t held = g_held.load(std::memory_order_relaxed) | g_macro_held.load(std::memory_order_relaxed);
    const uint64_t latched = g_latched.exchange(0, std::memory_order_relaxed);
    const uint64_t now = now_ms();
    const bool resumed = g_last_ms && now - g_last_ms > kGapMs;
    g_last_ms = now;
    // after a gap: the taps made in it are dropped and the held keys count as held before (no press)
    const uint64_t eff = resumed ? held : held | latched;
    const uint64_t prev = resumed ? held : g_prev;
    g_prev = eff;
    if (g_active.load(std::memory_order_relaxed)) {
        // (the pad's bits a macro hides from the player: the bow strip's ZR, VK 13; the PC keys still go in below)
        if (const uint64_t pm = g_pad_mask.load(std::memory_order_relaxed)) { *trig &= ~pm; *hold &= ~pm; *release &= ~pm; }
        *trig |= eff & ~prev & keep_trig;
        *hold |= eff & keep_hold;
        *release |= prev & ~eff & keep_release;
    }
    static uint64_t last_t, last_h, last_r;
    static int logs;
    if (verbose() && (*trig != last_t || *hold != last_h || *release != last_r) && logs < 4000) {
        logs++;
        printf("[pcvk] frame %llu trig %#llx hold %#llx release %#llx (pc %#llx keep %#llx)\n",
               (unsigned long long)g_frames.load(), (unsigned long long)*trig, (unsigned long long)*hold,
               (unsigned long long)*release, (unsigned long long)eff, (unsigned long long)keep_hold);
    }
    last_t = *trig; last_h = *hold; last_r = *release;
}
