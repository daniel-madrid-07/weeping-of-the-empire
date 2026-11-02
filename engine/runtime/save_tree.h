// save_tree.h: whether a save commit's snapshot equals the committed save (hle_fs.cpp's atomic commits, Windows).
// The game rewrites its save files with the same bytes (at each start, and at saves that changed nothing). Each such
// Commit used to publish an identical copy and move the committed one into save.backups, so a few starts pushed the
// player's real older backups out (the rival port's #260 / #263 notes saw the same). An equal snapshot is not published.
// Header-only so tools/engine/save_tree_test.cpp builds it without the runtime.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace save_tree_detail {
inline bool skip(const WIN32_FIND_DATAW &e) {   // (reparse points: snap_tree skips them, so they never differ)
    return !wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..") || (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
inline bool is_dir(DWORD a) { return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
inline bool read_some(HANDLE h, std::vector<unsigned char> &buf, DWORD *got) {
    *got = 0;
    while (*got < buf.size()) {
        DWORD k = 0;
        if (!ReadFile(h, buf.data() + *got, (DWORD)buf.size() - *got, &k, nullptr)) return false;
        if (!k) break;
        *got += k;
    }
    return true;
}
// One file of the snapshot against its committed twin: the same file (snap_tree's hard link, the usual case) or, where
// no link was made (a drive without hard links), the same bytes
inline bool same_file(const std::wstring &a, const std::wstring &b) {
    const DWORD sh = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE x = CreateFileW(a.c_str(), GENERIC_READ, sh, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (x == INVALID_HANDLE_VALUE) return false;
    HANDLE y = CreateFileW(b.c_str(), GENERIC_READ, sh, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION ix, iy;
    bool same = false;
    if (y != INVALID_HANDLE_VALUE && GetFileInformationByHandle(x, &ix) && GetFileInformationByHandle(y, &iy)) {
        if (ix.dwVolumeSerialNumber == iy.dwVolumeSerialNumber && ix.nFileIndexHigh == iy.nFileIndexHigh &&
            ix.nFileIndexLow == iy.nFileIndexLow)
            same = true;
        else if (ix.nFileSizeHigh == iy.nFileSizeHigh && ix.nFileSizeLow == iy.nFileSizeLow) {
            std::vector<unsigned char> ba(1u << 16), bb(1u << 16);
            for (same = true; same;) {
                DWORD gx = 0, gy = 0;
                same = read_some(x, ba, &gx) && read_some(y, bb, &gy) && gx == gy && !memcmp(ba.data(), bb.data(), gx);
                if (!gx) break;
            }
        }
    }
    if (y != INVALID_HANDLE_VALUE) CloseHandle(y);
    CloseHandle(x);
    return same;
}
// Every entry of `a` is in `b` with the same kind, and its files are the same (same_file); directories recurse
inline bool covered(const std::wstring &a, const std::wstring &b, bool files) {
    WIN32_FIND_DATAW e;
    HANDLE f = FindFirstFileW((a + L"\\*").c_str(), &e);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (skip(e)) continue;
        const std::wstring x = a + L"\\" + e.cFileName, y = b + L"\\" + e.cFileName;
        const DWORD ya = GetFileAttributesW(y.c_str());
        if (ya == INVALID_FILE_ATTRIBUTES || (ya & FILE_ATTRIBUTE_REPARSE_POINT)) ok = false;
        else if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = is_dir(ya) && covered(x, y, files);
        else ok = !is_dir(ya) && (!files || same_file(x, y));
    } while (ok && FindNextFileW(f, &e));
    FindClose(f);
    return ok;
}
}   // namespace save_tree_detail

// The snapshot `snap` holds exactly the committed save `committed`: the same names and kinds at every depth and every
// file the same file or bytes. False when either is missing or cannot be read (then the commit publishes as before).
inline bool save_tree_same(const std::wstring &snap, const std::wstring &committed) {
    using namespace save_tree_detail;
    if (!is_dir(GetFileAttributesW(snap.c_str())) || !is_dir(GetFileAttributesW(committed.c_str()))) return false;
    return covered(snap, committed, true) && covered(committed, snap, false);
}
#endif
