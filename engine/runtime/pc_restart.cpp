// pc_restart.cpp: pc_restart_process (pc_restart.h). Until 10-27 skip_restart spawned the exe itself and ended with code
// 0, so under the launcher the new game ran unwatched: the launcher took the 0 for a normal exit, offered an update over
// the new game and missed its crashes.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "pc_restart.h"

extern "C" void pc_restart_process(const char *const *env) {
    fflush(stdout);
    fflush(stderr);
    wchar_t file[MAX_PATH];
    if (GetEnvironmentVariableW(L"NXR_LAUNCHER_RESTART", file, MAX_PATH)) {   // the launcher starts us again
        if (FILE *f = _wfopen(file, L"wb")) {
            for (const char *const *e = env; e && *e; e++) fprintf(f, "%s\n", *e);
            fclose(f);
        }
        printf("[restart] ending with the restart code: the launcher starts the game again\n");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), PC_RESTART_CODE);
    }
    for (const char *const *e = env; e && *e; e++) {
        char k[128];
        const char *eq = strchr(*e, '=');
        if (!eq || eq - *e >= (int)sizeof k) continue;
        memcpy(k, *e, eq - *e);
        k[eq - *e] = 0;
        SetEnvironmentVariableA(k, eq[1] ? eq + 1 : nullptr);
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, GetCommandLineW(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        printf("[restart] CreateProcess failed (%lu): start the game again by hand\n", GetLastError());
        return;
    }
    printf("[restart] the game starts again (pid %lu)\n", pi.dwProcessId);
    fflush(stdout);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    TerminateProcess(GetCurrentProcess(), 0);
}
#endif
