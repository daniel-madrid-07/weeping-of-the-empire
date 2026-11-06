// totk_ui_anim_sound.cpp: a UI animation's sound event fires once per animation frame at any frame rate, as at 30 fps.
//
// eui::Screen::invokeSoundLink2AnimPlayEvent (main+0xb8fd30, slot 81 of the screens' vtables; x0 = the screen, x1 = the
// animator, x2 = "_play", w3 = the animator's frame as an int) emits the screen's SoundLink2 event "anim_<pane><anim>
// _play" through xlink2 (UserInstance::emitImpl, after handing the frame to the screen's SLink user). eui::Animator::
// Play calls it with frame 0, and PlayFromCurrent (main+0x2236788) on EVERY call that finds the animator short of its
// end, with (int)frame. Screens call PlayFromCurrent once per frame until the animation ends: UIHorseGaugeScreen's
// update (main+0x1aa6a94) does it for each spur icon's fill (and empty) animation. The animators step by the UI's
// animation step, [[GOT 0x45577c0]+8] (+ the ScreenMgr's +0x1ac, main+0x83df00): the frame's 30 Hz ratio r. So at 30
// fps a 10-frame animation reports frames 0, 1, .., 9, one event each; at 240 fps each frame number is reported 8
// times in a row: 8 events where the Switch has one, the spur's recharge sound stacked and clipping.
//
// The hook lets an event through when the animator reports another frame number than its last event's, or when
// the same frame number has gone 1.5 frames of 30 Hz without one (an animator that does not advance: the Switch
// repeats it every 30 Hz frame, this every 1.5); else it is held. So each frame number gets one event, at the
// first call that reaches it, as at 30 fps. One difference: a Play (frame 0) and a PlayFromCurrent at frame 0 in the
// same frame send the same event twice at 30 fps; here the second is held (the horse gauge never does it: it only
// calls PlayFromCurrent and StopAtMin). Per (screen, animator, name); an entry not seen for 0.5 s starts over (a
// new animator at a freed one's address). At 30 fps and below (r >= 1) the original exactly. Only the call's own
// arguments are read (no game fields). gen.py --hook b8fd30 in GEN_MAIN; TOTK_UI_ANIM_SOUND=0: the original alone.
// tools/oracle/test_ui_anim_sound.py.
#include "rt.h"
#include "totk_globals.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace {
// 30 Hz frames before an unchanged frame number may fire again. Not 1.0: the frame and this sum are rounded apart, so
// an animator that reached its frame number exactly on a whole frame could see 1.0 here one call before its next
// number, a repeat and then that number, two events a call apart (the stacking this fixes)
constexpr float kRepeat = 1.5f;
constexpr auto kStale = std::chrono::milliseconds(500);

struct Key {
    u64 screen, anim, name;
    bool operator==(const Key &o) const { return screen == o.screen && anim == o.anim && name == o.name; }
};
struct KeyHash {
    size_t operator()(const Key &k) const { return (size_t)(k.anim * 0x9e3779b97f4a7c15ull ^ k.screen ^ (k.name << 1)); }
};
struct Entry {
    s32 frame;                                   // the frame number of the last event let through
    float since;                                 // 30 Hz frames since it (the calls' r, summed)
    std::chrono::steady_clock::time_point seen;  // the last call
};
std::mutex g_mtx;
std::unordered_map<Key, Entry, KeyHash> g_last;
u64 g_held = 0, g_held_logged = 0;                       // repeats held: in all, at the last per-minute line
std::chrono::steady_clock::time_point g_held_t;      // that line's time
}  // namespace

#ifdef _WIN32
void f_b8fd30_orig(Ctx *__restrict c);
void uianimsound_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_b8fd30_orig@@YAXPEIAUCtx@@@Z=?uianimsound_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_b8fd30_orig(Ctx *__restrict c);
#endif
void hook_f_b8fd30(Ctx *__restrict c) {   // eui::Screen::invokeSoundLink2AnimPlayEvent (this, animator, name, frame)
#ifndef _WIN32
    if (!f_b8fd30_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_UI_ANIM_SOUND");
        const bool v = !e || strcmp(e, "0");
        printf("[ui-anim-sound] UI animation sound events %s\n",
               v ? "once per animation frame (as at 30 fps)" : "on every call, as the game (TOTK_UI_ANIM_SOUND=0)");
        return v;
    }();
    const u64 fd = totk_frame_delta();
    const float r = on && totk_ptr_ok(fd) ? totk_frame_ratio(fd) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_b8fd30_orig(c); return; }
    const Key k{c->x[0], c->x[1], c->x[2]};
    const s32 frame = (s32)(u32)c->x[3];
    const auto now = std::chrono::steady_clock::now();
    bool fire;
    u64 held = 0, minute = 0, all = 0;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_last.size() > 512)
            for (auto it = g_last.begin(); it != g_last.end();) it = now - it->second.seen > kStale ? g_last.erase(it) : std::next(it);
        auto it = g_last.find(k);
        if (it == g_last.end() || now - it->second.seen > kStale) {
            g_last[k] = Entry{frame, 0.0f, now};
            fire = true;
        } else {
            Entry &e = it->second;
            e.seen = now;
            e.since += r;
            fire = frame != e.frame || e.since >= kRepeat;
            if (fire) e.frame = frame, e.since = 0.0f;
            else held = ++g_held;
        }
        if (held && now - g_held_t >= std::chrono::seconds(60)) {   // (a line a minute while repeats are held)
            if (g_held_t.time_since_epoch().count()) minute = g_held - g_held_logged, all = g_held;
            g_held_t = now, g_held_logged = g_held;
        }
    }
    if (minute)
        printf("[ui-anim-sound] %llu repeats held since the last line, a minute or more ago (%llu in all)\n",
               (unsigned long long)minute, (unsigned long long)all);
    if (held && held <= 3)
        printf("[ui-anim-sound] held a repeat of an animation's sound event: frame %d again at r %.3f (%llu so far)\n", frame, r,
               (unsigned long long)held);
    if (fire) f_b8fd30_orig(c);
}
