// mods_exefs.cpp: the mods' ExeFS patches (.ips, IPS32, .pchtxt; their exefs folder and atmosphere/exefs_patches/*) and
// Atmosphere cheats (cheats/<build id>.txt) in the run, over exefs_scan.h and mods.h (the scan calls mods_exefs_hook per
// mod; mods_exefs_start runs once main's image is in).
//
// Lifted code cannot be byte-patched at run time. A file's writes into .rodata / .data are written into main's memory at
// start (gen.py reads those at run time). Its writes into code take a build of the game with them (a patched key:
// tools/engine/mods_exefs.py, about 3 minutes warm): the exe says which files it holds in exefs-set.txt next to it
// (`set <hash>`, then `file <id> <content>` / `cheat <id> <content>`, FNV-1a 32 of "<mod>/<path>" and of the bytes).
// When the mods' code writes differ from the exe's, mods_exefs_start asks TOTK_EXEFS_BUILD (the build command the
// player's install sets) for that build and exits; without it the code patches wait, unused, and the row says why.
// A file is all or nothing: its data writes wait with its code writes.
//
// Cheats: the data cheats run in hle_cheat_vm.inc (nxr_cheat_program), switched in the F10 Cheats tab (saved as
// cheats.on, ids '|' separated), live. A cheat made only of stores into main's code joins the code patches instead, on
// or off: the build holds a switch site for it (gen_ext.py: its words or the game's, by nxr_code_switch), so once the exe
// holds it, its switch works at once too (as the rival's rt_patch_switch).
//
// Rules after totk-pc-recomp (runtime/mods.c, runtime/cheats.c, tools/exefs_patches.py; MIT License,
// Copyright (c) 2026 Valiantsin Dzerakh).
#include "mods_exefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "exefs_scan.h"
#include "mods.h"
#include "mods_scan.h"
#include "pc_settings.h"
#include "pc_text.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" void nxr_cheat_program(const uint32_t *w, size_t n);   // hle_cheat_vm.inc

namespace {
namespace fs = std::filesystem;

constexpr uint64_t MAIN_BASE = 0x7100000000ull;

struct Mod {
    std::string name;
    bool on = false;
    int first = 0, files = 0;   // its range in g_files
};
struct CheatRow {
    std::string mod, name, file;
    exefs::Cheat c;
    std::string id;   // FNV-1a 32 of "<mod>/<name>"
    std::string key;  // "c" + FNV-1a 32 of "<name>\n<words>": the same cheat in a renamed mod folder (cheats.on keeps both)
    bool on = false;
    int slot = -1;    // a code cheat's switch (nxr_code_switch), -1 none
};

// The code cheats' switches: every code cheat of a mod on is built in (on or off) as a switch site (gen_ext.py), its
// words or the game's by this flag, so the F10 switch works at once once the exe holds it. Slots in scan order: the
// exe's slots are this scan's only when its set is this scan's (g_live).
constexpr int MAX_SWITCHES = 4096;
std::atomic<uint8_t> g_switch[MAX_SWITCHES];
bool g_live = false;                  // the exe was built with this scan's set: the switches are the cheats'

std::mutex g_mu;
bool g_image = false;                 // main is in memory (mods_exefs_start); the hook checks nothing before
exefs::Image g_im;                    // main's layout (img/main.json), at MAIN_BASE
bool g_early = false;                 // the hook ran before that (a scan from elsewhere first: said in the log)
std::vector<exefs::Site> g_port;      // the port's own patch sites (TOTK_IMG/patches.json)
std::vector<Mod> g_mods;              // this scan's mods, in order
std::vector<exefs::File> g_files;     // their ExeFS files for this build (and their code cheats), in order
int g_slots = 0;                      // code cheats in g_files (their switch slots 0..)
std::vector<std::string> g_file_ids;  // per g_files: "file <id> <content>" / "cheat <id> <content>"
std::vector<CheatRow> g_cheats;
std::set<std::string> g_cheats_on;    // cheats.on
// what the exe was built with (exefs-set.txt)
std::string g_built;                  // its set hash ("" none)
std::set<std::string> g_built_ids;
std::set<std::string> g_built_content;   // the same without the mods' names (exefs::content_id)
fs::path g_exe_dir;
bool g_can_build = false;             // TOTK_EXEFS_BUILD is set
std::string g_failed;                 // the set whose build failed last ("" none)

std::string fnv32(const void *p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 0x01000193u; }
    char b[9];
    snprintf(b, sizeof b, "%08x", h);
    return b;
}
std::string fnv32(const std::string &s) { return fnv32(s.data(), s.size()); }

std::string read_file(const fs::path &p, size_t cap = 64u << 20) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > cap) s.clear();
    return s;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// exefs_scan.h's reasons, which fmt translates (marked here for tools/engine/check_texts.py: it reads no headers)
[[maybe_unused]] const char *const kScanTexts[] = {
    PC_T("It writes the NSO header."),
    PC_T("It writes past main's image (bss)."),
    PC_T("It changes code the port already patches (%s)."),
    PC_T("It overlaps %s, higher in the list."),
    PC_T("It is cut short at word %s."),
    PC_T("It has an opcode the cheat engine does not know (%s)."),
    PC_T("It uses the alias or ASLR memory region, which the port does not have."),
    PC_T("It changes the game's code only under a condition (a button, a value), which the port cannot do yet."),
};

std::string fmt(const char *en, const std::string &a = "") {
    char b[512];
    snprintf(b, sizeof b, pc_tr(en), a.c_str());
    return b;
}
std::string fmt_n(const char *en, int n) {
    char b[256];
    snprintf(b, sizeof b, pc_tr(en), n);
    return b;
}

const exefs::Image &image() { return g_im; }

// the exe's size and last write time ("exe <size> <ns since 1970>"): exefs-set.txt counts only for the exe it was
// written for, so a plain exe put in its place (a Setup repair or update) means a rebuild, never a half patch
std::string exe_stamp() {
#ifdef _WIN32
    wchar_t b[32768];
    DWORD n = GetModuleFileNameW(nullptr, b, 32768);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!n || !GetFileAttributesExW(b, GetFileExInfoStandard, &a)) return "";
    const uint64_t size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    const uint64_t ft = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t ns = (ft - 116444736000000000ull) * 100;   // (Python's st_mtime_ns on Windows)
#else
    struct stat st;
    if (stat("/proc/self/exe", &st)) return "";
    const uint64_t size = (uint64_t)st.st_size;
    const uint64_t ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
    return "exe " + std::to_string(size) + " " + std::to_string(ns);
}

// the .ips / .pchtxt files of a folder, by name
std::vector<fs::path> patch_files(const fs::path &dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = mods_scan::u8(e.path().extension());
        if (mods_scan::ieq(ext, ".ips") || mods_scan::ieq(ext, ".pchtxt")) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string rel_of(const fs::path &p, const fs::path &root) {
    std::error_code ec;
    std::string r = mods_scan::u8(fs::relative(p, root, ec));
    if (ec || r.empty()) r = mods_scan::u8(p.filename());
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

std::string file_note(const exefs::File &f) {
    std::string why = f.note.empty() ? std::string() : fmt(f.note.c_str(), f.arg);
    return f.rel + ": " + why;
}

// one mod, in list order (the files of the mods above it are in g_files)
void scan_mod(nxr_mod &m) {
    m.exefs_patches = m.exefs_other = m.cheats_count = 0;
    m.exefs_note.clear();
    Mod md;
    md.name = m.name;
    md.on = m.on && m.problem == NXR_MOD_OK;
    md.first = (int)g_files.size();
    const fs::path root = mods_scan::from_u8(m.dir);
    std::vector<fs::path> files;
    for (auto &d : mods_scan::patch_dirs(root, mods_scan::from_u8(m.exefs)))   // (exefs/, exefs_patches/<name>/, loose)
        for (auto &f : patch_files(d)) files.push_back(f);
    std::error_code ec;

    const exefs::Image im = image();
    for (auto &p : files) {
        const std::string data = read_file(p);
        exefs::File f;
        f.mod = m.name;
        f.rel = rel_of(p, root);
        exefs::Writes w;
        std::string err, bid;
        bool ok, ours;
        if (mods_scan::ieq(mods_scan::u8(p.extension()), ".ips")) {
            ours = exefs::same_build(mods_scan::u8(p.stem()));
            ok = !ours || exefs::parse_ips((const uint8_t *)data.data(), data.size(), w, err);
        } else {
            ok = exefs::parse_pchtxt(data, bid, w, err);
            ours = exefs::same_build(bid);
        }
        if (!ours) { m.exefs_other++; continue; }
        m.exefs_patches++;
        if (!md.on) continue;
        if (!ok) {
            f.kind = exefs::File::BAD; f.note = PC_T("It cannot be read (%s)."); f.arg = err;
        } else if (!g_image) {
            continue;   // (a scan before main is in: nothing to check against; mods_exefs_start scans first)
        } else {
            exefs::classify(f, w, im, g_port);
        }
        g_files.push_back(f);
        g_file_ids.push_back("file " + fnv32(m.name + "/" + f.rel) + " " + fnv32(data));
    }
    // cheats: <cheats>/<first 8 bytes of the build id>.txt
    if (!m.cheats.empty()) {
        for (auto &e : fs::directory_iterator(mods_scan::from_u8(m.cheats), ec)) {
            const std::string fname = mods_scan::u8(e.path().filename());
            if (!e.is_regular_file(ec) || !exefs::cheat_file_name(fname)) continue;
            for (auto &c : exefs::parse_cheats(read_file(e.path(), 1u << 20))) {
                CheatRow r;
                r.mod = m.name; r.name = c.name; r.file = fname; r.c = c;
                if (g_image) exefs::classify_cheat(r.c, im);
                r.id = fnv32(m.name + "/" + c.name);
                r.key = "c" + fnv32(c.name + "\n" + std::string((const char *)c.words.data(), c.words.size() * 4));
                r.on = g_cheats_on.count(r.id) || g_cheats_on.count(r.key);
                if (!c.master) m.cheats_count++;
                // a code cheat joins the code patches, on or off: its switch site is built in, switched live
                if (md.on && r.c.kind == exefs::Cheat::CODE && g_slots < MAX_SWITCHES) {
                    exefs::File f;
                    f.mod = m.name; f.rel = "cheats/" + fname + " [" + c.name + "]"; f.kind = exefs::File::CODE;
                    f.code = r.c.code;
                    f.slot = r.slot = g_slots++;
                    g_files.push_back(f);
                    std::string words((const char *)c.words.data(), c.words.size() * 4);
                    g_file_ids.push_back("cheat " + r.id + " " + fnv32(words));
                }
                if (md.on) g_cheats.push_back(r);
            }
            break;
        }
    }
    md.files = (int)g_files.size() - md.first;
    g_mods.push_back(md);
    exefs::resolve(g_files);   // (only this mod's files change: the top wins)
}

// its line under the row, once every mod is in (the set hash and a failed build are the whole list's)
void note_mod(nxr_mod &m, const Mod &md) {
    if (!md.on) return;
    int in_use = 0, waiting = 0, data_only = 0;
    std::string left_out;   // the first file left out, and why
    std::vector<std::string> parts;
    for (int i = md.first; i < md.first + md.files; i++) {
        const exefs::File &f = g_files[i];
        if (f.kind == exefs::File::DATA) data_only++;
        else if (f.kind == exefs::File::CODE && !g_live && !g_built_content.count(exefs::content_id(g_file_ids[i])))
            waiting++;   // (by content: a renamed mod folder's code is still the exe's)
        else if (f.kind == exefs::File::CODE) in_use += f.slot < 0;   // (a code cheat built in: its F10 switch says, not left out)
        else if (f.kind != exefs::File::EMPTY && left_out.empty()) left_out = file_note(f);
    }
    if (in_use + data_only) parts.push_back(fmt_n(PC_T("%d ExeFS patches in use"), in_use + data_only));
    if (waiting) {
        if (!g_can_build) parts.push_back(fmt_n(PC_T("%d code patches need the game built on this PC (Builder install), not used"), waiting));
        else if (!g_failed.empty()) parts.push_back(fmt(PC_T("the build with its code patches failed (see exefs/%s/build.log next to totk.exe), not used"), g_failed));
        else parts.push_back(fmt_n(PC_T("%d code patches: the game is rebuilt with them at this start"), waiting));
    }
    if (m.exefs_other) parts.push_back(fmt_n(PC_T("%d ExeFS patches for another game version, not used"), m.exefs_other));
    if (!left_out.empty()) parts.push_back(fmt(PC_T("left out: %s"), left_out));
    if (!m.cheats.empty()) {
        if (m.cheats_count) parts.push_back(fmt_n(PC_T("%d cheats (F10 Cheats tab)"), m.cheats_count));
        else parts.push_back(fmt(PC_T("no cheat file for this game version (%s)"), exefs::cheat_file_of_build()));
    }
    std::string note;
    for (auto &p : parts) note += (note.empty() ? "" : "; ") + p;
    m.exefs_note = note;
}

// the hook (mods.cpp, under its list lock, once per scan, top first)
void hook(std::vector<nxr_mod> &list) {
    std::lock_guard<std::mutex> l(g_mu);
    g_mods.clear(); g_files.clear(); g_file_ids.clear(); g_cheats.clear();
    g_slots = 0;
    g_early = !g_image;
    std::vector<nxr_mod *> scanned;
    for (auto &m : list)
        if (m.problem != NXR_MOD_DUPLICATE && m.problem != NXR_MOD_BAD_NAME) scan_mod(m), scanned.push_back(&m);
    for (auto &r : g_cheats) {   // a code cheat left out (it overlaps a file higher in the list): greyed, with why
        if (r.slot < 0) continue;
        for (auto &f : g_files)
            if (f.slot == r.slot && f.kind != exefs::File::CODE) {
                r.c.kind = exefs::Cheat::BAD; r.c.note = f.note; r.c.arg = f.arg; r.slot = -1;
            }
    }
    const std::string set = exefs::set_hash(exefs::code_set(g_files));
    g_live = g_image && set == g_built && !set.empty();
    std::error_code ec;
    g_failed.clear();
    if (set != g_built && fs::exists(g_exe_dir / "exefs" / (set.empty() ? "none" : set) / "failed.txt", ec))
        g_failed = set.empty() ? "none" : set;
    for (size_t i = 0; i < scanned.size(); i++) note_mod(*scanned[i], g_mods[i]);
}

struct Install { Install() { mods_exefs_hook = hook; } } g_install;

// the port's own patch sites: TOTK_IMG/patches.json patches[] (offset "0x..", bytes "..", name "..")
void load_port_sites(const std::string &img_dir) {
    const std::string s = read_file(mods_scan::from_u8(img_dir) / "patches.json", 16u << 20);
    auto value = [&](size_t from, size_t to, const char *key) -> std::string {
        size_t k = s.find(key, from);
        if (k == std::string::npos || k >= to) return {};
        size_t a = s.find('"', k + strlen(key));
        size_t b = a == std::string::npos ? a : s.find('"', a + 1);
        return b == std::string::npos ? std::string() : s.substr(a + 1, b - a - 1);
    };
    for (size_t at = s.find("\"offset\""); at != std::string::npos; at = s.find("\"offset\"", at + 1)) {
        size_t end = s.find('}', at);
        size_t start = s.rfind('{', at);
        if (end == std::string::npos || start == std::string::npos) break;
        const std::string mod = value(start, end, "\"module\":");
        if (!mod.empty() && mod != "main") continue;
        exefs::Site site;
        site.off = (uint32_t)strtoul(value(start, end, "\"offset\":").c_str(), nullptr, 0);
        site.len = (uint32_t)value(start, end, "\"bytes\":").size() / 2;
        site.label = value(start, end, "\"name\":");
        if (site.label.empty()) site.label = "?";
        if (site.len) g_port.push_back(site);
    }
}

fs::path exe_dir() {
#ifdef _WIN32
    wchar_t b[32768];
    DWORD n = GetModuleFileNameW(nullptr, b, 32768);
    return fs::path(std::wstring(b, n)).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

// exefs-set.txt next to the exe: what it was built with
void load_built() {
    std::istringstream in(read_file(g_exe_dir / "exefs-set.txt", 1u << 20));
    std::string line, exe;
    std::string built;
    std::set<std::string> ids;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 4, "set ") == 0) built = line.substr(4);
        else if (line.compare(0, 4, "exe ") == 0) exe = line;
        else if (!line.empty()) ids.insert(line);
    }
    if (built.empty()) return;
    if (exe != exe_stamp()) {   // (another exe than the one built with them: it holds none of them)
        printf("[exefs] exefs-set.txt is for another totk.exe (%s, this one %s): no code patches built in\n",
               exe.empty() ? "no exe line" : exe.c_str(), exe_stamp().c_str());
        return;
    }
    g_built = built;
    std::set<std::string> content;
    for (auto &id : ids) content.insert(exefs::content_id(id));
    g_built_ids = ids;
    g_built_content = content;
}

// the program the cheat VM runs: per mod on, its masters and its data cheats switched on (none on: the mod adds nothing)
void load_program() {
    std::vector<uint32_t> prog;
    for (size_t i = 0; i < g_cheats.size();) {
        size_t j = i;
        while (j < g_cheats.size() && g_cheats[j].mod == g_cheats[i].mod) j++;
        bool any = false;
        for (size_t k = i; k < j; k++)
            any |= !g_cheats[k].c.master && g_cheats[k].on && g_cheats[k].c.kind == exefs::Cheat::DATA;
        if (any)
            for (int pass = 0; pass < 2; pass++)   // masters first
                for (size_t k = i; k < j; k++) {
                    const CheatRow &r = g_cheats[k];
                    if (r.c.kind != exefs::Cheat::DATA || r.c.master != (pass == 0) || (!r.c.master && !r.on)) continue;
                    prog.insert(prog.end(), r.c.words.begin(), r.c.words.end());
                }
        i = j;
    }
    static bool loaded = false;
    if (prog.empty() && !loaded) return;   // (no cheat yet: the VM stays as it is, TOTK_CHEATS included)
    loaded = true;
    nxr_cheat_program(prog.data(), prog.size());
}

// starts the build of the game with the mods' code patches (TOTK_EXEFS_BUILD) and exits; false: not started
bool start_build(const std::string &set) {
    const char *cmd = getenv("TOTK_EXEFS_BUILD");
    if (!cmd || !*cmd) return false;
    const std::string id = set.empty() ? "none" : set;
    const fs::path dir = g_exe_dir / "exefs" / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string list;
    if (!set.empty()) list += "set " + set + "\n";
    for (size_t i = 0; i < g_files.size(); i++)
        if (g_files[i].kind == exefs::File::CODE) list += g_file_ids[i] + "\n";
    std::ofstream(dir / "request.json", std::ios::binary) << exefs::request_json(g_files, image());
    std::ofstream(dir / "exefs-set.txt", std::ios::binary) << list;
    std::string line = std::string(cmd) + " --request \"" + mods_scan::u8(dir / "request.json") + "\" --exe-dir \""
                     + mods_scan::u8(g_exe_dir) + "\"";
    printf("[exefs] code patches changed (built %s, mods %s): %s\n", g_built.empty() ? "none" : g_built.c_str(), id.c_str(),
           line.c_str());
    fflush(stdout);
#ifdef _WIN32
    char buf[16];
    snprintf(buf, sizeof buf, "%lu", GetCurrentProcessId());
    line += std::string(" --pid ") + buf;
    std::wstring w = mods_scan::from_u8(line).wstring();
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi)) {
        printf("[exefs] the build did not start (error %lu)\n", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    line += " --pid " + std::to_string(getpid()) + " &";
    if (system(line.c_str()) != 0) return false;
#endif
    return true;
}

}  // namespace

void mods_exefs_start(const char *img_dir) {
    // a game version this file does not know, or no layout: no ExeFS side at all
    const std::string mj = read_file(mods_scan::from_u8(img_dir) / "main.json", 1u << 20);
    size_t b = mj.find("\"build_id\"");
    size_t q = b == std::string::npos ? b : mj.find('"', mj.find(':', b) + 1);
    exefs::Image im;
    const char *bid = q == std::string::npos ? nullptr : exefs::known_build(mj.substr(q + 1, 40));
    if (!bid || !exefs::image_layout(mj, im)) {
        mods_exefs_hook = nullptr;
        return;
    }
    im.bytes = (const uint8_t *)MAIN_BASE;
    {
        std::lock_guard<std::mutex> l(g_mu);
        exefs::g_bid = bid;   // (the files and cheats this build takes: its version's, by name)
        g_im = im;
        g_exe_dir = exe_dir();
        load_built();
        load_port_sites(img_dir);
        const char *cmd = getenv("TOTK_EXEFS_BUILD");
        g_can_build = cmd && *cmd;
        std::vector<char> on(64 * 1024);   // (two ids per cheat on: ~3600 cheats)
        if (pc_settings_raw("cheats.on", on.data(), on.size()))
            for (auto &id : split(on.data(), '|')) g_cheats_on.insert(id);
        g_image = true;
    }
    mods_list();   // the scan: the hook
    std::lock_guard<std::mutex> l(g_mu);
    if (g_early) printf("[exefs] the mods were scanned before main was in: their ExeFS patches are not used this run\n");
    const std::string set = exefs::set_hash(exefs::code_set(g_files));   // (hook: g_failed)
    bool any_files = !g_files.empty();
    if (set != g_built && g_failed.empty() && start_build(set)) {
        fflush(stdout);
        _exit(0);   // (the build tool waits for this process, swaps the exe in and starts the game again)
    }
    // the writes this exe can take: data-only files always, code files when the exe holds their code
    const bool built = set == g_built;
    int files = 0;
    size_t bytes = 0;
    for (auto &f : g_files) {
        if (f.slot >= 0) continue;   // (a code cheat: its switch, below; main's bytes stay the game's)
        if (f.kind == exefs::File::DATA || (f.kind == exefs::File::CODE && built)) {
            for (auto *w : {&f.code, &f.data})
                for (auto &[off, v] : *w) { ((uint8_t *)MAIN_BASE)[off] = v; bytes++; }
            files++;
        }
    }
    int switches = 0;
    for (auto &r : g_cheats)
        if (g_live && r.slot >= 0) g_switch[r.slot].store(r.on, std::memory_order_relaxed), switches++;
    if (any_files)
        printf("[exefs] %d patch files applied (%zu bytes), %d code cheats switchable, code set %s, exe built with %s%s\n",
               files, bytes, switches, set.empty() ? "none" : set.c_str(), g_built.empty() ? "none" : g_built.c_str(),
               built ? "" : g_failed.empty() ? " (code patches not used: no build command)" : " (code patches not used: their build failed)");
    if (!g_cheats.empty()) printf("[exefs] %zu cheats\n", g_cheats.size());
    load_program();
}

std::vector<nxr_cheat> mods_cheats() {
    std::lock_guard<std::mutex> l(g_mu);
    std::vector<nxr_cheat> out;
    for (auto &r : g_cheats) {
        if (r.c.master) continue;
        nxr_cheat c;
        c.mod = r.mod; c.name = r.name; c.on = r.on;
        c.usable = r.c.kind != exefs::Cheat::BAD;
        c.code = r.c.kind == exefs::Cheat::CODE;
        if (!c.usable) c.note = fmt(r.c.note.c_str(), r.c.arg);
        else if (c.code && !(g_live && r.slot >= 0))
            c.note = pc_tr("It changes the game's code: the game is rebuilt with it at the next start.");
        out.push_back(c);
    }
    return out;
}

void mods_cheat_set(int i, bool on) {
    std::lock_guard<std::mutex> l(g_mu);
    int n = -1;
    for (auto &r : g_cheats) {
        if (r.c.master || ++n != i) continue;
        if (r.on == on || r.c.kind == exefs::Cheat::BAD) return;
        r.on = on;
        if (on) g_cheats_on.insert({r.id, r.key}); else g_cheats_on.erase(r.id), g_cheats_on.erase(r.key);
        std::string v;
        for (auto &id : g_cheats_on) v += (v.empty() ? "" : "|") + id;
        pc_settings_raw_set("cheats.on", v.empty() ? nullptr : v.c_str());
        if (r.c.kind != exefs::Cheat::CODE) load_program();
        else if (g_live && r.slot >= 0) g_switch[r.slot].store(on, std::memory_order_relaxed);   // (built in: at once)
        else mods_need_restart("a cheat that changes the game's code");
        return;
    }
}

// the generated code's read of a switch site's flag (gen_ext.py ExtEmitter.switched; a C++ declaration there)
int nxr_code_switch(unsigned slot) {
    return slot < (unsigned)MAX_SWITCHES && g_switch[slot].load(std::memory_order_relaxed);
}
