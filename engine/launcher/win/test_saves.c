/* Tests for saves.c on fake folder trees in %TEMP% (no game, no GPU): emulator detection, import with a failure at
 * each step, recovery of an interrupted import, backups and their pruning. Built and run by build.py test. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include "saves.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static wchar_t g_root[MAX_PATH];

static void mkdirs(const wchar_t *path) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls", path);
    for (wchar_t *q = p; *q; q++)
        if (*q == L'\\' && q > p && q[-1] != L':') { *q = 0; CreateDirectoryW(p, NULL); *q = L'\\'; }
    CreateDirectoryW(p, NULL);
}

/* <root>\<rel>\slot_NN\progress.sav containing text, written with time minutes_ago before now */
static void fake_slot(const wchar_t *rel, int slot, const char *text, int minutes_ago) {
    wchar_t d[MAX_PATH], f[MAX_PATH];
    swprintf(d, MAX_PATH, L"%ls\\%ls\\slot_%02d", g_root, rel, slot);
    mkdirs(d);
    swprintf(f, MAX_PATH, L"%ls\\progress.sav", d);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WriteFile(h, text, (DWORD)strlen(text), &w, NULL);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
    t.QuadPart -= (ULONGLONG)minutes_ago * 60 * 10000000ull;
    FILETIME ft = {t.LowPart, t.HighPart};
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    swprintf(f, MAX_PATH, L"%ls\\caption.sav", d);
    h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, NULL);   /* read-only, as from an SD card */
    CloseHandle(h);
}

static void fake_file(const wchar_t *rel, const void *data, DWORD n) {
    wchar_t f[MAX_PATH];
    swprintf(f, MAX_PATH, L"%ls\\%ls", g_root, rel);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WriteFile(h, data, n, &w, NULL);
    CloseHandle(h);
}

/* the text of <dir>\slot_00\progress.sav */
static void slot0(const wchar_t *dir, char *out, size_t n) {
    wchar_t f[MAX_PATH];
    swprintf(f, MAX_PATH, L"%ls\\slot_00\\progress.sav", dir);
    out[0] = 0;
    FILE *fp = _wfopen(f, L"rb");
    if (!fp) return;
    size_t k = fread(out, 1, n - 1, fp);
    out[k] = 0;
    fclose(fp);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

int main(void) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_root, MAX_PATH, L"%lstotk-saves-test-%lu", tmp, GetCurrentProcessId());
    mkdirs(g_root);
    wchar_t appdata[MAX_PATH], base[MAX_PATH], save[MAX_PATH], p[MAX_PATH];
    swprintf(appdata, MAX_PATH, L"%ls\\appdata", g_root);
    swprintf(base, MAX_PATH, L"%ls\\appdata\\nxrecomp\\TotK-engine", g_root);
    saves_dir(base, save, MAX_PATH);

    /* emulators: yuzu (user A, 30 min old), eden (user B, 2 slots, 10 min), yuzu with another game (ignored),
     * Ryujinx TotK (5 min) and another game, our old runtime (60 min) */
    fake_slot(L"appdata\\yuzu\\nand\\user\\save\\0000000000000000\\AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\\0100F2C0115B6000", 0, "yuzu", 30);
    fake_slot(L"appdata\\eden\\nand\\user\\save\\0000000000000000\\BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\\0100F2C0115B6000", 0, "eden", 10);
    fake_slot(L"appdata\\eden\\nand\\user\\save\\0000000000000000\\BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\\0100F2C0115B6000", 1, "eden1", 12);
    fake_slot(L"appdata\\yuzu\\nand\\user\\save\\0000000000000000\\CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\\0100000000010000", 0, "other", 1);
    fake_slot(L"appdata\\Ryujinx\\bis\\user\\save\\0000000000000001\\0", 0, "ryujinx", 5);
    fake_slot(L"appdata\\Ryujinx\\bis\\user\\save\\0000000000000002\\0", 0, "ryujinx-other", 2);
    unsigned long long totk = TOTK_TITLE_ID, other = 0x0100000000010000ull;
    fake_file(L"appdata\\Ryujinx\\bis\\user\\save\\0000000000000001\\ExtraData0", &totk, 8);
    fake_file(L"appdata\\Ryujinx\\bis\\user\\save\\0000000000000002\\ExtraData0", &other, 8);
    fake_slot(L"appdata\\nxrecomp\\TotK\\profile\\save\\0123456789abcdef", 0, "nxrecomp", 60);
    /* a JKSV folder: two backups, a few levels down */
    fake_slot(L"sd\\JKSV\\The Legend of Zelda Tears of the Kingdom\\Player - 2026-10-01", 0, "jksv1", 100);
    fake_slot(L"sd\\JKSV\\The Legend of Zelda Tears of the Kingdom\\Player - 2026-10-20", 0, "jksv2", 50);

    SaveCandidate c[16];
    int n = saves_find(appdata, c, 16);
    CHECK(n == 4);
    if (n == 4) {
        CHECK(!wcscmp(c[0].source, L"Ryujinx"));
        CHECK(!wcscmp(c[1].source, L"eden") && c[1].slots == 2 && !wcsncmp(c[1].user, L"BBBB", 4));
        CHECK(!wcscmp(c[2].source, L"yuzu"));
        CHECK(!wcscmp(c[3].source, L"nxrecomp"));
    }
    SaveCandidate j[16];
    swprintf(p, MAX_PATH, L"%ls\\sd", g_root);
    int nj = saves_find_under(p, 4, j, 16);
    CHECK(nj == 2);
    if (nj == 2) CHECK(wcsstr(j[0].path, L"2026-10-20") != NULL);
    CHECK(saves_find_under(p, 1, j, 16) == 0);   /* too shallow */

    /* no save yet: nothing to back up */
    wchar_t bk[MAX_PATH];
    CHECK(saves_backup(base, L"auto", bk, MAX_PATH) == 0);

    /* import yuzu; read-only files stay deletable */
    char text[64];
    CHECK(saves_replace(base, c[2].path) == 0);
    slot0(save, text, sizeof text);
    CHECK(!strcmp(text, "yuzu"));

    /* a failure at each step keeps the yuzu save, leaves nothing behind */
    for (int step = 1; step <= 3; step++) {
        g_saves_fail_step = step;
        CHECK(saves_replace(base, c[1].path) == step);
        g_saves_fail_step = 0;
        slot0(save, text, sizeof text);
        CHECK(!strcmp(text, "yuzu"));
        swprintf(p, MAX_PATH, L"%ls.new", save);
        CHECK(!exists(p));
        swprintf(p, MAX_PATH, L"%ls.old", save);
        CHECK(!exists(p));
    }
    /* a source that is not a save is refused */
    swprintf(p, MAX_PATH, L"%ls\\sd", g_root);
    CHECK(saves_replace(base, p) == 1);
    slot0(save, text, sizeof text);
    CHECK(!strcmp(text, "yuzu"));

    /* an import interrupted between its renames: recover puts the old save back; a leftover save.new goes */
    swprintf(p, MAX_PATH, L"%ls.old", save);
    CHECK(MoveFileW(save, p));
    wchar_t fresh[MAX_PATH];
    swprintf(fresh, MAX_PATH, L"%ls.new", save);
    CHECK(saves_copy_tree(c[1].path, fresh) == 0);
    saves_recover(base);
    slot0(save, text, sizeof text);
    CHECK(!strcmp(text, "yuzu"));
    CHECK(!exists(p) && !exists(fresh));

    /* backups: 13 auto + 1 manual, pruned to 10 auto; listed newest first */
    CHECK(saves_backup(base, L"manual", bk, MAX_PATH) == 1);
    slot0(bk, text, sizeof text);
    CHECK(!strcmp(text, "yuzu"));
    for (int i = 0; i < 13; i++) CHECK(saves_backup(base, L"auto", bk, MAX_PATH) == 1);
    SaveBackup b[64];
    CHECK(saves_list_backups(base, b, 64) == 14);
    saves_prune(base, 10);
    int nb = saves_list_backups(base, b, 64), autos = 0, manual = 0;
    for (int i = 0; i < nb; i++) {
        if (wcsstr(b[i].name, L"-auto")) autos++;
        if (wcsstr(b[i].name, L"-manual")) manual++;
    }
    CHECK(nb == 11 && autos == 10 && manual == 1);
    CHECK(nb > 1 && !wcscmp(b[0].path, bk));   /* the newest is the last one made */
    for (int i = 1; i < nb; i++) CHECK(wcscmp(b[i - 1].name, b[i].name) > 0);

    /* restore a backup over the eden import */
    CHECK(saves_replace(base, c[1].path) == 0);
    slot0(save, text, sizeof text);
    CHECK(!strcmp(text, "eden"));
    CHECK(saves_replace(base, bk) == 0);
    slot0(save, text, sizeof text);
    CHECK(!strcmp(text, "yuzu"));

    CHECK(saves_delete_tree(g_root) == 0);
    CHECK(!exists(g_root));
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ok: saves", g_fail);
    return g_fail != 0;
}
