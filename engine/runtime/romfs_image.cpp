// romfs_image.cpp: romfs_image.h's index.
#include "romfs_image.h"
#include <string.h>

namespace {

uint32_t u32_at(const std::vector<uint8_t> &t, uint64_t o) { uint32_t v; memcpy(&v, t.data() + o, 4); return v; }
uint64_t u64_at(const std::vector<uint8_t> &t, uint64_t o) { uint64_t v; memcpy(&v, t.data() + o, 8); return v; }

}  // namespace

bool romfs_image_index(const std::function<uint64_t(uint64_t, void *, uint64_t)> &rd, uint64_t image_size,
                       std::vector<RomImageFile> *files, std::string *paths, const char **why) {
    uint64_t h[10];
    if (image_size < sizeof h || rd(0, h, sizeof h) != sizeof h) { *why = "short header"; return false; }
    if (h[0] != 0x50) { *why = "not a RomFS image (header size is not 0x50)"; return false; }
    for (int i = 1; i < 9; i += 2)
        if (h[i] > image_size || h[i + 1] > image_size - h[i]) { *why = "a table out of bounds"; return false; }
    const uint64_t data = h[9];
    if (data > image_size) { *why = "the data offset is out of bounds"; return false; }
    // (v100's are 114 KB and 22 MB: a corrupt header must not allocate up to the image's 17 GB)
    if (h[4] > (256ull << 20) || h[8] > (256ull << 20)) { *why = "a table over 256 MB"; return false; }
    std::vector<uint8_t> dirs(h[4]), ft(h[8]);
    if (rd(h[3], dirs.data(), h[4]) != h[4] || rd(h[7], ft.data(), h[8]) != h[8]) { *why = "short tables"; return false; }

    // directory entry offset -> its path, filled in table order: a parent precedes its children in the images the SDK
    // and hactool write, and an image where one does not fails the index (the folder serves the RomFS)
    std::vector<std::pair<uint32_t, std::string>> dir_path;   // (offset, path), sorted by offset as appended
    auto find_dir = [&](uint32_t off) -> const std::string * {
        size_t lo = 0, hi = dir_path.size();
        while (lo < hi) {
            const size_t m = (lo + hi) / 2;
            if (dir_path[m].first < off) lo = m + 1; else hi = m;
        }
        return lo < dir_path.size() && dir_path[lo].first == off ? &dir_path[lo].second : nullptr;
    };
    for (uint64_t o = 0; o + 24 <= dirs.size();) {
        const uint32_t parent = u32_at(dirs, o), len = u32_at(dirs, o + 20);
        if (len > dirs.size() - o - 24) { *why = "a directory name out of bounds"; return false; }
        std::string p;
        if (o) {
            const std::string *pp = find_dir(parent);
            if (!pp) { *why = "a directory before its parent"; return false; }
            p = pp->empty() ? std::string() : *pp + "/";
            p.append((const char *)dirs.data() + o + 24, len);
        }
        dir_path.push_back({(uint32_t)o, std::move(p)});
        o += 24 + ((len + 3) & ~3ull);
    }

    files->clear();
    paths->clear();
    files->reserve(ft.size() / 64);
    paths->reserve(ft.size());
    for (uint64_t o = 0; o + 32 <= ft.size();) {
        const uint32_t parent = u32_at(ft, o), len = u32_at(ft, o + 28);
        const uint64_t off = u64_at(ft, o + 8), size = u64_at(ft, o + 16);
        if (len > ft.size() - o - 32) { *why = "a file name out of bounds"; return false; }
        if (off > image_size - data || size > image_size - data - off) { *why = "a file's data out of bounds"; return false; }
        const std::string *dp = find_dir(parent);
        if (!dp) { *why = "a file in no directory"; return false; }
        RomImageFile f;
        f.path_off = (uint32_t)paths->size();
        if (!dp->empty()) { paths->append(*dp); paths->push_back('/'); }
        paths->append((const char *)ft.data() + o + 32, len);
        f.path_len = (uint32_t)(paths->size() - f.path_off);
        f.data_off = data + off;
        f.size = size;
        files->push_back(f);
        o += 32 + ((len + 3) & ~3ull);
    }
    if (files->empty()) { *why = "no files"; return false; }
    return true;
}
