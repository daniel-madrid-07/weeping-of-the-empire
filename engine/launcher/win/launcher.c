/* The "Tears of the Kingdom" launcher for the engine (totk-recomp, engine/). Upstream's launcher/launcher.cpp is a Steam Deck setup app
 * (fork/exec, popen, /proc, SDL3 + ImGui); this one is plain Win32, and settings live in the game's own F10 menu, not here.
 *
 * Packages: next to the launcher, packages\<name>\ (one installed build each) with current.txt (the newest) and
 * lastgood.txt (the one before it); or a single build in the launcher's own folder. A build is totk.exe plus
 * player.txt (key=value lines; relative paths are relative to the build's folder):
 *     img=<NSO images: main.bin, sdk.bin, ...>     romfs=<the game's extracted RomFS>     overlay=<our romfs overlay>
 *     commit=<git commit>                          env.NAME=value  (passed to the game, e.g. env.TOTK_PC_CRASH=1)
 *     settings=<settings.ini defaults, copied to %APPDATA%\nxrecomp\TotK-engine\settings.ini when there is none>
 * A release folder (package.py) is the single-build case: launcher, totk.exe, player.txt, settings.ini, LEEME.txt.
 * Tries current, then lastgood: a build that is missing files, or whose game exits with an error in its first EARLY_MS,
 * is skipped for the next one. Messages are in the game's language (lang_init): Spanish, Brazilian Portuguese, Russian,
 * Simplified Chinese or English.
 *
 * The game gets a clean environment (no TOTK_ / LARECOMP_ / NX* variables from a test shell) plus:
 *     TOTK_USERDATA = %APPDATA%\nxrecomp\TotK-engine\userdata    (save\ inside; the settings file sits next to it)
 *     TOTK_IMG, TOTK_ROMFS, TOTK_OVERLAY from player.txt;  TOTK_PRIORITY=normal (the game lowers itself to idle else)
 *     TOTK_CRASH_DIR = %LOCALAPPDATA%\nxrecomp\TotK-engine\crashes
 *     TOTK_MODS_DIR = <launcher folder>\mods, then %APPDATA%\nxrecomp\TotK-engine\mods (mods_env)
 * and runs in %LOCALAPPDATA%\nxrecomp\TotK-engine\run with stdout/stderr in ...\logs\totk-<time>.log (newest 8 kept).
 * After a failed exit it points at the crash report the game wrote, or at its log.
 *
 * Saves (saves.c): before each start the save is copied to %APPDATA%\nxrecomp\TotK-engine\backups (newest 10 automatic
 * copies kept). Holding Shift while starting it, or --saves, opens a menu to import a save (yuzu, Eden and other yuzu
 * forks, Ryujinx, our old runtime, or any folder such as a JKSV backup), restore a backup or make one. With no save
 * yet and an emulator save found, it offers the import once.
 *
 * A new package folder (another commit= than the last one started) needs nothing carried over: settings.ini, the save
 * and the caches are per user (above, and run\cache), shared by every package; its first start makes a "before-update"
 * copy of the save (note_package_change). A release folder's launcher also looks for a newer release folder beside its
 * own (TotK-PC-<YYYYMMDD-HHMM>-..., not marked NO-USAR) and offers to open that one instead (offer_newer_release).
 *
 * Updates: after the game exits, it starts update.cmd (release.py --auto) with no window; absent update.cmd = none.
 *
 * Build: python engine/launcher/win/build.py; a player install: python engine/launcher/win/release.py
 * (<work>\player-engine: packages\, current.txt, lastgood.txt, this launcher, update.cmd). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <cpuid.h>

#include <time.h>
#include <wctype.h>

#include "saves.h"
#include "autoupdate.h"
#include "preplay.h"
#include "text_tr.h"
#include "../../runtime/mods_install.h"
#include <bcrypt.h>

enum { EARLY_MS = 20000, LOG_MAX = 256 * 1024, KEEP_AUTO_BACKUPS = 10, KEEP_GAME_LOGS = 8, MAX_CANDIDATES = 16 };
static DWORD g_early_ms = EARLY_MS;   /* (test_launcher.c shortens it) */
static const wchar_t TITLE[] = L"Tears of the Kingdom";   /* (its message boxes: the game window's title)*/

static wchar_t g_dir[MAX_PATH];    /* the launcher's folder */
static wchar_t g_exe[MAX_PATH];    /* the launcher's own file name ("TotK Launcher.exe") */
static wchar_t g_base[MAX_PATH];   /* %APPDATA%\nxrecomp\TotK-engine: userdata, backups */
static wchar_t g_local[MAX_PATH];  /* %LOCALAPPDATA%\nxrecomp\TotK-engine: run, logs, crashes, launcher.log */
static FILE *g_log;
static int g_es = 1;   /* the player's texts in Spanish (else English): lang_init (Spanish until then: test_launcher.c) */
static int g_tr;       /* ... or TR_PT / TR_RU / TR_ZH (text_tr.h, looked up by the English: English where it lacks one) */
#define ES_EN(es, en) (g_es ? (es) : g_tr ? tr_of(g_tr, en) : (en))

static void say(const wchar_t *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}
static int g_messages_logged_only;   /* (test_launcher.c) */
static void message(UINT icon, const wchar_t *fmt, ...) {
    wchar_t text[2048];
    va_list a; va_start(a, fmt); vswprintf(text, 2048, fmt, a); va_end(a);
    say(L"message: %ls", text);
    if (g_messages_logged_only) return;
    MessageBoxW(NULL, text, TITLE, MB_OK | MB_SETFOREGROUND | icon);
}
/* message() for an error with a file that tells why (a crash report, a log): the box asks to open it, Yes opens it with
 * its usual program (rival parity, RIVAL-034: a player finds the log without knowing %LOCALAPPDATA%). A missing file:
 * the plain box. */
static wchar_t g_game_log[MAX_PATH];   /* the newest game log run_package made (the error boxes offer it) */
static int exists(const wchar_t *p);
static void open_file(const wchar_t *file);
static void message_open(UINT icon, const wchar_t *file, const wchar_t *fmt, ...) {
    wchar_t text[2048];
    va_list a; va_start(a, fmt); vswprintf(text, 2048, fmt, a); va_end(a);
    int can = file && file[0] && exists(file);
    if (can) {
        size_t n = wcslen(text);
        swprintf(text + n, 2048 - n, ES_EN(L"\n\n¿Abrir %ls ahora?", L"\n\nOpen %ls now?"), wcsrchr(file, L'\\') ? wcsrchr(file, L'\\') + 1 : file);
    }
    say(L"message: %ls", text);
    if (g_messages_logged_only) return;
    if (!can) { MessageBoxW(NULL, text, TITLE, MB_OK | MB_SETFOREGROUND | icon); return; }
    if (MessageBoxW(NULL, text, TITLE, MB_YESNO | MB_SETFOREGROUND | icon) != IDYES) return;
    open_file(file);
}
/* a log or report opened with its usual program */
static void open_file(const wchar_t *file) {
    INT_PTR r = (INT_PTR)ShellExecuteW(NULL, L"open", file, NULL, NULL, SW_SHOWNORMAL);
    if (r <= 32) {   /* no program for.log /.txt: Notepad reads both*/
        wchar_t q[MAX_PATH + 2], np[MAX_PATH];   /* (System32's Notepad by its path: a bare name is searched in the
                                                    game folder first)*/
        swprintf(q, MAX_PATH + 2, L"\"%ls\"", file);
        UINT n = GetSystemDirectoryW(np, MAX_PATH - 16);
        if (n && n < MAX_PATH - 16) {
            wcscat(np, L"\\notepad.exe");
            r = (INT_PTR)ShellExecuteW(NULL, L"open", np, q, NULL, SW_SHOWNORMAL);
        }
    }
    say(L"opened %ls (%ls)", file, r > 32 ? L"ok" : L"failed");
}
/* the players' community: the saves menu's and the failed start's Community button; the same address as Setup's last
 * page and the Discord status button*/
static const wchar_t COMMUNITY_URL[] = L"https://www.reddit.com/r/WeepingOfTheEmpire/";
static void open_community(void) {
    INT_PTR r = (INT_PTR)ShellExecuteW(NULL, L"open", COMMUNITY_URL, NULL, NULL, SW_SHOWNORMAL);   /* the default browser */
    say(L"opened %ls (%ls)", COMMUNITY_URL, r > 32 ? L"ok" : L"failed");
}
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
/* a folder holding anything besides . and .. */
static int dir_has_files(const wchar_t *dir) {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int any = 0;
    do any = wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."); while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}
static void mkdirs(const wchar_t *path) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls", path);
    for (wchar_t *q = p; *q; q++)
        if (*q == L'\\' && q > p && q[-1] != L':' && q[-1] != L'\\') { *q = 0; CreateDirectoryW(p, NULL); *q = L'\\'; }
    CreateDirectoryW(p, NULL);
}

static int init_paths(void) {
    DWORD n = GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) { swprintf(g_exe, MAX_PATH, L"%ls", slash + 1); *slash = 0; }
    wchar_t v[MAX_PATH];
    n = GetEnvironmentVariableW(L"APPDATA", v, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return 0;
    swprintf(g_base, MAX_PATH, L"%ls\\nxrecomp\\TotK-engine", v);
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", v, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return 0;
    swprintf(g_local, MAX_PATH, L"%ls\\nxrecomp\\TotK-engine", v);
    mkdirs(g_base);
    mkdirs(g_local);
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\launcher.log", g_local);
    WIN32_FILE_ATTRIBUTE_DATA a;
    int big = GetFileAttributesExW(p, GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > LOG_MAX);
    if (big) {   /* the full log becomes launcher-previous.log (the one before it goes), the new one starts empty */
        wchar_t prev[MAX_PATH]; swprintf(prev, MAX_PATH, L"%ls\\launcher-previous.log", g_local);
        if (MoveFileExW(p, prev, MOVEFILE_REPLACE_EXISTING)) big = 0;
    }
    g_log = _wfopen(p, big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8");
    return 1;
}

/* The player's language, as the game decides it (pc_lang.cpp): game.language in <g_base>\settings.ini when set (Spanish
 * for "es" and "es-419", Brazilian Portuguese for "pt-BR", Russian for "ru", Simplified Chinese for "zh-Hans", English
 * for any other), else Windows' display language. The launcher speaks those five.
 * Before the first start there is no settings.ini yet (run_package copies the build's defaults later), so Windows
 * decides, as it does for the game whose default is "automatic". */
static const wchar_t *lang_name(void) {
    return g_es ? L"Spanish" : g_tr == TR_PT ? L"Portuguese (Brasil)" : g_tr == TR_RU ? L"Russian"
         : g_tr == TR_ZH ? L"Simplified Chinese" : L"English";
}
static void lang_init(void) {
    wchar_t p[MAX_PATH], line[512], value[64] = L"";
    swprintf(p, MAX_PATH, L"%ls\\settings.ini", g_base);
    FILE *f = _wfopen(p, L"r, ccs=UTF-8");
    int have_file = f != NULL;
    if (f) {
        while (fgetws(line, 512, f)) {
            wchar_t *k = line;
            while (*k == L' ' || *k == L'\t') k++;
            if (wcsncmp(k, L"game.language", 13)) continue;
            wchar_t *v = k + 13;
            while (*v == L' ' || *v == L'\t') v++;
            if (*v != L'=') continue;   /* another key that starts the same */
            v++;
            v[wcscspn(v, L"#\r\n")] = 0;
            while (*v == L' ' || *v == L'\t') v++;
            size_t n = wcslen(v);
            while (n && (v[n - 1] == L' ' || v[n - 1] == L'\t')) v[--n] = 0;
            swprintf(value, 64, L"%.63ls", v);
            break;
        }
        fclose(f);
    }
    if (value[0]) {
        g_es = !wcscmp(value, L"es") || !wcscmp(value, L"es-419");
        g_tr = !wcscmp(value, L"pt-BR") ? TR_PT : !wcscmp(value, L"ru") ? TR_RU
             : !wcscmp(value, L"zh-Hans") ? TR_ZH : TR_EN;
        say(L"language: %ls (game.language = %ls)", lang_name(), value);
    } else {
        LANGID ui = GetUserDefaultUILanguage();
        g_es = PRIMARYLANGID(ui) == LANG_SPANISH;
        int simplified = SUBLANGID(ui) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(ui) == SUBLANG_CHINESE_SINGAPORE;
        g_tr = PRIMARYLANGID(ui) == LANG_PORTUGUESE ? TR_PT : PRIMARYLANGID(ui) == LANG_RUSSIAN ? TR_RU
             : PRIMARYLANGID(ui) == LANG_CHINESE && simplified ? TR_ZH : TR_EN;
        say(L"language: %ls (Windows' display language 0x%04x%ls)", lang_name(), (unsigned)ui,
            have_file ? L"; no game.language in settings.ini" : L"; no settings.ini");
    }
}

/* First line of <launcher dir>\<file> (a package name), trimmed; "" if absent. */
static void read_name(const wchar_t *file, wchar_t *out, size_t n) {
    out[0] = 0;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\%ls", g_dir, file);
    FILE *f = _wfopen(p, L"r, ccs=UTF-8");
    if (!f) return;
    if (fgetws(out, (int)n, f)) {
        size_t k = wcslen(out);
        while (k && (out[k - 1] == L'\n' || out[k - 1] == L'\r' || out[k - 1] == L' ')) out[--k] = 0;
    }
    fclose(f);
}

/* key=... in <pkg>\player.txt; "" if absent. Paths (path != 0) are made absolute against pkg. */
static void package_value(const wchar_t *pkg, const wchar_t *key, int path, wchar_t *out, size_t n) {
    out[0] = 0;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\player.txt", pkg);
    FILE *f = _wfopen(p, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024], v[1024] = L""; size_t kl = wcslen(key);
    while (fgetws(line, 1024, f)) {
        if (wcsncmp(line, key, kl) || line[kl] != L'=') continue;
        swprintf(v, 1024, L"%ls", line + kl + 1);
        size_t k = wcslen(v);
        while (k && (v[k - 1] == L'\n' || v[k - 1] == L'\r' || v[k - 1] == L' ')) v[--k] = 0;
        break;
    }
    fclose(f);
    if (!v[0]) return;
    if (path && !(v[0] && v[1] == L':') && !(v[0] == L'\\' && v[1] == L'\\')) {
        wchar_t rel[MAX_PATH * 2]; swprintf(rel, MAX_PATH * 2, L"%ls\\%ls", pkg, v);
        if (!GetFullPathNameW(rel, (DWORD)n, out, NULL)) swprintf(out, n, L"%ls", rel);
    } else {
        swprintf(out, n, L"%ls", v);
    }
    for (wchar_t *q = out; *q; q++) if (*q == L'/') *q = L'\\';
}

/* env.NAME=value lines of <pkg>\player.txt, set for the game */
static void package_env(const wchar_t *pkg) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\player.txt", pkg);
    FILE *f = _wfopen(p, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        if (wcsncmp(line, L"env.", 4)) continue;
        wchar_t *eq = wcschr(line + 4, L'=');
        if (!eq || eq == line + 4) continue;
        *eq = 0;
        size_t k = wcslen(eq + 1);
        while (k && (eq[k] == L'\n' || eq[k] == L'\r')) eq[k--] = 0;
        SetEnvironmentVariableW(line + 4, eq + 1);
        say(L"env %ls=%ls (player.txt)", line + 4, eq + 1);
    }
    fclose(f);
}

/* A totk.exe from under the launcher's folder is already running: a second game would share the save. */
static int already_running(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    wchar_t prefix[MAX_PATH]; swprintf(prefix, MAX_PATH, L"%ls\\", g_dir);
    PROCESSENTRY32W e = {0};
    e.dwSize = sizeof e;
    int found = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"totk.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!h) continue;
        wchar_t img[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, img, &n) && !_wcsnicmp(img, prefix, wcslen(prefix))) found = 1;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

/* Every variable the game reads comes from here, not from the shell that started the launcher. */
static void clean_environment(void) {
    static const wchar_t *const prefixes[] = {L"TOTK_", L"LARECOMP_", L"NX_", L"NXR_", L"NXRECOMP_"};
    wchar_t *block = GetEnvironmentStringsW();
    static wchar_t names[256][128]; int count = 0;
    for (const wchar_t *v = block; v && *v && count < 256; v += wcslen(v) + 1) {
        const wchar_t *eq = wcschr(v + 1, L'=');   /* "=C:=..." entries start with '=' */
        if (!eq || eq - v >= 128) continue;
        for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
            if (!_wcsnicmp(v, prefixes[i], wcslen(prefixes[i]))) { wcsncpy(names[count], v, (size_t)(eq - v)); names[count][eq - v] = 0; count++; break; }
    }
    if (block) FreeEnvironmentStringsW(block);
    for (int i = 0; i < count; i++) { say(L"cleared %ls", names[i]); SetEnvironmentVariableW(names[i], NULL); }
}

/* After the game.*/
static void maybe_update(const wchar_t *pkg) {
    wchar_t cmd[MAX_PATH], lastgood[MAX_PATH], good[64] = L"", have[64];
    swprintf(cmd, MAX_PATH, L"%ls\\update.cmd", g_dir);
    if (!exists(cmd)) { say(L"no update.cmd: no update check"); return; }
    swprintf(lastgood, MAX_PATH, L"%ls\\..\\canary-engine\\last-good.txt", g_dir);   /* tools/oracle/canary_engine.py */
    FILE *f = _wfopen(lastgood, L"r, ccs=UTF-8");
    if (f) {
        if (fgetws(good, 64, f)) good[wcscspn(good, L" \t\r\n")] = 0;
        fclose(f);
    }
    package_value(pkg, L"commit", 0, have, 64);
    size_t gl = wcslen(good);
    if (!gl) { say(L"update check: no canary last-good.txt"); return; }
    if (have[0] && !_wcsnicmp(have, good, gl < wcslen(have) ? gl : wcslen(have))) { say(L"update check: %ls is the newest good commit", have); return; }
    wchar_t line[MAX_PATH * 2]; swprintf(line, MAX_PATH * 2, L"cmd.exe /d /c \"\"%ls\"\"", cmd);
    STARTUPINFOW si = {0}; si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, g_dir, &si, &pi)) { say(L"update: could not start update.cmd (error %lu)", GetLastError()); return; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    say(L"update: canary last-good %ls, current package %ls: started update.cmd (pid %lu)", good, have[0] ? have : L"?", pi.dwProcessId);
}

/* ---------------------------------------------------------------- files the game leaves */

/* Deletes all but the newest keep files matching dir\pattern (the names sort by time). */
static int newest_first(const void *a, const void *b) { return wcscmp(*(const wchar_t *const *)b, *(const wchar_t *const *)a); }
/* Keeps the `keep` newest files matching pattern (names that sort by time, e.g. totk-YYYYMMDD-HHMMSS.log); any count */
static void prune_files(const wchar_t *dir, const wchar_t *pattern, int keep) {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\%ls", dir, pattern);
    wchar_t **names = NULL;
    int n = 0, cap = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n == cap) {
            wchar_t **more = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof *names);
            if (!more) break;
            names = more;
        }
        if (!(names[n] = _wcsdup(fd.cFileName))) break;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof *names, newest_first);
    for (int i = 0; i < n; i++) {
        if (i >= keep) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\%ls", dir, names[i]); DeleteFileW(p); }
        free(names[i]);
    }
    free(names);
}

/* The newest crash-*.txt in the crash folder written after `since`, or "". */
static void crash_report_since(FILETIME since, wchar_t *out, size_t n) {
    out[0] = 0;
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\crashes\\crash-*.txt", g_local);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME best = since;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            swprintf(out, n, L"%ls\\crashes\\%ls", g_local, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------------------------------------------------------------- saves menu */

static void when(FILETIME ft, wchar_t *out, size_t n) {
    FILETIME lt; SYSTEMTIME t;
    if (!ft.dwHighDateTime || !FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &t)) { swprintf(out, n, L"?"); return; }
    swprintf(out, n, L"%02u/%02u/%04u %02u:%02u", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
}

static int task(const wchar_t *main, const wchar_t *content, const TASKDIALOG_BUTTON *buttons, UINT nb,
                const TASKDIALOG_BUTTON *radios, UINT nr, int *radio, TASKDIALOG_COMMON_BUTTON_FLAGS common, PCWSTR icon) {
    TASKDIALOGCONFIG c = {0};
    c.cbSize = sizeof c;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | (nb ? TDF_USE_COMMAND_LINKS : 0);
    c.pszWindowTitle = TITLE;
    c.pszMainIcon = icon;
    c.pszMainInstruction = main;
    c.pszContent = content;
    c.pButtons = buttons;
    c.cButtons = nb;
    c.pRadioButtons = radios;
    c.cRadioButtons = nr;
    c.dwCommonButtons = common;
    int pressed = IDCANCEL, r = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, radio ? &r : NULL, NULL))) return IDCANCEL;
    if (radio) *radio = r;
    return pressed;
}

static int pick_folder(wchar_t *out, size_t n) {
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return 0;
    FILEOPENDIALOGOPTIONS o;
    if (SUCCEEDED(IFileOpenDialog_GetOptions(d, &o))) IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, ES_EN(L"Carpeta con la partida (por ejemplo, la copia de JKSV en la tarjeta SD)",
                                      L"Folder with the save (for example, the JKSV backup on the SD card)"));
    if (SUCCEEDED(IFileOpenDialog_Show(d, NULL))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) { swprintf(out, n, L"%ls", p); CoTaskMemFree(p); ok = 1; }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
    return ok;
}

/* Why savever refused a save, for the player (r->file names the file). */
static void refusal_text(const SaveConvReport *r, wchar_t *out, size_t n) {
    switch (r->refused) {
    case SV_UNKNOWN_VERSION:
        swprintf(out, n, ES_EN(L"%ls es de una versión del juego que el conversor no conoce (revisión %u). Convierte partidas de 1.1.x, 1.2.x y 1.4.x.",
                               L"%ls comes from a game version the converter doesn't know (revision %u). It converts saves of 1.1.x, 1.2.x and 1.4.x."),
                 r->file, r->detail);
        break;
    case SV_UNKNOWN_KEYS:
        swprintf(out, n, ES_EN(L"%ls tiene datos que la versión 1.0.0 no tiene y que el conversor no conoce (%u; una partida modificada o de una "
                               L"versión posterior a la 1.4.x). Convertida, la partida quedaría rota.",
                               L"%ls has data that version 1.0.0 doesn't have and that the converter doesn't know (%u; a modified save, or one "
                               L"from a version after 1.4.x). Converted, the save would be broken."), r->file, r->detail);
        break;
    case SV_TOO_BIG:
        swprintf(out, n, ES_EN(L"%ls tiene más datos de los que caben en una partida de la 1.0.0.", L"%ls holds more than a 1.0.0 save has room for."), r->file);
        break;
    case SV_PICTURE:
        swprintf(out, n, ES_EN(L"No se pudo convertir la imagen de %ls.", L"The picture in %ls couldn't be converted."), r->file);
        break;
    case SV_IO:
        swprintf(out, n, ES_EN(L"No se pudo leer o escribir %ls.", L"%ls couldn't be read or written."), r->file);
        break;
    default:
        swprintf(out, n, ES_EN(L"%ls está dañado o incompleto.", L"%ls is damaged or incomplete."), r->file);
    }
}

/* What the conversion changed, one line each (empty when nothing was converted). */
static void conversion_text(const SaveConvReport *r, wchar_t *out, size_t n) {
    out[0] = 0;
    if (!r->files) return;
    const wchar_t *from = savever_name(r->from);
    int k = swprintf(out, n, ES_EN(L"\n\nLa partida era de la versión %ls y ahora es de la 1.0.0:", L"\n\nThe save was from version %ls and is now a 1.0.0 save:"),
                     from ? from : L"?");
    if (k > 0 && r->keys_dropped + r->flags_dropped)
        k += swprintf(out + k, n - (size_t)k, ES_EN(L"\n· Se han quitado %d datos de actualizaciones posteriores que la 1.0.0 no tiene.",
                                                    L"\n· %d entries from later updates that 1.0.0 doesn't have were left out."), r->keys_dropped + r->flags_dropped);
    if (k > 0 && r->enums_reset)
        k += swprintf(out + k, n - (size_t)k, ES_EN(L"\n· %d valores que solo existen en versiones posteriores han vuelto a su valor por defecto.",
                                                    L"\n· %d values that exist only in later versions were set back to their default."), r->enums_reset);
    if (k > 0 && r->pictures_fitted + r->pictures_blank)
        k += swprintf(out + k, n - (size_t)k, ES_EN(L"\n· %d imágenes de ranura se han hecho más pequeñas (%d no se podían leer y ahora son negras).",
                                                    L"\n· %d slot pictures were made smaller (%d couldn't be read and are now black)."),
                      r->pictures_fitted + r->pictures_blank, r->pictures_blank);
    if (k > 0 && r->option_dropped)
        swprintf(out + k, n - (size_t)k, ES_EN(L"\n· Los ajustes del juego (option.sav) no se podían convertir: el juego empezará con los de por defecto.",
                                               L"\n· The game settings (option.sav) couldn't be converted: the game starts with its defaults."));
}

/* Backs up the current save (why), then makes src the save. An import from a later game version also keeps the
 * original, unconverted, in the backups (savever converts the copy that goes in, never src). */
static void replace_with(const wchar_t *src, const wchar_t *why, const wchar_t *what) {
    wchar_t bk[MAX_PATH] = L"", orig[MAX_PATH] = L"";
    if (saves_backup(g_base, why, bk, MAX_PATH) < 0) {
        message(MB_ICONWARNING, ES_EN(L"No se pudo hacer la copia de la partida actual; no se ha cambiado nada.",
                                      L"Couldn't back up the current save; nothing was changed."));
        return;
    }
    uint32_t rev = savever_dir_revision(src);
    if (!wcscmp(why, L"before-import") && rev && rev != savever_tables.target &&
        saves_backup_from(g_base, src, L"original", orig, MAX_PATH) < 0) {
        message(MB_ICONWARNING, ES_EN(L"No se pudo guardar una copia de la partida que querías importar; no se ha cambiado nada.",
                                      L"Couldn't keep a copy of the save you wanted to import; nothing was changed."));
        return;
    }
    SaveConvReport rep = {0};
    int r = saves_replace_report(g_base, src, &rep);
    say(L"%ls from %ls (revision %u): %ls (backup %ls; original %ls); converted %d files from %u: keys -%d flags -%d enums %d "
        L"pictures %d+%d option-dropped %d; refused %d %ls %u", what, src, rev, r ? L"failed" : L"done", bk[0] ? bk : L"none: no save",
        orig[0] ? orig : L"-", rep.files, rep.from, rep.keys_dropped, rep.flags_dropped, rep.enums_reset, rep.pictures_fitted,
        rep.pictures_blank, rep.option_dropped, rep.refused, rep.file, rep.detail);
    if (r == 4) {
        wchar_t why_text[512];
        refusal_text(&rep, why_text, 512);
        message(MB_ICONWARNING, L"%ls\n\n%ls\n\n%ls", ES_EN(L"No se ha importado la partida; la que había sigue igual.",
                                                          L"The save wasn't imported; your previous save is unchanged."), why_text, src);
        return;
    }
    if (r) {
        message(MB_ICONWARNING, ES_EN(L"No se pudo copiar la partida (paso %d). La partida que había sigue igual.\n\n%ls",
                                      L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls"), r, src);
        return;
    }
    wchar_t conv[1024];
    conversion_text(&rep, conv, 1024);
    int kept = orig[0] && rep.files;
    message(MB_ICONINFORMATION, L"%ls%ls%ls%ls%ls%ls", ES_EN(L"Hecho.", L"Done."), conv,
            kept ? ES_EN(L"\n\nLa partida original, sin convertir, está guardada en:\n", L"\n\nThe original save, not converted, is kept in:\n") : L"",
            kept ? orig : L"",
            bk[0] ? ES_EN(L"\n\nLa partida anterior está guardada en:\n", L"\n\nYour previous save is backed up in:\n") : L"", bk);
}

static void import_save(const SaveCandidate *found, int count) {
    SaveCandidate local[MAX_CANDIDATES];
    const SaveCandidate *c = found;
    for (;;) {
        static wchar_t labels[MAX_CANDIDATES][MAX_PATH + 160];
        TASKDIALOG_BUTTON radios[MAX_CANDIDATES];
        for (int i = 0; i < count; i++) {
            wchar_t t[32]; when(c[i].newest, t, 32);
            wchar_t user[16] = L"";
            if (c[i].user[0]) swprintf(user, 16, L" · %.8ls", c[i].user);
            const wchar_t *source = !wcscmp(c[i].source, L"carpeta") ? ES_EN(L"carpeta", L"folder") : c[i].source;   /* saves.c's */
            uint32_t rev = savever_dir_revision(c[i].path);
            const wchar_t *ver = savever_name(rev);
            wchar_t version[48] = L"";   /* (none for a file that is not a TotK save) */
            if (rev) swprintf(version, 48, L" · %ls", ver ? ver : ES_EN(L"versión desconocida", L"unknown version"));
            swprintf(labels[i], MAX_PATH + 160, L"%ls%ls · %d %ls · %ls%ls\n%ls", source, user, c[i].slots,
                     c[i].slots == 1 ? ES_EN(L"ranura", L"slot") : ES_EN(L"ranuras", L"slots"), t, version, c[i].path);
            radios[i].nButtonID = 1000 + i;
            radios[i].pszButtonText = labels[i];
        }
        const TASKDIALOG_BUTTON buttons[] = {
            {200, ES_EN(L"Importar la partida marcada\nLa partida actual se copia antes en backups.",
                        L"Import the selected save\nYour current save is backed up first.")},
            {201, ES_EN(L"Buscar en otra carpeta…\nPor ejemplo, una copia de JKSV de la consola o un emulador portable.",
                        L"Look in another folder…\nFor example, a JKSV backup from the console or a portable emulator.")},
        };
        int radio = 0;
        int b = task(count ? ES_EN(L"Elige la partida que quieres importar", L"Choose the save to import")
                           : ES_EN(L"No se ha encontrado ninguna partida de TotK", L"No TotK saves found"),
                     ES_EN(L"El juego es la versión 1.0.0. Una partida de la 1.1.x, 1.2.x o 1.4.x se convierte a la 1.0.0 al importarla; "
                           L"la original se guarda sin tocar en las copias.",
                           L"The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; "
                           L"the original is kept, untouched, in the backups."),
                     count ? buttons : buttons + 1, count ? 2 : 1, count ? radios : NULL, (UINT)count, &radio,
                     TDCBF_CANCEL_BUTTON, NULL);
        if (b == 200 && radio >= 1000 && radio < 1000 + count) {
            replace_with(c[radio - 1000].path, L"before-import", L"import");
            return;
        }
        if (b != 201) return;
        wchar_t dir[MAX_PATH];
        if (!pick_folder(dir, MAX_PATH)) continue;
        count = saves_find_under(dir, 5, local, MAX_CANDIDATES);
        c = local;
        say(L"import: %d save(s) under %ls", count, dir);
    }
}

static void restore_backup(void) {
    static SaveBackup b[MAX_CANDIDATES];
    int n = saves_list_backups(g_base, b, MAX_CANDIDATES);
    if (!n) { message(MB_ICONINFORMATION, ES_EN(L"Todavía no hay copias de la partida.", L"There are no save backups yet.")); return; }
    static wchar_t labels[MAX_CANDIDATES][160];
    TASKDIALOG_BUTTON radios[MAX_CANDIDATES];
    for (int i = 0; i < n; i++) {
        /* save-YYYYMMDD-HHMMSS-NN-<why> */
        const wchar_t *nm = b[i].name, *why = wcslen(nm) > 24 ? nm + 24 : L"";
        const wchar_t *what = !wcscmp(why, L"auto") ? ES_EN(L"automática", L"automatic")
                            : !wcscmp(why, L"manual") ? ES_EN(L"manual", L"manual")
                            : !wcscmp(why, L"before-import") ? ES_EN(L"antes de importar", L"before import")
                            : !wcscmp(why, L"before-restore") ? ES_EN(L"antes de restaurar", L"before restore")
                            : !wcscmp(why, L"before-update") ? ES_EN(L"antes de actualizar", L"before update")
                            : !wcscmp(why, L"original") ? ES_EN(L"original importada, sin convertir", L"imported original, not converted") : why;
        if (wcslen(nm) >= 20)
            swprintf(labels[i], 160, L"%.2ls/%.2ls/%.4ls %.2ls:%.2ls:%.2ls · %ls · %d %ls", nm + 11, nm + 9, nm + 5, nm + 14, nm + 16,
                     nm + 18, what, b[i].slots, b[i].slots == 1 ? ES_EN(L"ranura", L"slot") : ES_EN(L"ranuras", L"slots"));
        else swprintf(labels[i], 160, L"%ls", nm);
        radios[i].nButtonID = 1000 + i;
        radios[i].pszButtonText = labels[i];
    }
    const TASKDIALOG_BUTTON buttons[] = {{300, ES_EN(L"Restaurar la copia marcada\nLa partida actual se copia antes en backups.",
                                                     L"Restore the selected backup\nYour current save is backed up first.")}};
    int radio = 0;
    if (task(ES_EN(L"Elige la copia que quieres restaurar", L"Choose the backup to restore"), NULL, buttons, 1, radios, (UINT)n, &radio, TDCBF_CANCEL_BUTTON, NULL) == 300 &&
        radio >= 1000 && radio < 1000 + n)
        replace_with(b[radio - 1000].path, L"before-restore", L"restore");
}

/* The builds' own import=<save folder>, first; then the emulators'.*/
static int find_candidates(SaveCandidate *c, int max) {
    int n = 0;
    wchar_t cur[MAX_PATH], pkgs[2][MAX_PATH];
    read_name(L"current.txt", cur, MAX_PATH);
    swprintf(pkgs[0], MAX_PATH, L"%ls", g_dir);
    swprintf(pkgs[1], MAX_PATH, L"%ls\\packages\\%ls", g_dir, cur);
    for (int i = 0; i < (cur[0] ? 2 : 1) && n < max; i++) {
        wchar_t p[MAX_PATH];
        package_value(pkgs[i], L"import", 1, p, MAX_PATH);
        if (p[0] && saves_find_under(p, 0, &c[n], 1) == 1) {
            swprintf(c[n].source, 24, L"%ls", ES_EN(L"partida de este PC", L"save on this PC"));
            n++;
        }
    }
    /* our other game versions' saves (userdata-v<tag>\save, e.g. the 1.4.3 build's): into 1.0.0 through savever */
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\userdata-v*", g_base);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%ls\\%ls\\save", g_base, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < max && saves_find_under(p, 0, &c[n], 1) == 1) {
                swprintf(c[n].source, 24, L"nxrecomp %ls", fd.cFileName + 9);   /* "nxrecomp v143" */
                n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) n += saves_find(appdata, c + n, max - n);
    return n;
}

/* 1 = play, 0 = quit */
static int saves_menu(void) {
    for (;;) {
        wchar_t save[MAX_PATH], content[MAX_PATH + 160], t[32];
        saves_dir(g_base, save, MAX_PATH);
        int slots = 0; FILETIME newest;
        if (saves_is_save(save, &slots, &newest)) {
            when(newest, t, 32);
            swprintf(content, MAX_PATH + 160, ES_EN(L"Partida actual: %d %ls, guardada el %ls.\n%ls", L"Current save: %d %ls, saved on %ls.\n%ls"),
                     slots, slots == 1 ? ES_EN(L"ranura", L"slot") : ES_EN(L"ranuras", L"slots"), t, save);
        } else {
            swprintf(content, MAX_PATH + 160, ES_EN(L"Todavía no hay partida.\n%ls", L"No save yet.\n%ls"), save);
        }
        const TASKDIALOG_BUTTON buttons[] = {
            {100, ES_EN(L"Jugar", L"Play")},
            {101, ES_EN(L"Importar una partida…\nDe yuzu, Eden, Ryujinx, la versión anterior de nxrecomp o una carpeta (JKSV). "
                        L"Las de versiones posteriores del juego (1.1.x a 1.4.x) se convierten a la 1.0.0.",
                        L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV). "
                        L"Saves of later game versions (1.1.x to 1.4.x) are converted to 1.0.0.")},
            {102, ES_EN(L"Restaurar una copia…", L"Restore a backup…")},
            {103, ES_EN(L"Hacer una copia ahora", L"Make a backup now")},
            {104, ES_EN(L"Comunidad\nr/WeepingOfTheEmpire en Reddit: novedades, ayuda, capturas",
                        L"Community\nr/WeepingOfTheEmpire on Reddit: news, help, screenshots")},
        };
        int b = task(ES_EN(L"Partidas", L"Saves"), content, buttons, 5, NULL, 0, NULL, TDCBF_CLOSE_BUTTON, NULL);
        if (b == 100) return 1;
        if (b == 104) { open_community(); continue; }   /* (the menu stays) */
        if (b == 101) {
            static SaveCandidate c[MAX_CANDIDATES];
            import_save(c, find_candidates(c, MAX_CANDIDATES));
        } else if (b == 102) {
            restore_backup();
        } else if (b == 103) {
            wchar_t bk[MAX_PATH];
            int r = saves_backup(g_base, L"manual", bk, MAX_PATH);
            if (r > 0) message(MB_ICONINFORMATION, ES_EN(L"Copia hecha:\n%ls", L"Backup made:\n%ls"), bk);
            else message(MB_ICONINFORMATION, r ? ES_EN(L"No se pudo hacer la copia.", L"Couldn't make the backup.")
                                               : ES_EN(L"No hay partida que copiar.", L"There's no save to back up."));
        } else {
            return 0;
        }
    }
}

/* No save yet and there is one to import (the build's import=, an emulator's): offer it, once. Never over a save. */
static void offer_import(void) {
    wchar_t save[MAX_PATH], marker[MAX_PATH];
    saves_dir(g_base, save, MAX_PATH);
    swprintf(marker, MAX_PATH, L"%ls\\import-offered.txt", g_base);
    if (saves_is_save(save, NULL, NULL) || exists(marker)) return;
    static SaveCandidate c[MAX_CANDIDATES];
    int n = find_candidates(c, MAX_CANDIDATES);
    if (!n) return;
    FILE *f = _wfopen(marker, L"w");
    if (f) { fputs("asked\n", f); fclose(f); }
    if (task(ES_EN(L"Hay partidas de TotK que puedes importar", L"There are TotK saves you can import"),
             ES_EN(L"¿Quieres importar una antes de empezar? Se puede hacer más tarde "
                   L"manteniendo pulsado Mayús al abrir el lanzador.",
                   L"Do you want to import one before you start? You can also do it later "
                   L"by holding Shift while opening the launcher."), NULL, 0, NULL, 0, NULL, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON,
             TD_INFORMATION_ICON) == IDYES)
        import_save(c, n);
}

/* ---------------------------------------------------------------- updates */

/* A new package (another commit= than the last one started) gets a named copy of the save first. Nothing else moves:
 * settings.ini, the save and the caches (run\cache: pipelines, SPIR-V, guest code) are per user in %APPDATA% /
 * %LOCALAPPDATA%, shared by every package folder, so a newer folder unzipped next to an older one starts with them.
 * Returns 1 when it made the copy. g_base\last-package.txt: "<commit>\t<package folder>". */
static int note_package_change(const wchar_t *pkg) {
    wchar_t commit[64], last[MAX_PATH + 80] = L"", path[MAX_PATH];
    package_value(pkg, L"commit", 0, commit, 64);
    if (!commit[0]) return 0;
    swprintf(path, MAX_PATH, L"%ls\\last-package.txt", g_base);
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (f) { if (!fgetws(last, MAX_PATH + 80, f)) last[0] = 0; fclose(f); }
    wchar_t *tab = wcschr(last, L'\t');
    if (tab) *tab = 0;
    int made = 0;
    if (last[0] && wcscmp(last, commit)) {
        wchar_t bk[MAX_PATH] = L"";
        int r = saves_backup(g_base, L"before-update", bk, MAX_PATH);
        say(L"update: commit %ls -> %ls, save copy %ls", last, commit, r > 0 ? bk : r == 0 ? L"(no save)" : L"FAILED");
        made = r > 0;
    }
    if (!last[0] || wcscmp(last, commit)) {
        f = _wfopen(path, L"w, ccs=UTF-8");
        if (f) { fwprintf(f, L"%ls\t%ls\n", commit, pkg); fclose(f); }
    }
    return made;
}

/* "TotK-PC-YYYYMMDD-HHMM..." (package.py's folder name): its 13-character stamp, which sorts by time; else 0. */
static int release_stamp(const wchar_t *name, wchar_t stamp[14]) {
    if (_wcsnicmp(name, L"TotK-PC-", 8)) return 0;
    const wchar_t *s = name + 8;
    for (int i = 0; i < 13; i++)
        if (i == 8 ? s[i] != L'-' : !iswdigit(s[i])) return 0;
    wcsncpy(stamp, s, 13);
    stamp[13] = 0;
    return 1;
}

/* Marked not to be played: "...-NO-USAR", in any case, with '-', ' ' or '_' */
static int marked_unusable(const wchar_t *name) {
    wchar_t up[MAX_PATH]; swprintf(up, MAX_PATH, L"%ls", name);
    _wcsupr(up);
    for (wchar_t *q = up; *q; q++) if (*q == L' ' || *q == L'_') *q = L'-';
    return wcsstr(up, L"NO-USAR") != NULL;
}

/* dir holds a release (totk.exe, player.txt and a launcher: this one's file name, else "TotK Launcher.exe"); 1 with the
 * launcher's path */
static int release_launcher(const wchar_t *dir, wchar_t *launcher, size_t n) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\totk.exe", dir);
    if (!exists(p)) return 0;
    swprintf(p, MAX_PATH, L"%ls\\player.txt", dir);
    if (!exists(p)) return 0;
    if (g_exe[0]) { swprintf(launcher, n, L"%ls\\%ls", dir, g_exe); if (exists(launcher)) return 1; }
    swprintf(launcher, n, L"%ls\\TotK Launcher.exe", dir);
    return exists(launcher);
}

/* The newest release folder beside the launcher's own with a later stamp, not marked NO-USAR: 1 with its folder and
 * launcher. Explorer's "Extract all" nests a zip's folder in one of the same name, so <name>\<name> counts too. A
 * launcher whose own folder has no stamp (a player-engine install, a renamed folder) finds none. */
static int newer_release(wchar_t *dir, wchar_t *launcher, size_t n) {
    wchar_t parent[MAX_PATH], own[14], best[14] = L"", stamp[14], pat[MAX_PATH], cand[MAX_PATH], exe[MAX_PATH];
    swprintf(parent, MAX_PATH, L"%ls", g_dir);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (!slash || !release_stamp(slash + 1, own)) return 0;
    *slash = 0;
    wchar_t *up = wcsrchr(parent, L'\\');   /* this one nested too: its siblings are beside the outer folder */
    if (up && !_wcsicmp(up + 1, slash + 1)) *up = 0;
    swprintf(pat, MAX_PATH, L"%ls\\TotK-PC-*", parent);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !release_stamp(fd.cFileName, stamp)) continue;
        if (wcscmp(stamp, own) <= 0 || (best[0] && wcscmp(stamp, best) <= 0) || marked_unusable(fd.cFileName)) continue;
        swprintf(cand, MAX_PATH, L"%ls\\%ls", parent, fd.cFileName);
        if (!release_launcher(cand, exe, MAX_PATH)) {
            swprintf(cand, MAX_PATH, L"%ls\\%ls\\%ls", parent, fd.cFileName, fd.cFileName);
            if (!release_launcher(cand, exe, MAX_PATH)) { say(L"update check: %ls is not a complete release folder", fd.cFileName); continue; }
        }
        wcscpy(best, stamp);
        swprintf(dir, n, L"%ls", cand);
        swprintf(launcher, n, L"%ls", exe);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best[0] != 0;
}

enum { UPDATE_OPEN = 1001, UPDATE_STAY };
static int ask_update(const wchar_t *name) {
    wchar_t open[MAX_PATH + 64], content[1024];
    swprintf(open, MAX_PATH + 64, ES_EN(L"Abrir la versión nueva\n%ls", L"Open the new version\n%ls"), name);
    swprintf(content, 1024, ES_EN(L"Al lado de esta carpeta hay una versión más nueva del juego:\n%ls\n\nTu partida, tus ajustes y la "
                                  L"caché de shaders pasan solos (antes de su primer arranque se guarda una copia de tu partida).",
                                  L"There's a newer version of the game next to this folder:\n%ls\n\nYour save, your settings and the "
                                  L"shader cache carry over on their own (your save is backed up before its first start)."), name);
    const TASKDIALOG_BUTTON b[] = {{UPDATE_OPEN, open},
                                   {UPDATE_STAY, ES_EN(L"Seguir con esta\nNo volveré a preguntar por esa carpeta.",
                                                       L"Keep this one\nI won't ask about that folder again.")}};
    return task(ES_EN(L"Hay una versión más nueva", L"A newer version is available"), content, b, 2, NULL, 0, NULL, 0, TD_INFORMATION_ICON);
}
static int (*g_ask_update)(const wchar_t *name) = ask_update;   /* (test_launcher.c answers instead) */

/* A newer release folder beside this one (newer_release): offers it and starts its launcher on a yes; 1 = started, this
 * launcher exits. "Seguir con esta" is remembered for that folder (g_base\update-declined.txt), so the player is asked
 * again only for an even newer one; closing the dialog asks again next start. */
static int offer_newer_release(void) {
    wchar_t dir[MAX_PATH], launcher[MAX_PATH], declined[MAX_PATH] = L"", path[MAX_PATH];
    if (!newer_release(dir, launcher, MAX_PATH)) { say(L"update check: no newer release folder beside %ls", g_dir); return 0; }
    const wchar_t *name = wcsrchr(dir, L'\\') + 1;
    swprintf(path, MAX_PATH, L"%ls\\update-declined.txt", g_base);
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (f) { if (fgetws(declined, MAX_PATH, f)) declined[wcscspn(declined, L"\r\n")] = 0; fclose(f); }
    if (!_wcsicmp(declined, name)) { say(L"update check: newer release %ls, declined before: not asked again", name); return 0; }
    int r = g_ask_update(name);
    if (r == UPDATE_OPEN) {
        wchar_t cmd[MAX_PATH + 4]; swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", launcher);
        STARTUPINFOW si = {0}; si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(launcher, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            DWORD err = GetLastError();
            message(MB_ICONWARNING, ES_EN(L"No se pudo abrir la versión nueva (error %lu):\n%ls\n\nSigo con esta.",
                                          L"Couldn't open the new version (error %lu):\n%ls\n\nStaying on this one."), err, launcher);
            return 0;
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        say(L"update: the player opened the newer release %ls (pid %lu)", dir, pi.dwProcessId);
        return 1;
    }
    if (r == UPDATE_STAY) {
        f = _wfopen(path, L"w, ccs=UTF-8");
        if (f) { fwprintf(f, L"%ls\n", name); fclose(f); }
        say(L"update: the player stayed on %ls (newer %ls: not asked again for it)", g_dir, name);
    } else {
        say(L"update: newer release %ls offered, dialog closed: asked again next start", name);
    }
    return 0;
}

/* ---------------------------------------------------------------- automatic updates*/

/* At start, at most once a day (NXR_UPDATE_FORCE=1: every start): the latest release on GitHub (autoupdate.c). A newer one
 * than this build's player.txt release= is taken by downloading its Setup, checking it against GitHub's own sha256 digest of
 * the asset (an immutable release only), and running it headless on the same install folder; then the new build's launcher
 * starts and this one exits. An update that recompiles the whole game (update.json, autoupdate.h) asks first: now / next time
 * / skip this version. F10's "Automatic updates" (settings.ini launcher.auto_update = 0) turns the check off. Anything that
 * goes wrong is one log line and the installed build starts: [update]
 * result=<ok|current|off|throttled|norelease|offline|timeout|ratelimit|notfound|http|badjson|draft|mutable|
 * nosetup|nodigest|badsha|short|skipped|declined|cancelled|setup_failed|nolauncher> tag=<tag> ms=<ms> Tests:
 * NXR_UPDATE_API=<base url> (http only for 127.0.0.1 / localhost) instead of https://api.github.com; NXR_UPDATE_NOUI=1 no
 * progress window. NXR_UPDATE_DISABLE=1: no check at all.*/

enum { UPD_API_MS = 4000, UPD_DL_MS = 30000, UPD_DAY_S = 20 * 3600 };
static const wchar_t UPD_DEFAULT_REPO[] = L"daniel-madrid-07/weeping-of-the-empire";
static const wchar_t UPD_SETUP_KEY[] = L"Software\\Weeping of the Empire";   /* (Setup's choices: InstallDir) */

static void upd_result(const char *result, const char *tag, DWORD t0, const wchar_t *detail) {
    say(L"[update] result=%hs tag=%hs ms=%lu%ls%ls", result, tag && tag[0] ? tag : "-", GetTickCount() - t0,
        detail && detail[0] ? L" " : L"", detail ? detail : L"");
}

/* settings.ini's launcher.auto_update: 0 = off (F10 "Automatic updates"); absent = on */
static int upd_enabled(void) {
    wchar_t p[MAX_PATH], line[512];
    swprintf(p, MAX_PATH, L"%ls\\settings.ini", g_base);
    FILE *f = _wfopen(p, L"r, ccs=UTF-8");
    if (!f) return 1;
    int on = 1;
    while (fgetws(line, 512, f)) {
        wchar_t *h = wcschr(line, L'#'); if (h) *h = 0;
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t *k = line; while (iswspace(*k)) k++;
        size_t kl = wcslen(k); while (kl && iswspace(k[kl - 1])) k[--kl] = 0;
        if (wcscmp(k, L"launcher.auto_update")) continue;
        wchar_t *v = eq + 1; while (iswspace(*v)) v++;
        on = !(v[0] == L'0' || !_wcsnicmp(v, L"off", 3) || !_wcsnicmp(v, L"false", 5));
    }
    fclose(f);
    return on;
}

/* once a day: <local>\update-check.txt holds the last check's time (seconds since 1970) */
static int upd_due(void) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\update-check.txt", g_local);
    FILE *f = _wfopen(p, L"r");
    long long last = 0;
    if (f) { if (fscanf(f, "%lld", &last) != 1) last = 0; fclose(f); }
    return _time64(NULL) - last >= UPD_DAY_S;
}
static void upd_checked(void) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\update-check.txt", g_local);
    FILE *f = _wfopen(p, L"w");
    if (f) { fprintf(f, "%lld\n", (long long)_time64(NULL)); fclose(f); }
}

static int upd_skipped(const char *tag) {
    wchar_t p[MAX_PATH]; char s[64] = "";
    swprintf(p, MAX_PATH, L"%ls\\update-skip.txt", g_base);
    FILE *f = _wfopen(p, L"r");
    if (f) { if (fgets(s, sizeof s, f)) s[strcspn(s, "\r\n")] = 0; fclose(f); }
    return s[0] && !au_version_cmp(s, tag);
}
static void upd_skip(const char *tag) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\update-skip.txt", g_base);
    FILE *f = _wfopen(p, L"w");
    if (f) { fprintf(f, "%s\n", tag); fclose(f); }
}

/* the install folder Setup put this build in: its remembered InstallDir when this launcher's folder is inside its game\,
 * else this folder's grandparent (<install>\game\TotK-PC-...) */
static void upd_install_root(wchar_t *out, size_t n) {
    wchar_t dir[MAX_PATH] = L""; DWORD sz = sizeof dir;
    swprintf(out, n, L"%ls", g_dir);
    for (int i = 0; i < 2; i++) { wchar_t *s = wcsrchr(out, L'\\'); if (s) *s = 0; }
    if (RegGetValueW(HKEY_CURRENT_USER, UPD_SETUP_KEY, L"InstallDir", RRF_RT_REG_SZ, NULL, dir, &sz) == ERROR_SUCCESS && dir[0]) {
        wchar_t game[MAX_PATH]; swprintf(game, MAX_PATH, L"%ls\\game", dir);
        if (exists(game)) swprintf(out, n, L"%ls", dir);
    }
}

/* the dialog for an update that recompiles the whole game */
enum { UPD_NOW = 1101, UPD_LATER, UPD_SKIP };
static int upd_ask(const char *tag) {
    wchar_t main[128], content[1024], now[256];
    swprintf(main, 128, ES_EN(L"Hay una versión nueva: %hs", L"A new version is out: %hs"), tag);
    swprintf(content, 1024, ES_EN(L"Esta actualización vuelve a compilar el juego en tu PC: tarda de 15 a 45 minutos, y mientras tanto "
                                  L"no se puede jugar. Tu partida y tus ajustes se conservan.",
                                  L"This update compiles the game again on your PC: it takes 15 to 45 minutes, and you can't play "
                                  L"meanwhile. Your save and your settings are kept."));
    swprintf(now, 256, ES_EN(L"Actualizar ahora\nEl juego se abre solo al terminar.", L"Update now\nThe game opens by itself when it's done."));
    const TASKDIALOG_BUTTON b[] = {{UPD_NOW, now},
                                   {UPD_LATER, ES_EN(L"La próxima vez\nJuego con la versión instalada.", L"Next time\nPlay the installed version.")},
                                   {UPD_SKIP, ES_EN(L"Saltar esta versión\nNo volveré a preguntar por ella.",
                                                    L"Skip this version\nI won't ask about it again.")}};
    return task(main, content, b, 3, NULL, 0, NULL, 0, TD_INFORMATION_ICON);
}
static int (*g_upd_ask)(const char *tag) = upd_ask;   /* (test_launcher.c answers instead) */

/* The download and the Setup run, on a worker thread while the progress window shows */
typedef struct Upd {
    wchar_t setup_url[512], setup_path[MAX_PATH], root[MAX_PATH], log[MAX_PATH];
    char tag[48], sha256[65];
    volatile LONG pct, cancel, done;   /* done: 1 when the worker ended */
    volatile LONG result;              /* 0 ok, else an index into upd_codes */
    wchar_t status[256], detail[512];
    HANDLE setup;
    CRITICAL_SECTION cs;
} Upd;
enum { U_OK, U_SHORT, U_BADSHA, U_CANCELLED, U_SETUP_FAILED };
static const char *const upd_codes[] = {"ok", "short", "badsha", "cancelled", "setup_failed"};

static void upd_status(Upd *u, const wchar_t *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    EnterCriticalSection(&u->cs);
    vswprintf(u->status, 256, fmt, ap);
    LeaveCriticalSection(&u->cs);
    va_end(ap);
}

static int upd_progress(long long done, long long total, void *ctx) {
    Upd *u = ctx;
    if (total > 0) InterlockedExchange(&u->pct, (LONG)(done * 40 / total));   /* (the download: the first 40 %) */
    upd_status(u, ES_EN(L"Descargando el instalador: %lld de %lld MB", L"Downloading the installer: %lld of %lld MB"),
               done >> 20, total > 0 ? total >> 20 : 0);
    return u->cancel != 0;
}

/* Setup's log: its 'step N/M name' and 'NN% (' lines move the window (the last 60 %) */
static void upd_follow_log(Upd *u, long long *pos) {
    FILE *f = _wfopen(u->log, L"rb");
    if (!f) return;
    _fseeki64(f, *pos, SEEK_SET);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) break;   /* (a line still being written: read it next time) */
        *pos = _ftelli64(f);
        int i, n; char name[32];
        const char *s = strstr(line, "step ");
        if (s && sscanf(s, "step %d/%d %31[a-z]", &i, &n, name) == 3 && n > 0) {
            InterlockedExchange(&u->pct, 40 + 60 * i / (n + 1));
            wchar_t w[32]; MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 32);
            upd_status(u, ES_EN(L"Instalando: paso %d de %d (%ls)", L"Installing: step %d of %d (%ls)"), i, n, w);
        }
    }
    fclose(f);
}

static DWORD WINAPI upd_worker(void *arg) {
    Upd *u = arg;
    wchar_t err[256] = L"";
    int st = au_http_download(u->setup_url, u->setup_path, UPD_DL_MS, upd_progress, u, err, 256);
    if (st != 200) {
        u->result = st == -1 ? U_CANCELLED : U_SHORT;
        swprintf(u->detail, 512, L"%ls", err);
        InterlockedExchange(&u->done, 1);
        return 0;
    }
    char hex[65] = "";
    /* locked from the hash until Setup has started: the folder is the player's, nothing may swap the file between */
    HANDLE lock = au_lock_file(u->setup_path);
    if (!au_sha256_handle(lock, hex) || _stricmp(hex, u->sha256)) {
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        DeleteFileW(u->setup_path);
        u->result = U_BADSHA;
        swprintf(u->detail, 512, L"downloaded %hs, GitHub's digest %hs", hex, u->sha256);
        InterlockedExchange(&u->done, 1);
        return 0;
    }
    /* Setup headless on the same folder (its remembered dump, keys and choices): --auto presses Install, --hidden has no
       window and exits 0 / 1 at the end. Its setup.log is followed for the window. No --accept-terms: Setup takes the
       player's earlier acceptance only for the same terms (its TermsVersion), else it shows them.*/
    swprintf(u->log, MAX_PATH, L"%ls\\setup.log", u->root);
    long long pos = 0;
    { FILE *f = _wfopen(u->log, L"rb"); if (f) { _fseeki64(f, 0, SEEK_END); pos = _ftelli64(f); fclose(f); } }
    wchar_t cmd[MAX_PATH * 3];
    swprintf(cmd, MAX_PATH * 3, L"\"%ls\" --install-to=\"%ls\" --auto --hidden", u->setup_path, u->root);
    STARTUPINFOW si = {0}; si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessW(u->setup_path, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, u->root, &si, &pi);
    DWORD start_err = GetLastError();
    CloseHandle(lock);
    if (!started) {
        SetLastError(start_err);
        u->result = U_SETUP_FAILED;
        swprintf(u->detail, 512, L"could not start Setup (error %lu)", GetLastError());
        InterlockedExchange(&u->done, 1);
        return 0;
    }
    CloseHandle(pi.hThread);
    u->setup = pi.hProcess;
    upd_status(u, ES_EN(L"Instalando...", L"Installing..."));
    while (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT) {
        upd_follow_log(u, &pos);
        if (u->cancel) TerminateProcess(pi.hProcess, 1);   /* (Setup's job ends its build; Install again resumes it) */
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    if (code) {
        u->result = u->cancel ? U_CANCELLED : U_SETUP_FAILED;
        swprintf(u->detail, 512, L"Setup exit %lu, see %ls", code, u->log);
    } else {
        u->result = U_OK;
        InterlockedExchange(&u->pct, 100);
    }
    InterlockedExchange(&u->done, 1);
    return 0;
}

static HRESULT CALLBACK upd_dialog_cb(HWND h, UINT msg, WPARAM w, LPARAM l, LONG_PTR ref) {
    Upd *u = (Upd *)ref;
    (void)l;
    if (msg == TDN_CREATED) SendMessageW(h, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 100));
    if (msg == TDN_TIMER) {
        wchar_t s[256];
        EnterCriticalSection(&u->cs); swprintf(s, 256, L"%ls", u->status); LeaveCriticalSection(&u->cs);
        SendMessageW(h, TDM_SET_PROGRESS_BAR_POS, (WPARAM)u->pct, 0);
        SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)s);
        if (u->done) SendMessageW(h, TDM_CLICK_BUTTON, IDOK, 0);
    }
    if (msg == TDN_BUTTON_CLICKED && w == IDCANCEL && !u->done) {
        InterlockedExchange(&u->cancel, 1);
        upd_status(u, ES_EN(L"Cancelando...", L"Cancelling..."));
        return S_FALSE;   /* (stays open until the worker has stopped) */
    }
    return S_OK;
}

/* 1 = the updated build's launcher was started (this one exits); 0 = play the installed build */
static int auto_update(void) {
    DWORD t0 = GetTickCount();
    wchar_t rel[48] = L"", key[64] = L"", repo[128] = L"", api[512] = L"", url[1024], err[256] = L"";
    char tag[48] = "", installed[48] = "";
    package_value(g_dir, L"release", 0, rel, 48);
    package_value(g_dir, L"key", 0, key, 64);
    package_value(g_dir, L"release_repo", 0, repo, 128);
    if (!rel[0]) { upd_result("norelease", NULL, t0, L"(a build Setup did not make: no automatic update)"); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, rel, -1, installed, sizeof installed, NULL, NULL);
    if (!upd_enabled()) { upd_result("off", NULL, t0, L"(F10: Automatic updates off)"); return 0; }
    if (GetEnvironmentVariableW(L"NXR_UPDATE_DISABLE", url, 8) && url[0] == L'1') {   
        upd_result("off", NULL, t0, L"(NXR_UPDATE_DISABLE=1)");
        return 0;
    }
    int force = GetEnvironmentVariableW(L"NXR_UPDATE_FORCE", url, 8) && url[0] == L'1';
    if (!force && !upd_due()) { upd_result("throttled", NULL, t0, L"(checked in the last 20 h)"); return 0; }
    if (!GetEnvironmentVariableW(L"NXR_UPDATE_API", api, 512) || !api[0]) wcscpy(api, L"https://api.github.com");
    size_t al = wcslen(api); while (al && api[al - 1] == L'/') api[--al] = 0;
    swprintf(url, 1024, L"%ls/repos/%ls/releases/latest", api, repo[0] ? repo : UPD_DEFAULT_REPO);
    char *body = NULL; size_t len = 0;
    int st = au_http_get(url, UPD_API_MS, &body, &len, err, 256);
    if (st) upd_checked();   /* (an answer, any: no second ask within the day) */
    if (!st) { upd_result(wcsstr(err, L"timed out") ? "timeout" : "offline", NULL, t0, err); return 0; }
    if (st == 403 || st == 429) { upd_result("ratelimit", NULL, t0, NULL); free(body); return 0; }
    if (st == 404) { upd_result("notfound", NULL, t0, NULL); free(body); return 0; }
    if (st != 200) { swprintf(err, 256, L"HTTP %d", st); upd_result("http", NULL, t0, err); free(body); return 0; }
    static AuRelease r;
    int ok = au_parse_release(body, len, &r);
    free(body);
    if (!ok) { upd_result("badjson", NULL, t0, NULL); return 0; }
    strcpy(tag, r.tag);
    if (r.draft || r.prerelease) { upd_result("draft", tag, t0, NULL); return 0; }
    if (au_version_cmp(tag, installed) <= 0) { upd_result("current", tag, t0, rel); return 0; }
    if (upd_skipped(tag)) { upd_result("skipped", tag, t0, L"(the player skipped this version)"); return 0; }
    if (!r.immutable) { upd_result("mutable", tag, t0, L"(not an immutable release: not taken)"); return 0; }
    char suffix[80]; snprintf(suffix, sizeof suffix, "-Setup-%s.exe", tag);
    const AuAsset *setup = au_find_asset(&r, NULL, suffix);
    if (!setup) { upd_result("nosetup", tag, t0, NULL); return 0; }
    if (!setup->sha256[0]) { upd_result("nodigest", tag, t0, NULL); return 0; }
    /* the rebuild question: update.json's same_key_as (missing / unreadable = a rebuild, asked) */
    AuUpdateJson uj; int have_uj = 0;
    const AuAsset *ua = au_find_asset(&r, "update.json", NULL);
    if (ua) {
        wchar_t uurl[512]; MultiByteToWideChar(CP_UTF8, 0, ua->url, -1, uurl, 512);
        char *ub = NULL; size_t ul = 0;
        if (au_http_get(uurl, UPD_API_MS, &ub, &ul, err, 256) == 200) have_uj = au_parse_update_json(ub, ul, &uj);
        free(ub);
    }
    int rebuild = au_needs_rebuild(&uj, have_uj, installed);
    say(L"update: %hs is out (installed %ls, key %ls): %ls", tag, rel, key[0] ? key : L"?",
        rebuild ? L"it recompiles the game: asking" : L"same game key: taken now");
    if (rebuild) {
        int a = g_upd_ask(tag);
        if (a == UPD_SKIP) { upd_skip(tag); upd_result("skipped", tag, t0, L"(the player skipped it)"); return 0; }
        if (a != UPD_NOW) { upd_result("declined", tag, t0, L"(next time)"); return 0; }
    }
    static Upd u;
    memset(&u, 0, sizeof u);
    InitializeCriticalSection(&u.cs);
    strcpy(u.tag, tag);
    strcpy(u.sha256, setup->sha256);
    MultiByteToWideChar(CP_UTF8, 0, setup->url, -1, u.setup_url, 512);
    upd_install_root(u.root, MAX_PATH);
    wchar_t dl[MAX_PATH], wname[128];
    MultiByteToWideChar(CP_UTF8, 0, setup->name, -1, wname, 128);
    swprintf(dl, MAX_PATH, L"%ls\\updates", g_local);
    mkdirs(dl);
    swprintf(u.setup_path, MAX_PATH, L"%ls\\%ls", dl, wname);
    upd_status(&u, ES_EN(L"Preparando...", L"Getting ready..."));
    say(L"update: %hs -> %ls, then Setup on %ls", tag, u.setup_path, u.root);
    HANDLE th = CreateThread(NULL, 0, upd_worker, &u, 0, NULL);
    wchar_t main[128]; swprintf(main, 128, ES_EN(L"Actualizando a %hs", L"Updating to %hs"), tag);
    if (GetEnvironmentVariableW(L"NXR_UPDATE_NOUI", err, 8) && err[0] == L'1') {
        WaitForSingleObject(th, INFINITE);   /* (tests: no window) */
    } else {
        TASKDIALOGCONFIG c = {0};
        c.cbSize = sizeof c;
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
        c.pszWindowTitle = TITLE;
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = main;
        c.pszContent = u.status;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pfCallback = upd_dialog_cb;
        c.lpCallbackData = (LONG_PTR)&u;
        TaskDialogIndirect(&c, NULL, NULL, NULL);
        WaitForSingleObject(th, INFINITE);
    }
    CloseHandle(th);
    if (u.setup) CloseHandle(u.setup);
    if (u.result != U_OK) {
        upd_result(upd_codes[u.result], tag, t0, u.detail);
        if (u.result != U_CANCELLED)
            message(MB_ICONWARNING, ES_EN(L"No se pudo actualizar a %hs; se abre la versión instalada.\n\n%ls",
                                          L"Couldn't update to %hs; the installed version opens.\n\n%ls"), tag, u.detail);
        return 0;
    }
    DeleteFileW(u.setup_path);
    wchar_t dir[MAX_PATH], launcher[MAX_PATH];
    if (!newer_release(dir, launcher, MAX_PATH)) { upd_result("nolauncher", tag, t0, L"(Setup ended without a newer folder)"); return 0; }
    wchar_t cmd[MAX_PATH + 4]; swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", launcher);
    STARTUPINFOW si = {0}; si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(launcher, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        swprintf(err, 256, L"could not start %ls (error %lu)", launcher, GetLastError());
        upd_result("nolauncher", tag, t0, err);
        return 0;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    upd_result("ok", tag, t0, dir);
    return 1;
}

/* ---------------------------------------------------------------- the CPU */

/* totk.exe is built for x86-64-v3 (mkninja_win.py -march=x86-64-v3): on a CPU without one of these it dies on its first
   such instruction (0xC000001D) with nothing to tell the player why. The launcher itself is baseline x86-64, so it can
   check first. Writes the missing ones to `out` ("" when the CPU has them all). */
/* The instructions a build needs that this CPU lacks, comma-separated in out ("" = none): level 3 = x86-64-v3 (every
 * build but the Builder's --cpu v2), 2 = x86-64-v2 (build.py --march x86-64-v2, package.py cpu=x86-64-v2). */
static void cpu_missing(wchar_t *out, size_t n, int level) {
    unsigned a, b, c, d, c1 = 0, b7 = 0, ce = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) c1 = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) b7 = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) ce = c;
    unsigned xcr0 = 0;
    if (c1 & 1u << 27) { unsigned hi; __asm__ volatile("xgetbv" : "=a"(xcr0), "=d"(hi) : "c"(0)); }   /* OSXSAVE */
    const struct { const wchar_t *name; int ok; } need[] = {
        {L"SSE4.2", !!(c1 & 1u << 20)}, {L"POPCNT", !!(c1 & 1u << 23)}, {L"SSSE3", !!(c1 & 1u << 9)}, {L"AVX", !!(c1 & 1u << 28)},
        {ES_EN(L"AVX (activado por Windows)", L"AVX (enabled by Windows)"), (xcr0 & 6) == 6}, {L"AVX2", !!(b7 & 1u << 5)}, {L"FMA", !!(c1 & 1u << 12)},
        {L"BMI1", !!(b7 & 1u << 3)}, {L"BMI2", !!(b7 & 1u << 8)}, {L"F16C", !!(c1 & 1u << 29)},
        {L"LZCNT", !!(ce & 1u << 5)}, {L"MOVBE", !!(c1 & 1u << 22)},
    };
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < (level >= 3 ? sizeof need / sizeof need[0] : 3); i++)   /* (v2: the first three) */
        if (!need[i].ok && len + 40 < n) len += swprintf(out + len, n - len, L"%ls%ls", len ? L", " : L"", need[i].name);
}

/* The game's cutscene movies are VP9. Windows decodes them through the Store's "VP9 Video Extensions" (a Media Foundation
 * MFT, on the GPU where it can); installs without the Store (LTSC, N, offline images) lack it, and the game then decodes
 * them with its built-in libvpx on the CPU (engine/runtime/movie_core.h; issues #22, #25). 1 = the MFT is there, 0 = no
 * VP9 MFT, -1 = no Media Foundation at all (an N edition without the Media Feature Pack). */
static int vp9_mft_present(void) {
    HMODULE mf = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mf) return -1;
    typedef struct { GUID major, sub; } TypeInfo;   /* MFT_REGISTER_TYPE_INFO */
    typedef HRESULT(WINAPI * EnumEx)(GUID, UINT32, const TypeInfo *, const TypeInfo *, IUnknown ***, UINT32 *);
    EnumEx enum_ex = (EnumEx)(void (*)(void))GetProcAddress(mf, "MFTEnumEx");
    int r = -1;
    if (enum_ex) {
        static const GUID video_decoder = {0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
        const TypeInfo in = {{0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}},   /* MFMediaType_Video */
                             {0x30395056, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}};  /* MFVideoFormat_VP90 */
        IUnknown **acts = NULL; UINT32 n = 0;
        /* SYNCMFT | LOCALMFT | SORTANDFILTER | UNTRUSTED_STOREMFT: the flags the game's decoder enumerates with */
        r = SUCCEEDED(enum_ex(video_decoder, 0x1 | 0x8 | 0x40 | 0x400, &in, NULL, &acts, &n)) && n > 0;
        for (UINT32 i = 0; i < n; i++) IUnknown_Release(acts[i]);
        if (acts) CoTaskMemFree(acts);
    }
    FreeLibrary(mf);
    return r;
}
static int (*g_vp9_probe)(void) = vp9_mft_present;   /* (test_launcher.c answers instead) */

/* Logged at every start; said once (<local>\vp9-notice.txt), since the movies play either way. */
static void vp9_notice(void) {
    int r = g_vp9_probe();
    say(L"vp9: %ls", r > 0 ? L"Media Foundation decoder found" : r == 0 ? L"no VP9 MFT (the game decodes movies with libvpx)"
                                                                       : L"no Media Foundation (the game decodes movies with libvpx)");
    if (r > 0) return;
    wchar_t mark[MAX_PATH]; swprintf(mark, MAX_PATH, L"%ls\\vp9-notice.txt", g_local);
    if (exists(mark)) return;
    message(MB_ICONINFORMATION,
            ES_EN(L"Este Windows no tiene las «Extensiones de vídeo VP9» de Microsoft Store%ls.\n\n"
                  L"Las cinemáticas se ven igual: el juego trae su propio decodificador de vídeo (libvpx) y las decodifica con el "
                  L"procesador en vez de con la tarjeta gráfica. Si quieres que use la tarjeta gráfica, instala «Extensiones de "
                  L"vídeo VP9» desde Microsoft Store.\n\nEste aviso solo sale una vez.",
                  L"This Windows doesn't have the Microsoft Store's \"VP9 Video Extensions\"%ls.\n\n"
                  L"Cutscenes look the same: the game brings its own video decoder (libvpx) and decodes them on the processor "
                  L"instead of the graphics card. If you want the graphics card to do it, install \"VP9 Video Extensions\" "
                  L"from the Microsoft Store.\n\nThis notice only shows once."),
            r < 0 ? ES_EN(L" (ni Media Foundation: una edición N sin el Media Feature Pack)", L" (nor Media Foundation: an N edition without the Media Feature Pack)") : L"");
    FILE *f = _wfopen(mark, L"w");
    if (f) { fputs("shown\n", f); fclose(f); }
}

/* ---------------------------------------------------------------- the game */

enum { RUNNING, QUIT, FAILED, MISSING, RESTART };

/* The game asks to be started again (engine/runtime/pc_restart.cpp: F10's "Restart the game now", the in-game skip
 * into a new game): it writes the variables to add to <local>\restart.env ("KEY=VALUE", an empty value removes KEY)
 * and ends with RESTART_CODE; the package starts again here, watched as before. The variables of the last restart
 * are removed first, so a plain restart after a skip does not skip again.*/
#define RESTART_CODE 0x52535452u
/* The game found no GPU that can run it and has already said so in its own message box, in Spanish and English
 * (engine/runtime/nvn/vulkan/device.c gpu_unsupported): no second dialog, and no older package tried (it would fail
 * the same way). */
#define GPU_UNSUPPORTED_CODE 0x47505530u   /* "GPU0" */
#define DUMP_LOST_CODE 0x444D5030u         /* "DMP0": the game dump went away during play (hle_fs.cpp dump_lost; it said so) */
static wchar_t g_restart_file[MAX_PATH];
static wchar_t g_restart_set[8][64];
static int g_restart_n;
static void restart_env_apply(void) {
    for (int i = 0; i < g_restart_n; i++) SetEnvironmentVariableW(g_restart_set[i], NULL);
    g_restart_n = 0;
    FILE *f = _wfopen(g_restart_file, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        line[wcscspn(line, L"\r\n")] = 0;
        wchar_t *eq = wcschr(line, L'=');
        if (!eq || eq == line || eq - line >= 64) continue;
        *eq = 0;
        SetEnvironmentVariableW(line, eq[1] ? eq + 1 : NULL);
        say(L"restart: %ls=%ls", line, eq + 1);
        if (g_restart_n < 8) wcscpy(g_restart_set[g_restart_n++], line);
    }
    fclose(f);
    DeleteFileW(g_restart_file);
}
/* Mods: every folder in <g_dir>\mods (beside the launcher, in the game folder: where players look, as other ports
 * keep it; Setup updates that folder in place) and in <g_base>\mods (per user like the save) is a mod laid out as
 * emulators and the Switch take them (<name>\romfs, atmosphere\contents\<title>\romfs, ...;
 * engine/runtime/mods_scan.h). The game scans them (engine/runtime/mods.cpp): order and switches are the F10 Mods
 * tab's (settings.ini mods.order / mods.off), the top mod wins a file two of them change, the port's own files win
 * over a mod's, the resource size table is merged and computed for the mods' files (hle_fs.cpp rstb_merged). A name
 * starting with '_' or '.' is left out; a name in both folders is used from the game folder's.
 * Another game version's package (player.txt version=, e.g. 1.4.3: `ver` "1.4.3", `tag` "143") reads mods-v<tag> in
 * both places instead, as its saves live in userdata-v<tag>: a mod is made for one version, and a 1.4.3 mod's files
 * are wrong in 1.0.0 and the other way round.
 * Sets TOTK_MODS_DIR (the game folder's first: F10's "Open mods folder" opens it); returns how many folders exist. */
static void mods_readme(const wchar_t *dir, const wchar_t *ver) {   /* (rewritten each start) */
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\LEEME-README.txt", dir);
    FILE *f = _wfopen(p, L"w, ccs=UTF-8");
    if (!f) return;
    if (ver && *ver)
        fwprintf(f, L"Mods para la versión %ls del juego (los de 1.0.0 van en la carpeta mods).\n"
                    L"Mods for game version %ls (1.0.0's go in the mods folder).\n\n", ver, ver);
    fputws(L"Mods: una carpeta por mod, como en los emuladores (<nombre>\\romfs, <nombre>\\exefs, <nombre>\\cheats, o\n"
           L"atmosphere\\contents\\0100F2C0115B6000\\romfs de la tarjeta SD; la salida de TKMM también vale).\n"
           L"Orden y activación en el juego: F10 > Mods (gana el de arriba); se aplican al volver a empezar.\n"
           L"Un nombre que empieza por _ lo deja fuera. La tabla de tamaños (RSTB) se ajusta sola.\n"
           L"Sirven esta carpeta (junto al juego) y %APPDATA%\\nxrecomp\\TotK-engine\\mods.\n\n"
           L"Mods: one folder per mod, as emulators take them (<name>\\romfs, <name>\\exefs, <name>\\cheats, or the SD\n"
           L"card's atmosphere\\contents\\0100F2C0115B6000\\romfs; TKMM's merged output works too).\n"
           L"Order and switches in the game: F10 > Mods (the top one wins); they apply at the next start.\n"
           L"A name starting with _ leaves it out. The resource size table (RSTB) is adjusted by itself.\n"
           L"This folder (beside the game) and %APPDATA%\\nxrecomp\\TotK-engine\\mods both work.\n\n"
           L"Guía completa / full guide: https://github.com/daniel-madrid-07/weeping-of-the-empire/blob/main/MODS.md\n"
           L"Controles / controls: https://github.com/daniel-madrid-07/weeping-of-the-empire/blob/main/CONTROLS.md\n"
           L"Ajustes (F10) / settings: https://github.com/daniel-madrid-07/weeping-of-the-empire/blob/main/SETTINGS.md\n", f);
    fclose(f);
}
/* A mod dropped on the launcher (or its shortcut): each path on the command line that exists (an archive or a folder;
 * options start with "--") is installed into the game folder's mods / mods-v143 by mods_install.h, the version in use
 * being this folder's game's (its player.txt version=; none = 1.0.0), and said in one box; then the game starts as
 * usual. Nothing but the two mods folders is written. */
static void mi_version_text(int v, wchar_t *out, size_t n) {   /* 143 -> "1.4.3" */
    wchar_t d[16];
    swprintf(d, 16, L"%d", v);
    out[0] = 0;
    for (size_t i = 0; d[i] && wcslen(out) + 2 < n; i++) {
        size_t l = wcslen(out);
        if (i) out[l++] = L'.';
        out[l++] = d[i];
        out[l] = 0;
    }
}
static int install_paths(wchar_t **argv, int argc) {   /* argv[1..argc-1] (test_launcher.c calls it directly) */
    int done = 0;
    wchar_t cur[MAX_PATH] = L"", pkg[MAX_PATH], ver[32] = L"";
    read_name(L"current.txt", cur, MAX_PATH);
    if (cur[0]) swprintf(pkg, MAX_PATH, L"%ls\\packages\\%ls", g_dir, cur);
    else swprintf(pkg, MAX_PATH, L"%ls", g_dir);
    package_value(pkg, L"version", 0, ver, 32);
    int in_use = 100;
    if (ver[0] && wcscmp(ver, L"1.0.0")) {
        in_use = 0;
        for (const wchar_t *c = ver; *c; c++) if (*c >= L'0' && *c <= L'9') in_use = in_use * 10 + (*c - L'0');
    }
    wchar_t d100[MAX_PATH], d143[MAX_PATH];
    swprintf(d100, MAX_PATH, L"%ls\\mods", g_dir);
    swprintf(d143, MAX_PATH, L"%ls\\mods-v143", g_dir);
    for (int i = 1; i < argc; i++) {
        /* (a folder, or an archive by its extension: any other file on the command line is not a dropped mod) */
        const DWORD at = GetFileAttributesW(argv[i]);
        const wchar_t *ext = wcsrchr(argv[i], L'.');
        if (!wcsncmp(argv[i], L"--", 2) || at == INVALID_FILE_ATTRIBUTES ||
            (!(at & FILE_ATTRIBUTE_DIRECTORY) && !(ext && (!_wcsicmp(ext, L".zip") || !_wcsicmp(ext, L".7z") || !_wcsicmp(ext, L".rar")))))
            continue;
        ModInstall m;
        const int rc = mods_install(argv[i], d100, d143, in_use, &m);
        done++;
        say(L"mod install %ls: rc %d, kinds %d, version %d (%ls), %ls", argv[i], rc, m.kinds, m.version,
            m.named ? L"named by its files" : L"the one in use", rc == MI_OK ? m.dest : L"-");
        if (rc != MI_OK) {
            const wchar_t *why =
                rc == MI_UNPACK ? ES_EN(L"no se ha podido abrir. Descomprímelo tú y arrastra su carpeta al lanzador",
                                        L"it could not be opened. Unpack it yourself and drop its folder on the launcher")
                : rc == MI_NOT_A_MOD ? ES_EN(L"no tiene carpeta romfs, exefs ni cheats. Si el mod va comprimido dentro, "
                                             L"descomprímelo y arrastra esa carpeta",
                                             L"it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack "
                                             L"it and drop that folder instead")
                : rc == MI_OTHER_GAME ? ES_EN(L"es un mod de otro juego", L"it is a mod for another game")
                : rc == MI_CODE_MODULES ? ES_EN(L"carga código propio (subsdk / main), que un juego recompilado no puede cargar",
                                                L"it loads code of its own (subsdk / main), which a recompiled game cannot load")
                : ES_EN(L"no se ha podido copiar. Comprueba que se puede escribir en la carpeta mods junto al juego",
                        L"it could not be copied. Check that the mods folder beside the game can be written");
            message(MB_ICONWARNING, ES_EN(L"No he instalado el mod:\n%ls\n\nMotivo: %ls.", L"The mod was not installed:\n%ls\n\nWhy: %ls."),
                    argv[i], why);
            continue;
        }
        wchar_t what[96] = L"", vtext[16], use[16];
        if (m.kinds & MI_ROMFS) wcscat(what, L"RomFS");
        if (m.kinds & MI_EXEFS) wcscat(what, what[0] ? ES_EN(L" + parches de código", L" + code patches") : ES_EN(L"parches de código", L"code patches"));
        if (m.kinds & MI_CHEATS) wcscat(what, what[0] ? ES_EN(L" + trucos", L" + cheats") : ES_EN(L"trucos", L"cheats"));
        mi_version_text(m.version, vtext, 16);
        mi_version_text(in_use, use, 16);
        wchar_t note[512] = L"";
        if (!m.named)
            swprintf(note, 512, ES_EN(L"\nSus archivos no dicen la versión del juego: lo pongo para la que juegas (%ls).",
                                      L"\nIts files don't name a game version: it was put with the one you play (%ls)."), use);
        else if (m.version != 100 && m.version != 143)
            swprintf(note, 512, ES_EN(L"\nEstá hecho para la versión %ls del juego, que el port no crea: lo pongo con los mods de 1.4.3, "
                                      L"puede no funcionar.",
                                      L"\nIt is made for game version %ls, which the port does not build: it was put with the 1.4.3 "
                                      L"mods and may not work."), vtext);
        message(MB_ICONINFORMATION, ES_EN(L"Mod instalado: %ls (%ls, para %ls)\nen %ls%ls\n\nOrden y activación: F10 > Mods.",
                                          L"Mod installed: %ls (%ls, for %ls)\nin %ls%ls\n\nOrder and switches: F10 > Mods."),
                m.name, what, vtext, m.dest, note);
    }
    return done;
}
static int install_dropped(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    const int done = install_paths(argv, argc);
    LocalFree(argv);
    return done;
}

static int mods_env(const wchar_t *ver, const wchar_t *tag) {
    static wchar_t value[2 * MAX_PATH + 2];
    wchar_t local[MAX_PATH], user[MAX_PATH], name[48] = L"mods";
    if (tag && *tag) swprintf(name, 48, L"mods-v%ls", tag);
    swprintf(local, MAX_PATH, L"%ls\\%ls", g_dir, name);
    swprintf(user, MAX_PATH, L"%ls\\%ls", g_base, name);
    mkdirs(local);
    mkdirs(user);
    value[0] = 0;
    int n = 0;
    const wchar_t *dirs[2] = {local, user};
    for (int i = 0; i < 2; i++) {
        if (!exists(dirs[i]) || (i && !_wcsicmp(local, user))) continue;
        mods_readme(dirs[i], tag && *tag ? ver : NULL);
        size_t len = wcslen(value);
        swprintf(value + len, sizeof value / sizeof *value - len, L"%ls%ls", len ? L";" : L"", dirs[i]);
        n++;
    }
    SetEnvironmentVariableW(L"TOTK_MODS_DIR", n ? value : NULL);
    SetEnvironmentVariableW(L"TOTK_USER_MODS", NULL);
    say(L"mods: TOTK_MODS_DIR=%ls", value);
    return n;
}
/* The player's amiibo dumps (engine/runtime/pc_amiibo.cpp, F10 > Amiibo): an amiibo folder beside the game, made here
 * as mods is, and %APPDATA%\nxrecomp\TotK-engine\amiibo when it exists; the first is the one F10 opens. */
static void amiibo_env(void) {
    static wchar_t value[2 * MAX_PATH + 2];
    wchar_t local[MAX_PATH], user[MAX_PATH];
    swprintf(local, MAX_PATH, L"%ls\\amiibo", g_dir);
    swprintf(user, MAX_PATH, L"%ls\\amiibo", g_base);
    mkdirs(local);
    value[0] = 0;
    const wchar_t *dirs[2] = {local, user};
    for (int i = 0; i < 2; i++) {
        if (!exists(dirs[i]) || (i && !_wcsicmp(local, user))) continue;
        size_t len = wcslen(value);
        swprintf(value + len, sizeof value / sizeof *value - len, L"%ls%ls", len ? L";" : L"", dirs[i]);
    }
    SetEnvironmentVariableW(L"TOTK_AMIIBO_DIR", value[0] ? value : NULL);
    say(L"amiibo: TOTK_AMIIBO_DIR=%ls", value);
}

/* ---------------------------------------------------------------- the game's data in the player's dump */
/* Since v0.1.8 Setup leaves the game's data in the player's own dump (romfs-source.txt, pipeline/nx/romsource.py; the
 * game reads and decrypts it in place, engine/runtime/rom_source.h): no 17 GB romfs.bin. The dump has to stay where
 * Setup read it. Moved or deleted: one sentence and the choice to pick it again; a picked file is taken only when it is
 * the same size and the first 64 KiB of the game's data in it are the same bytes (sha256 of the raw bytes: no keys
 * needed here). Then the file's dump= line, Setup's Dump value and the file's record (<file>.verified.json, which the
 * next Setup run makes again) follow. */
typedef struct {
    wchar_t dump[MAX_PATH];
    unsigned long long dump_size, image_offset, image_size;
    char check[65];
} DumpSource;

static int is_dump_source(const wchar_t *p) {
    size_t n = wcslen(p);
    return n >= 16 && !_wcsicmp(p + n - 16, L"romfs-source.txt");
}
static char *read_small_text(const wchar_t *p) {   /* the whole file, NUL-terminated; NULL: none or over 1 MB */
    FILE *f = _wfopen(p, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 && n <= (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (b) b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}
static int source_value(const char *text, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            size_t m = strcspn(l + kl + 1, "\r\n");
            if (m >= n) return 0;
            memcpy(out, l + kl + 1, m);
            out[m] = 0;
            return 1;
        }
    return 0;
}
static int read_dump_source(const wchar_t *p, DumpSource *s) {
    char *t = read_small_text(p), v[4 * MAX_PATH];
    int ok = t && source_value(t, "dump", v, sizeof v) && MultiByteToWideChar(CP_UTF8, 0, v, -1, s->dump, MAX_PATH) > 0;
    ok = ok && source_value(t, "dump_size", v, sizeof v) && (s->dump_size = _strtoui64(v, NULL, 10)) > 0;
    ok = ok && source_value(t, "image_offset", v, sizeof v) && ((s->image_offset = _strtoui64(v, NULL, 10)), 1);
    ok = ok && source_value(t, "image_size", v, sizeof v) && (s->image_size = _strtoui64(v, NULL, 10)) > 0;
    ok = ok && source_value(t, "check", s->check, sizeof s->check) && strlen(s->check) == 64;
    free(t);
    return ok;
}
/* the file at `path` is the dump `s` names: the same size, the same raw bytes where the game's data starts */
static int dump_matches(const wchar_t *path, const DumpSource *s) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz, at;
    at.QuadPart = (LONGLONG)s->image_offset;
    int ok = GetFileSizeEx(h, &sz) && (unsigned long long)sz.QuadPart == s->dump_size && SetFilePointerEx(h, at, NULL, FILE_BEGIN);
    if (ok) {
        DWORD want = (DWORD)(s->image_size < 65536 ? s->image_size : 65536), got = 0;
        unsigned char *buf = malloc(want), d[32];
        BCRYPT_ALG_HANDLE alg = NULL;
        char hex[65];
        ok = buf && ReadFile(h, buf, want, &got, NULL) && got == want &&
             BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptHash(alg, NULL, 0, buf, want, d, 32) == 0;
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        free(buf);
        if (ok) {
            for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
            ok = !_stricmp(hex, s->check);
        }
    }
    CloseHandle(h);
    return ok;
}
/* romfs-source.txt `p` with its dump= line naming `path` (replaced whole); its record goes */
static int rewrite_dump_source(const wchar_t *p, const wchar_t *path) {
    char *t = read_small_text(p), u8[4 * MAX_PATH];
    wchar_t tmp[MAX_PATH], rec[MAX_PATH];
    if (!t || !WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof u8, NULL, NULL)) { free(t); return 0; }
    swprintf(tmp, MAX_PATH, L"%ls.partial", p);
    FILE *f = _wfopen(tmp, L"wb");
    int ok = f != NULL;
    for (const char *l = t; ok && *l;) {
        const char *e = strchr(l, '\n');
        size_t m = e ? (size_t)(e - l + 1) : strlen(l);
        if (!strncmp(l, "dump=", 5)) fprintf(f, "dump=%s\n", u8);
        else fwrite(l, 1, m, f);
        l += m;
    }
    if (f) ok = !fclose(f) && ok;
    free(t);
    ok = ok && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);
    swprintf(rec, MAX_PATH, L"%ls.verified.json", p);
    if (ok) DeleteFileW(rec);
    return ok;
}
enum { DUMP_PICK = 1101, DUMP_CLOSE };
static int pick_dump(const wchar_t *was, wchar_t *out, size_t n) {
    wchar_t content[1024];
    swprintf(content, 1024, ES_EN(L"El juego lee sus datos de tu copia del juego (el dump), que ya no está en:\n%ls\n\nSi la has "
                                  L"movido, elígela otra vez. Si la vas a borrar, vuelve a instalar con Setup marcando \"Copiar los "
                                  L"archivos del juego\".",
                                  L"The game reads its data from your game dump, which is no longer at:\n%ls\n\nIf you moved it, pick "
                                  L"it again. If you'll delete it, install again with Setup and tick \"Copy the game files\"."), was);
    const TASKDIALOG_BUTTON b[] = {{DUMP_PICK, ES_EN(L"Elegir el dump", L"Pick the dump")}, {DUMP_CLOSE, ES_EN(L"Cerrar", L"Close")}};
    if (task(ES_EN(L"No encuentro tu copia del juego", L"Can't find your game dump"), content, b, 2, NULL, 0, NULL, 0,
             TD_WARNING_ICON) != DUMP_PICK)
        return 0;
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return 0;
    const COMDLG_FILTERSPEC spec[] = {{ES_EN(L"Juego de Switch (NSP, XCI)", L"Switch game (NSP, XCI)"), L"*.nsp;*.xci"}};
    IFileOpenDialog_SetFileTypes(d, 1, spec);
    FILEOPENDIALOGOPTIONS o;
    if (SUCCEEDED(IFileOpenDialog_GetOptions(d, &o))) IFileOpenDialog_SetOptions(d, o | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, ES_EN(L"Tu copia del juego (la misma que leyó Setup)", L"Your game dump (the one Setup read)"));
    if (SUCCEEDED(IFileOpenDialog_Show(d, NULL))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) { swprintf(out, n, L"%ls", p); CoTaskMemFree(p); ok = 1; }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
    return ok;
}
static int (*g_pick_dump)(const wchar_t *was, wchar_t *out, size_t n) = pick_dump;   /* (test_launcher.c answers instead) */
static const wchar_t *g_setup_key = UPD_SETUP_KEY;   /* (test_launcher.c: a key that is not there) */

/* 1: the dump romfs-source.txt `src` names is there at its size, or the player picked it again; 0: not (said) */
static int dump_source_ready(const wchar_t *src) {
    DumpSource s;
    if (!read_dump_source(src, &s)) {
        message(MB_ICONWARNING, ES_EN(L"El archivo que dice dónde están los datos del juego está dañado:\n%ls\n\nVuelve a instalar con Setup.",
                                      L"The file that says where the game data is, is damaged:\n%ls\n\nInstall again with Setup."), src);
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(s.dump, GetFileExInfoStandard, &fa) &&
        ((unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) == s.dump_size)
        return 1;
    say(L"game dump %ls: not there at its size", s.dump);
    for (int tries = 0; tries < 5; tries++) {
        wchar_t picked[MAX_PATH] = L"";
        if (!g_pick_dump(s.dump, picked, MAX_PATH)) break;
        if (!dump_matches(picked, &s)) {
            message(MB_ICONWARNING, ES_EN(L"Ese archivo no es la copia del juego que leyó Setup:\n%ls",
                                          L"That file is not the game dump Setup read:\n%ls"), picked);
            continue;
        }
        if (!rewrite_dump_source(src, picked)) {
            message(MB_ICONWARNING, ES_EN(L"No pude guardar la nueva ubicación en:\n%ls", L"Couldn't save the new location in:\n%ls"), src);
            return 0;
        }
        HKEY k;   /* (Setup's own Dump value: its next run finds the dump there too) */
        if (RegOpenKeyExW(HKEY_CURRENT_USER, g_setup_key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"Dump", 0, REG_SZ, (const BYTE *)picked, (DWORD)((wcslen(picked) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
        say(L"game dump picked again: %ls", picked);
        return 1;
    }
    say(L"game dump: not picked again");
    return 0;
}

/* settings=<file>: the build's defaults, copied where the game reads settings.ini (next to userdata, pc_settings.cpp)
 * when the player has none yet; the player's own file is never overwritten. defs_out (MAX_PATH, or NULL): the defaults
 * file, "" when the build has none. */
static void settings_defaults(const wchar_t *pkg, wchar_t *defs_out) {
    wchar_t defs[MAX_PATH], dst[MAX_PATH];
    package_value(pkg, L"settings", 1, defs, MAX_PATH);
    if (!defs[0] || !exists(defs)) defs[0] = 0;
    if (defs_out) wcscpy(defs_out, defs);
    swprintf(dst, MAX_PATH, L"%ls\\settings.ini", g_base);
    if (defs[0] && !exists(dst))
        say(L"settings: %ls -> %ls (%ls)", defs, dst, CopyFileW(defs, dst, TRUE) ? L"copied" : L"copy failed");
}

/* The settings before play (preplay.c) on the game's own settings.ini, the build's defaults first when there is none
 * yet. note: why it opened (NULL: the player asked). 1: Play. */
static int preplay(const wchar_t *pkg, const wchar_t *note) {
    wchar_t defs[MAX_PATH], ini[MAX_PATH], title[128];
    settings_defaults(pkg, defs);
    swprintf(ini, MAX_PATH, L"%ls\\settings.ini", g_base);
    swprintf(title, 128, L"%ls - %ls", TITLE, ES_EN(L"Ajustes", L"Settings"));
    int play = preplay_dialog(ini, defs[0] ? defs : NULL, g_es ? TR_ES : g_tr, title, note);
    say(L"settings before play: %ls", play ? L"play" : L"closed");
    return play;
}

static int run_package(const wchar_t *pkg, const wchar_t *name, wchar_t *why, size_t whyn, HANDLE *game, FILETIME *started) {
    wchar_t exe[MAX_PATH], info[MAX_PATH], img[MAX_PATH], romfs[MAX_PATH], overlay[MAX_PATH], main_bin[MAX_PATH];
    swprintf(exe, MAX_PATH, L"%ls\\totk.exe", pkg);
    swprintf(info, MAX_PATH, L"%ls\\player.txt", pkg);
    package_value(pkg, L"img", 1, img, MAX_PATH);
    swprintf(main_bin, MAX_PATH, L"%ls\\main.bin", img);
    if (!exists(exe) || !exists(info) || !img[0] || !exists(main_bin)) {
        swprintf(why, whyn, ES_EN(L"%ls: incompleto (falta totk.exe, player.txt o img\\main.bin)",
                                  L"%ls: incomplete (totk.exe, player.txt or img\\main.bin is missing)"), name);
        say(L"%ls", why);
        return MISSING;
    }
    package_value(pkg, L"romfs", 1, romfs, MAX_PATH);
    package_value(pkg, L"overlay", 1, overlay, MAX_PATH);
    {   /* the build's processor level: package.py's cpu= (the key's --march), none = x86-64-v3 */
        wchar_t cpu[32], miss[256];
        package_value(pkg, L"cpu", 0, cpu, 32);
        int v2 = !wcscmp(cpu, L"x86-64-v2");
        cpu_missing(miss, 256, v2 ? 2 : 3);
        if (miss[0]) {
            swprintf(why, whyn, ES_EN(L"%ls: esta versión del juego se creó para procesadores con AVX2, y a este le falta %ls. "
                                      L"Vuelve a crearla con el Builder en este PC: elige sola la versión para procesadores sin AVX2.",
                                      L"%ls: this build of the game was made for processors with AVX2, and this one lacks %ls. "
                                      L"Make it again with the Builder on this PC: it picks the build for processors without AVX2 "
                                      L"by itself."), name, miss);
            say(L"%ls", why);
            return MISSING;
        }
        say(L"%ls: cpu level %ls", name, v2 ? L"x86-64-v2" : L"x86-64-v3");
    }

    wchar_t userdata[MAX_PATH], run[MAX_PATH], crashes[MAX_PATH], logs[MAX_PATH], logp[MAX_PATH];
    wchar_t ver[32], tag[32] = L"";   /* (another game version: its saves and mods folders, -v<tag>) */
    swprintf(userdata, MAX_PATH, L"%ls\\userdata", g_base);
    {   /* another game version (package.py's version=, e.g. 1.4.3; none = 1.0.0, the folder as before) keeps its own
           saves in userdata-v<digits>: a save that version writes does not load in 1.0.0. On its first start it gets a
           copy of the 1.0.0 saves (the game upgrades them as it loads them); the 1.0.0 folder is never touched. */
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        package_value(pkg, L"version", 0, ver, 32);
        if (ver[0] && wcscmp(ver, L"1.0.0")) {
            int n = 0;
            for (const wchar_t *c = ver; *c && n < 30; c++) if (*c >= L'0' && *c <= L'9') tag[n++] = *c;
            tag[n] = 0;
            swprintf(userdata, MAX_PATH, L"%ls\\userdata-v%ls", g_base, tag);
            swprintf(src, MAX_PATH, L"%ls\\userdata\\save", g_base);
            swprintf(dst, MAX_PATH, L"%ls\\save", userdata);
            if (!exists(dst) && exists(src)) {
                mkdirs(userdata);
                int rc = saves_copy_tree(src, dst);
                say(L"%ls: version %ls, its own saves in %ls; the 1.0.0 saves copied there (%ls)", name, ver, userdata,
                    rc ? L"FAILED" : L"ok");
                if (!rc) {   /* (the beta notice mentions the copy only when it happened) */
                    wchar_t mk[MAX_PATH]; swprintf(mk, MAX_PATH, L"%ls\\saves-copied-from-1.0.0", userdata);
                    FILE *cf = _wfopen(mk, L"w"); if (cf) fclose(cf);
                }
            } else {
                say(L"%ls: version %ls, its own saves in %ls", name, ver, userdata);
            }
            wchar_t beta[8], seen[MAX_PATH];
            package_value(pkg, L"beta", 0, beta, 8);
            swprintf(seen, MAX_PATH, L"%ls\\beta-notice-shown", userdata);
            if (!wcscmp(beta, L"1") && !exists(seen)) {   /* once per version folder: what this beta lacks */
                wchar_t mk[MAX_PATH], copied[512] = L"";
                swprintf(mk, MAX_PATH, L"%ls\\saves-copied-from-1.0.0", userdata);
                if (exists(mk))   /* (only when the 1.0.0 saves were really copied)*/
                    swprintf(copied, 512, ES_EN(L" Tus partidas de 1.0.0 se han copiado una vez a esta versión: lo que "
                                                L"juegues después en 1.0.0 no aparecerá aquí, y las partidas de %ls no "
                                                L"vuelven a 1.0.0.",
                                                L" Your 1.0.0 saves were copied to this version once: what you play later "
                                                L"in 1.0.0 will not show up here, and %ls saves do not go back to 1.0.0."), ver);
                message(MB_ICONINFORMATION, ES_EN(
                    /* (only gated claims, LEAD 11-04: 'runs at full frame rate' verified in the 9.1 gate's v143 line
                       (integration 11-04 15:27), with Setup's BetaNote. The controls: 9.1 runs the placed
                       player-state groups by default on 1.4.3 and fps360's 1.4.3 pins; the ability keys' game-data
                       calls have no 1.4.3 match, so 4-9 are dropped there. The menu mouse cursor needs ui fields
                       1.4.3's map does not place yet, so it is off there: menus by keyboard or pad)*/
                    L"TotK %ls (beta). Esta versión va a la frecuencia de fotogramas completa, pero está menos probada "
                    L"que la 1.0.0. "
                    L"Los controles de teclado y ratón funcionan como en la 1.0.0 durante el juego, salvo las teclas "
                    L"de poderes 4 a 9 (elige los poderes en la rueda de poderes). En los menús, usa el teclado o un "
                    L"mando: el puntero del ratón aún no funciona en los menús de la 1.4.3.%ls",
                    L"TotK %ls (beta). This version runs at full frame rate, but it is less tested than 1.0.0. The "
                    L"keyboard and mouse controls work as on 1.0.0 in play, except the ability keys 4 to 9 (pick "
                    L"abilities from the ability wheel instead). In menus, use the keyboard or a controller: the mouse "
                    L"pointer does not work in 1.4.3's menus yet.%ls"), ver, copied);
                mkdirs(userdata);
                FILE *mf = _wfopen(seen, L"w");
                if (mf) fclose(mf);
            }
        }
    }
    swprintf(run, MAX_PATH, L"%ls\\run", g_local);
    swprintf(crashes, MAX_PATH, L"%ls\\crashes", g_local);
    swprintf(logs, MAX_PATH, L"%ls\\logs", g_local);
    mkdirs(userdata); mkdirs(run); mkdirs(crashes); mkdirs(logs);
    SetEnvironmentVariableW(L"TOTK_USERDATA", userdata);
    SetEnvironmentVariableW(L"TOTK_IMG", img);
    SetEnvironmentVariableW(L"TOTK_ROMFS", romfs[0] ? romfs : NULL);
    SetEnvironmentVariableW(L"TOTK_OVERLAY", overlay[0] ? overlay : NULL);
    SetEnvironmentVariableW(L"TOTK_PRIORITY", L"normal");
    SetEnvironmentVariableW(L"TOTK_CRASH_DIR", crashes);
    mods_env(ver, tag);
    amiibo_env();
    swprintf(g_restart_file, MAX_PATH, L"%ls\\restart.env", g_local);   /* (the game's restart request: RESTART_CODE) */
    DeleteFileW(g_restart_file);
    SetEnvironmentVariableW(L"NXR_LAUNCHER_RESTART", g_restart_file);
    package_env(pkg);
    {   /* cache-seed\ next to totk.exe (package.py: the precompiled programs and pipelines). The engine only looks in
           its working folder (exec.c pc_seed_dir), which is run\, so the path goes in TOTK_PRECOMPILE_SEED. After
           player.txt: a TOTK_PRECOMPILE_SEED it already set is kept, empty included (env.TOTK_PRECOMPILE_SEED=: no
           seed, the engine's own off value; the seed-only opt-out). The shell's value never gets here
           (clean_environment).*/
        wchar_t seed[MAX_PATH], bin[MAX_PATH], have[8];
        swprintf(seed, MAX_PATH, L"%ls\\cache-seed", pkg);
        swprintf(bin, MAX_PATH, L"%ls\\precompile.bin", seed);
        SetLastError(0);
        const BOOL set = GetEnvironmentVariableW(L"TOTK_PRECOMPILE_SEED", have, 8) || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (set) say(L"cache-seed: TOTK_PRECOMPILE_SEED from player.txt kept");
        else if (exists(bin)) SetEnvironmentVariableW(L"TOTK_PRECOMPILE_SEED", seed);
    }
    settings_defaults(pkg, NULL);

    prune_files(logs, L"totk-*.log", KEEP_GAME_LOGS - 1);
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(logp, MAX_PATH, L"%ls\\totk-%04u%02u%02u-%02u%02u%02u.log", logs, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    swprintf(g_game_log, MAX_PATH, L"%ls", logp);
    HANDLE log = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    wchar_t cmd[MAX_PATH * 2];
    swprintf(cmd, MAX_PATH * 2, L"\"%ls\"", exe);
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = log;
    }
    PROCESS_INFORMATION pi;
    GetSystemTimeAsFileTime(started);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, run, &si, &pi);
    DWORD err = GetLastError();
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        swprintf(why, whyn, ES_EN(L"%ls: no se pudo iniciar (error %lu)", L"%ls: couldn't start (error %lu)"), name, err);
        say(L"%ls", why);
        return FAILED;
    }
    /* (the game's window comes to the front as it opens, rival v0.3.7 parity PARITY_037.md #27: the launcher, started by
       the player, may set the foreground and hands that on; shim_win.c asks for it at the window's first show. Windows
       refuses it once the player has used another window meanwhile, so a player who switched away is left there) */
    AllowSetForegroundWindow(pi.dwProcessId);
    say(L"started %ls (pid %lu), img %ls, romfs %ls, log %ls", name, pi.dwProcessId, img, romfs[0] ? romfs : L"(game default)", logp);
    DWORD w = WaitForSingleObject(pi.hProcess, g_early_ms), code = 0;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    if (w != WAIT_OBJECT_0) { say(L"%ls running after %.1f s", name, g_early_ms / 1000.0); *game = pi.hProcess; return RUNNING; }
    CloseHandle(pi.hProcess);
    /* (a restart this early is no failed start: the in-game skip at the title, F10's restart, a restart after a
       restart)*/
    if (code == RESTART_CODE) { say(L"%ls asked for a restart within %.1f s", name, g_early_ms / 1000.0); return RESTART; }
    if (code == 0) { say(L"%ls quit normally within %.1f s", name, g_early_ms / 1000.0); return QUIT; }
    if (code == GPU_UNSUPPORTED_CODE) { say(L"%ls: this GPU cannot run the game (the game showed why)", name); return QUIT; }
    if (code == DUMP_LOST_CODE) { say(L"%ls: the game dump could not be read (the game showed why)", name); return QUIT; }
    wchar_t rep[MAX_PATH];   /* a crash this early still writes its report (pc_crash.cpp): point at it, as after a late crash */
    crash_report_since(*started, rep, MAX_PATH);
    if (code == 0xC000001Du)   /* STATUS_ILLEGAL_INSTRUCTION: an instruction this CPU lacks that cpu_missing does not check */
        swprintf(why, whyn, ES_EN(L"%ls: el juego usó una instrucción que este procesador no tiene (código 0xC000001D)\nRegistro: %ls",
                                  L"%ls: the game used an instruction this processor doesn't have (code 0xC000001D)\nLog: %ls"), name, logp);
    else if (rep[0]) swprintf(why, whyn, ES_EN(L"%ls: el juego se cerró al arrancar (código 0x%08lx)\nInforme del fallo: %ls",
                                               L"%ls: the game closed while starting (code 0x%08lx)\nCrash report: %ls"), name, code, rep);
    else swprintf(why, whyn, ES_EN(L"%ls: el juego se cerró al arrancar (código 0x%08lx)\nRegistro: %ls",
                                   L"%ls: the game closed while starting (code 0x%08lx)\nLog: %ls"), name, code, logp);
    say(L"%ls", why);
    return FAILED;
}

/* run_package, then follow the game: its restarts (RESTART_CODE, early or late: the same package again, watched; at
 * most RESTART_MAX within RESTART_WINDOW_MS, then a message: a setting that restarts the game at every start would
 * loop) and the error dialog of a late error exit. RUNNING: it ran and ended; else run_package's result. */
#define RESTART_MAX 5
#define RESTART_WINDOW_MS 60000ull
static int play_package(const wchar_t *pkg, const wchar_t *name, wchar_t *why, size_t whyn) {
    HANDLE game = NULL;
    FILETIME started;
    ULONGLONG times[RESTART_MAX];
    int nt = 0;
    int r = run_package(pkg, name, why, whyn, &game, &started);
    for (;;) {
        if (r == RUNNING) {   /* stay (no window, no CPU) until the game exits */
            WaitForSingleObject(game, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(game, &code);
            CloseHandle(game);
            say(L"%ls exited (code 0x%08lx)", name, code);
            if (code != RESTART_CODE) {
                if (code && code != GPU_UNSUPPORTED_CODE && code != DUMP_LOST_CODE) {
                    wchar_t rep[MAX_PATH], logs[MAX_PATH];
                    crash_report_since(started, rep, MAX_PATH);
                    swprintf(logs, MAX_PATH, L"%ls\\logs", g_local);
                    if (rep[0]) message_open(MB_ICONWARNING, rep, ES_EN(L"El juego se cerró por un error.\n\nInforme del fallo:\n%ls",
                                                                        L"The game closed because of an error.\n\nCrash report:\n%ls"), rep);
                    else message_open(MB_ICONWARNING, g_game_log, ES_EN(L"El juego se cerró con un error (código 0x%08lx).\n\nRegistros: %ls",
                                                                        L"The game closed with an error (code 0x%08lx).\n\nLogs: %ls"), code, logs);
                }
                return RUNNING;
            }
            r = RESTART;
        }
        if (r != RESTART) return r;
        ULONGLONG now = GetTickCount64();
        if (nt == RESTART_MAX && now - times[0] < RESTART_WINDOW_MS) {
            say(L"%ls restarted %d times within %llu s: not again", name, RESTART_MAX, RESTART_WINDOW_MS / 1000);
            message(MB_ICONWARNING, ES_EN(L"El juego se ha reiniciado %d veces en menos de un minuto; no se vuelve a abrir.\n\n"
                                          L"Ábrelo de nuevo desde el lanzador.",
                                          L"The game restarted %d times in under a minute; it is not opened again.\n\n"
                                          L"Start it again from the launcher."), RESTART_MAX);
            return QUIT;
        }
        if (nt == RESTART_MAX) { memmove(times, times + 1, sizeof times[0] * (RESTART_MAX - 1)); nt--; }
        times[nt++] = now;
        restart_env_apply();
        say(L"%ls asked for a restart: starting it again", name);
        r = run_package(pkg, name, why, whyn, &game, &started);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)inst; (void)prev; (void)show;
    if (!init_paths()) return 1;
    say(L"launcher in %ls, data %ls, local %ls", g_dir, g_base, g_local);
    lang_init();   /* before any dialog; the first start has no settings.ini yet (run_package copies it): Windows decides */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (already_running()) {
        message(MB_ICONINFORMATION, ES_EN(L"El juego ya está abierto.", L"The game is already running."));
        return 0;
    }
    {
        wchar_t miss[256];
        cpu_missing(miss, 256, 2);   /* the floor of any build (x86-64-v2); a package's own level: run_package */
        if (miss[0]) {
            message(MB_ICONERROR, ES_EN(L"Este procesador no puede ejecutar el juego: le faltan instrucciones que necesita (%ls).\n\n"
                                        L"Hace falta al menos SSE4.2 y POPCNT: Intel Core de 1.ª generación (2008) o posterior, "
                                        L"o AMD FX o Ryzen.",
                                        L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\n"
                                        L"It needs at least SSE4.2 and POPCNT: a 1st-generation Intel Core (2008) or later, "
                                        L"or an AMD FX or Ryzen."), miss);
            return 1;
        }
        cpu_missing(miss, 256, 3);
        say(L"cpu: x86-64-v2 ok, x86-64-v3 %ls%ls, %lu logical processors", miss[0] ? L"missing " : L"ok", miss,
            GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    }
    {   /* the Vulkan loader (System32\vulkan-1.dll, installed by every GPU driver with Vulkan): totk.exe links it, so
           without it Windows would refuse the game with its own "DLL not found" box. What the GPU itself can do is the
           game's check (device.c gpu_unsupported). */
        HMODULE vk = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!vk) {
            message(MB_ICONERROR, ES_EN(L"No hay ningún controlador Vulkan instalado (falta vulkan-1.dll), y el juego lo necesita.\n\n"
                                        L"Instala el controlador más reciente de tu tarjeta gráfica desde la web de NVIDIA, AMD o "
                                        L"Intel. Hace falta una GPU con Vulkan 1.3: NVIDIA GTX 900 o posterior, AMD RX 400 o posterior, "
                                        L"Intel Arc o Iris Xe.",
                                        L"No Vulkan driver is installed (vulkan-1.dll is missing), and the game needs one.\n\n"
                                        L"Install the latest driver for your graphics card from the NVIDIA, AMD or Intel website. "
                                        L"A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or later, AMD RX 400 or later, Intel Arc or Iris Xe."));
            return 1;
        }
        FreeLibrary(vk);
        say(L"vulkan: loader found");
    }
    install_dropped();   /* (a mod dropped on the launcher: installed, said, then the game starts) */
    const int play = args && wcsstr(args, L"--play");   /* (tools: neither window below, whatever key is held) */
    int menu = !play && ((args && wcsstr(args, L"--saves")) || (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    /* Ctrl or --settings: the settings before play (preplay.c), for a game that closes or crawls before F10. --play:
       never (the tools that start the launcher headless: Ctrl is the desktop's global state, and anyone holding it at
       that moment opened a window no one would close; canary_release 11-03 judged a good v0.1.9 package 'bad') */
    int pre = !play &&
              ((args && wcsstr(args, L"--settings")) || (GetAsyncKeyState(VK_CONTROL) & 0x8000));
    if (menu) say(L"update check: skipped (saves menu)");   /* the player asked for this folder's menu */
    else if (auto_update()) return 0;                        /* the updated build's launcher does the rest */
    else if (offer_newer_release()) return 0;                /* its launcher does the rest (backups, import offer) */
    saves_recover(g_base);   /* an import cut short (power loss) leaves a complete save in place */
    if (menu) { if (!saves_menu()) return 0; }
    else offer_import();
    vp9_notice();

    /* builds: packages\<current>, packages\<lastgood>; or totk.exe in the launcher's own folder */
    wchar_t names[2][MAX_PATH], pkgs[2][MAX_PATH];
    read_name(L"current.txt", names[0], MAX_PATH);
    read_name(L"lastgood.txt", names[1], MAX_PATH);
    int count = 0;
    for (int i = 0; i < 2; i++)
        if (names[i][0] && (i == 0 || _wcsicmp(names[i], names[0]))) {
            if (i != count) wcscpy(names[count], names[i]);
            swprintf(pkgs[count], MAX_PATH, L"%ls\\packages\\%ls", g_dir, names[count]);
            count++;
        }
    if (!count) {
        wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%ls\\totk.exe", g_dir);
        if (exists(exe)) { wcscpy(names[0], ES_EN(L"(carpeta del lanzador)", L"(launcher folder)")); wcscpy(pkgs[0], g_dir); count = 1; }
    }
    if (!count) {
        message(MB_ICONINFORMATION,
                ES_EN(L"No encuentro el juego: ni packages\\ con current.txt ni totk.exe junto al lanzador.\n\n%ls",
                      L"Can't find the game: there's no packages\\ with current.txt and no totk.exe next to the launcher.\n\n%ls"), g_dir);
        return 0;
    }

    if (pre && !preplay(pkgs[0], NULL)) return 0;
    note_package_change(pkgs[0]);   /* a newer package: a named copy of the save before its first start */
    wchar_t bk[MAX_PATH];
    if (saves_backup(g_base, L"auto", bk, MAX_PATH) > 0) say(L"backup %ls", bk);
    saves_prune(g_base, KEEP_AUTO_BACKUPS);

    clean_environment();
    wchar_t why[2][768] = {L"", L""};
again:
    for (int i = 0; i < count; i++) {
        wchar_t romfs[MAX_PATH];
        package_value(pkgs[i], L"romfs", 1, romfs, MAX_PATH);
        if (romfs[0] && !exists(romfs)) {   /* shared by every build: no fallback helps */
            message(MB_ICONWARNING, ES_EN(L"No encuentro los datos del juego (RomFS extraída) en:\n%ls",
                                          L"Can't find the game data (extracted RomFS) in:\n%ls"), romfs);
            return 1;
        }
        /* a Builder install keeps the data only in romfs.bin (TOTK_ROMFS_PACK; the romfs= folder is an empty mount
           root): gone, nothing serves the game's files */
        wchar_t pack[MAX_PATH];
        package_value(pkgs[i], L"env.TOTK_ROMFS_PACK", 1, pack, MAX_PATH);
        if (pack[0] && !exists(pack) && !dir_has_files(romfs)) {
            message(MB_ICONWARNING, ES_EN(L"No encuentro los datos del juego en:\n%ls\n\nVuelve a instalar con Setup.",
                                          L"Can't find the game data in:\n%ls\n\nInstall again with Setup."), pack);
            return 1;
        }
        /* since v0.1.8 the data stays in the player's dump (romfs-source.txt): it has to be where Setup read it */
        if (pack[0] && is_dump_source(pack) && exists(pack) && !dump_source_ready(pack)) return 1;
        int r = play_package(pkgs[i], names[i], why[i], 768);
        if (r == RUNNING || r == QUIT) { maybe_update(pkgs[0]); return 0; }
        if (i + 1 < count) say(L"falling back to %ls", names[i + 1]);
    }
    wchar_t logp[MAX_PATH]; swprintf(logp, MAX_PATH, L"%ls\\launcher.log", g_local);
    if (!g_messages_logged_only) {   /* lower settings may get it going: the settings before play, then another try */
        wchar_t content[1600];
        swprintf(content, 1600, L"%ls\n%ls", why[0], count > 1 ? why[1] : L"");
        const TASKDIALOG_BUTTON b[] = {
            {401, ES_EN(L"Cambiar ajustes y volver a intentarlo\nModo seguro, ventana, escala de resolución, fotogramas...",
                        L"Change settings and try again\nSafe mode, window, resolution scale, frame rate...")},
            {402, ES_EN(L"Ver el registro", L"Show the log")},
            {403, ES_EN(L"Comunidad\nPide ayuda en r/WeepingOfTheEmpire (Reddit)",
                        L"Community\nAsk for help on r/WeepingOfTheEmpire (Reddit)")}};
        int r;
        while ((r = task(ES_EN(L"No se pudo arrancar el juego", L"Couldn't start the game"), content, b, 3, NULL, 0, NULL,
                         TDCBF_CLOSE_BUTTON, TD_WARNING_ICON)) == 403)
            open_community();   /* (the box stays: the log and the settings are still to hand) */
        if (r == 401 && preplay(pkgs[0], ES_EN(L"El juego se cerró al arrancar. El modo seguro, una escala de resolución "
                                               L"más baja o jugar en ventana pueden ayudar.",
                                               L"The game closed while starting. Safe mode, a lower resolution scale or a "
                                               L"window may help."))) {
            why[0][0] = why[1][0] = 0;
            say(L"settings changed after a failed start: trying again");
            goto again;
        }
        if (r == 402) open_file(g_game_log[0] && exists(g_game_log) ? g_game_log : logp);
        maybe_update(pkgs[0]);
        return 1;
    }
    maybe_update(pkgs[0]);   /* a newer good commit may start where these failed */
    /* the game's own log when a game ran (an early crash: why[] names it), else the launcher's */
    message_open(MB_ICONWARNING, g_game_log[0] && exists(g_game_log) ? g_game_log : logp,
                 ES_EN(L"No se pudo arrancar el juego.\n\n%ls\n%ls\n\nRegistro del lanzador: %ls",
                       L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls"),
                 why[0], count > 1 ? why[1] : L"", logp);
    return 1;
}
