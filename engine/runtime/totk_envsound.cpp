// totk_envsound.cpp: ambient one-shots (game::sound::EnvSound) keep the Switch's settle between two sounds of one
// emitter at any frame rate.
//
// EnvSound's calc (vtable 0x42f9550 slot 6, main+0xab8bc8) runs a sub-module (main+0xabafe8) whose two one-shot
// emitters step through main+0xabb518 (x0 = the emitter, s0 = the frame's seconds, *(frame-delta object)+0xc).
// State +0x80: 0 waits a random interval in dt and triggers (main+0xad0184: plays, +0x8c = 0), 1 counts CALLS in the
// int +0x8c (old = +0x8c, +0x8c = old + 1, old >= 199 -> state 2: `cmp w8,#0xc7`, main+0xabb570), 2 follows the
// sound until it ends, 3 waits the next interval in dt. Every timer but state 1's runs on dt: its 200 FRAMES (6.7 s at
// 30 fps, 0.83 s at 240) are a floor between two one-shots of the emitter, so short ones (birds, insects) come up to
// 8x closer at 240 fps.
//
// In state 1 the hook counts those frames in game time with totk_event_waits.cpp's totk_count_in_game_time (a float
// per emitter; the trigger's reset of +0x8c resyncs it); other states call the original alone. At 30 fps and below
// (r >= 1) the original exactly. gen.py --hook abb518 in GEN_MAIN; TOTK_ENVSOUND_SETTLE=0: the original alone.
// tools/oracle/test_envsound.py.
#include "rt.h"
#include "totk_globals.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void totk_count_in_game_time(Ctx *__restrict c, void (*orig)(Ctx *__restrict), u64 off, int on);

#ifdef _WIN32
void f_abb518_orig(Ctx *__restrict c);
void envsound_orig_stub(Ctx *__restrict) {}
#pragma comment(linker, "/alternatename:?f_abb518_orig@@YAXPEIAUCtx@@@Z=?envsound_orig_stub@@YAXPEIAUCtx@@@Z")
#else
__attribute__((weak)) void f_abb518_orig(Ctx *__restrict c);
#endif
void hook_f_abb518(Ctx *__restrict c) {   // EnvSound one-shot emitter step (this, dt in s0, ...)
#ifndef _WIN32
    if (!f_abb518_orig) return;
#endif
    static const bool on = [] {
        const char *e = getenv("TOTK_ENVSOUND_SETTLE");
        const bool v = !e || strcmp(e, "0");
        printf("[envsound] ambient one-shot settle %s\n", v ? "in game time" : "in frames, as the game (TOTK_ENVSOUND_SETTLE=0)");
        return v;
    }();
    const u64 e = c->x[0];
    if (on && totk_ptr_ok(e) && ld32(e + 0x80) == 1) totk_count_in_game_time(c, f_abb518_orig, 0x8c, 1);
    else f_abb518_orig(c);
}
