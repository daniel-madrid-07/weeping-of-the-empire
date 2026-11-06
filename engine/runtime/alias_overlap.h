// hle_os_mem.cpp's AllocateAddressRegion neighbour check, pure so tools/engine/test_alias_overlap.py can run it: the live
// region of `used` (base -> size, std::map) that [a, a + n) overlaps, if any. Only the two neighbours of a can: the
// first region at or above a (overlaps when it starts below a + n) and the one before it (when it ends above a).
#pragma once
#include <iterator>

template <class Map, class U>
inline bool alias_overlap(const Map &used, U a, U n, U *ob, U *on) {
    auto ov = used.lower_bound(a);
    if (!(ov != used.end() && ov->first < a + n)) {
        if (ov == used.begin()) return false;
        --ov;
        if (ov->first + ov->second <= a) return false;
    }
    *ob = ov->first;
    *on = ov->second;
    return true;
}
