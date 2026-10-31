/* "Zelda TotK (PC)" launcher for the engine (totk-recomp, engine/). Upstream's launcher/launcher.cpp is a Steam Deck setup app
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
 * is skipped for the next one. Messages are in Spanish or English, as the game's language (lang_init).
 *
 * The game gets a clean environment (no TOTK_ / LARECOMP_ / NX* variables from a test shell) plus:
 *     TOTK_USERDATA = %APPDATA%\nxrecomp\TotK-engine\userdata    (save\ inside; the settings file sits next to it)
 *     TOTK_IMG, TOTK_ROMFS, TOTK_OVERLAY from player.txt;  TOTK_PRIORITY=normal (the game lowers itself to idle else)
 *     TOTK_CRASH_DIR = %LOCALAPPDATA%\nxrecomp\TotK-engine\crashes
 *     TOTK_USER_MODS = the RomFS mods in %APPDATA%\nxrecomp\TotK-engine\mods (mods_env)
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
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <cpuid.h>

#include "saves.h"

enum { EARLY_MS = 20000, LOG_MAX = 256 * 1024, KEEP_AUTO_BACKUPS = 10, KEEP_GAME_LOGS = 8, MAX_CANDIDATES = 16 };
static DWORD g_early_ms = EARLY_MS;   /* (test_launcher.c shortens it) */
static const wchar_t TITLE[] = L"Zelda TotK (PC)";

static wchar_t g_dir[MAX_PATH];    /* the launcher's folder */
static wchar_t g_exe[MAX_PATH];    /* the launcher's own file name ("TotK Launcher.exe") */
static wchar_t g_base[MAX_PATH];   /* %APPDATA%\nxrecomp\TotK-engine: userdata, backups */
static wchar_t g_local[MAX_PATH];  /* %LOCALAPPDATA%\nxrecomp\TotK-engine: run, logs, crashes, launcher.log */
static FILE *g_log;
static int g_es = 1;   /* the player's texts in Spanish (else English): lang_init (Spanish until then: test_launcher.c) */
#define ES_EN(es, en) (g_es ? (es) : (en))

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
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
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
    g_log = _wfopen(p, big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8");
    return 1;
}

/* The player's language, as the game decides it (pc_lang.cpp): game.language in <g_base>\settings.ini when set (Spanish
 * for "es" and "es-419", English for any other), else Windows' display language. The launcher only speaks those two.
 * Before the first start there is no settings.ini yet (run_package copies the build's defaults later), so Windows
 * decides, as it does for the game whose default is "automatic". */
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
        say(L"language: %ls (game.language = %ls)", g_es ? L"Spanish" : L"English", value);
    } else {
        LANGID ui = GetUserDefaultUILanguage();
        g_es = PRIMARYLANGID(ui) == LANG_SPANISH;
        say(L"language: %ls (Windows' display language 0x%04x%ls)", g_es ? L"Spanish" : L"English", (unsigned)ui,
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

/* Backs up the current save (why), then makes src the save. */
static void replace_with(const wchar_t *src, const wchar_t *why, const wchar_t *what) {
    wchar_t bk[MAX_PATH] = L"";
    if (saves_backup(g_base, why, bk, MAX_PATH) < 0) {
        message(MB_ICONWARNING, ES_EN(L"No se pudo hacer la copia de la partida actual; no se ha cambiado nada.",
                                      L"Couldn't back up the current save; nothing was changed."));
        return;
    }
    int r = saves_replace(g_base, src);
    say(L"%ls from %ls: %ls (backup %ls)", what, src, r ? L"failed" : L"done", bk[0] ? bk : L"none: no save");
    if (r) message(MB_ICONWARNING, ES_EN(L"No se pudo copiar la partida (paso %d). La partida que había sigue igual.\n\n%ls",
                                         L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls"), r, src);
    else message(MB_ICONINFORMATION, ES_EN(L"Hecho.%ls%ls", L"Done.%ls%ls"),
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
            swprintf(labels[i], MAX_PATH + 160, L"%ls%ls · %d %ls · %ls\n%ls", source, user, c[i].slots,
                     c[i].slots == 1 ? ES_EN(L"ranura", L"slot") : ES_EN(L"ranuras", L"slots"), t, c[i].path);
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
                     ES_EN(L"El juego es la versión 1.0.0: una partida guardada con una versión posterior puede no cargar.",
                           L"The game is version 1.0.0: a save made with a later version may not load."),
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
                            : !wcscmp(why, L"before-update") ? ES_EN(L"antes de actualizar", L"before update") : why;
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
            {101, ES_EN(L"Importar una partida…\nDe yuzu, Eden, Ryujinx, la versión anterior de nxrecomp o una carpeta (JKSV).",
                        L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV).")},
            {102, ES_EN(L"Restaurar una copia…", L"Restore a backup…")},
            {103, ES_EN(L"Hacer una copia ahora", L"Make a backup now")},
        };
        int b = task(ES_EN(L"Partidas", L"Saves"), content, buttons, 4, NULL, 0, NULL, TDCBF_CLOSE_BUTTON, NULL);
        if (b == 100) return 1;
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

/* ---------------------------------------------------------------- the CPU */

/* totk.exe is built for x86-64-v3 (mkninja_win.py -march=x86-64-v3): on a CPU without one of these it dies on its first
   such instruction (0xC000001D) with nothing to tell the player why. The launcher itself is baseline x86-64, so it can
   check first. Writes the missing ones to `out` ("" when the CPU has them all). */
static void cpu_missing(wchar_t *out, size_t n) {
    unsigned a, b, c, d, c1 = 0, b7 = 0, ce = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) c1 = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) b7 = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) ce = c;
    unsigned xcr0 = 0;
    if (c1 & 1u << 27) { unsigned hi; __asm__ volatile("xgetbv" : "=a"(xcr0), "=d"(hi) : "c"(0)); }   /* OSXSAVE */
    const struct { const wchar_t *name; int ok; } need[] = {
        {L"SSE4.2", !!(c1 & 1u << 20)}, {L"POPCNT", !!(c1 & 1u << 23)}, {L"AVX", !!(c1 & 1u << 28)},
        {ES_EN(L"AVX (activado por Windows)", L"AVX (enabled by Windows)"), (xcr0 & 6) == 6}, {L"AVX2", !!(b7 & 1u << 5)}, {L"FMA", !!(c1 & 1u << 12)},
        {L"BMI1", !!(b7 & 1u << 3)}, {L"BMI2", !!(b7 & 1u << 8)}, {L"F16C", !!(c1 & 1u << 29)},
        {L"LZCNT", !!(ce & 1u << 5)}, {L"MOVBE", !!(c1 & 1u << 22)},
    };
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++)
        if (!need[i].ok && len + 40 < n) len += swprintf(out + len, n - len, L"%ls%ls", len ? L", " : L"", need[i].name);
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
/* Mods: every folder in <g_base>\mods is a RomFS mod laid out as emulators take them (<name>\romfs\..., or the RomFS tree
 * itself). Their RomFS folders go to the game in TOTK_USER_MODS, sorted by name (hle_fs.cpp: replacement files only, no new
 * files and no RSTB merge; the first mod by name wins a file two of them replace; our sets, glyphs and overlay win). A name
 * starting with '_' or '.' is switched off. An ExeFS-only mod (exefs\ and no romfs\) is skipped: code patches are not
 * supported. The folder is per user like the save, so mods carry over to every new build. Returns the mods passed. */
enum { MAX_MODS = 64 };
/* A mod with its own resource size table (System\Resource\ResourceSizeTable*): hle_fs merges it into the port's, each
 * entry at the larger size; logged here so a player's log shows it.*/
static void rstb_note(const wchar_t *name, const wchar_t *romfs) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\System\\Resource\\ResourceSizeTable*", romfs);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FindClose(h);
    say(L"mods: %ls brings its own resource size table (%ls): the game merges it into the port's", name, fd.cFileName);
}
static int cmp_names(const void *a, const void *b) { return _wcsicmp((const wchar_t *)a, (const wchar_t *)b); }
static int mods_env(void) {
    static wchar_t names[MAX_MODS][MAX_PATH], value[32767];
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\mods", g_base);
    mkdirs(dir);
    swprintf(p, MAX_PATH, L"%ls\\LEEME-README.txt", dir);
    if (!exists(p)) {
        FILE *f = _wfopen(p, L"w, ccs=UTF-8");
        if (f) {
            fputws(L"Mods de RomFS: una carpeta por mod (<nombre>\\romfs\\... como en los emuladores). Gana el primero por nombre.\n"
                   L"Un nombre que empieza por _ lo desactiva. Solo se reemplazan archivos (sin archivos nuevos ni mods de exefs).\n\n"
                   L"RomFS mods: one folder per mod (<name>\\romfs\\... as emulators use). The first by name wins.\n"
                   L"A name starting with _ turns it off. Files are only replaced (no new files, no exefs mods).\n", f);
            fclose(f);
        }
    }
    int n = 0;
    swprintf(p, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
            if (n == MAX_MODS) { say(L"mods: more than %d folders, the rest ignored", MAX_MODS); break; }
            swprintf(names[n++], MAX_PATH, L"%ls", fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    size_t len = 0;
    int used = 0;
    value[0] = 0;
    for (int i = 0; i < n; i++) {
        wchar_t romfs[MAX_PATH], exefs[MAX_PATH], *use = romfs;
        swprintf(romfs, MAX_PATH, L"%ls\\%ls\\romfs", dir, names[i]);
        swprintf(exefs, MAX_PATH, L"%ls\\%ls\\exefs", dir, names[i]);
        if (!exists(romfs)) {
            if (exists(exefs)) { say(L"mods: %ls skipped (exefs only: code patches are not supported)", names[i]); continue; }
            swprintf(romfs, MAX_PATH, L"%ls\\%ls", dir, names[i]);   /* the folder is the RomFS tree itself */
        } else if (exists(exefs)) {
            say(L"mods: %ls: its exefs folder is not used (code patches are not supported), only its romfs", names[i]);
        }
        rstb_note(names[i], use);
        size_t k = wcslen(use);
        if (len + k + 2 > sizeof value / sizeof *value) { say(L"mods: list too long, %ls and later ignored", names[i]); break; }
        if (len) value[len++] = L';';
        wcscpy(value + len, use);
        len += k;
        used++;
        say(L"mods: %ls", use);
    }
    SetEnvironmentVariableW(L"TOTK_USER_MODS", used ? value : NULL);
    return used;
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

    wchar_t userdata[MAX_PATH], run[MAX_PATH], crashes[MAX_PATH], logs[MAX_PATH], logp[MAX_PATH];
    swprintf(userdata, MAX_PATH, L"%ls\\userdata", g_base);
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
    mods_env();
    swprintf(g_restart_file, MAX_PATH, L"%ls\\restart.env", g_local);   /* (the game's restart request: RESTART_CODE) */
    DeleteFileW(g_restart_file);
    SetEnvironmentVariableW(L"NXR_LAUNCHER_RESTART", g_restart_file);
    {   /* cache-seed\ next to totk.exe (package.py: the precompiled programs and pipelines). The engine only looks in
           its working folder (exec.c pc_seed_dir), which is run\, so the path goes in TOTK_PRECOMPILE_SEED. */
        wchar_t seed[MAX_PATH], bin[MAX_PATH];
        swprintf(seed, MAX_PATH, L"%ls\\cache-seed", pkg);
        swprintf(bin, MAX_PATH, L"%ls\\precompile.bin", seed);
        if (exists(bin)) SetEnvironmentVariableW(L"TOTK_PRECOMPILE_SEED", seed);
    }
    package_env(pkg);
    {   /* settings=<file>: the build's defaults, copied where the game reads settings.ini (next to userdata, pc_settings.cpp)
           when the player has none yet; the player's own file is never overwritten */
        wchar_t defs[MAX_PATH], dst[MAX_PATH];
        package_value(pkg, L"settings", 1, defs, MAX_PATH);
        swprintf(dst, MAX_PATH, L"%ls\\settings.ini", g_base);
        if (defs[0] && exists(defs) && !exists(dst))
            say(L"settings: %ls -> %ls (%ls)", defs, dst, CopyFileW(defs, dst, TRUE) ? L"copied" : L"copy failed");
    }

    prune_files(logs, L"totk-*.log", KEEP_GAME_LOGS - 1);
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(logp, MAX_PATH, L"%ls\\totk-%04u%02u%02u-%02u%02u%02u.log", logs, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
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
                if (code && code != GPU_UNSUPPORTED_CODE) {
                    wchar_t rep[MAX_PATH], logs[MAX_PATH];
                    crash_report_since(started, rep, MAX_PATH);
                    swprintf(logs, MAX_PATH, L"%ls\\logs", g_local);
                    if (rep[0]) message(MB_ICONWARNING, ES_EN(L"El juego se cerró por un error.\n\nInforme del fallo:\n%ls",
                                                              L"The game closed because of an error.\n\nCrash report:\n%ls"), rep);
                    else message(MB_ICONWARNING, ES_EN(L"El juego se cerró con un error (código 0x%08lx).\n\nRegistros: %ls",
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
        cpu_missing(miss, 256);
        if (miss[0]) {
            message(MB_ICONERROR, ES_EN(L"Este procesador no puede ejecutar el juego: le faltan instrucciones que necesita (%ls).\n\n"
                                        L"Hace falta un procesador con AVX2: Intel Core de 4.ª generación (Haswell, 2013) o posterior, "
                                        L"o AMD Ryzen o posterior.",
                                        L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\n"
                                        L"You need a processor with AVX2: a 4th-generation Intel Core (Haswell, 2013) or later, "
                                        L"or an AMD Ryzen or later."), miss);
            return 1;
        }
        say(L"cpu: x86-64-v3 ok, %lu logical processors", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
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
    int menu = (args && wcsstr(args, L"--saves")) || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (menu) say(L"update check: skipped (saves menu)");   /* the player asked for this folder's menu */
    else if (offer_newer_release()) return 0;                /* its launcher does the rest (backups, import offer) */
    saves_recover(g_base);   /* an import cut short (power loss) leaves a complete save in place */
    if (menu) { if (!saves_menu()) return 0; }
    else offer_import();

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

    note_package_change(pkgs[0]);   /* a newer package: a named copy of the save before its first start */
    wchar_t bk[MAX_PATH];
    if (saves_backup(g_base, L"auto", bk, MAX_PATH) > 0) say(L"backup %ls", bk);
    saves_prune(g_base, KEEP_AUTO_BACKUPS);

    clean_environment();
    wchar_t why[2][768] = {L"", L""};
    for (int i = 0; i < count; i++) {
        wchar_t romfs[MAX_PATH];
        package_value(pkgs[i], L"romfs", 1, romfs, MAX_PATH);
        if (romfs[0] && !exists(romfs)) {   /* shared by every build: no fallback helps */
            message(MB_ICONWARNING, ES_EN(L"No encuentro los datos del juego (RomFS extraída) en:\n%ls",
                                          L"Can't find the game data (extracted RomFS) in:\n%ls"), romfs);
            return 1;
        }
        int r = play_package(pkgs[i], names[i], why[i], 768);
        if (r == RUNNING || r == QUIT) { maybe_update(pkgs[0]); return 0; }
        if (i + 1 < count) say(L"falling back to %ls", names[i + 1]);
    }
    maybe_update(pkgs[0]);   /* a newer good commit may start where these failed */
    wchar_t logp[MAX_PATH]; swprintf(logp, MAX_PATH, L"%ls\\launcher.log", g_local);
    message(MB_ICONWARNING, ES_EN(L"No se pudo arrancar el juego.\n\n%ls\n%ls\n\nRegistro del lanzador: %ls",
                                  L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls"),
            why[0], count > 1 ? why[1] : L"", logp);
    return 1;
}
