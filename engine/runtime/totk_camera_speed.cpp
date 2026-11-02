// totk_camera_speed.cpp: controls.stick_camera_speed (TOTK_STICK_SENS, 0.25.. 3, default 1, at the next start;
// rival-parity 11-02, PARITY 1.5): the right stick's camera speed times the row, on top of the game's own camera speed
// option. The game turns the camera by the processed stick times one of five speeds, the float array StickSensitivity of
// Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml in Pack/Actor/PlayerCamera.pack.zs (a
// SARC, zstd with the pack dictionary of Pack/ZsDic.pack.zs). hle_fs serves a copy of that pack, built at its first open
// from the file that would be served (a mod's too), with each speed times the row (host_path, game_rebuilt): the top
// speed rises as well, which a scaled stick cannot give (it stops at a full tilt). The floats sit in the BYML array
// itself, so no offset moves and the pack keeps its size (the resource size table holds); the copy is a stored zstd frame
// (rstb_merge.h), which needs no compressor and no dictionary. The camera keys drive the right stick, so they follow the
// row; the mouse camera adds to the processed stick (games/totk/patches mouse) and would turn faster too, so pc_mouse
// divides its multiplier by totk_camera_stick_applied() and the mouse keeps its own speed.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rstb_merge.h"

bool hle_fs_read_romfs(const char* rel, std::vector<uint8_t>* out);                                   // hle_fs.cpp
bool pc_romfs_unzstd(const std::vector<uint8_t>& in, const std::vector<uint8_t>& dict, std::vector<uint8_t>* out);
bool pc_romfs_sarc_span(const std::vector<uint8_t>& a, const char* name, size_t* at, size_t* n);   // pc_ui_font.cpp

namespace {

const char kMember[] = "Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml";
std::atomic<float> g_applied{1.0f};

float wanted() {
    const char* e = getenv("TOTK_STICK_SENS");
    const float k = e && *e ? (float)atof(e) : 1.0f;
    return !(k > 0) ? 1.0f : k < 0.25f ? 0.25f : k > 3.0f ? 3.0f : k;
}

// The StickSensitivity floats of the BYML p[0, n) (little-endian "YB": key table offset +4, root +12; a node's first
// word = type | count << 8; a dictionary entry = key index | type << 24, then its value; an array = its types padded to
// 4, then a word each) times k, the old values in was: their count, 0 when the root has no such array of floats
int scale_speeds(uint8_t* p, size_t n, float k, float was[8]) {
    auto u32 = [&](size_t off, uint32_t* v) {
        if (off > n || n - off < 4) return false;
        memcpy(v, p + off, 4);
        return true;
    };
    static const char kKey[] = "StickSensitivity";
    uint32_t keys, root, kh, rh;
    if (n < 16 || p[0] != 'Y' || p[1] != 'B' || !u32(4, &keys) || !u32(12, &root) || !u32(keys, &kh) ||
        (kh & 0xff) != 0xc2 || !u32(root, &rh) || (rh & 0xff) != 0xc1)
        return 0;
    for (uint32_t i = 0; i < rh >> 8; i++) {
        uint32_t e, at, arr, ah;
        if (!u32(root + 4 + 8 * (size_t)i, &e) || (e & 0xffffff) >= kh >> 8 ||
            !u32(keys + 4 + 4 * (size_t)(e & 0xffffff), &at))
            return 0;
        const size_t s = (size_t)keys + at;
        if (s >= n || n - s < sizeof kKey || memcmp(p + s, kKey, sizeof kKey)) continue;
        if (e >> 24 != 0xc0 || !u32(root + 8 + 8 * (size_t)i, &arr) || !u32(arr, &ah) || (ah & 0xff) != 0xc0) return 0;
        const uint32_t count = ah >> 8;
        const size_t vals = (size_t)arr + 4 + ((count + 3) & ~3u);
        if (!count || count > 8 || vals > n || n - vals < 4 * (size_t)count) return 0;
        for (uint32_t j = 0; j < count; j++)
            if (p[arr + 4 + j] != 0xd2) return 0;   // (floats only)
        for (uint32_t j = 0; j < count; j++) {
            float v;
            memcpy(&v, p + vals + 4 * j, 4);
            was[j] = v;
            v *= k;
            memcpy(p + vals + 4 * j, &v, 4);
        }
        return (int)count;
    }
    return 0;
}

}  // namespace

// hle_fs.cpp host_path: the RomFS path of the pack it rebuilds with totk_camera_pack_build, nullptr with the row at 1
extern "C" const char* totk_camera_pack_rel(void) { return wanted() == 1.0f ? nullptr : "/Pack/Actor/PlayerCamera.pack.zs"; }

// The camera pack `in` (the file that would be served) with the row's factor applied, as a stored zstd frame; false
// (serve `in`): the row at 1, or `in` is not a camera pack this code reads (logged)
bool totk_camera_pack_build(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    const float k = wanted();
    if (k == 1.0f) return false;
    std::vector<uint8_t> raw, dicts, dict, sarc;
    size_t at = 0, n = 0;
    const char* why = nullptr;
    float was[8];
    int count = 0;
    if (!hle_fs_read_romfs("Pack/ZsDic.pack.zs", &raw) || !pc_romfs_unzstd(raw, {}, &dicts) ||
        !pc_romfs_sarc_span(dicts, "pack.zsdic", &at, &n))
        why = "the pack dictionary is not readable";
    else if (dict.assign(dicts.begin() + (std::ptrdiff_t)at, dicts.begin() + (std::ptrdiff_t)(at + n)),
             !pc_romfs_unzstd(in, dict, &sarc) || !pc_romfs_sarc_span(sarc, kMember, &at, &n))
        why = "the pack has no camera parameters this build reads";
    else if (!(count = scale_speeds(sarc.data() + at, n, k, was)))
        why = "its camera parameters have no StickSensitivity array";
    if (why) {
        printf("[camera] right-stick camera speed x%.2f not applied: %s\n", k, why);
        return false;
    }
    char list[96] = "", *o = list;
    for (int j = 0; j < count; j++) o += snprintf(o, (size_t)(list + sizeof list - o), "%s%.2f", j ? " " : "", was[j]);
    printf("[camera] right-stick camera speed x%.2f: the game's speeds %s each times it\n", k, list);
    *out = rstb::zstd_stored(sarc);
    g_applied.store(k);
    return true;
}

// pc_mouse.cpp: the factor the served pack carries (1 until it is built, or when it is not)
extern "C" float totk_camera_stick_applied(void) { return g_applied.load(std::memory_order_relaxed); }
