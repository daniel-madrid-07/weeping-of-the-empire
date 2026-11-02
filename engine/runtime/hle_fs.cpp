#include <atomic>
#include "totk_version.h"
// HLE: nn::fs on host folders. Mount name -> host dir: the rom mount -> extract/romfs (read-only),
// save/cache/temporary storage -> userdata/<kind>. Paths are "<mount>:/a/b".
#include "hle.h"
#include "pc.h"
#include "pc_lang.h"   // pc_lang_code: the CJK glyph guard (glyph_patches)
#include "cpu_place.h"
#include "romfs_image.h"
#include "rom_source.h"
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
#include "mods.h"
#include "mods_scan.h"
#include "save_tree.h"   // (save_tree_same: a Commit that changed nothing is not published)
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
    // The Korean / Chinese font archives the bank covers too (bank.py CJK_FONT_ARCHIVES: 'fontalt' / 'altslot'): each
    // with the same bank font, its slots at their own offsets and key bytes, in g_bank.slots' order
    struct AltSlot { u64 hi, lo; u8 xhi, xlo; u16 cp; };
    struct Alt { std::string path; std::vector<AltSlot> slots; };
    std::vector<Alt> alts;
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
        else if (sscanf(line, "fontalt %u %255s", &v1, a) == 2) { if (v1 == b.alts.size()) b.alts.push_back({a, {}}); }
        else if (sscanf(line, "altslot %u %x %llu %u %llu %u", &v1, &cp, &x1, &v2, &x2, &v3) == 6) {
            if (v1 < b.alts.size()) b.alts[v1].slots.push_back({x1, x2, (u8)v2, (u8)v3, (u16)cp});
        }
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
    // an alt archive whose slots are not the main slots, in their order, is dropped: its text keeps the game's own glyphs
    // (the CJK guard) rather than pictures at the wrong code points
    for (auto& alt : b.alts) {
        bool same = alt.slots.size() == b.slots.size();
        for (size_t i = 0; same && i < alt.slots.size(); i++) same = alt.slots[i].cp == b.slots[i].cp;
        if (!same) {
            printf("[fs] glyph bank %s: %s's slots do not match the main ones: not covered\n", d.c_str(), alt.path.c_str());
            alt.slots.clear();
        }
    }
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
static void glyph_live_rearm();                                                 // (below)
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
// The Korean and Chinese font archives (Font/Font_KRko, Font_CNzh, Font_TWzh .Nin_NX_NVN.bfarc) are not covered by the
// bank (games/totk/glyphs/README.md): their External.bfotf is the game's own, byte for byte the main archive's before the
// bank adds its glyphs (U+F0xx, the game's own moved, and U+F100.., the bank's), so its cmap is found and rewritten like
// the main one's and the font engine's wrap asks it for glyphs it does not have: every button picture in text, the tabs'
// arrows and the A / B / X / Y prompts came out as tofu in Korean (public issue #12, v0.1.5, while the Korean text drew).
// Once the game opens one of them the bank's text part stays off: the characters, the cmap slots, the wrap and the
// DoCommand letter copies keep the game's own (Switch) button pictures; the masks (images) still follow the style.
// Covering them is the bank's work (one patched archive per language): v0.1.7.
static std::atomic<bool> g_cjk_text{false};
static int text_style(int style) { return g_cjk_text.load(std::memory_order_relaxed) && style >= 2 ? 0 : style; }
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
// A bank built with bank.py's CJK_FONT_ARCHIVES serves those archives with its glyphs (their slots: 'altslot'); one
// without them (older) leaves them as the game has them, and the CJK guard below keeps the text at the game's own
static bool bank_covers(const char* archive) {
    for (auto& alt : g_bank.alts)
        if (alt.path.find(archive) != std::string::npos && alt.slots.size() == g_bank.slots.size()) return true;
    return false;
}
static std::shared_ptr<const FilePatch> glyph_patches(const std::string& hp) {
    const GlyphBank& b = g_bank;
    if (b.dir.empty()) return nullptr;
    // (only in a Korean or Chinese game: an archive the game might open in another language never turns the bank off
    // for an en / ja / es player)
    static const bool cjk_game = [] {
        const char* l = pc_lang_code();
        return !strncmp(l, "ko", 2) || !strncmp(l, "zh", 2);
    }();
    if (cjk_game && !g_cjk_text.load(std::memory_order_relaxed))
        for (const char* a : {"Font_KRko.Nin_NX_NVN.bfarc", "Font_CNzh.Nin_NX_NVN.bfarc", "Font_TWzh.Nin_NX_NVN.bfarc"})
            if (hp.find(a) != std::string::npos && !bank_covers(a) && !g_cjk_text.exchange(true)) {
                printf("[fs] glyph bank: the game opened %s (Korean / Chinese text): the bank's text glyphs stay off, the "
                       "game's own button pictures in text (the bank does not cover that archive yet)\n", a);
                glyph_live_rearm();   // (what was rewritten already goes back at the live thread's next look)
            }
    bool font = hp == b.dir + "/" + b.font, common = hp == b.dir + "/" + b.common;
    const GlyphBank::Alt* alt = nullptr;   // (one of the Korean / Chinese archives the bank covers)
    for (auto& a : b.alts)
        if (hp == b.dir + "/" + a.path && a.slots.size() == b.slots.size()) alt = &a, font = true;
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
            if (alt) {
                const GlyphBank::AltSlot& a = alt->slots[&s - b.slots.data()];
                v.push_back({a.hi, (u8)((id >> 8) ^ a.xhi)});
                v.push_back({a.lo, (u8)((id & 0xFF) ^ a.xlo)});
                continue;
            }
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
            v.push_back({b.zoff(p.flag_at), (u8)(p.flag | (pane_shown(p, text_style(style)) ? 1 : 0))});
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
    unsigned gen = 0;                           // bumped by each serving that resets the state (glyph_live_search)
    std::chrono::steady_clock::time_point pane_search_at, rescan_at;   // the last search for pane objects; for a change
    std::atomic<long long> rescan_due{0};   // a rescan a change was too soon for: when the live loop runs it (live_ms; 0 none)
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
static long long live_ms() {   // (steady milliseconds, for rescan_due)
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
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
    if (cp < 0xE040 || cp > 0xE068 || g_bank.dir.empty() || g_cjk_text.load(std::memory_order_relaxed)) return cp;
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
    if (cp < 0xE040 || cp > 0xE068 || g_bank.dir.empty() || set < 0 || set >= kSets ||
        g_cjk_text.load(std::memory_order_relaxed))
        return cp;
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
// The live thread rewrites the game's copies at its next look whatever the epoch (glyph_patches: a Korean / Chinese font
// archive turned the bank's text part off)
static void glyph_live_rearm() {
    std::lock_guard<std::mutex> l(g_live.m);
    g_live.epoch = ~0u;
}
// glyph_patches: the font was served with these ids in the slots
static void glyph_live_font(const std::vector<u16>& ids) {
    std::lock_guard<std::mutex> l(g_live.m);
    if (!g_bank.cmap_sig.empty()) g_live.cmap_now = cmap_with(ids);
    g_live.cmap_at.clear();
    g_live.gen++;
}
// glyph_patches: the Common archive was served; keep its masks as the game got them
static void glyph_live_served(const std::string& hp, bool common, int style) {
    std::lock_guard<std::mutex> l(g_live.m);
    if (!common) return;
    g_live.gen++;
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
// The scan (seconds: 4.5 GB of heap and alias region) at below-normal CPU priority, so on a 4-core PC it takes no core
// from the game's and the render thread's work; it holds no lock others wait on (glyph_live_search). The thread's own
// priority again afterwards. TOTK_GLYPH_SCAN_PRIO=0: the thread's priority.
struct ScanPrio {
    int old = THREAD_PRIORITY_ERROR_RETURN;
    ScanPrio() {
        static const bool off = getenv("TOTK_GLYPH_SCAN_PRIO") && !strcmp(getenv("TOTK_GLYPH_SCAN_PRIO"), "0");
        old = GetThreadPriority(GetCurrentThread());
        if (off || old == THREAD_PRIORITY_ERROR_RETURN || old <= THREAD_PRIORITY_BELOW_NORMAL ||
            !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
            old = THREAD_PRIORITY_ERROR_RETURN;
    }
    ~ScanPrio() { if (old != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), old); }
};
static void mem_find(std::vector<MemFind>& f, const std::vector<std::pair<const u8*, size_t>>& mine) {
    if (f.empty()) return;
    ScanPrio prio;
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
#if !NXR_GAME_TOTK
    return 0;   // (TotK's ScalableFontMgr: another game has none at these addresses, nxr_game.h)
#endif
#if defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
    if (!totk_main_known_rt(0x463fef0) || !totk_main_known_rt(0x44fd948)) return 0;   // (another version: not placed)
#endif
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
// The copies found again: those still holding what they were last given; a search for the rest. The search runs without
// g_live.m, which the file serving's glyph_live_served / font / rearm take: its patterns are copied out under the lock
// (glyph_live_take), looked for unlocked (glyph_live_scan), and the hits put back under it (glyph_live_put), dropped if
// the serving reset the state meanwhile (g_live.gen).
struct LiveSearch {
    unsigned gen = 0;                           // g_live.gen when taken
    std::vector<std::vector<u8>> pats;          // the patterns (copies)
    std::vector<int> target;                    // per pattern: a mask's index, kTargetGki, kTargetCmap, or kTargetPane - i
    std::vector<bool> wide;
    std::vector<std::vector<u8*>> hits;
    size_t copies = 0;                          // the patterns before the panes' names
    std::vector<size_t> pane_search;
    std::vector<std::pair<const u8*, size_t>> mine;   // this file's own copies (hits there are not the game's)
    double ms = 0;
};
constexpr int kTargetGki = -1, kTargetCmap = -2, kTargetPane = -100;
constexpr std::chrono::seconds kPaneSearchGap{600}, kRescanGap{60};
static std::vector<u8*>* live_target(int t) {   // (g_live.m held)
    if (t == kTargetGki) return &g_live.gki_at;
    if (t == kTargetCmap) return &g_live.cmap_at;
    if (t >= 0) return (size_t)t < g_live.mask_at.size() ? &g_live.mask_at[t] : nullptr;
    const size_t i = (size_t)(kTargetPane - t);
    return i < g_live.pane_hits.size() ? &g_live.pane_hits[i] : nullptr;
}
// g_live.m held: the copies known checked (those changed are forgotten); false when nothing is left to look for.
// search false: the check alone (the pane objects' search window is not used)
static bool glyph_live_take(LiveSearch& s, bool search = true) {
    s = LiveSearch();
    s.gen = g_live.gen;
    auto keep = [&](std::vector<u8*>& at, const std::vector<u8>& now, int target) {
        std::vector<u8*> ok;
        for (u8* a : at) if (!memcmp(a, now.data(), now.size())) ok.push_back(a);
        at = ok;
        if (at.empty() && now.size() >= 16) s.pats.push_back(now), s.target.push_back(target), s.wide.push_back(true);
    };
    for (size_t i = 0; i < g_live.mask_now.size(); i++) if (!g_live.mask_now[i].empty()) keep(g_live.mask_at[i], g_live.mask_now[i], (int)i);
    keep(g_live.gki_at, g_live.gki_now, kTargetGki);
    keep(g_live.cmap_at, g_live.cmap_now, kTargetCmap);
    s.copies = s.pats.size();
    // DoCommand's pane copies: the objects and the resource copy still there; a search by name while either is missing
    // (the objects exist once the HUD is built, and are built again with it), at most every kPaneSearchGap, and each
    // search is a full heap + alias scan
    const auto now = std::chrono::steady_clock::now();
    const bool panes_due = search && (g_live.pane_search_at == std::chrono::steady_clock::time_point() ||
                                      now - g_live.pane_search_at >= kPaneSearchGap);
    if (panes_due) g_live.pane_search_at = now;
    for (size_t i = 0; i < g_bank.panes.size() && i < g_live.pane_obj.size(); i++) {
        const auto& p = g_bank.panes[i];
        auto& obj = g_live.pane_obj[i];
        auto& res = g_live.pane_res[i];
        obj.erase(std::remove_if(obj.begin(), obj.end(), [&](u8* a) { return !pane_object_ok(a, p); }), obj.end());
        res.erase(std::remove_if(res.begin(), res.end(), [&](u8* a) { return !pane_resource(a, p); }), res.end());
        if (!obj.empty() && !res.empty()) continue;
        if (!panes_due) continue;
        g_live.pane_hits[i].clear();
        s.pats.emplace_back((const u8*)p.name.c_str(), (const u8*)p.name.c_str() + p.name.size() + 1);
        s.target.push_back(kTargetPane - (int)i);
        s.wide.push_back(false);   // (guest memory only)
        s.pane_search.push_back(i);
    }
    if (s.pats.empty()) return false;
    s.mine = {{g_bank.gki_sig.data(), g_bank.gki_sig.size()}, {g_live.gki_now.data(), g_live.gki_now.size()},
              {g_bank.cmap_sig.data(), g_bank.cmap_sig.size()}, {g_live.cmap_now.data(), g_live.cmap_now.size()}};
    for (auto* v : {&g_live.mask_orig, &g_live.mask_now}) for (auto& t : *v) s.mine.push_back({t.data(), t.size()});
    for (auto& p : g_bank.panes) s.mine.push_back({(const u8*)p.name.c_str(), p.name.size() + 1});
    for (auto& p : s.pats) s.mine.push_back({p.data(), p.size()});
    return true;
}
// without the lock: the search itself
static void glyph_live_scan(LiveSearch& s) {
    s.hits.assign(s.pats.size(), {});
    std::vector<MemFind> f;
    for (size_t k = 0; k < s.pats.size(); k++) {
        MemFind e{s.pats[k].data(), s.pats[k].size(), 0, 0, &s.hits[k]};
        e.wide = s.wide[k];
        f.push_back(e);
    }
    const auto t0 = std::chrono::steady_clock::now();
    mem_find(f, s.mine);
    s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// g_live.m held: the hits where they belong, if the state is still the one searched
static void glyph_live_put(LiveSearch& s) {
    if (s.gen != g_live.gen) {
        printf("[fs] glyph bank live: the files were served again during the search (%.0f ms); its hits dropped\n", s.ms);
        return;
    }
    for (size_t k = 0; k < s.pats.size(); k++)
        if (std::vector<u8*>* at = live_target(s.target[k]); at && at->empty()) *at = s.hits[k];
    if (!s.pane_search.empty()) {
        int objs = 0, res = 0;
        const u8* stray = nullptr;
        for (size_t i : s.pane_search) {
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
               objs, g_bank.panes.size(), res, s.pane_search.size(), missing.empty() ? "" : "; no object:", missing.c_str());
        if (stray && !g_live.pane_dumped) { pane_dump(stray); g_live.pane_dumped = true; }
        if (!s.copies) return;
    }
    u8* const heap = (u8*)g_rt_heap_base;
    int found = 0;
    std::string where;
    for (size_t k = 0; k < s.copies; k++) {
        found += !s.hits[k].empty();
        for (u8* a : s.hits[k]) {
            char t[64];
            u64 ab = 0, an = 0;
            rt_alias_carved(&ab, &an);
            snprintf(t, sizeof t, " %p%s", (void*)a, a >= heap && a < heap + kernel_heap_size() ? "(heap)"
                                                   : (u64)a >= ab && (u64)a < ab + an ? "(alias)" : "(host)");
            if (where.size() < 600) where += t;
        }
    }
    g_live.located = true;
    printf("[fs] glyph bank live: %d of %zu copies found in %.0f ms:%s\n", found, s.copies, s.ms, where.c_str());
}
// The search, its lock held only around the take and the put (any thread but the file serving's)
static void glyph_live_search() {
    LiveSearch s;
    {
        std::lock_guard<std::mutex> l(g_live.m);
        if (!glyph_live_take(s)) return;
    }
    glyph_live_scan(s);
    std::lock_guard<std::mutex> l(g_live.m);
    glyph_live_put(s);
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
        if (g_font_wrapped.load(std::memory_order_relaxed) || g_cjk_text.load(std::memory_order_relaxed)) break;
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
        const int gi = g_cjk_text.load(std::memory_order_relaxed) ? -1 : glyph_index(style, b.slots[mc.slot].control);
        const u16 cp = (u16)(gi >= 0 ? glyph_codepoint(gi) : game_codepoint(mc.cp));
        if (cp == mc.now) continue;
        if (msg_put(mc.at, mc.now, cp)) mc.now = cp, msg_done++;
        else msg_gone++;
    }
    if (msg_gone * 2 > (int)g_live.msg.size()) g_live.msg.clear(), g_live.msg_looked = false;   // (reloaded: look again)
    // the font's slots too: literal button characters laid out from now on, and any the font engine draws again
    std::vector<u16> ids;
    for (auto& s : b.slots) {
        const char* g = g_cjk_text.load(std::memory_order_relaxed) ? nullptr : hid_glyph_for(style, s.control.c_str());
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
        const u8 vis = pane_shown(p, text_style(style)) ? 1 : 0;
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
    // (a pane object not found does not ask for a search: they are looked for every kPaneSearchGap at most)
    bool missing = g_live.gki_at.empty() || (!g_live.cmap_now.empty() && g_live.cmap_at.empty());
    for (size_t i = 0; i < g_live.mask_now.size(); i++) missing |= !g_live.mask_now[i].empty() && g_live.mask_at[i].empty();
    return missing;
}
// A change: the copies known are written at once, then the others looked for (a scan of seconds) and written.
static void glyph_live_apply(unsigned epoch) {
    const int style = hid_glyph_active();
    {
        std::lock_guard<std::mutex> l(g_live.m);
        LiveSearch known;
        glyph_live_take(known, false);   // (the copies known checked; no search yet)
        // a copy not found searched for at most every kRescanGap: each search is seconds of heap + alias scan, and a
        // copy can stay missing (every device switch rescanned before, cpu-hot 11-02)
        const auto now = std::chrono::steady_clock::now();
        if (!glyph_live_write(style, true)) {
            g_live.epoch = epoch;
            return;
        }
        if (g_live.rescan_at != std::chrono::steady_clock::time_point() && now - g_live.rescan_at < kRescanGap) {
            // (too soon: the live loop runs it once the gap has passed, so a moved copy does not wait for the next
            // change)
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kRescanGap - (now - g_live.rescan_at));
            g_live.rescan_due = live_ms() + (long long)left.count() + 1;
            g_live.epoch = epoch;
            return;
        }
        g_live.rescan_at = now;
        g_live.rescan_due = 0;
    }
    glyph_live_search();
    std::lock_guard<std::mutex> l(g_live.m);
    glyph_live_write(style, false);
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
                     // prompts at a menu / mount switch with the reset; 250 ms before that)
            bool kicked;
            {
                std::unique_lock<std::mutex> l(g_kick_mu);
                g_kick_cv.wait_for(l, std::chrono::milliseconds(20), [] { return g_kicked; });
                kicked = g_kicked;
                g_kicked = false;
            }
            if (!g_live.served) continue;
            if (!g_live.located && std::chrono::steady_clock::now() - g_live.served_at > std::chrono::seconds(90)) {
                glyph_live_search();
                std::lock_guard<std::mutex> l(g_live.m);
                g_live.located = true;
            }
            unsigned e = hid_glyph_epoch();
            static ULONGLONG seen_at;  // (when the new epoch was first seen: the glyph switch's latency)
            if (const long long due = g_live.rescan_due.load(); due && live_ms() >= due) {
                g_live.rescan_due = 0;
                glyph_live_apply(e);   // (the rescan an earlier change was too soon for)
                continue;
            }
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
// (the name is the build's version's: 1.0.0 .Product.100.rsizetable.zs; later versions, e.g. 1.4.3, add .Nin_NX_NVN:
// .Product.143.Nin_NX_NVN.rsizetable.zs, so the player's mods' tables merge there too)
#if NXR_GAME_TOTK && defined(TOTK_GAME_VERSION) && TOTK_GAME_VERSION != 100
#define TOTK_RSTB_VER2(v) #v
#define TOTK_RSTB_VER(v) TOTK_RSTB_VER2(v)
static const char* const kGlyphRstb =
    "System/Resource/ResourceSizeTable.Product." TOTK_RSTB_VER(TOTK_GAME_VERSION) ".Nin_NX_NVN.rsizetable.zs";
#else
static const char* const kGlyphRstb = "System/Resource/ResourceSizeTable.Product.100.rsizetable.zs";
#endif
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
    // Common archive, so its own bank, engine+glyphs@aspect-<w>x<h>[-center] (build.py glyph_bank; -center: the HUD in
    // the middle 16:9, graphics.hud_layout); the 16:9 one engine+glyphs
    std::string bank = base + "/engine+glyphs";
    if (const size_t a = od.find("aspect-"); a != std::string::npos) {
        size_t e = a + 7;
        while (e < od.size() && (isdigit((unsigned char)od[e]) || od[e] == 'x')) e++;
        if (od.compare(e, 7, "-center") == 0) e += 7;
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
// The player's mods (mods.cpp: the launchers' mods folders, or TOTK_USER_MODS): their RomFS roots this run, top first
// (none with TOTK_NO_OVERLAY, the pristine RomFS A/B)
static const std::vector<std::string>& user_mods() {
    static const std::vector<std::string> none;
    static const bool no_overlay = getenv("TOTK_NO_OVERLAY") != nullptr;
    return no_overlay ? none : mods_romfs_start();
}
enum { kByPort = -1 };   // rom_replacement's *who for a port file; a user mod's is its index in user_mods()
// The file that replaces the RomFS file `rel` ("/a/b"), if one does: the port's own first (skip-opening, our sets, the
// glyph bank, the overlay), then the player's mods (`user`). *who: kByPort or the mod's index. `log`: the open path's
// "[fs] ..." lines (host_path_ours; the mods' file count passes false). No other side effects.
static bool rom_replacement(const std::string& rel, bool user, std::string* out, int* who, bool log) {
    auto hit = [&](const std::string& p, int w) { *out = p; if (who) *who = w; return true; };
    if (g_skip_opening && replacement_exists(kSkipDir, rel)) {
        if (log) printf("[fs] skip-opening overlay %s\n", (kSkipDir + rel).c_str());
        return hit(kSkipDir + rel, kByPort);
    }
    // TOTK_MODS: our RomFS sets (totk_engine.c puts them here), first listed wins
    static const std::vector<std::string> sets = mod_dirs("TOTK_MODS", "mod");
    for (const auto& d : sets)
        if (replacement_exists(d, rel)) {
            static int logged; if (log && logged++ < 64) printf("[fs] mod file %s\n", (d + rel).c_str());
            return hit(d + rel, kByPort);
        }
    static const bool glyphs = true;   // (TOTK_GLYPHS: glyph_dir decides; on by default)
    if (glyphs && (rel.rfind("/UI/", 0) == 0 || rel.rfind("/Font/", 0) == 0 || rel == "/" + std::string(kGlyphRstb))) {
        const std::string& gd = glyph_dir();   // pad glyphs (above)
        if (!gd.empty() && replacement_exists(gd, rel)) {
            static int logged; if (log && logged++ < 8) printf("[fs] glyph file %s\n", (gd + rel).c_str());
            return hit(gd + rel, kByPort);
        }
    }
    // LayeredFS-style overlay: an existing file overlay/romfs/<path> replaces the romfs one
    if (replacement_exists(overlay_dir(), rel)) {
        static int logged; if (log && logged++ < 32) printf("[fs] overlay %s\n", (overlay_dir() + rel).c_str());
        return hit(overlay_dir() + rel, kByPort);
    }
    // The player's mods come after our sets, the glyph bank and the overlay, so a mod cannot take away the port's own
    // files (PC glyphs, the game-over and quick-menu fixes, our RSTB); among themselves the top of the list wins.
    // Their resource size tables are merged into the winner's instead (host_path below)
    if (user && rel != "/" + std::string(kGlyphRstb)) {
        const auto& um = user_mods();
        for (int i = 0; i < (int)um.size(); i++)
            if (replacement_exists(um[i], rel)) {
                static int logged; if (log && logged++ < 64) printf("[fs] user mod file %s\n", (um[i] + rel).c_str());
                return hit(um[i] + rel, i);
            }
    }
    return false;
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
    if (it->second == kRomDir && !no_overlay) rom_replacement(rel, true, out, nullptr, true);
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

struct ModsIndex;
static const ModsIndex& mods_index();
static bool rom_base_head(const std::string& rel, u8* head, size_t n, size_t* got, u64* size);
static u64 host_content_size(const std::string& path, const std::string& rel, u64 size);
static size_t rstb_mod_files(rstb::Table& out, const rstb::Table& base, size_t* estimated, size_t* other);

// The resource size table the game gets with the player's mods on: the one that wins without them (ours: the
// overlay's, or our sets' combined one) with every mod's own table merged in at the larger size (rstb_merge.h), then
// each file a mod serves raised to what its size needs (rstb_mod_files: the tables mods for other versions bring, and
// estimates), written once per start as a stored zstd frame to the temp folder. "" (serve `base`) when nothing needed
// raising, or when `base` does not read as a table.
bool hle_fs_read_romfs(const char* rel, std::vector<u8>* out);
// a file the guest's paths resolved to: one under the RomFS folder through hle_fs_read_romfs (the pack when one serves
// the RomFS: a player's install keeps no unpacked folder), any other (an overlay's, our sets') as is
static std::vector<u8> read_resolved(const std::string& hp) {
    const size_t n = strlen(kRomDir);
    if (hp.compare(0, n, kRomDir) == 0 && hp.size() > n && (hp[n] == '/' || hp[n] == '\\')) {
        const size_t a = hp.find_first_not_of("/\\", n);
        std::vector<u8> v;
        if (a == std::string::npos) return v;
        std::string rel = hp.substr(a);
        for (char& ch : rel) if (ch == '\\') ch = '/';
        hle_fs_read_romfs(rel.c_str(), &v);
        return v;
    }
    return read_host_file(hp);
}
// `z` written to the temp folder as <prefix><pid>.zs for the game to open: one file per game process, written beside
// and renamed into place; the ones of that prefix a day old (games that were killed) are removed here. Its path, or
// "" when it cannot be written.
static std::string temp_file(const char* prefix, const std::vector<u8>& z) {
    std::error_code ec;
    const fsys::path dir = fsys::temp_directory_path(ec);
    if (ec) return "";
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    // (increment(ec), not a range-for: its operator++ throws on an iteration error, here inside call_once)
    for (fsys::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        std::error_code e2;
        if (n.rfind(prefix, 0) == 0 &&
            fsys::last_write_time(it->path(), e2) < fsys::file_time_type::clock::now() - std::chrono::hours(24) && !e2)
            fsys::remove(it->path(), e2);
    }
    ec.clear();
    const fsys::path out = dir / (prefix + std::to_string(pid) + ".zs"), tmp = fsys::path(out) += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write((const char*)z.data(), (std::streamsize)z.size());
        if (!f.good()) { f.close(); fsys::remove(tmp, ec); return ""; }
    }
    fsys::rename(tmp, out, ec);
    if (ec) { fsys::remove(tmp, ec); return ""; }
    const std::u8string u8 = out.u8string();
    return std::string(u8.begin(), u8.end());
}
static std::string rstb_merged(const std::string& base) {
    const std::string rel = "/" + std::string(kGlyphRstb);
    rstb::Table t;
    std::vector<u8> raw;
    if (!rstb::unzstd(read_resolved(base), &raw) || !rstb::parse(raw, &t)) {
        printf("[fs] resource size table %s: not readable, mods' tables not merged\n", base.c_str());
        return "";
    }
    const rstb::Table ours = t;
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
    size_t estimated = 0, other = 0;
    const size_t computed = rstb_mod_files(t, ours, &estimated, &other);
    printf("[fs] resource size table: %zu entries raised for the mods' files (%zu added files estimated, %zu from tables "
           "for another version)\n", computed, estimated, other);
    if (!n && !computed) return "";
    const std::string path = temp_file("totk-rstb-merged-", rstb::zstd_stored(rstb::serialize(t)));
    if (path.empty()) return "";
    printf("[fs] resource size table: %s + %d mod table(s): %zu entries raised, %zu added; %zu raised for mod files -> %s\n",
           base.c_str(), n, c.raised, c.added, computed, path.c_str());
    return path;
}
// The game's own copy of a RomFS file, rebuilt by its glue at the first open (TotK: the player camera's pack with
// controls.stick_camera_speed, totk_camera_speed.cpp), from the file that would be served (the RomFS's, our overlay's
// or a player's mod's). "" (serve `base`): no rebuild wanted, or it failed (the glue logs why).
extern "C" const char* totk_camera_pack_rel(void);                               // totk_camera_speed.cpp, game/none.cpp
bool totk_camera_pack_build(const std::vector<uint8_t>& in, std::vector<uint8_t>* out);   // (none.cpp: never)
static std::string game_rebuilt(const std::string& base) {
    std::vector<u8> z;
    if (!totk_camera_pack_build(read_resolved(base), &z)) return "";
    const std::string path = temp_file("totk-camera-pack-", z);
    if (!path.empty()) printf("[fs] %s rebuilt -> %s\n", base.c_str(), path.c_str());
    return path;
}

// Every lookup but two is host_path_ours: the resource size table gets the mods' tables merged in (rstb_merged), and
// the file the game glue rebuilds is served from its rebuilt copy (game_rebuilt)
static bool host_path(const char* p, std::string* out) {
    if (!host_path_ours(p, out)) return false;
    static const std::string rstb_rel = "/" + std::string(kGlyphRstb);
    static const char* const rebuilt_rel = totk_camera_pack_rel();   // (nullptr: none)
    const char* colon = strchr(p, ':');
    if (!colon) return true;
    const bool rstb = rstb_rel == colon + 1 && !user_mods().empty() && !getenv("TOTK_NO_OVERLAY");
    const bool rebuilt = rebuilt_rel && !strcmp(rebuilt_rel, colon + 1);
    if (!rstb && !rebuilt) return true;
    {
        std::lock_guard<std::mutex> l(g_fs_mtx);
        auto it = g_mounts.find(std::string(p, (size_t)(colon - p)));
        if (it == g_mounts.end() || it->second != kRomDir) return true;
    }
    static std::once_flag once, once_rebuilt;
    static std::string merged, rebuilt_path;
    if (rstb) std::call_once(once, [&] { merged = rstb_merged(*out); });
    if (rebuilt) std::call_once(once_rebuilt, [&] { rebuilt_path = game_rebuilt(*out); });
    const std::string& use = rstb ? merged : rebuilt_path;
    if (!use.empty()) *out = use;
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
static bool wait_publisher(DWORD ms = INFINITE) {   // true: no publish under way
    std::unique_lock<std::mutex> l(g_pub_mtx);
    if (ms == INFINITE) { g_pub_cv.wait(l, [] { return !g_pub_busy; }); return true; }
    return g_pub_cv.wait_for(l, std::chrono::milliseconds(ms), [] { return !g_pub_busy; });
}
// No unbounded wait on the save path (#47, v0.1.8: an autosave never committed and the game waited forever while the
// picture went on). The commit lock (a named mutex shared with other processes), a Commit's wait for the previous
// publish and for the changes under way, and a change's wait for a pending snapshot each give up after
// TOTK_SAVE_WAIT_MS (default 5000) with a "[fs] save ..." line saying what it waited for and where the commit was
// (g_save_stage). The guest then goes on. A commit that could not be made safely is left for the next Commit / start
// (.pending), and a snapshot that a change could not wait for is dropped, never published (it may hold a mix of saves).
static const DWORD g_save_wait_ms = getenv("TOTK_SAVE_WAIT_MS") && atoi(getenv("TOTK_SAVE_WAIT_MS")) > 0
                                        ? (DWORD)atoi(getenv("TOTK_SAVE_WAIT_MS")) : 5000;
static std::atomic<const char*> g_save_stage{"idle"};   // the commit's step, for those lines
static std::atomic<u32> g_commit_no;                    // the commits of this run, numbered in the lines
static std::atomic<bool> g_commit_skipped;              // a Commit gave up waiting: the save stays .pending
static std::mutex g_pending_mtx;                        // pending() from the game thread and the publisher
static std::atomic<bool> g_pub_stuck;                   // a Commit gave up on the publish under way: later ones do not wait
static bool g_never_publish;   // this run started empty while another held the lock mid-swap: it never publishes over that save
// The snapshot in the background (TOTK_SAVE_SNAP_BG, default on with the async publish; =0: on the game thread as
// before; stutter plan fix 6: 31 and 63 ms frames at autosaves). The working copy must not change while it is taken, or
// a commit could hold some files of the game's next save and not others. So Commit starts it only after the changes
// already under way have finished, and every change to the save mount waits in a SaveWrite while it is pending. The
// changes are the guest's WriteFile, SetFileSize, CreateFile, DeleteFile, RenameFile, CreateDirectory,
// DeleteDirectoryRecursively and CleanDirectoryRecursively on the save mount: the only ways nn::fs changes it (OpenFile
// and FlushFile change no bytes), and nothing else in the runtime writes the working copy (it is this process's own
// save.work.<pid>). Reads go on: the snapshot only reads.
static const bool g_snap_bg = g_save_async && !(getenv("TOTK_SAVE_SNAP_BG") && !strcmp(getenv("TOTK_SAVE_SNAP_BG"), "0"));
static std::mutex g_snap_mtx;
static std::condition_variable g_snap_cv;
static bool g_snap_pending;    // a Commit's snapshot not taken yet: changes wait
static bool g_snap_torn;       // a change gave up waiting for the pending snapshot: that snapshot is dropped
static int g_snap_writers;     // changes to the save mount under way
static std::atomic<u32> g_snap_waits;      // changes that waited for a snapshot, and how long in all (the publish line)
static std::atomic<u64> g_snap_wait_ms;
static std::atomic<u32> g_save_changes;    // changes to the save mount since the last Commit (the publish line: > 0 at a save)
static bool in_save(const std::string& hp) {   // a host path inside the save mount's working copy
    const size_t n = g_save_work.size();
    return n && hp.size() >= n && !hp.compare(0, n, g_save_work) && (hp.size() == n || hp[n] == '/' || hp[n] == '\\');
}
struct SaveWrite {   // a change to the save mount: waits while a snapshot is pending, and holds the next one off
    bool on = false;
    explicit SaveWrite(bool save) {
        if (save) g_save_changes++;
        if (!save || !g_snap_bg) return;
        std::unique_lock<std::mutex> l(g_snap_mtx);
        if (g_snap_pending && !g_snap_torn) {
            u64 t = GetTickCount64();
            if (!g_snap_cv.wait_for(l, std::chrono::milliseconds(g_save_wait_ms), [] { return !g_snap_pending || g_snap_torn; })) {
                g_snap_torn = true;   // (later changes go on at once; the publisher drops this snapshot)
                printf("[fs] save write: waited %lu ms for commit #%u's snapshot (it is at: %s); writing now, that commit is "
                       "dropped and the next one publishes\n", (unsigned long)g_save_wait_ms, g_commit_no.load(), g_save_stage.load());
                fflush(stdout);
            }
            g_snap_waits++; g_snap_wait_ms += GetTickCount64() - t;
        }
        g_snap_writers++; on = true;
    }
    ~SaveWrite() {
        if (!on) return;
        { std::lock_guard<std::mutex> l(g_snap_mtx); g_snap_writers--; }
        g_snap_cv.notify_all();
    }
    SaveWrite(const SaveWrite&) = delete;
    SaveWrite& operator=(const SaveWrite&) = delete;
};

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
// Who holds the commit lock, for the line of one that waits too long: process, thread and since when, in a small named
// mapping beside the mutex (every process that takes it writes it).
struct SaveLockOwner { DWORD pid, tid; u64 since_ms; };
static SaveLockOwner* lock_owner() {
    static SaveLockOwner* p = [] {
        HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SaveLockOwner), L"Local\\totk-recomp-save-commit-owner");
        return m ? (SaveLockOwner*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SaveLockOwner)) : nullptr;
    }();
    return p;
}
// true: held (or no lock to take); false: still held elsewhere after g_save_wait_ms (said, with the holder)
static bool commits_lock(const char* who) {
    DWORD r = WaitForSingleObject(g_save_mutex, g_save_wait_ms);   // abandoned: the on-disk state is recoverable (.new / .ready)
    if (r == WAIT_TIMEOUT) {
        SaveLockOwner o{};
        if (SaveLockOwner* p = lock_owner()) memcpy(&o, p, sizeof o);   // (another process writes it: a hint only)
        printf("[fs] save %s: the commit lock is still held after %lu ms, by process %lu%s thread %lu for %llu ms; %s\n", who,
               (unsigned long)g_save_wait_ms, (unsigned long)o.pid, o.pid == GetCurrentProcessId() ? " (this one)" : "",
               (unsigned long)o.tid, (unsigned long long)(o.since_ms ? GetTickCount64() - o.since_ms : 0),
               strcmp(who, "data") ? "going on without it" : "the start waits up to 60 s for it");
        fflush(stdout);
        return false;
    }
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
        printf("[fs] save commit mutex: wait failed (%lu)\n", GetLastError());
        return true;   // (no lock to take: as before, the commit goes on)
    }
    if (SaveLockOwner* p = lock_owner()) *p = SaveLockOwner{GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64()};
    return true;
}
static void commits_unlock() {
    if (SaveLockOwner* p = lock_owner()) *p = SaveLockOwner{};
    ReleaseMutex(g_save_mutex);
}
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
// After snapshot_locked (mutex held): a snapshot equal to the committed save (save_tree.h: the game rewrote its files with
// the same bytes, as it does at each start) is dropped instead of published, so it takes no backup's place; the committed
// save already holds it. TOTK_SAVE_SAME=publish publishes it anyway (the old behaviour, for A/Bs).
static bool snapshot_unchanged() {
    static const bool off = getenv("TOTK_SAVE_SAME") && !strcmp(getenv("TOTK_SAVE_SAME"), "publish");
    if (off || wexists(rd_dir()) || !save_tree_same(nw_dir(), g_committed)) return false;
    if (!remove_tree(nw_dir())) printf("[fs] save commit: unchanged, but %ls could not be removed (the next commit does)\n", nw_dir().c_str());
    printf("[fs] save commit: no change since the committed save; not published, no backup\n");
    return true;
}
// publish `work` as the committed save in one go (mutex held). On failure the committed save is unchanged.
static bool publish_locked(const std::wstring& work) {
    u64 t0 = GetTickCount64();
    if (!snapshot_locked(work, nullptr)) return false;
    if (snapshot_unchanged()) return true;
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
    // Held elsewhere: another process is in a commit (an earlier run's exit publish, say). The start is not frame-critical,
    // so it waits up to 60 s (a line every TOTK_SAVE_WAIT_MS). Still held: that process's .new / .ready are its own and it
    // may be mid-swap (save moved away, save.ready not in place yet), so nothing is created or recovered here; the working
    // copy starts read-only from the committed save, else from a complete save.ready, else empty with a loud line.
    bool locked = false;
    for (u64 t = GetTickCount64(); !(locked = commits_lock("data")) && GetTickCount64() - t < 60000;) {}
    std::wstring source = g_committed;   // the newest complete save: the working copy starts from it
    if (!locked && !wexists(g_committed)) {
        if (wexists(rd_dir())) {
            source = rd_dir();
            printf("[fs] save data: another process holds the commit lock and is putting its save in place; playing from its save.ready\n");
        } else {
            g_never_publish = true;
            printf("[fs] save data: WARNING: another process holds the commit lock and there is no committed save to start from: "
                   "this run starts with no save and publishes none (yours is not deleted: it is put in place when that "
                   "process finishes; restart the game then)\n");
        }
    }
    if (locked && wexists(rd_dir())) {
        printf("[fs] save data: finishing a commit interrupted after it was complete (save.ready)\n");
        if (!swap_ready()) { printf("[fs] save data: the interrupted commit cannot be put in place now (error %lu): playing from save.ready\n", GetLastError()); source = rd_dir(); }
    }
    if (locked && wexists(nw_dir()) && wexists(complete_marker()) && !wexists(rd_dir())) {   // a finished snapshot whose rename failed
        printf("[fs] save data: finishing a commit whose snapshot was complete (save.new)\n");
        if (move_retry(nw_dir(), rd_dir())) { mark(complete_marker(), false); if (swap_ready()) source = g_committed; else source = rd_dir(); }
    }
    if (locked && wexists(nw_dir()) && !wexists(complete_marker()) && !remove_tree(nw_dir()))
        printf("[fs] save data: an incomplete commit (save.new) could not be removed; the previous commit stays\n");
    if (locked) fsys::create_directories(g_committed, ec);
    if (locked) drop_stale_work(ud);
    g_work_lock = CreateFileW((g_work + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    remove_tree(g_work);
    if (!copy_tree(source, g_work) && !(source != g_committed && remove_tree(g_work) && copy_tree(g_committed, g_work)))
        printf("[fs] save data: making the working copy %ls failed (error %lu)\n", g_work.c_str(), GetLastError());
    if (locked) commits_unlock();
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
static void pending(bool on) {   // (the game thread's Commit or the publisher)
    std::lock_guard<std::mutex> l(g_pending_mtx);
    std::wstring pend = g_work + L".pending";
    if (on && !g_commit_pending) { mark(pend, true); g_commit_pending = true; }   // retried at the next Commit / start
    // (not while a Commit that gave up waiting is uncovered: an earlier snapshot's publish does not hold its changes)
    if (!on && g_commit_pending && !g_commit_skipped) { DeleteFileW(pend.c_str()); g_commit_pending = false; }
}
// A Commit that gave up waiting (said): the save is left .pending for the next Commit / start, the game goes on
static void commit_skipped(u32 n, const char* what) {
    { std::lock_guard<std::mutex> l(g_pending_mtx); g_commit_skipped = true; }
    pending(true);
    printf("[fs] save commit #%u: %s for %lu ms (the previous commit is at: %s); not committed now, the next commit or "
           "the next start does it\n", n, what, (unsigned long)g_save_wait_ms, g_save_stage.load());
    fflush(stdout);
}
// A snapshot is about to be taken: it holds every change so far, a skipped Commit's too
static void commit_covers_skipped() { std::lock_guard<std::mutex> l(g_pending_mtx); g_commit_skipped = false; }
static void commit_save() {
    std::lock_guard<std::mutex> l(g_commit_mtx);
    const u32 n = ++g_commit_no;
    if (g_never_publish) {   // (save_init's WARNING: the player's real save is being put in place by another process)
        if (n == 1 || n % 10 == 0) printf("[fs] save commit #%u: not published: this run started without your save (see the WARNING at start)\n", n);
        return;
    }
    if (!g_save_async) {
        if (!commits_lock("commit")) { commit_skipped(n, "waited for the commit lock"); return; }
        commit_covers_skipped();
        bool ok = publish_locked(g_work);
        commits_unlock();
        pending(!ok);
        return;
    }
    u64 tw = GetTickCount64();
    if (!wait_publisher(g_pub_stuck ? 0 : g_save_wait_ms)) {
        g_pub_stuck = true;
        commit_skipped(n, "waited for the previous commit's background publish");
        return;
    }
    u64 t0 = GetTickCount64();
    if (t0 - tw >= 50) printf("[fs] save commit #%u: waited %llu ms for the previous commit's background publish\n", n, (unsigned long long)(t0 - tw));
    auto copied = std::make_shared<std::vector<std::wstring>>();
    if (g_snap_bg) {   // the snapshot too in the background (SaveWrite above)
        {
            std::unique_lock<std::mutex> l(g_snap_mtx);
            g_snap_pending = true;
            g_snap_torn = false;
            // (a change under way on another thread finishes)
            if (!g_snap_cv.wait_for(l, std::chrono::milliseconds(g_save_wait_ms), [] { return g_snap_writers == 0; })) {
                g_snap_pending = false;
                l.unlock();
                g_snap_cv.notify_all();
                commit_skipped(n, "waited for a change to the save that does not finish");
                return;
            }
        }
        commit_covers_skipped();
        g_save_stage = "starting";
        { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = true; }
        const u64 game = GetTickCount64() - t0;
        const u32 changes = g_save_changes.exchange(0);
        g_snap_waits = 0; g_snap_wait_ms = 0;
        printf("[fs] save commit #%u: start (%u changes to the save since the last commit; snapshot in the background)\n", n, changes);
        std::thread([copied, game, changes, n] {
            // the.pending marker (its create + flush: the commit's last game-thread stall) first: it only has to be on
            // disk before finish_locked touches the published save; the snapshot writes save.new alone. Until the
            // publish lands; a failed snapshot is retried at the next Commit / start.
            pending(true);
            u64 t1 = GetTickCount64();
            g_save_stage = "waiting for the commit lock";
            const bool locked = commits_lock("commit");
            u64 tl = GetTickCount64();
            bool ok = false, same = false;
            if (locked) {
                // TOTK_SAVE_SNAP_DELAY_MS=<ms> (tests): the snapshot starts that much later, so the game's next changes
                // wait (past TOTK_SAVE_WAIT_MS: they go on, and this snapshot is dropped)
                static const DWORD delay = getenv("TOTK_SAVE_SNAP_DELAY_MS") ? (DWORD)atoi(getenv("TOTK_SAVE_SNAP_DELAY_MS")) : 0;
                if (delay) Sleep(delay);
                g_save_stage = "taking the snapshot";
                ok = snapshot_locked(g_work, copied.get());
                g_save_stage = "comparing the snapshot with the committed save";
                same = ok && snapshot_unchanged();
            }
            u64 ts = GetTickCount64();
            bool torn;
            {
                std::lock_guard<std::mutex> l(g_snap_mtx);
                torn = g_snap_torn;
                g_snap_torn = false;
                g_snap_pending = false;
            }
            g_snap_cv.notify_all();
            if (!locked) printf("[fs] save commit #%u: no snapshot without the commit lock; the next commit or start does it\n", n);
            else if (!ok) printf("[fs] save commit #%u: the snapshot failed (error %lu); the next commit or start does it\n", n, GetLastError());
            else if (torn) {   // a change went on during the snapshot: it may hold a mix of two saves
                if (!same) remove_tree(nw_dir());
                printf("[fs] save commit #%u: dropped, a change to the save could not wait for the snapshot; the next commit does it\n", n);
                ok = false;
            } else if (!same) printf("[fs] save commit #%u: snapshot taken in %llu ms (lock %llu ms), publishing\n", n,
                                     (unsigned long long)(ts - tl), (unsigned long long)(tl - t1));
            u64 ms[3] = {};
            g_move_retries = 0; g_move_max_ms = 0;
            if (ok && !same) {
                g_save_stage = "flushing and publishing";
                ok = finish_locked(*copied, ms);
            }
            if (locked) commits_unlock();
            g_save_stage = "idle";
            if (ok && same) pending(false);
            else if (ok) {
                pending(false);
                printf("[fs] save commit #%u published: %llu ms on the game thread, snapshot %llu ms + flush + rename + swap %llu ms in"
                       " the background (mutex %llu, flush %llu ms for %zu files, .ready %llu, swap + backup %llu; renames retried %u,"
                       " longest %u ms; %u changes to the save since the last commit; %u later ones waited %llu ms for the"
                       " snapshot)\n", n,
                       (unsigned long long)game, (unsigned long long)(ts - tl), (unsigned long long)(GetTickCount64() - ts),
                       (unsigned long long)(tl - t1), (unsigned long long)ms[0], copied->size(), (unsigned long long)ms[1],
                       (unsigned long long)ms[2], g_move_retries.load(), g_move_max_ms.load(), changes, g_snap_waits.load(),
                       (unsigned long long)g_snap_wait_ms.load());
            }
            { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = false; }
            g_pub_stuck = false;
            g_pub_cv.notify_all();
        }).detach();
        return;
    }
    if (!commits_lock("commit")) { commit_skipped(n, "waited for the commit lock"); return; }
    commit_covers_skipped();
    bool ok = snapshot_locked(g_work, copied.get());
    const bool same = ok && snapshot_unchanged();
    commits_unlock();
    if (same) { pending(false); return; }
    pending(true);   // until the background publish lands (a failed snapshot is retried at the next Commit / start)
    if (!ok) return;
    { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = true; }
    u64 snap = GetTickCount64() - t0;
    std::thread([copied, snap, n] {
        u64 t1 = GetTickCount64();
        g_save_stage = "waiting for the commit lock";
        if (!commits_lock("commit")) {   // (save.new stays incomplete: the next snapshot or start removes it)
            printf("[fs] save commit #%u: not published without the commit lock; the next commit or start does it\n", n);
            g_save_stage = "idle";
            { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = false; }
            g_pub_stuck = false;
            g_pub_cv.notify_all();
            return;
        }
        u64 tl = GetTickCount64(), ms[3] = {};
        g_move_retries = 0; g_move_max_ms = 0;
        g_save_stage = "flushing and publishing";
        bool ok = finish_locked(*copied, ms);
        commits_unlock();
        g_save_stage = "idle";
        if (ok) {
            pending(false);
            printf("[fs] save commit #%u published: snapshot %llu ms on the game thread, flush + rename + swap %llu ms in the background"
                   " (mutex %llu, flush %llu ms for %zu files, .ready %llu, swap + backup %llu; renames retried %u, longest %u ms)\n", n,
                   (unsigned long long)snap, (unsigned long long)(GetTickCount64() - t1), (unsigned long long)(tl - t1),
                   (unsigned long long)ms[0], copied->size(), (unsigned long long)ms[1], (unsigned long long)ms[2],
                   g_move_retries.load(), g_move_max_ms.load());
        }
        { std::lock_guard<std::mutex> p(g_pub_mtx); g_pub_busy = false; }
        g_pub_stuck = false;
        g_pub_cv.notify_all();
    }).detach();
}
#else
static bool in_save(const std::string&) { return false; }   // (no save commits off Windows: writes go in place)
struct SaveWrite { explicit SaveWrite(bool) {} };
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
// default since 2026-10-27, no NXRP copy needed. Since v0.1.8 it may name romfs-source.txt instead (rom_source.h): the same
// image read straight from the player's dump, each read decrypted in place (AES-CTR), so a player's install keeps no 17 GB
// romfs.bin.
#ifdef _WIN32
namespace {
struct PackEntry { u64 hash; u32 path_off, path_len; u64 data_off, size; };
static_assert(sizeof(PackEntry) == 32, "NXRP v1 entry");
struct RomPack {
    HANDLE h = INVALID_HANDLE_VALUE;
    const RomSource* src = nullptr;   // the image in the player's dump (romfs-source.txt), else h is the image itself
    std::vector<PackEntry> ents; std::string paths; std::vector<u32> order;
};
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
// The game dump stopped answering during play (its drive unplugged, a network share gone: the open handle keeps the
// file itself from being moved or deleted): one clear message, then the game ends with "DMP0" (the launcher shows no
// second dialog, tries no older package, and checks the dump at the next start). Never a hang or a garbled file.
[[noreturn]] static void dump_lost(const RomSource* src) {
    static std::atomic<bool> once{false};
    if (once.exchange(true))
        for (;;) Sleep(INFINITE);   // (another reader is showing the message)
    printf("[fatal] the game dump %s cannot be read any more (moved, deleted or its drive unplugged)\n", src->dump.c_str());
    fflush(stdout);
    EnumWindows([](HWND w, LPARAM) -> BOOL {   // (the game's own windows out of the way: a fullscreen one hides the box)
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(w)) ShowWindow(w, SW_HIDE);
        return TRUE;
    }, 0);
    const std::wstring d = widen(src->dump);
    const std::wstring msg =
        L"No se puede leer tu copia del juego (el dump):\n" + d +
        L"\n\nSe ha movido, borrado o se ha desconectado su unidad. El juego tiene que cerrarse. Vuelve a conectar la "
        L"unidad, o elige el dump otra vez al abrir el juego.\n\n"
        L"Your game dump can't be read any more:\n" + d +
        L"\n\nIt was moved, deleted or its drive was unplugged. The game has to close. Plug the drive back in, or pick "
        L"the dump again when you start the game.";
    MessageBoxW(nullptr, msg.c_str(), L"TotK", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    _exit((int)0x444D5030u);   // "DMP0" (launcher.c DUMP_LOST_CODE)
}
// image bytes at `off`: the file itself, or (src) the image in the player's dump: raw bytes, decrypted in place. A short
// read inside the image is tried once more after a moment, then ends the game with a message (dump_lost).
u64 rom_read(HANDLE h, const RomSource* src, u64 off, void* buf, u64 size) {
    if (!src) return pack_read(h, off, buf, size);
    if (off >= src->image_size) return 0;
    const u64 want = std::min<u64>(size, src->image_size - off);
    u64 n = pack_read(h, src->image_offset + off, buf, want);
    if (n < want) {
        Sleep(250);
        n += pack_read(h, src->image_offset + off + n, (u8*)buf + n, want - n);
        if (n < want) dump_lost(src);
    }
    rom_source_decrypt(*src, off, buf, n);
    return n;
}
u64 pack_read(const RomPack* pk, u64 off, void* buf, u64 size) { return rom_read(pk->h, pk->src, off, buf, size); }
RomPack* rompack() {
    static RomPack* pk = []() -> RomPack* {
        const char* e = getenv("TOTK_ROMFS_PACK");
        if (!e || !*e) return nullptr;
        RomSource* src = nullptr;   // romfs-source.txt: the image in the player's dump (never freed: the run's)
        std::string file = e, src_why;
        if (file.size() >= 16 && _stricmp(file.c_str() + file.size() - 16, "romfs-source.txt") == 0) {
            rom_source_on_secret = pc_crash_secret;      // (the key never reaches a crash dump: pc_crash.cpp)
            rom_source_on_busy = pc_crash_secret_busy;
            src = new RomSource;
            if (!rom_source_load(widen(file).c_str(), src, &src_why)) {
                printf("[fs] romfs source %s: %s; serving the folder\n", e, src_why.c_str());
                delete src;
                return nullptr;
            }
            file = src->dump;
        }
        HANDLE h = CreateFileW(widen(file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
        auto fail = [&](const char* why) -> RomPack* {
            printf("[fs] romfs pack %s: %s; serving the folder\n", e, why);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            delete src;
            return nullptr;
        };
        if (h == INVALID_HANDLE_VALUE) return fail(src ? "cannot open the game dump it reads (moved or deleted?)" : "cannot open");
        LARGE_INTEGER sz{};
        if (src && (!GetFileSizeEx(h, &sz) || (u64)sz.QuadPart != src->dump_size))
            return fail("the game dump it reads changed size since Setup read it");
        u8 hd[128];
        if (rom_read(h, src, 0, hd, sizeof hd) != sizeof hd) return fail("short header");
        u32 ver, n, pb; u64 io, po, oo, total;
        memcpy(&ver, hd + 8, 4); memcpy(&n, hd + 16, 4); memcpy(&pb, hd + 20, 4);
        memcpy(&io, hd + 24, 8); memcpy(&po, hd + 32, 8); memcpy(&oo, hd + 40, 8); memcpy(&total, hd + 56, 8);
        if (src || memcmp(hd, "NXRPACK1", 8) != 0) {
            // The extraction's own RomFS image (romfs.bin, romfs_image.h): the same index built from its tables, so the
            // pack needs no second 17 GB copy. The bytes are the ones the folder was unpacked from (every IVFC level
            // checked at extraction; tools/engine/romfs_image_verify.py compares all of them with the folder).
            u64 h0;
            memcpy(&h0, hd, 8);
            if (h0 != 0x50) return fail(src ? "the image in the game dump does not decrypt to a RomFS" : "neither an NXRP v1 pack nor a RomFS image");
            if (src) sz.QuadPart = (LONGLONG)src->image_size;
            else if (!GetFileSizeEx(h, &sz)) return fail("no file size");
            const u64 t0 = GetTickCount64();
            std::vector<RomImageFile> files;
            std::string paths;
            const char* why = "";
            if (!romfs_image_index([h, src](u64 off, void* buf, u64 n) { return rom_read(h, src, off, buf, n); }, (u64)sz.QuadPart,
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
            r->src = src;
            printf("[fs] romfs image %s: %zu files, %.2f GiB, indexed in %llu ms; the folder is the fallback\n", e,
                   r->ents.size(), bytes / 1073741824.0, (unsigned long long)(GetTickCount64() - t0));
            if (src)
                printf("[fs] romfs image read from the game dump %s (%s)\n", src->dump.c_str(),
                       !src->ctr ? "plain sections" : src->aesni ? "AES-CTR, AES-NI" : "AES-CTR, BCrypt");
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

// ---- the game's own RomFS (the pack, else the folder) under the replacements, for the mods' file count and table
// The game's RomFS has `rel` ("/a/b") as: 1 file, 0 directory, -1 neither
static int rom_base_type(const std::string& rel) {
#ifdef _WIN32
    if (RomPack* pk = rompack()) {
        const size_t a = rel.find_first_not_of('/');
        return pack_type(pk, a == std::string::npos ? std::string() : rel.substr(a));
    }
#endif
    std::string hp = kRomDir + rel;
    ci_fix(hp);
    std::error_code ec;
    auto st = fsys::status(widen(hp), ec);
    return ec || !fsys::exists(st) ? -1 : fsys::is_directory(st) ? 0 : 1;
}
// The first bytes of the game's own file `rel` (no replacement) and its size; false: no such file
static bool rom_base_head(const std::string& rel, u8* head, size_t n, size_t* got, u64* size) {
#ifdef _WIN32
    if (RomPack* pk = rompack()) {
        const PackEntry* e = pack_find(pk, rel.substr(rel.find_first_not_of('/')));
        if (!e) return false;
        *size = e->size;
        *got = (size_t)pack_read(pk, e->data_off, head, std::min<u64>(n, e->size));
        return true;
    }
#endif
    std::string hp = kRomDir + rel;
    ci_fix(hp);
    std::ifstream f(fsys::path(widen(hp)), std::ios::binary | std::ios::ate);
    if (!f) return false;
    *size = (u64)f.tellg();
    f.seekg(0);
    f.read((char*)head, (std::streamsize)std::min<u64>(n, *size));
    *got = (size_t)f.gcount();
    return true;
}
static u64 host_content_size(const std::string& path, const std::string& rel, u64 size) {   // a mod file's (rstb::content_size)
    if (rel.size() < 3 || rel.compare(rel.size() - 3, 3, ".zs")) return size;   // (only a .zs file is opened)
    std::ifstream f(fsys::path(widen(path)), std::ios::binary | std::ios::ate);
    if (!f) return size;
    u8 head[18] = {};
    f.seekg(0);
    f.read((char*)head, sizeof head);
    return rstb::content_size(rel, head, (size_t)f.gcount(), size);
}

// The files the player's mods serve, counted once (the first RomFS open with mods): what each mod replaces or adds,
// what the port's files or a higher mod serve instead, and its files for another game version (mods.h counts, the
// F10 Mods tab). Added files and the folders only mods have go into the directory listings (OpenDirectory,
// GetEntryType).
struct ModFile { std::string rel, path; int mod; bool added; u64 size; };
struct ModEnt { std::string name; int type; s64 size; };   // (type: 0 directory, 1 file, as nn::fs)
struct ModsIndex {
    std::vector<ModFile> served;
    std::vector<std::vector<std::string>> other_tables;            // per mod: its tables for another game version
    std::unordered_map<std::string, std::vector<ModEnt>> listing;  // lower-cased folder ("" the root) -> added entries
    std::unordered_set<std::string> dirs;                          // lower-cased folders only mods have
};
// The RomFS path ("/a/b", "" the root) of host path `hp` under the RomFS folder; false: elsewhere
static bool rom_rel(const std::string& hp, std::string* rel) {
    const size_t n = strlen(kRomDir);
    if (hp.compare(0, n, kRomDir) != 0 || (hp.size() > n && hp[n] != '/' && hp[n] != '\\')) return false;
    rel->assign(hp, n, std::string::npos);
    for (char& ch : *rel) if (ch == '\\') ch = '/';
    while (!rel->empty() && rel->back() == '/') rel->pop_back();
    if (!rel->empty() && (*rel)[0] != '/') rel->insert(0, "/");
    return true;
}
static std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
// this game's version in RomFS names ("100" from ResourceSizeTable.Product.100.rsizetable.zs)
static std::string rom_version() {
    std::string t = kGlyphRstb;
    const size_t a = t.find(".Product.") + 9;
    return t.substr(a, t.find('.', a) - a);
}
static const ModsIndex& mods_index() {
    static ModsIndex x;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto& um = user_mods();
        if (um.empty()) return;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string ver = rom_version(), rstb_rel = "/" + std::string(kGlyphRstb);
        x.other_tables.resize(um.size());
        for (int i = 0; i < (int)um.size(); i++) {
            int replaced = 0, added = 0, by_port = 0, by_mod = 0, other = 0;
            std::string by_mod_name;
            std::error_code ec;
            const fsys::path root = fsys::path(widen(um[i]));
            const std::u8string bu = root.generic_u8string();
            const std::string base(bu.begin(), bu.end());
            for (fsys::recursive_directory_iterator d(root, fsys::directory_options::skip_permission_denied, ec), e;
                 !ec && d != e; d.increment(ec)) {
                if (!d->is_regular_file(ec)) continue;
                const std::u8string g = d->path().generic_u8string();
                std::string p(g.begin(), g.end());
                if (p.size() <= base.size()) continue;
                const std::string rel = p.substr(base.size());
                // (another version's file: tagged .Product.<n>. with n not this build's AND not a file of this game: a
                //  1.4.3 RomFS has its own .140. / .142. / .110. / .101. files besides .143.)
                if (mods_scan::other_version(rel, ver.c_str()) && rom_base_type(rel) != 1) {
                    other++;
                    if (rel.rfind("/System/Resource/ResourceSizeTable.", 0) == 0) x.other_tables[i].push_back(p);
                    continue;
                }
                if (rel == rstb_rel) continue;   // (its table: merged, rstb_merged)
                std::string winner;
                int who = i;
                if (!rom_replacement(rel, true, &winner, &who, false) || who == i) {
                    const bool add = rom_base_type(rel) != 1;
                    x.served.push_back({rel, p, i, add, (u64)d->file_size(ec)});
                    (add ? added : replaced)++;
                    if (add) {   // into its folder's listing, and each folder only mods have into its parent's
                        std::string child = rel;
                        s64 size = (s64)d->file_size(ec);
                        int type = 1;
                        while (true) {
                            const size_t sl = child.rfind('/');
                            const std::string dir = child.substr(0, sl), name = child.substr(sl + 1);
                            auto& v = x.listing[lower(dir)];
                            bool have = false;
                            for (const auto& en : v) have |= mods_scan::ieq(en.name, name);
                            if (!have) v.push_back({name, type, type ? size : 0});
                            if (dir.empty() || rom_base_type(dir) == 0 || !x.dirs.insert(lower(dir)).second) break;
                            child = dir, type = 0, size = 0;
                        }
                    }
                } else if (who == kByPort) {
                    by_port++;
                } else {
                    if (!by_mod++) {
                        for (const auto& m : mods_list())
                            if (m.romfs == um[who]) by_mod_name = m.name;
                    }
                }
            }
            mods_set_counts(um[i], replaced, added, by_port, by_mod, other, by_mod_name);
            printf("[mods] %s: %d replaced, %d added, %d left to the port's files, %d to a higher mod, %d for another "
                   "game version\n", um[i].c_str(), replaced, added, by_port, by_mod, other);
        }
        printf("[mods] %zu mod file(s) served, %zu folder(s) added, counted in %.0f ms\n", x.served.size(), x.dirs.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    });
    return x;
}

// rstb_merged: each file a mod serves gets the entry its size needs in `out` (never lowered): a mod's table for another
// game version gives its own files' entries; a replaced file that `base` (the port's table, before the mods' own) lists
// grows by what the file grew (rstb::grow; one it does not list is sized by the game without the table, left alone);
// an added file is estimated (rstb::estimate). Returns the entries raised; *estimated / *other: of those, the
// estimates and the other-version tables' entries. After totk-pc-recomp runtime/romfs.c restbl_overlay (MIT,
// Copyright (c) 2026 Valiantsin Dzerakh).
static size_t rstb_mod_files(rstb::Table& out, const rstb::Table& base, size_t* estimated, size_t* other) {
    const ModsIndex& x = mods_index();
    size_t raised = 0;
    for (size_t i = 0; i < x.other_tables.size(); i++)
        for (const auto& p : x.other_tables[i]) {
            rstb::Table m;
            std::vector<u8> raw;
            if (!rstb::unzstd(read_host_file(p), &raw) || !rstb::parse(raw, &m)) {
                printf("[fs] resource size table %s: not a table this game reads, left out\n", p.c_str());
                continue;
            }
            for (const auto& f : x.served) {
                uint32_t v;
                const std::string k = rstb::key(f.rel);
                if (f.mod == (int)i && rstb::get(m, k, &v) && rstb::raise(out, k, v)) raised++, (*other)++;
            }
        }
    for (const auto& f : x.served) {
        const std::string k = rstb::key(f.rel);
        const bool mc = f.rel.size() >= 3 && !f.rel.compare(f.rel.size() - 3, 3, ".mc");
        uint32_t want = 0, entry;
        if (!f.added && !rstb::get(base, k, &entry)) continue;   // (sized by the game without the table)
        const u64 now = host_content_size(f.path, f.rel, f.size);   // (.mc: its size; the factor is in grow / estimate)
        if (!f.added) {
            u8 head[18] = {};
            size_t got = 0;
            u64 size = 0;
            if (!rom_base_head(f.rel, head, sizeof head, &got, &size)) continue;
            want = rstb::grow(k, entry, mc ? size : rstb::content_size(f.rel, head, got, size), now, mc);
        } else if ((want = rstb::estimate(k, mc ? now * 8 : now))) {   // (a mesh codec file unpacks to several times its size)
            (*estimated)++;
        }
        if (want && rstb::raise(out, k, want)) raised++;
    }
    return raised;
}

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
#if !NXR_GAME_TOTK
// CommitSaveData(mount): the SDK 11 name of Commit for a save mount (SDK 11.4)
HLE_AS(fs_CommitSaveData, "_ZN2nn2fs14CommitSaveDataEPKc") { hle_fs_Commit(c); }
#endif

// ---- open-ahead Windows scans a file the first time it is opened (Defender real-time protection: 4-14 ms a file on
// the dev PC, cached for about a minute), and the game's two loader threads open ~6.6 k files one after the other from
// Continue to the world: 16-18 s on a busy host, 3 s on a quiet one. Each session records the order in which the RomFS
// files were first opened (cache/fs_openorder.txt: host paths, this session's order first, then the older entries it
// did not reach); the next session follows it. When the game opens a recorded file, TOTK_FS_PREOPEN_THREADS host
// threads (default 8) open and close the next TOTK_FS_PREOPEN_AHEAD recorded files (default 512), so their scan is paid
// in parallel, ahead of the loaders. The boot, Continue and any fast travel or shrine of an earlier session are
// covered; a load never recorded is not. Pre-read (stream-hitch, 2026-11-02; TOTK_FS_PREREAD, default on, =0 off): with
// the RomFS served from a pack (the romfs.bin image, or since v0.1.8 the player's own dump, possibly on a slow or USB
// drive) there is no scan to pay, but the pack's single handle reads with FILE_FLAG_RANDOM_ACCESS (no system
// read-ahead) on the loader threads, one file after the other. The same record is kept and followed then; instead of
// opening, TOTK_FS_PREREAD_THREADS threads (default 2) read the next recorded files' raw bytes (no decryption), up to
// TOTK_FS_PREREAD_MB (default 128) ahead of the game, through their own handles at low I/O priority, so the system file
// cache holds them when the game asks (an AES-CTR decrypt costs ~0.1 ms a MB with AES-NI: test_stream_hitch.py).
// Nothing is kept in the process: the pages are the system cache's (standby memory), and the game's reads are
// unchanged.
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
    bool pack = false;                           // pre-read (above): the record's files read ahead from the pack
    std::vector<u64> off, cum;                   // (pack) each record entry's raw offset in the pack file; prefix sums of sizes
    u64 window = 128ull << 20, read_bytes = 0;   // (pack) bytes ahead of the game; bytes pre-read
};
const char* const kOpenOrder = "cache/fs_openorder.txt";
void preopen_write(Preopen* p) {
    std::vector<std::string> out;
    u64 opened, hits, late, bytes;
    {
        std::lock_guard<std::mutex> l(p->m);
        if (p->now.size() == p->saved) return;
        p->saved = p->now.size();
        out = p->now;
        for (auto& r : p->rec) if (!p->now_seen.count(r)) out.push_back(r);
        opened = p->opened; hits = p->hits; late = p->late; bytes = p->read_bytes;
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
    if (p->pack)
        printf("[fs] preread: %zu files recorded this session (%zu in %s); %llu pre-read (%.0f MB), %llu game opens followed "
               "the record, %llu of them before the pre-read got there\n", p->saved, out.size(), kOpenOrder,
               (unsigned long long)opened, bytes / 1048576.0, (unsigned long long)hits, (unsigned long long)late);
    else
        printf("[fs] preopen: %zu files recorded this session (%zu in %s); %llu pre-opened, %llu game opens followed the "
               "record, %llu of them before the pre-open got there\n", p->saved, out.size(), kOpenOrder,
               (unsigned long long)opened, (unsigned long long)hits, (unsigned long long)late);
}
// (pre-read) the record index past which `bytes` of file data from entry `from` on end; at least one entry past `from`
u32 preread_end(const Preopen* p, u32 from, u64 bytes) {
    const u32 e = (u32)(std::upper_bound(p->cum.begin() + from + 1, p->cum.end(), p->cum[from] + bytes) - p->cum.begin() - 1);
    return std::min<u32>(std::max<u32>(e, from + 1), (u32)p->rec.size());
}
Preopen* preopen() {
    static Preopen* p = [] () -> Preopen* {
        RomPack* pk = rompack();   // (a pack: the pre-read instead of the pre-open, above)
        if (pk ? !fs_flag("TOTK_FS_PREREAD") : !fs_flag("TOTK_FS_PREOPEN")) return nullptr;
        Preopen* p = new Preopen;
        p->pack = pk != nullptr;
        if (const char* a = getenv("TOTK_FS_PREOPEN_AHEAD"); a && atoi(a) > 0) p->ahead = (u32)atoi(a);
        if (const char* a = getenv("TOTK_FS_PREREAD_MB"); a && atoi(a) > 0) p->window = (u64)atoi(a) << 20;
        int nt = p->pack ? 2 : 8;
        if (const char* t = getenv(p->pack ? "TOTK_FS_PREREAD_THREADS" : "TOTK_FS_PREOPEN_THREADS"); t && atoi(t) > 0)
            nt = std::min(atoi(t), 32);
        if (FILE* f = fopen(kOpenOrder, "rb")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                size_t n = strcspn(line, "\r\n"); line[n] = 0;
                if (n && p->at.emplace(line, (u32)p->rec.size()).second) p->rec.emplace_back(line);
            }
            fclose(f);
        }
        p->target = std::min<u32>(p->ahead, (u32)p->rec.size());   // the boot's first files right away
        if (p->pack) {   // each entry's bytes in the pack file (0 bytes: not in it, an overlay or mod file)
            p->off.resize(p->rec.size()); p->cum.assign(p->rec.size() + 1, 0);
            const u64 base = pk->src ? pk->src->image_offset : 0;
            for (size_t i = 0; i < p->rec.size(); i++) {
                std::string rel;
                const PackEntry* e = pack_rel(p->rec[i], &rel) ? pack_find(pk, rel) : nullptr;
                p->off[i] = e ? base + e->data_off : 0;
                p->cum[i + 1] = p->cum[i] + (e ? e->size : 0);
            }
            p->target = std::min<u32>(p->target, preread_end(p, 0, p->window));
            printf("[fs] preread: following %zu recorded files (%s, %.0f MB) on %d threads, %llu MB / %u files ahead\n",
                   p->rec.size(), kOpenOrder, p->cum.back() / 1048576.0, nt, (unsigned long long)(p->window >> 20), p->ahead);
        } else
            printf("[fs] preopen: following %zu recorded files (%s) on %d threads, %u ahead\n", p->rec.size(), kOpenOrder,
                   nt, p->ahead);
        for (int i = 0; i < nt; i++)
            std::thread([p, pk] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                cpu_place_thread(CPU_HELPER);   // (opens ahead of the game: the E-cores / the other CCD, cpu_place.h)
                if (p->pack) {   // pre-read: its own synchronous handle (one per thread: a synchronous handle serializes)
                    HANDLE wh = ReOpenFile(pk->h, GENERIC_READ, FILE_SHARE_READ, 0);
                    if (wh == INVALID_HANDLE_VALUE) { printf("[fs] preread: ReOpenFile failed (%lu)\n", GetLastError()); return; }
                    FILE_IO_PRIORITY_HINT_INFO hint{IoPriorityHintLow};   // (never in the way of the game's own reads)
                    SetFileInformationByHandle(wh, FileIoPriorityHintInfo, &hint, sizeof hint);
                    // (its cache pages are repurposed first: on an 8 GB PC the window yields to the game's other standby
                    // pages)
                    MEMORY_PRIORITY_INFORMATION mp{MEMORY_PRIORITY_LOW};
                    SetThreadInformation(GetCurrentThread(), ThreadMemoryPriority, &mp, sizeof mp);
                    std::vector<u8> buf(1u << 20);
                    for (;;) {
                        u32 i;
                        {
                            std::unique_lock<std::mutex> l(p->m);
                            p->cv.wait(l, [p] { return p->next < p->target; });
                            i = p->next++;
                        }
                        const u64 size = p->cum[i + 1] - p->cum[i];
                        u64 done = 0;
                        while (done < size) {
                            OVERLAPPED ov{}; const u64 o = p->off[i] + done; ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
                            DWORD n = 0, want = (DWORD)std::min<u64>(size - done, buf.size());
                            if (!ReadFile(wh, buf.data(), want, &n, &ov) || n == 0) break;
                            done += n;
                        }
                        if (size) {
                            std::lock_guard<std::mutex> l(p->m);
                            p->opened++; p->read_bytes += done;
                        }
                    }
                }
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
    u32 t = std::min<u32>(i + 1 + p->ahead, (u32)p->rec.size());
    if (p->pack) t = std::min<u32>(t, preread_end(p, i + 1, p->window));   // (TOTK_FS_PREREAD_MB of file bytes past it)
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
            preopen_note(hp);   // (the pre-read's record and window)
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
// Reads on the thread that presents the game's frames (stream-hitch): the hitch line's "game file reads" sums every
// thread's, and the two loader threads' reads cost no frame unless the game waits for them; these do. nvn/objects.c
// nvnQueuePresentTexture marks the thread. The longest is kept until exec.c's frame line takes it.
static thread_local bool t_game_thread;
static std::atomic<u64> g_rd_game_bytes{0}, g_rd_game_ns{0}, g_rd_game_calls{0}, g_rd_game_max_ns{0};
extern "C" void hle_fs_mark_game_thread(void) { t_game_thread = true; }
extern "C" void hle_fs_game_reads(unsigned long long out[4]) {   // bytes, ns, reads (cumulative); the longest since the last call
    out[0] = g_rd_game_bytes.load(std::memory_order_relaxed); out[1] = g_rd_game_ns.load(std::memory_order_relaxed);
    out[2] = g_rd_game_calls.load(std::memory_order_relaxed); out[3] = g_rd_game_max_ns.exchange(0, std::memory_order_relaxed);
}
// load-progress counters: a line every 64 MiB read (is the loading screen streaming or stalled?)
static void count_read(u64 n, u64 ns) {
    static const u64 t0 = GetTickCount64();
    if (t_game_thread) {
        g_rd_game_bytes += n; g_rd_game_ns += ns; g_rd_game_calls++;
        u64 m = g_rd_game_max_ns.load(std::memory_order_relaxed);
        while (ns > m && !g_rd_game_max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    }
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
        done = n ? pack_read(rompack(), f->pk_off + (u64)off, buf, n) : 0;
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
    const u64 ns = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count();
    count_read(done, ns);
    if (ns > 30000000ull) {   // (stream-hitch: which read, from where, on which thread; the first 100, then every 100th)
        static std::atomic<u32> slow{0};
        const u32 k = ++slow;
        if (k <= 100 || k % 100 == 0) {
#ifdef _WIN32
            const RomPack* pk = f->packed ? rompack() : nullptr;
            const char* from = !pk ? "folder" : pk->src && pk->src->ctr ? "dump, decrypted" : pk->src ? "dump" : "romfs image";
#else
            const char* from = "folder";
#endif
            printf("[fs] slow read %u: %s, %llu KB at +%llu in %.1f ms (%s) on %s\n", k, f->path.c_str(),
                   (unsigned long long)(done >> 10), (unsigned long long)off, ns / 1e6, from,
                   t_game_thread ? "the game's presenting thread" : "another game thread (a loader?)");
        }
    }
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
    SaveWrite sw(in_save(f->path));
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
    SaveWrite sw(in_save(f->path));
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
    if (ec || !fsys::exists(st)) {
        std::string rel;   // a folder only the player's mods have
        if (!user_mods().empty() && rom_rel(hp, &rel) && mods_index().dirs.count(lower(rel))) { *(u32*)c->x[0] = 0; c->x[0] = 0; return; }
        c->x[0] = kPathNotFound; return;
    }
    *(u32*)c->x[0] = fsys::is_directory(st) ? 0 : 1; c->x[0] = 0;
}
HLE_AS(fs_CreateFile, "_ZN2nn2fs10CreateFileEPKcl") {
    std::string hp; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    SaveWrite sw(in_save(hp));
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
    SaveWrite sw(in_save(hp));
    bool removed = false;
    retry_locked(ec, [&] { removed = fsys::remove(widen(hp), ec); });
    c->x[0] = removed ? 0 : ec ? fs_err(ec, "DeleteFile") : kPathNotFound;
}
HLE_AS(fs_RenameFile, "_ZN2nn2fs10RenameFileEPKcS2_") {
    std::string a, b; std::error_code ec;
    if (!host_path((const char*)c->x[0], &a) || !host_path((const char*)c->x[1], &b)) { c->x[0] = kPathNotFound; return; }
    SaveWrite sw(in_save(a) || in_save(b));
    retry_locked(ec, [&] { fsys::rename(widen(a), widen(b), ec); }); c->x[0] = fs_err(ec, "RenameFile");
}
HLE_AS(fs_CreateDirectory, "_ZN2nn2fs15CreateDirectoryEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    SaveWrite sw(in_save(hp));
    c->x[0] = fsys::create_directory(widen(hp), ec) ? 0 : (ec ? fs_err(ec, "CreateDirectory") : kPathExists);
}
HLE_AS(fs_DeleteDirectoryRecursively, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    SaveWrite sw(in_save(hp));
    if (hp.rfind(kRomDir, 0) == 0) { c->x[0] = kUnsupported; return; }      // never touch the extracted romfs
    retry_locked(ec, [&] { fsys::remove_all(widen(hp), ec); }); c->x[0] = fs_err(ec, "DeleteDirectoryRecursively");
}

#if !NXR_GAME_TOTK
// CleanDirectoryRecursively(path): the directory's contents go, the directory stays (SDK 11.4)
HLE_AS(fs_CleanDirectoryRecursively, "_ZN2nn2fs25CleanDirectoryRecursivelyEPKc") {
    std::string hp; std::error_code ec; if (!host_path((const char*)c->x[0], &hp)) { c->x[0] = kPathNotFound; return; }
    SaveWrite sw(in_save(hp));
    if (hp.rfind(kRomDir, 0) == 0) { c->x[0] = kUnsupported; return; }      // never touch the extracted romfs
    for (const auto& e : fsys::directory_iterator(widen(hp), ec)) {
        std::error_code e2;
        retry_locked(e2, [&] { fsys::remove_all(e.path(), e2); });
        if (e2) { ec = e2; break; }
    }
    c->x[0] = fs_err(ec, "CleanDirectoryRecursively");
}

#endif
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
    if (std::string mrel; ci_fix(hp), !fsys::is_directory(widen(hp), ec)) {
        if (user_mods().empty() || !rom_rel(hp, &mrel) || !mods_index().dirs.count(lower(mrel))) { c->x[0] = kPathNotFound; return; }
    } else {   // (else a folder only the player's mods have: their entries, below)
    for (auto& e : fsys::directory_iterator(widen(hp), ec)) {
        bool dir = e.is_directory(ec);
        if ((dir && !(mode & 1)) || (!dir && !(mode & 2))) continue;
        std::u8string nm = e.path().filename().u8string();
        ents.push_back({ std::string(nm.begin(), nm.end()), { dir ? 0 : 1, dir ? 0 : (s64)e.file_size(ec) } });
    }
    }
    if (std::string mrel; !user_mods().empty() && rom_rel(hp, &mrel)) {   // the files and folders the player's mods add
        const auto& ls = mods_index().listing;
        if (auto it = ls.find(lower(mrel)); it != ls.end())
            for (const ModEnt& m : it->second) {
                if ((m.type == 0 && !(mode & 1)) || (m.type == 1 && !(mode & 2))) continue;
                bool have = false;
                for (const auto& e : ents) have |= mods_scan::ieq(e.first, m.name);
                if (!have) ents.push_back({ m.name, { m.type, m.size } });
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

// One whole file of the game's RomFS for host code (pc_ui_font.cpp: the game's UI font for the fps readout): the RomFS
// pack when one serves the RomFS (TOTK_ROMFS_PACK), else the RomFS folder; never an overlay or a mod (the game's own
// file). rel: RomFS-relative, '/' separators. False (out empty) when the file is not there.
bool hle_fs_read_romfs(const char* rel, std::vector<u8>* out) {
    out->clear();
#ifdef _WIN32
    if (RomPack* pk = rompack()) {
        if (const PackEntry* e = pack_find(pk, rel)) {
            out->resize((size_t)e->size);
            if (pack_read(pk, e->data_off, out->data(), e->size) == e->size) return true;
            out->clear();
            return false;
        }
    }
#endif
    *out = read_host_file(std::string(kRomDir) + "/" + rel);
    return !out->empty();
}
// hle_movie.cpp: a RomFS file the pack serves (hp under the RomFS folder and in the pack) as the pack's file handle,
// offset and size, for a read-only mapping of that slice (a player's install keeps no unpacked folder). False: hp itself
// is the file (an overlay's or a mod's, or the folder when no pack serves the RomFS), or the pack is the image in the
// encrypted game dump (hle_fs_pack_bytes). The handle stays the pack's.
bool hle_fs_pack_slice(const std::string& hp, void** h, u64* off, u64* size) {
#ifdef _WIN32
    std::string rel;
    if (RomPack* pk = rompack(); pk && !(pk->src && pk->src->ctr) && pack_rel(hp, &rel))
        if (const PackEntry* e = pack_find(pk, rel)) {
            *h = pk->h; *off = (pk->src ? pk->src->image_offset : 0) + e->data_off; *size = e->size;
            return true;
        }
#endif
    (void)hp; (void)h; (void)off; (void)size;
    return false;
}
// hle_movie.cpp: a RomFS file the pack serves from the encrypted game dump (no mapping can show it decrypted): its
// bytes, read whole and decrypted. False: not such a file (hle_fs_pack_slice or hp itself).
bool hle_fs_pack_bytes(const std::string& hp, std::vector<u8>* out) {
#ifdef _WIN32
    std::string rel;
    if (RomPack* pk = rompack(); pk && pk->src && pk->src->ctr && pack_rel(hp, &rel))
        if (const PackEntry* e = pack_find(pk, rel)) {
            out->resize((size_t)e->size);
            if (pack_read(pk, e->data_off, out->data(), e->size) == e->size) return true;
            out->clear();
        }
#endif
    (void)hp; (void)out;
    return false;
}
