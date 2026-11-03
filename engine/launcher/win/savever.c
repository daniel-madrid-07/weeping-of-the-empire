/* Saves of later game versions into 1.0.0: see savever.h. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#include "savever.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include "savever_data.h"

#define MAGIC 0x01020304u
#define TABLE 0x20u
#define GROUPS 33
#define G_ENUM 6
#define G_ENUM_ARRAY 7
#define G_BINARY 18
#define G_BOOL64 32
/* 1.1.x and 1.2.x share one GameDataList revision*/
#define REV_11X 4710644u

static const uint32_t later_revs[] = {REV_11X, SAVEVER_REV_LATEST};
const SaveTables savever_tables = {
    SAVEVER_REV_100, later_revs, 2,
    file_specs, (int)(sizeof file_specs / sizeof *file_specs),
    drop_b64, (int)(sizeof drop_b64 / sizeof *drop_b64),
    enum_maps, (int)(sizeof enum_maps / sizeof *enum_maps),
    bin_rooms, (int)(sizeof bin_rooms / sizeof *bin_rooms),
    256, 144,   /* the 1.0.0 caption JPEG */
};

const wchar_t *savever_name(uint32_t rev) {
    if (rev == SAVEVER_REV_100) return L"1.0.0";
    if (rev == REV_11X) return L"1.1.x / 1.2.x";
    if (rev == SAVEVER_REV_LATEST) return L"1.4.x";
    return NULL;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int inline_group(uint32_t g) { return g == 0 || g == 2 || g == 4 || g == 6 || g == 20; }

uint32_t savever_revision(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint8_t b[8];
    DWORD got = 0;
    int ok = ReadFile(h, b, 8, &got, NULL) && got == 8 && rd32(b) == MAGIC;
    CloseHandle(h);
    return ok ? rd32(b + 4) : 0;
}

uint32_t savever_dir_revision(const wchar_t *dir) {
    uint32_t best = 0;
    for (int i = 0; i < 6; i++) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\slot_%02d\\progress.sav", dir, i);
        uint32_t r = savever_revision(p);
        if (r > best) best = r;
    }
    return best;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* FNV-1a 64 over the sorted (group << 32 | hash) values, little-endian (savever_gen.py: digest). Sorts keys. */
static uint64_t key_digest(uint64_t *keys, size_t n) {
    qsort(keys, n, sizeof *keys, cmp_u64);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) h = (h ^ (uint8_t)(keys[i] >> (8 * b))) * 0x100000001b3ull;
    return h;
}

typedef struct Entry { uint32_t group, hash, value; } Entry;
typedef struct Block { uint32_t off, len, entry; } Block;   /* a data block, the table entry pointing at it */

static int cmp_block(const void *a, const void *b) {
    uint32_t x = ((const Block *)a)->off, y = ((const Block *)b)->off;
    return x < y ? -1 : x > y;
}

static int dropped(const SaveFileSpec *f, uint32_t g, uint32_t h) {
    for (int i = 0; i < f->n_drop; i++)
        if (f->drop[i].group == g && f->drop[i].hash == h) return 1;
    return 0;
}

static int later_value(const SaveEnumMap *m, uint32_t v) {
    for (int i = 0; i < m->n_added; i++)
        if (m->added[i] == v) return 1;
    return 0;
}

static const SaveEnumMap *enum_map(const SaveTables *t, int sfi, uint32_t g, uint32_t h) {
    for (int i = 0; i < t->n_enums; i++)
        if (t->enums[i].sfi == sfi && (uint32_t)t->enums[i].group == g && t->enums[i].key == h) return &t->enums[i];
    return NULL;
}

static const SaveBinRoom *bin_room(const SaveTables *t, int sfi, uint32_t h) {
    for (int i = 0; i < t->n_bins; i++)
        if (t->bins[i].sfi == sfi && t->bins[i].key == h) return &t->bins[i];
    return NULL;
}

static int refuse(SaveConvReport *r, int why, uint32_t detail) {
    r->refused = why;
    r->detail = detail;
    return why;
}

int savever_convert(const SaveTables *t, const SaveFileSpec *f, const uint8_t *in, size_t n, uint8_t *out, SaveConvReport *r) {
    if (n < 8 || rd32(in) != MAGIC) return SV_SAME;   /* not a game save file: as it was, the game decides */
    uint32_t rev = rd32(in + 4), start = n >= 12 ? rd32(in + 8) : 0;
    if (rev == t->target) return SV_SAME;
    int known = 0;
    for (int i = 0; i < t->n_later; i++) known |= t->later[i] == rev;
    if (!known) return refuse(r, SV_UNKNOWN_VERSION, rev);
    if (n < TABLE + 8 || n > 64u << 20 || start < TABLE + 8 || start > n || (start - TABLE) % 8) return refuse(r, SV_BAD_FILE, 0);

    size_t ne = (start - TABLE) / 8, nb = 0, kept = 0, out_len = 0;
    Entry *e = calloc(ne, sizeof *e);
    Block *b = calloc(ne, sizeof *b);
    uint64_t *keys = calloc(ne, sizeof *keys);
    uint8_t *data = NULL;
    uint32_t *newoff = calloc(ne, sizeof *newoff);
    int rc = SV_BAD_FILE;
    if (!e || !b || !keys || !newoff) { rc = refuse(r, SV_IO, 0); goto done; }

    /* the table: (0, 0) first, then the markers of groups 1..32 in order */
    int group = -1;
    for (size_t i = 0; i < ne; i++) {
        e[i].hash = rd32(in + TABLE + 8 * i);
        e[i].value = rd32(in + TABLE + 8 * i + 4);
        if (!e[i].hash) {
            if ((int)e[i].value != group + 1) goto bad;   /* every group, in order (all 33 in every file) */
            group = (int)e[i].value;
        } else if (group < 0) goto bad;
        e[i].group = (uint32_t)group;
        if (e[i].hash && !inline_group(e[i].group) && e[i].value) {   /* 0: no block (an empty Bool64bitKey list) */
            if (e[i].value < start || e[i].value >= n) goto bad;
            b[nb].off = e[i].value;
            b[nb++].entry = (uint32_t)i;
        }
    }
    if (group != GROUPS - 1) goto bad;
    qsort(b, nb, sizeof *b, cmp_block);
    for (size_t i = 0; i < nb; i++) {
        uint32_t end = i + 1 < nb ? b[i + 1].off : (uint32_t)n;
        if (end <= b[i].off) goto bad;   /* two keys on one block */
        b[i].len = end - b[i].off;
    }

    /* what stays, with the added enum values reset */
    for (size_t i = 0; i < ne; i++) {
        if (!e[i].hash) continue;
        if (dropped(f, e[i].group, e[i].hash)) { r->keys_dropped++; e[i].group = GROUPS; continue; }
        keys[kept++] = (uint64_t)e[i].group << 32 | e[i].hash;
        const SaveEnumMap *m = e[i].group == G_ENUM ? enum_map(t, f->sfi, G_ENUM, e[i].hash) : NULL;
        if (m && later_value(m, e[i].value)) { e[i].value = m->def; r->enums_reset++; }
    }
    if (kept != f->count || key_digest(keys, kept) != f->digest) {
        rc = refuse(r, SV_UNKNOWN_KEYS, kept > f->count ? (uint32_t)(kept - f->count) : 0);
        goto done;
    }

    /* the data blocks, in their order; the table shrinks by the dropped keys */
    size_t rows = 0;
    for (size_t i = 0; i < ne; i++) rows += e[i].group != GROUPS;
    size_t cap = TABLE + rows * 8 + n, head = TABLE + rows * 8;
    for (int i = 0; i < t->n_bins; i++) cap += 4 + (size_t)t->bins[i].room;
    if (!(data = calloc(cap, 1))) { rc = refuse(r, SV_IO, 0); goto done; }
    size_t pos = head;
    for (size_t k = 0; k < nb; k++) {
        Entry *x = &e[b[k].entry];
        if (x->group == GROUPS) continue;
        const uint8_t *src = in + b[k].off;
        uint32_t len = b[k].len;
        newoff[b[k].entry] = (uint32_t)pos;
        const SaveEnumMap *m = x->group == G_ENUM_ARRAY ? enum_map(t, f->sfi, G_ENUM_ARRAY, x->hash) : NULL;
        const SaveBinRoom *room = x->group == G_BINARY ? bin_room(t, f->sfi, x->hash) : NULL;
        if (x->group == G_BOOL64) {
            /* the set flags up to a zero key; what lies past it is stale (TotkMods/Research, Formats/Sav) */
            size_t q = 0, w = 0;
            for (; q + 8 <= len && rd64(src + q); q += 8) {
                uint64_t key = rd64(src + q);
                int later = 0;
                if (f->sfi == 0)
                    for (int j = 0; j < t->n_b64; j++) later |= t->drop_b64[j] == key;
                if (later) { r->flags_dropped++; continue; }
                memcpy(data + pos + w, src + q, 8);
                w += 8;
            }
            if (q + 8 > len) goto bad;   /* no end */
            pos += w + 8;
        } else if (m) {
            uint32_t c = len >= 4 ? rd32(src) : 0;
            if (len < 4 || (len - 4) / 4 < c) goto bad;
            memcpy(data + pos, src, len);
            for (uint32_t j = 0; j < c; j++)
                if (later_value(m, rd32(src + 4 + 4 * j))) { wr32(data + pos + 4 + 4 * j, m->def); r->enums_reset++; }
            pos += len;
        } else if (room) {
            uint32_t sz = len >= 4 ? rd32(src) : 0;
            if (len < 4 || len - 4 < sz) goto bad;
            if (sz <= room->room) memcpy(data + pos, src, 4 + sz);
            else {
                size_t got = 0;
                if (savever_fit_jpeg(src + 4, sz, t->pic_w, t->pic_h, data + pos + 4, room->room, &got) == 0) r->pictures_fitted++;
                else if (savever_fit_jpeg(NULL, 0, t->pic_w, t->pic_h, data + pos + 4, room->room, &got) == 0) r->pictures_blank++;
                else { rc = refuse(r, SV_PICTURE, 0); goto done; }
                wr32(data + pos, (uint32_t)got);
            }
            pos += 4 + room->room;   /* 1.0.0 keeps the whole room */
        } else {
            memcpy(data + pos, src, len);
            pos += len;
        }
    }
    /* 1.0.0's size: the zero tail (the last block runs to the end of the file) may shrink, nothing else */
    out_len = pos;
    while (out_len > f->size && out_len > head && !data[out_len - 1]) out_len--;
    if (out_len > f->size) { rc = refuse(r, SV_TOO_BIG, 0); goto done; }

    memset(out, 0, f->size);
    memcpy(out + head, data + head, out_len - head);
    wr32(out, MAGIC);
    wr32(out + 4, t->target);
    wr32(out + 8, (uint32_t)head);
    for (size_t i = 0, row = 0; i < ne; i++) {
        if (e[i].group == GROUPS) continue;
        uint32_t v = e[i].hash && !inline_group(e[i].group) && e[i].value ? newoff[i] : e[i].value;
        wr32(out + TABLE + 8 * row, e[i].hash);
        wr32(out + TABLE + 8 * row + 4, v);
        row++;
    }
    if (rev > r->from) r->from = rev;
    r->files++;
    rc = SV_CONVERTED;
    goto done;
bad:
    rc = refuse(r, SV_BAD_FILE, 0);
done:
    free(e);
    free(b);
    free(keys);
    free(data);
    free(newoff);
    return rc;
}

static uint8_t *read_all(const wchar_t *path, size_t *n) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *p = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart <= (64 << 20) && (p = malloc((size_t)sz.QuadPart + 1)) &&
        sz.QuadPart && (!ReadFile(h, p, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) {
        free(p);
        p = NULL;
    }
    CloseHandle(h);
    *n = p ? (size_t)sz.QuadPart : 0;
    return p;
}

static int write_all(const wchar_t *path, const uint8_t *p, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD put = 0;
    int ok = WriteFile(h, p, (DWORD)n, &put, NULL) && put == (DWORD)n && FlushFileBuffers(h);
    CloseHandle(h);
    return ok ? 0 : -1;
}

int savever_convert_dir(const SaveTables *t, const wchar_t *dir, SaveConvReport *r) {
    enum { MAXF = 64 };
    struct { wchar_t path[MAX_PATH]; uint8_t *data; size_t n; int drop; } job[MAXF];
    int nj = 0, rc = SV_SAME;
    for (int s = -1; s < 6; s++)   /* -1: option.sav, beside the slots */
        for (int i = 0; i < t->n_files; i++) {
            const SaveFileSpec *f = &t->files[i];
            if ((s < 0) != (f->sfi == 1) || nj >= MAXF) continue;
            wchar_t p[MAX_PATH], rel[48];
            if (s < 0) swprintf(rel, 48, L"%ls", f->name);
            else swprintf(rel, 48, L"slot_%02d\\%ls", s, f->name);
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, rel);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
            size_t n = 0;
            uint8_t *in = read_all(p, &n), *out = in ? malloc(f->size) : NULL;
            SaveConvReport one = {0};
            int cv = out ? savever_convert(t, f, in, n, out, &one) : refuse(&one, in ? SV_IO : SV_BAD_FILE, 0);
            free(in);
            r->keys_dropped += one.keys_dropped, r->flags_dropped += one.flags_dropped, r->enums_reset += one.enums_reset;
            r->pictures_fitted += one.pictures_fitted, r->pictures_blank += one.pictures_blank;
            if (cv == SV_SAME) { free(out); continue; }
            if (cv < 0 && f->sfi == 1 && cv != SV_IO) {   /* settings: leave them out rather than refuse the save */
                free(out);
                out = NULL;
                job[nj].drop = 1;
            } else if (cv < 0) {
                free(out);
                r->refused = one.refused, r->detail = one.detail;
                swprintf(r->file, 48, L"%ls", rel);
                rc = cv;
                goto done;
            } else {
                job[nj].drop = 0;
                if (one.from > r->from) r->from = one.from;
                r->files++;
            }
            swprintf(job[nj].path, MAX_PATH, L"%ls", p);
            job[nj].data = out;
            job[nj++].n = f->size;
        }
    for (int j = 0; j < nj; j++) {
        if (job[j].drop) {
            SetFileAttributesW(job[j].path, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(job[j].path)) { rc = refuse(r, SV_IO, 0); goto done; }
            r->option_dropped = 1;
            continue;
        }
        wchar_t tmp[MAX_PATH];
        swprintf(tmp, MAX_PATH, L"%ls.tmp", job[j].path);
        if (write_all(tmp, job[j].data, job[j].n) || !MoveFileExW(tmp, job[j].path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp);
            rc = refuse(r, SV_IO, 0);
            swprintf(r->file, 48, L"%ls", job[j].path + wcslen(dir) + 1);
            goto done;
        }
        rc = SV_CONVERTED;
    }
done:
    for (int j = 0; j < nj; j++) free(job[j].data);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the picture (WIC) */

static int encode(IWICImagingFactory *fac, IWICBitmapSource *src, float q, uint8_t *out, size_t room, size_t *out_n) {
    IStream *ms = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *bag = NULL;
    int rc = -1;
    UINT w = 0, h = 0;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(IWICBitmapSource_GetSize(src, &w, &h)) || FAILED(CreateStreamOnHGlobal(NULL, TRUE, &ms)) ||
        FAILED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg, NULL, &enc)) ||
        FAILED(IWICBitmapEncoder_Initialize(enc, ms, WICBitmapEncoderNoCache)) ||
        FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &bag)))
        goto done;
    PROPBAG2 opt = {0};
    opt.pstrName = L"ImageQuality";
    VARIANT v;
    memset(&v, 0, sizeof v);   /* VT_EMPTY */
    v.vt = VT_R4;
    v.fltVal = q;
    if (FAILED(IPropertyBag2_Write(bag, 1, &opt, &v)) || FAILED(IWICBitmapFrameEncode_Initialize(fr, bag)) ||
        FAILED(IWICBitmapFrameEncode_SetSize(fr, w, h)) || FAILED(IWICBitmapFrameEncode_SetPixelFormat(fr, &pf)) ||
        !IsEqualGUID(&pf, &GUID_WICPixelFormat24bppBGR) || FAILED(IWICBitmapFrameEncode_WriteSource(fr, src, NULL)) ||
        FAILED(IWICBitmapFrameEncode_Commit(fr)) || FAILED(IWICBitmapEncoder_Commit(enc)))
        goto done;
    STATSTG st;
    if (FAILED(IStream_Stat(ms, &st, STATFLAG_NONAME))) goto done;
    if (st.cbSize.QuadPart > room) { rc = 1; goto done; }
    LARGE_INTEGER zero = {0};
    ULONG got = 0;
    if (SUCCEEDED(IStream_Seek(ms, zero, STREAM_SEEK_SET, NULL)) && SUCCEEDED(IStream_Read(ms, out, (ULONG)st.cbSize.QuadPart, &got)) &&
        got == st.cbSize.QuadPart) {
        *out_n = got;
        rc = 0;
    }
done:
    if (bag) IPropertyBag2_Release(bag);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (ms) IStream_Release(ms);
    return rc;
}

int savever_fit_jpeg(const uint8_t *jpg, size_t n, int w, int h, uint8_t *out, size_t room, size_t *out_n) {
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* S_FALSE / RPC_E_CHANGED_MODE: COM is up already */
    IWICImagingFactory *fac = NULL;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmap *black = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    uint8_t *pixels = NULL;
    int rc = -1;
    IWICBitmapSource *src = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac)))
        goto done;
    if (jpg) {
        if (FAILED(IWICImagingFactory_CreateStream(fac, &st)) || FAILED(IWICStream_InitializeFromMemory(st, (BYTE *)jpg, (DWORD)n)) ||
            FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) ||
            FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)))
            goto done;
        src = (IWICBitmapSource *)frame;
    } else {
        if (!(pixels = calloc((size_t)w * h, 3)) ||
            FAILED(IWICImagingFactory_CreateBitmapFromMemory(fac, (UINT)w, (UINT)h, &GUID_WICPixelFormat24bppBGR, (UINT)w * 3,
                                                             (UINT)w * h * 3, pixels, &black)))
            goto done;
        src = (IWICBitmapSource *)black;
    }
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(fac, &sc)) ||
        FAILED(IWICBitmapScaler_Initialize(sc, src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(fac, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                              NULL, 0.0, WICBitmapPaletteTypeCustom)))
        goto done;
    for (int q = 92; q >= 20 && rc; q -= 8) {
        int e = encode(fac, (IWICBitmapSource *)cv, q / 100.0f, out, room, out_n);
        if (e < 0) break;
        rc = e;
    }
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (black) IWICBitmap_Release(black);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    if (fac) IWICImagingFactory_Release(fac);
    free(pixels);
    if (SUCCEEDED(co)) CoUninitialize();
    return rc ? -1 : 0;
}
