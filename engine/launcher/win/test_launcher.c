/* Tests for launcher.c: game-log pruning, the early-exit window and the update path. launcher.c is one translation unit with
 * wWinMain and static helpers, so this file includes it; build.py test builds it next to test_saves.c with fake_game.exe as the
 * package's totk.exe. Fake trees in %TEMP%\totk-launcher-test-<pid>.*/
#define wWinMain launcher_wWinMain
#include "launcher.c"
#undef wWinMain

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static wchar_t g_root[MAX_PATH];

static void touch(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static int count(const wchar_t *dir, const wchar_t *pattern) {
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* A package folder: totk.exe (fake_game.exe), img\main.bin, player.txt with env.FAKE_GAME=<spec> */
static void make_package(const wchar_t *pkg, const wchar_t *fake_exe, const char *spec) {
    wchar_t p[MAX_PATH], img[MAX_PATH];
    mkdirs(pkg);
    swprintf(img, MAX_PATH, L"%ls\\img", pkg); mkdirs(img);
    swprintf(p, MAX_PATH, L"%ls\\main.bin", img); touch(p);
    swprintf(p, MAX_PATH, L"%ls\\totk.exe", pkg); CopyFileW(fake_exe, p, FALSE);
    swprintf(p, MAX_PATH, L"%ls\\player.txt", pkg);
    FILE *f = _wfopen(p, L"w");
    fprintf(f, "img=img\nenv.FAKE_GAME=%s\n", spec);
    fclose(f);
}

/* A release folder parent\name as package.py makes it: "TotK Launcher.exe" and totk.exe (both fake_game.exe; with_game 0
 * leaves totk.exe out), player.txt */
static void make_release(const wchar_t *parent, const wchar_t *name, const wchar_t *fake_exe, int with_game) {
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\%ls", parent, name);
    mkdirs(dir);
    swprintf(p, MAX_PATH, L"%ls\\TotK Launcher.exe", dir); CopyFileW(fake_exe, p, FALSE);
    if (with_game) { swprintf(p, MAX_PATH, L"%ls\\totk.exe", dir); CopyFileW(fake_exe, p, FALSE); }
    swprintf(p, MAX_PATH, L"%ls\\player.txt", dir);
    FILE *f = _wfopen(p, L"w"); fprintf(f, "commit=%ls\n", name); fclose(f);
}

static int g_answer, g_asked;
static int answer_update(const wchar_t *name) { (void)name; g_asked++; return g_answer; }

static int log_has(const char *text) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\launcher.log", g_local);
    fflush(g_log);
    FILE *f = _wfopen(p, L"r");
    if (!f) return 0;
    char line[2048]; int found = 0;
    while (!found && fgets(line, sizeof line, f)) found = strstr(line, text) != NULL;
    fclose(f);
    return found;
}

static int run(const wchar_t *fake_exe, const char *spec, DWORD early_ms, wchar_t *why, HANDLE *game) {
    wchar_t pkg[MAX_PATH];
    static int k;
    Sleep(1100);   /* the game log is named by the second: one start per second, as a player does */
    swprintf(pkg, MAX_PATH, L"%ls\\pkg%d", g_root, k++);
    make_package(pkg, fake_exe, spec);
    g_early_ms = early_ms;
    FILETIME started;
    why[0] = 0;
    *game = NULL;
    return run_package(pkg, L"test", why, 768, game, &started);
}

int main(int argc, char **argv) {
    wchar_t tmp[MAX_PATH], fake[MAX_PATH], p[MAX_PATH], logs[MAX_PATH], crashes[MAX_PATH];
    if (argc < 2) { fprintf(stderr, "usage: test_launcher.exe <fake_game.exe>\n"); return 2; }
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, fake, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_root, MAX_PATH, L"%lstotk-launcher-test-%lu", tmp, GetCurrentProcessId());
    swprintf(g_base, MAX_PATH, L"%ls\\appdata\\nxrecomp\\TotK-engine", g_root);
    swprintf(g_local, MAX_PATH, L"%ls\\localappdata\\nxrecomp\\TotK-engine", g_root);
    swprintf(g_dir, MAX_PATH, L"%ls", g_root);
    mkdirs(g_base); mkdirs(g_local);
    swprintf(p, MAX_PATH, L"%ls\\launcher.log", g_local);
    g_log = _wfopen(p, L"w");
    swprintf(logs, MAX_PATH, L"%ls\\logs", g_local);
    swprintf(crashes, MAX_PATH, L"%ls\\crashes", g_local);

    /* prune_files: 600 logs (more than the old fixed 512-name table), the newest 7 stay, other files untouched */
    mkdirs(logs);
    for (int i = 0; i < 600; i++) {
        swprintf(p, MAX_PATH, L"%ls\\totk-202610%02d-%06d.log", logs, 1 + i / 100, i % 100);
        touch(p);
    }
    swprintf(p, MAX_PATH, L"%ls\\notes.txt", logs); touch(p);
    prune_files(logs, L"totk-*.log", 7);
    CHECK(count(logs, L"totk-*.log") == 7);
    swprintf(p, MAX_PATH, L"%ls\\totk-20261006-000099.log", logs); CHECK(exists(p));    /* the newest */
    swprintf(p, MAX_PATH, L"%ls\\totk-20261006-000093.log", logs); CHECK(exists(p));    /* the 7th newest */
    swprintf(p, MAX_PATH, L"%ls\\totk-20261006-000092.log", logs); CHECK(!exists(p));
    swprintf(p, MAX_PATH, L"%ls\\notes.txt", logs); CHECK(exists(p));
    prune_files(logs, L"none-*.log", 7);   /* no match: nothing happens */
    CHECK(count(logs, L"totk-*.log") == 7);

    /* run_package: each start prunes to KEEP_GAME_LOGS - 1 and adds its own log, so 8 stay */
    wchar_t why[768];
    HANDLE game;
    int r = run(fake, "0,0", 5000, why, &game);
    CHECK(r == QUIT);   /* exit 0 inside the window: the player quit, no fallback */
    CHECK(count(logs, L"totk-*.log") == KEEP_GAME_LOGS);

    /* an error exit inside the window: FAILED (the next package is tried), the reason names the code and the log */
    r = run(fake, "3,100", 5000, why, &game);
    CHECK(r == FAILED);
    CHECK(wcsstr(why, L"0x00000003") != NULL);
    CHECK(wcsstr(why, L"Registro:") != NULL);

    /* an illegal instruction inside the window (a CPU without what totk.exe was built for): said so, with the log */
    r = run(fake, "-1073741795,100", 5000, why, &game);
    CHECK(r == FAILED);
    CHECK(wcsstr(why, L"0xC000001D") != NULL && wcsstr(why, L"procesador") != NULL);
    CHECK(wcsstr(why, L"Registro:") != NULL);

    /* this test machine builds and runs the game, so it has every x86-64-v3 feature */
    { wchar_t miss[256]; cpu_missing(miss, 256, 3); CHECK(miss[0] == 0); if (miss[0]) fwprintf(stderr, L"missing: %ls\n", miss); }
    { wchar_t miss[256]; cpu_missing(miss, 256, 2); CHECK(miss[0] == 0); }

    /* a crash inside the window: the reason points at the report the game wrote (was: only the log) */
    r = run(fake, "5,100,crash", 5000, why, &game);
    CHECK(r == FAILED);
    CHECK(wcsstr(why, L"Informe del fallo:") != NULL && wcsstr(why, L"crash-") != NULL);
    CHECK(count(crashes, L"crash-*.txt") == 1);

    /* still running when the window ends: RUNNING with the process handle; an error after it is not a fallback */
    r = run(fake, "7,1500", 300, why, &game);
    CHECK(r == RUNNING && game);
    if (game) {
        CHECK(WaitForSingleObject(game, 5000) == WAIT_OBJECT_0);
        DWORD code = 0; GetExitCodeProcess(game, &code);
        CHECK(code == 7);
        CloseHandle(game);
    }

    /* a missing totk.exe: MISSING */
    swprintf(p, MAX_PATH, L"%ls\\empty", g_root); mkdirs(p);
    FILETIME st;
    CHECK(run_package(p, L"empty", why, 768, &game, &st) == MISSING);

    CHECK(count(logs, L"totk-*.log") == KEEP_GAME_LOGS);

    /* note_package_change: the first package is only recorded; the same one again does nothing; a package with another
     * commit makes a "before-update" copy of the save (with no save: none, but it is recorded) */
    {
        wchar_t pa[MAX_PATH], pb[MAX_PATH], pc[MAX_PATH], save[MAX_PATH], bks[MAX_PATH];
        const wchar_t *dirs[3] = {pa, pb, pc};
        const char *commits[3] = {"aaaa111", "bbbb222", "cccc333"};
        swprintf(pa, MAX_PATH, L"%ls\\TotK-PC-20261025-0013-aaaa111", g_root);
        swprintf(pb, MAX_PATH, L"%ls\\TotK-PC-20261025-0243-bbbb222", g_root);
        swprintf(pc, MAX_PATH, L"%ls\\TotK-PC-20261026-1200-cccc333", g_root);
        for (int i = 0; i < 3; i++) {
            mkdirs(dirs[i]);
            swprintf(p, MAX_PATH, L"%ls\\player.txt", dirs[i]);
            FILE *pf = _wfopen(p, L"w"); fprintf(pf, "commit=%s\n", commits[i]); fclose(pf);
        }
        saves_dir(g_base, save, MAX_PATH);
        saves_backups_dir(g_base, bks, MAX_PATH);
        swprintf(p, MAX_PATH, L"%ls\\slot_00", save); mkdirs(p);
        swprintf(p, MAX_PATH, L"%ls\\slot_00\\progress.sav", save); touch(p);
        CHECK(note_package_change(pa) == 0);              /* first start ever: recorded, no copy */
        CHECK(count(bks, L"*before-update") == 0);
        CHECK(note_package_change(pa) == 0);              /* same package */
        CHECK(note_package_change(pb) == 1);              /* the update */
        CHECK(count(bks, L"*before-update") == 1);
        CHECK(note_package_change(pb) == 0);              /* its second start */
        CHECK(count(bks, L"*before-update") == 1);
        swprintf(p, MAX_PATH, L"%ls\\slot_00\\progress.sav", save); DeleteFileW(p);
        swprintf(p, MAX_PATH, L"%ls\\slot_00", save); RemoveDirectoryW(p);
        CHECK(note_package_change(pc) == 0);              /* no save to copy, but recorded */
        swprintf(p, MAX_PATH, L"%ls\\last-package.txt", g_base);
        wchar_t got[MAX_PATH + 80] = L"";
        FILE *lf = _wfopen(p, L"r, ccs=UTF-8"); if (lf) { fgetws(got, MAX_PATH + 80, lf); fclose(lf); }
        CHECK(wcsncmp(got, L"cccc333\t", 8) == 0);
    }

    /* newer_release: the newest complete release folder beside the launcher's own with a later stamp; NO-USAR (any
     * spelling), incomplete and unstamped folders are skipped; a nested <name>\<name> (Explorer's "Extract all") counts */
    {
        wchar_t rel[MAX_PATH], own[MAX_PATH], newer[MAX_PATH], nested[MAX_PATH], dir[MAX_PATH], la[MAX_PATH];
        swprintf(rel, MAX_PATH, L"%ls\\release", g_root);
        make_release(rel, L"TotK-PC-20261025-0013-aaaa111", fake, 1);
        make_release(rel, L"TotK-PC-20261025-0243-bbbb222", fake, 1);
        make_release(rel, L"TotK-PC-20261025-0644-cccc333-NO-USAR", fake, 1);
        swprintf(own, MAX_PATH, L"%ls\\TotK-PC-20261025-0243-bbbb222", rel);
        swprintf(g_dir, MAX_PATH, L"%ls", own);
        wcscpy(g_exe, L"TotK Launcher.exe");
        CHECK(!newer_release(dir, la, MAX_PATH));        /* only an older one and a NO-USAR one */
        make_release(rel, L"TotK-PC-20261025-0859-dddd444", fake, 1);
        make_release(rel, L"TotK-PC-20261026-0100-eeee555", fake, 0);             /* no totk.exe */
        make_release(rel, L"TotK-PC-20261026-0200-ffff666 - no usar", fake, 1);   /* marked by hand */
        make_release(rel, L"TotK-PC-2026-copia", fake, 1);                       /* no stamp */
        swprintf(p, MAX_PATH, L"%ls\\Otra cosa", rel); mkdirs(p);
        swprintf(newer, MAX_PATH, L"%ls\\TotK-PC-20261025-0859-dddd444", rel);
        CHECK(newer_release(dir, la, MAX_PATH) && !wcscmp(dir, newer));
        swprintf(p, MAX_PATH, L"%ls\\TotK Launcher.exe", newer);
        CHECK(!wcscmp(la, p));
        swprintf(p, MAX_PATH, L"%ls\\TotK-PC-20261026-0900-gggg777", rel);
        make_release(p, L"TotK-PC-20261026-0900-gggg777", fake, 1);
        swprintf(nested, MAX_PATH, L"%ls\\TotK-PC-20261026-0900-gggg777", p);
        CHECK(newer_release(dir, la, MAX_PATH) && !wcscmp(dir, nested));
        swprintf(g_dir, MAX_PATH, L"%ls", newer);          /* from the 0859 one too */
        CHECK(newer_release(dir, la, MAX_PATH) && !wcscmp(dir, nested));
        swprintf(g_dir, MAX_PATH, L"%ls", nested);         /* the newest: nothing beside it */
        CHECK(!newer_release(dir, la, MAX_PATH));
        swprintf(p, MAX_PATH, L"%ls\\TotK-PC-20261025-0500-hhhh888", rel);   /* an older one nested too */
        make_release(p, L"TotK-PC-20261025-0500-hhhh888", fake, 1);
        swprintf(g_dir, MAX_PATH, L"%ls\\TotK-PC-20261025-0500-hhhh888", p);
        CHECK(newer_release(dir, la, MAX_PATH) && !wcscmp(dir, nested));
        swprintf(g_dir, MAX_PATH, L"%ls", g_root);         /* not a release folder */
        CHECK(!newer_release(dir, la, MAX_PATH));

        /* offer_newer_release: "Seguir con esta" is remembered for that folder; closing the dialog is not; "Abrir" starts
         * the newer launcher in its folder and this one exits; each choice is logged */
        swprintf(g_dir, MAX_PATH, L"%ls", own);
        g_ask_update = answer_update;
        g_answer = UPDATE_STAY; g_asked = 0;
        CHECK(offer_newer_release() == 0 && g_asked == 1);
        CHECK(log_has("the player stayed on") && log_has("gggg777: not asked again"));
        CHECK(offer_newer_release() == 0 && g_asked == 1);   /* not asked again */
        CHECK(log_has("declined before"));
        swprintf(p, MAX_PATH, L"%ls\\update-declined.txt", g_base); DeleteFileW(p);
        g_answer = IDCANCEL;
        CHECK(offer_newer_release() == 0 && g_asked == 2);
        CHECK(offer_newer_release() == 0 && g_asked == 3);   /* asked again */
        CHECK(log_has("dialog closed"));
        wchar_t mark[MAX_PATH]; swprintf(mark, MAX_PATH, L"%ls\\mark.txt", g_root);
        SetEnvironmentVariableW(L"FAKE_GAME", L"0,0");
        SetEnvironmentVariableW(L"FAKE_GAME_MARK", mark);
        g_answer = UPDATE_OPEN;
        CHECK(offer_newer_release() == 1 && g_asked == 4);
        CHECK(log_has("the player opened the newer release"));
        for (int i = 0; i < 50 && !exists(mark); i++) Sleep(100);
        char cwd[MAX_PATH] = "", want[MAX_PATH];
        FILE *mf = _wfopen(mark, L"r"); if (mf) { if (fgets(cwd, MAX_PATH, mf)) cwd[strcspn(cwd, "\r\n")] = 0; fclose(mf); }
        WideCharToMultiByte(CP_ACP, 0, nested, -1, want, MAX_PATH, NULL, NULL);
        CHECK(!_stricmp(cwd, want));                          /* the newer launcher ran in its own folder */
        SetEnvironmentVariableW(L"FAKE_GAME_MARK", NULL);
        SetEnvironmentVariableW(L"FAKE_GAME", NULL);
        g_ask_update = ask_update;
    }
    {   /* restarts: early and late are restarts, not failed starts; a loop stops at the cap*/
        g_messages_logged_only = 1;
        wchar_t pkg[MAX_PATH], why[768];
        swprintf(pkg, MAX_PATH, L"%ls\\rstr-early", g_root);
        make_package(pkg, fake, "0,0,rstr");
        g_early_ms = 3000;
        CHECK(play_package(pkg, L"early", why, 768) == QUIT);   /* RSTR at once, then the restarted game quits */
        CHECK(log_has("early asked for a restart within"));
        CHECK(log_has("early asked for a restart: starting it again"));
        CHECK(log_has("restart: FAKE_GAME_RESTARTED=1"));
        SetEnvironmentVariableW(L"FAKE_GAME_RESTARTED", NULL);
        swprintf(pkg, MAX_PATH, L"%ls\\rstr-late", g_root);
        make_package(pkg, fake, "0,600,rstr");
        g_early_ms = 100;
        CHECK(play_package(pkg, L"late", why, 768) == RUNNING);   /* running, RSTR, running again, exits 0 */
        CHECK(log_has("late exited (code 0x52535452)"));
        CHECK(log_has("late asked for a restart: starting it again"));
        SetEnvironmentVariableW(L"FAKE_GAME_RESTARTED", NULL);
        swprintf(pkg, MAX_PATH, L"%ls\\rstr-loop", g_root);
        make_package(pkg, fake, "1381192786,0");   /* RSTR at every start */
        g_early_ms = 3000;
        CHECK(play_package(pkg, L"loop", why, 768) == QUIT);
        CHECK(log_has("loop restarted 5 times within 60 s: not again"));
        /* a GPU that cannot run the game (device.c gpu_unsupported: its own box, code GPU0): no second message, no older
           package (early: QUIT, not FAILED), and none after a late one either */
        swprintf(pkg, MAX_PATH, L"%ls\\gpu-early", g_root);
        make_package(pkg, fake, "1196447024,0");   /* GPU_UNSUPPORTED_CODE */
        g_early_ms = 3000;
        CHECK(play_package(pkg, L"gpuearly", why, 768) == QUIT);
        CHECK(log_has("gpuearly: this GPU cannot run the game"));
        swprintf(pkg, MAX_PATH, L"%ls\\gpu-late", g_root);
        make_package(pkg, fake, "1196447024,600");
        g_early_ms = 100;
        CHECK(play_package(pkg, L"gpulate", why, 768) == RUNNING);
        CHECK(log_has("gpulate exited (code 0x47505530)"));
        CHECK(!log_has("0x47505530)."));   /* (the error message's "(code 0x...)." form) */
        g_messages_logged_only = 0;
    }
    {   /* mods_env: <g_base>\mods\<name>\romfs (or the folder itself) sorted by name, '_' / '.' off, exefs-only skipped */
        wchar_t m[MAX_PATH], v[4096], want[4096];
        CHECK(mods_env() == 0);   /* (creates the folder and its readme; no mods: TOTK_USER_MODS unset) */
        CHECK(GetEnvironmentVariableW(L"TOTK_USER_MODS", v, 4096) == 0);
        swprintf(m, MAX_PATH, L"%ls\\mods\\LEEME-README.txt", g_base); CHECK(exists(m));
        swprintf(m, MAX_PATH, L"%ls\\mods\\b-tree\\Actor", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\A-emu\\romfs\\Actor", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\_off\\romfs", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\code\\exefs", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\file.txt", g_base); touch(m);
        CHECK(mods_env() == 2);
        CHECK(GetEnvironmentVariableW(L"TOTK_USER_MODS", v, 4096) > 0);
        swprintf(want, 4096, L"%ls\\mods\\A-emu\\romfs;%ls\\mods\\b-tree", g_base, g_base);
        CHECK(!wcscmp(v, want));
        CHECK(log_has("mods: code skipped (exefs only"));
        /* a romfs mod's exefs is named in the log; a mod's own RSTB is logged (the game merges it: hle_fs.cpp), no dialog */
        g_messages_logged_only = 1;
        swprintf(m, MAX_PATH, L"%ls\\mods\\A-emu\\exefs", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\b-tree\\System\\Resource", g_base); mkdirs(m);
        swprintf(m, MAX_PATH, L"%ls\\mods\\b-tree\\System\\Resource\\ResourceSizeTable.Product.100.rsizetable.zs", g_base); touch(m);
        CHECK(mods_env() == 2);
        CHECK(log_has("mods: A-emu: its exefs folder is not used"));
        CHECK(log_has("mods: b-tree brings its own resource size table"));
        CHECK(!log_has("message: The mod") && !log_has("message: El mod"));
        g_messages_logged_only = 0;
    }
    {   /* a package's processor level: player.txt cpu= (package.py), none = x86-64-v3 (this test PC has both) */
        wchar_t pkg[MAX_PATH], p[MAX_PATH], why[768];
        CHECK(log_has("test: cpu level x86-64-v3"));   /* (the packages above have no cpu= line) */
        swprintf(pkg, MAX_PATH, L"%ls\\cpu-v2", g_root);
        make_package(pkg, fake, "0,0");
        swprintf(p, MAX_PATH, L"%ls\\player.txt", pkg);
        FILE *f = _wfopen(p, L"a"); fprintf(f, "cpu=x86-64-v2\n"); fclose(f);
        Sleep(1100);
        HANDLE game = NULL; FILETIME st;
        g_early_ms = 3000;
        CHECK(run_package(pkg, L"cpuv2", why, 768, &game, &st) == QUIT);
        CHECK(log_has("cpuv2: cpu level x86-64-v2"));
    }
    fclose(g_log);
    if (g_fail) { fprintf(stderr, "FAILED: launcher (%d)\n", g_fail); return 1; }
    printf("ok: launcher (0 failed)\n");
    return 0;
}
