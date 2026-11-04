// TotK: the VRAM room residency fits while another process holds video memory. Pure state machine, no Vulkan:
// tools/engine/test_vram_fit.py drives it.
//
// A demotion of ours latches "fit the room": the residency budget is then min(the OS budget, the room the other
// processes leave). The room it fits falls at once with the room, in VRAM_FIT_STEP steps, and rises only after the room
// has stayed VRAM_FIT_RISE or more above it for VRAM_FIT_HOLD_NS, or VRAM_FIT_FAST_RISE or more above it for
// VRAM_FIT_FAST_NS (a closed app's memory back: no 10 s wait; another app's usage flickering moves nothing); never
// under VRAM_FIT_FLOOR. The latch lets go once that room reaches the OS budget (the other app gave its memory back), so
// a PC where nothing else holds VRAM (no demotion, or the room at or above the OS budget) keeps the budget it had.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VRAM_FIT_STEP (128ull << 20)
#define VRAM_FIT_RISE (256ull << 20)
#define VRAM_FIT_FLOOR (1536ull << 20)
#define VRAM_FIT_HOLD_NS 10000000000ull
#define VRAM_FIT_FAST_RISE (1024ull << 20)   // (a room back by this much rises after VRAM_FIT_FAST_NS: v0.1.9.1 P1.1)
#define VRAM_FIT_FAST_NS 2000000000ull

typedef struct { uint64_t room, up_ns; } VramFit;   // room: the latched room (0: not latched)

// os_budget: the OS's budget; eff: the residency budget so far. Returns eff, or the latched room when that is lower.
static inline uint64_t vram_fit_step(VramFit *s, uint64_t room, bool demoted, uint64_t os_budget, uint64_t eff, uint64_t now) {
    uint64_t r = room / VRAM_FIT_STEP * VRAM_FIT_STEP;
    if (r < VRAM_FIT_FLOOR) r = VRAM_FIT_FLOOR;
    if (demoted && !s->room) { s->room = r; s->up_ns = 0; }
    if (!s->room) return eff;
    if (r < s->room) { s->room = r; s->up_ns = 0; }
    else if (r >= s->room + VRAM_FIT_RISE) {
        if (!s->up_ns) s->up_ns = now;
        else if (now - s->up_ns >= (r >= s->room + VRAM_FIT_FAST_RISE ? VRAM_FIT_FAST_NS : VRAM_FIT_HOLD_NS)) { s->room = r; s->up_ns = 0; }
    } else s->up_ns = 0;
    if (s->room >= os_budget) { s->room = 0; s->up_ns = 0; return eff; }   // (room for the OS budget again: let go)
    return s->room < eff ? s->room : eff;
}
