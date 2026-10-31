#include <atomic>
#include "totk_version.h"
// HLE: nn::fs on host folders. Mount name -> host dir: the rom mount -> extract/romfs (read-only),
// save/cache/temporary storage -> userdata/<kind>. Paths are "<mount>:/a/b".
#include "hle.h"
#include "pc.h"
#include "cpu_place.h"
#include "romfs_image.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <chrono>
#include <memory>
#include <thread>
#include <condition_variable>
#include <fstream>

#include "rstb_merge.h"
#ifndef _WIN32
#include <unistd.h>   // (getpid: the merged resource size table's name)
#endif
#include <cctype>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "linux/plat_posix.h"
#endif

namespace fsys = std::filesystem;
#ifdef _WIN32
// (a variable set but empty counts as unset: "" was taken as the path, i.e. The current directory)
static const char* env_dir(const char* name, const char* def) { const char* v = getenv(name); return v && *v ? v : def; }
static const char* kRomDir  = env_dir("TOTK_ROMFS", "C:/totk/extract/romfs");
// TOTK_USERDATA=<dir>: saves/cache elsewhere (test runs that start a New Game must not touch the player's saves)
static const char* kUserDir = env_dir("TOTK_USERDATA", "C:/totk/userdata");
// (read at its first use, the game's first RomFS open, not in static init: totk_engine_boot may point it at the layout
// overlay for the canvas's shape, the graphics.aspect_ratio)
static const std::string& overlay_dir() { static const std::string d = env_dir("TOTK_OVERLAY", "C:/totk/overlay/romfs"); return d; }   // tools/rstb_slack.py writes here (U9)
// Skip the opening (user 2026-10-15): while on, overlay/skip_opening/romfs (tools/make_skip_opening.py: the opening's
// event flow jumps straight to its own end-of-prologue chain -> Great Sky Island wake-up) is served before the romfs.
// TOTK_SKIP_OPENING=1 starts with it on; F2 / the pad's Minus toggles it on the title screen (hle_hid.cpp).
static const std::string kSkipDir = getenv("TOTK_SKIP_DIR") ? getenv("TOTK_SKIP_DIR") : "C:/totk/overlay/skip_opening/romfs";
#else
// Linux: no drive letters. TOTK_ROMFS = the extracted romfs (default <exe dir>/data/romfs), TOTK_USERDATA = saves / cache
// (default $XDG_DATA_HOME/totk-recomp/userdata), TOTK_OVERLAY = LayeredFS-style replacement files (default <exe dir>/data/overlay/romfs,
// normally absent), TOTK_SKIP_DIR = the skip-opening overlay (default <exe dir>/data/overlay/skip_opening/romfs). See README in the package.
static std::string env_or(const char* name, const std::string& def) { const char* e = getenv(name); return e && *e ? e : def; }
static std::string data_home() {
    const char* x = getenv("XDG_DATA_HOME"); const char* h = getenv("HOME");
    return (x && *x ? std::string(x) : std::string(h && *h ? h : ".") + "/.local/share") + "/totk-recomp";
}
static const std::string kRomStr  = env_or("TOTK_ROMFS", std::string(rt_posix_exe_dir()) + "/data/romfs");
static const std::string kUserStr = env_or("TOTK_USERDATA", data_home() + "/userdata");
static const char* kRomDir  = kRomStr.c_str();
static const char* kUserDir = kUserStr.c_str();
static const std::string& overlay_dir() { static const std::string d = env_or("TOTK_OVERLAY", std::string(rt_posix_exe_dir()) + "/data/overlay/romfs"); return d; }
static const std::string kSkipDir = env_or("TOTK_SKIP_DIR", std::string(rt_posix_exe_dir()) + "/data/overlay/skip_opening/romfs");
#endif
extern "C" { volatile int g_skip_opening = 0, g_title_phase = 1, g_in_opening = 0; volatile uint64_t g_in_opening_since = 0; }
extern "C" { volatile int g_title_stage = 0; }   // the title's stage (TitleScene_*) is loading or up (hle_vi.cpp TOTK_TITLE_FPS)
static const int skip_init = (g_skip_opening = getenv("TOTK_SKIP_OPENING") && atoi(getenv("TOTK_SKIP_OPENING")) ? 1 : 0);
extern "C" uint64_t nvn_present_count(void);

static const u32 kOk = 0, kPathNotFound = 0x202, kPathExists = 0x402, kOutOfRange = 0x3E02 /* not exact */, kUnsupported = 0x6402;
static const u32 kTargetLocked = 0xE02;   // nn::fs ResultTargetLocked (module 2, description 7)

static std::mutex g_fs_mtx;
static std::unordered_map<std::string, std::string> g_mounts;       // name -> host dir
static bool g_trace_fs = getenv("TOTK_TRACE_FS") != nullptr;
static int g_open_logged;
static std::atomic<u64> g_rd_bytes{0}, g_rd_calls{0}, g_opens_ok{0}, g_rd_ns{0};

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w;
}
// another process holds the file (sync client, antivirus, Explorer preview): retry, then TargetLocked (review N11)
static bool host_locked(DWORD e) { return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED; }
// is `p` an existing file (not a directory)? overlay lookups
static bool host_file_exists(const std::string& p) {
    DWORD at = GetFileAttributesW(widen(p).c_str());
    return at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY);
}
#else
static const std::string& widen(const std::string& s) { return s; }   // (host paths are UTF-8 already)
// Windows file systems ignore case, Linux ones do not: the guest asks for the names the romfs lists, but when a lookup fails
// each path component is matched case-insensitively against its directory (the first hit wins; result cached per path).
static std::mutex g_ci_mtx;
static std::unordered_map<std::string, std::string> g_ci_cache;
static std::string resolve_ci(const std::string& path) {
    {
        std::lock_guard<std::mutex> l(g_ci_mtx);
        auto it = g_ci_cache.find(path);
        if (it != g_ci_cache.end()) return it->second;
    }
    std::string cur = path.size() && path[0] == '/' ? "/" : "";
    size_t i = path.size() && path[0] == '/' ? 1 : 0;
    bool ok = true;
    while (ok && i <= path.size()) {
        size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        std::string cand = cur + (cur.empty() || cur.back() == '/' ? "" : "/") + part;
        struct stat st;
        if (stat(cand.c_str(), &st) == 0) { cur = cand; continue; }
        ok = false;
        if (DIR* d = opendir(cur.empty() ? "." : cur.c_str())) {
            while (dirent* e = readdir(d))
                if (!strcasecmp(e->d_name, part.c_str())) { cur = cur + (cur.empty() || cur.back() == '/' ? "" : "/") + e->d_name; ok = true; break; }
            closedir(d);
        }
    }
    std::string r = ok ? cur : path;
    std::lock_guard<std::mutex> l(g_ci_mtx);
    g_ci_cache[path] = r;
    return r;
}
static bool host_file_exists(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0 && stat(resolve_ci(p).c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
}
static void ci_fix(std::string& p) { struct stat st; if (stat(p.c_str(), &st) != 0) p = resolve_ci(p); }
#endif
#ifdef _WIN32
static void ci_fix(std::string&) {}   // (NTFS is case-insensitive)
#endif
// the load-time flags: on unless set to 0 (unset or empty: on)
static bool fs_flag(const char* name) { const char* e = getenv(name); return !e || !*e || atoi(e) != 0; }
// Does the replacement folder `root` (skip-opening, a mod, a glyph set, the overlay) hold `rel` ("/a/b")?
// TOTK_FS_OVERLAY_INDEX: each folder is listed once, the first time it is asked, and the check is a set lookup
// (case-insensitive, like NTFS and like the Linux fallback above) instead of a GetFileAttributes per folder per open, made
// while host_path holds g_fs_mtx: the game's two loader threads open ~6.6 k files from Continue to the world. A file added to
// a folder while the game runs is not seen.
static bool replacement_exists(const std::string& root, const std::string& rel) {
    static const bool idx = fs_flag("TOTK_FS_OVERLAY_INDEX");
    if (!idx) return host_file_exists(root + rel);
    auto key = [](std::string s) {
        for (auto& ch : s) ch = ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
        return s;
    };
    static std::mutex m;
    static std::unordered_map<std::string, std::unordered_set<std::string>> lists;
    std::lock_guard<std::mutex> l(m);
    auto it = lists.find(root);
    if (it == lists.end()) {
        std::unordered_set<std::string> files;
        std::error_code ec;
        const std::string base = fsys::path(widen(root)).generic_string();
        for (fsys::recursive_directory_iterator d(widen(root), fsys::directory_options::skip_permission_denied, ec), e;
             !ec && d != e; d.increment(ec))
            if (d->is_regular_file(ec)) {
                std::string p = d->path().generic_string();
                if (p.size() > base.size()) files.insert(key(p.substr(base.size())));
            }
        printf("[fs] overlay index %s: %zu files\n", root.c_str(), files.size());
        it = lists.emplace(root, std::move(files)).first;
    }
    return it->second.count(key(rel)) != 0;
}
// Pad glyphs: TOTK_GLYPHS=auto|kbm|xbox|ps serves the UI and font files of a glyph set, the button prompts redrawn for
// the controls in use, ahead of TOTK_OVERLAY. Sets live in TOTK_GLYPHS_DIR (default: <TOTK_OVERLAY>/../../glyphs, i.e.
// <work>/engine/glyphs next to <work>/engine/overlay/romfs): engine+glyphs/: the glyph bank (games/totk/glyphs/bank.py;
// below): every prompt in the game, switched while serving to the keys / mouse / pad buttons bound now, in the style in
// use (hid_glyph_active: TOTK_GLYPHS, else the input used last, else the pads connected). engine+glyphs-<style>/: the
// older fixed sets (the action cluster's masks only, the pc preset's keys), used when there is no bank: auto picks the
// style from the pads seen when the first prompt archive opens. Each set's base.fnv holds the size and FNV-1a 64 of the
// overlay's Common archive it was built on; if TOTK_OVERLAY's copy differs (the overlay was rebuilt) the set is
// skipped, so it never undoes the overlay's own fixes. Default auto (unset or ""); switch, off or 0: the game's own
// prompts. A missing set is logged and the game's prompts stay.
extern "C" const char* hid_glyph_style(void);   // hle_hid.cpp: "ps" / "xbox" / "kbm", or null for the game's own
extern "C" int hid_glyph_active(void);          // hle_hid.cpp: the style in use (0 the game's own, 1 kbm, 2 xbox, 3 ps)
extern "C" const char* hid_glyph_for(int style, const char* control);   // hle_hid.cpp: a control's bank glyph, or null
// The glyph bank: glyphs.txt (bank.py writes it) says where, in the set's two archives (zstd frames of raw blocks, so a
// byte of the archive is at a known file offset), each button glyph's cmap slot (a big-endian glyph id inside the XOR-
// encrypted External.bfotf) and each DoCommand mask (72x72 R8, block-linear) sit; masks.bin has every bank glyph drawn
// into each mask's box. Serving those files, the reads get the slots pointed at, and the masks drawn as, the bank
// glyphs hid_glyph_for names; the game's own where it names none.
struct GlyphBank {
    std::string dir, font, common;
    u64 zhdr = 13, zblock = 131072;
    struct Slot { std::string control; u64 hi, lo; u8 xhi, xlo; u16 orig; u32 toff = ~0u; u16 cp = 0; };   // (toff: in cmap_sig)
    struct Mask { std::string control; u64 start; int w, h, bh, group; bool bc4 = false; };   // (bc4: BC4 blocks, else R8)
    std::vector<Slot> slots;
    std::vector<Mask> masks;
    std::unordered_map<std::string, std::pair<u16, int>> glyphs;   // name -> (glyph id, index in the bank)
    int groups = 0, count = 0, mw = 0, mh = 0;
    std::vector<u8> maskbank;                              // R8 masks: [group][glyph][mh][mw]
    std::vector<u8> maskbank4;                             // BC4 masks: [texture][glyph][bc4_bytes] (blocks row-major)
    int bc4_bytes = 0;
    std::vector<u8> cmap_sig;                              // External's cmap table as built (the game's own glyph ids)
    std::vector<u8> gki_sig;                               // GuideKeyIcon.msbt's TXT2 body as the game loads it
    std::vector<std::pair<u32, std::string>> gki;          // its button characters: (offset in that body, control)
    // DoCommand's diamond, one copy per style (bank.py LETTER_GROUPS / BASE_GROUP): shown for its style, hidden otherwise
    struct Pane {
        std::string name, group, control;                  // group "@s" / "@x" / "@p" (letters), "@b" (the diamond)
        u64 flag_at, text_at;                              // archive offsets: the pane's flag byte, its text (0: none)
        u8 flag, alpha;                                    // as built (hidden)
        u8 place[40];                                      // translate, rotate, scale, size
    };
    std::vector<Pane> panes;
    u64 zoff(u64 i) const { return zhdr + i / zblock * (zblock + 3) + 3 + i % zblock; }
};
static GlyphBank g_bank;   // (loaded once by glyph_dir, read-only after)
static bool bank_load(const std::string& d) {
    FILE* f = fopen((d + "/glyphs.txt").c_str(), "r");
    if (!f) return false;
    GlyphBank b;
    b.dir = d;
    static char line[4096];
    char a[256], c[256];
    std::string bankfile, bank4file;
    int bank4n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long x1, x2, x3; unsigned v1, v2, v3, v4, cp;
        if (sscanf(line, "zstd %llu %llu", &x1, &x2) == 2) { b.zhdr = x1; b.zblock = x2; }
        else if (sscanf(line, "font %255s", a) == 1) b.font = a;
        else if (sscanf(line, "common %255s", a) == 1) b.common = a;
        else if (int k = sscanf(line, "slot %x %255s %llu %u %llu %u %u %u", &cp, a, &x1, &v1, &x2, &v2, &v3, &v4); k >= 7)
            b.slots.push_back({a, x1, x2, (u8)v1, (u8)v2, (u16)v3, k == 8 ? v4 : ~0u, (u16)cp});
        else if (sscanf(line, "glyph %255s %u %x", a, &v1, &v2) == 3) { int i = (int)b.glyphs.size(); b.glyphs[a] = {(u16)v1, i}; }
        else if (sscanf(line, "mask %255s %255s %llu %u %u %u %u", a, c, &x3, &v1, &v2, &v3, &v4) == 7)
            b.masks.push_back({c, x3, (int)v1, (int)v2, (int)v3, (int)v4});
        else if (sscanf(line, "maskbank %255s %u %u %u %u", a, &v1, &v2, &v3, &v4) == 5) {
            bankfile = a; b.groups = (int)v1; b.count = (int)v2; b.mw = (int)v3; b.mh = (int)v4;
        }
        else if (sscanf(line, "mask4 %255s %255s %llu %u %u %u %u", a, c, &x3, &v1, &v2, &v3, &v4) == 7) {
            GlyphBank::Mask m{c, x3, (int)v1, (int)v2, (int)v3, (int)v4};
            m.bc4 = true;
            b.masks.push_back(m);
        }
        else if (sscanf(line, "maskbank4 %255s %u %u %u", a, &v1, &v2, &v3) == 4) { bank4file = a; bank4n = (int)v1; b.bc4_bytes = (int)v3; }
        else if (!strncmp(line, "guidekeyicon ", 13) || !strncmp(line, "cmaptable ", 10)) {
            std::vector<u8>& to = line[0] == 'g' ? b.gki_sig : b.cmap_sig;
            for (const char* h = strchr(line, ' ') + 1; isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1]); h += 2)
                to.push_back((u8)strtoul(std::string(h, 2).c_str(), nullptr, 16));
        }
        else if (sscanf(line, "gki %u %255s", &v1, a) == 2) b.gki.push_back({v1, a});
        else if (char g[8], ctl[8], pl[96]; sscanf(line, "pane %255s %7s %7s %llu %llu %u %u %95s", a, g, ctl, &x1, &x2, &v1, &v2, pl) == 8 &&
                 strlen(pl) == 80) {
            GlyphBank::Pane p{a, g, ctl, x1, x2, (u8)v1, (u8)v2, {}};
            for (int k = 0; k < 40; k++) p.place[k] = (u8)strtoul(std::string(pl + 2 * k, 2).c_str(), nullptr, 16);
            b.panes.push_back(p);
        }
    }
    fclose(f);
    if (b.font.empty() || b.common.empty() || b.slots.empty() || b.count != (int)b.glyphs.size()) {
        printf("[fs] glyph bank %s: glyphs.txt incomplete\n", d.c_str());
        return false;
    }
    if (FILE* m = fopen((d + "/" + bankfile).c_str(), "rb")) {
        b.maskbank.resize((size_t)b.groups * b.count * b.mw * b.mh);
        if (fread(b.maskbank.data(), 1, b.maskbank.size(), m) != b.maskbank.size()) b.maskbank.clear();
        fclose(m);
    }
    if (!bank4file.empty())
        if (FILE* m = fopen((d + "/" + bank4file).c_str(), "rb")) {
            b.maskbank4.resize((size_t)bank4n * b.count * b.bc4_bytes);
            if (fread(b.maskbank4.data(), 1, b.maskbank4.size(), m) != b.maskbank4.size()) b.maskbank4.clear();
            fclose(m);
        }
    // a mask without its bank is dropped (the game's own stays; the text glyphs still switch)
    b.masks.erase(std::remove_if(b.masks.begin(), b.masks.end(), [&](const GlyphBank::Mask& m) {
        return m.bc4 ? b.maskbank4.empty() || (m.w / 4) * (m.h / 4) * 8 != b.bc4_bytes
                     : b.maskbank.empty() || m.w != b.mw || m.h != b.mh;
    }), b.masks.end());
    g_bank = std::move(b);
    return true;
}
static void glyph_live_served(const std::string& hp, bool common, int style);   // (below)
static void glyph_live_font(const std::vector<u16>& ids);                       // (below)
// Byte patches over a served file: sorted offsets and their bytes (host_pread applies them).
struct FilePatch { std::vector<u64> off; std::vector<u8> val; };
static u64 r8_block_linear(int x, int y, int w, int bh) {   // offset of byte x of row y, rows w bytes wide (block-linear)
    u64 gx = (u64)(w + 63) / 64;
    u64 gob = (u64)(x % 64 / 32) * 256 + (u64)(y % 8 / 2) * 64 + (u64)(x % 32 / 16) * 32 + (u64)(y % 2) * 16 + x % 16;
    return ((u64)(y / (8 * bh)) * gx + (u64)x / 64) * 512 * bh + (u64)(y % (8 * bh) / 8) * 512 + gob;
}
// A mask's picture of bank glyph gi, and every byte of it at its offset in the texture.
static const u8* mask_src(const GlyphBank::Mask& m, int gi) {
    const GlyphBank& b = g_bank;
    return m.bc4 ? &b.maskbank4[((size_t)m.group * b.count + gi) * b.bc4_bytes] : &b.maskbank[((size_t)m.group * b.count + gi) * b.mw * b.mh];
}
template <class F> static void mask_each(const GlyphBank::Mask& m, const u8* src, F put) {
    if (!m.bc4) {
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) put(r8_block_linear(x, y, m.w, m.bh), src[y * m.w + x]);
        return;
    }
    const int bw = m.w / 4, rows = m.h / 4;   // 8-byte blocks of 4x4 texels, laid out like 8-byte texels
    for (int by = 0; by < rows; by++)
        for (int bx = 0; bx < bw; bx++)
            for (int k = 0; k < 8; k++) put(r8_block_linear(bx * 8 + k, by, bw * 8, m.bh), src[(by * bw + bx) * 8 + k]);
}
// DoCommand's diamond: the copy shown for a style (keys and mouse: none, the key masks say it all)
static bool pane_shown(const GlyphBank::Pane& p, int style) {
    const char g = p.group.size() > 1 ? p.group[1] : 0;
    return g == 'b' ? style != 1 : g == 's' ? style == 0 : g == 'x' ? style == 2 : g == 'p' && style == 3;
}
// a letter copy's character: the bank glyph of the button bound in its style, else the game's own letter glyph
static u32 pane_codepoint(const GlyphBank::Pane& p) {
    const char* g = hid_glyph_for(p.group == "@x" ? 2 : 3, p.control.c_str());
    auto it = g ? g_bank.glyphs.find(g) : g_bank.glyphs.end();
    const char* abxy = strchr("ABXY", p.control[0]);
    return it != g_bank.glyphs.end() ? 0xF100 + (u32)it->second.second : 0xF040 + (u32)(abxy ? abxy - "ABXY" : 0);
}
static std::shared_ptr<const FilePatch> glyph_patches(const std::string& hp) {
    const GlyphBank& b = g_bank;
    if (b.dir.empty()) return nullptr;
    bool font = hp == b.dir + "/" + b.font, common = hp == b.dir + "/" + b.common;
    if (!font && !common) return nullptr;
    const int style = hid_glyph_active();
    std::vector<std::pair<u64, u8>> v;
    int changed = 0;
    auto glyph = [&](const std::string& control) -> const std::pair<u16, int>* {
        const char* g = hid_glyph_for(style, control.c_str());
        if (!g) return nullptr;
        auto it = b.glyphs.find(g);
        if (it == b.glyphs.end()) { printf("[fs] glyph bank: no glyph %s (%s)\n", g, control.c_str()); return nullptr; }
        return &it->second;
    };
    if (font) {
        std::vector<u16> ids;
        for (auto& s : b.slots) {
            auto* g = glyph(s.control);
            u16 id = g ? g->first : s.orig;
            changed += g != nullptr;
            ids.push_back(id);
            v.push_back({s.hi, (u8)((id >> 8) ^ s.xhi)});
            v.push_back({s.lo, (u8)((id & 0xFF) ^ s.xlo)});
        }
        glyph_live_font(ids);
    }
    if (common) {
        for (auto& m : b.masks) {
            auto* g = glyph(m.control);
            if (!g) continue;   // the game's own mask stays
            changed++;
            mask_each(m, mask_src(m, g->second), [&](u64 off, u8 val) { v.push_back({b.zoff(m.start + off), val}); });
        }
        for (auto& p : b.panes) {
            v.push_back({b.zoff(p.flag_at), (u8)(p.flag | (pane_shown(p, style) ? 1 : 0))});
            if (!p.text_at) continue;
            const u32 cp = pane_codepoint(p);
            v.push_back({b.zoff(p.text_at), (u8)cp});
            v.push_back({b.zoff(p.text_at + 1), (u8)(cp >> 8)});
        }
    }
    glyph_live_served(hp, common, style);
    std::sort(v.begin(), v.end(), [](const std::pair<u64, u8>& l, const std::pair<u64, u8>& r) { return l.first < r.first; });
    auto p = std::make_shared<FilePatch>();
    for (auto& e : v) { p->off.push_back(e.first); p->val.push_back(e.second); }
    static const char* const names[] = {"the game's own", "keyboard / mouse", "Xbox", "PlayStation"};
    printf("[fs] glyph bank: %s as %s (%d of %d glyphs from the bank)\n", font ? "font" : "Common archive",
           names[style & 3], changed, font ? (int)b.slots.size() : (int)b.masks.size());
    return p;
}
// Live switching: when the style in use or a binding changes after the files were served (a pad picked up, a key
// rebound in the overlay; hid_glyph_epoch), the copies the game holds are rewritten in memory: the DoCommand masks
// (found by the bytes they were served as; the texture cache sees the write and uploads them again), GuideKeyIcon's
// characters (a message's button tag takes its character from there, so prompts laid out afterwards show the bank glyph
// at its own code point, which the font maps for good) and External's cmap slots (the decrypted table the font engine
// reads). Literal button characters already drawn (the menu tabs' L / R, the HUD's ability L) keep the style of the
// boot: the font engine keeps the glyph it drew for a code point. DoCommand's diamond has a copy per style instead, one
// shown (pane_shown). All copies sit in the alias region (found in ~3 s); they are looked for a minute and a half after
// the Common archive was served, and at each change for those not found or gone (the pane objects exist once the HUD is
// built).
struct GlyphLive {
    std::mutex m;
    bool served = false, located = false;
    std::chrono::steady_clock::time_point served_at;
    unsigned epoch = 0;                         // what the game's copies show now
    std::vector<std::vector<u8>> mask_orig;     // per mask: the game's own texture
    std::vector<std::vector<u8>> mask_now;      // per mask: the texture as the game has it now
    std::vector<std::vector<u8*>> mask_at;      // where it is
    std::vector<u8*> gki_at;
    std::vector<u8> gki_now;
    std::vector<u8*> cmap_at;                   // External's cmap where the font engine reads it (decrypted)
    std::vector<u8> cmap_now;
    std::vector<std::vector<u8*>> pane_obj;     // per DoCommand pane copy: its flag byte in the pane objects built
    std::vector<std::vector<u8*>> pane_res;     // per copy: its name in the layout resource (the archive as loaded)
    std::vector<std::vector<u8*>> pane_hits;    // (a search's hits of its name)
    bool pane_dumped = false;
    struct MsgChar { u8* at; u16 cp; int slot; u16 now; };   // a button character written out in a message text
    std::vector<MsgChar> msg;
    bool msg_looked = false;
};
static GlyphLive g_live;
extern "C" unsigned hid_glyph_epoch(void);   // hle_hid.cpp
extern "C" u64 g_rt_heap_base;                // hle_os_mem.cpp
extern "C" u64 kernel_heap_size(void);        // hle_os_mem.cpp: committed bytes from g_rt_heap_base
extern "C" void rt_alias_carved(u64* base, u64* size);   // hle_os_mem.cpp: the alias region carved so far
static u64 mask_bytes(const GlyphBank::Mask& m) {   // the texture's size (rows of 64-byte GOBs, blocks of bh GOBs)
    const int wb = m.bc4 ? m.w / 4 * 8 : m.w, rows = m.bc4 ? m.h / 4 : m.h;
    return (u64)((wb + 63) / 64) * (u64)((rows + 8 * m.bh - 1) / (8 * m.bh)) * 512 * m.bh;
}
static void mask_draw(std::vector<u8>& tex, const GlyphBank::Mask& m, const u8* src) {
    mask_each(m, src, [&](u64 off, u8 val) { tex[off] = val; });
}
// the bank glyph's index for a control in a style, or -1
static int glyph_index(int style, const std::string& control) {
    const char* g = hid_glyph_for(style, control.c_str());
    auto it = g ? g_bank.glyphs.find(g) : g_bank.glyphs.end();
    return it == g_bank.glyphs.end() ? -1 : it->second.second;
}
static u32 glyph_codepoint(int index) {   // the bank's code points: U+F100 + index (bank.py BANK_BASE)
    return 0xF100 + (u32)index;
}
// the game's own glyph of a button character, at a code point no slot moves (bank.py: U+E0xx -> U+F0xx)
static u32 game_codepoint(u32 cp) { return cp - 0xE000 + 0xF000; }
// The font engine (nn::fontll, lifted sdk code) maps a code point through tables it built at LoadFont, so the cmap slots
// rewritten after the boot do not reach it, and the characters already laid out (the pause menu's tabs, the HUD's
// ability button) kept the boot's style. Its glyph calls from the game's font cache are wrapped instead: a slot's code
// point (U+E040 ..) is drawn as the bank glyph of the style in force now, at the bank's own code point, which the font
// maps for good (the game's own glyph at U+F0xx). With the cache reset on a change (font_cache_reset) every glyph
// already laid out is drawn again that way.
// (a table per glyph epoch: every prompt's characters come through here at layout, and glyph_index builds the bindings)
static u32 glyph_font_code(u32 cp) {
    if (cp < 0xE040 || cp > 0xE068 || g_bank.dir.empty()) return cp;
    static std::mutex m;
    static unsigned memo_epoch = ~0u;
    static u32 memo[0xE068 - 0xE040 + 1];
    const unsigned e = hid_glyph_epoch();
    std::lock_guard<std::mutex> l(m);
    if (e != memo_epoch) {
        const int style = hid_glyph_active();
        bool set[0xE068 - 0xE040 + 1] = {};
        for (u32 i = 0; i <= 0xE068 - 0xE040; i++) memo[i] = 0xE040 + i;
        for (auto& s : g_bank.slots)
            if (s.cp >= 0xE040 && s.cp <= 0xE068 && !set[s.cp - 0xE040]) {   // (the first slot of a code point, as before)
                set[s.cp - 0xE040] = true;
                const int gi = glyph_index(style, s.control);
                memo[s.cp - 0xE040] = gi >= 0 ? glyph_codepoint(gi) : game_codepoint(s.cp);
            }
        memo_epoch = e;
    }
    return memo[cp - 0xE040];
}
// The System tab's controls diagram: its icon panes, show the gameplay set's key (PCX_WORLD's table, through kbm_glyph
// like every prompt) at the bank glyph's own code point, which no live remap moves; a button with no key there keeps
// the game's own pad glyph (U+F0xx).
extern "C" const char* hid_glyph_for_set(int style, const char* control, int set);   // hle_hid.cpp
extern "C" unsigned pc_glyph_sig_in(int set);                                       // pc_input.cpp
extern "C" unsigned pc_glyph_epoch_in(int set) { return hid_glyph_epoch() * 31u ^ pc_glyph_sig_in(set); }
extern "C" unsigned pc_glyph_gameplay_epoch(void) { return pc_glyph_epoch_in(0); }   // (pc_input.h's PCX_WORLD)
extern "C" uint16_t pc_glyph_cp_in(int set, uint16_t cp) {
    constexpr int kKbm = 1, kSets = 64, kCps = 0xE068 - 0xE040 + 1;   // (hle_hid's GlyphKbm; kSets > PCX_COUNT)
    if (cp < 0xE040 || cp > 0xE068 || g_bank.dir.empty() || set < 0 || set >= kSets) return cp;
    static std::mutex m;
    static unsigned memo_epoch[kSets];
    static bool memo_ok[kSets];
    static uint16_t memo[kSets][kCps];
    const unsigned e = pc_glyph_epoch_in(set);
    std::lock_guard<std::mutex> l(m);
    if (!memo_ok[set] || e != memo_epoch[set]) {
        const bool kbm = hid_glyph_active() == kKbm;
        bool done[kCps] = {};
        for (int i = 0; i < kCps; i++) memo[set][i] = (uint16_t)(0xE040 + i);
        for (auto& s : g_bank.slots)
            if (kbm && s.cp >= 0xE040 && s.cp <= 0xE068 && !done[s.cp - 0xE040]) {   // (a code point's first slot)
                done[s.cp - 0xE040] = true;
                const char* g = hid_glyph_for_set(kKbm, s.control.c_str(), set);
                auto it = g ? g_bank.glyphs.find(g) : g_bank.glyphs.end();
                memo[set][s.cp - 0xE040] = (uint16_t)(it == g_bank.glyphs.end() ? game_codepoint(s.cp) : glyph_codepoint(it->second.second));
            }
        memo_epoch[set] = e;
        memo_ok[set] = true;
    }
    return memo[set][cp - 0xE040];
}
extern "C" uint16_t pc_glyph_gameplay_cp(uint16_t cp) { return pc_glyph_cp_in(0, cp); }
static std::atomic<bool> g_font_wrapped{false};   // the wrapped font engine has drawn (glyph_live_write's GuideKeyIcon)
// ScalableFontEngine::AcquireGlyphmap(this, u32 code, u16 format) -> the glyph's bitmap
HLE_WRAP(fontll_AcquireGlyphmap, "_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt") {
    g_font_wrapped.store(true, std::memory_order_relaxed);
    c->x[1] = glyph_font_code((u32)c->x[1]);
    wrap_orig_fontll_AcquireGlyphmap(c);
}
// ScalableFontEngine::GetAdvance(this, s16*, s16*, s32*, s32*, u32 code, u16 format): the width laid out for it
HLE_WRAP(fontll_GetAdvance, "_ZN2nn6fontll18ScalableFontEngine10GetAdvanceEPsS2_PiS3_jt") {
    c->x[5] = glyph_font_code((u32)c->x[5]);
    wrap_orig_fontll_GetAdvance(c);
}
// The cmap table with these glyph ids in its slots (big endian), in the order of g_bank.slots
static std::vector<u8> cmap_with(const std::vector<u16>& ids) {
    std::vector<u8> t = g_bank.cmap_sig;
    for (size_t i = 0; i < ids.size() && i < g_bank.slots.size(); i++) {
        u32 o = g_bank.slots[i].toff;
        if (o + 1 < t.size()) { t[o] = (u8)(ids[i] >> 8); t[o + 1] = (u8)ids[i]; }
    }
    return t;
}
// glyph_patches: the font was served with these ids in the slots
static void glyph_live_font(const std::vector<u16>& ids) {
    std::lock_guard<std::mutex> l(g_live.m);
    if (!g_bank.cmap_sig.empty()) g_live.cmap_now = cmap_with(ids);
    g_live.cmap_at.clear();
}
// glyph_patches: the Common archive was served; keep its masks as the game got them
static void glyph_live_served(const std::string& hp, bool common, int style) {
    std::lock_guard<std::mutex> l(g_live.m);
    if (!common) return;
    const GlyphBank& b = g_bank;
    g_live.mask_orig.assign(b.masks.size(), {});
    g_live.mask_now.assign(b.masks.size(), {});
    g_live.mask_at.assign(b.masks.size(), {});
    FILE* f = fopen(hp.c_str(), "rb");
    if (!f) return;
    for (size_t i = 0; i < b.masks.size(); i++) {
        const auto& m = b.masks[i];
        u64 n = mask_bytes(m), lo = b.zoff(m.start), hi = b.zoff(m.start + n - 1) + 1;
        std::vector<u8> raw(hi - lo), tex(n);
        if (fseek(f, (long)lo, SEEK_SET) || fread(raw.data(), 1, raw.size(), f) != raw.size()) { fclose(f); return; }
        for (u64 k = 0; k < n; k++) tex[k] = raw[b.zoff(m.start + k) - lo];
        g_live.mask_orig[i] = tex;
        int gi = glyph_index(style, m.control);
        if (gi >= 0) mask_draw(tex, m, mask_src(m, gi));
        g_live.mask_now[i] = tex;
    }
    fclose(f);
    g_live.gki_now = b.gki_sig;
    g_live.pane_obj.assign(b.panes.size(), {});
    g_live.pane_res.assign(b.panes.size(), {});
    g_live.pane_hits.assign(b.panes.size(), {});
    g_live.epoch = hid_glyph_epoch();
    g_live.served_at = std::chrono::steady_clock::now();
    g_live.served = true;
}
#ifdef _WIN32
// Several patterns looked for in one pass over committed, readable memory: each by an 8-byte key (its most varied
// 4-aligned window) at 4-byte steps, then compared whole. The guest heap first, then, for the patterns not found there,
// the rest of the process (what the HLE's allocator gave the game is host memory).
// (wide: looked for in the rest of the process too when not in the guest heap or the alias region)
// the __except filter of the guarded reads below: an access violation is the expected fault; anything else is a crash
#define FS_AV_ONLY (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
struct MemFind { const u8* pat; size_t len, w; u64 key; std::vector<u8*>* at; bool wide = true; };
static int scan_range(u8* p, u8* end, const MemFind* f, int nf, const u8* bloom, u8** hit, int* hit_f, int max_hits) {
    int n = 0;
    pc_fault_guard(1);   // (pc.h: else rt_main's vectored crash handler takes the fault before the __except)
    __try {   // (a region freed under the scan ends it)
        for (u8* q = (u8*)(((uintptr_t)p + 3) & ~(uintptr_t)3); q + 8 <= end; q += 4) {
            u64 v;
            memcpy(&v, q, 8);
            if (!(bloom[(v & 0xFFFF) >> 3] & (1 << (v & 7)))) continue;
            for (int i = 0; i < nf; i++)
                if (v == f[i].key && q - f[i].w >= p && q - f[i].w + f[i].len <= end && n < max_hits &&
                    !memcmp(q - f[i].w, f[i].pat, f[i].len))
                    hit[n] = q - f[i].w, hit_f[n++] = i;
        }
    } __except (FS_AV_ONLY) {}
    pc_fault_guard(0);
    return n;
}
static void mem_find(std::vector<MemFind>& f, const std::vector<std::pair<const u8*, size_t>>& mine) {
    if (f.empty()) return;
    std::vector<u8> bloom(8192);
    for (auto& e : f) {
        int best = -1;
        for (size_t o = 0; o + 8 <= e.len; o += 4) {
            bool seen[256] = {};
            int d = 0;
            for (size_t k = 0; k < 8; k++) d += !seen[e.pat[o + k]], seen[e.pat[o + k]] = true;
            if (d > best) best = d, e.w = o;
        }
        memcpy(&e.key, e.pat + e.w, 8);
        bloom[(e.key & 0xFFFF) >> 3] |= (u8)(1 << (e.key & 7));
    }
    u8* const heap = (u8*)g_rt_heap_base;
    u8* const heap_end = heap + kernel_heap_size();
    auto pass = [&](u8* lo, u8* hi, bool skip_heap) {
        std::vector<MemFind> want;
        for (auto& e : f) if (e.at->empty() && (e.wide || !skip_heap)) want.push_back(e);
        if (want.empty()) return;
        u8* hit[64]; int hit_f[64];
        for (u8* p = lo; p < hi;) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof mi)) break;
            u8* r_end = std::min(hi, (u8*)mi.BaseAddress + mi.RegionSize);
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            bool in_heap = p < heap_end && r_end > heap;
            if (mi.State == MEM_COMMIT && mi.Type != MEM_IMAGE && (mi.Protect & readable) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                !(skip_heap && in_heap)) {
                int n = scan_range(p, r_end, want.data(), (int)want.size(), bloom.data(), hit, hit_f, 64);
                for (int i = 0; i < n; i++) {
                    bool ours = false;   // (this file's own copies: the patterns, the textures as served)
                    for (auto& r : mine) ours |= hit[i] >= r.first && hit[i] < r.first + r.second;
                    if (!ours) want[hit_f[i]].at->push_back(hit[i]);
                }
            }
            p = r_end;
        }
    };
    u64 ab = 0, an = 0;
    rt_alias_carved(&ab, &an);
    pass(heap, heap_end, false);
    if (ab && an) pass((u8*)ab, (u8*)(ab + an), false);
    bool missing = false;
    for (auto& e : f) missing |= e.at->empty() && e.wide;
    if (missing) {   // anywhere else in the process (slow: 2 minutes)
        printf("[fs] glyph bank live: not all in the guest heap or the alias region; looking through the process\n");
        pass((u8*)0x10000, (u8*)0x7FFFFFFF0000ull, true);
    }
}
// DoCommand's pane copies in memory, found by name. In the layout resource (the archive as loaded) the name sits in the
// pane's section: flag and alpha 4 and 2 bytes before it, translate .. size 32 bytes after. In a pane object built from
// it (nn::ui2d::Pane) translate .. size come before the name, followed by the flag and alpha bytes; a match of all three
// is the object (else nothing is written).
static bool pane_resource(const u8* h, const GlyphBank::Pane& p) {
    bool r;
    pc_fault_guard(1);
    __try { r = (h[-4] & 0xFE) == (p.flag & 0xFE) && h[-2] == p.alpha && !memcmp(h + 32, p.place, 40); }
    __except (FS_AV_ONLY) { r = false; }
    pc_fault_guard(0);
    return r;
}
// (an object's flag byte compared on the layout's bits only, influenced alpha and location adjust: the runtime sets
// others, e.g. bit 4 on N_LoopA_00's panes)
static bool pane_object_ok(const u8* flag, const GlyphBank::Pane& p) {   // flag: the object's flag byte
    bool r;
    pc_fault_guard(1);
    __try { r = (flag[0] & 0x06) == (p.flag & 0x06) && flag[1] == p.alpha && !memcmp(flag - 40, p.place, 40); }
    __except (FS_AV_ONLY) { r = false; }
    pc_fault_guard(0);
    return r;
}
static u8* pane_object_flag(u8* h, const GlyphBank::Pane& p) {   // h: the name in a pane object
    for (u8* q = (u8*)((uintptr_t)(h - 42) & ~(uintptr_t)3); q >= h - 0x200; q -= 4)
        if (pane_object_ok(q + 40, p)) return q + 40;
    return nullptr;
}
static void pane_dump(const u8* h) {   // (a name hit that is neither: the bytes before it, to see the object's layout)
    char s[0x400];
    int n = snprintf(s, sizeof s, "[fs] glyph bank live: pane name at %p, the 0x110 bytes before:", (const void*)h);
    pc_fault_guard(1);
    __try {
        for (int k = -0x110; k < 0 && n < (int)sizeof s - 4; k++) n += snprintf(s + n, sizeof s - n, "%s%02x", k % 16 ? "" : " ", h[k]);
    } __except (FS_AV_ONLY) {}
    pc_fault_guard(0);
    printf("%s\n", s);
}
// Message texts with a button character written out instead of a button tag (the pause menu's tabs, the HUD's ability
// button, shop, pouch and dialog hints: 16 LayoutMsg files): the characters are given the bank glyph's code point on
// a change, like GuideKeyIcon's, so the texts laid out afterwards follow. The language's Mals archive as loaded is the
// SARC around GuideKeyIcon's copy; each MSBT's TXT2 strings are walked (tags skipped) for the slots' code points.
static const u8* sarc_around(const u8* inside, u32* size) {
    const u8* p = (const u8*)((uintptr_t)inside & ~(uintptr_t)3);
    const u8* r = nullptr;
    pc_fault_guard(1);
    __try {
        for (const u8* q = p; q > p - (64u << 20); q -= 4)
            if (q[0] == 'S' && q[1] == 'A' && q[2] == 'R' && q[3] == 'C' && q[6] == 0xFF && q[7] == 0xFE) {
                u32 n;
                memcpy(&n, q + 8, 4);
                if (q + n > inside) { *size = n; r = q; break; }
            }
    } __except (FS_AV_ONLY) {}
    pc_fault_guard(0);
    return r;
}
static bool msg_put(u8* at, u16 expect, u16 v) {   // the character at at, if it is still what was written last
    bool r = false;
    pc_fault_guard(1);
    __try {
        u16 o;
        memcpy(&o, at, 2);
        if (o == expect) { memcpy(at, &v, 2); r = true; }
    } __except (FS_AV_ONLY) { r = false; }
    pc_fault_guard(0);
    return r;
}
// The game's font glyph cache (nn::font::TextureCache, one, owned by eui::ScalableFontMgr) keeps every glyph it drew by
// code point, so a character already laid out (the pause menu's tabs, the HUD's ability button) kept the boot's style
// after the cmap slots were rewritten. ScalableFontMgr::update (main+0x83e2f4, v1.0.0) resets the cache itself when its
// byte +0x3e is set (ResetTextureCache, the alternate glyphs registered again, the generation +0x38 counted on), on its
// own thread between frames: that byte is set, after a check of the singleton's vtable (main+0x44fd948).
static u64 font_mgr_request() {   // the manager, its reset byte set; 0 when there is none (yet)
    const u64 inst = totk_main(0x463fef0), vtbl = totk_main(0x44fd948);
    u64 r = 0;
    pc_fault_guard(1);
    __try {
        const u64 mgr = *(const volatile u64*)inst;
        if (mgr && *(const volatile u64*)mgr == vtbl) { *(volatile u8*)(mgr + 0x3e) = 1; r = mgr; }
    } __except (FS_AV_ONLY) { r = 0; }
    pc_fault_guard(0);
    return r;
}
static bool font_cache_reset() {
    const u64 mgr = font_mgr_request();
    if (!mgr) return false;
    std::thread([mgr] {   // (the manager taking it: +0x3e back to 0, the generation +0x38 counted on)
        const u32 g0 = *(const volatile u32*)(mgr + 0x38);
        // its cost on screen: the longest gap between presents from the request to 300 ms after the reset was taken,
        // against the median gap of that window (each 1 ms look at the count)
        using clk = std::chrono::steady_clock;
        std::vector<double> gaps;
        u64 n_last = nvn_present_count();
        clk::time_point t_last = clk::now(), taken_at{};
        int taken_ms = -1;
        for (int ms = 0; ms <= 3300; ms++) {
            const u64 n = nvn_present_count();
            const clk::time_point t = clk::now();
            if (n != n_last) { gaps.push_back(std::chrono::duration<double, std::milli>(t - t_last).count() / (double)(n - n_last)); n_last = n; t_last = t; }
            if (taken_ms < 0 && !*(const volatile u8*)(mgr + 0x3e)) { taken_ms = ms; taken_at = t; }
            if (taken_ms >= 0 && t - taken_at >= std::chrono::milliseconds(300)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (taken_ms >= 0) {
            std::vector<double> s = gaps;
            std::sort(s.begin(), s.end());
            printf("[fs] glyph bank live: font glyph cache reset taken after %d ms (generation %u -> %u); presents from the request "
                   "to 300 ms after: %zu, longest gap %.1f ms, median %.1f ms\n", taken_ms, g0, *(const volatile u32*)(mgr + 0x38),
                   gaps.size(), s.empty() ? 0.0 : s.back(), s.empty() ? 0.0 : s[s.size() / 2]);
            return;
        }
        printf("[fs] glyph bank live: font glyph cache reset not taken in 3 s (+0x3c %u, +0x3d %u, generation %u)\n",
               *(const volatile u8*)(mgr + 0x3c), *(const volatile u8*)(mgr + 0x3d), *(const volatile u32*)(mgr + 0x38));
    }).detach();
    return true;
}
static bool readable(const u8* p, size_t n) {   // committed and readable through [p, p + n)
    for (const u8* q = p; q < p + n;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(q, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        q = (const u8*)mi.BaseAddress + mi.RegionSize;
    }
    return true;
}
static void msg_literals(const u8* gki) {
    auto rd16 = [](const u8* p) { u16 v; memcpy(&v, p, 2); return v; };
    auto rd32 = [](const u8* p) { u32 v; memcpy(&v, p, 4); return v; };
    u32 n = 0;
    const u8* s = sarc_around(gki, &n);
    if (!s || n < 0x40 || !readable(s, n)) return;
    const u32 hdr = rd16(s + 4), data = rd32(s + 12);
    const u8* sfat = s + hdr;
    if (memcmp(sfat, "SFAT", 4)) return;
    const u32 count = rd16(sfat + 6);
    const u8* names = sfat + 12 + (size_t)count * 16 + 8;
    if (names > s + n || memcmp(names - 8, "SFNT", 4)) return;
    int files = 0;
    for (u32 i = 0; i < count; i++) {
        const u8* e = sfat + 12 + (size_t)i * 16;
        const u8* name = names + (size_t)(rd32(e + 4) & 0xFFFFFF) * 4;
        const u32 lo = rd32(e + 8), hi = rd32(e + 12);
        if (name >= s + n || (u64)data + hi > n || hi < lo + 0x30) continue;
        const size_t nl = strnlen((const char*)name, (const char*)(s + n) - (const char*)name);
        if (nl < 5 || memcmp(name + nl - 5, ".msbt", 5) || strstr((const char*)name, "GuideKeyIcon")) continue;
        const u8* m = s + data + lo, *m_end = s + data + hi;
        if (memcmp(m, "MsgStdBn", 8) || m[8] != 0xFF || m[9] != 0xFE) continue;
        const u8* sec = m + 0x20;
        for (u32 k = rd16(m + 0x0E); k-- && sec + 0x10 <= m_end;) {
            const u32 size = rd32(sec + 4);
            const u8* body = sec + 0x10, *b_end = std::min(body + size, m_end);
            if (!memcmp(sec, "TXT2", 4) && body + 4 <= b_end) {
                const u32 strs = rd32(body);
                const size_t before = g_live.msg.size();
                for (u32 t = 0; t < strs && body + 8 + 4 * (size_t)t <= b_end; t++) {
                    const u8* c = body + rd32(body + 4 + 4 * t);
                    const u8* c_end = t + 1 < strs ? body + rd32(body + 8 + 4 * t) : b_end;
                    for (c_end = std::min(c_end, b_end); c + 2 <= c_end;) {
                        const u16 ch = rd16(c);
                        if (ch == 0) break;
                        if (ch == 0x0E) { c += c + 8 <= c_end ? 8 + rd16(c + 6) : 8; continue; }   // tag
                        if (ch == 0x0F) { c += 6; continue; }                                     // tag end
                        for (size_t sl = 0; sl < g_bank.slots.size(); sl++)
                            if (g_bank.slots[sl].cp == ch) { g_live.msg.push_back({(u8*)c, ch, (int)sl, ch}); break; }
                        c += 2;
                    }
                }
                files += g_live.msg.size() > before;
            }
            sec += (size + 0x10 + 15) & ~(size_t)15;
        }
    }
    printf("[fs] glyph bank live: %zu button characters written out in %d message files (Mals at %p, %u bytes)\n",
           g_live.msg.size(), files, (const void*)s, n);
}
// The copies found again: those still holding what they were last given; a search for the rest (search).
static void glyph_live_locate(bool search = true) {
    std::vector<MemFind> f;
    auto keep = [&](std::vector<u8*>& at, const std::vector<u8>& now) {
        std::vector<u8*> ok;
        for (u8* a : at) if (!memcmp(a, now.data(), now.size())) ok.push_back(a);
        at = ok;
        if (at.empty() && now.size() >= 16) f.push_back({now.data(), now.size(), 0, 0, &at});
    };
    for (size_t i = 0; i < g_live.mask_now.size(); i++) if (!g_live.mask_now[i].empty()) keep(g_live.mask_at[i], g_live.mask_now[i]);
    keep(g_live.gki_at, g_live.gki_now);
    keep(g_live.cmap_at, g_live.cmap_now);
    const size_t copies = f.size();
    // DoCommand's pane copies: the objects and the resource copy still there; a search by name while either is missing
    // (the objects exist once the HUD is built, and are built again with it)
    std::vector<size_t> pane_search;
    for (size_t i = 0; i < g_bank.panes.size() && i < g_live.pane_obj.size(); i++) {
        const auto& p = g_bank.panes[i];
        auto& obj = g_live.pane_obj[i];
        auto& res = g_live.pane_res[i];
        obj.erase(std::remove_if(obj.begin(), obj.end(), [&](u8* a) { return !pane_object_ok(a, p); }), obj.end());
        res.erase(std::remove_if(res.begin(), res.end(), [&](u8* a) { return !pane_resource(a, p); }), res.end());
        if (!obj.empty() && !res.empty()) continue;
        g_live.pane_hits[i].clear();
        MemFind e{(const u8*)p.name.c_str(), p.name.size() + 1, 0, 0, &g_live.pane_hits[i]};
        e.wide = false;   // (guest memory only)
        f.push_back(e);
        pane_search.push_back(i);
    }
    if (f.empty() || !search) return;
    std::vector<std::pair<const u8*, size_t>> mine = {{g_bank.gki_sig.data(), g_bank.gki_sig.size()},
                                                      {g_live.gki_now.data(), g_live.gki_now.size()},
                                                      {g_bank.cmap_sig.data(), g_bank.cmap_sig.size()},
                                                      {g_live.cmap_now.data(), g_live.cmap_now.size()}};
    for (auto* v : {&g_live.mask_orig, &g_live.mask_now}) for (auto& t : *v) mine.push_back({t.data(), t.size()});
    for (auto& p : g_bank.panes) mine.push_back({(const u8*)p.name.c_str(), p.name.size() + 1});
    const auto t0 = std::chrono::steady_clock::now();
    mem_find(f, mine);
    if (!pane_search.empty()) {
        int objs = 0, res = 0;
        const u8* stray = nullptr;
        for (size_t i : pane_search) {
            const auto& p = g_bank.panes[i];
            auto add = [](std::vector<u8*>& v, u8* a) { if (std::find(v.begin(), v.end(), a) == v.end()) v.push_back(a); };
            const u8* other = nullptr;
            for (u8* h : g_live.pane_hits[i]) {
                if (pane_resource(h, p)) add(g_live.pane_res[i], h);
                else if (u8* fl = pane_object_flag(h, p)) add(g_live.pane_obj[i], fl);
                else other = h;
            }
            if (g_live.pane_obj[i].empty() && other && !stray) {
                stray = other;
                printf("[fs] glyph bank live: %s: %zu hits, none a pane object\n", p.name.c_str(), g_live.pane_hits[i].size());
            }
        }
        std::string missing;
        for (size_t i = 0; i < g_bank.panes.size(); i++) {
            objs += !g_live.pane_obj[i].empty(), res += !g_live.pane_res[i].empty();
            if (g_live.pane_obj[i].empty() && missing.size() < 300) missing += " " + g_bank.panes[i].name;
        }
        printf("[fs] glyph bank live: DoCommand pane copies: %d of %zu in pane objects, %d in the layout resource (%zu looked for)%s%s\n",
               objs, g_bank.panes.size(), res, pane_search.size(), missing.empty() ? "" : "; no object:", missing.c_str());
        if (stray && !g_live.pane_dumped) { pane_dump(stray); g_live.pane_dumped = true; }
        f.resize(copies);
        if (f.empty()) return;
    }
    u8* const heap = (u8*)g_rt_heap_base;
    int found = 0;
    std::string where;
    for (auto& e : f) {
        found += !e.at->empty();
        for (u8* a : *e.at) {
            char s[64];
            u64 ab = 0, an = 0;
            rt_alias_carved(&ab, &an);
            snprintf(s, sizeof s, " %p%s", (void*)a, a >= heap && a < heap + kernel_heap_size() ? "(heap)"
                                                   : (u64)a >= ab && (u64)a < ab + an ? "(alias)" : "(host)");
            if (where.size() < 600) where += s;
        }
    }
    g_live.located = true;
    printf("[fs] glyph bank live: %d of %zu copies found in %.0f ms:%s\n", found, f.size(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), where.c_str());
}
// The game's copies (those located) made to show the style and bindings in force now; true when some are not located.
static bool glyph_live_write(int style, bool log) {
    const GlyphBank& b = g_bank;
    int masks = 0, mask_places = 0;
    for (size_t i = 0; i < b.masks.size() && i < g_live.mask_now.size(); i++) {
        const auto& m = b.masks[i];
        if (g_live.mask_now[i].empty() || g_live.mask_at[i].empty()) continue;
        std::vector<u8> tex = g_live.mask_orig[i];
        int gi = glyph_index(style, m.control);
        if (gi >= 0) mask_draw(tex, m, mask_src(m, gi));
        if (tex == g_live.mask_now[i]) continue;
        for (u8* a : g_live.mask_at[i]) memcpy(a, tex.data(), tex.size());
        g_live.mask_now[i] = tex;
        masks++;
        mask_places += (int)g_live.mask_at[i].size();
    }
    // GuideKeyIcon keeps the game's characters, the slots' code points, once the wrapped font engine draws: it draws a slot
    // in the style in force, so a prompt already laid out (an open menu's bottom guide) changes with the font cache reset
    // like the tabs. Without the wrap, the bank glyph's own code point of the style (prompts laid out afterwards only).
    std::vector<u8> gki = b.gki_sig;
    for (auto& e : b.gki) {
        if (g_font_wrapped.load(std::memory_order_relaxed)) break;
        if (e.first + 1 >= gki.size()) continue;
        int gi = glyph_index(style, e.second);
        u32 cp = gi >= 0 ? glyph_codepoint(gi) : game_codepoint(gki[e.first] | (u32)gki[e.first + 1] << 8);
        gki[e.first] = (u8)cp; gki[e.first + 1] = (u8)(cp >> 8);
    }
    bool gki_done = false;
    if (!g_live.gki_at.empty() && gki != g_live.gki_now) {
        for (u8* a : g_live.gki_at) memcpy(a, gki.data(), gki.size());
        g_live.gki_now = gki;
        gki_done = true;
    }
    // the button characters written out in message texts (found once, around GuideKeyIcon's copy); TOTK_GLYPHS_MESSAGES=1
    // (tests; off: the texts that carry them, LayoutMsg's, are bound when their layouts are built, at boot for the pause
    // menu and the HUD)
    static const bool msgs = getenv("TOTK_GLYPHS_MESSAGES") && !strcmp(getenv("TOTK_GLYPHS_MESSAGES"), "1");
    if (msgs && !g_live.msg_looked && !g_live.gki_at.empty()) {
        g_live.msg_looked = true;
        msg_literals(g_live.gki_at[0]);
    }
    int msg_done = 0, msg_gone = 0;
    for (auto& mc : g_live.msg) {
        const int gi = glyph_index(style, b.slots[mc.slot].control);
        const u16 cp = (u16)(gi >= 0 ? glyph_codepoint(gi) : game_codepoint(mc.cp));
        if (cp == mc.now) continue;
        if (msg_put(mc.at, mc.now, cp)) mc.now = cp, msg_done++;
        else msg_gone++;
    }
    if (msg_gone * 2 > (int)g_live.msg.size()) g_live.msg.clear(), g_live.msg_looked = false;   // (reloaded: look again)
    // the font's slots too: literal button characters laid out from now on, and any the font engine draws again
    std::vector<u16> ids;
    for (auto& s : b.slots) {
        const char* g = hid_glyph_for(style, s.control.c_str());
        auto it = g ? b.glyphs.find(g) : b.glyphs.end();
        ids.push_back(it != b.glyphs.end() ? it->second.first : s.orig);
    }
    std::vector<u8> cm = cmap_with(ids);
    bool cmap_done = false;
    if (!g_live.cmap_at.empty() && !g_live.cmap_now.empty() && cm != g_live.cmap_now) {
        for (u8* a : g_live.cmap_at) memcpy(a, cm.data(), cm.size());
        g_live.cmap_now = cm;
        cmap_done = true;
    }
    // and the glyphs already drawn drawn again, through the wrapped font engine (glyph_font_code) in the new style
    // (TOTK_GLYPHS_FONT_RESET=0: not). The game takes the reset in ~20 ms.
    static const bool no_reset = getenv("TOTK_GLYPHS_FONT_RESET") && !strcmp(getenv("TOTK_GLYPHS_FONT_RESET"), "0");
    const bool reset = log && !no_reset && font_cache_reset();   // (once a change: the first write)
    if (log && !no_reset) printf("[fs] glyph bank live: font glyph cache reset %s\n", reset ? "requested" : "not possible (no ScalableFontMgr)");
    // DoCommand's diamond: the copy of this style shown, the others hidden, in the panes built and in the resource the
    // next ones are built from (with the letters of the buttons bound now)
    int panes = 0, pane_flips = 0;
    for (size_t i = 0; i < b.panes.size() && i < g_live.pane_obj.size(); i++) {
        const auto& p = b.panes[i];
        const u8 vis = pane_shown(p, style) ? 1 : 0;
        panes += !g_live.pane_obj[i].empty();
        for (u8* a : g_live.pane_obj[i])
            if ((*a & 1) != vis) *a = (u8)((*a & 0xFE) | vis), pane_flips++;
        for (u8* h : g_live.pane_res[i]) {
            h[-4] = (u8)((h[-4] & 0xFE) | vis);
            if (!p.text_at) continue;
            const u32 cp = pane_codepoint(p);
            u8* t = h + (p.text_at - p.flag_at - 4);
            t[0] = (u8)cp; t[1] = (u8)(cp >> 8);
        }
    }
    if (log || masks || gki_done || cmap_done || pane_flips || msg_done)
        printf("[fs] glyph bank live: style %d: %d masks rewritten (%d places), GuideKeyIcon %s (%zu places), font slots %s (%zu "
               "places), DoCommand %d of %zu pane copies (%d shown / hidden), messages %d characters rewritten (%d gone); tick %llu\n",
               style, masks, mask_places, gki_done ? "rewritten" : "unchanged", g_live.gki_at.size(),
               cmap_done ? "rewritten" : "unchanged", g_live.cmap_at.size(), panes, b.panes.size(), pane_flips, msg_done, msg_gone,
               (unsigned long long)GetTickCount64());   // (tick: against pc_input's context lines, the rewrite's latency)
    bool missing = g_live.gki_at.empty() || (!g_live.cmap_now.empty() && g_live.cmap_at.empty()) || panes < (int)b.panes.size();
    for (size_t i = 0; i < g_live.mask_now.size(); i++) missing |= !g_live.mask_now[i].empty() && g_live.mask_at[i].empty();
    return missing;
}
// A change: the copies known are written at once, then the others looked for (a scan of seconds) and written.
static void glyph_live_apply(unsigned epoch) {
    std::lock_guard<std::mutex> l(g_live.m);
    const int style = hid_glyph_active();
    glyph_live_locate(false);
    if (glyph_live_write(style, true)) {
        glyph_live_locate(true);
        glyph_live_write(style, false);
    }
    g_live.epoch = epoch;
}
// pc_input's glyph intent (a key press naming the set it puts up): the rewrite at once, not at the next 20 ms look and
// without the settle look, so the font cache reset is asked for before the game lays out the screen the press opens (it
// reads the press a frame or more later)
extern "C" void pc_glyph_kick_register(void (*fn)(void));   // pc_input.cpp
static std::mutex g_kick_mu;
static std::condition_variable g_kick_cv;
static bool g_kicked;
static void glyph_kick() {
    { std::lock_guard<std::mutex> l(g_kick_mu); g_kicked = true; }
    g_kick_cv.notify_one();
}
static void glyph_live_start() {
    static const bool off = getenv("TOTK_GLYPHS_LIVE") && !strcmp(getenv("TOTK_GLYPHS_LIVE"), "0");
    if (off) return;
    pc_glyph_kick_register(glyph_kick);
    std::thread([] {
        unsigned seen = 0;
        for (;;) {   // (every 20 ms: a change applies after two equal looks, 20-40 ms; it was 50 ms, ~100-150 ms of the old
                     // prompts at a menu / mount switch with the reset; 250 ms before that
            bool kicked;
            {
                std::unique_lock<std::mutex> l(g_kick_mu);
                g_kick_cv.wait_for(l, std::chrono::milliseconds(20), [] { return g_kicked; });
                kicked = g_kicked;
                g_kicked = false;
            }
            if (!g_live.served) continue;
            if (!g_live.located && std::chrono::steady_clock::now() - g_live.served_at > std::chrono::seconds(90)) {
                std::lock_guard<std::mutex> l(g_live.m);
                glyph_live_locate();
                g_live.located = true;
            }
            unsigned e = hid_glyph_epoch();
            static ULONGLONG seen_at;  // (when the new epoch was first seen: the glyph switch's latency)
            if (e == g_live.epoch) { seen = e; continue; }
            if (e != seen) {   // (the same for two looks: a burst settles first; an intent's change is meant, at once)
                seen = e;
                seen_at = GetTickCount64();
                if (!kicked) continue;
            }
            const ULONGLONG t0 = GetTickCount64();
            glyph_live_apply(e);
            static int tlogs;
            if (tlogs++ < 200)
                printf("[fs] glyph epoch %08x: seen at tick %llu, rewrite at %llu took %llu ms\n", e, (unsigned long long)seen_at,
                       (unsigned long long)t0, (unsigned long long)(GetTickCount64() - t0));
        }
    }).detach();
}
#else
static void glyph_live_start() {}
#endif
// The resource size table: the bank's two archives are larger than the game's, and the game sizes a resource's buffer
// from this table (a larger font archive overran its buffer at boot), so the bank carries the overlay's table with those
// two entries raised and is served it (base.fnv's "rstb" line: the overlay table it was built on).
static const char* const kGlyphRstb = "System/Resource/ResourceSizeTable.Product.100.rsizetable.zs";
static std::pair<u64, u64> fnv_file(const std::string& path) {   // (size, FNV-1a 64); size 0 when missing
    u64 h = 0xcbf29ce484222325ull, n = 0;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        unsigned char buf[65536]; size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) { n += r; for (size_t i = 0; i < r; i++) h = (h ^ buf[i]) * 0x100000001b3ull; }
        fclose(f);
    }
    return std::make_pair(n, h);
}
static bool glyph_base_ok(const std::string& d) {   // base.fnv matches the overlay's Common archive (and its RSTB)
    static const std::pair<u64, u64> have = fnv_file(overlay_dir() + "/UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs");
    unsigned long long size = 0, want = 0, rsize = 0, rwant = 0;
    if (FILE* f = fopen((d + "/base.fnv").c_str(), "r")) {
        if (fscanf(f, "%llu %llx", &size, &want) != 2) size = 0;
        if (fscanf(f, " rstb %llu %llx", &rsize, &rwant) != 2) rsize = 0;
        fclose(f);
    }
    if (!size) { printf("[fs] glyphs: no set at %s (base.fnv missing)\n", d.c_str()); return false; }
    if (size != have.first || want != have.second) {
        printf("[fs] glyphs: %s was built on another overlay archive (size %llu, now %llu): skipped\n", d.c_str(), size,
               (unsigned long long)have.first);
        return false;
    }
    if (rsize) {
        static const std::pair<u64, u64> rhave = fnv_file(overlay_dir() + "/" + kGlyphRstb);
        if (rsize != rhave.first || rwant != rhave.second) {
            printf("[fs] glyphs: %s was built on another resource size table (size %llu, now %llu): skipped\n", d.c_str(),
                   rsize, (unsigned long long)rhave.first);
            return false;
        }
    }
    return true;
}
static const std::string& glyph_dir() {
    static std::mutex m;
    static bool done;
    static std::string dir;
    std::lock_guard<std::mutex> l(m);
    if (done) return dir;
    done = true;
    const char* e = getenv("TOTK_GLYPHS");
    if (!e || !*e) e = "auto";
    if (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "switch")) return dir;
    const char* gd = getenv("TOTK_GLYPHS_DIR");
    const std::string od = overlay_dir(), base = gd && *gd ? gd : od + "/../../glyphs";
    // the bank built on the overlay in use: an aspect overlay (graphics.aspect_ratio, romfs-aspect-<w>x<h>) has its own
    // Common archive, so its own bank, engine+glyphs@aspect-<w>x<h> (build.py glyph_bank); the 16:9 one engine+glyphs
    std::string bank = base + "/engine+glyphs";
    if (const size_t a = od.find("aspect-"); a != std::string::npos) {
        size_t e = a + 7;
        while (e < od.size() && (isdigit((unsigned char)od[e]) || od[e] == 'x')) e++;
        bank += "@" + od.substr(a, e - a);
    }
    if (FILE* t = fopen((bank + "/glyphs.txt").c_str(), "r")) {
        fclose(t);
        if (glyph_base_ok(bank) && bank_load(bank)) {
            dir = g_bank.dir;
            glyph_live_start();
            printf("[fs] glyph bank %s: %zu glyphs, %zu slots, %zu masks, %zu DoCommand pane copies; style now %d\n",
                   dir.c_str(), g_bank.glyphs.size(), g_bank.slots.size(), g_bank.masks.size(), g_bank.panes.size(),
                   hid_glyph_active());
            return dir;
        }
    }
    const char* style = strcmp(e, "auto") ? e : hid_glyph_style();
    if (!style) { printf("[fs] glyphs auto: Nintendo pad, the game's own prompts\n"); return dir; }
    std::string d = base + "/engine+glyphs-" + style;
    if (!glyph_base_ok(d)) return dir;
    dir = d;
    printf("[fs] glyphs %s%s: %s\n", style, strcmp(e, "auto") ? "" : " (auto)", d.c_str());
    return dir;
}
// A list of RomFS directories in an environment variable (';' on Windows, ':' elsewhere), each logged as "[fs] <what> <dir>"
static std::vector<std::string> mod_dirs(const char* var, const char* what) {
    std::vector<std::string> v; const char* e = getenv(var);
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (std::string m = e ? e : ""; !m.empty();) {
        size_t c = m.find(sep); std::string d = m.substr(0, c);
        while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        if (!d.empty()) { v.push_back(d); printf("[fs] %s %s\n", what, d.c_str()); }
        if (c == std::string::npos) break;
        m = m.substr(c + 1);
    }
    return v;
}
static const std::vector<std::string>& user_mods() {   // TOTK_USER_MODS, read once
    static const std::vector<std::string> v = mod_dirs("TOTK_USER_MODS", "user mod");
    return v;
}
// Where a game path is on the host, without the mods' resource size tables merged (host_path below adds that)
static bool host_path_ours(const char* p, std::string* out) {
    std::string s = p; size_t k = s.find(':');
    if (k == std::string::npos) return false;
    std::lock_guard<std::mutex> l(g_fs_mtx);
    auto it = g_mounts.find(s.substr(0, k));
    if (it == g_mounts.end()) return false;
    *out = it->second + s.substr(k + 1);
    static const bool no_overlay = getenv("TOTK_NO_OVERLAY") != nullptr;   // A/B: run on the pristine romfs
    std::string rel = s.substr(k + 1);
    if (it->second == kRomDir && rel.find("Event/EventFlow/OpeningEvent.bfevfl") != std::string::npos) {
        printf("[fs] OpeningEvent opened at present %llu: skip opening %s\n", (unsigned long long)nvn_present_count(), g_skip_opening ? "ON" : "OFF");
        g_title_phase = 0;
    }
    if (it->second == kRomDir && (rel.rfind("/Banc/NormalStage/", 0) == 0 || rel.rfind("/Bake/Scene/", 0) == 0)) {
        // stage files: the under-castle opening is the OpeningField stage (in-game skip button, hle_hid.cpp)
        static std::set<std::string> seen;
        if (seen.insert(rel).second) printf("[fs] stage file %s at present %llu" "\n", rel.c_str(), (unsigned long long)nvn_present_count());
        bool op = rel.find("OpeningField") != std::string::npos;
        // any stage but the title's own (TitleScene_*) ends the title: Continue / Load go straight to MainField without
        // opening OpeningEvent, and the skip-opening banner stayed up in gameplay (user screenshot 2026-10-15 15:50)
        if (g_title_phase && rel.find("TitleScene") == std::string::npos) {
            g_title_phase = 0;
            printf("[fs] title over: stage %s at present %llu\n", rel.c_str(), (unsigned long long)nvn_present_count());
        } else if (g_title_phase) g_title_stage = 1;
        if (op && !g_in_opening) { g_in_opening = 1; g_in_opening_since = nvn_present_count(); g_title_phase = 0; printf("[fs] entered the opening stage" "\n"); }
        else if (!op && g_in_opening) { g_in_opening = 0; printf("[fs] left the opening stage (%s)" "\n", rel.c_str()); }
    }
    {   // measure what marks the end of the title screen (Continue / Load / New Game)
        static int n = 0;
        if (g_title_phase && n < 40 && (rel.find("Banc/") != std::string::npos || it->second != kRomDir)) {
            n++; printf("[fs] title-phase open %s at present %llu\n", p, (unsigned long long)nvn_present_count());
        }
    }
    if (it->second == kRomDir && g_skip_opening && !no_overlay) {
        std::string sk = kSkipDir + rel;
        if (replacement_exists(kSkipDir, rel)) {
            printf("[fs] skip-opening overlay %s\n", sk.c_str());
            *out = sk;
            return true;
        }
    }
    if (it->second == kRomDir && !no_overlay) {    // TOTK_MODS: our RomFS sets (totk_engine.c puts them here), first listed wins
        // shortcut: replacement files only (a mod's NEW files don't show in directory listings, no RSTB merge); fine for
        // the usual texture / model / param replacements, upgrade when a mod needs added files.
        static const std::vector<std::string> mods = mod_dirs("TOTK_MODS", "mod");
        for (const auto& d : mods) {
            std::string mp = d + rel;
            if (replacement_exists(d, rel)) {
                static int logged; if (logged++ < 64) printf("[fs] mod file %s\n", mp.c_str());
                *out = mp;
                return true;
            }
        }
    }
    static const bool glyphs = true;   // (TOTK_GLYPHS: glyph_dir decides; on by default)
    if (it->second == kRomDir && !no_overlay && glyphs && (rel.rfind("/UI/", 0) == 0 || rel.rfind("/Font/", 0) == 0 ||
                                                       rel == "/" + std::string(kGlyphRstb))) {   // pad glyphs (above)
        const std::string& gd = glyph_dir();
        std::string gp = gd + rel;
        if (!gd.empty() && replacement_exists(gd, rel)) {
            static int logged; if (logged++ < 8) printf("[fs] glyph file %s\n", gp.c_str());
            *out = gp;
            return true;
        }
    }
    if (it->second == kRomDir && !no_overlay) {    // LayeredFS-style overlay: an existing file overlay/romfs/<path> replaces the romfs one
        std::string ov = overlay_dir() + s.substr(k + 1);
        if (replacement_exists(overlay_dir(), rel)) {
            static int logged; if (logged++ < 32) printf("[fs] overlay %s\n", ov.c_str());
            *out = ov;
            return true;
        }
    }
    // TOTK_USER_MODS (the launchers' mods folder): the player's mods come after our sets, the glyph bank and the overlay,
    // so a mod cannot take away the port's own files (PC glyphs, the game-over and quick-menu fixes, our RSTB); among
    // themselves the first listed wins. Their resource size tables are merged into the winner's instead (host_path below)
    if (it->second == kRomDir && !no_overlay && rel != "/" + std::string(kGlyphRstb)) {
        for (const auto& d : user_mods()) {
            if (replacement_exists(d, rel)) {
                std::string mp = d + rel;
                static int logged; if (logged++ < 64) printf("[fs] user mod file %s\n", mp.c_str());
                *out = mp;
                return true;
            }
        }
    }
    return true;
}

static std::vector<u8> read_host_file(const std::string& p) {
    std::vector<u8> v;
#ifdef _WIN32
    FILE* f = _wfopen(widen(p).c_str(), L"rb");
#else
    FILE* f = fopen(p.c_str(), "rb");
#endif
    if (!f) return v;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
            v.resize((size_t)n);
            if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
        }
    }
    fclose(f);
    return v;
}

// The resource size table the game gets when the player's mods bring their own: the one that wins without them (ours: the
// overlay's, or our sets' combined one) with every mod's entries merged in at the larger size (rstb_merge.h), written once
// per start as a stored zstd frame to the temp folder. "" (serve `base`) without mod tables, or when one does not read as
// a table.
static std::string rstb_merged(const std::string& base) {
    const std::string rel = "/" + std::string(kGlyphRstb);
    rstb::Table t;
    std::vector<u8> raw;
    if (!rstb::unzstd(read_host_file(base), &raw) || !rstb::parse(raw, &t)) {
        printf("[fs] resource size table %s: not readable, mods' tables not merged\n", base.c_str());
        return "";
    }
    int n = 0;
    rstb::Counts c;
    for (const auto& d : user_mods()) {
        if (!replacement_exists(d, rel)) continue;
        rstb::Table m;
        if (!rstb::unzstd(read_host_file(d + rel), &raw) || !rstb::parse(raw, &m) || !rstb::merge(t, m, &c)) {
            printf("[fs] resource size table of mod %s: not a table this game reads, left out\n", d.c_str());
            continue;
        }
        n++;
    }
    if (!n) return "";
    std::error_code ec;
    const fsys::path dir = fsys::temp_directory_path(ec);
    if (ec) return "";
    // One file per game process, written beside and renamed into place; the ones a day old (games that were killed)
    // are removed here
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    // (increment(ec), not a range-for: its operator++ throws on an iteration error, here inside call_once
    for (fsys::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        std::error_code e2;
        if (n.rfind("totk-rstb-merged-", 0) == 0 &&
            fsys::last_write_time(it->path(), e2) < fsys::file_time_type::clock::now() - std::chrono::hours(24) && !e2)
            fsys::remove(it->path(), e2);
    }
    ec.clear();
    const fsys::path out = dir / ("totk-rstb-merged-" + std::to_string(pid) + ".zs"), tmp = fsys::path(out) += ".tmp";
    const std::vector<u8> z = rstb::zstd_stored(rstb::serialize(t));
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write((const char*)z.data(), (std::streamsize)z.size());
        if (!f.good()) { f.close(); fsys::remove(tmp, ec); return ""; }
    }
    fsys::rename(tmp, out, ec);
    if (ec) { fsys::remove(tmp, ec); return ""; }
    const std::u8string u8 = out.u8string();
    const std::string path(u8.begin(), u8.end());
    printf("[fs] resource size table: %s + %d mod table(s): %zu entries raised, %zu added -> %s\n", base.c_str(), n, c.raised,
           c.added, path.c_str());
    return path;
}

// Every lookup but the resource size table's is host_path_ours; that one gets the mods' tables merged in (rstb_merged)
static bool host_path(const char* p, std::string* out) {
    if (!host_path_ours(p, out)) return false;
    static const std::string rstb_rel = "/" + std::string(kGlyphRstb);
    const char* colon = strchr(p, ':');
    if (!colon || rstb_rel != colon + 1 || user_mods().empty() || getenv("TOTK_NO_OVERLAY")) return true;
    {
        std::lock_guard<std::mutex> l(g_fs_mtx);
        auto it = g_mounts.find(std::string(p, (size_t)(colon - p)));
        if (it == g_mounts.end() || it->second != kRomDir) return true;
    }
    static std::once_flag once;
    static std::string merged;
    std::call_once(once, [&] { merged = rstb_merged(*out); });
    if (!merged.empty()) *out = merged;
    return true;
}
bool hle_fs_host_path(const char* p, std::string* out) { bool r = host_path(p, out); if (r) ci_fix(*out); return r; }   // hle_movie.cpp (movie files)
static void mount(const char* name, const std::string& dir) {
    fsys::create_directories(widen(dir));
    std::lock_guard<std::mutex> l(g_fs_mtx); g_mounts[name] = dir;
    printf("[fs] mount %s: -> %s\n", name, dir.c_str());
}

// ---- handles
#ifdef _WIN32
typedef HANDLE FH;
static const FH kNoFile = INVALID_HANDLE_VALUE, kClosed = nullptr;
#else
typedef int FH;                                   // file descriptor
static const FH kNoFile = -1, kClosed = -1;
// A file the guest opened for reading. Linux opens a directory with O_RDONLY, Windows CreateFileW refuses: refuse it here too.
static FH fh_open(const std::string& path, int mode) {
    int fl = ((mode & 2) ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    int fd = open(path.c_str(), fl);
    if (fd < 0 && errno == ENOENT) fd = open(resolve_ci(path).c_str(), fl);
    if (fd < 0) return kNoFile;
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { close(fd); return kNoFile; }
    return fd;
}
#endif
#ifdef _WIN32
// ---- atomic save commits The save mount is this process's working copy <userdata>/save.work.<pid>, made from the
// committed save <userdata>/save at the first MountSaveData. Only nn::fs::Commit publishes it:
//   1. save.new gets the working tree (a file whose bytes equal the committed one is a hard link to it, any other is
//      copied and flushed), then the marker save.new.complete;
//   2. save.new is renamed to save.ready (the commit is complete on disk);
//   3. save moves to save.backups/<local time> and save.ready is renamed to save.
// A crash at any point leaves the old or the new commit, never a mix: the next start finishes a.ready (or a complete.new)
// and deletes an incomplete.new. Writes not committed are lost on a crash, as on the console. Committed and backup files
// are never written in place, so the hard links between them are safe. The last TOTK_SAVE_BACKUPS (default 10, 0 = none)
// earlier commits are kept. A commit that cannot be published (a file held open by another process) leaves the marker
// save.work.<pid>.pending and is retried at the next Commit; the next start publishes it if the working copy was not
// written after the marker. Several processes on one userdata each get their own working copy (a sibling.lock file held
// open for the process's life) and one named mutex orders commits. On by default since n10c/n10d (2026-10-23: a new game
// saved through it, 8 commits in 0-31 ms, and Continue reloaded it); TOTK_SAVE_JOURNAL=0 = the old in-place writes, for
// A/Bs.
static const bool g_journal = !(getenv("TOTK_SAVE_JOURNAL") && !strcmp(getenv("TOTK_SAVE_JOURNAL"), "0"));
static std::string g_save_work;                     // the save mount's host dir (UTF-8), set by save_init
static std::wstring g_committed, g_work, g_backups;
static HANDLE g_save_mutex, g_work_lock;
static std::mutex g_commit_mtx;                     // this process's commits and the copy buffers
static std::vector<u8> g_cbuf_a(1u << 20), g_cbuf_b(1u << 20);
static bool g_commit_pending;
static const bool g_save_async = !(getenv("TOTK_SAVE_ASYNC") && !strcmp(getenv("TOTK_SAVE_ASYNC"), "0"));
static std::mutex g_pub_mtx;
static std::condition_variable g_pub_cv;
static bool g_pub_busy;
static void wait_publisher(DWORD ms = INFINITE) {
    std::unique_lock<std::mutex> l(g_pub_mtx);
    if (ms == INFINITE) g_pub_cv.wait(l, [] { return !g_pub_busy; });
    else g_pub_cv.wait_for(l, std::chrono::milliseconds(ms), [] { return !g_pub_busy; });
}

static bool wexists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool remove_tree(const std::wstring& p) { std::error_code ec; fsys::remove_all(p, ec); return !ec; }
static bool read_all(HANDLE h, std::vector<u8>& buf, DWORD* got) {
    *got = 0;
    while (*got < buf.size()) {
        DWORD k = 0;
        if (!ReadFile(h, buf.data() + *got, (DWORD)buf.size() - *got, &k, nullptr)) return false;
        if (!k) break;
        *got += k;
    }
    return true;
}
// dst := a copy of src (dst must not exist), flushed unless `flush` is false (the async publish flushes it later). The
// source may be open in the game (shared read/write).
static bool copy_flushed(const std::wstring& src, const std::wstring& dst, bool flush = true) {
    HANDLE s = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s == INVALID_HANDLE_VALUE) return false;
    HANDLE d = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = d != INVALID_HANDLE_VALUE;
    for (DWORD got = 0; ok;) {
        ok = read_all(s, g_cbuf_a, &got);
        if (!ok || !got) break;
        DWORD put = 0; ok = WriteFile(d, g_cbuf_a.data(), got, &put, nullptr) && put == got;
    }
    ok = ok && (!flush || FlushFileBuffers(d));
    if (d != INVALID_HANDLE_VALUE) CloseHandle(d);
    CloseHandle(s);
    return ok;
}
static bool same_bytes(const std::wstring& a, const std::wstring& b) {
    const DWORD sh = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE x = CreateFileW(a.c_str(), GENERIC_READ, sh, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (x == INVALID_HANDLE_VALUE) return false;
    HANDLE y = CreateFileW(b.c_str(), GENERIC_READ, sh, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER sx, sy;
    bool same = y != INVALID_HANDLE_VALUE && GetFileSizeEx(x, &sx) && GetFileSizeEx(y, &sy) && sx.QuadPart == sy.QuadPart;
    while (same) {
        DWORD gx = 0, gy = 0;
        same = read_all(x, g_cbuf_a, &gx) && read_all(y, g_cbuf_b, &gy) && gx == gy && !memcmp(g_cbuf_a.data(), g_cbuf_b.data(), gx);
        if (!gx) break;
    }
    if (y != INVALID_HANDLE_VALUE) CloseHandle(y);
    CloseHandle(x);
    return same;
}
// The new directory dst gets work's tree; files equal to those in committed are hard links to them (reparse points,
// which the game cannot make, are skipped). With `copied`, copies are not flushed and their paths are collected.
static bool snap_tree(const std::wstring& work, const std::wstring& committed, const std::wstring& dst,
                      std::vector<std::wstring>* copied = nullptr) {
    if (!CreateDirectoryW(dst.c_str(), nullptr)) return false;
    WIN32_FIND_DATAW e; HANDLE f = FindFirstFileW((work + L"\\*").c_str(), &e);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..") || (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring w = work + L"\\" + e.cFileName, c = committed + L"\\" + e.cFileName, d = dst + L"\\" + e.cFileName;
        if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = snap_tree(w, c, d, copied);
        else if (same_bytes(w, c) && CreateHardLinkW(d.c_str(), c.c_str(), nullptr)) ok = true;
        else { ok = copy_flushed(w, d, !copied); if (ok && copied) copied->push_back(d); }
    } while (ok && FindNextFileW(f, &e));
    FindClose(f);
    return ok;
}
static bool copy_tree(const std::wstring& src, const std::wstring& dst) {   // (the working copy at the first mount)
    if (!CreateDirectoryW(dst.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    WIN32_FIND_DATAW e; HANDLE f = FindFirstFileW((src + L"\\*").c_str(), &e);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    bool ok = true;
    do {
        if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..") || (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring s = src + L"\\" + e.cFileName, d = dst + L"\\" + e.cFileName;
        ok = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? copy_tree(s, d) : copy_flushed(s, d);
    } while (ok && FindNextFileW(f, &e));
    FindClose(f);
    return ok;
}
static std::atomic<int> g_backups_set{-1};   // game.save_backups, live (hle_fs_set_save_backups); -1: the env
static int backups_kept() {
    if (int n = g_backups_set.load(std::memory_order_relaxed); n >= 0) return n;
    const char* e = getenv("TOTK_SAVE_BACKUPS");
    long n = e ? strtol(e, nullptr, 10) : 10;
    return n < 0 ? 0 : n > 1000 ? 1000 : (int)n;
}
// Renames fail while another process has a file inside open (antivirus, the search indexer, a sync client, an Explorer
// window), typically for a moment right after the files were written: retried 10 times 50 ms apart.
static std::atomic<u32> g_move_retries, g_move_max_ms;   // (the publish line: retried renames, the longest MoveFileExW)
static bool move_retry(const std::wstring& from, const std::wstring& to) {
    for (int i = 0;; i++) {
        u64 t = GetTickCount64();
        BOOL ok = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH);
        DWORD e = GetLastError();
        u32 ms = (u32)(GetTickCount64() - t);
        if (ms > g_move_max_ms) g_move_max_ms = ms;
        if (ok) return true;
        if (i == 9 || (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION)) { SetLastError(e); return false; }
        g_move_retries++;
        Sleep(50);
    }
}
static std::wstring local_time_name(const wchar_t* suffix) {
    SYSTEMTIME t; GetLocalTime(&t); wchar_t n[96];
    swprintf(n, 96, L"%04u%02u%02u-%02u%02u%02u-%03u%ls", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, suffix);
    return n;
}
// The committed copy moves into the backups (or is deleted when none are kept); the oldest backups beyond the limit go.
static bool retire_committed() {
    int keep = backups_kept();
    if (!keep) return remove_tree(g_committed);
    std::error_code ec; fsys::create_directories(g_backups, ec);
    std::wstring dst = g_backups + L"\\" + local_time_name(L"");
    for (int i = 1; wexists(dst); i++) dst = g_backups + L"\\" + local_time_name((L"-" + std::to_wstring(i)).c_str());
    if (!move_retry(g_committed, dst)) return false;
    std::vector<std::wstring> names;
    for (auto& d : fsys::directory_iterator(g_backups, ec))
        if (d.is_directory(ec) && d.path().filename().wstring().find(L"unpublished") == std::wstring::npos) names.push_back(d.path().wstring());
    std::sort(names.begin(), names.end());   // names sort by time
    for (size_t i = 0; i + keep < names.size(); i++)
        if (!remove_tree(names[i])) printf("[fs] save backup %ls could not be removed\n", names[i].c_str());
    return true;
}
static std::wstring nw_dir() { return g_committed + L".new"; }
static std::wstring rd_dir() { return g_committed + L".ready"; }
static std::wstring complete_marker() { return g_committed + L".new.complete"; }
static void mark(const std::wstring& m, bool on) {
    if (!on) { DeleteFileW(m.c_str()); return; }
    HANDLE h = CreateFileW(m.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
}
// step 3, and the recovery of an interrupted one
static bool swap_ready() {
    if (wexists(g_committed) && !retire_committed()) return false;
    return move_retry(rd_dir(), g_committed);
}
static void commits_lock() {
    DWORD r = WaitForSingleObject(g_save_mutex, INFINITE);   // abandoned: the on-disk state is recoverable (.new / .ready)
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) printf("[fs] save commit mutex: wait failed (%lu)\n", GetLastError());
}
static void commits_unlock() { ReleaseMutex(g_save_mutex); }
// Step 1 (mutex held): the snapshot of `work` in save.new. With `copied` the copies are not flushed (the async publish
// flushes them in step 2); files equal to the committed ones are hard links either way.
static bool snapshot_locked(const std::wstring& work, std::vector<std::wstring>* copied) {
    if (wexists(rd_dir()) && !swap_ready()) { printf("[fs] save commit: an earlier swap still fails (error %lu)\n", GetLastError()); return false; }
    mark(complete_marker(), false);
    if (wexists(nw_dir()) && !remove_tree(nw_dir())) return false;
    if (!snap_tree(work, g_committed, nw_dir(), copied)) {
        printf("[fs] save commit: building %ls failed (error %lu)\n", nw_dir().c_str(), GetLastError());
        remove_tree(nw_dir()); return false;
    }
    return true;
}
// Steps 1b-3 (mutex held): flush what step 1 copied unflushed, mark complete, .new -> .ready, swap + backup.
// `ms` (optional): the steps' times, flush / .new -> .ready / swap + backup (a publish's "[fs] save commit published").
static bool finish_locked(const std::vector<std::wstring>& copied, u64* ms = nullptr) {
    u64 t0 = GetTickCount64();
    for (const auto& f : copied) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) { printf("[fs] save commit: flushing %ls failed (error %lu)\n", f.c_str(), GetLastError()); return false; }
    }
    u64 t1 = GetTickCount64();
    mark(complete_marker(), true);
    if (!move_retry(nw_dir(), rd_dir())) { printf("[fs] save commit: .new -> .ready failed (error %lu); kept for the retry\n", GetLastError()); return false; }
    mark(complete_marker(), false);
    u64 t2 = GetTickCount64();
    if (!swap_ready()) { printf("[fs] save commit: swap failed (error %lu); finished at the next commit or start\n", GetLastError()); return false; }
    if (ms) { ms[0] = t1 - t0; ms[1] = t2 - t1; ms[2] = GetTickCount64() - t2; }
    return true;
}
// publish `work` as the committed save in one go (mutex held). On failure the committed save is unchanged.
static bool publish_locked(const std::wstring& work) {
    u64 t0 = GetTickCount64();
    if (!snapshot_locked(work, nullptr)) return false;
    u64 t1 = GetTickCount64();
    if (!finish_locked({})) return false;
    u64 t2 = GetTickCount64();
    printf("[fs] save commit published in %llu ms (snapshot %llu, rename + swap + backup %llu)\n", (unsigned long long)(t2 - t0),
           (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));
    return true;
}
static bool newer_than(const std::wstring& dir, const FILETIME& t) {   // a file under dir written after t
    WIN32_FIND_DATAW e; HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &e);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool newer = false;
    do {
        if (!wcscmp(e.cFileName, L".") || !wcscmp(e.cFileName, L"..")) continue;
        newer = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? newer_than(dir + L"\\" + e.cFileName, t) : CompareFileTime(&e.ftLastWriteTime, &t) > 0;
    } while (!newer && FindNextFileW(f, &e));
    FindClose(f);
    return newer;
}
// Working copies of processes that have exited (their .lock is no longer held): one holding a commit that never reached
// disk (.pending) is published if nothing in it is newer than the marker, else set aside in the backups; the rest go.
static void drop_stale_work(const std::wstring& userdir) {
    WIN32_FIND_DATAW e; HANDLE f = FindFirstFileW((userdir + L"\\save.work.*").c_str(), &e);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring w = userdir + L"\\" + e.cFileName, lk = w + L".lock", pend = w + L".pending";
        if (w == g_work) continue;
        if (!DeleteFileW(lk.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) continue;   // held: that process is running
        if (wexists(pend)) {
            WIN32_FILE_ATTRIBUTE_DATA md;
            bool clean = GetFileAttributesExW(pend.c_str(), GetFileExInfoStandard, &md) && !newer_than(w, md.ftLastWriteTime);
            if (clean && publish_locked(w)) { printf("[fs] save data: published the commit an earlier run could not finish (%ls)\n", w.c_str()); DeleteFileW(pend.c_str()); }
            else if (clean) { printf("[fs] save data: %ls still holds an unpublished commit; kept for the next start\n", w.c_str()); continue; }
            else {
                std::error_code ec; fsys::create_directories(g_backups, ec);
                std::wstring dst = g_backups + L"\\" + local_time_name(L"-unpublished");
                if (move_retry(w, dst)) {
                    printf("[fs] save data: an unpublished commit was followed by further writes; not published (it may be torn), kept as %ls\n", dst.c_str());
                    DeleteFileW(pend.c_str());
                } else printf("[fs] save data: %ls holds an unpublished commit followed by further writes; left in place\n", w.c_str());
                continue;
            }
        }
        if (!remove_tree(w)) printf("[fs] save data: stale working copy %ls could not be removed\n", w.c_str());
    } while (FindNextFileW(f, &e));
    FindClose(f);
}
// First MountSaveData: finish or drop an interrupted commit, then make this process's working copy.
static void save_init() {
    std::wstring ud = widen(kUserDir);
    for (auto& ch : ud) if (ch == L'/') ch = L'\\';
    g_committed = ud + L"\\save";
    g_work = ud + L"\\save.work." + std::to_wstring(GetCurrentProcessId());
    g_backups = ud + L"\\save.backups";
    g_save_work = std::string(kUserDir) + "/save.work." + std::to_string(GetCurrentProcessId());
    std::error_code ec; fsys::create_directories(ud, ec);
    g_save_mutex = CreateMutexW(nullptr, FALSE, L"Local\\totk-recomp-save-commit");
    commits_lock();
    std::wstring source = g_committed;   // the newest complete save: the working copy starts from it
    if (wexists(rd_dir())) {
        printf("[fs] save data: finishing a commit interrupted after it was complete (save.ready)\n");
        if (!swap_ready()) { printf("[fs] save data: the interrupted commit cannot be put in place now (error %lu): playing from save.ready\n", GetLastError()); source = rd_dir(); }
    }
    if (wexists(nw_dir()) && wexists(complete_marker()) && !wexists(rd_dir())) {   // a finished snapshot whose rename failed
        printf("[fs] save data: finishing a commit whose snapshot was complete (save.new)\n");
        if (move_retry(nw_dir(), rd_dir())) { mark(complete_marker(), false); if (swap_ready()) source = g_committed; else source = rd_dir(); }
    }
    if (wexists(nw_dir()) && !wexists(complete_marker()) && !remove_tree(nw_dir()))
        printf("[fs] save data: an incomplete commit (save.new) could not be removed; the previous commit stays\n");
    fsys::create_directories(g_committed, ec);
    drop_stale_work(ud);
    g_work_lock = CreateFileW((g_work + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    remove_tree(g_work);
    if (!copy_tree(source, g_work) && !(source != g_committed && remove_tree(g_work) && copy_tree(g_committed, g_work)))
        printf("[fs] save data: making the working copy %ls failed (error %lu)\n", g_work.c_str(), GetLastError());
    commits_unlock();
    if (g_save_async) std::atexit([] { wait_publisher(5000); });
    printf("[fs] save data: committed %ls, working copy %ls, %d backups kept in %ls\n", g_committed.c_str(), g_work.c_str(), backups_kept(), g_backups.c_str());
}
static const std::string& save_mount_dir() {
    static std::once_flag once;
    std::call_once(once, [] { if (g_journal) save_init(); else g_save_work = std::string(kUserDir) + "/save"; });
    return g_save_work;
}
// Async publish, on by default since async90 (2026-10-23: 8 commits, snapshot 0-15 ms on the game thread, the rest
// 0-16 ms in the background; TOTK_SAVE_ASYNC=0 = all on the game thread): Commit takes only the snapshot (hard links and
// unflushed copies) and a background thread flushes and publishes it; the next Commit waits for that publish first, and
// an exit waits up to 5 s for it. A crash before it finishes loses that commit only (the previous one stays committed),
// as a power cut before the console's commit would.
static void pending(bool on) {   // (g_commit_mtx held, or the publisher thread while g_pub_busy keeps Commit waiting)
    std::wstring pend = g_work + L".pending";
    if (on && !g_commit_pending) { mark(pend, true); g_commit_pending = true; }   // retried at the next Commit / start
    if (!on && g_commit_pending) { DeleteFileW(pend.c_str()); g_commit_pending = false; }
}
static void commit_save() {
    std::lock_guard<std::mutex> l(g_commit_mtx);
    if (!g_save_async) {
        commits_lock();
        bool ok = publish_locked(g_work);
        commits_unlock();
        pending(!ok);
        return;
    }
    u64 tw = GetTickCount64();
    wait_publisher();
    u64 t0 = GetTickCount64();
    if (t0 - tw >= 50) printf("[fs] save commit: waited %llu ms for the previous commit's background publish\n", (unsigned long long)(t0 - tw));
    auto copied = std::make_shared<std::vector<std::wstring>>();
    commits_lock();
    bool ok = snapshot_locked(g_work, copied.get());
    commits_unlock();
    pending(true);   // until the background publish lands (a failed snapshot is retried at the next Commit / start)
    if (!ok) return;
    { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = true; }
    u64 snap = GetTickCount64() - t0;
    std::thread([copied, snap] {
        u64 t1 = GetTickCount64();
        commits_lock();
        u64 tl = GetTickCount64(), ms[3] = {};
        g_move_retries = 0; g_move_max_ms = 0;
        bool ok = finish_locked(*copied, ms);
        commits_unlock();
        if (ok) {
            pending(false);
            printf("[fs] save commit published: snapshot %llu ms on the game thread, flush + rename + swap %llu ms in the background"
                   " (mutex %llu, flush %llu ms for %zu files, .ready %llu, swap + backup %llu; renames retried %u, longest %u ms)\n",
                   (unsigned long long)snap, (unsigned long long)(GetTickCount64() - t1), (unsigned long long)(tl - t1),
                   (unsigned long long)ms[0], copied->size(), (unsigned long long)ms[1], (unsigned long long)ms[2],
                   g_move_retries.load(), g_move_max_ms.load());
        }
        { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = false; }
        g_pub_cv.notify_all();
    }).detach();
}
#endif
// Handle tables: a fixed array of slots, reused through a free list. The guest's handle value is (generation << 32) |
// (slot + 1), so a handle used after its Close does not reach the slot's next file. Lookups take no lock (the streaming
// threads read concurrently); slot objects are recycled, never deleted, so a lookup racing a Close (a guest bug) touches
// valid memory. Open / Close take g_h_mtx.
static std::mutex g_h_mtx;
template <class T> struct Slots {
    static const u32 kN = 1u << 16;
    std::atomic<T*> slot[kN] {};
    std::vector<u32> free_;
    u32 next = 0;
    u64 put(const std::function<void(T*)>& init) {   // -> handle value, 0 when every slot is in use
        std::lock_guard<std::mutex> l(g_h_mtx);
        u32 i;
        if (!free_.empty()) { i = free_.back(); free_.pop_back(); }
        else if (next < kN) i = next++;
        else return 0;
        T* o = slot[i].load(std::memory_order_relaxed);
        if (!o) o = new T();
        init(o);
        u32 g = o->gen.load(std::memory_order_relaxed) + 1;
        o->live = true;
        o->gen.store(g, std::memory_order_release);
        slot[i].store(o, std::memory_order_release);
        return ((u64)g << 32) | (i + 1);
    }
    T* get(u64 v) {
        u32 i = (u32)v - 1;
        if (!(u32)v || i >= kN) return nullptr;
        T* o = slot[i].load(std::memory_order_acquire);
        return o && o->live && o->gen.load(std::memory_order_acquire) == (u32)(v >> 32) ? o : nullptr;
    }
    template <class F> void close(u64 v, F fin) {   // fin(o) releases o's resources, under the lock
        std::lock_guard<std::mutex> l(g_h_mtx);
        T* o = get(v);
        if (!o) return;
        fin(o);
        o->live = false;
        free_.push_back((u32)v - 1);
    }
};
// TOTK_ROMFS_PACK=<file> (off without it Windows): the RomFS served from one file. A loose file's first open costs 4-14 ms
// of host-side scanning (Defender) on a busy PC, about 6.6k of them per Continue -> world; the pack is opened once. Per
// path: the skip-opening / mod / overlay folders first (host_path, unchanged), then the pack, then the folder (a path the
// pack lacks). Reads are overlapped ReadFile at data_off + offset on the one handle (a synchronous handle serializes I/O
// per file object: the loader threads would queue) GetFileSize, GetEntryType and directory listings come from the index. A
// pack that fails its checks (magic, version, size, bounds) is logged and ignored. With a pack the open-ahead is off.
// TOTK_ROMFS_PACK may also name the extraction's RomFS image (romfs.bin: romfs_image.h), indexed at start in ~0.1 s; the
// default since 2026-10-27, no NXRP copy needed.
#ifdef _WIN32
namespace {
struct PackEntry { u64 hash; u32 path_off, path_len; u64 data_off, size; };
static_assert(sizeof(PackEntry) == 32, "NXRP v1 entry");
struct RomPack { HANDLE h = INVALID_HANDLE_VALUE; std::vector<PackEntry> ents; std::string paths; std::vector<u32> order; };
u64 fnv1a64(const char* s, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 0x100000001b3ull; }
    return h;
}
std::string_view pack_path(const RomPack* p, u32 i) { const PackEntry& x = p->ents[i]; return {p->paths.data() + x.path_off, x.path_len}; }
// bytes read at `off` (stops at a short read or an error)
u64 pack_read(HANDLE h, u64 off, void* buf, u64 size) {
    // (closed when the thread exits: a plain thread_local HANDLE leaked one event per reading thread that ended)
    static thread_local struct ReadEvent {
        HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~ReadEvent() { if (h) CloseHandle(h); }
    } re;
    HANDLE ev = re.h;
    u64 done = 0;
    while (done < size) {
        OVERLAPPED ov{}; u64 o = off + done; ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32); ov.hEvent = ev;
        DWORD want = (DWORD)std::min<u64>(size - done, 1u << 30), n = 0;
        if (!ReadFile(h, (u8*)buf + done, want, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (!GetOverlappedResult(h, &ov, &n, TRUE) || n == 0) break;
        done += n;
    }
    return done;
}
RomPack* rompack() {
    static RomPack* pk = []() -> RomPack* {
        const char* e = getenv("TOTK_ROMFS_PACK");
        if (!e || !*e) return nullptr;
        HANDLE h = CreateFileW(widen(e).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
        auto fail = [&](const char* why) -> RomPack* {
            printf("[fs] romfs pack %s: %s; serving the folder\n", e, why);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return nullptr;
        };
        if (h == INVALID_HANDLE_VALUE) return fail("cannot open");
        u8 hd[128];
        if (pack_read(h, 0, hd, sizeof hd) != sizeof hd) return fail("short header");
        u32 ver, n, pb; u64 io, po, oo, total;
        memcpy(&ver, hd + 8, 4); memcpy(&n, hd + 16, 4); memcpy(&pb, hd + 20, 4);
        memcpy(&io, hd + 24, 8); memcpy(&po, hd + 32, 8); memcpy(&oo, hd + 40, 8); memcpy(&total, hd + 56, 8);
        LARGE_INTEGER sz{};
        if (memcmp(hd, "NXRPACK1", 8) != 0) {
            // The extraction's own RomFS image (romfs.bin, romfs_image.h): the same index built from its tables, so the
            // pack needs no second 17 GB copy. The bytes are the ones the folder was unpacked from (every IVFC level
            // checked at extraction; tools/engine/romfs_image_verify.py compares all of them with the folder).
            u64 h0;
            memcpy(&h0, hd, 8);
            if (h0 != 0x50) return fail("neither an NXRP v1 pack nor a RomFS image");
            if (!GetFileSizeEx(h, &sz)) return fail("no file size");
            const u64 t0 = GetTickCount64();
            std::vector<RomImageFile> files;
            std::string paths;
            const char* why = "";
            if (!romfs_image_index([h](u64 off, void* buf, u64 n) { return pack_read(h, off, buf, n); }, (u64)sz.QuadPart,
                                   &files, &paths, &why))
                return fail(why);
            RomPack* r = new RomPack;
            r->paths = std::move(paths);
            // The entries by hash; each keeps its table index through the sort, so the order by path follows
            // from the table's: the images the SDK and hactool write list the files sorted by path already (v100: 304,207
            // in order). Checked in one pass; a table out of order is sorted as before (~0.2 s of string compares, which
            // was 3/4 of this index's time).
            struct Key { u64 hash; u32 tix; };
            std::vector<Key> keys(files.size());
            u64 bytes = 0;
            bool table_sorted = true;
            for (size_t i = 0; i < files.size(); i++) {
                const RomImageFile& f = files[i];
                keys[i] = {fnv1a64(r->paths.data() + f.path_off, f.path_len), (u32)i};
                bytes += f.size;
                if (i && table_sorted)
                    table_sorted = std::string_view(r->paths.data() + files[i - 1].path_off, files[i - 1].path_len) <
                                   std::string_view(r->paths.data() + f.path_off, f.path_len);
            }
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.hash < b.hash; });
            r->ents.resize(files.size());
            r->order.resize(files.size());
            for (u32 k = 0; k < (u32)keys.size(); k++) {
                const RomImageFile& f = files[keys[k].tix];
                r->ents[k] = {keys[k].hash, f.path_off, f.path_len, f.data_off, f.size};
                r->order[keys[k].tix] = k;   // (table rank -> entry)
            }
            if (!table_sorted)
                std::sort(r->order.begin(), r->order.end(), [r](u32 a, u32 b) { return pack_path(r, a) < pack_path(r, b); });
            r->h = h;
            printf("[fs] romfs image %s: %zu files, %.2f GiB, indexed in %llu ms; the folder is the fallback\n", e,
                   r->ents.size(), bytes / 1073741824.0, (unsigned long long)(GetTickCount64() - t0));
            return r;
        }
        if (ver != 1) return fail("not an NXRP v1 pack");
        if (!GetFileSizeEx(h, &sz) || (u64)sz.QuadPart != total) return fail("file size differs from its header");
        if (io + (u64)n * 32 > total || po + pb > total || oo + (u64)n * 4 > total) return fail("tables out of bounds");
        RomPack* r = new RomPack;
        r->ents.resize(n); r->paths.resize(pb); r->order.resize(n);
        if (pack_read(h, io, r->ents.data(), (u64)n * 32) != (u64)n * 32 || pack_read(h, po, r->paths.data(), pb) != pb ||
            pack_read(h, oo, r->order.data(), (u64)n * 4) != (u64)n * 4) { delete r; return fail("short tables"); }
        u64 bytes = 0;
        for (u32 i = 0; i < n; i++) {
            const PackEntry& x = r->ents[i];
            if ((u64)x.path_off + x.path_len > pb || x.data_off + x.size > total || r->order[i] >= n) { delete r; return fail("an entry out of bounds"); }
            bytes += x.size;
        }
        // pack_find and pack_range binary-search: an index not sorted by hash, or an order not sorted by path, would make
        // lookups miss without a word (with the folder deleted: files not found). Checked once, ~0.3 M compares for a full pack.
        for (u32 i = 1; i < n; i++) {
            if (r->ents[i - 1].hash > r->ents[i].hash) { delete r; return fail("index not sorted by path hash"); }
            if (pack_path(r, r->order[i - 1]) > pack_path(r, r->order[i])) { delete r; return fail("order not sorted by path"); }
        }
        {   // order must name every entry once (romfs_pack.py's reader checks the same): a duplicate sorts in place
            std::vector<bool> seen(n);
            for (u32 i = 0; i < n; i++) {
                if (seen[r->order[i]]) { delete r; return fail("order is not a permutation of the index"); }
                seen[r->order[i]] = true;
            }
        }
        r->h = h;
        printf("[fs] romfs pack %s: %u files, %.2f GiB; the folder is the fallback\n", e, n, bytes / 1073741824.0);
        return r;
    }();
    return pk;
}
const PackEntry* pack_find(const RomPack* p, const std::string& rel) {
    const u64 hv = fnv1a64(rel.data(), rel.size());
    auto it = std::lower_bound(p->ents.begin(), p->ents.end(), hv, [](const PackEntry& a, u64 v) { return a.hash < v; });
    for (; it != p->ents.end() && it->hash == hv; ++it)
        if (it->path_len == rel.size() && memcmp(p->paths.data() + it->path_off, rel.data(), rel.size()) == 0) return &*it;
    return nullptr;
}
// [a, b) of order[] whose paths start with `pre` (order is sorted by path, byte order)
std::pair<size_t, size_t> pack_range(const RomPack* p, const std::string& pre) {
    auto lo = std::lower_bound(p->order.begin(), p->order.end(), pre,
                               [p](u32 i, const std::string& v) { return pack_path(p, i) < std::string_view(v); });
    // the paths with that prefix are contiguous in path order: the end by binary search too (a walk over the range made
    // GetEntryType of a big directory, Model/ in a full pack, cost one compare per file under it)
    auto hi = std::partition_point(lo, p->order.end(),
                                   [p, &pre](u32 i) { return pack_path(p, i).substr(0, pre.size()) == pre; });
    return {(size_t)(lo - p->order.begin()), (size_t)(hi - p->order.begin())};
}
// the RomFS-relative path of host path `hp` ('/' separators, no leading or trailing '/'), or false when `hp` is not
// under the RomFS folder (an overlay, mod or skip-opening file, or another mount)
bool pack_rel(const std::string& hp, std::string* rel) {
    const size_t n = strlen(kRomDir);
    if (hp.compare(0, n, kRomDir) != 0 || (hp.size() > n && hp[n] != '/' && hp[n] != '\\')) return false;
    rel->assign(hp, n, std::string::npos);
    for (char& ch : *rel) if (ch == '\\') ch = '/';
    size_t a = rel->find_first_not_of('/');
    *rel = a == std::string::npos ? std::string() : rel->substr(a);
    while (!rel->empty() && rel->back() == '/') rel->pop_back();
    return true;
}
// 1 file, 0 directory, -1 not in the pack
int pack_type(const RomPack* p, const std::string& rel) {
    if (rel.empty()) return 0;
    if (pack_find(p, rel)) return 1;
    auto [a, b] = pack_range(p, rel + "/");
    return a < b ? 0 : -1;
}
}   // namespace
// rt_main, before the guest starts: the pack's index (an image's is built: ~0.1 s for v100's 304k files) on its own
// thread, so the game's first open finds it ready instead of building it (rompack's static waits for it if not yet)
static std::atomic<u64> g_index_ready_ns{0};   // (steady clock) when the early thread had the index
static u64 steady_ns() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void hle_fs_index_romfs_early(void) {
    if (const char* e = getenv("TOTK_ROMFS_PACK"); e && *e)
        std::thread([] { rompack(); g_index_ready_ns = steady_ns(); }).detach();
}
// The game's first RomFS open: how long it waited for the early thread's index (0 when it was ready), logged once, so
// a start's serial part is measured, not assumed
static RomPack* rompack_first_open() {
    static std::atomic<bool> first{true};
    if (!first.exchange(false)) return rompack();
    const u64 t0 = steady_ns();
    RomPack* pk = rompack();
    u64 ready = g_index_ready_ns.load();
    if (!ready) ready = steady_ns();   // (built here, or the early thread had not stored its time yet)
    if (pk) printf("[fs] romfs index: the first open waited %.1f ms for it\n", ready > t0 ? (ready - t0) / 1e6 : 0.0);
    return pk;
}
#else
extern "C" void hle_fs_index_romfs_early(void) {}
#endif
struct HFile { FH h = kClosed; int mode = 0; std::string path; std::atomic<u32> gen{0}; volatile bool live = false;
               std::shared_ptr<const FilePatch> patch;    // (patch: the glyph bank's bytes over the file's)
               bool packed = false; u64 pk_off = 0, pk_size = 0; };   // (packed: served from the RomFS pack, h unused)
static Slots<HFile> g_files;
static HFile* file(u64 v) { return g_files.get(v); }

HLE_AS(fs_QueryMountRomCacheSize, "_ZN2nn2fs22QueryMountRomCacheSizeEPm") { *(u64*)c->x[0] = 0x1000; c->x[0] = 0; }
HLE_AS(fs_MountRom, "_ZN2nn2fs8MountRomEPKcPvm") { mount((const char*)c->x[0], kRomDir); c->x[0] = 0; }
#ifdef _WIN32
// the save mount reads and writes this process's working copy; Commit publishes it (atomic save commits above)
HLE_AS(fs_MountSaveData, "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE") { mount((const char*)c->x[0], save_mount_dir()); c->x[0] = 0; }
#else
HLE_AS(fs_MountSaveData, "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE") { mount((const char*)c->x[0], std::string(kUserDir) + "/save"); c->x[0] = 0; }
#endif
HLE_AS(fs_MountSaveDataReadOnly, "_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE") { mount((const char*)c->x[0], std::string(kUserDir) + "/save"); c->x[0] = 0; }
HLE_AS(fs_IsSaveDataExisting, "_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE") { c->x[0] = 1; }
HLE_AS(fs_MountTemporaryStorage, "_ZN2nn2fs21MountTemporaryStorageEPKc") { mount((const char*)c->x[0], std::string(kUserDir) + "/temp"); c->x[0] = 0; }
HLE_AS(fs_MountCacheStorage, "_ZN2nn2fs17MountCacheStorageEPKci") { mount((const char*)c->x[0], std::string(kUserDir) + "/cache" + std::to_string((int)c->x[1])); c->x[0] = 0; }
HLE_AS(fs_CreateCacheStorage, "_ZN2nn2fs18CreateCacheStorageEill") { c->x[0] = 0; }
HLE_AS(fs_DeleteCacheStorage, "_ZN2nn2fs18DeleteCacheStorageEi") { c->x[0] = 0; }
HLE_AS(fs_OpenCacheStorageList, "_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE") { *(u64*)c->x[0] = 1; c->x[0] = 0; }
HLE_AS(fs_ReadCacheStorageList, "_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi") { *(s32*)c->x[0] = 0; c->x[0] = 0; }
HLE_AS(fs_CloseCacheStorageList, "_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE") { }
HLE_AS(fs_Unmount, "_ZN2nn2fs7UnmountEPKc") { std::lock_guard<std::mutex> l(g_fs_mtx); g_mounts.erase((const char*)c->x[0]); }
#ifdef _WIN32
// Commit of the save mount publishes the working copy. The game always gets success: its data is in the working copy,
// and a publish that fails is retried at the next Commit and at the next start (.pending).
HLE_AS(fs_Commit, "_ZN2nn2fs6CommitEPKc") {
    bool save = false;
    {
        std::lock_guard<std::mutex> l(g_fs_mtx);
        auto it = g_mounts.find((const char*)c->x[0]);
        save = g_journal && it != g_mounts.end() && !g_save_work.empty() && it->second == g_save_work;
    }
    if (save) commit_save();
    c->x[0] = 0;
}
#else
HLE_AS(fs_Commit, "_ZN2nn2fs6CommitEPKc") { c->x[0] = 0; }
#endif
HLE_AS(fs_SetPriorityOnCurrentThread, "_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE") { }

// ---- open-ahead Windows scans a file the first time it is opened (Defender real-time protection: 4-14 ms a file on
// the dev PC, cached for about a minute), and the game's two loader threads open ~6.6 k files one after the other from
// Continue to the world: 16-18 s on a busy host, 3 s on a quiet one. Each session records the order in which the RomFS
// files were first opened (cache/fs_openorder.txt: host paths, this session's order first, then the older entries it
// did not reach); the next session follows it. When the game opens a recorded file, TOTK_FS_PREOPEN_THREADS host
// threads (default 8) open and close the next TOTK_FS_PREOPEN_AHEAD recorded files (default 512), so their scan is paid
// in parallel, ahead of the loaders. The boot, Continue and any fast travel or shrine of an earlier session are
// covered; a load never recorded is not.
#ifdef _WIN32
namespace {
struct Preopen {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::string> rec;                // the record being followed
    std::unordered_map<std::string, u32> at;     // host path -> index in rec
    u32 next = 0, target = 0;                    // the next index to open; open while next < target
    u32 ahead = 512;
    std::vector<std::string> now;                // this session's first-open order
    std::unordered_set<std::string> now_seen;
    size_t saved = 0;                            // now.size() at the last write
    u64 opened = 0, hits = 0, late = 0;          // files pre-opened; game opens of recorded files; of those, before the pre-open got there
};
const char* const kOpenOrder = "cache/fs_openorder.txt";
void preopen_write(Preopen* p) {
    std::vector<std::string> out;
    u64 opened, hits, late;
    {
        std::lock_guard<std::mutex> l(p->m);
        if (p->now.size() == p->saved) return;
        p->saved = p->now.size();
        out = p->now;
        for (auto& r : p->rec) if (!p->now_seen.count(r)) out.push_back(r);
        opened = p->opened; hits = p->hits; late = p->late;
    }
    if (out.size() > 200000) out.resize(200000);
    std::error_code ec;
    fsys::create_directories("cache", ec);
    std::string tmp = std::string(kOpenOrder) + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        for (auto& s : out) { fwrite(s.data(), 1, s.size(), f); fputc('\n', f); }
        bool ok = fclose(f) == 0;
        if (ok) MoveFileExA(tmp.c_str(), kOpenOrder, MOVEFILE_REPLACE_EXISTING);
    }
    printf("[fs] preopen: %zu files recorded this session (%zu in %s); %llu pre-opened, %llu game opens followed the "
           "record, %llu of them before the pre-open got there\n", p->saved, out.size(), kOpenOrder,
           (unsigned long long)opened, (unsigned long long)hits, (unsigned long long)late);
}
Preopen* preopen() {
    static Preopen* p = [] () -> Preopen* {
        if (!fs_flag("TOTK_FS_PREOPEN")) return nullptr;
        if (rompack()) { printf("[fs] preopen: off (the RomFS comes from a pack)\n"); return nullptr; }
        Preopen* p = new Preopen;
        if (const char* a = getenv("TOTK_FS_PREOPEN_AHEAD"); a && atoi(a) > 0) p->ahead = (u32)atoi(a);
        int nt = 8;
        if (const char* t = getenv("TOTK_FS_PREOPEN_THREADS"); t && atoi(t) > 0) nt = std::min(atoi(t), 32);
        if (FILE* f = fopen(kOpenOrder, "rb")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                size_t n = strcspn(line, "\r\n"); line[n] = 0;
                if (n && p->at.emplace(line, (u32)p->rec.size()).second) p->rec.emplace_back(line);
            }
            fclose(f);
        }
        p->target = std::min<u32>(p->ahead, (u32)p->rec.size());   // the boot's first files right away
        printf("[fs] preopen: following %zu recorded files (%s) on %d threads, %u ahead\n", p->rec.size(), kOpenOrder,
               nt, p->ahead);
        for (int i = 0; i < nt; i++)
            std::thread([p] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                cpu_place_thread(CPU_HELPER);   // (opens ahead of the game: the E-cores / the other CCD, cpu_place.h)
                for (;;) {
                    std::string path;
                    {
                        std::unique_lock<std::mutex> l(p->m);
                        p->cv.wait(l, [p] { return p->next < p->target; });
                        path = p->rec[p->next++];
                    }
                    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        CloseHandle(h);
                        std::lock_guard<std::mutex> l(p->m);
                        p->opened++;
                    }
                }
            }).detach();
        // the record is written every 20 s while it grows (a run ends by being killed: no exit hook to rely on)
        std::thread([p] {
            cpu_place_thread(CPU_HELPER);
            for (;;) { std::this_thread::sleep_for(std::chrono::seconds(20)); preopen_write(p); }
        }).detach();
        return p;
    }();
    return p;
}
// the game opened host file `hp` for reading: record it, and move the pre-open window past it if it is in the record
void preopen_note(const std::string& hp) {
    Preopen* p = preopen();
    if (!p) return;
    std::lock_guard<std::mutex> l(p->m);
    if (p->now_seen.insert(hp).second) p->now.push_back(hp);
    auto it = p->at.find(hp);
    if (it == p->at.end()) return;
    const u32 i = it->second;
    p->hits++;
    if (i >= p->next) { p->late++; p->next = i + 1; }   // behind the game (or a jump): what it has passed is no use
    const u32 t = std::min<u32>(i + 1 + p->ahead, (u32)p->rec.size());
    if (t > p->target) { p->target = t; p->cv.notify_all(); }
}
}   // namespace
#else
static void preopen_note(const std::string&) {}   // (the open scan is a Windows Defender cost)
#endif

HLE_AS(fs_OpenFile, "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci") {
    const char* p = (const char*)c->x[1]; int mode = (int)c->x[2]; std::string hp;
    if (!host_path(p, &hp)) { printf("[fs] OpenFile %s: unknown mount\n", p); c->x[0] = kPathNotFound; return; }
    u32 fail = kPathNotFound;
    bool pack_missed = false;   // (a RomFS path the pack lacks: counted once the folder really has it)
#ifdef _WIN32
    if (RomPack* pk = mode == 1 ? rompack_first_open() : nullptr) {   // the RomFS pack, after the overlay folders (host_path)
        std::string rel;
        const bool in_rom = pack_rel(hp, &rel);
        const PackEntry* pe = in_rom ? pack_find(pk, rel) : nullptr;
        if (pe) {
            if (g_trace_fs || g_open_logged < 40) { g_open_logged++; printf("[fs] OpenFile %s mode %d (pack)\n", p, mode); }
            g_opens_ok++;
            std::shared_ptr<const FilePatch> patch = glyph_patches(hp);
            u64 v = g_files.put([&](HFile* f) { f->h = kClosed; f->mode = mode; f->path = hp; f->patch = patch;
                                                f->packed = true; f->pk_off = pe->data_off; f->pk_size = pe->size; });
            if (!v) { printf("[fs] OpenFile %s: no free file handle\n", p); c->x[0] = kUnsupported; return; }
            *(u64*)c->x[0] = v; c->x[0] = 0;
            return;
        }
        pack_missed = in_rom;   // a RomFS path the pack lacks: the folder serves it (or it does not exist)
    }
    DWORD acc = GENERIC_READ | ((mode & 2) ? GENERIC_WRITE : 0);
    HANDLE h;
    for (int i = 0;; i++) {   // N11: a file held open by another process (sync client, antivirus) is locked, not missing
        h = CreateFileW(widen(hp).c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (!host_locked(e)) break;
        if (i == 9) { fail = kTargetLocked; printf("[fs] OpenFile %s: held by another process (error %lu) after 10 tries -> TargetLocked\n", p, e); break; }
        Sleep(50);
    }
#else
    FH h = fh_open(hp, mode);
#endif
    if (g_trace_fs || g_open_logged < 40 || h == kNoFile) {
        g_open_logged++; printf("[fs] OpenFile %s mode %d%s\n", p, mode, h == kNoFile ? (fail == kTargetLocked ? " -> LOCKED" : " -> NOT FOUND") : "");
    }
    if (h == kNoFile) { c->x[0] = fail; return; }
    if (pack_missed) {  // only a file the folder has:
        // the full image logged 50 of those, TexToGo/*_Fi.txtg, Event and WorldMgr probes)
        static std::atomic<u32> misses{0};
        u32 k = ++misses;
        if (k <= 50 || k % 500 == 0) printf("[fs] pack miss %u (from the folder): %s\n", k, p);
    }
    g_opens_ok++;
    if (mode == 1 && hp.compare(0, strlen(kUserDir), kUserDir) != 0) preopen_note(hp);   // (RomFS reads, not saves)
    std::shared_ptr<const FilePatch> patch = glyph_patches(hp);
    u64 v = g_files.put([&](HFile* f) { f->h = h; f->mode = mode; f->path = hp; f->patch = patch; f->packed = false; });
    if (!v) {   // 65536 files open at once: refuse rather than grow without bound
#ifdef _WIN32
        CloseHandle(h);
#else
        close(h);
#endif
        printf("[fs] OpenFile %s: no free file handle\n", p); c->x[0] = kUnsupported; return;
    }
    *(u64*)c->x[0] = v; c->x[0] = 0;
}
HLE_AS(fs_CloseFile, "_ZN2nn2fs9CloseFileENS0_10FileHandleE") {
    g_files.close(c->x[0], [](HFile* f) {
#ifdef _WIN32
        if (f->h) CloseHandle(f->h);
#else
        if (f->h != kClosed) close(f->h);
#endif
        f->h = kClosed; f->path.clear(); f->path.shrink_to_fit(); f->patch.reset(); f->packed = false;
    });
}
// load-progress counters: a line every 64 MiB read (is the loading screen streaming or stalled?)
static void count_read(u64 n, u64 ns) {
    static const u64 t0 = GetTickCount64();
    u64 before = g_rd_bytes.fetch_add(n); g_rd_calls++; g_rd_ns += ns;
    if ((before >> 26) != ((before + n) >> 26))
        printf("[fs] progress: %llu MiB in %llu reads, %llu opens, t=%.1f s\n", (unsigned long long)((before + n) >> 20),
               (unsigned long long)g_rd_calls.load(), (unsigned long long)g_opens_ok.load(), (GetTickCount64() - t0) / 1000.0);
}
extern "C" unsigned long long hle_fs_read_bytes(void) { return g_rd_bytes.load(std::memory_order_relaxed); }   // ([hitch] log)
// bytes, reads, opens and the wall time spent in reads, summed over threads (rt_main.cpp's TOTK_LOADTRACE timeline)
extern "C" void hle_fs_read_stats(unsigned long long out[4]) {
    out[0] = g_rd_bytes.load(std::memory_order_relaxed); out[1] = g_rd_calls.load(std::memory_order_relaxed);
    out[2] = g_opens_ok.load(std::memory_order_relaxed); out[3] = g_rd_ns.load(std::memory_order_relaxed);
}
// hle_os_mem.cpp (TOTK_ALIAS_SOFT_WW): ReadFile into a read-only alias-window page fails instead of faulting
extern "C" void alias_soft_io(const void* ptr, size_t len, int begin);
static u64 host_pread(HFile* f, s64 off, void* buf, u64 size) {
    const auto t_start = std::chrono::steady_clock::now();
    u64 done = 0;
#ifdef _WIN32
    alias_soft_io(buf, size, 1);
    if (f->packed) {   // the file's bytes in the RomFS pack, clipped to its size
        u64 n = off < 0 || (u64)off >= f->pk_size ? 0 : std::min<u64>(size, f->pk_size - (u64)off);
        done = n ? pack_read(rompack()->h, f->pk_off + (u64)off, buf, n) : 0;
        size = done;   // (no loose-file loop below)
    }
#endif
    while (done < size) {
#ifdef _WIN32
        OVERLAPPED ov{}; u64 o = (u64)off + done; ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD n = 0, want = (DWORD)std::min<u64>(size - done, 1u << 30);
        if (!ReadFile(f->h, (u8*)buf + done, want, &n, &ov) || n == 0) break;
        done += n;
#else
        size_t want = (size_t)std::min<u64>(size - done, 1u << 30);
        rt_ww_prepare_kernel_write((u8*)buf + done, want);   // a write-watched (read-only) page would give EFAULT, not a fault we handle
        ssize_t n = ::pread(f->h, (u8*)buf + done, want, (off_t)((u64)off + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (u64)n;
#endif
    }
#ifdef _WIN32
    alias_soft_io(buf, size, 0);
#endif
    if (f->patch && done) {   // the glyph bank's bytes in this range
        const FilePatch& p = *f->patch;
        for (size_t i = std::lower_bound(p.off.begin(), p.off.end(), (u64)off) - p.off.begin(); i < p.off.size() && p.off[i] < (u64)off + done; i++)
            ((u8*)buf)[p.off[i] - (u64)off] = p.val[i];
    }
    count_read(done, (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count());
    return done;
}
// ReadFile(size_t* out, FileHandle, s64 off, void* buf, size_t size[, ReadOption const&])
static void read_out(Ctx* c) {
    HFile* f = file(c->x[1]); if (!f) { c->x[0] = kUnsupported; return; }
    *(u64*)c->x[0] = host_pread(f, (s64)c->x[2], (void*)c->x[3], c->x[4]); c->x[0] = 0;
}
HLE_AS(fs_ReadFile6, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE") { read_out(c); }
HLE_AS(fs_ReadFile5, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm") { read_out(c); }
// ReadFile(FileHandle, s64 off, void* buf, size_t size): must read everything
HLE_AS(fs_ReadFile4, "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm") {
    HFile* f = file(c->x[0]); if (!f) { c->x[0] = kUnsupported; return; }
    u64 n = host_pread(f, (s64)c->x[1], (void*)c->x[2], c->x[3]);
    c->x[0] = n == c->x[3] ? kOk : kOutOfRange;
}
HLE_AS(fs_WriteFile, "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE") {
    HFile* f = file(c->x[0]); if (!f) { c->x[0] = kUnsupported; return; }
#ifdef _WIN32
    OVERLAPPED ov{}; u64 o = c->x[1]; ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
    DWORD n = 0; BOOL ok = WriteFile(f->h, (void*)c->x[2], (DWORD)c->x[3], &n, &ov);
    c->x[0] = ok && n == c->x[3] ? kOk : kUnsupported;
#else
    u64 done = 0, want = c->x[3];
    while (done < want) {
        ssize_t n = ::pwrite(f->h, (const u8*)c->x[2] + done, (size_t)(want - done), (off_t)(c->x[1] + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (u64)n;
    }
    c->x[0] = done == want ? kOk : kUnsupported;
#endif
}
#ifdef _WIN32
HLE_AS(fs_FlushFile, "_ZN2nn2fs9FlushFileENS0_10FileHandleE") { if (HFile* f = file(c->x[0])) FlushFileBuffers(f->h); c->x[0] = 0; }
#else
HLE_AS(fs_FlushFile, "_ZN2nn2fs9FlushFileENS0_10FileHandleE") { if (HFile* f = file(c->x[0])) fsync(f->h); c->x[0] = 0; }
#endif
HLE_AS(fs_GetFileSize, "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE") {
#ifdef _WIN32
    HFile* f = file(c->x[1]); LARGE_INTEGER s{};
    if (f && f->packed) { *(s64*)c->x[0] = (s64)f->pk_size; c->x[0] = 0; return; }
    if (!f || !GetFileSizeEx(f->h, &s)) { c->x[0] = kUnsupported; return; }
    *(s64*)c->x[0] = s.QuadPart; c->x[0] = 0;
#else
    HFile* f = file(c->x[1]); struct stat st;
    if (!f || fstat(f->h, &st) != 0) { c->x[0] = kUnsupported; return; }
    *(s64*)c->x[0] = (s64)st.st_size; c->x[0] = 0;
#endif
}
HLE_AS(fs_SetFileSize, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl") {
    HFile* f = file(c->x[0]); if (!f) { c->x[0] = kUnsupported; return; }
#ifdef _WIN32
    FILE_END_OF_FILE_INFO e; e.EndOfFile.QuadPart = (s64)c->x[1];
    c->x[0] = SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e) ? 0 : kUnsupported;
#else
    c->x[0] = ftruncate(f->h, (off_t)c->x[1]) == 0 ? 0 : kUnsupported;
#endif
}

// ---- paths
// N11: host errors -> nn::fs results. Sharing / lock / access errors (another process holds the file) are TargetLocked,
// never PathNotFound; every one is logged.
static u32 fs_err(const std::error_code& ec, const char* what = "") {
    if (!ec) return kOk;
    if (ec == std::errc::file_exists) return kPathExists;
#ifdef _WIN32
    if (ec.category() == std::system_category() && host_locked((DWORD)ec.value())) {
        printf("[fs] %s: held by another process (error %d) -> TargetLocked\n", what, ec.value()); return kTargetLocked;
    }
#endif
    if (ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy) {
        printf("[fs] %s: %s -> TargetLocked\n", what, ec.message().c_str()); return kTargetLocked;
    }
    return kPathNotFound;
}
// runs op (which sets ec) up to 10 times 50 ms apart while the host reports the target locked
template <class Op> static void retry_locked(std::error_code& ec, Op op) {
    for (int i = 0; i < 10; i++) {
        ec.clear(); op();
#ifdef _WIN32
        if (!(ec && ec.category() == std::system_category() && host_locked((DWORD)ec.value()))) return;
        Sleep(50);
#else
        return;
#endif
    }
}
HLE_AS(fs_GetEntryType, "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc") {
    std::string hp; std::error_code ec;
    if (!host_path((const char*)c->x[1], &hp)) { c->x[0] = kPathNotFound; return; }
#ifdef _WIN32
    if (RomPack* pk = rompack()) {   // a RomFS path the pack knows (else the folder, below)
        std::string rel; int t = pack_rel(hp, &rel) ? pack_type(pk, rel) : -1;
        if (t >= 0) { *(u32*)c->x[0] = (u32)t; c->x[0] = 0; return; }
    }
#endif
    ci_fix(hp);
    auto st = fsys::status(widen(hp), ec);
    if (ec || !fsys::exists(st)) { c->x[0] = kPathNotFound; return; }
    *(u32*)c->x[0] = fsys::is_directory(st) ? 0 : 1; c->x[0] = 0;
}
HLE_AS(fs_CreateFile, "_ZN2nn2fs10CreateFileEPKcl") {
    std::string hp; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(hp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { c->x[0] = GetLastError() == ERROR_FILE_EXISTS ? kPathExists : kPathNotFound; return; }
    FILE_END_OF_FILE_INFO e; e.EndOfFile.QuadPart = (s64)c->x[1]; SetFileInformationByHandle(h, FileEndOfFileInfo, &e, sizeof e);
    CloseHandle(h); c->x[0] = 0;
#else
    int h = open(hp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (h < 0) { c->x[0] = errno == EEXIST ? kPathExists : kPathNotFound; return; }
    if (ftruncate(h, (off_t)c->x[1]) != 0) { /* (size is best effort, like the Windows path) */ }
    close(h); c->x[0] = 0;
#endif
}
HLE_AS(fs_DeleteFile, "_ZN2nn2fs10DeleteFileEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    bool removed = false;
    retry_locked(ec, [&] { removed = fsys::remove(widen(hp), ec); });
    c->x[0] = removed ? 0 : ec ? fs_err(ec, "DeleteFile") : kPathNotFound;
}
HLE_AS(fs_RenameFile, "_ZN2nn2fs10RenameFileEPKcS2_") {
    std::string a, b; std::error_code ec;
    if (!host_path((const char*)c->x[0], &a) || !host_path((const char*)c->x[1], &b)) { c->x[0] = kPathNotFound; return; }
    retry_locked(ec, [&] { fsys::rename(widen(a), widen(b), ec); }); c->x[0] = fs_err(ec, "RenameFile");
}
HLE_AS(fs_CreateDirectory, "_ZN2nn2fs15CreateDirectoryEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    c->x[0] = fsys::create_directory(widen(hp), ec) ? 0 : (ec ? fs_err(ec, "CreateDirectory") : kPathExists);
}
HLE_AS(fs_DeleteDirectoryRecursively, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    if (hp.rfind(kRomDir, 0) == 0) { c->x[0] = kUnsupported; return; }      // never touch the extracted romfs
    retry_locked(ec, [&] { fsys::remove_all(widen(hp), ec); }); c->x[0] = fs_err(ec, "DeleteDirectoryRecursively");
}

// ---- directories. nn::fs::DirectoryEntry: name[0x301], pad[3], s8 type @0x304, pad[3], s64 size @0x308 (0x310)
typedef std::vector<std::pair<std::string, std::pair<int, s64>>> DirEnts;
struct HDir { DirEnts ents; size_t pos = 0; std::atomic<u32> gen{0}; volatile bool live = false; };
static Slots<HDir> g_dirs;
HLE_AS(fs_OpenDirectory, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci") {
    std::string hp; std::error_code ec; int mode = (int)c->x[2];
    if (!host_path((const char*)c->x[1], &hp)) { c->x[0] = kPathNotFound; return; }
    DirEnts ents;
#ifdef _WIN32
    std::string rel;
    if (RomPack* pk = rompack(); pk && pack_rel(hp, &rel) && pack_type(pk, rel) == 0) {   // listed from the pack's index
        const std::string pre = rel.empty() ? std::string() : rel + "/";
        auto [a, b] = pack_range(pk, pre);
        std::string last;
        for (size_t k = a; k < b; k++) {
            std::string_view s = pack_path(pk, pk->order[k]).substr(pre.size());
            size_t sl = s.find('/');
            if (sl == std::string_view::npos) {
                if (mode & 2) ents.push_back({ std::string(s), { 1, (s64)pk->ents[pk->order[k]].size } });
            } else if (std::string d(s.substr(0, sl)); d != last) {   // (a subfolder's entries are contiguous in path order)
                last = d;
                if (mode & 1) ents.push_back({ d, { 0, 0 } });
            }
        }
    } else
#endif
    {
    if (ci_fix(hp), !fsys::is_directory(widen(hp), ec)) { c->x[0] = kPathNotFound; return; }
    for (auto& e : fsys::directory_iterator(widen(hp), ec)) {
        bool dir = e.is_directory(ec);
        if ((dir && !(mode & 1)) || (!dir && !(mode & 2))) continue;
        std::u8string nm = e.path().filename().u8string();
        ents.push_back({ std::string(nm.begin(), nm.end()), { dir ? 0 : 1, dir ? 0 : (s64)e.file_size(ec) } });
    }
    }
    u64 v = g_dirs.put([&](HDir* d) { d->ents = std::move(ents); d->pos = 0; });
    if (!v) { printf("[fs] OpenDirectory: no free directory handle\n"); c->x[0] = kUnsupported; return; }
    *(u64*)c->x[0] = v; c->x[0] = 0;
}
HLE_AS(fs_ReadDirectory, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl") {
    HDir* d = g_dirs.get(c->x[2]);
    if (!d) { c->x[0] = kUnsupported; return; }
    u8* out = (u8*)c->x[1]; s64 max = (s64)c->x[3], n = 0;
    for (; n < max && d->pos < d->ents.size(); n++, d->pos++) {
        u8* e = out + n * 0x310; memset(e, 0, 0x310);
        auto& [name, ts] = d->ents[d->pos];
        memcpy(e, name.c_str(), std::min<size_t>(name.size(), 0x300));
        e[0x304] = (u8)ts.first; *(s64*)(e + 0x308) = ts.second;
    }
    *(s64*)c->x[0] = n; c->x[0] = 0;
}
HLE_AS(fs_CloseDirectory, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE") {
    g_dirs.close(c->x[0], [](HDir* d) { DirEnts().swap(d->ents); d->pos = 0; });   // (the listing is freed)
}

// game.save_backups, live (pc_settings.cpp): the commits from now on keep n backups (0 = none; the next commit trims the
// older ones). The save journal is Windows-only.
extern "C" void hle_fs_set_save_backups(int n) {
#ifdef _WIN32
    g_backups_set.store(n < 0 ? 0 : n > 1000 ? 1000 : n, std::memory_order_relaxed);
#else
    (void)n;
#endif
}
