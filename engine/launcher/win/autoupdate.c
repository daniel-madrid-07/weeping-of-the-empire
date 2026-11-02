/* The launcher's automatic update: release JSON, version order, the rebuild question, HTTP and the sha256 check
 * (autoupdate.h). Plain C: WinHTTP for the downloads, BCrypt for the hash. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>

#include "autoupdate.h"

/* ------------------------------------------------------------------ JSON (just enough for GitHub's release object) */

typedef struct { const char *p, *end; } J;

static void jws(J *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++; }
static int jpeek(J *j) { jws(j); return j->p < j->end ? *j->p : 0; }

/* a string into out (truncated to n-1; \uXXXX outside ASCII becomes '?'); 1 on success */
static int jstr(J *j, char *out, size_t n) {
    if (jpeek(j) != '"') return 0;
    j->p++;
    size_t k = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\') {
            if (j->p >= j->end) return 0;
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    if (j->p >= j->end || !isxdigit((unsigned char)*j->p)) return 0;
                    char h = *j->p++;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                }
                c = v < 0x80 ? (char)v : '?';
                break;
            }
            default: c = e;   /* \" \\ \/ */
            }
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (j->p >= j->end) return 0;
    j->p++;
    if (out && n) out[k] = 0;
    return 1;
}

static int jskip(J *j);

static int jskip_container(J *j, char open, char close) {
    j->p++;   /* open */
    if (jpeek(j) == close) { j->p++; return 1; }
    for (;;) {
        if (open == '{') {
            if (!jstr(j, NULL, 0)) return 0;
            if (jpeek(j) != ':') return 0;
            j->p++;
        }
        if (!jskip(j)) return 0;
        int c = jpeek(j);
        if (c == ',') { j->p++; continue; }
        if (c == close) { j->p++; return 1; }
        return 0;
    }
}

/* any value */
static int jskip(J *j) {
    int c = jpeek(j);
    if (c == '"') return jstr(j, NULL, 0);
    if (c == '{') return jskip_container(j, '{', '}');
    if (c == '[') return jskip_container(j, '[', ']');
    if (!c) return 0;
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n' && *j->p != '\r' &&
           *j->p != '\t')
        j->p++;   /* a number, true, false, null */
    return 1;
}

/* true / false / a number into *v (true = 1); other values are skipped and leave *v as it was */
static int jscalar(J *j, long long *v) {
    int c = jpeek(j);
    if (c == 't' && j->end - j->p >= 4 && !strncmp(j->p, "true", 4)) { j->p += 4; *v = 1; return 1; }
    if (c == 'f' && j->end - j->p >= 5 && !strncmp(j->p, "false", 5)) { j->p += 5; *v = 0; return 1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[32]; size_t k = 0;
        while (j->p < j->end && k < sizeof buf - 1 && strchr("-+.eE0123456789", *j->p)) buf[k++] = *j->p++;
        buf[k] = 0;
        *v = _strtoi64(buf, NULL, 10);
        return 1;
    }
    return jskip(j);
}

/* walks an object: for each key calls f(key, j, ctx), which must consume the value (return 0 = malformed) */
static int jobject(J *j, int (*f)(const char *key, J *j, void *ctx), void *ctx) {
    if (jpeek(j) != '{') return 0;
    j->p++;
    if (jpeek(j) == '}') { j->p++; return 1; }
    for (;;) {
        char key[64];
        if (!jstr(j, key, sizeof key)) return 0;
        if (jpeek(j) != ':') return 0;
        j->p++;
        if (!f(key, j, ctx)) return 0;
        int c = jpeek(j);
        if (c == ',') { j->p++; continue; }
        if (c == '}') { j->p++; return 1; }
        return 0;
    }
}

static int asset_field(const char *key, J *j, void *ctx) {
    AuAsset *a = ctx;
    if (!strcmp(key, "name")) return jpeek(j) == '"' ? jstr(j, a->name, sizeof a->name) : jskip(j);
    if (!strcmp(key, "browser_download_url")) return jpeek(j) == '"' ? jstr(j, a->url, sizeof a->url) : jskip(j);
    if (!strcmp(key, "digest")) {
        char d[96] = "";
        if (jpeek(j) != '"') return jskip(j);   /* null: no digest */
        if (!jstr(j, d, sizeof d)) return 0;
        if (!_strnicmp(d, "sha256:", 7) && strlen(d + 7) == 64) {
            for (int i = 0; i < 64; i++) a->sha256[i] = (char)tolower((unsigned char)d[7 + i]);
            a->sha256[64] = 0;
        }
        return 1;
    }
    if (!strcmp(key, "size")) return jscalar(j, &a->size);
    return jskip(j);
}

static int release_field(const char *key, J *j, void *ctx) {
    AuRelease *r = ctx;
    long long v = 0;
    if (!strcmp(key, "tag_name")) return jpeek(j) == '"' ? jstr(j, r->tag, sizeof r->tag) : jskip(j);
    if (!strcmp(key, "immutable")) { if (!jscalar(j, &v)) return 0; r->immutable = v == 1; return 1; }
    if (!strcmp(key, "draft")) { if (!jscalar(j, &v)) return 0; r->draft = v == 1; return 1; }
    if (!strcmp(key, "prerelease")) { if (!jscalar(j, &v)) return 0; r->prerelease = v == 1; return 1; }
    if (!strcmp(key, "assets")) {
        if (jpeek(j) != '[') return jskip(j);
        j->p++;
        if (jpeek(j) == ']') { j->p++; return 1; }
        for (;;) {
            AuAsset tmp = {0};
            if (!jobject(j, asset_field, &tmp)) return 0;
            if (r->nassets < AU_MAX_ASSETS && tmp.name[0]) r->assets[r->nassets++] = tmp;
            int c = jpeek(j);
            if (c == ',') { j->p++; continue; }
            if (c == ']') { j->p++; return 1; }
            return 0;
        }
    }
    return jskip(j);
}

int au_parse_release(const char *json, size_t len, AuRelease *r) {
    memset(r, 0, sizeof *r);
    J j = {json, json + len};
    return jobject(&j, release_field, r) && r->tag[0];
}

static int update_field(const char *key, J *j, void *ctx) {
    AuUpdateJson *u = ctx;
    if (!strcmp(key, "tag")) return jpeek(j) == '"' ? jstr(j, u->tag, sizeof u->tag) : jskip(j);
    if (!strcmp(key, "same_key_as")) {
        if (jpeek(j) != '[') return jskip(j);
        j->p++;
        if (jpeek(j) == ']') { j->p++; return 1; }
        for (;;) {
            char t[48] = "";
            if (jpeek(j) == '"') { if (!jstr(j, t, sizeof t)) return 0; }
            else if (!jskip(j)) return 0;
            if (t[0] && u->nsame < AU_MAX_SAME) strcpy(u->same[u->nsame++], t);
            int c = jpeek(j);
            if (c == ',') { j->p++; continue; }
            if (c == ']') { j->p++; return 1; }
            return 0;
        }
    }
    return jskip(j);
}

int au_parse_update_json(const char *json, size_t len, AuUpdateJson *u) {
    memset(u, 0, sizeof *u);
    J j = {json, json + len};
    return jobject(&j, update_field, u) && u->tag[0];
}

/* ------------------------------------------------------------------ versions, assets, decisions */

int au_version_cmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        while (*a && *a != '.') a++;   /* (a suffix such as "-rc1" counts as nothing) */
        while (*b && *b != '.') b++;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

const AuAsset *au_find_asset(const AuRelease *r, const char *name, const char *suffix) {
    for (int i = 0; i < r->nassets; i++) {
        const char *n = r->assets[i].name;
        if (name && !_stricmp(n, name)) return &r->assets[i];
        if (!name && suffix) {
            size_t ln = strlen(n), ls = strlen(suffix);
            if (ln >= ls && !_stricmp(n + ln - ls, suffix)) return &r->assets[i];
        }
    }
    return NULL;
}

int au_needs_rebuild(const AuUpdateJson *u, int have_update_json, const char *installed_tag) {
    if (!have_update_json || !installed_tag || !installed_tag[0]) return 1;
    for (int i = 0; i < u->nsame; i++)
        if (!au_version_cmp(u->same[i], installed_tag)) return 0;
    return 1;
}

int au_url_allowed(const wchar_t *url) {
    if (!_wcsnicmp(url, L"https://", 8)) return 1;
    if (_wcsnicmp(url, L"http://", 7)) return 0;
    const wchar_t *h = url + 7;
    size_t n = wcscspn(h, L":/");
    return (n == 9 && !_wcsnicmp(h, L"127.0.0.1", 9)) || (n == 9 && !_wcsnicmp(h, L"localhost", 9));
}

/* ------------------------------------------------------------------ HTTP */

static void err(wchar_t *out, size_t n, const wchar_t *what, DWORD e) {
    if (out && n) swprintf(out, n, L"%ls (error %lu)", what, e);
}

typedef struct { HINTERNET s, c, r; } Http;

static void http_close(Http *h) {
    if (h->r) WinHttpCloseHandle(h->r);
    if (h->c) WinHttpCloseHandle(h->c);
    if (h->s) WinHttpCloseHandle(h->s);
    memset(h, 0, sizeof *h);
}

/* the request sent and its headers read: the HTTP status, or 0 (errtext says why) */
static int http_open(Http *h, const wchar_t *url, DWORD timeout_ms, long long *length, wchar_t *errtext, size_t errn) {
    memset(h, 0, sizeof *h);
    if (!au_url_allowed(url)) { if (errtext) swprintf(errtext, errn, L"not an https address: %ls", url); return 0; }
    URL_COMPONENTS u = {0};
    wchar_t host[256], path[2048];
    u.dwStructSize = sizeof u;
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    wchar_t extra[1024]; u.lpszExtraInfo = extra; u.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) { err(errtext, errn, L"bad address", GetLastError()); return 0; }
    if (u.dwExtraInfoLength) wcsncat(path, extra, 2048 - wcslen(path) - 1);
    h->s = WinHttpOpen(L"nxrecomp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->s)   /* (before Windows 8.1 there is no automatic proxy) */
        h->s = WinHttpOpen(L"nxrecomp-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->s) { err(errtext, errn, L"WinHttpOpen", GetLastError()); return 0; }
    WinHttpSetTimeouts(h->s, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
    h->c = WinHttpConnect(h->s, host, u.nPort, 0);
    if (!h->c) { err(errtext, errn, L"connect", GetLastError()); http_close(h); return 0; }
    h->r = WinHttpOpenRequest(h->c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!h->r) { err(errtext, errn, L"request", GetLastError()); http_close(h); return 0; }
    const wchar_t *hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(h->r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h->r, NULL)) {
        DWORD e = GetLastError();
        err(errtext, errn, e == ERROR_WINHTTP_TIMEOUT ? L"timed out" : L"no connection", e);
        http_close(h);
        return 0;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(h->r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (length) {
        wchar_t cl[32]; DWORD cls = sizeof cl;
        *length = WinHttpQueryHeaders(h->r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cls,
                                      WINHTTP_NO_HEADER_INDEX) ? _wtoi64(cl) : -1;
    }
    return (int)status;
}

int au_http_get(const wchar_t *url, DWORD timeout_ms, char **out, size_t *len, wchar_t *errtext, size_t errn) {
    Http h;
    *out = NULL; *len = 0;
    int status = http_open(&h, url, timeout_ms, NULL, errtext, errn);
    if (!status) return 0;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(h.r, &avail)) { err(errtext, errn, L"read", GetLastError()); free(buf); http_close(&h); return 0; }
        if (!avail) break;
        if (n + avail > (size_t)8 << 20) { if (errtext) swprintf(errtext, errn, L"answer too big"); free(buf); http_close(&h); return 0; }
        if (n + avail > cap) { while (n + avail > cap) cap *= 2; buf = realloc(buf, cap + 1); }
        if (!WinHttpReadData(h.r, buf + n, avail, &got)) { err(errtext, errn, L"read", GetLastError()); free(buf); http_close(&h); return 0; }
        n += got;
    }
    buf[n] = 0;
    http_close(&h);
    *out = buf; *len = n;
    return status;
}

int au_http_download(const wchar_t *url, const wchar_t *path, DWORD timeout_ms,
                     int (*progress)(long long done, long long total, void *ctx), void *ctx, wchar_t *errtext, size_t errn) {
    Http h;
    long long total = -1, done = 0;
    int status = http_open(&h, url, timeout_ms, &total, errtext, errn);
    if (status != 200) { if (status && errtext) swprintf(errtext, errn, L"HTTP %d", status); http_close(&h); return status; }
    FILE *f = _wfopen(path, L"wb");
    if (!f) { err(errtext, errn, L"cannot write the download", GetLastError()); http_close(&h); return 0; }
    static char buf[1 << 16];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(h.r, buf, sizeof buf, &got)) { err(errtext, errn, L"read", GetLastError()); status = 0; break; }
        if (!got) break;
        if (fwrite(buf, 1, got, f) != got) { err(errtext, errn, L"disk full?", GetLastError()); status = 0; break; }
        done += got;
        if (progress && progress(done, total, ctx)) { status = -1; break; }
    }
    fclose(f);
    http_close(&h);
    if (status == 200 && total >= 0 && done != total) {
        if (errtext) swprintf(errtext, errn, L"cut short: %lld of %lld bytes", done, total);
        status = 0;
    }
    if (status != 200) DeleteFileW(path);
    return status;
}

/* ------------------------------------------------------------------ sha256 */

int au_sha256_handle(HANDLE h, char hex[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    UCHAR digest[32];
    int ok = 0;
    LARGE_INTEGER zero = {0};
    if (h == INVALID_HANDLE_VALUE || !SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        static UCHAR buf[1 << 16];
        DWORD n;
        ok = 1;
        while (ReadFile(h, buf, sizeof buf, &n, NULL) && n > 0)
            if (BCryptHashData(hash, buf, n, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(hash, digest, 32, 0) != 0) ok = 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (ok) for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    return ok;
}

HANDLE au_lock_file(const wchar_t *path) {
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

int au_sha256_file(const wchar_t *path, char hex[65]) {
    HANDLE h = au_lock_file(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = au_sha256_handle(h, hex);
    CloseHandle(h);
    return ok;
}
