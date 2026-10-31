// pl_fonts.h: the shared fonts' files, for hle_pl.cpp (nn::pl) and its CPU test (tools/engine/test_pc_lang.py through
// tools/engine/pc_lang_test.cpp): the firmware's BFTTF fonts, decoded, and the Windows font that stands in for them when
// the player gave no firmware. Pure functions over files; no game state, no locks.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace pl_fonts {

using bytes = std::vector<uint8_t>;

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

inline bytes read_file(const std::string& path) {
    bytes v;
    FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!fp) return v;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long n = ftell(fp);
        if (n > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            v.resize((size_t)n);
            if (fread(v.data(), 1, v.size(), fp) != v.size()) v.clear();
        }
    }
    fclose(fp);
    return v;
}

// <dir>/<type>-*.bfttf (pipeline/nx/fonts.py names them), or ""
inline std::string bfttf_path(const std::string& dir, int type) {
    const std::string prefix = std::to_string(type) + "-";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().filename().string().rfind(prefix, 0) == 0 && e.path().extension() == ".bfttf") return e.path().string();
    return "";
}

// A BFTTF decoded: big-endian words XORed with key = word0 ^ 0x7f9a0218, word1 ^ key = the size, then the TrueType /
// OpenType font; empty when the file is not one
inline bytes bfttf_decode(const bytes& raw) {
    if (raw.size() <= 12) return {};
    const uint32_t key = be32(raw.data()) ^ 0x7f9a0218, size = be32(raw.data() + 4) ^ key, first = be32(raw.data() + 8) ^ key;
    if ((uint64_t)size + 8 != raw.size() || (first != 0x00010000 && first != 0x4f54544f)) return {};
    const uint8_t kb[4] = {(uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
    bytes f(size);
    for (uint32_t i = 0; i < size; i++) f[i] = raw[8 + i] ^ kb[i & 3];
    return f;
}

// A single sfnt font: its version tag is TrueType / OpenType / 'true' and its table directory and tables lie in the data
inline bool sfnt_ok(const bytes& f) {
    if (f.size() < 12) return false;
    const uint32_t v = be32(f.data());
    if (v != 0x00010000 && v != 0x4f54544f && v != 0x74727565) return false;
    const unsigned n = (unsigned)f[4] << 8 | f[5];
    if (!n || 12 + 16ull * n > f.size()) return false;
    for (unsigned i = 0; i < n; i++)
        if ((uint64_t)be32(&f[12 + 16 * i + 8]) + be32(&f[12 + 16 * i + 12]) > f.size()) return false;
    return true;
}

// A font file as one font: a single sfnt as it is; a .ttc collection's first face rebuilt on its own (the table directory
// with new offsets; each table's checksum covers only the table, the head checkSumAdjustment goes stale, which font
// engines ignore); empty when it is neither
inline bytes sfnt_face0(const bytes& b) {
    if (b.size() < 12) return {};
    if (memcmp(b.data(), "ttcf", 4)) return sfnt_ok(b) ? b : bytes{};
    if (b.size() < 16 || !be32(&b[8])) return {};
    const uint64_t off = be32(&b[12]);
    if (off + 12 > b.size()) return {};
    const unsigned n = (unsigned)b[off + 4] << 8 | b[off + 5];
    if (!n || off + 12 + 16ull * n > b.size()) return {};
    bytes o(b.begin() + (std::ptrdiff_t)off, b.begin() + (std::ptrdiff_t)(off + 12 + 16ull * n));
    for (unsigned i = 0; i < n; i++) {
        const uint32_t from = be32(&o[12 + 16 * i + 8]), len = be32(&o[12 + 16 * i + 12]);
        if ((uint64_t)from + len > b.size()) return {};
        while (o.size() & 3) o.push_back(0);
        const uint32_t at = (uint32_t)o.size();
        uint8_t* r = &o[12 + 16 * i + 8];
        r[0] = (uint8_t)(at >> 24), r[1] = (uint8_t)(at >> 16), r[2] = (uint8_t)(at >> 8), r[3] = (uint8_t)at;
        o.insert(o.end(), b.begin() + from, b.begin() + from + len);
    }
    return sfnt_ok(o) ? o : bytes{};
}

// The Windows font for a game language: Malgun Gothic (ko), Microsoft YaHei (zh-Hans), Microsoft JhengHei (zh-Hant),
// Yu Gothic (any other), in <windir>\Fonts; "" without a Windows folder
inline std::string windows_font_path(const char* lang, const std::string& windir) {
    if (windir.empty()) return "";
    const char* file = !strncmp(lang, "ko", 2) ? "malgun.ttf" : !strcmp(lang, "zh-Hans") ? "msyh.ttc"
                     : !strncmp(lang, "zh", 2) ? "msjh.ttc" : "YuGothM.ttc";
    return (std::filesystem::path(windir) / "Fonts" / file).string();
}

// The font a language's text gets, decided once: the firmware's standard font (decoded into *firmware0) when the player gave
// the firmware, else the language's Windows font (its face into *windows_face). False with neither: the caller falls back
// to English (pc_lang.cpp). *firmware0 / *windows_face are left as they are when already filled.
inline bool font_for_language(const char* lang, const std::string& fonts_dir, const std::string& windir, bytes* firmware0,
                              bytes* windows_face) {
    if (firmware0->empty()) *firmware0 = bfttf_decode(read_file(bfttf_path(fonts_dir, 0)));
    if (!firmware0->empty()) return true;
    if (windows_face->empty()) *windows_face = sfnt_face0(read_file(windows_font_path(lang, windir)));
    return !windows_face->empty();
}

}  // namespace pl_fonts
