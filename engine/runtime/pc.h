// PC polish: F12 screenshots (pc_shot.cpp) and crash reports with minidumps (pc_crash.cpp). Windows only; on other
// platforms the calls are no-ops. Both are on unless TOTK_PC_SHOT=0 / TOTK_PC_CRASH=0 (exercised by boots db-play2 and
// db-pc1, 2026-10-23).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pc_shot_request(void);   // F12 (window thread): save the next presented frame
bool pc_shot_take(void);      // present thread: true once per request
// Copies the rows and returns; the PNG is encoded and written on a thread of its own (Screenshots folder).
// rgba: the pixels are RGBA (else BGRA). Alpha is written opaque.
void pc_shot_write(const uint8_t *px, uint32_t w, uint32_t h, uint32_t row_bytes, int rgba);

bool pc_crash_enabled(void);
void pc_crash_install(void);  // first thing in main(): crash folder, unhandled-exception filter, abort handlers
void pc_crash_set_enabled(bool on);   // game.crash_reports, live (installs on the first on)
// Writes crash-<time>-<pid>.txt (reason, host registers and frames, then runtime_text) and .dmp (a minidump carrying the
// same text as a comment stream) from a fresh thread; returns when they are written (at most 60 s). A second crashing
// thread blocks here forever. exception_pointers: EXCEPTION_POINTERS*.
void pc_crash_report(void *exception_pointers, const char *reason, const char *runtime_text);
// Reads of game memory that may fault (a stale pointer during a load) run under __try, but rt_main.cpp's crash handler
// is a vectored handler: it runs before any __except and ends the process. Bracket such a read with pc_fault_guard(1)
// and pc_fault_guard(0) (they nest, per thread): an access violation on this thread then reaches the caller's __except.
// Other exceptions (stack overflow, illegal instruction) still end the process. A no-op off Windows.
void pc_fault_guard(int on);

#ifdef __cplusplus
}
#endif
