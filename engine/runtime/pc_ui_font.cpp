// pc_ui_font.cpp: the game's own UI font (Rodin NTLG) for the PC overlay's fps readout (pc_overlay.cpp), read from the
// player's RomFS when it is first needed, never shipped.
//
// Font/Font.Nin_NX_NVN.bfarc.zs: a SARC, zstd with the game's dictionary 1 (zs.zsdic in Pack/ZsDic.pack.zs, itself a
// SARC in a plain zstd frame); its scft/*.bfotf are OpenType (CFF) fonts behind the Switch's font obfuscation: word 0
// BE ^ 0x7f9a0218 is the key, word 1 BE ^ key the size, the bytes from 8 on XOR the key's (big-endian) bytes in turn.
// Any failure (another game on this engine, a damaged dump): false, and the overlay keeps its own font.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../third_party/zstd/zstd.h"

bool hle_fs_read_romfs(const char* rel, std::vector<uint8_t>* out);   // hle_fs.cpp

namespace {

uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t le16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Where SARC member `name` is in `a`: its bytes [*at, *at + *n) (little-endian SARC only, as the game's), or false
bool sarc_span(const std::vector<uint8_t>& a, const char* name, size_t* at_out, size_t* n_out) {
    if (a.size() < 0x14 || memcmp(a.data(), "SARC", 4) || le16(&a[6]) != 0xfeff) return false;
    const size_t head = le16(&a[4]), data = le32(&a[0x0c]);
    if (head + 12 > a.size() || memcmp(&a[head], "SFAT", 4)) return false;
    const size_t count = le16(&a[head + 6]), nodes = head + 12, names = nodes + count * 16 + 8;
    if (names > a.size() || memcmp(&a[names - 8], "SFNT", 4)) return false;
    const size_t want = strlen(name);
    for (size_t i = 0; i < count; i++) {
        const uint8_t* n = &a[nodes + i * 16];
        const uint32_t attr = le32(n + 4), lo = le32(n + 8), hi = le32(n + 12);
        if (!(attr >> 24)) continue;   // (no name)
        const size_t at = names + (size_t)(attr & 0xffffff) * 4;
        if (at + want >= a.size() || memcmp(&a[at], name, want) || a[at + want]) continue;
        if (lo > hi || data + hi > a.size()) return false;
        *at_out = data + lo;
        *n_out = hi - lo;
        return true;
    }
    return false;
}

// The bytes of SARC member `name`, or false
bool sarc_member(const std::vector<uint8_t>& a, const char* name, std::vector<uint8_t>* out) {
    size_t at, n;
    if (!sarc_span(a, name, &at, &n)) return false;
    out->assign(a.begin() + (std::ptrdiff_t)at, a.begin() + (std::ptrdiff_t)(at + n));
    return true;
}

// One zstd frame of the RomFS, with dictionary `dict` (empty: none)
bool unzstd(const std::vector<uint8_t>& in, const std::vector<uint8_t>& dict, std::vector<uint8_t>* out) {
    const unsigned long long n = ZSTD_getFrameContentSize(in.data(), in.size());
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR || n > (64u << 20)) return false;
    out->resize((size_t)n);
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    if (!dc) return false;
    const size_t r = ZSTD_decompress_usingDict(dc, out->data(), out->size(), in.data(), in.size(),
                                               dict.empty() ? nullptr : dict.data(), dict.size());
    ZSTD_freeDCtx(dc);
    return !ZSTD_isError(r) && r == n;
}

}  // namespace

// (pc_camera_speed.cpp reads its pack with the same two)
bool pc_romfs_sarc_span(const std::vector<uint8_t>& a, const char* name, size_t* at, size_t* n) { return sarc_span(a, name, at, n); }
bool pc_romfs_unzstd(const std::vector<uint8_t>& in, const std::vector<uint8_t>& dict, std::vector<uint8_t>* out) {
    return unzstd(in, dict, out);
}

// The decoded OpenType bytes of the game's font `name` (e.g. "ninP_RodinNTLG-B_DH_003_subset"), or false
bool pc_ui_font(const char* name, std::vector<uint8_t>* otf) {
    std::vector<uint8_t> raw, pack, dict, arc, f;
    if (!hle_fs_read_romfs("Pack/ZsDic.pack.zs", &raw) || !unzstd(raw, {}, &pack) || !sarc_member(pack, "zs.zsdic", &dict))
        return false;
    if (!hle_fs_read_romfs("Font/Font.Nin_NX_NVN.bfarc.zs", &raw) || !unzstd(raw, dict, &arc)) return false;
    if (!sarc_member(arc, ("scft/" + std::string(name) + ".bfotf").c_str(), &f) || f.size() < 12) return false;
    const uint32_t key = be32(&f[0]) ^ 0x7f9a0218u, size = be32(&f[4]) ^ key;
    if (size > f.size() - 8) return false;
    const uint8_t k[4] = {(uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
    otf->resize(size);
    for (uint32_t i = 0; i < size; i++) (*otf)[i] = f[8 + i] ^ k[i & 3];
    return !memcmp(otf->data(), "OTTO", 4) || be32(otf->data()) == 0x00010000u;
}
