// mods.cpp: the player's mods list (mods.h): the scan of the TOTK_MODS_DIR folders, its order and switches in
// settings.ini (mods.order / mods.off), and what each mod is for the F10 Mods tab.
#include "mods.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "mods_scan.h"
#include "pc_settings.h"

namespace {

namespace ms = mods_scan;

const char* const kTitle = "0100F2C0115B6000";   // TotK

std::mutex g_mtx;
std::vector<nxr_mod> g_mods;           // now (the tab's edits included)
std::vector<std::string> g_start_key;  // name + on of each at start (mods_changed)
std::vector<std::string> g_romfs;      // the RomFS roots served this run
std::vector<std::string> g_folders;
bool g_read_only = false;
std::atomic<bool> g_need_restart{false};   // mods_need_restart

std::vector<std::string> env_list(const char* var) {
    std::vector<std::string> v;
    const char* e = getenv(var);
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (std::string s = e ? e : ""; !s.empty();) {
        size_t k = s.find(sep);
        std::string d = s.substr(0, k);
        while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        if (!d.empty()) v.push_back(d);
        if (k == std::string::npos) break;
        s = s.substr(k + 1);
    }
    return v;
}

std::string setting(const char* key) {
    char buf[8192];
    return pc_settings_raw(key, buf, sizeof buf) ? std::string(buf) : std::string();
}

bool usable(const nxr_mod& m) { return m.problem == NXR_MOD_OK && (m.kind & ms::ROMFS); }

void save_locked() {
    std::vector<std::string> order, off;
    for (const auto& m : g_mods) {
        if (m.problem == NXR_MOD_BAD_NAME || m.problem == NXR_MOD_DUPLICATE) continue;
        order.push_back(m.name);
        if (!m.on) off.push_back(m.name);
    }
    pc_settings_raw_set("mods.order", ms::join(order).c_str());
    pc_settings_raw_set("mods.off", off.empty() ? nullptr : ms::join(off).c_str());
}

void scan_locked() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* u = getenv("TOTK_USER_MODS"); u && *u) {   // the launcher's own list: as given, all on
        g_read_only = true;
        for (const auto& d : env_list("TOTK_USER_MODS")) {
            nxr_mod m;
            m.dir = m.romfs = d;
            const ms::fs::path p = ms::from_u8(d);   // (<mod>/romfs: the mod's name)
            m.name = ms::u8(ms::ieq(ms::u8(p.filename()), "romfs") ? p.parent_path().filename() : p.filename());
            m.layout = "TOTK_USER_MODS";
            m.kind = ms::ROMFS;
            g_mods.push_back(m);
            g_romfs.push_back(d);
            printf("[mods] %s: %s (TOTK_USER_MODS)\n", m.name.c_str(), d.c_str());
        }
        for (const auto& m : g_mods) g_start_key.push_back(m.name + "\x01" "1");
        return;
    }
    g_folders = env_list("TOTK_MODS_DIR");
    std::vector<nxr_mod> found;
    for (const auto& folder : g_folders) {
        std::error_code ec;
        std::vector<std::pair<std::string, ms::fs::path>> dirs;   // (name, folder)
        for (ms::fs::directory_iterator it(ms::from_u8(folder), ec), e; !ec && it != e; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const std::string n = ms::u8(it->path().filename());
            if (n.empty() || n[0] == '_' || n[0] == '.') continue;
            ms::fs::path home;   // a copied emulator collection (yuzu load/<title>/, Ryujinx contents/<title>/): its mods
            const std::vector<ms::fs::path> inner = ms::collection(it->path(), kTitle, &home);
            if (inner.empty()) { dirs.push_back({n, it->path()}); continue; }
            printf("[mods] %s: a collection of %zu mod(s)\n", n.c_str(), inner.size());
            if (!home.empty()) dirs.push_back({n, home});   // (its own romfs / exefs / cheats beside them)
            for (const auto& d : inner) dirs.push_back({ms::u8(d.filename()), d});
        }
        for (const auto& [name, dir] : dirs) {
            nxr_mod m;
            m.name = name;
            m.dir = ms::u8(dir);
            ms::Found f = ms::scan_mod(dir, kTitle);
            m.romfs = f.romfs.empty() ? "" : ms::u8(f.romfs);
            m.exefs = f.exefs.empty() ? "" : ms::u8(f.exefs);
            m.cheats = f.cheats.empty() ? "" : ms::u8(f.cheats);
            m.layout = f.layout;
            m.other_title = f.other_title;
            m.kind = f.kind;
            bool dup = false;
            for (const auto& o : found) dup |= ms::ieq(o.name, m.name);
            if (dup) m.problem = NXR_MOD_DUPLICATE;
            else if (m.name.find('|') != std::string::npos) m.problem = NXR_MOD_BAD_NAME;
            else if (f.kind & ms::SUBSDK) m.problem = NXR_MOD_PLUGIN;
            else if (!f.kind) m.problem = f.other_title.empty() ? NXR_MOD_EMPTY : NXR_MOD_OTHER_GAME;
            else if (!(f.kind & ms::ROMFS) && !mods_exefs_hook) m.problem = NXR_MOD_EXEFS_ONLY;
            found.push_back(m);
        }
    }
    std::vector<std::string> names;
    for (const auto& m : found)
        if (m.problem != NXR_MOD_DUPLICATE) names.push_back(m.name);
    const std::vector<std::string> off = ms::split(setting("mods.off"));
    for (const auto& n : ms::order(names, setting("mods.order")))
        for (auto& m : found)
            if (m.name == n && m.problem != NXR_MOD_DUPLICATE) {
                m.on = std::find(off.begin(), off.end(), n) == off.end();
                g_mods.push_back(m);
            }
    for (auto& m : found)
        if (m.problem == NXR_MOD_DUPLICATE) g_mods.push_back(m);
    if (mods_exefs_hook) mods_exefs_hook(g_mods);
    static const char* const why[] = {"", "empty", "'|' in its name", "another game's", "same name as an earlier one",
                                      "code modules (not supported)", "code patches / cheats only (not supported)",
                                      "its ExeFS side: "};
    for (const auto& m : g_mods) {
        g_start_key.push_back(m.name + "\x01" + (m.on ? "1" : "0"));
        if (m.on && usable(m)) g_romfs.push_back(m.romfs);
        const bool ok = m.problem == NXR_MOD_OK;
        printf("[mods] %s: %s%s%s%s%s\n", m.name.c_str(), !m.on ? "off" : ok ? "on" : "not used: ",
               m.on && !ok ? why[m.problem] : "", m.on && m.problem == NXR_MOD_EXEFS ? m.problem_note.c_str() : "",
               m.on && usable(m) ? ", romfs " : m.on && ok ? " (no romfs)" : "",
               m.on && usable(m) ? (m.layout + " " + m.romfs).c_str() : "");
    }
    if (!g_folders.empty()) printf("[mods] %zu mod(s), %zu RomFS served\n", g_mods.size(), g_romfs.size());
}

}  // namespace

std::vector<nxr_mod> mods_list() {
    std::lock_guard<std::mutex> l(g_mtx);
    scan_locked();
    return g_mods;
}

const std::vector<std::string>& mods_romfs_start() {
    std::lock_guard<std::mutex> l(g_mtx);
    scan_locked();
    return g_romfs;   // (fixed after the scan)
}

std::string mods_folder() {
    std::lock_guard<std::mutex> l(g_mtx);
    scan_locked();
    return g_folders.empty() ? "" : g_folders[0];
}

bool mods_read_only() {
    std::lock_guard<std::mutex> l(g_mtx);
    scan_locked();
    return g_read_only;
}

void mods_set_on(int i, bool on) {
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_read_only || i < 0 || i >= (int)g_mods.size() || g_mods[i].on == on) return;
    g_mods[i].on = on;
    save_locked();
}

void mods_move(int i, int delta) {
    std::lock_guard<std::mutex> l(g_mtx);
    int j = i + delta;
    if (g_read_only || i < 0 || j < 0 || i >= (int)g_mods.size() || j >= (int)g_mods.size()) return;
    std::swap(g_mods[i], g_mods[j]);
    save_locked();
}

bool mods_changed() {
    if (g_need_restart.load()) return true;
    std::lock_guard<std::mutex> l(g_mtx);
    std::vector<std::string> now;
    for (const auto& m : g_mods) now.push_back(m.name + "\x01" + (m.on ? "1" : "0"));
    return now != g_start_key;
}

namespace {
std::vector<nxr_mod> g_start;   // nxr_mods_*: the list at start (filled by the first call, then fixed)
const std::vector<nxr_mod>& start_list() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::lock_guard<std::mutex> l(g_mtx);
        scan_locked();
        g_start = g_mods;   // (a tab edit before this call is not in it: callers ask at start)
        for (auto& m : g_start) {   // the order and switches this run uses
            for (size_t k = 0; k < g_start_key.size(); k++)
                if (g_start_key[k].rfind(m.name + "\x01", 0) == 0) m.on = g_start_key[k].back() == '1';
        }
        std::stable_sort(g_start.begin(), g_start.end(), [](const nxr_mod& a, const nxr_mod& b) {
            auto pos = [](const std::string& n) {
                for (size_t k = 0; k < g_start_key.size(); k++)
                    if (g_start_key[k].rfind(n + "\x01", 0) == 0) return k;
                return g_start_key.size();
            };
            return pos(a.name) < pos(b.name);
        });
    });
    return g_start;
}
const nxr_mod* start_at(int i) { const auto& v = start_list(); return i >= 0 && i < (int)v.size() ? &v[i] : nullptr; }
}  // namespace

int nxr_mods_count(void) { return (int)start_list().size(); }
const char* nxr_mods_name(int i) { const nxr_mod* m = start_at(i); return m ? m->name.c_str() : ""; }
const char* nxr_mods_dir(int i) { const nxr_mod* m = start_at(i); return m ? m->dir.c_str() : ""; }
const char* nxr_mods_exefs(int i) { const nxr_mod* m = start_at(i); return m ? m->exefs.c_str() : ""; }
const char* nxr_mods_cheats(int i) { const nxr_mod* m = start_at(i); return m ? m->cheats.c_str() : ""; }
int nxr_mods_on(int i) {
    const nxr_mod* m = start_at(i);
    return m && m->on && m->problem == NXR_MOD_OK;
}

void (*mods_exefs_hook)(std::vector<nxr_mod>& list) = nullptr;

void mods_need_restart(const char* why) {
    if (!g_need_restart.exchange(true)) printf("[mods] applies at the next start: %s\n", why ? why : "");
}

void mods_set_counts(const std::string& romfs, int replaced, int added, int by_port, int by_mod, int other_version,
                     const std::string& by_mod_name) {
    std::lock_guard<std::mutex> l(g_mtx);
    for (auto& m : g_mods)
        if (m.romfs == romfs) {
            m.counted = true;
            m.replaced = replaced, m.added = added, m.by_port = by_port, m.by_mod = by_mod;
            m.other_version = other_version, m.by_mod_name = by_mod_name;
        }
}
