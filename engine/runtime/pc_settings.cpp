// pc_settings.cpp: the settings registry, settings.ini and the start-up bridge to the engine's environment knobs
// (pc_settings.h). The engine's own rows are at the end; other areas add theirs with pc_settings_add.
#include "pc_settings.h"
#include "pc_overlay.h"
#include "pc_display_caps.h"
#include "../../games/totk/include/totk/totk_patches.h"  // totk_quality_presets() (the preset table)
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Before every ordinary static initializer of the runtime: hle_audio.cpp's g_master, hle_fs.cpp's skip-opening flag
// and hle_osd.cpp's recomp_settings.ini loader read the environment in theirs.
#pragma init_seg(lib)

namespace {

struct Row {
    const pc_setting *def;
    char value[96];
    char boot[96];      // the value this run started with
    char saved[96];     // the file's value (or the default): what an overridden row writes back
    bool overridden;    // NX_SET_<KEY>
    bool placeholder;   // the overlay's stand-in for a knob another workstream owns: its own row replaces this one
};

std::mutex g_mu;
std::vector<Row> &rows() { static std::vector<Row> v; return v; }
std::vector<std::string> g_lines;   // the file as read: comments and unknown keys are written back
std::string g_path;
bool g_booted, g_file_on, g_dirty, g_first_run, g_fresh_file;
std::vector<pc_preset_row> g_presets;   // graphics.preset's members (pc_settings_add_presets)
ULONGLONG g_dirty_at;

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

void env_put(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }   // ("" removes it)

std::string override_name(const char *key) {
    std::string n = "NX_SET_";
    for (const char *p = key; *p; p++) n += *p == '.' ? '_' : (char)toupper((unsigned char)*p);
    return n;
}

bool valid(const pc_setting *d, const char *v) {
    if (d->flags & (PC_SET_KEYS | PC_SET_PAD)) return strlen(v) < 90 && !strpbrk(v, "#=\r\n");   // (hle_hid.cpp checks the names)
    if (d->values) {
        for (const char *const *p = d->values; *p; p++) if (!strcmp(*p, v)) return true;
        return false;
    }
    char *e;
    const float x = static_cast<float>(strtod(v, &e));   // as a float, like min / max: "0.1" is not below 0.1f
    return e != v && !*e && x >= d->min && x <= d->max;
}

Row *find(const char *key) {
    for (Row &r : rows()) if (!strcmp(r.def->key, key)) return &r;
    return nullptr;
}

void to_env(const Row &r) {
    if (r.def->to_env) r.def->to_env(r.value);
    else if (r.def->env) env_put(r.def->env, r.value);
}

// Paths are UTF-8 here and wide at the Win32 calls, so a profile folder outside the ANSI code page works
std::wstring widen(const std::string &s) {
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}
std::string env_u8(const wchar_t *k) {   // "" when unset
    const wchar_t *v = _wgetenv(k);
    if (!v || !*v) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, v, -1, &s[0], n, nullptr, nullptr);
    s.resize(n > 0 ? n - 1 : 0);
    return s;
}
FILE *ufopen(const std::string &path, const wchar_t *mode) { return _wfopen(widen(path).c_str(), mode); }

std::string settings_dir() {   // hle_osd.cpp settings_path()'s folder
    std::string f = env_u8(L"TOTK_SETTINGS_FILE");
    if (!f.empty()) {
        size_t i = f.find_last_of("/\\");
        return i == std::string::npos ? "." : f.substr(0, i);
    }
    std::string d = env_u8(L"TOTK_USERDATA");
    if (d.empty()) d = "C:/totk/userdata";
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    size_t s = d.find_last_of("/\\");
    return s == std::string::npos ? "." : d.substr(0, s);
}

bool parse(const std::string &line, std::string *k, std::string *v) {
    std::string s = line;
    size_t h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    size_t eq = s.find('=');
    if (eq == std::string::npos) return false;
    *k = trim(s.substr(0, eq));
    *v = trim(s.substr(eq + 1));
    return !k->empty();
}

// recomp_settings.ini (TOTK_X=value, hle_osd.cpp's Graphics menu) -> our rows, the first time only
int import_recomp_settings(std::vector<std::pair<std::string, std::string>> *kv) {
    FILE *f = ufopen(settings_dir() + "/recomp_settings.ini", L"r");
    if (!f) return 0;
    int n = 0;
    char buf[512];
    std::string fps;   // display.fps_cap from TOTK_FPS and TOTK_UNLOCK_FPS ("raw", the unpaced mode, is not offered)
    bool unlocked = false;
    while (fgets(buf, sizeof buf, f)) {
        std::string k, v;
        if (!parse(buf, &k, &v)) continue;
        if (k == "TOTK_FPS") { fps = v == "0" || v.empty() ? "30" : v; continue; }
        if (k == "TOTK_UNLOCK_FPS") { unlocked = !v.empty() && v != "0" && v != "raw"; continue; }
        for (const Row &r : rows())
            if (r.def->env && k == r.def->env) { kv->push_back({r.def->key, v}); n++; }
    }
    fclose(f);
    if (unlocked) fps = "unlocked";
    if (!fps.empty()) { kv->push_back({"display.fps_cap", fps}); n++; }
    return n;
}

void save_now();
std::vector<std::pair<std::string, std::string>> g_kv;   // key -> value as read from the file at start

// A row's value for this run: the file's, then NX_SET_<KEY> over it; into the environment when either is set
void start_value(Row &r) {
    bool set = false;
    for (const auto &e : g_kv)
        if (e.first == r.def->key && valid(r.def, e.second.c_str())) {
            snprintf(r.value, sizeof r.value, "%s", e.second.c_str());
            set = true;
        }
    memcpy(r.saved, r.value, sizeof r.saved);
    std::string on = override_name(r.def->key);
    if (const char *o = getenv(on.c_str())) {
        if (valid(r.def, o)) { snprintf(r.value, sizeof r.value, "%s", o); r.overridden = set = true; }
        else printf("[pc-settings] %s=%s is not a value of %s: ignored\n", on.c_str(), o, r.def->key);
    }
    if (set) to_env(r);
    memcpy(r.boot, r.value, sizeof r.boot);
}

// The first start's Frame rate for the primary monitor (where a first start opens), from its fastest mode at its
// resolution: a 60 Hz panel (59-61) 60, a 90 Hz one (89-91) 90, from 125 Hz (144 / 165 / 240) "unlocked" (paced up to
// the refresh: present's 17b9f4d); anything else (75, 100, 120 Hz) keeps the package's 120, which a 75 / 100 Hz panel
// shows at its own rate. nullptr: keep the file's. A fast monitor merely set low in Windows offers its fast mode, so
// it is judged by that.
const char *fps_cap_for_hz(long top) {   // (59.94 / 89.91 Hz modes report 59 / 89)
    return top >= 59 && top <= 61 ? "60" : top >= 89 && top <= 91 ? "90" : top >= 125 ? "unlocked" : nullptr;
}
// The first start's monitor, the primary (pc_display_caps.cpp). NXR_FIRST_RUN_HZ (test_pc_settings.py) stands in for
// it: its fastest Hz, with NXR_FIRST_RUN_VRR (PCD_VRR_*) and NXR_FIRST_RUN_PEAK (nits, Windows HDR on), and is
// described the same way (pcd_caps_text's pc_tr: the language's first use, test_pc_settings.py's game.language case).
bool first_run_display(pcd_caps *c) {
    *c = {};
    if (const char *t = getenv("NXR_FIRST_RUN_HZ")) {
        c->hz = c->top_hz = atoi(t);
        if (const char *v = getenv("NXR_FIRST_RUN_VRR")) c->vrr = atoi(v);
        if (const char *p = getenv("NXR_FIRST_RUN_PEAK")) c->hdr_on = (c->peak_nits = atoi(p)) > 0;
        snprintf(c->name, sizeof c->name, "NXR_FIRST_RUN_HZ");
        c->vrr_kind = c->vrr ? "G-SYNC" : nullptr;
    } else if (!pcd_caps_of(nullptr, c)) {
        return false;
    }
    char line[384];
    pcd_caps_text(*c, line, sizeof line);
    printf("[pc-settings] first start: primary monitor %s (NVAPI VRR bits %d)\n", line, c->nv_bits);
    return true;
}
// With VRR on (the driver's word: PCD_VRR_ON), Unlocked: hle_vi.cpp paces it just under the refresh, inside the VRR
// range (116 at 120 Hz, 138 at 144), where a fixed 120 at 120 Hz would sit at the top and queue (FIFO). On a fixed
// refresh that pacing repeats frames (116 on 120 Hz: about 4 a second), so an EDID-only VRR (PCD_VRR_MONITOR) counts
// as fixed. A 60 Hz panel keeps 60.
const char *first_run_fps_cap(const pcd_caps &c) {
    return c.vrr == PCD_VRR_ON && c.top_hz >= 62 ? "unlocked" : fps_cap_for_hz(c.top_hz);
}

void boot() {
    if (g_booted || !pc_overlay_enabled()) return;
    g_booted = true;
    const char *off = getenv("TOTK_SETTINGS"), *mine = getenv("NXR_SETTINGS_OWNED");
    // (NXR_SETTINGS_OWNED: the 0 is ours, inherited by a restart of this exe (hle_hid.cpp's auto new game))
    g_file_on = !(off && off[0] == '0') || (mine && mine[0] == '1');
    std::vector<std::pair<std::string, std::string>> kv;   // key -> value from the file
    if (g_file_on) {
        g_path = env_u8(L"NXR_SETTINGS");
        if (g_path.empty()) g_path = settings_dir() + "/settings.ini";
        if (FILE *f = ufopen(g_path, L"r")) {
            char buf[512];
            while (fgets(buf, sizeof buf, f)) {
                std::string line = buf;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                g_lines.push_back(line);
                std::string k, v;
                if (parse(line, &k, &v)) kv.push_back({k, v});
            }
            fclose(f);
            printf("[pc-settings] %zu value(s) from %s\n", kv.size(), g_path.c_str());
            // The package's settings.ini (the launcher copies it to the user's folder): a few lines and no
            // pc.welcome_done, which every save writes. A first start too: the Welcome screen shows.
            g_fresh_file = std::none_of(kv.begin(), kv.end(), [](const auto &e) { return e.first == "pc.welcome_done"; });
        } else if (int n = import_recomp_settings(&kv)) {
            printf("[pc-settings] no %s yet: %d value(s) imported from recomp_settings.ini\n", g_path.c_str(), n);
            g_dirty = true;
            g_dirty_at = GetTickCount64();
        } else {
            g_first_run = true;   // nothing to start from: the overlay offers a preset (first-run screen)
        }
        // ours from here: hle_osd.cpp's loader (a later static initializer) leaves the environment alone
        env_put("TOTK_SETTINGS", "0");
        env_put("NXR_SETTINGS_OWNED", "1");
    } else {
        printf("[pc-settings] TOTK_SETTINGS=0: defaults and NX_SET_ overrides only, nothing saved\n");
    }
    // display.present_mode (the raw present modes, until 2026-10-25) -> pc_window.cpp's display.vsync / display.vrr:
    // immediate = vsync off, fifo / fifo_relaxed = VRR (FIFO). Its line leaves the file at the next save.
    std::string pm;
    bool newer = false;
    for (const auto &e : kv) {
        if (e.first == "display.present_mode") pm = e.second;
        newer |= e.first == "display.vsync" || e.first == "display.vrr";
    }
    if (!pm.empty() && !newer) {
        if (pm == "immediate") kv.push_back({"display.vsync", "off"});
        if (pm == "fifo" || pm == "fifo_relaxed") kv.push_back({"display.vrr", "on"});
        printf("[pc-settings] display.present_mode = %s: now display.vsync / display.vrr\n", pm.c_str());
    }
    for (size_t i = g_lines.size(); i-- > 0;) {
        std::string k, v;
        if (parse(g_lines[i], &k, &v) && k == "display.present_mode") g_lines.erase(g_lines.begin() + i);
    }
    // controls.gameplay: "pc" by default since 4.22, but every file saved before holds the old default "console" (a save
    // writes every row), so an install that ran 1810 would keep the console layout. Once per file (the marker
    // controls.gameplay_rev = 2): console becomes pc; a choice made after that stays. Rev 3: attack moved to LMB, lock-on
    // (and the horse's reset_camera, its key) to RMB, aim retired. A bind of those saved before was made against the old
    // keys, and a saved lock_on on Mouse Left took LMB from attack where lock_on's row comes first (AIR) and lost it where
    // attack's does. Once per file: those saved binds are dropped, the new defaults apply. Rev 4: the shields and the
    // ability wheel got CLIMB rows, and their F10 entries moved from WORLD to GAMEPLAY so one bind reaches both. A saved
    // controls.world.{shields,ability_wheel} would stay nearest on foot (a GAMEPLAY rebind then trips the mixed levels, or
    // leaves foot and wall on different keys). Once per file: each moves to controls.gameplay.<action>, unless that is set
    // already (then the world line goes). Rev 5: an ability is left with Tab, its own action "leave", and Esc is the pause
    // there. A saved controls.ability.quit (the old Esc entry's) or controls.ability.pause (the old dead pause) would keep
    // or kill keys that no longer mean that: dropped once per file. Rev 6: controls.gyro_scale (Motion sensitivity) is a
    // real pad's only now; it used to scale the mouse as motion aim too. With controls.mouse_gyro on and gyro_scale != 1,
    // its factor moves once into controls.mouse_gyro_speed (the mouse gyro's own, linear in the same rate), so that aim
    // turns as before.
    if (g_file_on) {
        int rev = 0;
        for (const auto &e : kv) if (e.first == "controls.gameplay_rev") rev = atoi(e.second.c_str());
        if (rev < 6) {
            if (rev < 2) {
                for (auto &e : kv)
                    if (e.first == "controls.gameplay" && e.second == "console") {
                        e.second = "pc";
                        printf("[pc] settings: controls.gameplay migrated console -> pc (rev 2)\n");
                    }
                for (std::string &l : g_lines) {
                    std::string k, v;
                    if (parse(l, &k, &v) && k == "controls.gameplay" && v == "console") l = "controls.gameplay = pc";
                }
            }
            if (rev < 3) {
                // controls.<action> / controls.<scope>.<action>, the action one of 4.23's
                auto moved = [](const std::string &k) {
                    if (k.compare(0, 9, "controls.") != 0) return false;
                    const std::string a = k.substr(k.rfind('.') + 1);
                    return a == "attack" || a == "lock_on" || a == "aim" || a == "reset_camera";
                };
                for (size_t i = kv.size(); i-- > 0;)
                    if (moved(kv[i].first)) {
                        printf("[pc] settings: %s = %s dropped (4.23 moved attack / lock-on: the default now, rev 3)\n",
                               kv[i].first.c_str(), kv[i].second.c_str());
                        kv.erase(kv.begin() + (long)i);
                    }
                for (size_t i = g_lines.size(); i-- > 0;) {
                    std::string k, v;
                    if (parse(g_lines[i], &k, &v) && moved(k)) g_lines.erase(g_lines.begin() + (long)i);
                }
            }
            for (const char *a : {"shields", "ability_wheel"}) {   // (rev 4)
                if (rev >= 4) break;
                const std::string from = std::string("controls.world.") + a, to = std::string("controls.gameplay.") + a;
                bool have_to = false;
                for (const auto &e : kv) have_to |= e.first == to;
                for (size_t i = kv.size(); i-- > 0;)
                    if (kv[i].first == from) {
                        printf("[pc] settings: %s = %s %s%s (rev 4: one entry for on foot and the wall)\n", from.c_str(),
                               kv[i].second.c_str(), have_to ? "dropped, set already: " : "-> ", to.c_str());
                        if (have_to) kv.erase(kv.begin() + (long)i);
                        else { kv[i].first = to; have_to = true; }
                    }
                for (size_t i = g_lines.size(); i-- > 0;) {
                    std::string k, v;
                    if (!parse(g_lines[i], &k, &v) || k != from) continue;
                    bool line_to = false;
                    for (const std::string &l : g_lines) { std::string k2, v2; line_to |= parse(l, &k2, &v2) && k2 == to; }
                    if (line_to) g_lines.erase(g_lines.begin() + (long)i);
                    else g_lines[i] = to + " = " + v;
                }
            }
            for (const char *gone : {"controls.ability.quit", "controls.ability.pause"}) {   // (rev 5)
                if (rev >= 5) break;
                for (size_t i = kv.size(); i-- > 0;)
                    if (kv[i].first == gone) {
                        printf("[pc] settings: %s = %s dropped (4.25: Tab leaves an ability, Esc pauses; rev 5)\n", gone,
                               kv[i].second.c_str());
                        kv.erase(kv.begin() + (long)i);
                    }
                for (size_t i = g_lines.size(); i-- > 0;) {
                    std::string k, v;
                    if (parse(g_lines[i], &k, &v) && k == gone) g_lines.erase(g_lines.begin() + (long)i);
                }
            }
            {   // (rev 6; the last value of a key wins, as the read above)
                auto last = [&kv](const char *key) -> std::string * {
                    std::string *p = nullptr;
                    for (auto &e : kv) if (e.first == key) p = &e.second;
                    return p;
                };
                // (each value as the runtime takes it, valid()'s test on hle_hid's rows: a value outside the row is
                // not used, so it moves nothing; the rows themselves are not in yet, hle_hid adds them later)
                auto num = [](const std::string *s, float lo, float hi, double *x) {
                    if (!s) return false;
                    char *e;
                    const double d = strtod(s->c_str(), &e);
                    if (e == s->c_str() || *e || (float)d < lo || (float)d > hi) return false;
                    *x = d;
                    return true;
                };
                const std::string *mg = last("controls.mouse_gyro");
                const bool gyro_on = mg && (*mg == "aim" || *mg == "always");   // (anything else: the row's default, off)
                double g = 1, s0 = 1;
                if (gyro_on && num(last("controls.gyro_scale"), 0.25f, 3.0f, &g) && g != 1) {
                    const char *key = "controls.mouse_gyro_speed";
                    std::string *sp = last(key);
                    num(sp, 0.05f, 4.0f, &s0);   // (not a value of the row: its default, 1)
                    char nv[16];   // (the row's 0.05 steps, 0.05 .. 4)
                    snprintf(nv, sizeof nv, "%g", std::clamp((int)(s0 * g * 20 + 0.5), 1, 80) / 20.0);
                    printf("[pc] settings: %s %g -> %s (controls.gyro_scale %g: the pad's only now; rev 6)\n", key, s0, nv,
                           g);
                    if (sp) *sp = nv;
                    else kv.push_back({key, nv});
                    bool line = false;
                    for (std::string &l : g_lines) {
                        std::string k, v;
                        if (parse(l, &k, &v) && k == key) { l = std::string(key) + " = " + nv; line = true; }
                    }
                    if (!line) g_lines.push_back(std::string(key) + " = " + nv);
                }
            }
            for (size_t i = g_lines.size(); i-- > 0;) {
                std::string k, v;
                if (parse(g_lines[i], &k, &v) && k == "controls.gameplay_rev") g_lines.erase(g_lines.begin() + (long)i);
            }
            bool had = false;
            for (auto &e : kv) if (e.first == "controls.gameplay_rev") { e.second = "6"; had = true; }
            if (!had) kv.push_back({"controls.gameplay_rev", "6"});
            g_lines.push_back("controls.gameplay_rev = 6   # (the one-time settings moves are done)");
            // (saved now when a file was read; a fresh install writes it with its first save, so the first-run screen
            // still shows until the player has saved something)
            if (!g_first_run) {
                g_dirty = true;
                g_dirty_at = GetTickCount64();
            }
        }
    }
    // The package's settings.ini says display.fps_cap = 120 for every PC and nothing of VRR or HDR (package.py); on its
    // first start (no pc.welcome_done yet) the monitor decides:
    //  - Frame rate (only the package's 120): first_run_fps_cap.
    //  - VRR (no display.vrr line yet): on where the driver says it is on (G-SYNC on for the monitor in NVIDIA's
    //    driver): FIFO, no tearing, no judder. A FreeSync / Adaptive-Sync monitor only its EDID declares is suggested by
    //    the Display line, not set.
    //  - HDR peak (no display.hdr_peak line yet, Windows HDR on): the monitor's, so HDR output is right once turned on.
    //    HDR output itself stays off: it expands the picture's highlights (present.c).
    // The Welcome screen then sets pc.welcome_done, so the player's own choices are never touched. No file: left alone.
    // Scripted boots (TOTK_PRESS / TOTK_CTL) get the frame rate only: their measurements must not follow the host's VRR.
    // The file's values are in g_kv before anything below can translate a text (first_run_display's monitor line,
    // pc_tr): the language's first use registers game.language (pc_lang.cpp), whose start value comes from g_kv. Until
    // 10-31 they came after: "[lang] es (Windows' display language es-ES)" with game.language = en-US).
    g_kv = kv;
    {
        bool welcomed = false, has_vrr = false, has_peak = false;
        int cap = -1;   // (an index: the pushes below may move kv)
        for (size_t i = 0; i < kv.size(); i++) {
            welcomed |= kv[i].first == "pc.welcome_done" && kv[i].second == "1";
            if (kv[i].first == "display.fps_cap") cap = (int)i;   // (the last one is the one start_value keeps)
            has_vrr |= kv[i].first == "display.vrr";
            has_peak |= kv[i].first == "display.hdr_peak";
        }
        const bool scripted = getenv("TOTK_PRESS") || getenv("TOTK_CTL");
        pcd_caps c;
        if (!welcomed && cap >= 0 && first_run_display(&c)) {
            // A new key goes into the lines too: pc_window.cpp's rows come after this start's save, which keeps a line
            // no row has yet (in the file from this start on, so the Options rows and settings.ini agree)
            auto set = [&](const char *k, const char *v, bool add) {
                printf("[pc-settings] first start: %s -> %s (the monitor's)\n", k, v);
                if (add) {
                    kv.push_back({k, v});
                    g_lines.push_back(std::string(k) + " = " + v);
                }
                g_dirty = true;
                g_dirty_at = GetTickCount64();
            };
            const bool vrr = !scripted && !has_vrr && c.vrr == PCD_VRR_ON;   // (MONITOR: suggested only)
            pcd_caps fixed = c;   // (VRR left off: the frame rate of a fixed refresh)
            if (!vrr) fixed.vrr = PCD_VRR_NONE;
            if (kv[cap].second == "120")
                if (const char *fps = first_run_fps_cap(fixed)) {
                    kv[cap].second = fps;
                    set("display.fps_cap", fps, false);
                }
            if (vrr) set("display.vrr", "on", true);
            if (!scripted && !has_peak && c.hdr_on && c.peak_nits > 0) {
                char p[16];
                snprintf(p, sizeof p, "%d", std::clamp((c.peak_nits + 25) / 50 * 50, 400, 4000));   // (the row's steps)
                set("display.hdr_peak", p, true);
            }
        }
    }
    g_kv = kv;
    for (Row &r : rows()) start_value(r);
    if (g_dirty) save_now();   // after an import: settings.ini exists from the first start on
}

void save_now() {
    std::lock_guard<std::mutex> lk(g_mu);   // (pc_settings_raw_set edits g_lines / g_kv from other threads: the Options rows)
    if (!g_file_on || g_path.empty()) { g_dirty = false; return; }
    std::vector<std::string> out;
    std::vector<bool> written(rows().size());
    auto line_for = [](const Row &r) {
        char b[256];
        snprintf(b, sizeof b, "%-34s = %-12s # %s", r.def->key, r.overridden ? r.saved : r.value, r.def->label);
        return std::string(b);
    };
    // (the header once, at the top of a file this writes first: g_lines may already hold the migration's marker)
    bool header = false;
    for (const std::string &l : g_lines) header |= l.rfind("# nxrecomp settings", 0) == 0;
    if (!header) {
        out.push_back("# nxrecomp settings for TotK on the engine (F10 in game). Read when the game starts.");
        out.push_back("# key = value; NX_SET_<KEY> overrides one for a run. Unknown lines are kept.");
    }
    for (const std::string &l : g_lines) {
        std::string k, v;
        int idx = -1;
        if (parse(l, &k, &v))
            for (size_t i = 0; i < rows().size(); i++) if (k == rows()[i].def->key) { idx = (int)i; break; }
        if (idx < 0) { out.push_back(l); continue; }   // comment, blank or unknown key: as it was
        if (!written[idx]) { out.push_back(line_for(rows()[idx])); written[idx] = true; }   // (a key twice: once)
    }
    for (size_t i = 0; i < rows().size(); i++) if (!written[i]) out.push_back(line_for(rows()[i]));
    std::string tmp = g_path + ".tmp";
    FILE *f = ufopen(tmp, L"w");
    if (!f) { printf("[pc-settings] can't write %s\n", tmp.c_str()); g_dirty = false; return; }
    for (const std::string &l : out) fprintf(f, "%s\n", l.c_str());
    bool ok = fclose(f) == 0 && MoveFileExW(widen(tmp).c_str(), widen(g_path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    printf(ok ? "[pc-settings] saved %s\n" : "[pc-settings] can't replace %s\n", g_path.c_str());
    g_lines = out;
    g_dirty = false;
}

}  // namespace

void add_rows(const pc_setting *defs, int n, bool placeholder) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < n; i++) {
        if (Row *old = find(defs[i].key)) {
            if (!old->placeholder || placeholder) {
                printf("[pc-settings] %s registered twice: the first row stays\n", defs[i].key);
                continue;
            }
            old->def = &defs[i];
            old->placeholder = old->overridden = false;
            snprintf(old->value, sizeof old->value, "%s", defs[i].def ? defs[i].def : "");
            memcpy(old->boot, old->value, sizeof old->boot);
            memcpy(old->saved, old->value, sizeof old->saved);
            if (g_booted) start_value(*old);
            printf("[pc-settings] %s: the row registered first replaces the overlay's\n", defs[i].key);
            continue;
        }
        Row r = {};
        r.placeholder = placeholder;
        r.def = &defs[i];
        snprintf(r.value, sizeof r.value, "%s", defs[i].def ? defs[i].def : "");
        memcpy(r.boot, r.value, sizeof r.boot);
        memcpy(r.saved, r.value, sizeof r.saved);
        rows().push_back(r);
        // A row added after the bridge (another TU's static initializer, or later): its start value now. Its variable
        // reaches whoever reads it after this; a reader that ran earlier keeps the default.
        if (g_booted) start_value(rows().back());
    }
}

extern "C" void pc_settings_add(const pc_setting *defs, int n) { add_rows(defs, n, false); }

extern "C" const char *pc_settings_get(const char *key) {
    Row *r = find(key);
    return r ? r->value : nullptr;
}

// A registered row's value copied under the lock (any thread: pc_settings_set may be rewriting it)
extern "C" bool pc_settings_copy(const char *key, char *out, size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    Row *r = find(key);
    if (!r) return false;
    if (n) snprintf(out, n, "%s", r->value);
    return true;
}

// A key's value with or without a registered row (pc_input.cpp: the per-context bindings controls.<ctx>.<action>,
// before the overlay has rows for them): NX_SET_<KEY>, else the file's line; false when neither.
extern "C" bool pc_settings_raw(const char *key, char *out, size_t n) {
    std::string v;
    bool have = false;
    if (const char *e = getenv(override_name(key).c_str())) { v = e; have = true; }
    else {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto &e : g_kv) if (e.first == key) { v = e.second; have = true; }
    }
    if (have && n) snprintf(out, n, "%s", v.c_str());
    return have;
}

extern "C" void pc_settings_raw_set(const char *key, const char *value) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        bool found = false;
        for (size_t i = g_kv.size(); i-- > 0;)
            if (g_kv[i].first == key) {
                if (value && !found) { g_kv[i].second = value; found = true; }
                else g_kv.erase(g_kv.begin() + (long)i);
            }
        if (value && !found) g_kv.push_back({key, value});
        bool line = false;   // the file's line for it: rewritten in place, removed, or added at the end
        for (size_t i = g_lines.size(); i-- > 0;) {
            std::string k, v;
            if (!parse(g_lines[i], &k, &v) || k != key) continue;
            if (value && !line) { g_lines[i] = std::string(key) + " = " + value; line = true; }
            else g_lines.erase(g_lines.begin() + (long)i);
        }
        if (value && !line) g_lines.push_back(std::string(key) + " = " + value);
        g_dirty = true;
        g_dirty_at = GetTickCount64();
    }
    printf("[pc-settings] %s %s%s\n", key, value ? "= " : "removed", value ? value : "");
}

extern "C" void pc_settings_set(const char *key, const char *value) {
    Row *r;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        r = find(key);
        if (!r || !value || !valid(r->def, value) || !strcmp(r->value, value)) return;
        snprintf(r->value, sizeof r->value, "%s", value);
        r->overridden = false;
        g_dirty = true;
        g_dirty_at = GetTickCount64();
    }
    printf("[pc-settings] %s = %s%s\n", key, value, r->def->flags & PC_SET_RESTART ? " (at the next start)" : "");
    if (r->def->apply) r->def->apply(r->value);
}

int pc_settings_count() { return (int)rows().size(); }

pc_setting_view pc_settings_at(int i) {
    const Row &r = rows()[i];
    return {r.def, r.value, (r.def->flags & PC_SET_RESTART) && strcmp(r.value, r.boot) != 0, r.overridden};
}

void pc_settings_tick(bool flush) {
    for (Row &r : rows())   // F11 / Alt+Enter change the window mode behind the overlay's back
        if (r.def->live_value) {
            const char *now = r.def->live_value();
            if (now && strcmp(now, r.value) && valid(r.def, now)) {
                std::lock_guard<std::mutex> lk(g_mu);
                snprintf(r.value, sizeof r.value, "%s", now);
                if (!r.overridden) { g_dirty = true; g_dirty_at = GetTickCount64(); }
            }
        }
    if (g_dirty && (flush || GetTickCount64() - g_dirty_at >= 500)) save_now();
}

// With the overlay closed (pc_overlay_build returns before pc_settings_tick): a change from elsewhere (the in-game
// Options rows' pc_bind_set) is saved too, 500 ms after it, without the overlay's per-frame row sync
void pc_settings_tick_save() {
    if (g_dirty && GetTickCount64() - g_dirty_at >= 500) save_now();
}
// shim_win.c's WM_CLOSE (the window thread): what is still waiting for the 500 ms, now (the game may be gone by then)
extern "C" void pc_settings_save_pending(void) {
    if (g_dirty) save_now();
}

const char *pc_settings_file() { return g_file_on && !g_path.empty() ? g_path.c_str() : nullptr; }
bool pc_settings_first_run() { return g_first_run || g_fresh_file; }

extern "C" void pc_settings_add_presets(const pc_preset_row *add, int n) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < n; i++) {
        bool replaced = false;
        for (pc_preset_row &p : g_presets)
            if (!strcmp(p.key, add[i].key)) { p = add[i]; replaced = true; }
        if (!replaced) g_presets.push_back(add[i]);
    }
}

static const char *const k_preset_names[PC_PRESETS] = {"low", "medium", "high", "ultra", "ultra+"};
int pc_settings_preset_index(const char *name) {
    for (int i = 0; i < PC_PRESETS; i++) if (name && !strcmp(name, k_preset_names[i])) return i;
    return -1;
}

namespace {
// Picking a preset sets its members (each through pc_settings_set: validated, applied, saved)
void preset_apply(const char *v) {
    const int idx = pc_settings_preset_index(v);
    if (idx < 0) return;   // custom: the rows stay as they are
    std::vector<pc_preset_row> p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = g_presets;
    }
    for (const pc_preset_row &r : p)
        if (r.value[idx] && find(r.key)) pc_settings_set(r.key, r.value[idx]);
}
// The preset the current values match, else custom (so any change by hand reads as custom)
const char *preset_now() {
    for (int idx = 0; idx < PC_PRESETS; idx++) {
        int members = 0;
        bool all = true;
        for (const pc_preset_row &r : g_presets) {
            Row *row = r.value[idx] ? find(r.key) : nullptr;
            if (!row) continue;
            members++;
            all &= !strcmp(row->value, r.value[idx]);
        }
        if (members && all) return k_preset_names[idx];
    }
    return "custom";
}
}  // namespace

// ================================================================== the engine's knobs
extern "C" bool shim_window_fullscreen(void);       // shim_win.c
extern "C" void hle_audio_set_master(float gain);   // hle_audio.cpp: the master gain, read at every mix
extern "C" void hle_audio_set_mute_unfocused(bool on);   // hle_audio.cpp: silent while the window is out of focus

namespace {

const char *window_mode_now() { return shim_window_fullscreen() ? "borderless" : "windowed"; }
void window_mode_apply(const char *v) {   // shim_win.c's WM_TOTK_FULLSCREEN (WM_APP + 2): apply_fullscreen on the window thread
    if (HWND h = (HWND)pc_overlay_hwnd()) PostMessageA(h, WM_APP + 2, strcmp(v, "borderless") == 0, 0);
}
void window_mode_env(const char *v) { env_put("TOTK_FULLSCREEN", strcmp(v, "borderless") == 0 ? "1" : "0"); }
void window_size_env(const char *v) {
    // LARECOMP_WINDOW also keeps the window from starting fullscreen (device.c ui_window_init): windowed only
    const char *m = pc_settings_get("display.window_mode");
    env_put("LARECOMP_WINDOW", m && strcmp(m, "windowed") == 0 ? v : "");
}
// Live: the window's client area to WxH ("" = the engine's 1280 x 720), windowed only. SWP_ASYNCWINDOWPOS: the window
// thread does it, the present thread does not wait; vk_frame_begin then follows the new size with a new swapchain.
void window_size_apply(const char *v) {
    HWND h = (HWND)pc_overlay_hwnd();
    const char *m = pc_settings_get("display.window_mode");
    int w = 1280, ht = 720;
    if (!h || !m || strcmp(m, "windowed") || (*v && sscanf(v, "%dx%d", &w, &ht) != 2)) return;
    RECT r = {0, 0, w, ht};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}
const char *perf_now() { return pc_overlay_perf() ? "1" : "0"; }   // F9 toggles it behind the overlay's back
void perf_apply(const char *v) { pc_overlay_set_perf(strcmp(v, "1") == 0); }
void mute_unfocused_apply(const char *v) { hle_audio_set_mute_unfocused(!strcmp(v, "1")); }
extern "C" void pc_shot_set_enabled(bool on);   // pc_shot.cpp
// anti-aliasing, the window upscaler and sharpening act from the next presented frame (exec.c; nullptr keeps that
// one as it is; the values are their variables')
extern "C" void renderer_apply_post(const char *aa, const char *upscale, const char *sharpen);
void aa_apply(const char *v) { renderer_apply_post(v, nullptr, nullptr); }
void upscale_apply(const char *v) { renderer_apply_post(nullptr, v, nullptr); }
void sharpen_apply(const char *v) { renderer_apply_post(nullptr, nullptr, v); }
void screenshots_apply(const char *v) { pc_shot_set_enabled(strcmp(v, "0") != 0); }
extern "C" void shim_set_fps_overlay(int mode);   // shim_win.c / shim_sdl3.c: -1 automatic (fullscreen), 0 off, 1 on
extern "C" int shim_fps_overlay_mode(void);
void fps_readout_apply(const char *v) { shim_set_fps_overlay(!*v ? -1 : strcmp(v, "0") != 0); }
const char *fps_readout_now() {   // F3 toggles it behind the overlay's back
    const int m = shim_fps_overlay_mode();
    return m < 0 ? "" : m ? "1" : "0";
}
// crash reports and the backups kept follow a change while the game runs. Only a change: apply never runs at start
// (pc_settings_set alone calls it), where the variables stay in charge, since pc_crash_install at static init would
// come ahead of rt_main's install point
extern "C" void pc_crash_set_enabled(bool on);   // pc_crash.cpp (pc.h)
extern "C" void hle_fs_set_save_backups(int n);  // hle_fs.cpp: from the next save commit (0..1000; 0 drops them all there)
void crash_reports_apply(const char *v) { pc_crash_set_enabled(strcmp(v, "0") != 0); }
void save_backups_apply(const char *v) { hle_fs_set_save_backups(atoi(v)); }
void volume_apply(const char *v) { hle_audio_set_master((float)atof(v)); }   // (one aligned float: a mix sees the old or the new)
// textures.c: TOTK_OUT_SCALE, on by default (unset). The default leaves the variable alone, so a TOTK_OUT_SCALE=0 / 1
// given at start (tests, A/B boots) is not undone by a settings file that holds the default row; Off writes the 0.
void native_ui_env(const char *v) { if (*v) env_put("TOTK_OUT_SCALE", v); }
void fps_env(const char *v) {   // hle_vi.cpp: TOTK_FPS (unset: the game's own 30) and TOTK_UNLOCK_FPS (no pacing)
    // "steady": unlocked, paced at a rate the frames hold (hle_vi.cpp steady_fps, TOTK_UNLOCK_STEADY)
    const bool steady = strcmp(v, "steady") == 0, unlocked = steady || strcmp(v, "unlocked") == 0, own = strcmp(v, "30") == 0;
    env_put("TOTK_FPS", unlocked || own ? "" : v);
    env_put("TOTK_UNLOCK_FPS", unlocked ? "1" : "");
    const char *st = getenv("TOTK_UNLOCK_STEADY");   // (steady keeps a set "vrr": its ladder of 10 % steps)
    env_put("TOTK_UNLOCK_STEADY", !steady ? "" : st && !strcmp(st, "vrr") ? "vrr" : "1");
}
extern "C" void vi_set_frame_rate(const char *v);   // hle_vi.cpp: the pacing reads it per frame
void fps_apply(const char *v) { vi_set_frame_rate(v); }  // (live since 2026-10-27)

const char *const V_WINDOW[] = {"windowed", "borderless", nullptr};
const char *const N_WINDOW[] = {"Windowed", "Borderless fullscreen", nullptr};
const char *const V_SIZE[] = {"", "1600x900", "1920x1080", "2560x1440", "3840x2160", nullptr};
const char *const N_SIZE[] = {"1280 x 720 (default)", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", nullptr};
// "unlocked": present's unlocked mode (hle_vi.cpp unlock_mode 1, 17b9f4d: paced up to the monitor's rate, 60..240, or
// TOTK_UNLOCK_CEIL), never "raw"
const char *const V_FPS[] = {"30", "60", "90", "120", "unlocked", "steady", nullptr};
const char *const N_FPS[] = {"30 (the game's own)", "60", "90", "120", "Unlocked (monitor rate)",
                             "Unlocked, steady (a rate it can hold)", nullptr};
const char *const V_FPSOSD[] = {"", "1", "0", nullptr};
const char *const N_FPSOSD[] = {"In fullscreen (default)", "Always", "Never", nullptr};
const char *const V_SCALE[] = {"0.67", "0.8", "0.9", "1", "1.5", "2", "auto", nullptr};
const char *const N_SCALE[] = {"67 %", "80 %", "90 %", "100 % (native)", "150 %", "200 %", "Follow the window height", nullptr};
const char *const V_NATIVE_UI[] = {"", "0", nullptr};
const char *const N_NATIVE_UI[] = {"On (default)", "Off", nullptr};
const char *const V_OFFON[] = {"0", "1", nullptr};
// Dynamic resolution as one row. The row writes both; its value is read back from them (a preset, F11 or an old
// settings file moves them), the floor to the nearest step. The floor is live; the clock's aim (TOTK_DYNRES) takes the
// next start, as before
const char *const V_DYNRES[] = {"off", "90", "75", "50", "game", nullptr};
const char *const N_DYNRES[] = {"Off (fixed size)", "On, never below 90 %", "On, never below 75 %", "On, never below 50 %",
                                "On, the game's own (no minimum)", nullptr};
void dynres_apply(const char *v) {
    static const struct { const char *v, *floor, *dynres; } k[] = {
        {"off", "1", "0"}, {"90", "0.9", "1"}, {"75", "0.75", "1"}, {"50", "0.5", "1"}, {"game", "0", "1"}};
    for (const auto &e : k)
        if (v && !strcmp(v, e.v)) {
            pc_settings_set("graphics.min_resolution", e.floor);
            pc_settings_set("graphics.dynres", e.dynres);
        }
}
const char *dynres_now() {
    const char *f = pc_settings_get("graphics.min_resolution");
    const double x = f && *f ? atof(f) : 1.0;
    if (x >= 0.995) return "off";
    if (x < 0.01) return "game";
    return x >= 0.825 ? "90" : x >= 0.625 ? "75" : "50";
}
const char *const N_OFFON[] = {"Off", "On", nullptr};
const char *const V_SSAA[] = {"", "fetch", nullptr};
const char *const N_SSAA[] = {"Bicubic (recomp)", "FSR 1 (the game's)", nullptr};
const char *const V_UPSCALE[] = {"", "fsr", nullptr};
const char *const N_UPSCALE[] = {"Bilinear", "FSR 1 (window above 1080p)", nullptr};
const char *const V_AA[] = {"", "smaa", nullptr};
const char *const N_AA[] = {"Off", "SMAA 1x", nullptr};
const char *const V_SHARPEN[] = {"", "1", "0.5", "0.2", nullptr};
const char *const N_SHARPEN[] = {"Off", "RCAS low", "RCAS medium", "RCAS high", nullptr};
const char *const V_SHADOW[] = {"1", "2", "3", "4", nullptr};
const char *const N_SHADOW[] = {"1x (Switch)", "2x (default)", "3x", "4x", nullptr};
const char *const V_LABELS[] = {"", "1", nullptr};
const char *const N_LABELS[] = {"By position (Switch layout)", "By label (Xbox A = A)", nullptr};
const char *const V_RUMBLE[] = {"0", "1", "2", "3", "4", nullptr};
const char *const N_RUMBLE[] = {"Off", "Normal", "x2", "x3", "x4", nullptr};
const char *const V_GYRO[] = {"", "0", nullptr};
const char *const N_GYRO[] = {"On", "Off", nullptr};
const char *const V_SKIP[] = {"", "1", nullptr};
const char *const N_SKIP[] = {"Off", "On (start at the Great Sky Island)", nullptr};
const char *const V_MODE[] = {"", "1", nullptr};
const char *const N_MODE[] = {"Docked", "Handheld", nullptr};

const unsigned R = PC_SET_RESTART;
const char *const V_PRESET[] = {"low", "medium", "high", "ultra", "ultra+", "custom", nullptr};
const char *const N_PRESET[] = {"Low", "Medium", "High", "Ultra", "Ultra+", "Custom", nullptr};
const pc_setting k_engine[] = {
    {"graphics.preset", "Graphics", "Preset", "Graphics preset", "Sets the rows below it; any change by hand makes it "
     "Custom.", "custom", V_PRESET, N_PRESET, 0, 0, 0, 0, nullptr, nullptr, preset_apply, preset_now},
    {"pc.welcome_done", "", "", "First-run screen done", nullptr, "0", V_OFFON, N_OFFON, 0, 0, 0, 0, nullptr, nullptr,
     nullptr, nullptr},   // (no page: not shown)
    // key, page, section, label, help, default, values, names, min, max, step, flags, env, to_env, apply, live_value
    {"display.window_mode", "Graphics", "Display", "Window mode", "F11 or Alt+Enter also switch it.", "borderless",
     V_WINDOW, N_WINDOW, 0, 0, 0, 0, nullptr, window_mode_env, window_mode_apply, window_mode_now},
    {"display.window_size", "Graphics", "Display", "Window size", "Windowed mode only (LARECOMP_WINDOW at start).", "",
     V_SIZE, N_SIZE, 0, 0, 0, 0, nullptr, window_size_env, window_size_apply, nullptr},
    {"display.fps_cap", "Graphics", "Display", "Frame rate", "TOTK_FPS / TOTK_UNLOCK_FPS. Unlocked: up to the monitor's "
     "refresh rate (60 to 240), loading screens at the game's own pace. Steady: unlocked, but held at the highest rate "
     "the scene keeps, in whole refreshes of the monitor (240, 120, 80, 60 on a 240 Hz one), instead of swinging.", "30", V_FPS, N_FPS, 0, 0, 0, 0, nullptr, fps_env, fps_apply, nullptr},
    {"display.perf_overlay", "Graphics", "Display", "Perf overlay (F9)", "Fps, 1% low and the frame-time graph.", "0",
     V_OFFON, N_OFFON, 0, 0, 0, 0, nullptr, perf_apply, perf_apply, perf_now},
    {"display.fps_readout", "Graphics", "Display", "Fps readout (F3)", "TOTK_FPS_OVERLAY. F3 toggles it in game.", "",
     V_FPSOSD, N_FPSOSD, 0, 0, 0, 0, "TOTK_FPS_OVERLAY", nullptr, fps_readout_apply, fps_readout_now},
    {"graphics.render_scale", "Graphics", "Rendering", "Render resolution", "TOTK_RES_SCALE, of the game's 1600 x 900.",
     "1", V_SCALE, N_SCALE, 0, 0, 0, R, "TOTK_RES_SCALE", nullptr, nullptr, nullptr},
    {"graphics.native_ui", "Graphics", "Rendering", "Native-resolution interface", "TOTK_OUT_SCALE: above 1080p, menus, "
     "text and the HUD drawn at the window's resolution instead of the game's 1920 x 1080 stretched.", "", V_NATIVE_UI,
     N_NATIVE_UI, 0, 0, 0, R, nullptr, native_ui_env, nullptr, nullptr},
    {"graphics.dynamic_resolution", "Graphics", "Rendering", "Dynamic resolution", "Off: the scene always at the render "
     "resolution. On: the game lowers it only when the GPU is short of time, never below the chosen floor, aiming at the "
     "frame rate (that aim takes effect at the next start).", "off", V_DYNRES, N_DYNRES, 0, 0, 0, 0, nullptr, nullptr,
     dynres_apply, dynres_now},
    // (internal since graphics.dynamic_resolution: no page)
    {"graphics.dynres", "", "Rendering", "Dynamic resolution (clock)", "TOTK_DYNRES: the game's own, aimed at the frame "
     "rate.", "0", V_OFFON, N_OFFON, 0, 0, 0, R, "TOTK_DYNRES", nullptr, nullptr, nullptr},
    {"graphics.upscaler_3d", "Graphics", "Rendering", "3D upscaler", "TOTK_SSAA.", "", V_SSAA, N_SSAA, 0, 0, 0, R,
     "TOTK_SSAA", nullptr, nullptr, nullptr},
    {"graphics.window_upscaler", "Graphics", "Rendering", "Window upscaler", "TOTK_UPSCALE.", "", V_UPSCALE, N_UPSCALE,
     0, 0, 0, 0, "TOTK_UPSCALE", nullptr, upscale_apply, nullptr},
    {"graphics.antialiasing", "Graphics", "Rendering", "Anti-aliasing", "TOTK_AA, on the presented image.", "", V_AA,
     N_AA, 0, 0, 0, 0, "TOTK_AA", nullptr, aa_apply, nullptr},
    {"graphics.sharpening", "Graphics", "Rendering", "Sharpening", "TOTK_SHARPEN.", "", V_SHARPEN, N_SHARPEN, 0, 0, 0,
     0, "TOTK_SHARPEN", nullptr, sharpen_apply, nullptr},
    {"graphics.shadow_scale", "Graphics", "Rendering", "Shadow map scale (renderer)", "TOTK_SHADOW_SCALE: the renderer's "
     "shadow targets. The game patches' Shadow resolution is separate.", "2", V_SHADOW,
     N_SHADOW, 0, 0, 0, R, "TOTK_SHADOW_SCALE", nullptr, nullptr, nullptr},
    {"audio.volume", "Audio", "Output", "Volume", "TOTK_VOLUME, master gain.", "1", nullptr, nullptr, 0.0f, 1.0f, 0.05f,
     0, "TOTK_VOLUME", nullptr, volume_apply, nullptr},
    {"audio.mute_unfocused", "Audio", "Output", "Mute in the background", "TOTK_MUTE_UNFOCUSED: no sound while the game "
     "window is not in front; the game runs on (Pause when the window loses focus stops it too).", "0", V_OFFON, N_OFFON,
     0, 0, 0, 0, "TOTK_MUTE_UNFOCUSED", nullptr, mute_unfocused_apply, nullptr},
    {"controls.pad_labels", "Controls", "Pad", "Face buttons", "TOTK_PAD_LABELS.", "", V_LABELS, N_LABELS, 0, 0, 0, R,
     "TOTK_PAD_LABELS", nullptr, nullptr, nullptr},
    {"controls.rumble", "Controls", "Pad", "Rumble", "TOTK_RUMBLE.", "1", V_RUMBLE, N_RUMBLE, 0, 0, 0, R, "TOTK_RUMBLE",
     nullptr, nullptr, nullptr},
    {"controls.gyro", "Controls", "Pad", "Motion controls", "TOTK_GYRO.", "", V_GYRO, N_GYRO, 0, 0, 0, R, "TOTK_GYRO",
     nullptr, nullptr, nullptr},
    {"game.skip_opening", "Game", "Start", "Skip the opening", "TOTK_SKIP_OPENING. F2 or Minus on the title screen "
     "also toggles it.", "", V_SKIP, N_SKIP, 0, 0, 0, R, "TOTK_SKIP_OPENING", nullptr, nullptr, nullptr},
    {"game.console_mode", "Game", "Start", "Console mode", "TOTK_HANDHELD: handheld renders at the lower resolution.", "",
     V_MODE, N_MODE, 0, 0, 0, R, "TOTK_HANDHELD", nullptr, nullptr, nullptr},
};

// The start-up bridge, once the engine's rows are in (other areas' rows added later get theirs in pc_settings_add)
// Our other workstreams' knobs on the engine (each read once at start, so restart rows). Not here:
// TOTK_MOUSE_CAMERA, which pc_mouse.cpp reads as on / off and the game patches' controls.mouse_camera_scale row
// writes as a 0-4 multiplier (reported).
const char *const V_LAYOUT[] = {"classic", "pc", "custom", nullptr};
const char *const N_LAYOUT[] = {"Classic", "PC (WASD)", "Custom (TOTK_KBM_<ACTION>)", nullptr};
const char *const V_ON_DEF[] = {"", "0", nullptr};   // variables that are on unless "0"
const char *const N_ON_DEF[] = {"On", "Off", nullptr};
const char *const V_OFF_DEF[] = {"", "1", nullptr};  // variables that are off unless "1"
const char *const N_OFF_DEF[] = {"Off", "On", nullptr};
const char *const V_GYROMODE[] = {"aim", "always", "off", nullptr};
const char *const N_GYROMODE[] = {"While aiming", "Always", "Off", nullptr};
const char *const V_ANISO[] = {"0", "2", "4", "8", "16", nullptr};
const char *const N_ANISO[] = {"The game's", "2x", "4x", "8x", "16x", nullptr};
const char *const V_SCENEUP[] = {"", "easu", nullptr};
const char *const N_SCENEUP[] = {"Catmull-Rom", "FSR 1 EASU", nullptr};
const char *const V_BACKUPS[] = {"0", "3", "10", "30", nullptr};
const char *const N_BACKUPS[] = {"None", "3", "10 (default)", "30", nullptr};
const pc_setting k_pc[] = {
    {"controls.keyboard_layout", "Controls", "Keyboard and mouse", "Keyboard layout", "TOTK_KEYBOARD_LAYOUT.", "classic",
     V_LAYOUT, N_LAYOUT, 0, 0, 0, R, "TOTK_KEYBOARD_LAYOUT", nullptr, nullptr, nullptr},
    {"controls.mouse_aim", "Controls", "Keyboard and mouse", "Mouse buttons aim and shoot", "TOTK_MOUSE_AIM.", "",
     V_ON_DEF, N_ON_DEF, 0, 0, 0, R, "TOTK_MOUSE_AIM", nullptr, nullptr, nullptr},
    {"controls.mouse_sensitivity", "Controls", "Keyboard and mouse", "Mouse sensitivity", "TOTK_MOUSE_SENS.", "1",
     nullptr, nullptr, 0.1f, 20.0f, 0.1f, R, "TOTK_MOUSE_SENS", nullptr, nullptr, nullptr},
    {"controls.mouse_stick_speed", "Controls", "Keyboard and mouse", "Mouse counts for a full stick", "TOTK_MOUSE_STICK_SPEED "
     "(per second).", "1500", nullptr, nullptr, 50.0f, 5000.0f, 50.0f, R, "TOTK_MOUSE_STICK_SPEED", nullptr, nullptr, nullptr},
    {"controls.mouse_invert_y", "Controls", "Keyboard and mouse", "Invert mouse vertical", "TOTK_MOUSE_INVERT.", "",
     V_OFF_DEF, N_OFF_DEF, 0, 0, 0, R, "TOTK_MOUSE_INVERT", nullptr, nullptr, nullptr},
    {"controls.mouse_gyro", "Controls", "Keyboard and mouse", "Mouse as motion aim", "TOTK_MOUSE_GYRO.", "off",
     V_GYROMODE, N_GYROMODE, 0, 0, 0, R, "TOTK_MOUSE_GYRO", nullptr, nullptr, nullptr},
    {"controls.mouse_gyro_speed", "Controls", "Keyboard and mouse", "Motion aim speed", "TOTK_MOUSE_GYRO_SPEED.", "1",
     nullptr, nullptr, 0.05f, 4.0f, 0.05f, R, "TOTK_MOUSE_GYRO_SPEED", nullptr, nullptr, nullptr},
    {"graphics.anisotropy", "Graphics", "Rendering", "Anisotropic filtering", "TOTK_ANISO: a floor under the game's.", "0",
     V_ANISO, N_ANISO, 0, 0, 0, R, "TOTK_ANISO", nullptr, nullptr, nullptr},
    {"graphics.lod_bias", "Graphics", "Rendering", "Texture LOD bias", "TOTK_LOD_BIAS: below 0 is sharper.", "0", nullptr,
     nullptr, -2.0f, 2.0f, 0.25f, R, "TOTK_LOD_BIAS", nullptr, nullptr, nullptr},
    {"graphics.scene_upscaler", "Graphics", "Rendering", "Scene upscaler", "TOTK_SCENE_UPSCALE.", "", V_SCENEUP, N_SCENEUP,
     0, 0, 0, R, "TOTK_SCENE_UPSCALE", nullptr, nullptr, nullptr},
    {"game.pause_unfocused", "Game", "Window", "Pause when the window loses focus", "TOTK_PAUSE_UNFOCUSED.", "", V_OFF_DEF,
     N_OFF_DEF, 0, 0, 0, R, "TOTK_PAUSE_UNFOCUSED", nullptr, nullptr, nullptr},
    {"game.save_backups", "Game", "Saves", "Save backups kept", "TOTK_SAVE_BACKUPS.", "10", V_BACKUPS, N_BACKUPS, 0, 0, 0, 0,
     "TOTK_SAVE_BACKUPS", nullptr, save_backups_apply, nullptr},
    {"game.screenshots", "Game", "PC", "F12 screenshots", "TOTK_PC_SHOT: PNGs in the Screenshots folder.", "1", V_OFFON,
     N_OFFON, 0, 0, 0, 0, "TOTK_PC_SHOT", nullptr, screenshots_apply, nullptr},
    {"game.crash_reports", "Game", "PC", "Crash reports", "TOTK_PC_CRASH: a report and a minidump per crash.", "1",
     V_OFFON, N_OFFON, 0, 0, 0, 0, "TOTK_PC_CRASH", nullptr, crash_reports_apply, nullptr},
};

// Key bindings of the Custom layout: TOTK_KBM_<ACTION> (hle_hid.cpp bindings()), read once at start
#define KBM(key, label, act) {"controls.kbm." key, "Controls", "Key bindings (Custom layout)", label, \
    "TOTK_KBM_" act ". Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's " \
    "keys.", "", nullptr, nullptr, 0, 0, 0, R | PC_SET_KEYS, "TOTK_KBM_" act, \
    nullptr, nullptr, nullptr}
const pc_setting k_kbm[] = {
    KBM("a", "A", "A"), KBM("b", "B", "B"), KBM("x", "X", "X"), KBM("y", "Y", "Y"),
    KBM("lstick", "Left stick click", "LSTICK"), KBM("rstick", "Right stick click", "RSTICK"), KBM("l", "L", "L"),
    KBM("r", "R", "R"), KBM("zl", "ZL", "ZL"), KBM("zr", "ZR", "ZR"), KBM("plus", "+", "PLUS"), KBM("minus", "-", "MINUS"),
    KBM("dpad_left", "D-pad left", "DPAD_LEFT"), KBM("dpad_up", "D-pad up", "DPAD_UP"),
    KBM("dpad_right", "D-pad right", "DPAD_RIGHT"), KBM("dpad_down", "D-pad down", "DPAD_DOWN"),
    KBM("move_left", "Move left", "MOVE_LEFT"), KBM("move_up", "Move forward", "MOVE_UP"),
    KBM("move_right", "Move right", "MOVE_RIGHT"), KBM("move_down", "Move back", "MOVE_DOWN"),
    KBM("camera_left", "Camera left", "CAMERA_LEFT"), KBM("camera_up", "Camera up", "CAMERA_UP"),
    KBM("camera_right", "Camera right", "CAMERA_RIGHT"), KBM("camera_down", "Camera down", "CAMERA_DOWN"),
    KBM("walk", "Walk (hold)", "WALK"),
};
#undef KBM

// Presets: low, medium, high, ultra, ultra+ (beyond the game's own values; not offered by the first-run screen). The
// engine's rows here; the game-patch rows from the own table (totk_quality_presets(): the patch rows, with the field
// of view, fog and simulation distance left to the player). Rows that are not registered are skipped.
const pc_preset_row k_presets[] = {
    {"graphics.render_scale", {"0.8", "1", "auto", "auto", "auto"}},
    {"graphics.dynres", {"1", "0", "0", "0", "0"}},
    {"graphics.shadow_scale", {"1", "2", "2", "3", "4"}},
    {"graphics.anisotropy", {"0", "4", "8", "16", "16"}},
    {"graphics.antialiasing", {"", "", "smaa", "smaa", "smaa"}},
};

// totk_quality_presets() (Low .. Ultra+, each a list of key / value) -> one pc_preset_row per key
void add_game_presets() {
    static std::vector<pc_preset_row> rows;
    unsigned n = 0;
    const TotkQualityPreset *p = totk_quality_presets(&n);
    for (unsigned i = 0; p && i < n; i++) {
        std::string name = p[i].name ? p[i].name : "";
        for (char &c : name) c = (char)tolower((unsigned char)c);
        const int idx = pc_settings_preset_index(name.c_str());
        if (idx < 0) continue;
        for (unsigned j = 0; j < p[i].count; j++) {
            pc_preset_row *r = nullptr;
            for (pc_preset_row &x : rows) if (!strcmp(x.key, p[i].values[j].key)) r = &x;
            if (!r) { rows.push_back({p[i].values[j].key, {}}); r = &rows.back(); }
            r->value[idx] = p[i].values[j].value;
        }
    }
    pc_settings_add_presets(rows.data(), (int)rows.size());   // (copied: rows may move later without harm)
}

struct AddEngine {
    AddEngine() {
        pc_settings_add_presets(k_presets, (int)(sizeof k_presets / sizeof k_presets[0]));
        add_game_presets();
        pc_settings_add(k_engine, (int)(sizeof k_engine / sizeof k_engine[0]));
        // Stand-ins for knobs other workstreams own (present: window mode and size, its vsync / VRR rows are only in
        // pc_window.cpp; audio: volume: k_pc, k_kbm). (the controls.rumble and controls.gyro: hle_hid.cpp's live rows)
        // Only our own rows, "borderless").
        const pc_setting *const own = k_engine, *const own_end = k_engine + sizeof k_engine / sizeof k_engine[0];
        for (const char *k : {"display.window_mode", "display.window_size", "audio.volume",
                              "controls.rumble", "controls.gyro"})
            if (Row *r = find(k); r && r->def >= own && r->def < own_end) r->placeholder = true;
        add_rows(k_pc, (int)(sizeof k_pc / sizeof k_pc[0]), true);
        add_rows(k_kbm, (int)(sizeof k_kbm / sizeof k_kbm[0]), true);
        boot();
    }
} g_add_engine;

}  // namespace
