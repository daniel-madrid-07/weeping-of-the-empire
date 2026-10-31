/* Save import and backup for the Windows launcher: see saves.h. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_saves_fail_step;

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int is_dir(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int dots(const wchar_t *n) { return !wcscmp(n, L".") || !wcscmp(n, L".."); }
static int newer(FILETIME a, FILETIME b) { return CompareFileTime(&a, &b) > 0; }

int saves_is_save(const wchar_t *dir, int *slots, FILETIME *newest) {
    int n = 0;
    FILETIME best = {0, 0};
    for (int i = 0; i < 6; i++) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\slot_%02d\\progress.sav", dir, i);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        n++;
        if (newer(a.ftLastWriteTime, best)) best = a.ftLastWriteTime;
    }
    if (slots) *slots = n;
    if (newest) *newest = best;
    return n > 0;
}

static int add(SaveCandidate *out, int count, int max, const wchar_t *source, const wchar_t *path, const wchar_t *user) {
    int slots;
    FILETIME t;
    if (count >= max || !saves_is_save(path, &slots, &t)) return count;
    SaveCandidate *c = &out[count];
    memset(c, 0, sizeof *c);
    swprintf(c->source, 24, L"%ls", source);
    swprintf(c->path, MAX_PATH, L"%ls", path);
    swprintf(c->user, 40, L"%ls", user ? user : L"");
    c->slots = slots;
    c->newest = t;
    return count + 1;
}

static int by_newest(const void *a, const void *b) {
    return CompareFileTime(&((const SaveCandidate *)b)->newest, &((const SaveCandidate *)a)->newest);
}

/* Calls fn(dir, name, ctx) for each subfolder of dir. */
typedef int (*dir_fn)(const wchar_t *dir, const wchar_t *name, void *ctx);
static void each_subdir(const wchar_t *dir, dir_fn fn, void *ctx) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !dots(fd.cFileName) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            if (fn(dir, fd.cFileName, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct Scan {
    SaveCandidate *out;
    int count, max;
    const wchar_t *source;
} Scan;

/* yuzu family: <root>\<user>\0100F2C0115B6000 */
static int yuzu_user(const wchar_t *dir, const wchar_t *name, void *ctx) {
    Scan *s = ctx;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls\\%016llX", dir, name, TOTK_TITLE_ID);
    s->count = add(s->out, s->count, s->max, s->source, p, name);
    return s->count >= s->max;
}

/* Ryujinx: <root>\<index>\0 (the committed copy; 1 is the working one), ExtraData0 starts with the program id */
static int ryujinx_index(const wchar_t *dir, const wchar_t *name, void *ctx) {
    Scan *s = ctx;
    wchar_t p[MAX_PATH], extra[MAX_PATH];
    swprintf(extra, MAX_PATH, L"%ls\\%ls\\ExtraData0", dir, name);
    FILE *f = _wfopen(extra, L"rb");
    if (f) {
        unsigned long long id = 0;
        size_t got = fread(&id, 1, 8, f);
        fclose(f);
        if (got == 8 && id != TOTK_TITLE_ID) return 0;
    }
    swprintf(p, MAX_PATH, L"%ls\\%ls\\0", dir, name);
    if (!saves_is_save(p, NULL, NULL)) swprintf(p, MAX_PATH, L"%ls\\%ls\\1", dir, name);
    s->count = add(s->out, s->count, s->max, s->source, p, L"");
    return s->count >= s->max;
}

static int nxrecomp_user(const wchar_t *dir, const wchar_t *name, void *ctx) {
    Scan *s = ctx;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", dir, name);
    s->count = add(s->out, s->count, s->max, s->source, p, name);
    return s->count >= s->max;
}

int saves_find(const wchar_t *appdata, SaveCandidate *out, int max) {
    static const wchar_t *const yuzus[] = {L"yuzu", L"eden", L"citron", L"sudachi", L"suyu", L"torzu"};
    Scan s = {out, 0, max, NULL};
    wchar_t root[MAX_PATH];
    for (size_t i = 0; i < sizeof yuzus / sizeof *yuzus; i++) {
        swprintf(root, MAX_PATH, L"%ls\\%ls\\nand\\user\\save\\0000000000000000", appdata, yuzus[i]);
        s.source = yuzus[i];
        if (is_dir(root)) each_subdir(root, yuzu_user, &s);
    }
    swprintf(root, MAX_PATH, L"%ls\\Ryujinx\\bis\\user\\save", appdata);
    s.source = L"Ryujinx";
    if (is_dir(root)) each_subdir(root, ryujinx_index, &s);
    swprintf(root, MAX_PATH, L"%ls\\nxrecomp\\TotK\\profile\\save", appdata);
    s.source = L"nxrecomp";
    if (is_dir(root)) each_subdir(root, nxrecomp_user, &s);
    qsort(out, (size_t)s.count, sizeof *out, by_newest);
    return s.count;
}

static void walk(Scan *s, const wchar_t *dir, int depth) {
    if (s->count >= s->max) return;
    if (saves_is_save(dir, NULL, NULL)) {   /* a save folder: its slot_0N are not searched further */
        s->count = add(s->out, s->count, s->max, L"carpeta", dir, L"");
        return;
    }
    if (depth <= 0) return;
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t sub[MAX_PATH];
        if (swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0) continue;
        walk(s, sub, depth - 1);
    } while (s->count < s->max && FindNextFileW(h, &fd));
    FindClose(h);
}

int saves_find_under(const wchar_t *root, int depth, SaveCandidate *out, int max) {
    Scan s = {out, 0, max, L"carpeta"};
    walk(&s, root, depth);
    qsort(out, (size_t)s.count, sizeof *out, by_newest);
    return s.count;
}

void saves_dir(const wchar_t *base, wchar_t *out, size_t n) { swprintf(out, n, L"%ls\\userdata\\save", base); }
void saves_backups_dir(const wchar_t *base, wchar_t *out, size_t n) { swprintf(out, n, L"%ls\\backups", base); }

static void mkdirs(const wchar_t *path) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls", path);
    for (wchar_t *q = p; *q; q++)
        if (*q == L'\\' && q > p && q[-1] != L':' && q[-1] != L'\\') { *q = 0; CreateDirectoryW(p, NULL); *q = L'\\'; }
    CreateDirectoryW(p, NULL);
}

int saves_copy_tree(const wchar_t *src, const wchar_t *dst) {
    if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    int rc = 0;
    do {
        if (dots(fd.cFileName) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t s[MAX_PATH], d[MAX_PATH];
        if (swprintf(s, MAX_PATH, L"%ls\\%ls", src, fd.cFileName) < 0 || swprintf(d, MAX_PATH, L"%ls\\%ls", dst, fd.cFileName) < 0) { rc = -1; break; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rc = saves_copy_tree(s, d);
        else if (!CopyFileW(s, d, FALSE)) rc = -1;
        else SetFileAttributesW(d, FILE_ATTRIBUTE_NORMAL);   /* a read-only file from an SD card stays deletable */
    } while (!rc && FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

int saves_delete_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int rc = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (dots(fd.cFileName)) continue;
            wchar_t p[MAX_PATH];
            if (swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0) { rc = -1; continue; }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (saves_delete_tree(p)) rc = -1;
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirectoryW(p)) rc = -1;   /* a junction: the link, not what it points at */
            } else {
                SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(p)) rc = -1;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir) && exists(dir)) rc = -1;
    return rc;
}

void saves_recover(const wchar_t *base) {
    wchar_t save[MAX_PATH], tmp[MAX_PATH];
    saves_dir(base, save, MAX_PATH);
    swprintf(tmp, MAX_PATH, L"%ls.new", save);
    if (exists(tmp)) saves_delete_tree(tmp);
    swprintf(tmp, MAX_PATH, L"%ls.old", save);
    if (exists(tmp)) {
        if (!exists(save)) MoveFileW(tmp, save);
        else saves_delete_tree(tmp);
    }
}

int saves_backup(const wchar_t *base, const wchar_t *why, wchar_t *out_path, size_t n) {
    wchar_t save[MAX_PATH], dir[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH];
    saves_dir(base, save, MAX_PATH);
    if (!saves_is_save(save, NULL, NULL)) return 0;
    saves_backups_dir(base, dir, MAX_PATH);
    mkdirs(dir);
    SYSTEMTIME t;
    GetLocalTime(&t);
    /* <time>-<nn>: nn counts backups made in the same second, so the names sort by age whatever the why */
    for (int k = 0; k < 100; k++) {
        swprintf(dst, MAX_PATH, L"%ls\\save-%04u%02u%02u-%02u%02u%02u-%02d-%ls", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond, k, why);
        wchar_t pat[MAX_PATH];
        swprintf(pat, MAX_PATH, L"%ls\\save-%04u%02u%02u-%02u%02u%02u-%02d-*", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond, k);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) break;
        FindClose(h);
    }
    swprintf(tmp, MAX_PATH, L"%ls.tmp", dst);
    if (exists(tmp)) saves_delete_tree(tmp);
    if (saves_copy_tree(save, tmp) || !MoveFileW(tmp, dst)) {
        saves_delete_tree(tmp);
        return -1;
    }
    if (out_path) swprintf(out_path, n, L"%ls", dst);
    return 1;
}

static int by_name_desc(const void *a, const void *b) {
    return wcscmp(((const SaveBackup *)b)->name, ((const SaveBackup *)a)->name);
}

int saves_list_backups(const wchar_t *base, SaveBackup *out, int max) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    saves_backups_dir(base, dir, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%ls\\save-*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        size_t len = wcslen(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || len >= 64 || (len > 4 && !wcscmp(fd.cFileName + len - 4, L".tmp"))) continue;
        if (count >= max) break;
        SaveBackup *b = &out[count];
        swprintf(b->name, 64, L"%ls", fd.cFileName);
        swprintf(b->path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (!saves_is_save(b->path, &b->slots, &b->newest)) continue;
        count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(out, (size_t)count, sizeof *out, by_name_desc);   /* the names start with the time */
    return count;
}

void saves_prune(const wchar_t *base, int keep) {
    enum { MAX = 512 };
    SaveBackup *all = calloc(MAX, sizeof *all);
    if (!all) return;
    int n = saves_list_backups(base, all, MAX), autos = 0;
    for (int i = 0; i < n; i++) {
        size_t len = wcslen(all[i].name);
        int is_auto = len >= 5 && !wcscmp(all[i].name + len - 5, L"-auto");
        if (is_auto && ++autos > keep) saves_delete_tree(all[i].path);
    }
    free(all);
}

int saves_replace(const wchar_t *base, const wchar_t *src) {
    wchar_t save[MAX_PATH], fresh[MAX_PATH], old[MAX_PATH], parent[MAX_PATH];
    saves_dir(base, save, MAX_PATH);
    swprintf(fresh, MAX_PATH, L"%ls.new", save);
    swprintf(old, MAX_PATH, L"%ls.old", save);
    swprintf(parent, MAX_PATH, L"%ls\\userdata", base);
    mkdirs(parent);
    if (exists(fresh)) saves_delete_tree(fresh);
    if (g_saves_fail_step == 1 || saves_copy_tree(src, fresh) || !saves_is_save(fresh, NULL, NULL)) {
        saves_delete_tree(fresh);
        return 1;
    }
    int had = exists(save);
    if (had) {
        if (exists(old)) saves_delete_tree(old);
        if (g_saves_fail_step == 2 || !MoveFileW(save, old)) {
            saves_delete_tree(fresh);
            return 2;
        }
    }
    if (g_saves_fail_step == 3 || !MoveFileW(fresh, save)) {
        if (had) MoveFileW(old, save);
        saves_delete_tree(fresh);
        return 3;
    }
    if (had) saves_delete_tree(old);
    return 0;
}
