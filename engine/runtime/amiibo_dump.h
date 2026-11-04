// amiibo_dump.h: an amiibo dump the player made of their own figure (an NTAG215 image: a 540-byte .bin from a phone /
// emulator tool, or a Flipper Zero .nfc text file) into what nn::nfp hands the game (pc_amiibo.cpp): the tag's UID for
// GetTagInfo and the figure's identification block for GetModelInfo. Only the plaintext parts are read: the UID
// (pages 0-1) and the identification block (page 0x15, bytes 0x54-0x5B); the encrypted figure data is never touched, so
// no keys are needed. Layouts from switchbrew (NFC services: TagInfo 0x58 bytes, ModelInfo 0x40) and 3dbrew (Amiibo).
// Header-only so tools/engine/amiibo_test.cpp checks it without the runtime.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct amiibo_tag {
    uint8_t uid[7];   // NTAG215 serial: bytes 0-2 and 4-7 (3 and 8 are its check bytes)
    uint8_t id[8];    // character (2, big-endian), variant, figure type, model number (2, big-endian), series, 0x02
};

// The image's bytes from a file's contents: binary as it is, or a Flipper .nfc ("Page N: AA BB CC DD" lines)
inline std::vector<uint8_t> amiibo_image(const std::string &file) {
    if (file.compare(0, 9, "Filetype:") && file.find("\nPage 0:") == std::string::npos)
        return std::vector<uint8_t>(file.begin(), file.end());
    std::vector<uint8_t> img;
    for (size_t at = file.find("Page "); at != std::string::npos; at = file.find("Page ", at + 5)) {
        if (at && file[at - 1] != '\n') continue;   // (only lines that start with it)
        char *end = nullptr;
        const long page = strtol(file.c_str() + at + 5, &end, 10);
        if (end == file.c_str() + at + 5 || *end != ':' || page < 0 || page > 255) continue;
        if (img.size() < (size_t)(page + 1) * 4) img.resize((size_t)(page + 1) * 4);
        const char *p = end + 1;
        for (int i = 0; i < 4; i++) {
            while (*p == ' ' || *p == '\t') p++;
            if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
            const char hex[3] = {p[0], p[1], 0};
            img[(size_t)page * 4 + i] = (uint8_t)strtoul(hex, nullptr, 16);
            p += 2;
        }
    }
    return img;
}

// Why a file is no usable amiibo (amiibo_parse; the F10 tab words each)
enum { AMIIBO_OK = 0, AMIIBO_SIZE, AMIIBO_NO_CC, AMIIBO_FORMAT, AMIIBO_UNREADABLE };
// AMIIBO_OK: *tag filled
inline int amiibo_parse(const std::vector<uint8_t> &img, amiibo_tag *tag) {
    if (img.size() < 0x5C || img.size() > 1024) return AMIIBO_SIZE;   // (an NTAG215 image is 540 bytes; 532 / 572 too)
    static const uint8_t cc[4] = {0xF1, 0x10, 0xFF, 0xEE};   // the capability container every amiibo carries (page 3)
    if (memcmp(img.data() + 0x0C, cc, 4)) return AMIIBO_NO_CC;
    if (img[0x5B] != 0x02) return AMIIBO_FORMAT;   // (the identification block's format byte)
    memcpy(tag->uid, img.data(), 3);
    memcpy(tag->uid + 3, img.data() + 4, 4);
    memcpy(tag->id, img.data() + 0x54, 8);
    return AMIIBO_OK;
}

// nn::nfp::TagInfo (0x58): UID (10, here 7 used), its length, protocol (NFC-A) and tag type (Type 2)
inline void amiibo_tag_info(const amiibo_tag &t, uint8_t out[0x58]) {
    memset(out, 0, 0x58);
    memcpy(out, t.uid, 7);
    out[0x0A] = 7;
    const uint32_t protocol = 1, type = 2;
    memcpy(out + 0x20, &protocol, 4);
    memcpy(out + 0x24, &type, 4);
}

// nn::nfp::ModelInfo (0x40): character id and variant in the tag's own byte order, series, the model number in the
// console's (little-endian; the tag stores it big-endian), figure type. The character id went byte-swapped before
// (rival v0.3.7's fix of the same, their #579 / #541: "could not tell figures of one character apart", tag c19b4bec
// runtime/amiibo.c amiibo_get_model_info; the series / type places and the model's order were already as theirs)
inline void amiibo_model_info(const amiibo_tag &t, uint8_t out[0x40]) {
    memset(out, 0, 0x40);
    memcpy(out, t.id, 3);   // character id (2, as on the tag), variant
    out[3] = t.id[6];       // series
    const uint16_t model = (uint16_t)(t.id[4] << 8 | t.id[5]);
    memcpy(out + 4, &model, 2);
    out[6] = t.id[3];       // figure type
}
