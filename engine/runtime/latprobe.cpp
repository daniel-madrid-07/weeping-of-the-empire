// latprobe.cpp: mouse -> screen latency probe for the direct mouse camera.
//
// TOTK_LATPROBE=<start s>[,<period ms>[,<count>[,<dx counts>]]]   (default 40,700,120,600; off without the variable)
// From <start s> after process start, every <period ms>: read the PlayerCamera's look direction (data page +136, camera
// +0x88..+0x90, as TOTK_CAMLOG), inject <dx> raw mouse counts with pc_mouse_inject (the TOTK_CTL "mouse" path: the same
// totals the WM_INPUT handler fills, so the hardware -> WM_INPUT leg is not in the figure), alternating the sign so the
// camera swings back, then poll the look direction every ~50 us until it moves and log "[lat] <i> inject <t0 ns>
// present <n0> applied <t1 ns> present <n1> dt <us>" on the present log's clock (SDL_GetTicksNS). t1 is when the game
// thread's camera step took the mouse into the camera; tools/engine/cpuprof/latency.py joins t0 / t1 with
// TOTK_PRESENT_LOG's rows (game present, window present, display) to split the latency into: waiting for the camera
// step, the rest of the game frame, the render / present queue, and the display queue. Needs TOTK_MOUSE_CAMERA=1 and a
// patched key (the mouse veneer). A camera that does not move within 250 ms (a menu, a cutscene, no veneer) logs
// "missed". Windows only (SEH, the Win32 timer and thread priority); the Linux / Deck build compiles every
// engine/runtime/*.cpp, so the whole file is behind _WIN32.
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "pc.h"

extern "C" void* totk_data_page_current(void);       // games/totk/runtime/totk_patches.c: the data page, or NULL
extern "C" uint64_t nvn_present_count(void);          // hle_vi / nvn: presents so far
extern "C" void pc_mouse_inject(double dx, double dy);  // pc_mouse.cpp: raw counts as if moved while captured
extern "C" uint64_t SDL_GetTicksNS(void);             // SDL3 (linked; its header is not on this file's include path)

namespace {

bool look_dir(float out[3]) {
    const uint8_t* p = (const uint8_t*)totk_data_page_current();
    uint64_t cam = p ? __atomic_load_n((const uint64_t*)(p + 136), __ATOMIC_ACQUIRE) : 0;
    if (!cam) return false;
    bool ok = false;
    pc_fault_guard(1);   // (pc.h: else rt_main's vectored crash handler takes the fault before the __except)
    __try {
        for (int i = 0; i < 3; i++) out[i] = *(const volatile float*)(cam + 0x88 + 4 * i);
        ok = true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    pc_fault_guard(0);
    return ok && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void probe(double start_s, int period_ms, int count, double dx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // the poll must not wait behind the game
    uint64_t t_start = SDL_GetTicksNS() + (uint64_t)(start_s * 1e9);
    while (SDL_GetTicksNS() < t_start) Sleep(100);
    printf("[lat] probe on: period %d ms, %d injections of %.0f counts\n", period_ms, count, dx);
    fflush(stdout);
    int missed = 0;
    for (int i = 0; i < count; i++) {
        float a[3], b[3];
        if (!look_dir(a)) { Sleep(period_ms); missed++; continue; }
        double d = (i & 1) ? -dx : dx;
        uint64_t n0 = nvn_present_count(), t0 = SDL_GetTicksNS();
        pc_mouse_inject(d, 0);
        uint64_t t1 = 0, n1 = 0;
        while (SDL_GetTicksNS() - t0 < 250000000ull) {
            if (look_dir(b) && (fabsf(b[0] - a[0]) + fabsf(b[1] - a[1]) + fabsf(b[2] - a[2])) > 1e-4f) {
                t1 = SDL_GetTicksNS(); n1 = nvn_present_count();
                break;
            }
            for (int k = 0; k < 200; k++) YieldProcessor();   // ~50 us between reads, no sleep (1 ms timer quantum)
        }
        if (t1) printf("[lat] %d inject %llu present %llu applied %llu present %llu dt %.0f\n", i,
                       (unsigned long long)t0, (unsigned long long)n0, (unsigned long long)t1, (unsigned long long)n1,
                       (t1 - t0) / 1e3);
        else { printf("[lat] %d inject %llu present %llu missed\n", i, (unsigned long long)t0, (unsigned long long)n0); missed++; }
        fflush(stdout);
        // a fixed period phase-locks to a capped frame rate (700 ms = 42 frames at 60.0 fps: every injection landed at
        // the same point of the frame, and the camera-step wait read 16 ms instead of half a frame); a random extra
        // 0..period/2 spreads the injections over the frame
        Sleep(period_ms + (DWORD)(((uint64_t)__rdtsc() * 2654435761u >> 16) % (DWORD)(period_ms / 2 + 1)));
    }
    printf("[lat] probe done: %d of %d missed\n", missed, count);
    fflush(stdout);
}

struct Start {
    Start() {
        const char* e = getenv("TOTK_LATPROBE");
        if (!e) return;
        double start_s = 40, dx = 600;
        int period = 700, count = 120;
        sscanf(e, "%lf,%d,%d,%lf", &start_s, &period, &count, &dx);
        std::thread(probe, start_s, period, count, dx).detach();
    }
} g_start;

}  // namespace
#endif  // _WIN32
