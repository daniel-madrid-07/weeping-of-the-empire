/* Tests for savever.c on synthetic saves (no game files): a later-version file down to 1.0.0's layout, each refusal,
 * the caption picture through WIC, a folder all-or-nothing, and the import through saves_replace_report. Built and
 * run by build.py test.
 *
 *   test_savever.exe                 the tests
 *   test_savever.exe convert <dir>   convert a save folder in place with the real tables and print the report (the*/
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include "savever.h"
#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ------------------------------------------------------------------------------------------------ synthetic tables */

enum { REV_OLD = 100, REV_NEW = 200, FSIZE = 4096, ROOM = 1000, PIC_W = 16, PIC_H = 9 };
static const SaveKey drop_p[] = {{0, 0x13}, {1, 0x1a}, {22, 0x2a}};
static const SaveKey drop_o[] = {{0, 0x14}};
static const uint64_t drop64[] = {0x0bad0000000064ull};
static const uint32_t later_enum[] = {0x6f, 0x7f};
static const uint32_t later_revs[] = {REV_NEW};
static const SaveFileSpec files[] = {
    /* 1.0.0 keys: Bool 0x11 0x12, Int 0x21, Enum 0x61, EnumArray 0x71, Binary 0x181, Bool64bitKey 0x320 (savever_gen.py
     * digest: 0x6c6d471dd086cd69) */
    {0, L"progress.sav", FSIZE, 7, 0x6c6d471dd086cd69ull, drop_p, 3},
    {1, L"option.sav", 512, 2, 0x98f778fadd8596c5ull, drop_o, 1},   /* Bool 0x11, Bool64bitKey 0x320 */
};
static const SaveEnumMap enums[] = {{0, 6, 0x61, 0x60, later_enum, 2}, {0, 7, 0x71, 0x70, later_enum, 2}};
static const SaveBinRoom bins[] = {{0, 0x181, ROOM}};
static const SaveTables T = {REV_OLD, later_revs, 1, files, 2, drop64, 1, enums, 2, bins, 1, PIC_W, PIC_H};

/* ------------------------------------------------------------------------------------------------ synthetic saves */

typedef struct Item { uint32_t group, hash, value; const void *blob; uint32_t len; } Item;

/* A save of size bytes: the table (every group's marker, the items in their group), then the items' blocks in item
 * order. Returns the end of the data. */
static size_t build(uint8_t *buf, size_t size, uint32_t rev, const Item *it, int n) {
    memset(buf, 0, size);
    size_t rows = 33;
    for (int i = 0; i < n; i++) rows++;
    size_t start = 0x20 + 8 * rows, pos = start, row = 0;
    uint32_t offs[64];
    for (int i = 0; i < n; i++) {
        offs[i] = it[i].blob ? (uint32_t)pos : it[i].value;
        if (it[i].blob) { memcpy(buf + pos, it[i].blob, it[i].len); pos += it[i].len; }
    }
    for (uint32_t g = 0; g < 33; g++) {
        wr32(buf + 0x20 + 8 * row, 0), wr32(buf + 0x24 + 8 * row, g), row++;
        for (int i = 0; i < n; i++)
            if (it[i].group == g) wr32(buf + 0x20 + 8 * row, it[i].hash), wr32(buf + 0x24 + 8 * row, offs[i]), row++;
    }
    wr32(buf, 0x01020304);
    wr32(buf + 4, rev);
    wr32(buf + 8, (uint32_t)start);
    return pos;
}

/* The value (or block offset) of (group, hash) in a save; -1 when absent. */
static int64_t find(const uint8_t *s, uint32_t group, uint32_t hash) {
    uint32_t start = rd32(s + 8), g = 0;
    for (uint32_t o = 0x20; o < start; o += 8) {
        uint32_t k = rd32(s + o), v = rd32(s + o + 4);
        if (!k) g = v;
        else if (g == group && k == hash) return v;
    }
    return -1;
}

static uint8_t g_in[FSIZE + 1024], g_out[FSIZE];
static uint8_t g_barr[8] = {30, 0, 0, 0, 0xff, 0xff, 0xff, 0x3f};      /* BoolArray of 30 */
static uint8_t g_earr[16] = {3, 0, 0, 0, 0x70, 0, 0, 0, 0x7f, 0, 0, 0, 0x72, 0, 0, 0};   /* EnumArray: 0x70 0x7f 0x72 */
static uint8_t g_i64[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static uint8_t g_b64[48];   /* keys 0x101, (later), 0x102, end, stale 0x103 */
static uint8_t g_bin[4 + 40];

static int later_save(uint8_t *buf, size_t size, int extra_key) {
    memset(g_b64, 0, sizeof g_b64);
    uint64_t k[] = {0x101, drop64[0], 0x102, 0, 0x103};
    for (int i = 0; i < 5; i++) memcpy(g_b64 + 8 * i, &k[i], 8);
    wr32(g_bin, 40);
    for (int i = 0; i < 40; i++) g_bin[4 + i] = (uint8_t)(i + 1);
    Item it[] = {
        {0, 0x11, 1, 0, 0}, {0, 0x13, 1, 0, 0}, {0, 0x12, 0, 0, 0}, {1, 0x1a, 0, g_barr, sizeof g_barr},
        {2, 0x21, 0xfffffff6u, 0, 0}, {6, 0x61, 0x6f, 0, 0}, {7, 0x71, 0, g_earr, sizeof g_earr},
        {22, 0x2a, 0, g_i64, sizeof g_i64}, {18, 0x181, 0, g_bin, sizeof g_bin}, {32, 0x320, 0, g_b64, sizeof g_b64},
        {0, 0x55, 1, 0, 0},
    };
    return (int)build(buf, size, REV_NEW, it, extra_key ? 11 : 10);
}

static void test_convert(void) {
    SaveConvReport r = {0};
    later_save(g_in, FSIZE + 1024, 0);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_CONVERTED);
    CHECK(rd32(g_out) == 0x01020304 && rd32(g_out + 4) == REV_OLD);
    CHECK(r.keys_dropped == 3 && r.flags_dropped == 1 && r.enums_reset == 2 && r.from == REV_NEW && r.files == 1);
    CHECK(rd32(g_out + 8) == 0x20 + 8 * (33 + 7));   /* the table lost the 3 dropped rows */
    CHECK(find(g_out, 0, 0x11) == 1 && find(g_out, 0, 0x12) == 0 && find(g_out, 0, 0x13) < 0 && find(g_out, 1, 0x1a) < 0);
    CHECK(find(g_out, 2, 0x21) == 0xfffffff6u && find(g_out, 22, 0x2a) < 0);
    CHECK(find(g_out, 6, 0x61) == 0x60);   /* a later value: the default */
    /* the blocks follow the table without gaps, in their order: EnumArray, Binary, Bool64bitKey */
    int64_t ea = find(g_out, 7, 0x71), bin = find(g_out, 18, 0x181), b64 = find(g_out, 32, 0x320);
    CHECK(ea == rd32(g_out + 8) && bin == ea + 16 && b64 == bin + 4 + ROOM);
    if (ea > 0 && bin > 0 && b64 > 0 && b64 + 24 <= FSIZE) {
        CHECK(rd32(g_out + ea) == 3 && rd32(g_out + ea + 4) == 0x70 && rd32(g_out + ea + 8) == 0x70 && rd32(g_out + ea + 12) == 0x72);
        CHECK(rd32(g_out + bin) == 40 && !memcmp(g_out + bin + 4, g_bin + 4, 40) && g_out[bin + 4 + 40] == 0);
        CHECK(rd32(g_out + b64) == 0x101 && rd32(g_out + b64 + 8) == 0x102 && rd32(g_out + b64 + 16) == 0 &&
              rd32(g_out + b64 + 24) == 0);   /* the later flag dropped, the stale key past the end gone */
        int zero = 1;
        for (size_t i = (size_t)b64 + 24; i < FSIZE; i++) zero &= !g_out[i];
        CHECK(zero);
    }
    /* converting the result again: it is 1.0.0 now */
    SaveConvReport r2 = {0};
    static uint8_t again[FSIZE];
    CHECK(savever_convert(&T, &files[0], g_out, FSIZE, again, &r2) == SV_SAME);
    /* option.sav: a dropped key, and Bool64bitKey offset 0 (the file has no list) stays 0 */
    static uint8_t opt[700], oout[512];
    Item o[] = {{0, 0x11, 1, 0, 0}, {0, 0x14, 1, 0, 0}, {32, 0x320, 0, 0, 0}};
    build(opt, sizeof opt, REV_NEW, o, 3);
    memset(&r2, 0, sizeof r2);
    CHECK(savever_convert(&T, &files[1], opt, sizeof opt, oout, &r2) == SV_CONVERTED && r2.keys_dropped == 1);
    CHECK(find(oout, 0, 0x11) == 1 && find(oout, 0, 0x14) < 0 && find(oout, 32, 0x320) == 0 && rd32(oout + 8) == 0x20 + 8 * 35);
}

static void test_refusals(void) {
    SaveConvReport r = {0};
    /* a key neither 1.0.0 nor the tables know */
    later_save(g_in, FSIZE + 1024, 1);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_UNKNOWN_KEYS && r.refused == SV_UNKNOWN_KEYS && r.detail == 1);
    /* an unknown revision */
    memset(&r, 0, sizeof r);
    later_save(g_in, FSIZE + 1024, 0);
    wr32(g_in + 4, 300);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_UNKNOWN_VERSION && r.detail == 300);
    /* not a save at all: left alone */
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], (const uint8_t *)"hello world, not a save", 24, g_out, &r) == SV_SAME);
    /* a damaged table: markers out of order, a block offset past the end, two keys on one block, no flag list end */
    later_save(g_in, FSIZE + 1024, 0);
    uint32_t start = rd32(g_in + 8);
    for (uint32_t o = 0x20; o < start; o += 8)
        if (!rd32(g_in + o) && rd32(g_in + o + 4) == 2) wr32(g_in + o + 4, 1);   /* group 1 twice */
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_BAD_FILE);
    later_save(g_in, FSIZE + 1024, 0);
    int64_t ea = find(g_in, 7, 0x71);
    for (uint32_t o = 0x20; o < start; o += 8)
        if (rd32(g_in + o) == 0x71) wr32(g_in + o + 4, FSIZE + 1024);
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_BAD_FILE);
    later_save(g_in, FSIZE + 1024, 0);
    for (uint32_t o = 0x20; o < start; o += 8)
        if (rd32(g_in + o) == 0x181) wr32(g_in + o + 4, (uint32_t)ea);
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_BAD_FILE);
    later_save(g_in, FSIZE + 1024, 0);
    int64_t b64 = find(g_in, 32, 0x320);
    memset(g_in + b64, 0xab, FSIZE + 1024 - (size_t)b64);   /* flags to the end of the file, no zero key */
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_BAD_FILE);
    /* more data than 1.0.0's file has room for */
    later_save(g_in, FSIZE + 1024, 0);
    b64 = find(g_in, 32, 0x320);
    memset(g_in + b64 + 8, 0x11, FSIZE + 1024 - 24 - (size_t)b64);   /* a flag list longer than the 1.0.0 file */
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], g_in, FSIZE + 1024, g_out, &r) == SV_TOO_BIG);
}

/* The picture: a JPEG over the room is re-encoded at PIC_W x PIC_H to fit; one WIC cannot read becomes black. */
static void test_picture(void) {
    static uint8_t big[1 << 16], fitted[ROOM];
    size_t nbig = 0, nfit = 0;
    CHECK(savever_fit_jpeg(NULL, 0, 1920, 1080, big, sizeof big, &nbig) == 0 && nbig > ROOM);   /* a 1920x1080 JPEG */
    CHECK(savever_fit_jpeg(big, nbig, PIC_W, PIC_H, fitted, ROOM, &nfit) == 0 && nfit > 0 && nfit <= ROOM);
    CHECK(fitted[0] == 0xff && fitted[1] == 0xd8);
    static uint8_t blob[4 + sizeof big];
    wr32(blob, (uint32_t)nbig);
    memcpy(blob + 4, big, nbig);
    Item it[] = {{0, 0x11, 1, 0, 0}, {0, 0x12, 0, 0, 0}, {2, 0x21, 0, 0, 0}, {6, 0x61, 0x60, 0, 0},
                 {7, 0x71, 0, g_earr, sizeof g_earr}, {18, 0x181, 0, blob, 4 + (uint32_t)nbig}, {32, 0x320, 0, g_b64, 8}};
    static uint8_t in[FSIZE + sizeof blob];
    memset(g_b64, 0, sizeof g_b64);
    build(in, sizeof in, REV_NEW, it, 7);
    SaveConvReport r = {0};
    CHECK(savever_convert(&T, &files[0], in, sizeof in, g_out, &r) == SV_CONVERTED && r.pictures_fitted == 1);
    int64_t bin = find(g_out, 18, 0x181);
    CHECK(bin > 0 && rd32(g_out + bin) <= ROOM && g_out[bin + 4] == 0xff && g_out[bin + 5] == 0xd8);
    /* garbage where the JPEG should be */
    memset(blob + 4, 0x5a, nbig);
    build(in, sizeof in, REV_NEW, it, 7);
    memset(&r, 0, sizeof r);
    CHECK(savever_convert(&T, &files[0], in, sizeof in, g_out, &r) == SV_CONVERTED && r.pictures_blank == 1 && r.pictures_fitted == 0);
}

/* ------------------------------------------------------------------------------------------------ folders */

static wchar_t g_root[MAX_PATH];

static void put(const wchar_t *rel, const void *data, size_t n) {
    wchar_t p[MAX_PATH], d[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", g_root, rel);
    swprintf(d, MAX_PATH, L"%ls", p);
    for (wchar_t *q = d; *q; q++)
        if (*q == L'\\' && q > d + 3) { *q = 0; CreateDirectoryW(d, NULL); *q = L'\\'; }
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WriteFile(h, data, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

static size_t get(const wchar_t *rel, uint8_t *buf, size_t cap) {
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", g_root, rel);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(h, buf, (DWORD)cap, &got, NULL);
    CloseHandle(h);
    return got;
}

static void test_dir(void) {
    static uint8_t a[FSIZE + 1024], b[FSIZE + 1024], opt[600], back[FSIZE + 1024];
    wchar_t dir[MAX_PATH];
    later_save(a, sizeof a, 0);
    later_save(b, sizeof b, 1);   /* slot_01: a key nobody knows */
    Item o[] = {{0, 0x11, 1, 0, 0}, {0, 0x14, 1, 0, 0}, {0, 0x99, 1, 0, 0}, {32, 0x320, 0, g_b64 + 24, 8}};
    memset(g_b64 + 24, 0, 8);
    build(opt, sizeof opt, REV_NEW, o, 4);   /* option.sav with a key nobody knows: left out */
    put(L"dir\\slot_00\\progress.sav", a, sizeof a);
    put(L"dir\\slot_01\\progress.sav", b, sizeof b);
    put(L"dir\\option.sav", opt, sizeof opt);
    put(L"dir\\slot_00\\notes.txt", "x", 1);
    swprintf(dir, MAX_PATH, L"%ls\\dir", g_root);
    SaveConvReport r = {0};
    CHECK(savever_convert_dir(&T, dir, &r) == SV_UNKNOWN_KEYS && !wcscmp(r.file, L"slot_01\\progress.sav"));
    CHECK(get(L"dir\\slot_00\\progress.sav", back, sizeof back) == sizeof a && !memcmp(back, a, sizeof a));   /* none written */
    CHECK(get(L"dir\\option.sav", back, sizeof back) == sizeof opt);
    CHECK(savever_dir_revision(dir) == REV_NEW);
    put(L"dir\\slot_01\\progress.sav", a, sizeof a);
    memset(&r, 0, sizeof r);
    CHECK(savever_convert_dir(&T, dir, &r) == SV_CONVERTED && r.files == 2 && r.option_dropped == 1 && !r.refused && r.from == REV_NEW);
    CHECK(get(L"dir\\slot_01\\progress.sav", back, sizeof back) == FSIZE && rd32(back + 4) == REV_OLD);
    CHECK(get(L"dir\\option.sav", back, sizeof back) == 0);
    CHECK(savever_dir_revision(dir) == REV_OLD);
    memset(&r, 0, sizeof r);
    CHECK(savever_convert_dir(&T, dir, &r) == SV_SAME && r.files == 0);
}

/* The import (saves_replace_report, the real tables): a refused save leaves the current one and the source as they
 * were (step 4); a folder of files that are not later-version saves goes in unchanged. */
static void test_import(void) {
    static uint8_t a[FSIZE + 1024], back[FSIZE + 1024];
    wchar_t base[MAX_PATH], src[MAX_PATH];
    swprintf(base, MAX_PATH, L"%ls\\base", g_root);
    swprintf(src, MAX_PATH, L"%ls\\later", g_root);
    put(L"base\\userdata\\save\\slot_00\\progress.sav", "current", 7);
    later_save(a, sizeof a, 0);
    wr32(a + 4, savever_tables.later[savever_tables.n_later - 1]);   /* a real later revision, keys the tables don't know */
    put(L"later\\slot_00\\progress.sav", a, sizeof a);
    SaveConvReport r = {0};
    CHECK(saves_replace_report(base, src, &r) == 4 && r.refused == SV_UNKNOWN_KEYS && !wcscmp(r.file, L"slot_00\\progress.sav"));
    CHECK(get(L"base\\userdata\\save\\slot_00\\progress.sav", back, sizeof back) == 7 && !memcmp(back, "current", 7));
    CHECK(get(L"later\\slot_00\\progress.sav", back, sizeof back) == sizeof a && !memcmp(back, a, sizeof a));
    wchar_t probe[MAX_PATH];
    swprintf(probe, MAX_PATH, L"%ls\\userdata\\save.new", base);
    CHECK(GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES);
    put(L"later\\slot_00\\progress.sav", "not a game save file", 20);
    memset(&r, 0, sizeof r);
    CHECK(saves_replace_report(base, src, &r) == 0 && r.files == 0 && !r.refused);
    CHECK(get(L"base\\userdata\\save\\slot_00\\progress.sav", back, sizeof back) == 20 && !memcmp(back, "not a game save file", 20));
    wchar_t bk[MAX_PATH] = L"";
    CHECK(saves_backup_from(base, src, L"original", bk, MAX_PATH) == 1 && wcsstr(bk, L"-original"));
}

/* The real tables: a converted file is 1.0.0's size, the revisions have names. */
static void test_real_tables(void) {
    CHECK(savever_tables.n_files == 5 && savever_tables.files[0].sfi == 0 && savever_tables.files[0].size == 2307552);
    CHECK(savever_name(savever_tables.target) && !wcscmp(savever_name(savever_tables.target), L"1.0.0"));
    for (int i = 0; i < savever_tables.n_later; i++) CHECK(savever_name(savever_tables.later[i]) != NULL);
    CHECK(savever_name(12345) == NULL);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "convert")) {
        wchar_t dir[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, dir, MAX_PATH);
        SaveConvReport r = {0};
        int rc = savever_convert_dir(&savever_tables, dir, &r);
        printf("rc %d from %u files %d keys_dropped %d flags_dropped %d enums_reset %d pictures_fitted %d pictures_blank %d "
               "option_dropped %d refused %d detail %u file %ls\n", rc, r.from, r.files, r.keys_dropped, r.flags_dropped,
               r.enums_reset, r.pictures_fitted, r.pictures_blank, r.option_dropped, r.refused, r.detail, r.file);
        return rc < 0 ? 1 : 0;
    }
    if (argc == 5 && !strcmp(argv[1], "fit")) {   /* fit <in.jpg> <out.jpg> <room>: the caption picture path alone */
        static uint8_t in[1 << 22], out[1 << 20];
        FILE *f = fopen(argv[2], "rb");
        size_t n = f ? fread(in, 1, sizeof in, f) : 0, got = 0;
        if (f) fclose(f);
        size_t room = (size_t)atoi(argv[4]);
        int rc = n && room <= sizeof out ? savever_fit_jpeg(in, n, 256, 144, out, room, &got) : -1;
        if (!rc && (f = fopen(argv[3], "wb"))) { fwrite(out, 1, got, f); fclose(f); }
        printf("fit rc %d: %zu -> %zu bytes (room %zu)\n", rc, n, got, room);
        return rc ? 1 : 0;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_root, MAX_PATH, L"%lsnxr_test_savever_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(g_root, NULL);
    test_convert();
    test_refusals();
    test_picture();
    test_dir();
    test_import();
    test_real_tables();
    saves_delete_tree(g_root);
    printf(g_fail ? "test_savever: %d FAILED\n" : "test_savever: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
