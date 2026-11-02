/* mods_install.h: install a mod the player picked (an archive or a folder) into the right mods folder, for the launcher
 * (a mod dropped on it) and the F10 Mods tab ("Install a mod..."). Header-only, valid C and C++: launcher.c and
 * pc_mods_ui.cpp include it, one copy each. Windows only.
 *
 * The mod is unpacked (archives through Windows' own tar.exe:.zip everywhere,.7z /.rar only where that tar reads
 * them; a folder is copied) into a staging folder "_installing-<tick>" inside the 1.0.0 mods folder (the game's scan
 * leaves '_' names out), one lone wrapper folder looked through, then read: a RomFS part (a romfs / romfslite folder,
 * or the game's own top folders: mods_scan.h), ExeFS patches (.ips /.pchtxt), cheats (<build id>.txt), code modules
 * (exefs subsdk* / main: refused, a recompiled game cannot load them), another game's contents\<title id>. The game
 * version it is for comes from its files: the build ids its patch / cheat files are named by (1.0.0 082CE09B06E33A12,
 * 1.4.2 5CB42B1CF25469FB, 1.4.3 277178B7DBA1B6D4), else the ".Product.<n>." tags of its RomFS files, else `fallback`
 * (the game in use). The version in use wins when the mod names it among others; a later version goes to the 1.4.3
 * build's folder (mods-v143), 1.0.0 to mods. Then the staging folder is moved there under the mod's name (" (2)"...
 * when taken). Nothing outside the two mods folders is written; a failure leaves no staging folder behind. Paths:
 * every one is an absolute \\?\ path built on the heap with its length checked (a deep RomFS tree under a long
 * profile path passes MAX_PATH): a path that would not fit stops the install (MI_IO), and nothing is ever called on a
 * cut-off path.*/
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

enum { MI_ROMFS = 1, MI_EXEFS = 2, MI_CHEATS = 4 };
enum { MI_OK = 0, MI_NOT_FOUND, MI_UNPACK, MI_NOT_A_MOD, MI_OTHER_GAME, MI_CODE_MODULES, MI_IO };

typedef struct {
    int kinds;                  /* MI_ROMFS | MI_EXEFS | MI_CHEATS */
    int version;                /* the version chosen: 100, 142, 143, another tag (e.g. 121), or the fallback */
    int named;                  /* the mod's files named its version (else the fallback was taken) */
    int to_v143;                /* put in the later build's folder */
    wchar_t name[128];          /* its folder name */
    wchar_t dest[MAX_PATH];     /* where it went (shown to the player; cut to MAX_PATH there only) */
    wchar_t other_title[20];    /* MI_OTHER_GAME: the title id it is for */
} ModInstall;

typedef struct {
    int kinds, code, other, toolong;
    int vers[16], nvers;        /* versions named (build ids, then tags) */
    wchar_t other_title[20];
} MiScan;

#define MI_LONG 32760           /* the longest \\?\ path Windows takes, a little under 32767 */

static int mi_ieq(const wchar_t *a, const wchar_t *b) { return _wcsicmp(a, b) == 0; }
/* a \ b on the heap; NULL when it would not fit (the caller stops) */
static wchar_t *mi_join(const wchar_t *a, const wchar_t *b) {
    const size_t la = wcslen(a), lb = wcslen(b);
    if (la + 1 + lb + 1 > MI_LONG) return NULL;
    wchar_t *p = (wchar_t *)malloc((la + lb + 2) * sizeof(wchar_t));
    if (!p) return NULL;
    memcpy(p, a, la * sizeof(wchar_t));
    p[la] = L'\\';
    memcpy(p + la + 1, b, lb * sizeof(wchar_t));
    p[la + 1 + lb] = 0;
    return p;
}
/* the absolute \\?\ form of `p` on the heap (a drive path or a \\server\share one), NULL on failure */
static wchar_t *mi_abs(const wchar_t *p) {
    if (!wcsncmp(p, L"\\\\?\\", 4)) return _wcsdup(p);
    DWORD n = GetFullPathNameW(p, 0, NULL, NULL);
    if (!n || n > MI_LONG - 8) return NULL;
    wchar_t *full = (wchar_t *)malloc((n + 1) * sizeof(wchar_t)), *out = (wchar_t *)malloc((n + 9) * sizeof(wchar_t));
    if (!full || !out || !GetFullPathNameW(p, n + 1, full, NULL)) { free(full); free(out); return NULL; }
    for (wchar_t *c = full; *c; c++) if (*c == L'/') *c = L'\\';
    size_t l = wcslen(full);
    while (l > 3 && full[l - 1] == L'\\') full[--l] = 0;
    if (full[0] == L'\\' && full[1] == L'\\') { wcscpy(out, L"\\\\?\\UNC"); wcscat(out, full + 1); }
    else { wcscpy(out, L"\\\\?\\"); wcscat(out, full); }
    free(full);
    return out;
}
/* the plain form of a \\?\ path (for tar.exe and the player's eyes), into out[n] */
static void mi_plain(const wchar_t *p, wchar_t *out, size_t n) {
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) _snwprintf(out, n, L"\\\\%ls", p + 8);
    else _snwprintf(out, n, L"%ls", !wcsncmp(p, L"\\\\?\\", 4) ? p + 4 : p);
    out[n - 1] = 0;   /* (display / tar only: never handed to a delete) */
}
static int mi_is_dir(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int mi_hex16(const wchar_t *s, size_t n) {   /* n hex digits, n >= 16 */
    if (n < 16) return 0;
    for (size_t i = 0; i < n; i++) if (!iswxdigit(s[i])) return 0;
    return 1;
}
static void mi_add_ver(MiScan *s, int v) {
    for (int i = 0; i < s->nvers; i++) if (s->vers[i] == v) return;
    if (s->nvers < 16) s->vers[s->nvers++] = v;
}
static int mi_bid_version(const wchar_t *stem) {   /* a build id's version, -1 unknown */
    if (!_wcsnicmp(stem, L"082CE09B06E33A12", 16)) return 100;
    if (!_wcsnicmp(stem, L"5CB42B1CF25469FB", 16)) return 142;
    if (!_wcsnicmp(stem, L"277178B7DBA1B6D4", 16)) return 143;
    return -1;
}
static int mi_romfs_top(const wchar_t *n) {   /* (mods_scan.h romfs_top) */
    static const wchar_t *const tops[] = {L"Actor", L"AI", L"AnimationEvent", L"AS", L"Bake", L"Banc", L"Cave", L"Effect",
        L"Event", L"Font", L"Game", L"GameParameter", L"LevelSensor", L"Lib", L"Logic", L"Mals", L"Model", L"Movie",
        L"Pack", L"Phive", L"Preset", L"RSDB", L"Sequence", L"Shader", L"Sound", L"System", L"TexToGo", L"UI"};
    for (size_t i = 0; i < sizeof tops / sizeof *tops; i++) if (mi_ieq(n, tops[i])) return 1;
    return 0;
}
static int mi_part(const wchar_t *n) {   /* a folder name that is a mod's part, never a wrapper */
    return mi_ieq(n, L"romfs") || mi_ieq(n, L"romfslite") || mi_ieq(n, L"exefs") || mi_ieq(n, L"exefs_patches") ||
           mi_ieq(n, L"cheats") || mi_ieq(n, L"atmosphere") || mi_ieq(n, L"contents") || mi_ieq(n, L"load") ||
           mi_hex16(n, wcslen(n)) || mi_romfs_top(n);
}
static int mi_dots(const wchar_t *n) { return !wcscmp(n, L".") || !wcscmp(n, L".."); }

/* where: 1 in a RomFS part, 2 in an exefs part, 3 in a cheats folder, 0 not known yet */
static void mi_walk(const wchar_t *dir, int depth, int where, int parent_contents, MiScan *s) {
    WIN32_FIND_DATAW fd;
    wchar_t *pat = mi_join(dir, L"*");
    if (depth > 64 || !pat) { s->toolong = 1; free(pat); return; }
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *n = fd.cFileName;
        if (mi_dots(n)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* (links: not followed) */
            int w = where;
            if (mi_ieq(n, L"romfs") || mi_ieq(n, L"romfslite")) w = 1;
            else if (mi_ieq(n, L"exefs") || mi_ieq(n, L"exefs_patches")) w = 2;
            else if (mi_ieq(n, L"cheats")) w = 3;
            else if (!where && depth == 0 && mi_romfs_top(n)) w = 1;   /* a bare RomFS tree */
            if (parent_contents && wcslen(n) == 16 && mi_hex16(n, 16) && _wcsnicmp(n, L"0100F2C0115B6", 13) != 0) {
                s->other = 1;   /* (TotK 0100F2C0115B6000, its update ...6800) */
                wcsncpy(s->other_title, n, 19);
                continue;
            }
            wchar_t *p = mi_join(dir, n);
            if (!p) { s->toolong = 1; continue; }
            mi_walk(p, depth + 1, w, mi_ieq(n, L"contents"), s);
            free(p);
            continue;
        }
        const wchar_t *dot = wcsrchr(n, L'.');
        const size_t stem = dot ? (size_t)(dot - n) : wcslen(n);
        if (where == 1) {
            s->kinds |= MI_ROMFS;
            const wchar_t *t = wcsstr(n, L".Product.");
            if (t) mi_add_ver(s, _wtoi(t + 9));
        }
        if (dot && (mi_ieq(dot, L".ips") || mi_ieq(dot, L".pchtxt"))) {
            s->kinds |= MI_EXEFS;
            if (mi_hex16(n, 16)) { int v = mi_bid_version(n); if (v > 0) mi_add_ver(s, v); }
        } else if (dot && mi_ieq(dot, L".txt") && mi_hex16(n, stem)) {
            s->kinds |= MI_CHEATS;
            int v = mi_bid_version(n);
            if (v > 0) mi_add_ver(s, v);
        } else if (where == 2 && (!_wcsnicmp(n, L"subsdk", 6) || mi_ieq(n, L"main"))) {
            s->code = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Deletes `dir` (only ever our own staging folder) and what it holds; links are removed, never followed. Every path
 * is whole (mi_join): one that would not fit is skipped, never cut. */
static void mi_rmtree(const wchar_t *dir) {
    WIN32_FIND_DATAW fd;
    wchar_t *pat = mi_join(dir, L"*");
    HANDLE h = pat ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
    free(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (mi_dots(fd.cFileName)) continue;
            wchar_t *p = mi_join(dir, fd.cFileName);
            if (!p) continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) mi_rmtree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); if (!DeleteFileW(p)) RemoveDirectoryW(p); }
            free(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
static int mi_copytree(const wchar_t *src, const wchar_t *dst, int depth) {
    WIN32_FIND_DATAW fd;
    if (depth > 64 || (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) return 0;
    wchar_t *pat = mi_join(src, L"*");
    if (!pat) return 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return 1;   /* (nothing to copy) */
    int ok = 1;
    do {
        if (mi_dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;   /* (links: not followed) */
        wchar_t *a = mi_join(src, fd.cFileName), *b = mi_join(dst, fd.cFileName);
        ok = a && b && ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? mi_copytree(a, b, depth + 1) : CopyFileW(a, b, TRUE) != 0);
        free(a);
        free(b);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
static int mi_untar(const wchar_t *archive, const wchar_t *into) {   /* Windows' tar.exe (bsdtar: refuses .. paths) */
    wchar_t tar[MAX_PATH];
    if (!GetSystemDirectoryW(tar, MAX_PATH - 10)) return 0;
    wcscat(tar, L"\\tar.exe");
    if (GetFileAttributesW(tar) == INVALID_FILE_ATTRIBUTES) return 0;
    const size_t la = wcslen(archive) + 2, ld = wcslen(into) + 2, n = wcslen(tar) + la + ld + 32;
    wchar_t *a = (wchar_t *)malloc(la * sizeof(wchar_t)), *d = (wchar_t *)malloc(ld * sizeof(wchar_t));
    wchar_t *cmd = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!a || !d || !cmd) { free(a); free(d); free(cmd); return 0; }
    mi_plain(archive, a, la);
    mi_plain(into, d, ld);
    _snwprintf(cmd, n, L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, a, d);
    cmd[n - 1] = 0;
    free(a);
    free(d);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    const BOOL started = CreateProcessW(tar, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    if (!started) return 0;
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 30 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    return code == 0;
}
/* the content root (heap): the staging folder, or the one folder it holds when that is all and no part of a mod (a zip
 * of a folder) */
static wchar_t *mi_root(const wchar_t *staging) {
    wchar_t *root = _wcsdup(staging);
    for (int i = 0; root && i < 2; i++) {
        WIN32_FIND_DATAW fd;
        wchar_t only[MAX_PATH] = L"", *pat = mi_join(root, L"*");
        int dirs = 0, files = 0;
        HANDLE h = pat ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
        free(pat);
        if (h == INVALID_HANDLE_VALUE) break;
        do {
            if (mi_dots(fd.cFileName)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs++; wcsncpy(only, fd.cFileName, MAX_PATH - 1); }
            else files++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (dirs != 1 || files || mi_part(only)) break;
        wchar_t *next = mi_join(root, only);
        if (!next) break;
        free(root);
        root = next;
    }
    return root;
}
static void mi_name(const wchar_t *src, wchar_t *name, size_t n) {   /* the source's base name, safe for the list */
    const wchar_t *b = wcsrchr(src, L'\\'), *f = wcsrchr(src, L'/');
    if (f > b) b = f;
    b = b ? b + 1 : src;
    wcsncpy(name, b, n - 1);
    name[n - 1] = 0;
    if (!mi_is_dir(src)) {   /* an archive: no extension (and no .tar of .tar.gz) */
        wchar_t *d = wcsrchr(name, L'.');
        if (d && d != name) *d = 0;
        d = wcsrchr(name, L'.');
        if (d && d != name && mi_ieq(d, L".tar")) *d = 0;
    }
    for (wchar_t *c = name; *c; c++) if (*c == L'|' || *c < 32) *c = L'-';
    while (name[0] == L'_' || name[0] == L'.' || name[0] == L' ') memmove(name, name + 1, wcslen(name) * sizeof *name);
    size_t l = wcslen(name);
    while (l && (name[l - 1] == L'.' || name[l - 1] == L' ')) name[--l] = 0;
    if (!name[0]) wcscpy(name, L"mod");
}

static int mods_install(const wchar_t *src_in, const wchar_t *v100_in, const wchar_t *v143_in, int fallback, ModInstall *out) {
    memset(out, 0, sizeof *out);
    wchar_t *src = mi_abs(src_in), *d100 = mi_abs(v100_in), *d143 = v143_in && *v143_in ? mi_abs(v143_in) : NULL;
    wchar_t *staging = NULL, *root = NULL, *dest = NULL, tick[48];
    int rc = MI_IO;
    if (!src || !d100 || (v143_in && *v143_in && !d143)) goto done;
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) { rc = MI_NOT_FOUND; goto done; }
    CreateDirectoryW(d100, NULL);
    swprintf(tick, 48, L"_installing-%llu", (unsigned long long)GetTickCount64());
    if (!(staging = mi_join(d100, tick)) || !CreateDirectoryW(staging, NULL)) { free(staging); staging = NULL; goto done; }
    {
        const int folder = mi_is_dir(src);
        if (!(folder ? mi_copytree(src, staging, 0) : mi_untar(src, staging))) { rc = folder ? MI_IO : MI_UNPACK; goto done; }
    }
    if (!(root = mi_root(staging))) goto done;
    {
        MiScan s;
        memset(&s, 0, sizeof s);
        mi_walk(root, 0, 0, 0, &s);
        out->kinds = s.kinds;
        if (s.toolong) goto done;   /* (a path past what Windows takes: MI_IO, nothing kept) */
        if (s.code) { rc = MI_CODE_MODULES; goto done; }
        if (!s.kinds) {
            if (s.other) { wcscpy(out->other_title, s.other_title); rc = MI_OTHER_GAME; }
            else rc = MI_NOT_A_MOD;
            goto done;
        }
        /* the version: the one in use when the mod names it, else the newest it names, else the one in use */
        int v = 0;
        for (int i = 0; i < s.nvers; i++) {
            if (s.vers[i] == fallback) { v = fallback; break; }
            if (s.vers[i] > v) v = s.vers[i];
        }
        out->named = v != 0;
        out->version = v ? v : fallback;
        out->to_v143 = out->version > 100 && d143 != NULL;
    }
    {
        const wchar_t *target = out->to_v143 ? d143 : d100;
        CreateDirectoryW(target, NULL);
        mi_name(src, out->name, sizeof out->name / sizeof *out->name);
        wchar_t base[128];
        wcscpy(base, out->name);
        base[100] = 0;   /* (room for " (99)": the formatted name always fits) */
        for (int k = 2; k < 100; k++) {
            free(dest);
            if (!(dest = mi_join(target, out->name))) goto done;
            if (GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES) break;
            if (swprintf(out->name, sizeof out->name / sizeof *out->name, L"%ls (%d)", base, k) < 0) goto done;
        }
        if (!MoveFileExW(root, dest, 0)) goto done;
        mi_plain(dest, out->dest, MAX_PATH);
        rc = MI_OK;
    }
done:
    if (staging && (rc != MI_OK || (root && wcscmp(root, staging)))) mi_rmtree(staging);   /* (a failure, or the wrappers left) */
    free(src); free(d100); free(d143); free(staging); free(root); free(dest);
    return rc;
}
