// rom_source.h: the game's RomFS image read straight from the player's dump (v0.1.8: no 17 GB romfs.bin copy). The
// extraction (pipeline/nx/romsource.py, extract --in-dump) verifies every IVFC level and the pinned hash, then writes
// romfs-source.txt: the dump, where the image (IVFC level 5) sits in it and its AES-CTR section key and nonce (key=value
// lines; the key comes from the player's own prod.keys and is never printed). hle_fs serves the RomFS from the dump
// through it (TOTK_ROMFS_PACK=<...>\romfs-source.txt): raw reads at image_offset + p, then the keystream XORed in place.
// CTR counter of image byte p: nonce (8 bytes) || big-endian (ctr_offset + p) / 16. AES-128 with AES-NI, 8 blocks in
// flight (several GB/s a core); without AES-NI (or TOTK_ROMFS_NO_AESNI=1) Windows' BCrypt AES-ECB over the counters. The
// key must never reach a crash dump players attach to public issues: the key, its round keys, the nonce and BCrypt's key
// object live in a page of their own (RomSecret, handed to rom_source_on_secret: pc_crash.cpp zeroes it and leaves it out
// of the minidump), the file's text is read with no intermediate copy and zeroed after parsing, a thread is marked while
// it decrypts (rom_source_on_busy: its registers and stack stay out of the dump), and the decryption wipes the XMM
// registers and its dead stack frame when it is done.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <immintrin.h>
#include <wmmintrin.h>

// The crash reporter's (pc_crash.cpp; hle_fs sets them before the first source is loaded): a range that is never written
// into a minidump, and the calling thread holding key material in its registers / stack (1: from now, 0: no longer)
inline void (*rom_source_on_secret)(void* base, size_t size) = nullptr;
inline void (*rom_source_on_busy)(int enter) = nullptr;

struct RomSecret {
    alignas(16) uint8_t rk[11 * 16];        // AES-128 round keys (AES-NI)
    uint8_t key[16], nonce[8];
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg;                  // the BCrypt fallback: one ECB key, its object below
    BCRYPT_KEY_HANDLE bkey;
    ULONG obj_len;
    alignas(16) uint8_t obj[1];             // (obj_len bytes)
#endif
};

struct RomSource {
    std::string dump;                       // the NSP / XCI (UTF-8)
    uint64_t dump_size = 0, dump_mtime_ns = 0;
    uint64_t image_offset = 0, image_size = 0, ctr_offset = 0;
    bool ctr = false;                       // false: crypt=none (a dump with decrypted sections)
    bool aesni = false;
    RomSecret* sec = nullptr;               // (the run's: never freed)
    std::string check, romfs_sha256;
};

namespace rom_source_detail {
inline void wipe(void* p, size_t n) {
#ifdef _WIN32
    SecureZeroMemory(p, n);
#else
    volatile uint8_t* q = (volatile uint8_t*)p;
    while (n--) *q++ = 0;
#endif
}
inline int nib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
inline bool hex(const char* s, size_t len, uint8_t* out, size_t n) {
    if (len != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
inline bool cpu_aesni() {
#ifdef _WIN32
    int r[4];
    __cpuid(r, 1);
    return ((unsigned)r[2] >> 25) & 1;
#else
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 25) & 1);
#endif
}
#define ROMSRC_AES __attribute__((target("aes,sse4.1")))
ROMSRC_AES inline __m128i expand_step(__m128i k, __m128i g) {
    g = _mm_shuffle_epi32(g, 0xff);
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    return _mm_xor_si128(k, g);
}
// (straight into the secret page; the registers are wiped after)
ROMSRC_AES inline void expand(const uint8_t* key, uint8_t* rk) {
    __m128i* k = (__m128i*)rk;
    _mm_store_si128(k, _mm_loadu_si128((const __m128i*)key));
#define ROMSRC_STEP(i, rcon) _mm_store_si128(k + i, expand_step(_mm_load_si128(k + i - 1), _mm_aeskeygenassist_si128(_mm_load_si128(k + i - 1), rcon)))
    ROMSRC_STEP(1, 0x01); ROMSRC_STEP(2, 0x02); ROMSRC_STEP(3, 0x04); ROMSRC_STEP(4, 0x08); ROMSRC_STEP(5, 0x10);
    ROMSRC_STEP(6, 0x20); ROMSRC_STEP(7, 0x40); ROMSRC_STEP(8, 0x80); ROMSRC_STEP(9, 0x1b); ROMSRC_STEP(10, 0x36);
#undef ROMSRC_STEP
}
// the XMM registers, cleared (key material must not stay in them after the decryption)
inline void wipe_xmm() {
    __asm__ volatile("pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\tpxor %%xmm2, %%xmm2\n\tpxor %%xmm3, %%xmm3\n\t"
                     "pxor %%xmm4, %%xmm4\n\tpxor %%xmm5, %%xmm5\n\tpxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7\n\t"
                     "pxor %%xmm8, %%xmm8\n\tpxor %%xmm9, %%xmm9\n\tpxor %%xmm10, %%xmm10\n\tpxor %%xmm11, %%xmm11\n\t"
                     "pxor %%xmm12, %%xmm12\n\tpxor %%xmm13, %%xmm13\n\tpxor %%xmm14, %%xmm14\n\tpxor %%xmm15, %%xmm15"
                     ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
                         "xmm12", "xmm13", "xmm14", "xmm15");
}
// the stack below the caller, where the decryption's frames were (spilled round keys), cleared
__attribute__((noinline)) inline void wipe_stack() {
    uint8_t pad[8192];
    wipe(pad, sizeof pad);
    __asm__ volatile("" ::"r"(pad) : "memory");
}
// the keystream of `nb` blocks from counter block `blk` XORed into p (nb * 16 bytes); round keys read from the page
ROMSRC_AES inline void xor_aesni(const RomSource& s, uint64_t blk, uint8_t* p, size_t nb) {
    const __m128i* k = (const __m128i*)s.sec->rk;
    uint64_t lo;
    memcpy(&lo, s.sec->nonce, 8);
    auto ctr = [&](uint64_t b) { return _mm_set_epi64x((long long)__builtin_bswap64(b), (long long)lo); };
    while (nb >= 8) {
        __m128i x[8];
        for (int j = 0; j < 8; j++) x[j] = _mm_xor_si128(ctr(blk + j), _mm_load_si128(k));
        for (int r = 1; r < 10; r++) {
            const __m128i kr = _mm_load_si128(k + r);
            for (int j = 0; j < 8; j++) x[j] = _mm_aesenc_si128(x[j], kr);
        }
        const __m128i kl = _mm_load_si128(k + 10);
        for (int j = 0; j < 8; j++) {
            x[j] = _mm_aesenclast_si128(x[j], kl);
            __m128i* q = (__m128i*)(p + 16 * j);
            _mm_storeu_si128(q, _mm_xor_si128(_mm_loadu_si128(q), x[j]));
        }
        blk += 8, p += 128, nb -= 8;
    }
    for (; nb; nb--, blk++, p += 16) {
        __m128i x = _mm_xor_si128(ctr(blk), _mm_load_si128(k));
        for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, _mm_load_si128(k + r));
        x = _mm_aesenclast_si128(x, _mm_load_si128(k + 10));
        _mm_storeu_si128((__m128i*)p, _mm_xor_si128(_mm_loadu_si128((const __m128i*)p), x));
    }
}
#ifdef _WIN32
// BCrypt AES-ECB over the counter blocks: one key (its object in the secret page), one caller at a time
inline std::mutex& bcrypt_mutex() { static std::mutex m; return m; }
inline void xor_bcrypt(const RomSource& s, uint64_t blk, uint8_t* p, size_t nb) {
    uint8_t ks[4096];
    std::lock_guard<std::mutex> l(bcrypt_mutex());
    while (nb) {
        size_t m = nb < sizeof ks / 16 ? nb : sizeof ks / 16;
        for (size_t j = 0; j < m; j++) {
            uint64_t b = __builtin_bswap64(blk + j);
            memcpy(ks + 16 * j, s.sec->nonce, 8);
            memcpy(ks + 16 * j + 8, &b, 8);
        }
        ULONG got = 0;
        if (!s.sec->bkey || BCryptEncrypt(s.sec->bkey, ks, (ULONG)(m * 16), nullptr, nullptr, 0, ks, (ULONG)(m * 16), &got, 0) != 0)
            memset(ks, 0, m * 16), memset(p, 0, m * 16);   // (no AES at all: zeros, which fail every check above us)
        for (size_t j = 0; j < m * 16; j++) p[j] ^= ks[j];
        blk += m, p += m * 16, nb -= m;
    }
}
#endif
inline void xor_blocks(const RomSource& s, uint64_t blk, uint8_t* p, size_t nb) {
#ifdef _WIN32
    if (!s.aesni) return xor_bcrypt(s, blk, p, nb);
#endif
    xor_aesni(s, blk, p, nb);
}
inline bool alloc_secret(RomSource* s, std::string* why) {
    size_t extra = 0;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    ULONG obj_len = 0, got = 0;
    if (s->ctr && !s->aesni) {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0 ||
            BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0 ||
            BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof obj_len, &got, 0) != 0) {
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
            *why = "no AES-NI and no Windows AES (BCrypt)";
            return false;
        }
        extra = obj_len;
    }
    const size_t size = (sizeof(RomSecret) + extra + 4095) / 4096 * 4096;
    s->sec = (RomSecret*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s->sec) { if (alg) BCryptCloseAlgorithmProvider(alg, 0); *why = "no memory for the key"; return false; }
    s->sec->alg = alg, s->sec->obj_len = obj_len;
#else
    const size_t size = (sizeof(RomSecret) + extra + 4095) / 4096 * 4096;
    s->sec = (RomSecret*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->sec == (RomSecret*)MAP_FAILED) { s->sec = nullptr; *why = "no memory for the key"; return false; }
    madvise(s->sec, size, MADV_DONTDUMP);   // (no core dump either)
#endif
    if (rom_source_on_secret) rom_source_on_secret(s->sec, size);
    return true;
}
}  // namespace rom_source_detail

// romfs-source.txt's text (text[0..n), changed: the key's hex is overwritten) -> *s with its secret page and key schedule
// ready. False with *why on a missing / bad field.
inline bool rom_source_parse(char* text, size_t n, RomSource* s, std::string* why) {
    namespace d = rom_source_detail;
    static const char* const names[12] = {"version", "dump", "dump_size", "dump_mtime_ns", "image_offset", "image_size",
                                          "crypt", "key", "ctr", "ctr_offset", "check", "romfs_sha256"};
    const char* val[12] = {};
    size_t len[12] = {};
    for (size_t i = 0; i < n;) {   // (in place: no line or value is copied but the plain fields below)
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        size_t end = e;
        while (end > i && (text[end - 1] == '\r' || text[end - 1] == ' ')) end--;
        size_t eq = i;
        while (eq < end && text[eq] != '=') eq++;
        if (end > i && text[i] != '#' && eq < end)
            for (int k = 0; k < 12; k++)
                if (strlen(names[k]) == eq - i && memcmp(text + i, names[k], eq - i) == 0) val[k] = text + eq + 1, len[k] = end - eq - 1;
        i = e + 1;
    }
    for (int k = 0; k < 12; k++)
        if (!val[k] && k != 3) { *why = std::string("no ") + names[k] + " line"; return false; }
    auto str = [&](int k) { return std::string(val[k], len[k]); };
    auto num = [&](int k) { return val[k] ? (uint64_t)strtoull(str(k).c_str(), nullptr, 10) : 0ull; };
    if (str(0) != "1") { *why = "not version 1"; return false; }
    s->dump = str(1);
    s->dump_size = num(2), s->dump_mtime_ns = num(3);
    s->image_offset = num(4), s->image_size = num(5), s->ctr_offset = num(9);
    s->check = str(10), s->romfs_sha256 = str(11);
    const std::string crypt = str(6);
    if (crypt != "ctr" && crypt != "none") { *why = "crypt is neither ctr nor none"; return false; }
    if (s->dump.empty() || !s->image_size || s->image_offset + s->image_size > s->dump_size) {
        *why = "the image lies outside the dump";
        return false;
    }
    s->ctr = crypt == "ctr";
    const char* no = getenv("TOTK_ROMFS_NO_AESNI");
    s->aesni = d::cpu_aesni() && !(no && *no == '1');
#ifndef _WIN32
    if (s->ctr && !s->aesni) { *why = "no AES-NI on this processor"; return false; }
#endif
    if (!d::alloc_secret(s, why)) return false;
    if (s->ctr) {
        const bool ok = d::hex(val[7], len[7], s->sec->key, 16) && d::hex(val[8], len[8], s->sec->nonce, 8);
        d::wipe((void*)val[7], len[7]);
        if (!ok) { *why = "a bad key or ctr line"; return false; }
        if (s->aesni) {
            d::expand(s->sec->key, s->sec->rk);
            d::wipe_xmm();
        }
#ifdef _WIN32
        else if (BCryptGenerateSymmetricKey(s->sec->alg, &s->sec->bkey, s->sec->obj, s->sec->obj_len, s->sec->key, 16, 0) != 0) {
            *why = "Windows AES (BCrypt) refused the key";
            return false;
        }
#endif
    }
    return true;
}

#ifdef _WIN32
// romfs-source.txt at `path` -> *s: read straight into a buffer of its own (no stream buffer keeps a copy), parsed,
// zeroed and freed.
inline bool rom_source_load(const wchar_t* path, RomSource* s, std::string* why) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *why = "cannot read it"; return false; }
    LARGE_INTEGER sz{};
    char* text = nullptr;
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 20) &&
              (text = (char*)VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) &&
              ReadFile(h, text, (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
    CloseHandle(h);
    if (!ok) *why = "cannot read it";
    else ok = rom_source_parse(text, got, s, why);
    if (text) {
        rom_source_detail::wipe(text, (size_t)sz.QuadPart);
        VirtualFree(text, 0, MEM_RELEASE);
    }
    return ok;
}
#else
// (Linux, the same: the text read with read(2) straight into a page of its own, kept out of core dumps, parsed, zeroed
// and unmapped; linux-test D#83: the Linux build had no dump reader, so no game file was found)
inline bool rom_source_load(const char* path, RomSource* s, std::string* why) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *why = "cannot read it"; return false; }
    struct stat st;
    char* text = nullptr;
    size_t size = 0, got = 0;
    bool ok = fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (1 << 20);
    if (ok) {
        size = ((size_t)st.st_size + 4095) / 4096 * 4096;
        void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok = m != MAP_FAILED;
        if (ok) {
            text = (char*)m;
            madvise(text, size, MADV_DONTDUMP);
            while (got < (size_t)st.st_size) {
                const ssize_t n = read(fd, text + got, (size_t)st.st_size - got);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += (size_t)n;
            }
            ok = got == (size_t)st.st_size;
        }
    }
    close(fd);
    if (!ok) *why = "cannot read it";
    else ok = rom_source_parse(text, got, s, why);
    if (text) {
        rom_source_detail::wipe(text, size);
        munmap(text, size);
    }
    return ok;
}
#endif

// Decrypt buf[0..n) in place: the raw dump bytes of image offset `off` onward (crypt=none: nothing to do)
inline void rom_source_decrypt(const RomSource& s, uint64_t off, void* buf, uint64_t n) {
    if (!s.ctr || !n) return;
    if (rom_source_on_busy) rom_source_on_busy(1);
    uint8_t* p = (uint8_t*)buf;
    uint64_t at = s.ctr_offset + off, blk = at / 16;
    unsigned skip = (unsigned)(at % 16);
    alignas(16) uint8_t one[16];
    if (skip) {   // the first block's tail
        unsigned m = (unsigned)(n < 16 - skip ? n : 16 - skip);
        memset(one, 0, 16);
        memcpy(one + skip, p, m);
        rom_source_detail::xor_blocks(s, blk, one, 1);
        memcpy(p, one + skip, m);
        p += m, n -= m, blk++;
    }
    if (n >= 16) {
        rom_source_detail::xor_blocks(s, blk, p, (size_t)(n / 16));
        blk += n / 16, p += n / 16 * 16, n %= 16;
    }
    if (n) {   // the last block's head
        memset(one, 0, 16);
        memcpy(one, p, (size_t)n);
        rom_source_detail::xor_blocks(s, blk, one, 1);
        memcpy(p, one, (size_t)n);
    }
    rom_source_detail::wipe_xmm();   // (BCrypt uses AES-NI inside too when it has it)
    rom_source_detail::wipe_stack();
    if (rom_source_on_busy) rom_source_on_busy(0);
}
