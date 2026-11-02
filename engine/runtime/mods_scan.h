// mods_scan.h: the pure parts of the player's mods folder (mods.cpp): where a mod folder keeps its RomFS / ExeFS /
// cheats, the list order from the saved keys, and which RomFS files are made for another game version. No game data,
// no settings, no globals; tools/engine/test_mods.py checks them on folders it makes.
// The layouts are the ones emulators and the Switch take (and TKMM's merged output, a romfs / romfslite folder):
// <mod>/romfs, <mod>/romfslite, <mod>/atmosphere/contents/<title>/romfs, <mod>/contents/<title>/romfs,
// <mod>/<title>/romfs (the same for exefs and cheats); else the folder itself when it is a RomFS tree (a known top
// folder like Actor/ or Pack/); else the one folder it holds when that is all it holds (a zip unpacked into a folder
// of its own name). Layout list after totk-pc-recomp runtime/mods.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Beyond it: a whole emulator mods folder copied in (collection: yuzu load/<title>/<mod>, Ryujinx
// mods/contents/<title>/<mod>) is one mod per folder, and cheat files right in a mod folder are its cheats.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace mods_scan {

namespace fs = std::filesystem;

enum Kind { ROMFS = 1, EXEFS = 2, SUBSDK = 4, CHEATS = 8 };

struct Found {
    fs::path romfs, exefs, cheats;   // empty: none
    std::string layout;              // how the RomFS was found ("romfs", "atmosphere/contents/<title>/romfs", "folder", ...)
    std::string other_title;         // contents/<id>/ for a title that is not this game's ("" none)
    int kind = 0;                    // Kind bits
};

inline std::string u8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
inline fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

inline bool ieq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
    });
}
inline bool ends_ci(const std::string& s, const char* tail) {
    std::string t = tail;
    return s.size() >= t.size() && ieq(s.substr(s.size() - t.size()), t);
}

inline bool is_dir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }

// Does the folder hold a file anywhere below it?
inline bool has_file(const fs::path& d) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), e; !ec && it != e;
         it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

// The child of `d` named `name` in any case ("" path: none)
inline fs::path child_ci(const fs::path& d, const char* name) {
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), e; !ec && it != e; it.increment(ec))
        if (ieq(u8(it->path().filename()), name)) return it->path();
    return {};
}

// The top folders of this game's RomFS a bare tree is recognised by
inline bool romfs_top(const std::string& n) {
    static const char* const tops[] = {"Actor", "AI", "AnimationEvent", "AS", "Bake", "Banc", "Cave", "Effect", "Event",
                                       "Font", "Game", "GameParameter", "LevelSensor", "Lib", "Logic", "Mals", "Model",
                                       "Movie", "Pack", "Phive", "Preset", "RSDB", "Sequence", "Shader", "Sound",
                                       "System", "TexToGo", "UI"};
    for (const char* t : tops)
        if (ieq(n, t)) return true;
    return false;
}

// The `part` folder (romfs, exefs, cheats) of a mod in the places emulators and Atmosphere keep it; empty: none.
// `layout` gets the place's name. A contents/<id>/ for another title is reported in *other.
inline fs::path find_part(const fs::path& mod, const char* part, const std::string& title, std::string* layout,
                          std::string* other) {
    if (fs::path p = child_ci(mod, part); !p.empty() && is_dir(p)) { *layout = part; return p; }
    for (const char* pre : {"atmosphere/contents", "contents", ""}) {
        fs::path base = mod;
        bool ok = true;
        for (std::string s = pre; ok && !s.empty();) {
            size_t k = s.find('/');
            base = child_ci(base, s.substr(0, k).c_str());
            ok = !base.empty();
            s = k == std::string::npos ? "" : s.substr(k + 1);
        }
        if (!ok || !is_dir(base)) continue;
        if (fs::path t = child_ci(base, title.c_str()); !t.empty()) {
            if (fs::path p = child_ci(t, part); !p.empty() && is_dir(p)) {
                *layout = std::string(*pre ? pre : "") + (*pre ? "/" : "") + title + "/" + part;
                return p;
            }
            continue;
        }
        if (*pre) {   // contents/<another 16-digit id>/: a mod for another game
            std::error_code ec;
            for (fs::directory_iterator it(base, ec), e; !ec && it != e; it.increment(ec)) {
                std::string n = u8(it->path().filename());
                if (n.size() == 16 && std::all_of(n.begin(), n.end(), [](char c) { return std::isxdigit((unsigned char)c); }))
                    { *other = n; break; }
            }
        }
    }
    return {};
}

// An Atmosphere cheat file's name: main's build id (its first 16 hex digits, or more) + ".txt"
inline bool cheat_name(const std::string& n) {
    if (!ends_ci(n, ".txt") || n.size() < 16 + 4 || n.size() > 64 + 4) return false;
    return std::all_of(n.begin(), n.end() - 4, [](char c) { return std::isxdigit((unsigned char)c); });
}

inline bool holds_cheat_file(const fs::path& d) {
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), e; !ec && it != e; it.increment(ec))
        if (it->is_regular_file(ec) && cheat_name(u8(it->path().filename()))) return true;
    return false;
}

// A file an ExeFS patch is in (.ips / IPS32, named by main's build id; .pchtxt, which names it inside)
inline bool patch_name(const std::string& n) { return ends_ci(n, ".ips") || ends_ci(n, ".pchtxt"); }

inline bool holds_patch_file(const fs::path& d) {
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), e; !ec && it != e; it.increment(ec))
        if (it->is_regular_file(ec) && patch_name(u8(it->path().filename()))) return true;
    return false;
}

// The folders a mod keeps ExeFS patch files in, in order: its exefs folder (scan_mod's Found::exefs), then Atmosphere's
// exefs_patches/<name>/ (under atmosphere/, bare when the SD card's atmosphere folder is the mod, or the mod folder
// itself), then the folder itself when the files are right in it. The folder and, when it holds one folder only, that
// one are looked at (a zip unpacked into a folder of its own).
inline std::vector<fs::path> patch_dirs(const fs::path& mod, const fs::path& exefs) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        if (holds_patch_file(d) && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };
    if (!exefs.empty()) add(exefs);
    std::vector<fs::path> where = {mod};
    {
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(mod, ec), e; !ec && it != e; it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(it->path());
        if (dirs.size() == 1) where.push_back(dirs[0]);
    }
    for (const fs::path& w : where) {
        std::vector<fs::path> eps;
        if (ieq(u8(w.filename()), "exefs_patches")) eps.push_back(w);
        if (fs::path a = child_ci(w, "atmosphere"); !a.empty())
            if (fs::path p = child_ci(a, "exefs_patches"); !p.empty()) eps.push_back(p);
        if (fs::path p = child_ci(w, "exefs_patches"); !p.empty()) eps.push_back(p);
        for (const fs::path& ep : eps) {
            add(ep);
            std::vector<fs::path> subs;
            std::error_code ec;
            for (fs::directory_iterator it(ep, ec), e; !ec && it != e; it.increment(ec))
                if (it->is_directory(ec)) subs.push_back(it->path());
            std::sort(subs.begin(), subs.end());
            for (const fs::path& s : subs) add(s);
        }
    }
    for (const fs::path& w : where) add(w);
    return out;
}

inline Found scan_mod(const fs::path& mod, const std::string& title);

// A copied emulator collection: the folder is, or holds under load/, contents/, atmosphere/contents/ or mods/contents/
// (one lone wrapper folder looked through), <title>/ with one folder per mod (yuzu's load/<title>/<mod>, Ryujinx's
// mods/contents/<title>/<mod>). Returns those mod folders ('_' / '.' names and folders with nothing for the game left
// out); none: not a collection. *home: that <title>/ folder when it has a romfs / exefs / cheats of its own beside them
// (the collection is a mod too, scanned there), else empty.
inline std::vector<fs::path> collection(const fs::path& dir, const std::string& title, fs::path* home) {
    home->clear();
    bool own = false;
    std::vector<fs::path> where = {dir};
    {
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(dir, ec), e; !ec && it != e; it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(it->path());
        if (dirs.size() == 1) where.push_back(dirs[0]);
    }
    fs::path t;
    for (const fs::path& w : where) {
        if (ieq(u8(w.filename()), title)) { t = w; break; }
        for (const char* pre : {"", "load", "contents", "atmosphere/contents", "mods/contents"}) {
            fs::path base = w;
            for (std::string s = pre; !base.empty() && !s.empty();) {
                size_t k = s.find('/');
                base = child_ci(base, s.substr(0, k).c_str());
                s = k == std::string::npos ? "" : s.substr(k + 1);
            }
            if (!base.empty()) t = child_ci(base, title.c_str());
            if (!t.empty() && is_dir(t)) break;
            t.clear();
        }
        if (!t.empty()) break;
    }
    std::vector<fs::path> out;
    if (t.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(t, ec), e; !ec && it != e; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string n = u8(it->path().filename());
        if (ieq(n, "romfs") || ieq(n, "romfslite") || ieq(n, "exefs") || ieq(n, "cheats")) { own = true; continue; }
        if (n.empty() || n[0] == '_' || n[0] == '.') continue;
        const Found f = scan_mod(it->path(), title);
        if (f.kind || !f.other_title.empty()) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    if (!out.empty() && own) *home = t;
    return out;
}

// What a mod folder holds and where (one level of a lone wrapper folder is looked through). Cheat files right in the
// folder (no cheats/ around them) are its cheats.
inline Found scan_mod(const fs::path& mod, const std::string& title) {
    Found f;
    fs::path root = mod;
    for (int depth = 0; depth < 2; depth++) {
        std::string lay, other;
        f.romfs = find_part(root, "romfs", title, &lay, &other);
        if (f.romfs.empty()) f.romfs = find_part(root, "romfslite", title, &lay, &other);   // TKMM's lite output
        f.layout = lay;
        std::string l2;
        f.exefs = find_part(root, "exefs", title, &l2, &other);
        f.cheats = find_part(root, "cheats", title, &l2, &other);
        if (f.other_title.empty()) f.other_title = other;
        if (!f.romfs.empty() || !f.exefs.empty() || !f.cheats.empty()) break;
        // the folder itself is a RomFS tree?
        std::vector<fs::path> dirs;
        bool files = false, top = false;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), e; !ec && it != e; it.increment(ec)) {
            if (it->is_directory(ec)) {
                dirs.push_back(it->path());
                top |= romfs_top(u8(it->path().filename()));
            } else if (!ends_ci(u8(it->path().filename()), ".txt") && !ends_ci(u8(it->path().filename()), ".md") &&
                       !ends_ci(u8(it->path().filename()), ".png") && !ends_ci(u8(it->path().filename()), ".jpg") &&
                       !ends_ci(u8(it->path().filename()), ".json") && !ends_ci(u8(it->path().filename()), ".ini")) {
                files = true;   // (a readme, a picture or a manifest beside the content does not count)
            }
        }
        if (top) { f.romfs = root; f.layout = depth ? "folder/" + u8(root.filename()) : "folder"; break; }
        if (dirs.size() != 1 || files) break;
        root = dirs[0];   // a zip unpacked into a folder of its own: look inside once
    }
    if (f.cheats.empty())
        for (const fs::path& d : {mod, root})
            if (holds_cheat_file(d)) { f.cheats = d; break; }
    if (!f.romfs.empty() && has_file(f.romfs)) f.kind |= ROMFS;
    else f.romfs.clear();
    std::error_code ec;
    if (!f.exefs.empty())
        for (fs::directory_iterator it(f.exefs, ec), e; !ec && it != e; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string n = u8(it->path().filename());
            if ((n.size() >= 6 && ieq(n.substr(0, 6), "subsdk")) || ieq(n, "main")) f.kind |= SUBSDK;
        }
    if (!patch_dirs(mod, f.exefs).empty()) f.kind |= EXEFS;
    if (!f.cheats.empty())
        for (fs::directory_iterator it(f.cheats, ec), e; !ec && it != e; it.increment(ec))
            if (it->is_regular_file(ec) && ends_ci(u8(it->path().filename()), ".txt")) f.kind |= CHEATS;
    return f;
}

// '|'-separated names
inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    for (size_t a = 0; a < s.size();) {
        size_t b = s.find('|', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}
inline std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& n : v) s += (s.empty() ? "" : "|") + n;
    return s;
}

// The names in list order: the saved order's names first (those still there), then the new ones by name
inline std::vector<std::string> order(std::vector<std::string> names, const std::string& saved) {
    std::sort(names.begin(), names.end());
    std::vector<std::string> out;
    for (const auto& n : split(saved))
        if (std::find(names.begin(), names.end(), n) != names.end() && std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    for (const auto& n : names)
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    return out;
}

// A RomFS file made for another version of the game: its name has ".Product.<n>." with n not this game's
// ("/Mals/USen.Product.121.sarc.zs" in a mod for 1.2.1; the 1.0.0 game asks for .Product.100.). hle_fs also requires
// the game itself not to have that file: a later version's RomFS keeps older tags on some files (1.4.3: .140., .110.)
inline bool other_version(const std::string& rel, const char* ver) {
    for (size_t at = 0; (at = rel.find(".Product.", at)) != std::string::npos; at += 9) {
        size_t a = at + 9, b = a;
        while (b < rel.size() && std::isdigit((unsigned char)rel[b])) b++;
        if (b > a && b < rel.size() && rel[b] == '.') return rel.compare(a, b - a, ver) != 0;
    }
    return false;
}

}  // namespace mods_scan
