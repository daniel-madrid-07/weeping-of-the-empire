// Crash reports and minidumps. rt_main.cpp's on_crash keeps printing its report to stderr and hands the same text here
// before it ends the process; an unhandled-exception filter, abort() and the CRT's invalid-parameter / pure-call
// handlers cover what on_crash does not see (crashes before it is installed, C++ exceptions nobody caught,
// breakpoints). Each crash writes, into TOTK_CRASH_DIR (default %LOCALAPPDATA%\nxrecomp\TotK-engine\crashes; the newest
// 10 kept): crash-YYYYMMDD-HHMMSS-<pid>.txt reason, exception, host registers and frames, then the runtime's report
// crash-YYYYMMDD-HHMMSS-<pid>.dmp minidump (WinDbg / Visual Studio) with that text as its comment stream The files are
// written from a new thread with its own stack, so a stack overflow can be reported too. On unless TOTK_PC_CRASH=0;
// TOTK_PC_CRASH_NODUMP=1 keeps the.txt only.
#include "pc.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
wchar_t g_dir[MAX_PATH];
volatile LONG g_entered;
volatile int g_on = -1;   // (game.crash_reports sets it live: pc_crash_set_enabled)
volatile LONG g_installed;
constexpr int kKeep = 10;

typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct Job {
    EXCEPTION_POINTERS* ep;
    DWORD thread_id;
    const char* reason;
    const char* runtime_text;
};

// The report is built in memory first: it goes to the .txt, to stderr and into the minidump.
char g_text[64 << 10];
size_t g_len;

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_text + g_len, sizeof g_text - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len = std::min(g_len + (size_t)n, sizeof g_text - 1);
}

// "totk.exe+0x1234" for a host address, or the bare address
void host_symbol(uint64_t addr, char* out, size_t out_size) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)addr, &mod) && mod) {
        wchar_t path[MAX_PATH];
        char name[MAX_PATH];
        if (GetModuleFileNameW(mod, path, MAX_PATH)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, name, sizeof name, nullptr, nullptr);
            snprintf(out, out_size, "%s+0x%llx", name, (unsigned long long)(addr - (uint64_t)(uintptr_t)mod));
            return;
        }
    }
    snprintf(out, out_size, "0x%016llx", (unsigned long long)addr);
}

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case 0xC0000409: return "fail fast / stack buffer overrun";
    case 0xE06D7363: return "C++ exception";
    case 0xE0000ABB: return "abort()";
    default: return "exception";
    }
}

void host_frames(const CONTEXT* ctx_in) {
    CONTEXT ctx = *ctx_in;
    say("\nHost frames:\n");
    for (int i = 0; i < 48 && ctx.Rip; i++) {
        char sym[MAX_PATH + 32];
        host_symbol(ctx.Rip, sym, sizeof sym);
        say("  #%02d %s\n", i, sym);
        DWORD64 image_base;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!fn) {   // leaf function: the return address is at rsp
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void*)(uintptr_t)ctx.Rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
                (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                break;
            ctx.Rip = *(DWORD64*)(uintptr_t)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    }
}

void write_all(const wchar_t* path, const char* p, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, p, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

DWORD WINAPI crash_thread(void* arg) {
    Job* job = (Job*)arg;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stem[MAX_PATH + 64], path[MAX_PATH + 80];
    _snwprintf(stem, MAX_PATH + 64, L"%s\\crash-%04u%02u%02u-%02u%02u%02u-%lu", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, GetCurrentProcessId());
    char stem8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, stem, -1, stem8, sizeof stem8, nullptr, nullptr);

    say("totk.exe crash: %s (host thread %lu)\n", job->reason, (unsigned long)job->thread_id);
    if (job->ep) {
        EXCEPTION_RECORD* er = job->ep->ExceptionRecord;
        char sym[MAX_PATH + 32];
        host_symbol((uint64_t)(uintptr_t)er->ExceptionAddress, sym, sizeof sym);
        say("Exception 0x%08lx (%s) at %s\n", (unsigned long)er->ExceptionCode, exception_name(er->ExceptionCode), sym);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            say("  %s of address 0x%016llx\n",
                er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 8 ? "execute" : "write",
                (unsigned long long)er->ExceptionInformation[1]);
        const CONTEXT* c = job->ep->ContextRecord;
        say("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\nrsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\nr12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
            "rip=%016llx eflags=%08lx mxcsr=%08lx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13,
            c->R14, c->R15, c->Rip, (unsigned long)c->EFlags, (unsigned long)c->MxCsr);
        host_frames(c);
    }
    if (job->runtime_text && *job->runtime_text) say("\nRuntime report:\n%s", job->runtime_text);

    const char* nodump = getenv("TOTK_PC_CRASH_NODUMP");
    bool dumped = false, dump_failed = false;
    if (!(nodump && *nodump == '1')) {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        auto write_dump = dbghelp ? (MiniDumpWriteDumpFn)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
        _snwprintf(path, MAX_PATH + 80, L"%s.dmp", stem);
        HANDLE f = write_dump ? CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                              : INVALID_HANDLE_VALUE;
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {job->thread_id, job->ep, FALSE};
            MINIDUMP_USER_STREAM us = {CommentStreamA, (ULONG)(g_len + 1), g_text};
            MINIDUMP_USER_STREAM_INFORMATION usi = {1, &us};
            // Indirectly referenced memory captures what the registers point at without the multi-GiB guest regions. No data
            // segments: those of the 275 MB exe made a dump 97 MB (db-pc1); the text report carries the guest registers.
            dumped = write_dump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                                MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                                job->ep ? &mei : nullptr, &usi, nullptr);
            CloseHandle(f);
            if (!dumped) { dump_failed = true; DeleteFileW(path); }
        }
    }
    if (dumped) say("\nMinidump: %s.dmp\n", stem8);
    else if (dump_failed) say("\nMinidump: MiniDumpWriteDump failed (%lu)\n", GetLastError());
    say("Report: %s.txt\n", stem8);
    _snwprintf(path, MAX_PATH + 80, L"%s.txt", stem);
    write_all(path, g_text, g_len);
    fputs("[crash] ---- report written ----\n", stderr);
    fwrite(g_text, 1, g_len, stderr);
    fflush(stderr);
    return 0;
}

// Keeps the newest kKeep - 1 crashes at start-up (the names sort by time), so a crash in this run makes at most kKeep.
// A crash is a stem with a .txt, a .dmp or both: a dump whose report was never written (killed mid-crash) counts too.
void prune() {
    std::vector<std::wstring> stems;
    WIN32_FIND_DATAW fd;
    for (const wchar_t* ext : {L".txt", L".dmp"}) {
        std::wstring pat = std::wstring(g_dir) + L"\\crash-*" + ext;
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            stems.push_back(n.substr(0, n.size() - 4));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(stems.rbegin(), stems.rend());
    stems.erase(std::unique(stems.begin(), stems.end()), stems.end());
    for (size_t i = kKeep - 1; i < stems.size(); i++) {
        std::wstring base = std::wstring(g_dir) + L"\\" + stems[i];
        DeleteFileW((base + L".txt").c_str());
        DeleteFileW((base + L".dmp").c_str());
    }
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    if (g_on != 1) return EXCEPTION_CONTINUE_SEARCH;   // switched off after the install: Windows' own handling
    pc_crash_report(ep, "unhandled exception", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

void on_abort(int) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er = {};
    er.ExceptionCode = 0xE0000ABBu;   // synthetic: abort()
    er.ExceptionAddress = (void*)(uintptr_t)ctx.Rip;
    EXCEPTION_POINTERS ep = {&er, &ctx};
    if (g_on == 1) pc_crash_report(&ep, "abort()", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { on_abort(SIGABRT); }
void on_purecall() { on_abort(SIGABRT); }
}   // namespace

extern "C" bool pc_crash_enabled(void) {
    if (g_on < 0) { const char* e = getenv("TOTK_PC_CRASH"); g_on = !(e && *e == '0'); }   // on unless "0" (db-pc1)
    return g_on;
}

extern "C" void pc_crash_install(void) {
    if (!pc_crash_enabled() || InterlockedExchange(&g_installed, 1)) return;
    const char* d = getenv("TOTK_CRASH_DIR");
    if (d && *d) MultiByteToWideChar(CP_UTF8, 0, d, -1, g_dir, MAX_PATH);
    else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH);
        if (!n || n >= MAX_PATH - 40) wcscpy(g_dir, L".");
        wcscat(g_dir, L"\\nxrecomp\\TotK-engine\\crashes");
    }
    for (wchar_t* p = g_dir; *p; p++) if (*p == L'/') *p = L'\\';
    // Create the folder now: at crash time the filesystem calls should be as few as possible.
    for (wchar_t* p = g_dir; *p; p++) {
        if (*p == L'\\' && p > g_dir && p[-1] != L':' && p[-1] != L'\\') {
            *p = 0;
            CreateDirectoryW(g_dir, nullptr);
            *p = L'\\';
        }
    }
    CreateDirectoryW(g_dir, nullptr);
    prune();

    // Stack for the exception path on a stack overflow (the report itself runs on a new thread). Guest threads get the
    // same in rt_bind_thread.
    ULONG guarantee = 64 << 10;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(unhandled_filter);
    signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    fprintf(stderr, "[crash] reports and minidumps go to %ls\n", g_dir);
}

// game.crash_reports, live: off = no report (rt_main.cpp's on_crash and the handlers here read g_on at crash time);
// on = the install above when the start had it off (the folder, the filter and the abort handlers; rt_main's vectored
// on_crash is a separate chain). Threads bound while it was off have no stack guarantee: a stack overflow on one of them
// is reported only if the report thread still gets to run.
extern "C" void pc_crash_set_enabled(bool on) {
    g_on = on ? 1 : 0;
    if (on) pc_crash_install();
}

extern "C" void pc_input_keys_restore(void);   // pc_input.cpp: the Sticky / Toggle / Filter Keys hotkeys as they were
extern "C" void pc_crash_report(void* exception_pointers, const char* reason, const char* runtime_text) {
    pc_input_keys_restore();   // (before anything that can hang: the process ends after the report)
    if (InterlockedExchange(&g_entered, 1)) Sleep(INFINITE);   // a second thread crashed: let the first report finish
    Job job = {(EXCEPTION_POINTERS*)exception_pointers, GetCurrentThreadId(), reason, runtime_text};
    HANDLE th = CreateThread(nullptr, 1 << 20, crash_thread, &job, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, 60000);
        CloseHandle(th);
    }
}

#else   // not Windows: plat_posix.cpp's signal handler prints the report
extern "C" bool pc_crash_enabled(void) { return false; }
extern "C" void pc_crash_install(void) {}
extern "C" void pc_crash_set_enabled(bool) {}
extern "C" void pc_crash_report(void*, const char*, const char*) {}
#endif
