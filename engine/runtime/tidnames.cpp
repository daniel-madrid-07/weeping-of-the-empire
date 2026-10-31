// tidnames.cpp: TOTK_TIDNAMES=<s>[,<s>...] prints "[tid] <host thread id> <name>" for every thread of the process at
// those seconds after start, the name being the thread description (hle_os_thread.cpp's SetThreadName sets the guest
// name there). It maps the host thread ids of TOTK_EVRING_DUMP's evring.txt to names, for the wait-chain analysis of
// loads (tools/engine/cpuprof/waitchain.py). Off without the variable; Windows only.
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

void dump() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD me = GetCurrentProcessId();
    THREADENTRY32 te = {};
    te.dwSize = sizeof te;
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != me) continue;
        char name[128] = "";
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            PWSTR w = nullptr;
            if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name - 1, nullptr, nullptr);
                LocalFree(w);
            }
            CloseHandle(h);
        }
        printf("[tid] %lu %s\n", (unsigned long)te.th32ThreadID, name[0] ? name : "-");
        n++;
    }
    CloseHandle(snap);
    printf("[tid] %d threads listed\n", n);
    fflush(stdout);
}

struct Start {
    Start() {
        const char* e = getenv("TOTK_TIDNAMES");
        if (!e) return;
        std::vector<double> at;
        for (const char* s = e; *s;) {
            at.push_back(atof(s));
            s = strchr(s, ',');
            if (!s) break;
            s++;
        }
        std::thread([at] {
            ULONGLONG t0 = GetTickCount64();
            for (double s : at) {
                ULONGLONG due = t0 + (ULONGLONG)(s * 1000);
                ULONGLONG now = GetTickCount64();
                if (due > now) Sleep((DWORD)(due - now));
                dump();
            }
        }).detach();
    }
} g_start;

}  // namespace
#endif  // _WIN32
