// pc_input_rec.h: the line format of TOTK_INPUT_RECORD / TOTK_INPUT_SCRIPT (hle_hid.cpp npad_fill), apart so
// tools/engine/test_pc_input_rec.py checks it without the game: "<present> <ms> <buttons hex> <lx> <ly> <rx> <ry>",
// '#' starts a comment.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

struct InputRec {
    uint64_t frame, ms, buttons;   // present count, ms since the first pad read, Npad buttons (no stick-direction bits)
    int32_t lx, ly, rx, ry;        // -32767 .. 32767
};

inline bool input_rec_same(const InputRec& a, const InputRec& b) {
    return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

// one line, with its '\n'
inline void input_rec_write(FILE* f, const InputRec& r) {
    fprintf(f, "%llu %llu %llx %d %d %d %d\n", (unsigned long long)r.frame, (unsigned long long)r.ms,
            (unsigned long long)r.buttons, (int)r.lx, (int)r.ly, (int)r.rx, (int)r.ry);
}

// a recording's states in order (a line that goes back in time, a comment or a malformed line is skipped)
inline std::vector<InputRec> input_rec_read(FILE* f, bool wall) {
    std::vector<InputRec> v;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) {
        unsigned long long fr, ms, b;
        int lx, ly, rx, ry;
        if (buf[0] == '#' || sscanf(buf, "%llu %llu %llx %d %d %d %d", &fr, &ms, &b, &lx, &ly, &rx, &ry) != 7) continue;
        const InputRec r = {fr, ms, b, lx, ly, rx, ry};
        if (v.empty() || (wall ? r.ms >= v.back().ms : r.frame >= v.back().frame)) v.push_back(r);
    }
    return v;
}

// The state at `now` (present count, or ms with wall): the last line at or before it, until the next line's; false
// before the first line and from the last line on (it ends the script). *at: the next line, kept by the caller (now
// only moves forward)
inline bool input_rec_at(const std::vector<InputRec>& v, size_t* at, uint64_t now, bool wall, InputRec* out) {
    while (*at < v.size() && (wall ? v[*at].ms : v[*at].frame) <= now) ++*at;
    if (*at == 0 || *at == v.size()) return false;
    *out = v[*at - 1];
    return true;
}
