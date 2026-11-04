// The Linux RomFS reader (linux-port 2026-11-04, linux-test D#83): what hle_fs.cpp's pack path does on Linux, end to end on
// a real install. romfs-source.txt loaded with rom_source.h's Linux loader (the key stays in its MADV_DONTDUMP page), the
// image in the player's dump read with pread at image_offset + p and decrypted in place (AES-CTR, AES-NI), indexed with
// romfs_image.cpp, then the files the game opens first read whole: each one's size, and zstd's magic for a.zs file. Prints
// counts and RomFS paths only: never the key, the nonce or file bytes.
//   test_romfs_source <romfs-source.txt>      (exit 0: every check passed)
// Built and run by tools/linux/test_romfs_source.sh (WSL / Linux, after a Setup's extraction step).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "rom_source.h"
#include "romfs_image.h"

static uint64_t pread_all(int fd, uint64_t off, void* buf, uint64_t n) {   // (hle_fs.cpp pack_read, Linux)
    uint64_t done = 0;
    while (done < n) {
        const ssize_t r = pread(fd, (uint8_t*)buf + done, (size_t)std::min<uint64_t>(n - done, 1u << 30), (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (uint64_t)r;
    }
    return done;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_romfs_source <romfs-source.txt>\n"); return 2; }
    int fails = 0;
    auto check = [&](const char* what, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; };
    RomSource src;
    std::string why;
    const bool loaded = rom_source_load(argv[1], &src, &why);
    check(loaded ? "romfs-source.txt loaded (Linux loader)" : ("romfs-source.txt: " + why).c_str(), loaded);
    if (!loaded) return 1;
    printf("     dump %s, image %.2f GiB at +%llu, %s\n", src.dump.c_str(), src.image_size / 1073741824.0,
           (unsigned long long)src.image_offset, !src.ctr ? "plain" : src.aesni ? "AES-CTR, AES-NI" : "AES-CTR");
    const int fd = open(src.dump.c_str(), O_RDONLY | O_CLOEXEC);
    check("the dump opens", fd >= 0);
    if (fd < 0) return 1;
    struct stat st;
    check("the dump has the size Setup recorded", fstat(fd, &st) == 0 && (uint64_t)st.st_size == src.dump_size);
    auto rd = [&](uint64_t off, void* buf, uint64_t n) -> uint64_t {   // (hle_fs.cpp rom_read, without the dump-lost exit)
        if (off >= src.image_size) return 0;
        const uint64_t want = std::min<uint64_t>(n, src.image_size - off);
        const uint64_t got = pread_all(fd, src.image_offset + off, buf, want);
        rom_source_decrypt(src, off, buf, got);
        return got;
    };
    uint64_t h0 = 0;
    check("the image decrypts to a RomFS (header size 0x50)", rd(0, &h0, 8) == 8 && h0 == 0x50);
    std::vector<RomImageFile> files;
    std::string paths;
    const char* iwhy = "";
    const bool indexed = romfs_image_index(rd, src.image_size, &files, &paths, &iwhy);
    check(indexed ? "the image's tables index" : iwhy, indexed);
    if (!indexed) return 1;
    printf("     %zu files\n", files.size());
    check("v100's file count (about 304k)", files.size() > 300000);
    // the first files 1.0.0 opens at start (the D#83 log's NOT FOUND list), then two big ones read whole
    const char* const want[] = {"System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs", "System/RegionLangMask.txt",
                                "System/GameRomConfig/NormalProduct.game__RomConfig.bgyml",
                                "System/AddressTable/Product.100.Nin_NX_NVN.atbl.byml.zs",
                                "System/Resource/ResourceSizeTable.Product.100.rsizetable.zs", "Pack/ZsDic.pack.zs",
                                "Pack/Bootup.Nin_NX_NVN.pack.zs"};
    for (const char* w : want) {
        const RomImageFile* f = nullptr;
        for (const RomImageFile& x : files)
            if (std::string_view(paths.data() + x.path_off, x.path_len) == w) { f = &x; break; }
        std::string line = std::string(w) + ": in the index";
        if (!f) { check(line.c_str(), false); continue; }
        std::vector<uint8_t> buf((size_t)f->size);
        const bool whole = rd(f->data_off, buf.data(), f->size) == f->size;
        const std::string_view name(w);
        const bool zs = name.size() > 3 && name.substr(name.size() - 3) == ".zs";
        const bool magic = !zs || (buf.size() >= 4 && buf[0] == 0x28 && buf[1] == 0xb5 && buf[2] == 0x2f && buf[3] == 0xfd);
        line = std::string(w) + ": " + std::to_string(f->size) + " bytes read whole" + (zs ? ", zstd magic" : "");
        check(line.c_str(), whole && magic && f->size > 0);
    }
    close(fd);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
