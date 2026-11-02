/* Tests for autoupdate.c: GitHub's release JSON, update.json, version order, the rebuild question, the URL rule and
 * the sha256 (nxrecomp, installer). No network: setup-clean's fake release server covers the HTTP side. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include "autoupdate.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the shape of api.github.com's releases/latest (trimmed; v0.1.6's real digests) */
static const char *REL =
    "{\"url\":\"https://api.github.com/repos/o/r/releases/1\",\"id\":1,\"author\":{\"login\":\"x\",\"id\":2},"
    "\"tag_name\":\"v0.1.6\",\"name\":\"v0.1.6: shaders\",\"draft\":false,\"immutable\":true,\"prerelease\":false,"
    "\"body\":\"**Setup** \\\"quoted\\\" \\u00e9 and [a,b] {c}\",\"assets\":["
    "{\"name\":\"Weeping-of-the-Empire-Setup-v0.1.6.exe\",\"size\":93967360,"
    "\"digest\":\"sha256:6DBD878810B5451A82646B2C9543AFE849287EC8DE954F6F73926AE4D56E2C0D\",\"uploader\":{\"id\":3},"
    "\"browser_download_url\":\"https://github.com/o/r/releases/download/v0.1.6/Weeping-of-the-Empire-Setup-v0.1.6.exe\"},"
    "{\"name\":\"precompile.bin\",\"size\":4550776,\"digest\":null,\"browser_download_url\":\"https://x/p.bin\"}],"
    "\"reactions\":{\"+1\":1}}";

int main(void) {
    AuRelease r;
    CHECK(au_parse_release(REL, strlen(REL), &r));
    CHECK(!strcmp(r.tag, "v0.1.6") && r.immutable == 1 && r.draft == 0 && r.nassets == 2);
    const AuAsset *s = au_find_asset(&r, NULL, "-Setup-v0.1.6.exe");
    CHECK(s && s->size == 93967360 && !strcmp(s->sha256, "6dbd878810b5451a82646b2c9543afe849287ec8de954f6f73926ae4d56e2c0d"));
    CHECK(s && !strncmp(s->url, "https://github.com/o/r/releases/download/v0.1.6/", 48));
    const AuAsset *p = au_find_asset(&r, "precompile.bin", NULL);
    CHECK(p && !p->sha256[0]);   /* a null digest: none */
    CHECK(!au_find_asset(&r, "update.json", NULL));
    CHECK(!au_parse_release("{\"tag_name\":", 12, &r));            /* cut short */
    CHECK(!au_parse_release("<html>rate limit</html>", 23, &r));  /* not JSON */
    CHECK(!au_parse_release("{\"message\":\"Not Found\"}", 23, &r)); /* no tag */
    const char *mut = "{\"tag_name\":\"v0.1.7\",\"assets\":[]}";
    CHECK(au_parse_release(mut, strlen(mut), &r) && r.immutable == 0 && r.nassets == 0);

    AuUpdateJson u;
    const char *uj = "{\"tag\":\"v0.1.7\",\"same_key_as\":[\"v0.1.6\",\"v0.1.6.1\"],\"note\":{\"x\":[1,2]}}";
    CHECK(au_parse_update_json(uj, strlen(uj), &u) && u.nsame == 2 && !strcmp(u.same[1], "v0.1.6.1"));
    CHECK(!au_needs_rebuild(&u, 1, "v0.1.6.1"));
    CHECK(!au_needs_rebuild(&u, 1, "0.1.6"));
    CHECK(au_needs_rebuild(&u, 1, "v0.1.5"));
    CHECK(au_needs_rebuild(&u, 0, "v0.1.6"));   /* no update.json: assume a rebuild, ask */
    CHECK(au_needs_rebuild(&u, 1, ""));

    CHECK(au_version_cmp("v0.1.6", "v0.1.6.1") < 0);
    CHECK(au_version_cmp("v0.1.10", "v0.1.9") > 0);
    CHECK(au_version_cmp("v0.1.6", "0.1.6") == 0);
    CHECK(au_version_cmp("v0.2.0", "v0.1.99") > 0);
    CHECK(au_version_cmp("v0.1.6.0", "v0.1.6") == 0);

    CHECK(au_url_allowed(L"https://api.github.com/x"));
    CHECK(au_url_allowed(L"http://127.0.0.1:8765/repos"));
    CHECK(au_url_allowed(L"http://localhost/x"));
    CHECK(!au_url_allowed(L"http://example.com/x"));
    CHECK(!au_url_allowed(L"http://127.0.0.1.evil.com/x"));
    CHECK(!au_url_allowed(L"ftp://x"));

    wchar_t tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(file, MAX_PATH, L"%lsau-test-%lu.txt", tmp, GetCurrentProcessId());
    FILE *f = _wfopen(file, L"wb");
    fputs("abc", f);
    fclose(f);
    char hex[65];
    CHECK(au_sha256_file(file, hex) && !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    {   /* locked: no writer and no delete while the handle is open; the hash read through it */
        HANDLE h = au_lock_file(file);
        CHECK(h != INVALID_HANDLE_VALUE);
        char hx[65] = "";
        CHECK(au_sha256_handle(h, hx) && !strcmp(hx, hex));
        HANDLE w = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(w == INVALID_HANDLE_VALUE);
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        CHECK(!DeleteFileW(file));
        CloseHandle(h);
    }
    {   /* an exe locked that way still starts (the launcher holds the lock across CreateProcess) */
        wchar_t exe[MAX_PATH], sys[MAX_PATH], cmd[MAX_PATH + 8];
        GetSystemDirectoryW(sys, MAX_PATH);
        swprintf(cmd, MAX_PATH, L"%ls\\whoami.exe", sys);
        swprintf(exe, MAX_PATH, L"%lsau-test-%lu.exe", tmp, GetCurrentProcessId());
        CHECK(CopyFileW(cmd, exe, FALSE));
        HANDLE h = au_lock_file(exe);
        swprintf(cmd, MAX_PATH + 8, L"\"%ls\"", exe);
        STARTUPINFOW si = {0}; si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        CHECK(ok);
        if (ok) { WaitForSingleObject(pi.hProcess, 10000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        CloseHandle(h);
        DeleteFileW(exe);
    }
    DeleteFileW(file);
    CHECK(!au_sha256_file(file, hex));

    printf(fails ? "FAILED: autoupdate (%d)\n" : "ok: autoupdate (%d failed)\n", fails);
    return fails != 0;
}
