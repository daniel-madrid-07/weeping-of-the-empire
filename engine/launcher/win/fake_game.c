/* A stand-in totk.exe for test_launcher.c: FAKE_GAME=<exit code>,<ms>[,crash] (from the package's player.txt env lines) sleeps <ms>, with "crash" writes a report into TOTK_CRASH_DIR like pc_crash.cpp,
 * and exits with <exit code>; with "rstr" asks the launcher once for a restart (restart.env, code RSTR), then exits 0. It also prints the env the launcher passed, for the test to read in the game log.
 * With FAKE_GAME_MARK=<file> it first writes its working folder there (the update check starts it as a newer launcher).*/
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *spec = getenv("FAKE_GAME");
    int code = 0, ms = 0;
    char what[16] = "";
    if (spec) sscanf(spec, "%d,%d,%15s", &code, &ms, what);
    const char *mark = getenv("FAKE_GAME_MARK");
    if (mark) {
        char cwd[MAX_PATH] = "";
        GetCurrentDirectoryA(MAX_PATH, cwd);
        FILE *m = fopen(mark, "w");
        if (m) { fprintf(m, "%s\n", cwd); fclose(m); }
    }
    printf("fake game: code %d, %d ms%s; TOTK_USERDATA=%s; FAKE_GAME_RESTARTED=%s\n", code, ms,
           strcmp(what, "crash") ? "" : ", crash", getenv("TOTK_USERDATA") ? getenv("TOTK_USERDATA") : "(none)",
           getenv("FAKE_GAME_RESTARTED") ? getenv("FAKE_GAME_RESTARTED") : "-");
    fflush(stdout);
    Sleep(ms);
    if (!strcmp(what, "rstr")) {   /* "rstr": one restart through the launcher (engine/runtime/pc_restart.cpp's way) */
        if (getenv("FAKE_GAME_RESTARTED")) return 0;
        const char *file = getenv("NXR_LAUNCHER_RESTART");
        FILE *f = file ? fopen(file, "w") : NULL;
        if (f) { fputs("FAKE_GAME_RESTARTED=1\n", f); fclose(f); }
        return 0x52535452;
    }
    if (!strcmp(what, "crash")) {
        const char *dir = getenv("TOTK_CRASH_DIR");
        char p[MAX_PATH];
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(p, sizeof p, "%s\\crash-%04u%02u%02u-%02u%02u%02u-%lu.txt", dir ? dir : ".", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
        FILE *f = fopen(p, "w");
        if (f) { fputs("fake crash report\n", f); fclose(f); }
    }
    return code;
}
