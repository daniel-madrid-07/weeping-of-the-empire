/* The launcher's automatic update: what the latest GitHub release is, whether it is newer than the installed build and
 * whether taking it rebuilds the whole game, its Setup downloaded and checked against GitHub's own sha256 digest of the
 * asset. No UI here: launcher.c asks and shows progress; test_autoupdate.c checks the parts.
 *
 * The installed build: player.txt release=<tag> (package.py, from the source's tag; a build with none, never updates
 * itself) and key=<engine key>. The release, or NXR_UPDATE_API: http only for 127.0.0.1 / localhost, the tests' fake
 * server) -> tag_name, immutable, assets[] name / browser_download_url / digest ("sha256:<hex>") / size. Its
 * update.json asset {"tag": "v0.1.7", "same_key_as": ["v0.1.6",...]} says which earlier releases build the same game
 * key (taking it from one of those only relinks the runtime: minutes); without one, or from any other release, the
 * update recompiles the game (15-45 min) and the player is asked first.*/
#pragma once
#include <windows.h>
#include <wchar.h>

#define AU_MAX_ASSETS 16
#define AU_MAX_SAME 32

typedef struct AuAsset {
    char name[128];
    char url[512];
    char sha256[65];     /* "" when GitHub gave no digest */
    long long size;
} AuAsset;

typedef struct AuRelease {
    char tag[48];
    int immutable;       /* 1: GitHub's immutable release (its assets cannot be replaced) */
    int draft, prerelease;
    int nassets;
    AuAsset assets[AU_MAX_ASSETS];
} AuRelease;

typedef struct AuUpdateJson {
    char tag[48];
    int nsame;
    char same[AU_MAX_SAME][48];   /* earlier tags whose builds have this release's game key */
} AuUpdateJson;

/* 1 when json is a GitHub release object with a tag_name */
int au_parse_release(const char *json, size_t len, AuRelease *r);
/* 1 when json is an update.json */
int au_parse_update_json(const char *json, size_t len, AuUpdateJson *u);
/* <0, 0, >0 like strcmp, numerically on "v1.2.3[.4]" ("v" optional; a non-number part compares as 0) */
int au_version_cmp(const char *a, const char *b);
/* the asset whose name is exactly name, or that ends with suffix (name NULL); NULL when none */
const AuAsset *au_find_asset(const AuRelease *r, const char *name, const char *suffix);
/* the update recompiles the game: no update.json, or the installed tag is not among its same_key_as */
int au_needs_rebuild(const AuUpdateJson *u, int have_update_json, const char *installed_tag);
/* 1 when url may be used: https, or http to 127.0.0.1 / localhost (the tests) */
int au_url_allowed(const wchar_t *url);

/* HTTP GET (WinHTTP; redirects followed, e.g. GitHub's 302 to its asset host) into a malloc'd buffer (*out, *len) or
 * a file (path; progress(done, total, ctx) as it goes, total -1 when unknown; a nonzero return cancels). Returns the
 * HTTP status (200 ...), 0 when the connection failed or timed out (errtext says), -1 cancelled. */
int au_http_get(const wchar_t *url, DWORD timeout_ms, char **out, size_t *len, wchar_t *errtext, size_t errn);
int au_http_download(const wchar_t *url, const wchar_t *path, DWORD timeout_ms,
                     int (*progress)(long long done, long long total, void *ctx), void *ctx, wchar_t *errtext, size_t errn);
/* the file's sha256 as 64 lowercase hex digits; 0 when it cannot be read */
int au_sha256_file(const wchar_t *path, char hex[65]);
/* the file opened for reading with read-only sharing: nobody can write, replace or delete it while the handle is
 * open; INVALID_HANDLE_VALUE when it cannot*/
HANDLE au_lock_file(const wchar_t *path);
/* the sha256 of what the handle reads, from its start */
int au_sha256_handle(HANDLE h, char hex[65]);
