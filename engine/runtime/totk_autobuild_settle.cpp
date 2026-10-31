// totk_autobuild_settle.cpp: the settle window after an Autobuild keeps the Switch's length at any frame
// rate.
//
// After an Autobuild event SpecialPowerReceiver::preCalcAutoBuilder sets the receiver's u8 +0x1a0 = 5 (main+0x69a168).
// SpecialPowerReceiver::postCalcUltraHand (main+0x9d8d00, x0 = the receiver, once per frame) then, while it is > 0,
// decrements it and runs a contact pass instead of StabilizeBond::update over +0x188 (0x9d9324 / 0x9d957c): a window of
// 5 FRAMES, 167 ms at 30 fps but 35 / 21 ms at 144 / 240, so the bonds start stabilising while the new structure is
// still settling (physics settles in game time).
//
// The hook keeps the window's remaining 30 Hz frames per receiver as a float g: a fresh 5 (a value the hook did not
// leave) starts g = 5; each call takes r and stores 2 (still open after this call) or 1 (this call closes it) for the
// original to decrement, so the contact pass runs for 5 frames of game time. Receivers with +0x1a0 = 0 go straight
// to the original. At 30 fps and below (r >= 1) the original exactly. gen.py --hook 9d8d00 in GEN_MAIN;
// TOTK_AUTOBUILD_SETTLE=0: the original alone. tools/oracle/test_autobuild_settle.py.
#include "rt.h"
#include "totk_globals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 kFrameDelta = 0x45577c0;   // GOT slot of the frame-delta setter's object (+8: 30 Hz ratio)
constexpr u64 kWindow = 0x1a0;

struct Open { u64 receiver; float left; u8 stored; };   // stored: the u8 the original left after the hook's last call
Open g_open[32];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

Open *find(u64 rcv, bool make) {
    Open *empty = nullptr;
    for (Open &o : g_open) {
        if (o.receiver == rcv) return &o;
        if (!o.receiver && !empty) empty = &o;
    }
    if (!make || !empty) return nullptr;
    *empty = {rcv, 0.0f, 0xff};
    return empty;
}
}  // namespace

#ifdef _WIN32
void f_9d8d00_orig(Ctx *__restrict c);
void settle_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_9d8d00_orig@@YAXPEIAUCtx@@@Z=?settle_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_9d8d00_orig(Ctx *__restrict c);
#endif
void hook_f_9d8d00(Ctx *__restrict c) {   // SpecialPowerReceiver::postCalcUltraHand(this, ...)
#ifndef _WIN32
    if (!f_9d8d00_orig) return;
#endif
    const u64 rcv = c->x[0];
    if (!totk_ptr_ok(rcv) || !ld8(rcv + kWindow)) { f_9d8d00_orig(c); return; }
    static const bool on = [] {
        const char *e = getenv("TOTK_AUTOBUILD_SETTLE");
        const bool v = !e || strcmp(e, "0");
        printf("[autobuild-settle] settle window after an Autobuild %s\n", v ? "in game time" : "in frames, as the game (TOTK_AUTOBUILD_SETTLE=0)");
        return v;
    }();
    const u64 fd = totk_global(kFrameDelta);
    const float r = on && totk_ptr_ok(fd) ? ldf32(fd + 8) : 1.0f;
    if (!(r > 0.01f && r < 1.0f)) { f_9d8d00_orig(c); return; }
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    Open *o = find(rcv, true);
    if (o) {
        const u8 v = ld8(rcv + kWindow);
        if (v != o->stored) o->left = (float)v;   // a fresh window (or written outside the hook)
        o->left -= r;
        st8(rcv + kWindow, o->left > 1e-4f ? 2 : 1);
    }
    g_lock.clear(std::memory_order_release);
    f_9d8d00_orig(c);
    if (!o) return;   // (table full: this call ran the original's frame count)
    while (g_lock.test_and_set(std::memory_order_acquire)) {}
    o->stored = ld8(rcv + kWindow);
    if (!o->stored) o->receiver = 0;   // closed: free the slot
    g_lock.clear(std::memory_order_release);
}
