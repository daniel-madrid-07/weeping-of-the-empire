// HLE: libc / libm / C++ ABI imports. Guest pointers are host pointers (identity mapping).
#include "hle.h"
#include "nxr_game.h"   // (NXR_GAME_TOTK: forms TotK's build leaves out)
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <mutex>
#ifndef _WIN32
#include <strings.h>
#define _strnicmp strncasecmp   // (UCRT spelling)
#endif

// ---- memory / strings
HLE(memcpy)  { memcpy(P(c->x[0]), P(c->x[1]), c->x[2]); }
HLE(memmove) { memmove(P(c->x[0]), P(c->x[1]), c->x[2]); }
HLE(memset)  { memset(P(c->x[0]), (int)c->x[1], c->x[2]); }
HLE(memcmp)  { c->x[0] = (u64)(s64)memcmp(P(c->x[0]), P(c->x[1]), c->x[2]); }
HLE(memchr)  { c->x[0] = (u64)memchr(P(c->x[0]), (int)c->x[1], c->x[2]); }
HLE(strlen)  { c->x[0] = strlen((const char*)c->x[0]); }
HLE(strnlen) { c->x[0] = strnlen((const char*)c->x[0], c->x[1]); }
HLE(strcmp)  { c->x[0] = (u64)(s64)strcmp((const char*)c->x[0], (const char*)c->x[1]); }
HLE(strncmp) { c->x[0] = (u64)(s64)strncmp((const char*)c->x[0], (const char*)c->x[1], c->x[2]); }
HLE(strncasecmp) { c->x[0] = (u64)(s64)_strnicmp((const char*)c->x[0], (const char*)c->x[1], c->x[2]); }
HLE(strcpy)  { strcpy((char*)c->x[0], (const char*)c->x[1]); }
HLE(strncpy) { strncpy((char*)c->x[0], (const char*)c->x[1], c->x[2]); }
HLE(strchr)  { c->x[0] = (u64)strchr((const char*)c->x[0], (int)c->x[1]); }
HLE(strrchr) { c->x[0] = (u64)strrchr((const char*)c->x[0], (int)c->x[1]); }
HLE(strstr)  { c->x[0] = (u64)strstr((const char*)c->x[0], (const char*)c->x[1]); }
HLE(tolower) { c->x[0] = (u64)tolower((int)c->x[0]); }
HLE(strtoll)  { c->x[0] = (u64)strtoll((const char*)c->x[0], (char**)c->x[1], (int)c->x[2]); }
HLE(strtoul)  { c->x[0] = (u64)strtoull((const char*)c->x[0], (char**)c->x[1], (int)c->x[2]); }   // unsigned long is 64-bit on the guest
HLE(strtoull) { c->x[0] = (u64)strtoull((const char*)c->x[0], (char**)c->x[1], (int)c->x[2]); }
HLE(strtod)   { retd(c, strtod((const char*)c->x[0], (char**)c->x[1])); }

// ---- libm (host UCRT; may differ from the SDK's musl libm in the last ulp)
#define M1F(n) HLE(n) { retf(c, ::n(F(c, 0))); }
#define M2F(n) HLE(n) { retf(c, ::n(F(c, 0), F(c, 1))); }
#define M1D(n) HLE(n) { retd(c, ::n(D(c, 0))); }
#define M2D(n) HLE(n) { retd(c, ::n(D(c, 0), D(c, 1))); }
M1F(sinf) M1F(cosf) M1F(tanf) M1F(asinf) M1F(acosf) M1F(atanf) M1F(sqrtf) M1F(expf) M1F(exp2f) M1F(logf)
M1F(log2f) M1F(log10f) M1F(cbrtf) M2F(powf) M2F(atan2f) M2F(fmodf)
M1D(sqrt) M1D(sin) M1D(acos) M1D(exp2) M1D(logb) M2D(pow) M2D(fmod)
HLE(ldexpf) { retf(c, ldexpf(F(c, 0), (int)c->x[0])); }
HLE(scalbn) { retd(c, scalbn(D(c, 0), (int)c->x[0])); }

// ---- C++ ABI / runtime
static std::recursive_mutex g_guard_mtx;
HLE(__cxa_guard_acquire) {          // returns 1 if the caller must run the initializer
    g_guard_mtx.lock();
    if (*(volatile u8*)c->x[0]) { g_guard_mtx.unlock(); c->x[0] = 0; return; }
    c->x[0] = 1;                     // lock held until release (static init is rare, recursion allowed)
}
HLE(__cxa_guard_release) { *(volatile u8*)c->x[0] = 1; g_guard_mtx.unlock(); }
HLE(__cxa_pure_virtual) { rt_trap(c, c->x[30], "__cxa_pure_virtual"); }
HLE(__cxa_deleted_virtual) { rt_trap(c, c->x[30], "__cxa_deleted_virtual"); }
HLE(abort) { rt_trap(c, c->x[30], "guest abort()"); }
HLE(exit)  { fprintf(stderr, "[rt] guest exit(%d)\n", (int)c->x[0]); fflush(stdout); fflush(stderr); _Exit((int)c->x[0]); }
static thread_local int t_errno;
HLE(__errno_location) { c->x[0] = (u64)&t_errno; }
HLE(__udivti3) {
    u128 a = ((u128)c->x[1] << 64) | c->x[0], b = ((u128)c->x[3] << 64) | c->x[2], q = a / b;
    c->x[0] = (u64)q; c->x[1] = (u64)(q >> 64);
}
static std::mutex g_atomic_mtx;      // libatomic generic: (size, src/ptr, dst/val, order)
HLE(__atomic_load)  { std::lock_guard<std::mutex> l(g_atomic_mtx); memcpy(P(c->x[2]), P(c->x[1]), c->x[0]); }
HLE(__atomic_store) { std::lock_guard<std::mutex> l(g_atomic_mtx); memcpy(P(c->x[1]), P(c->x[2]), c->x[0]); }
HLE(clock) { c->x[0] = (u64)clock(); }
HLE(time)  { time_t t = ::time(nullptr); if (c->x[0]) *(s64*)c->x[0] = t; c->x[0] = (u64)t; }

// qsort with a guest comparator: call back into recompiled code on the same context
static thread_local Ctx* t_qctx; static thread_local u64 t_qcmp;
static int qcmp(const void* a, const void* b) {
    Ctx* c = t_qctx; c->x[0] = (u64)a; c->x[1] = (u64)b; c->x[30] = 0; call_ind(c, t_qcmp); return (int)(s32)c->x[0];
}
HLE(qsort) {
    Ctx* sc = t_qctx; u64 sf = t_qcmp; t_qctx = c; t_qcmp = c->x[3];
    qsort(P(c->x[0]), c->x[1], c->x[2], qcmp);
    t_qctx = sc; t_qcmp = sf;
}

// Per-module libc init called from the module's DT_INIT with its layout (x0 = image base, x2/x3 = .tdata,
// x6/x7 = .tbss, ...). Returning 1 tells DT_INIT the libc will run init_array itself later; the loader does
// that after nninitStartup, matching the SDK order (returning 0 made DT_INIT run all ctors before the heap existed).
HLE(__nnmusl_init_dso) { c->x[0] = 1; }

// libc++ std::__itoa::__u32toa / __u64toa(value, buf): write decimal digits, no terminator, return end pointer
static u64 utoa(u64 v, char* b) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); for (int i = 0; i < n; i++) b[i] = t[n - 1 - i]; return (u64)(b + n); }
HLE_AS(itoa_u32, "_ZNSt3__16__itoa8__u32toaEjPc") { c->x[0] = utoa((u32)c->x[0], (char*)c->x[1]); }
HLE_AS(itoa_u64, "_ZNSt3__16__itoa8__u64toaEmPc") { c->x[0] = utoa(c->x[0], (char*)c->x[1]); }

// setjmp/longjmp (sdk-internal: image codecs / font engine error paths). setjmp records the guest callee-saved state
// (musl aarch64 jmp_buf order: x19-x30, sp, d8-d15) and returns 0. A real longjmp would have to unwind host frames of
// lifted code back to the setjmp call site, which the generator can't do yet: trap with the site instead.
HLE_AS(sj, "setjmp") {
    u64* b = (u64*)c->x[0];
    for (int i = 0; i < 12; i++) b[i] = c->x[19 + i];
    b[12] = c->sp;
    for (int i = 0; i < 8; i++) b[13 + i] = c->v[8 + i].d[0];
    c->x[0] = 0;
}
HLE_AS(sj_, "_setjmp") { hle_sj(c); }
HLE_AS(lj, "longjmp") { rt_trap(c, c->x[30], "longjmp (error path in sdk code; not supported yet)"); }
HLE_AS(lj_, "_longjmp") { hle_lj(c); }

#if !NXR_GAME_TOTK
// HLE: the SDK's abort / error paths and nn::Result's C helpers (SDK 11.4 forms; TotK imports none of these names).
// An abort logs its condition, function, file, line and format string, then traps: a boot that dies there names the
// failed check instead of running on from a stub's 0. Semantics from an SDK 11.4 sdk's code:
// nndetailDiagAbortFormatImpl(cond, func, file, line, fmt,...) forwards to nn::diag::detail::AbortImpl(char const*,
// char const*, char const*, int, char const*,...); nnResult* are bit tests (module = bits 0-8, description = 9-21).

static const char* str_or(u64 p, const char* dflt) { return p ? (const char*)p : dflt; }
static void abort_log(Ctx* c, const char* what, u64 cond, u64 func, u64 file, u64 line, u64 fmt, u32 result) {
    printf("[diag] %s: '%s' failed in %s (%s:%d)%s%s", what, str_or(cond, "?"), str_or(func, "?"), str_or(file, "?"), (int)line,
           fmt ? " - " : "", fmt ? (const char*)fmt : "");
    if (result) printf(" result %#x (module %u, description %u)", result, result & 0x1FF, (result >> 9) & 0x1FFF);
    printf(" lr=%llx\n", (unsigned long long)c->x[30]);
    fflush(stdout);
    rt_trap(c, c->x[30], what);
}
HLE_AS(diag_AbortImpl4, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i") { abort_log(c, "nn::diag abort", c->x[0], c->x[1], c->x[2], c->x[3], 0, 0); }
HLE_AS(diag_AbortImplResult, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z") {
    abort_log(c, "nn::diag abort", c->x[0], c->x[1], c->x[2], c->x[3], c->x[5], c->x[4] ? *(u32*)c->x[4] : 0);
}
HLE_AS(diag_AbortFormatC, "nndetailDiagAbortFormatImpl") { abort_log(c, "nn::diag abort", c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], 0); }
HLE_AS(detail_UnexpectedDefault, "_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i") {
    abort_log(c, "unexpected default", 0, c->x[0], c->x[1], c->x[2], 0, 0);
}
// nn::err::ShowError(ErrorCode): the error applet; logged, the game carries on (ErrorCode = category, number)
HLE_AS(err_ShowErrorCode, "_ZN2nn3err9ShowErrorENS0_9ErrorCodeE") {
    printf("[err] ShowError(ErrorCode %u-%u) lr=%llx\n", (u32)c->x[0], (u32)(c->x[0] >> 32), (unsigned long long)c->x[30]);
    fflush(stdout);
}
HLE_AS(res_IsSuccess, "nnResultIsSuccess") { c->x[0] = (u32)c->x[0] == 0; }
HLE_AS(res_IsFailure, "nnResultIsFailure") { c->x[0] = (u32)c->x[0] != 0; }
HLE_AS(res_GetModule, "nnResultGetModule") { c->x[0] = (u32)c->x[0] & 0x1FF; }
HLE_AS(res_GetDescription, "nnResultGetDescription") { c->x[0] = ((u32)c->x[0] >> 9) & 0x1FFF; }

#endif