// romfs_image.h: the index of a Nintendo RomFS image (the level-3 romfs.bin the extraction verifies and unpacks, every
// IVFC level checked; romfs-report.json), read in place: hle_fs serves the RomFS from it like an NXRP pack, so no second
// 17 GB copy is made. Format (switchbrew "RomFS"): a 0x50-byte header of ten u64 (header size 0x50, the directory hash
// table, the directory table, the file hash table, the file table as offset / size pairs, the file data offset); a
// directory entry is u32 parent, sibling, first child directory, first child file, hash link, name length, then the
// name padded to 4; a file entry is u32 parent, sibling, u64 data offset (from the data offset), u64 size, u32 hash
// link, u32 name length, then the name padded to 4. The root directory is entry 0, with no name.
#pragma once
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

struct RomImageFile {
    uint32_t path_off, path_len;   // in the paths string: '/'-separated, no leading '/'
    uint64_t data_off, size;       // absolute offset in the image, bytes
};

// rd(offset, buffer, bytes) -> bytes read. False with *why on any header, table or bound failure.
bool romfs_image_index(const std::function<uint64_t(uint64_t, void *, uint64_t)> &rd, uint64_t image_size,
                       std::vector<RomImageFile> *files, std::string *paths, const char **why);
