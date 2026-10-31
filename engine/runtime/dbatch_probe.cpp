// Off unless TOTK_DBATCH_PROBE=1. At every pad read it builds one line of state and prints it when it changes:
//   - the pause manager's active reasons (the global at GOT slot main+0x455aa88): the hash -> bit map at +0x40 (keys
//     [0], bit indices [1], capacity +0x14), the active bits at +0x30. 0xf6e0a4c8 is the reason that cuts all player
//     input (ExecutePlayerCalcKey's first block);
//   - the event: EventMgr (GOT slot main+0x4557b30) +0x58 mpMainEvent, +0x20 mActiveEventCount;
//   - the screens not closed: eui::ScreenMgr (main+0x4640010) +0x08 count, +0x10 array; each screen's state byte
//     +0x16e (2 = Opened) and its layout name (Screen +0x28 layout, +0x30 name).
// Lines are " <ms> <frame> ev=<0|1>/<count> reasons=<hash,...> screens=<name:state...>", the ms since the first read, so
// the order of a reason clearing against a screen closing is in the log. Reads only.
#include "rt.h"
#include "totk_version.h"
#include "totk_globals.h"
#include "pc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr u64 kPauseMgrSlot = 0x455aa88, kEventMgrSlot = 0x4557b30, kScreenMgr = 0x4640010;
constexpr u32 kInputCut = 0xf6e0a4c8u;
constexpr int kMaxLines = 6000;

bool probe_on() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("TOTK_DBATCH_PROBE");
        on = e && *e && *e != '0';
    }
    return on != 0;
}

// An object of the game: 8-aligned, its vtable in main's image
bool game_object(u64 p) {
    if (!totk_ptr_ok(p)) return false;
    const u64 vt = ld64(p);
    return vt >= kTotkMain && vt < kTotkMain + 0x6000000;
}

// The reason table, read the way the original's input-off test reads it (main+0x1ccf8a8.. 0x1ccf938; the same reader
// as totk_pc_vk.cpp's input_off, which logs "input off... at bit 43" in the loads): mgr+0x40 -> keys (u32[cap] at
// [0]), bit indices (u8[cap] at [8]), cap (+0x14); the active bits are the words at mgr+0x30. A mapped hash whose bit
// is set is an active reason.
struct ReasonMap { u64 keys, bits; u32 cap; };
bool reason_map(u64 mgr, ReasonMap* m) {
    const u64 map = ld64(mgr + 0x40);
    if (!totk_ptr_ok(map)) return false;
    m->cap = ld32(map + 0x14);
    m->keys = ld64(map);
    m->bits = ld64(map + 8);
    return m->cap && m->cap <= 4096 && m->keys >= 0x10000 && m->bits >= 0x10000;
}
bool bit_on(u64 mgr, u32 b) { return b < 128 && (ld32(mgr + 0x30 + ((b >> 3) & 0x1c)) >> (b & 31) & 1); }

// The name pointer is read as the layout_name does (any address in the user range.
void layout_name(u64 scr, char* out, size_t n) {
    snprintf(out, n, "?");
    const u64 lyt = ld64(scr + 0x28), name = totk_ptr_ok(lyt) ? ld64(lyt + 0x30) : 0;
    if (name < 0x10000 || name >= (1ull << 47)) return;
    size_t i = 0;
    for (; i + 1 < n; i++) {
        const char ch = (char)ld8(name + i);
        if (ch < 0x20 || ch > 0x7e) break;
        out[i] = ch;
    }
    out[i] = 0;
}

// Once: every mapped reason hash with its bit
void dump_reasons(u64 mgr) {
    ReasonMap m;
    if (!reason_map(mgr, &m)) return;
    int mapped = 0;
    for (u32 i = 0; i < m.cap; i++) mapped += ld32(m.keys + 4ull * i) != 0;
    printf("[dbatch-probe] pause manager %#llx: %d reasons mapped (capacity %u)\n", (unsigned long long)mgr, mapped, m.cap);
    for (u32 i = 0; i < m.cap; i++) {
        const u32 k = ld32(m.keys + 4ull * i);
        if (k) printf("[dbatch-probe]   reason %08x bit %u%s\n", k, (unsigned)ld8(m.bits + i), k == kInputCut ? " (INPUT_CUT)" : "");
    }
    fflush(stdout);
}

// One line of state. Early in the boot (and during loads) the globals can hold values that are not pointers yet,
// so the reads run under SEH on Windows, as totk_player_state.cpp's walk does; a fault drops the line (the first
// probe boot, faulted at 2 s on the EventMgr slot before any frame). false: no line.
bool build_line(char* line, size_t cap, bool* dumped) {
    size_t len = 0;
    const u64 em = totk_global(kEventMgrSlot);
    const bool emok = game_object(em);
    const bool ev = emok && ld64(em + 0x58) != 0;
    len += (size_t)snprintf(line + len, cap - len, "ev=%d/%d reasons=", ev ? 1 : 0, emok ? (int)ld32(em + 0x20) : -1);
    const u64 pm = totk_global(kPauseMgrSlot);
    if (totk_ptr_ok(pm)) {
        if (!*dumped) { *dumped = true; dump_reasons(pm); }
        ReasonMap m;
        bool any = false;
        if (reason_map(pm, &m))
            for (u32 i = 0; i < m.cap && len < cap - 200; i++) {
                const u32 k = ld32(m.keys + 4ull * i);
                if (!k || !bit_on(pm, ld8(m.bits + i))) continue;
                len += (size_t)snprintf(line + len, cap - len, "%s%08x%s", any ? "," : "", k, k == kInputCut ? "(INPUT_CUT)" : "");
                any = true;
            }
        if (!any) len += (size_t)snprintf(line + len, cap - len, "-");
    } else {
        len += (size_t)snprintf(line + len, cap - len, "?");
    }
    len += (size_t)snprintf(line + len, cap - len, " screens=");
    const u64 sm = ld64(totk_main(kScreenMgr));
    if (game_object(sm)) {
        const s32 n = (s32)ld32(sm + 0x8);
        const u64 arr = ld64(sm + 0x10);
        bool any = false;
        if (n > 0 && n <= 1024 && totk_ptr_ok(arr))
            for (s32 i = 0; i < n && len < cap - 64; i++) {
                const u64 scr = ld64(arr + 8ull * i);
                if (!game_object(scr)) continue;
                const u8 st = ld8(scr + 0x16e);
                if (!st) continue;
                char nm[40];
                layout_name(scr, nm, sizeof nm);
                len += (size_t)snprintf(line + len, cap - len, "%s%s:%u", any ? " " : "", nm, st);
                any = true;
            }
        if (!any) len += (size_t)snprintf(line + len, cap - len, "-");
    }
    return true;
}

// pc_fault_guard (pc.h): without it rt_main.cpp's vectored crash handler takes the fault before this __except sees
// it.
bool build_line_safe(char* line, size_t cap, bool* dumped) {
#ifdef _WIN32
    bool ok = false;
    pc_fault_guard(1);
    __try {
        ok = game_object(ld64(totk_main(kScreenMgr))) && build_line(line, cap, dumped);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    pc_fault_guard(0);
    return ok;
#else
    return game_object(ld64(totk_main(kScreenMgr))) && build_line(line, cap, dumped);
#endif
}

}  // namespace

extern "C" void dbatch_probe_frame(void) {
    if (!probe_on()) return;
    using clk = std::chrono::steady_clock;
    static const clk::time_point t0 = clk::now();
    static u64 frame;
    static int lines, faults;
    static bool dumped;
    static char last[2048];
    frame++;
    if (lines >= kMaxLines) return;

    char line[2048];
    line[0] = 0;
    if (!build_line_safe(line, sizeof line, &dumped)) {   // nothing before eui's ScreenMgr exists, or a read fault
        if (line[0] && faults++ < 20) { printf("[dbatch-probe] read fault at frame %llu (skipped)\n", (unsigned long long)frame); fflush(stdout); }
        return;
    }
    if (!strcmp(line, last)) return;
    memcpy(last, line, strlen(line) + 1);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
    printf("[dbatch-probe] %lld %llu %s\n", ms, (unsigned long long)frame, line);
    fflush(stdout);
    lines++;
}
