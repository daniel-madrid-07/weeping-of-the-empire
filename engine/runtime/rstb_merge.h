// rstb_merge.h: TotK's resource size table (System/Resource/ResourceSizeTable.Product.<ver>.rsizetable.zs), parsed, merged
// and written back, for hle_fs.cpp: the player's mods (TOTK_USER_MODS) come after our overlay, which ships its own table
// (enlarged for the glyph bank and our sets), so a mod's table would be shadowed and a file it enlarged could overflow our
// entry; hle_fs serves the merge instead, each entry at the larger of the sizes. The table, zstd-compressed without a
// dictionary: "RESTBL", u32 version 1, u32 name field length (160), u32 crc entry count, u32 name entry count, then the
// crc entries {u32 crc32 of the path, u32 size} sorted by crc, then the name entries {char path[name field length], u32
// size} for paths whose crc collides (all little-endian). Mods without a table (or with one for another version) get their
// files' entries computed (estimate / grow below). Pure functions, no game data; tools/engine/test_rstb_merge.py checks
// them on synthetic tables.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../third_party/zstd/zstd.h"

namespace rstb {

struct Table {
    uint32_t name_len = 160;
    std::map<uint32_t, uint32_t> crc;         // (ordered: written back sorted, as the game's binary search needs)
    std::map<std::string, uint32_t> names;
};

// The decompressed bytes of one .zs frame (no dictionary), or false
inline bool unzstd(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    unsigned long long n = ZSTD_getFrameContentSize(in.data(), in.size());
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR || n > (256u << 20)) return false;
    out->resize((size_t)n);
    size_t r = ZSTD_decompress(out->data(), out->size(), in.data(), in.size());
    return !ZSTD_isError(r) && r == n;
}

inline uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }

inline bool parse(const std::vector<uint8_t>& b, Table* t) {
    if (b.size() < 22 || memcmp(b.data(), "RESTBL", 6)) return false;
    const uint32_t ver = le32(&b[6]), nl = le32(&b[10]), nc = le32(&b[14]), nn = le32(&b[18]);
    if (ver != 1 || nl == 0 || nl > 4096 || 22 + (uint64_t)nc * 8 + (uint64_t)nn * (nl + 4) > b.size()) return false;
    t->name_len = nl;
    t->crc.clear();
    t->names.clear();
    const uint8_t* q = &b[22];
    for (uint32_t i = 0; i < nc; i++, q += 8) t->crc[le32(q)] = le32(q + 4);
    for (uint32_t i = 0; i < nn; i++, q += nl + 4) t->names[std::string((const char*)q, strnlen((const char*)q, nl))] = le32(q + nl);
    return true;
}

struct Counts { size_t raised = 0, added = 0; };

// into = into + from, each entry at the larger size; false (into unchanged) when the name field lengths differ
inline bool merge(Table& into, const Table& from, Counts* c) {
    if (from.name_len != into.name_len) return false;
    for (const auto& [k, v] : from.crc) {
        auto [it, fresh] = into.crc.emplace(k, v);
        if (fresh) c->added++;
        else if (v > it->second) it->second = v, c->raised++;
    }
    for (const auto& [k, v] : from.names) {
        if (k.size() >= into.name_len) continue;   // (would not fit the field with its terminator)
        auto [it, fresh] = into.names.emplace(k, v);
        if (fresh) c->added++;
        else if (v > it->second) it->second = v, c->raised++;
    }
    return true;
}

inline std::vector<uint8_t> serialize(const Table& t) {
    std::vector<uint8_t> o;
    o.reserve(22 + t.crc.size() * 8 + t.names.size() * (t.name_len + 4));
    o.insert(o.end(), {'R', 'E', 'S', 'T', 'B', 'L'});
    put32(o, 1);
    put32(o, t.name_len);
    put32(o, (uint32_t)t.crc.size());
    put32(o, (uint32_t)t.names.size());
    for (const auto& [k, v] : t.crc) put32(o, k), put32(o, v);
    for (const auto& [k, v] : t.names) {
        size_t at = o.size();
        o.resize(at + t.name_len, 0);
        memcpy(&o[at], k.data(), k.size());
        put32(o, v);
    }
    return o;
}

// ---- a mod file's entry (hle_fs rstb_merged: mods that bring no table, or one for another version). Adapted from
// totk-pc-recomp runtime/restbl.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh): the factors are its measured ones.

// The standard (reflected, 0xEDB88320) CRC-32 the table hashes its keys with
inline uint32_t crc32(const std::string& s) {
    static const auto tab = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) c = tab[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// A RomFS path's key: no leading '/', no .zs / .mc ("Pack/Actor/X.pack" for "/Pack/Actor/X.pack.zs")
inline std::string key(std::string rel) {
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.size() >= 3 && (!rel.compare(rel.size() - 3, 3, ".zs") || !rel.compare(rel.size() - 3, 3, ".mc")))
        rel.resize(rel.size() - 3);
    return rel;
}

// The entry of a key (the name entries hold the keys whose crc collides); false: none
inline bool get(const Table& t, const std::string& k, uint32_t* size) {
    if (auto n = t.names.find(k); n != t.names.end()) return *size = n->second, true;
    if (auto c = t.crc.find(crc32(k)); c != t.crc.end()) return *size = c->second, true;
    return false;
}

// Raises a key's entry to `want` (adds it when missing); true when it changed
inline bool raise(Table& t, const std::string& k, uint32_t want) {
    if (auto n = t.names.find(k); n != t.names.end()) {
        if (n->second >= want) return false;
        n->second = want;
        return true;
    }
    auto [c, fresh] = t.crc.emplace(crc32(k), want);
    if (fresh) return true;
    if (c->second >= want) return false;
    c->second = want;
    return true;
}

// The factor a type's entry needs over its decompressed size (1000 = 1.0); 0 for a type the table never lists
inline unsigned type_factor(const std::string& k) {
    static const struct { const char* ext; unsigned f; } types[] = {
        {"bfres", 1500}, {"bkres", 2050}, {"bgyml", 3850}, {"asb", 1400}, {"bstar", 1200}, {"bphsc", 1100},
        {"byml", 1000}, {"txtg", 1000}, {"pack", 1000}, {"bntx", 1000}, {"bphsh", 1000}, {"vsts", 1000},
        {"ainb", 1000}, {"bphnm", 1000}, {"bars", 1000}, {"chunk", 1000}, {"bfevfl", 1000}, {"quad", 1000},
        {"baev", 1000}, {"bcul", 1000}, {"crbin", 1000}, {"wbr", 1000}, {"bhtmp", 1000}, {"dpi", 1000},
        {"cai", 1000}, {"png", 1000}, {"cutinfo", 1000}, {"blwp", 1000}, {"sarc", 1000}, {"tscb", 1000},
        {"bfsha", 1000}, {"jpg", 1000}, {"beco", 1000}, {"bfarc", 1000}, {"blarc", 1000}, {"belnk", 1000},
        {"genvb", 1000}, {"bnsh", 1000}, {"bslnk", 1000}, {"baatarc", 1000}, {"bagst", 1000},
        {"bwav", 0}, {"webm", 0},
    };
    const size_t slash = k.rfind('/'), dot = k.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return 1500;
    for (const auto& t : types)
        if (!k.compare(dot + 1, std::string::npos, t.ext)) return t.f;
    return 1500;
}

inline uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)v; }

// An added file's entry from its decompressed size (0: a type the table never lists)
inline uint32_t estimate(const std::string& k, uint64_t size) {
    const unsigned f = type_factor(k);
    if (!f) return 0;
    const uint64_t e = (size * f + 999) / 1000 + 8192;
    return clamp32((e + 4095) & ~(uint64_t)4095);
}

// A replaced file's entry: the game's, grown by what the file grew (`scaled`: .mc, a mesh codec file: in proportion)
inline uint32_t grow(const std::string& k, uint32_t entry, uint64_t was, uint64_t now, bool scaled) {
    if (now <= was) return entry;
    if (scaled) return clamp32(was ? ((uint64_t)entry * now + was - 1) / was : estimate(k, now * 8));
    const unsigned f = type_factor(k);
    if (!f) return entry;
    const uint64_t e = entry + ((now - was) * f + 999) / 1000;
    return clamp32((e + 31) & ~(uint64_t)31);
}

// The decompressed size of a file from its first bytes: a .zs file's frame content size (4x its size without one),
// else its size
inline uint64_t content_size(const std::string& rel, const uint8_t* head, size_t n, uint64_t size) {
    if (rel.size() < 3 || rel.compare(rel.size() - 3, 3, ".zs")) return size;
    const unsigned long long c = ZSTD_getFrameContentSize(head, n);
    return c == ZSTD_CONTENTSIZE_ERROR ? size : c == ZSTD_CONTENTSIZE_UNKNOWN ? size * 4 : c;
}

// One zstd frame of raw (stored) blocks: any zstd decoder reads it, and no compressor is needed. Frame header: single
// segment, a 4-byte content size, no checksum, no dictionary; blocks of at most 128 KiB.
inline std::vector<uint8_t> zstd_stored(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> o = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};   // magic 0xFD2FB528; descriptor: FCS 4 bytes, single segment
    put32(o, (uint32_t)d.size());
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(d.size() - at, 128 * 1024);
        const uint32_t h = (at + n == d.size() ? 1u : 0u) | (uint32_t)(n << 3);   // last flag, block type 0 (raw), size
        o.push_back((uint8_t)h), o.push_back((uint8_t)(h >> 8)), o.push_back((uint8_t)(h >> 16));
        o.insert(o.end(), d.begin() + (std::ptrdiff_t)at, d.begin() + (std::ptrdiff_t)(at + n));
        at += n;
    } while (at < d.size());
    return o;
}

}  // namespace rstb
