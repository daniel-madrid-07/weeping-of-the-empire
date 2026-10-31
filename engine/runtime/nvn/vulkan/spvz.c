// spvz.h: the SPIR-V disk cache's compressed entries (warmup 2026-10-30, the "2.3 GB of SPIR-V"). Measured offline
// on fp_kak_1831's cache: XPRESS Huffman 3.35x, 0.12 ms a file to decompress, 0.28 ms to compress; zstd -3 would be
// 3.74x but is not in the link, LZ4 2.2x, plain XPRESS 2.4x. Reads happen on the shader jobs and the warm-up
// (against ~1 ms for spvgen, ~20 for shaderc), writes after a compile. tools/engine/test_spvz.py round-trips real
// cache files.
#include "spvz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static bool spvz_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_SPIRV_COMPRESS"); on = !(e && *e == '0'); }
    return on;
}

#ifdef _WIN32
typedef int(__stdcall *spvz_create_fn)(uint32_t alg, void *routines, void **handle);
typedef int(__stdcall *spvz_run_fn)(void *handle, const void *in, size_t in_size, void *out, size_t out_size, size_t *done);
static spvz_create_fn ccreate, dcreate;
static spvz_run_fn compress_, decompress_;
static bool api(void) {   // (cabinet.dll, once; a race only loads it twice)
    static volatile int ok = -1;
    if (ok < 0) {
        HMODULE m = LoadLibraryA("cabinet.dll");
        ccreate = m ? (spvz_create_fn)GetProcAddress(m, "CreateCompressor") : NULL;
        dcreate = m ? (spvz_create_fn)GetProcAddress(m, "CreateDecompressor") : NULL;
        compress_ = m ? (spvz_run_fn)GetProcAddress(m, "Compress") : NULL;
        decompress_ = m ? (spvz_run_fn)GetProcAddress(m, "Decompress") : NULL;
        ok = ccreate && dcreate && compress_ && decompress_;
    }
    return ok;
}
// one compressor / decompressor per thread, kept (creating one is most of a small entry's cost)
static void *handle(bool comp) {
    static __thread void *c, *d;
    void **h = comp ? &c : &d;
    if (!*h && api() && !(comp ? ccreate : dcreate)(SPVZ_XPRESS_HUFF, NULL, h)) *h = NULL;
    return *h;
}
#endif

uint8_t *spvz_pack(const uint32_t *spv, size_t bytes, size_t *out_bytes) {
#ifdef _WIN32
    void *c = spvz_on() && bytes > 64 && bytes <= (64u << 20) ? handle(true) : NULL;
    uint8_t *z = c ? malloc(16 + bytes) : NULL;
    size_t zn = 0;
    if (z && compress_(c, spv, bytes, z + 16, bytes, &zn) && zn && 16 + zn <= bytes - bytes / 10) {
        const uint32_t h[4] = {SPVZ_MAGIC, SPVZ_XPRESS_HUFF, (uint32_t)bytes, (uint32_t)zn};
        memcpy(z, h, sizeof h);
        *out_bytes = (16 + zn + 3) / 4 * 4;   // (whole words: the loader's size check)
        memset(z + 16 + zn, 0, *out_bytes - 16 - zn);
        return z;
    }
    free(z);
#else
    (void)spv, (void)bytes, (void)out_bytes, (void)spvz_on;
#endif
    return NULL;
}

uint32_t *spvz_unpack(const uint8_t *file, size_t n, size_t *bytes) {
    uint32_t h[4];
    if (n < 16) return NULL;
    memcpy(h, file, sizeof h);
    if (h[0] != SPVZ_MAGIC || h[1] != SPVZ_XPRESS_HUFF || h[2] <= 20 || h[2] % 4 || h[2] > (64u << 20) || !h[3] ||
        h[3] > n - 16)
        return NULL;
#ifdef _WIN32
    void *d = handle(false);
    uint32_t *w = d ? malloc(h[2]) : NULL;
    size_t got = 0;
    if (w && decompress_(d, file + 16, h[3], w, h[2], &got) && got == h[2] && w[0] == 0x07230203) {
        *bytes = h[2];
        return w;
    }
    free(w);
#else
    (void)bytes;
#endif
    return NULL;
}

int spvz_compact(const char *dir, size_t *raw, size_t *packed) {
    *raw = *packed = 0;
    int done = 0;
#ifdef _WIN32
    char path[600], tmp[620], mark[600], spz[600];
    snprintf(mark, sizeof mark, "%s/compact2.done", dir);
    if (!spvz_on() || !api() || GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES) return 0;
    snprintf(path, sizeof path, "%s/*.spv", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int skipped = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // (very low I/O priority too: the game streams)
    do {
        // (a size that is no entry's: one being written, whose directory size lags until it is closed; next pass)
        if (fd.nFileSizeHigh || fd.nFileSizeLow <= 20 || fd.nFileSizeLow % 4) { skipped++; continue; }
        snprintf(path, sizeof path, "%s/%s", dir, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) { skipped++; continue; }
        const size_t n = fd.nFileSizeLow;
        uint32_t *d = malloc(n);
        const int ok = d && fread(d, 1, n, f) == n;
        fclose(f);
        snprintf(spz, sizeof spz, "%s", path);
        memcpy(spz + strlen(spz) - 3, "spz", 3);
        if (ok && d[0] == SPVZ_MAGIC) {   // (db866e6d stored compressed entries under the raw name)
            if (!MoveFileExA(path, spz, MOVEFILE_REPLACE_EXISTING)) skipped++;
            free(d);
            continue;
        }
        size_t zn = 0;
        uint8_t *z = ok && d[0] == 0x07230203 ? spvz_pack(d, n, &zn) : NULL;
        if (z) {   // (<hash>.spz, then the .spv removed: a reader holding it open makes that fail, and the next pass retries)
            snprintf(tmp, sizeof tmp, "%s.z%lu", spz, (unsigned long)GetCurrentThreadId());
            FILE *o = fopen(tmp, "wb");
            int w = o && fwrite(z, 1, zn, o) == zn;
            if (o) w = !fclose(o) && w;
            if (w && MoveFileExA(tmp, spz, MOVEFILE_REPLACE_EXISTING)) {
                done++;
                *raw += n;
                *packed += zn;
                if (!DeleteFileA(path)) skipped++;
            } else {
                DeleteFileA(tmp);
                skipped++;
            }
            Sleep(5);
        }
        free(z);
        free(d);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (!skipped) {   // (a whole pass: no raw entry left to look for)
        FILE *m = fopen(mark, "wb");
        if (m) fclose(m);
    }
#else
    (void)dir;
#endif
    return done;
}
