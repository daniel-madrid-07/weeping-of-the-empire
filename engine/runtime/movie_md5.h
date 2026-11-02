// MD5 of a decoded movie frame, as hex, the hash ffmpeg's framemd5 muxer writes for the same tight NV12 frame
// (`ffmpeg -i movie.webm -map 0:v:0 -pix_fmt nv12 -f framemd5 -`): TOTK_TEST_MOVIE_MD5 (hle_movie.cpp) lists the frames
// the game took, tools/engine/movie_check.py compares them with ffmpeg's. Windows: BCrypt; elsewhere none (false).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace moviemd5 {

inline bool hex(const uint8_t* p, size_t n, char out[33]) {
#ifdef _WIN32
    uint8_t dig[16];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, (PUCHAR)p, (ULONG)n, dig, sizeof dig))) return false;
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", dig[i]);
    return true;
#else
    (void)p; (void)n; out[0] = 0;
    return false;
#endif
}

}  // namespace moviemd5
