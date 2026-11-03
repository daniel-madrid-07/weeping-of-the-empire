// TotK Linux port: the subset of <windows.h> the runtime and the NVN layer use, implemented on POSIX (runtime/linux/plat_posix.cpp).
// Only the Linux build puts runtime/linux on the include path (tools/recomp/mkninja_linux.py), so `#include <windows.h>` in
// hle_os_mem.cpp, hle_os_thread.cpp, hle_vi.cpp, hle_time.cpp, nvn/cmdbuf.c, ... resolves to this file there and to the real header on
// Windows. LP64: DWORD/LONG are 64-bit here (printf("%lu") of a DWORD, `long` variables passed to Interlocked*).
// Not emulated (the files that use them have #ifdef _WIN32 branches): XInput / raw HID, WASAPI, GDI, Media Foundation, SEH,
// thread suspend / context, VirtualQuery.
#pragma once
#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "crt_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WINAPI
#define CALLBACK
#define WIN32_LEAN_AND_MEAN
#define INFINITE 0xFFFFFFFFul
#define TRUE 1
#define FALSE 0
typedef unsigned long DWORD, ULONG;
typedef long LONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, DWORD64;
typedef unsigned short WORD, USHORT;
typedef short SHORT;
typedef unsigned char BYTE;
typedef unsigned char BOOLEAN;
typedef int BOOL;
typedef unsigned int UINT;
typedef void *HANDLE, *LPVOID, *PVOID;
typedef const void *LPCVOID;
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR;
typedef DWORD *LPDWORD;
typedef const wchar_t *PCWSTR;            // (wchar_t is 32-bit here; SetThreadDescription callers pass ASCII thread names)
typedef long HRESULT;
typedef uint32_t UINT32;
typedef const wchar_t *LPCWSTR;
typedef wchar_t *LPWSTR;
// window-system handles: opaque here (no GDI / USER32); they only travel through code that is compiled for both, such as
// the vulkan_win32.h structs present.c fills only when VK_EXT_full_screen_exclusive exists (never on Linux)
typedef void *HWND, *HINSTANCE, *HMODULE, *HMONITOR, *HDC;
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

// ---- time
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return TRUE; }   // ns ticks
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (LONGLONG)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return TRUE;
}
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ull + (ULONGLONG)ts.tv_nsec / 1000000ull;
}
static inline void Sleep(DWORD ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000l};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
static inline BOOL SwitchToThread(void) { sched_yield(); return TRUE; }

typedef int64_t __time64_t;
static inline __time64_t _time64(__time64_t *p) { time_t t = time(NULL); if (p) *p = t; return t; }
static inline int _gmtime64_s(struct tm *out, const __time64_t *t) { time_t tt = (time_t)*t; return gmtime_r(&tt, out) ? 0 : EINVAL; }
static inline int _localtime64_s(struct tm *out, const __time64_t *t) { time_t tt = (time_t)*t; return localtime_r(&tt, out) ? 0 : EINVAL; }
static inline __time64_t _mkgmtime64(struct tm *tm) { return (__time64_t)timegm(tm); }

// ---- misc CRT
// (_strnicmp, _stricmp, _aligned_malloc, _putenv_s, YieldProcessor, MAX_PATH, strlcpy: crt_compat.h, included above)

// ---- UTF-8 <-> wide (wchar_t is UTF-32 here). Only CP_UTF8 / CP_ACP (both UTF-8 on Linux); the Win32 call shapes, so the
// runtime's path helpers (UTF-8 inside, wide at the file calls) compile unchanged: the wide calls below take the path
// back to UTF-8
#define CP_ACP 0u
#define CP_UTF8 65001u
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, wchar_t *w, int wn);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *w, int n, char *s, int sn, const char *def, BOOL *used);
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
const wchar_t *_wgetenv(const wchar_t *name);   // (a per-name copy that lives as long as the process)
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_WRITE_THROUGH 0x8u
BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags);   // rename(2): atomic replace; WRITE_THROUGH: fsync
BOOL MoveFileExA(const char *from, const char *to, DWORD flags);

// ---- interlocked / SRW locks (cmdbuf.c)
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG exch, LONG cmp) { __atomic_compare_exchange_n(p, &cmp, exch, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp; }
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_mutex_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_mutex_unlock(l); }
static inline BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_mutex_trylock(l) == 0; }
// condition variables on those locks: waits on CLOCK_MONOTONIC (pthread_cond_clockwait, glibc 2.30), so a wall-clock step
// never stretches one; a timed-out wait returns FALSE with GetLastError() == ERROR_TIMEOUT, as on Windows
#define ERROR_TIMEOUT 1460ul
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(c, NULL); }
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_signal(c); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(c); }
static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags) {
    (void)flags;   // (CONDITION_VARIABLE_LOCKMODE_SHARED: the lock is one mutex here)
    if (ms == INFINITE) return pthread_cond_wait(c, l) == 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000l;
    if (t.tv_nsec >= 1000000000l) { t.tv_sec++; t.tv_nsec -= 1000000000l; }
    int r = pthread_cond_clockwait(c, l, CLOCK_MONOTONIC, &t);
    if (r == ETIMEDOUT) errno = (int)ERROR_TIMEOUT;
    return r == 0;
}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u
// one-time initialisation (InitOnceBeginInitialize without INIT_ONCE_ASYNC): the first caller gets pending = TRUE and
// runs the init holding the lock, the others wait in Begin until InitOnceComplete and get pending = FALSE
typedef struct { pthread_mutex_t m; int done; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
static inline BOOL InitOnceBeginInitialize(INIT_ONCE *o, DWORD flags, BOOL *pending, LPVOID *ctx) {
    (void)flags; (void)ctx;
    if (__atomic_load_n(&o->done, __ATOMIC_ACQUIRE)) { *pending = FALSE; return TRUE; }
    pthread_mutex_lock(&o->m);
    if (o->done) { pthread_mutex_unlock(&o->m); *pending = FALSE; return TRUE; }
    *pending = TRUE;   // (the lock stays held until InitOnceComplete)
    return TRUE;
}
static inline BOOL InitOnceComplete(INIT_ONCE *o, DWORD flags, LPVOID ctx) {
    (void)flags; (void)ctx;
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}

// ---- virtual memory (plat_posix.cpp): reserve = PROT_NONE mapping, commit = mprotect(RW), decommit = MADV_DONTNEED + PROT_NONE.
// MEM_WRITE_WATCH regions track writes with mprotect + a SIGSEGV handler (see plat_posix.cpp for the cost and the protocol).
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_WRITE_WATCH 0x00200000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define WRITE_WATCH_FLAG_RESET 1u
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, DWORD *gran);
UINT ResetWriteWatch(PVOID base, SIZE_T size);
// Kernel writes into a tracked region (read(2) into a guest buffer) fail with EFAULT on a write-protected page instead of raising
// SIGSEGV: call this first on the destination (marks the pages written and unprotects them). No-op outside tracked regions.
void rt_ww_prepare_kernel_write(const void *p, size_t n);
// Is [p, p + n) readable? (process_vm_readv probe: EFAULT instead of a signal.) Used by the crash / watchdog dumpers.
int rt_posix_readable(unsigned long long p, unsigned long long n);

// ---- threads / waitable timers (plat_posix.cpp)
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define TIMER_ALL_ACCESS 0x1F0003u
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102ul
#define WAIT_FAILED 0xFFFFFFFFul
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef void *LPSECURITY_ATTRIBUTES;
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);   // thread handles (done) and waitable timers (fired)
BOOL CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);                   // pseudo handle
DWORD GetCurrentThreadId(void);
BOOL SetThreadPriority(HANDLE h, int prio);      // lowering works unprivileged; raising is tried and ignored on EACCES
HRESULT SetThreadDescription(HANDLE h, PCWSTR name);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, const void *name, DWORD flags, DWORD access);
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *cb, LPVOID arg, void *ctx, ULONG delay);
BOOL CancelWaitableTimer(HANDLE h);
// events on eventfd (plat_posix.cpp): auto-reset ones are consumed by the wait that sees them; WaitForMultipleObjects
// waits for any of events and timers (not threads), bWaitAll unsupported
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, BOOL initial, const wchar_t *name);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, BOOL initial, const char *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
// (present.c / rt_main.cpp's plain form: no resume on Linux, so the Ex call without a context)
static inline BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *cb, LPVOID arg, BOOL resume) {
    (void)resume;
    return SetWaitableTimerEx(h, due, period, cb, arg, NULL, 0);
}
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static inline BOOL TerminateProcess(HANDLE h, unsigned code) { (void)h; _exit((int)code); return TRUE; }
static inline void ExitProcess(unsigned code) { _exit((int)code); }
static inline DWORD GetLastError(void) { return (DWORD)errno; }

// ---- the window-message vocabulary (WinUser.h values). linux/shim_sdl3.c turns SDL's events into these for the code
// that speaks them on both systems (pc_overlay.cpp's F10 window, pc_input.cpp's key capture), with the client area in
// pixels as on Windows; the few user32 calls they make are answered there (GetKeyState from SDL's key state,
// GetClientRect the window's pixel size, PostMessage's WM_CLOSE a quit, SetCursor shows the pointer)
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM, LRESULT;
typedef const char *LPCSTR;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct tagPOINT { LONG x, y; } POINT;
typedef void *HCURSOR;
#define WM_ACTIVATE 0x0006
#define WA_INACTIVE 0
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_DISPLAYCHANGE 0x007E
#define WM_INPUT 0x00FF
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_MOUSEHWHEEL 0x020E
#define WM_DEVICECHANGE 0x0219
#define WM_CAPTURECHANGED 0x0215
#define WM_APP 0x8000
#define HTCLIENT 1
#define WHEEL_DELTA 120
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#define IDC_ARROW ((LPCSTR)(uintptr_t)32512)
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xFFFF))
#define MAKELPARAM(lo, hi) ((LPARAM)(DWORD)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))
#define MAKEWPARAM(lo, hi) ((WPARAM)(DWORD)(((WORD)(lo)) | ((DWORD)((WORD)(hi))) << 16))
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#define GET_XBUTTON_WPARAM(w) (HIWORD(w))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
SHORT GetKeyState(int vk);                                   // (shim_sdl3.c) bit 15: held now
BOOL GetClientRect(HWND h, RECT *r);                         // (shim_sdl3.c) the window's size in pixels
BOOL PostMessageW(HWND h, UINT msg, WPARAM w, LPARAM l);     // (shim_sdl3.c) WM_CLOSE: quit; WM_APP + 2: fullscreen
#define PostMessageA PostMessageW
HCURSOR SetCursor(HCURSOR c);                                // (shim_sdl3.c) shows the pointer (NULL hides it)
#define MONITOR_DEFAULTTONULL 0u
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u
HMONITOR MonitorFromWindow(HWND h, DWORD flags);             // (shim_sdl3.c) the window's SDL display, as a handle
HWND GetForegroundWindow(void);                              // (shim_sdl3.c) the game window while it has the keyboard
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
int GetSystemMetrics(int index);                             // (shim_sdl3.c) the primary display's size in pixels
static inline DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid) { (void)h; if (pid) *pid = (DWORD)getpid(); return 0; }
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
// the keyboard layout, as pc_input.cpp reads it (shim_sdl3.c, from SDL's keymap): set-1 scan codes <-> VK codes, the
// character a key prints, its name; HKL changes when the desktop's layout does (SDL_EVENT_KEYMAP_CHANGED)
typedef void *HKL;
typedef wchar_t WCHAR;
#define MAPVK_VK_TO_VSC 0u
#define MAPVK_VSC_TO_VK 1u
HKL GetKeyboardLayout(DWORD tid);
UINT MapVirtualKeyExW(UINT code, UINT type, HKL layout);
int ToUnicodeEx(UINT vk, UINT scan, const BYTE *state, WCHAR *buf, int n, UINT flags, HKL layout);
int GetKeyNameTextW(LONG lparam, WCHAR *buf, int n);
DWORD CharUpperBuffW(WCHAR *s, DWORD n);
static inline HCURSOR LoadCursorA(HINSTANCE i, LPCSTR name) { (void)i; return (HCURSOR)name; }
#define LoadCursorW(i, n) LoadCursorA(i, (LPCSTR)(n))
#define LoadCursor LoadCursorA
BOOL IsIconic(HWND h);                                       // (shim_sdl3.c) the window minimised
// files: the attribute probe the runtime uses for "does it exist / is it a folder" (plat_posix.cpp, stat(2))
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
DWORD GetFileAttributesA(const char *path);
DWORD GetFileAttributesW(const wchar_t *path);

// ---- virtual-key codes (WinUser.h values): the PC input layer's bindings, settings.ini key names and glyph tables are
// keyed on them on both systems; Linux maps SDL scancodes to these (pc_input / hle_hid)
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F24 0x87
#define VK_PROCESSKEY 0xE5
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_102 0xE2

#ifdef __cplusplus
}
#endif

// ---- SEH: __try / __except, for the runtime's guarded reads of game memory (a stale pointer during a load; pc.h's
// pc_fault_guard). Each __try pushes a sigjmp_buf frame on its thread; plat_posix.cpp's SIGSEGV / SIGBUS handler pops
// the innermost one and jumps back to it, and the __except block runs. The filter is not evaluated: every guarded site
// takes an access violation (SIGSEGV / SIGBUS here); any other signal stays a crash. As with SEH, a local the body
// changed is unspecified in the handler unless volatile. C++ only (every __try site is).
#ifdef __cplusplus
extern "C" {
struct rt_try_frame { sigjmp_buf jb; struct rt_try_frame *prev; };
extern __thread struct rt_try_frame *rt_try_top;
}
struct RtTry {
    rt_try_frame f;
    RtTry() { f.prev = rt_try_top; rt_try_top = &f; }
    ~RtTry() { if (rt_try_top == &f) rt_try_top = f.prev; }   // (a jump already popped it)
    RtTry(const RtTry &) = delete;
    RtTry &operator=(const RtTry &) = delete;
};
#define __try if (RtTry rt_try_; sigsetjmp(rt_try_.f.jb, 0) == 0)
#define __except(filter) else
#endif
#endif   // !_WIN32
