// On-screen text for runtime options (user 2026-10-15: "add a thing in the totk menu to skip the opening under hyrule
// castle sequence"). GDI renders the banner into a BGRA block; device.c (vk_frame_end) copies it into the window image
// before present, bottom left. Only the render thread calls osd_banner().
// Linux: the same blocks rendered with stb_truetype (runtime/linux/stb_truetype.h, public domain) from a TrueType font file:
// $TOTK_FONT, <exe dir>/data/fonts/DejaVuSans-Bold.ttf, then the usual system font directories; no font = no on-screen text.
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include "pc_text.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "linux/stb_truetype.h"
#include "linux/plat_posix.h"
typedef uint32_t COLORREF;                                                       // 0x00BBGGRR like GDI
#define RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#endif

extern "C" volatile int g_skip_opening, g_title_phase, g_in_opening, g_skip_hold_ms;   // hle_fs.cpp, hle_hid.cpp
extern "C" volatile uint64_t g_in_opening_since;
extern "C" uint64_t nvn_present_count(void);
extern "C" int vk_precompile_recorded_progress(int* done, int* total);   // exec.c shader precompile (recorded phase)
extern "C" int vk_precompile_warmup_progress(int* done, int* total);     // exec.c, TOTK_WARMUP only
extern "C" uint64_t vk_scene_idle_frames(void);                           // exec.c (pause screens)
extern "C" int osd_menu_is_open(void);                                    // below (Graphics menu)
#include "pc_overlay.h"                                                   // the PC overlay replaces that menu

// one rendered text block (banner, fps readout)
struct OsdBlock { std::vector<uint32_t> px; int w = 0, h = 0, font_h = 0; std::string text; };
static OsdBlock g_banner, g_fps;

#ifdef _WIN32
static void render(OsdBlock& o, const std::string& t, int font_h, COLORREF fg) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = CreateFontA(-font_h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = {};
    std::wstring wt(t.size() + 1, L'\0');   // (UTF-8: the PC texts are, pc_text.h; ASCII stays as it was)
    wt.resize((size_t)std::max(0, MultiByteToWideChar(CP_UTF8, 0, t.c_str(), (int)t.size(), wt.data(), (int)wt.size())));
    GetTextExtentPoint32W(dc, wt.c_str(), (int)wt.size(), &sz);
    int pad = font_h / 2, w = sz.cx + 2 * pad, h = sz.cy + pad;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    HBRUSH br = CreateSolidBrush(RGB(12, 14, 16));
    FillRect(dc, &r, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    TextOutW(dc, pad, pad / 2, wt.c_str(), (int)wt.size());
    GdiFlush();
    o.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * h);
    for (auto& p : o.px) p |= 0xff000000u;   // GDI leaves alpha 0
    o.w = w; o.h = h;
    SelectObject(dc, ob); SelectObject(dc, of);
    DeleteObject(bm); DeleteObject(f); DeleteDC(dc);
}
#else
static bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out->data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}
// TOTK_FONT, then data/fonts next to the executable, then system fonts (SteamOS ships DejaVu / Noto / Liberation)
static bool load_font(stbtt_fontinfo* fi, std::vector<unsigned char>* data) {
    std::vector<std::string> cand;
    if (const char* e = getenv("TOTK_FONT")) cand.push_back(e);
    std::string ex = rt_posix_exe_dir();
    for (const char* n : {"DejaVuSans-Bold.ttf", "DejaVuSans.ttf"}) { cand.push_back(ex + "/data/fonts/" + n); cand.push_back(ex + "/" + n); }
    for (const char* d : {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/"})
        for (const char* n : {"DejaVuSans-Bold.ttf", "DejaVuSans.ttf"}) cand.push_back(std::string(d) + n);
    for (const char* p : {"/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                          "/usr/share/fonts/TTF/LiberationSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"})
        cand.push_back(p);
    for (auto& c : cand)
        if (read_file(c, data) && stbtt_InitFont(fi, data->data(), stbtt_GetFontOffsetForIndex(data->data(), 0))) {
            printf("[osd] font %s\n", c.c_str());
            return true;
        }
    printf("[osd] no TrueType font found (set TOTK_FONT or put DejaVuSans-Bold.ttf in data/fonts): no on-screen text\n");
    return false;
}
// Same block layout as the GDI version: pad = font_h / 2 around the text, background RGB(12, 14, 16), BGRA with alpha 255.
// UTF-8 -> code points (a malformed byte stands for itself)
static std::vector<int> code_points(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned c = (unsigned char)s[i];
        int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (i + n >= s.size() + (n ? 0 : 1) || n == 0) { out.push_back((int)c); i++; continue; }
        unsigned cp = c & (0x3f >> n);
        for (int k = 1; k <= n; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3f);
        out.push_back((int)cp);
        i += n + 1;
    }
    return out;
}
static void render(OsdBlock& o, const std::string& ts, int font_h, COLORREF fg) {
    const std::vector<int> t = code_points(ts);
    static stbtt_fontinfo fi; static std::vector<unsigned char> data; static int state = 0;   // 0 untried, 1 loaded, -1 none
    if (!state) state = load_font(&fi, &data) ? 1 : -1;
    o.px.clear(); o.w = o.h = 0;
    if (state < 0) return;
    float sc = stbtt_ScaleForMappingEmToPixels(&fi, (float)font_h);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    int ascent = (int)ceilf(asc * sc), descent = (int)ceilf(-desc * sc);
    float width = 0;
    for (size_t i = 0; i < t.size(); i++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, t[i], &adv, &lsb);
        width += adv * sc;
        if (i + 1 < t.size()) width += stbtt_GetCodepointKernAdvance(&fi, t[i], t[i + 1]) * sc;
    }
    int pad = font_h / 2, w = (int)ceilf(width) + 2 * pad, h = ascent + descent + pad;
    const uint32_t bg = 0xff000000u | (12u << 16) | (14u << 8) | 16u;
    o.px.assign((size_t)w * h, bg);
    int fr = fg & 255, fgc = fg >> 8 & 255, fb = fg >> 16 & 255;
    float pen = (float)pad;
    int base = pad / 2 + ascent;
    std::vector<unsigned char> bm;
    for (size_t i = 0; i < t.size(); i++) {
        int cp = t[i], x0, y0, x1, y1, adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb);
        float fx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&fi, cp, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            bm.assign((size_t)gw * gh, 0);
            stbtt_MakeCodepointBitmapSubpixel(&fi, bm.data(), gw, gh, gw, sc, sc, fx, 0, cp);
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    int a = bm[(size_t)y * gw + x], px = (int)floorf(pen) + x0 + x, py = base + y0 + y;
                    if (!a || px < 0 || px >= w || py < 0 || py >= h) continue;
                    uint32_t& d = o.px[(size_t)py * w + px];
                    int dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
                    d = 0xff000000u | (uint32_t)((dr * (255 - a) + fr * a) / 255) << 16 | (uint32_t)((dg * (255 - a) + fgc * a) / 255) << 8 |
                        (uint32_t)((db * (255 - a) + fb * a) / 255);
                }
        }
        pen += adv * sc;
        if (i + 1 < t.size()) pen += stbtt_GetCodepointKernAdvance(&fi, cp, t[i + 1]) * sc;
    }
    o.w = w; o.h = h;
}
#endif

// TOTK_GFX_MENU=0 (the launcher's default, 2026-10-20: settings live in the launcher, the game only plays): no L3 + R3 / F10
// menu and no banner hint for it.
static bool osd_menu_enabled() {
    static const bool on = !(getenv("TOTK_GFX_MENU") && getenv("TOTK_GFX_MENU")[0] == '0');
    return on;
}
static bool g_hint_held;                                   // osd_banner (render thread): a held picture's hint timer
static std::chrono::steady_clock::time_point g_hint_since;
// A short notice over the banner (pc_shot.cpp's "Screenshot saved"), from any thread; osd_banner shows it for `ms`.
static std::mutex g_toast_mu;
static std::string g_toast;
static std::chrono::steady_clock::time_point g_toast_until;
extern "C" void osd_toast(const char* text, int ms) {
    std::lock_guard<std::mutex> l(g_toast_mu);
    g_toast = text ? text : "";
    g_toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}
// Another notice is up (a different text, still showing): a refreshed hint waits, so it never cuts F12's "Screenshot
// saved"
extern "C" int osd_toast_busy(const char* text) {
    std::lock_guard<std::mutex> l(g_toast_mu);
    return std::chrono::steady_clock::now() < g_toast_until && g_toast != (text ? text : "");
}
// The banner for a window image target_h pixels tall, or null when nothing is shown. BGRA, rows of *w pixels.
extern "C" const uint32_t* osd_banner(int target_h, int* w, int* h) {
    static const bool off = getenv("TOTK_OSD") && getenv("TOTK_OSD")[0] == '0';
    std::string t;
    bool toast = false;
    if (g_in_opening) {   // in-game skip (hle_hid.cpp skip_restart): hint for the first 20 s in the opening, countdown while Minus is held
        if (g_skip_hold_ms > 0) {
            char b[96];
            snprintf(b, sizeof b, pc_tr("Skipping to Great Sky Island in %.1f s... (release Minus to cancel)"), (1500 - g_skip_hold_ms) / 1000.0);
            t = b;
        } else if (nvn_present_count() - g_in_opening_since < 600) {
            t = pc_tr("Skip to Great Sky Island: F2 or hold Minus (restarts into a new game)");
        }
    } else if (g_title_phase) {
        t = g_skip_opening ? pc_tr("Skip opening: ON   (F2 / Minus to change)") : pc_tr("Skip opening: OFF   (F2 / Minus to change)");
    } else if (osd_menu_enabled() && !osd_menu_is_open() && vk_scene_idle_frames() > 45) {   // a pause / System menu holds the picture
        // With the PC overlay: the first 4 s of each such screen only (it sat on every map and inventory screen), and
        // not while the overlay is open
        const auto now = std::chrono::steady_clock::now();
        if (!g_hint_held) g_hint_since = now;
        g_hint_held = true;
        if (!pc_overlay_enabled() || (!pc_overlay_open() && now - g_hint_since < std::chrono::seconds(4)))
            t = pc_tr("Graphics settings: F10  (pad: L3 + R3)");
    }
    if (!(vk_scene_idle_frames() > 45)) g_hint_held = false;
    // (the shader precompile's progress: its own block, osd_precompile below, so the title's hint no longer hides it)
    {   // TOTK_WARMUP (warmup.sh): the warm-up's progress replaces everything else (the title screen is all that runs)
        int d = 0, n = 0;
        if (vk_precompile_warmup_progress(&d, &n) && n) {
            char b[256];
            snprintf(b, sizeof b, pc_tr("Shader warm-up: %d / %d (%d %%) - press A to play now, it keeps compiling in the background"),
                     d, n, d * 100 / n);
            t = b;
        }
    }
    {   // a notice (osd_toast) replaces the line while it lasts; the F12 shot is taken before the banner is drawn
        std::lock_guard<std::mutex> l(g_toast_mu);
        if (!g_toast.empty() && std::chrono::steady_clock::now() < g_toast_until) { t = g_toast; toast = true; }
    }
    if (off || t.empty()) return nullptr;
    int fh = target_h / 34 < 14 ? 14 : target_h / 34;
    if (t != g_banner.text || fh != g_banner.font_h) {
        render(g_banner, t, fh, g_skip_opening && !toast ? RGB(140, 235, 140) : RGB(235, 235, 235));
        g_banner.text = t; g_banner.font_h = fh;
    }
    *w = g_banner.w; *h = g_banner.h;
    return g_banner.px.empty() ? nullptr : g_banner.px.data();   // (empty: no font on Linux)
}

// fps readout (device.c, top left while fullscreen or after F3): `text` changes every 0.5 s, so GDI renders twice a
// second at most. Render thread only, like osd_banner.
extern "C" const uint32_t* osd_fps(const char* text, int target_h, int* w, int* h) {
    if (!text || !*text) return nullptr;
    int fh = target_h / 48 < 12 ? 12 : target_h / 48;
    if (g_fps.text != text || fh != g_fps.font_h) {
        render(g_fps, text, fh, RGB(235, 235, 235));
        g_fps.text = text; g_fps.font_h = fh;
    }
    *w = g_fps.w; *h = g_fps.h;
    return g_fps.px.empty() ? nullptr : g_fps.px.data();
}

// Shader precompile progress (exec.c, the recorded / seed phase), bottom right while it runs: "Compiling shaders N %"
// on a first start or after a driver change (no pipeline cache loaded: device.c g_vk_pcache_loaded), "Loading shaders"
// otherwise, with a bar under the text; shown after 1 s of it (a warm start, and the driver's own disk cache, finish
// it in ~1.3 s). It used to share the bottom-left banner and only while no hint showed, so the title screen's hint (where a
// first start spends its precompile) hid it. Render thread only. TOTK_OSD=0 hides it with the rest.
extern "C" size_t g_vk_pcache_loaded;
extern "C" int vk_precompile_cold_active(void);   // exec.c: the first-start pass (every core but one)
static OsdBlock g_pc;
extern "C" const uint32_t* osd_precompile(int target_h, int* w, int* h) {
    static const bool off = getenv("TOTK_OSD") && getenv("TOTK_OSD")[0] == '0';
    static std::chrono::steady_clock::time_point since;
    static bool was;
    int d = 0, n = 0;
    bool on = !off && vk_precompile_recorded_progress(&d, &n) && n > 0;
    const auto now = std::chrono::steady_clock::now();
    if (on && !was) since = now;
    was = on;
    const bool compiling = g_vk_pcache_loaded == 0;
    if (!on || now - since < std::chrono::seconds(1)) return nullptr;
    int pct = d * 100 / n;
    char b[160];
    snprintf(b, sizeof b, vk_precompile_cold_active() ? pc_tr("Preparing the shaders for this PC (first start): %d %%")
                          : compiling ? pc_tr("Compiling shaders %d %%") : pc_tr("Loading shaders %d %%"), pct);
    int fh = target_h / 48 < 12 ? 12 : target_h / 48;
    if (g_pc.text != b || fh != g_pc.font_h) {
        OsdBlock t;
        render(t, b, fh, RGB(235, 235, 235));
        g_pc.px.clear(); g_pc.w = g_pc.h = 0;
        if (!t.px.empty()) {   // the text block + a progress bar (track and fill) in a strip below it
            int pad = fh / 2, bar = fh / 5 < 3 ? 3 : fh / 5, hh = t.h + bar + pad / 2;
            g_pc.px.assign((size_t)t.w * hh, 0xff000000u | (12u << 16) | (14u << 8) | 16u);
            memcpy(g_pc.px.data(), t.px.data(), t.px.size() * 4);
            int x0 = pad, x1 = t.w - pad, xf = x0 + (x1 - x0) * pct / 100;
            for (int y = t.h; y < t.h + bar; y++)
                for (int x = x0; x < x1; x++) g_pc.px[(size_t)y * t.w + x] = x < xf ? 0xffebebebu : 0xff3a3d42u;
            g_pc.w = t.w; g_pc.h = hh;
        }
        g_pc.text = b; g_pc.font_h = fh;
    }
    *w = g_pc.w; *h = g_pc.h;
    return g_pc.px.empty() ? nullptr : g_pc.px.data();
}

// ---- Graphics menu (user 2026-10-18: "add an IN GAME menu option in the system menu to change resolution / upscaling").
// L3 + R3 (both sticks clicked) or F10 opens it over the game; d-pad up / down picks a row, left / right or A changes it,
// B (or L3 + R3 / F10 again) closes it. While a pause / System menu holds the picture, the banner shows how to open it.
// Choices go to recomp_settings.ini next to the userdata folder and are put into the environment when the game starts,
// over the launcher's defaults (all of them are read once at start, so a change applies on the next start).
// TOTK_SETTINGS=0 ignores the file (test runs: tools/run_test.sh, tools/linux/deck_remote_run.sh).
struct MenuOpt { const char* val; const char* name; };   // val "" = variable unset
struct MenuItem { const char* key; const char* label; const char* def; std::vector<MenuOpt> opts; };
static const std::vector<MenuItem>& menu_items() {
    static const std::vector<MenuItem> v = {
        {"TOTK_RES_SCALE", "Render resolution", "1",
         {{"0.67", "67 %"}, {"0.8", "80 %"}, {"0.9", "90 %"}, {"1", "100 % (native)"}, {"1.5", "150 %"}, {"2", "200 %"}}},
        {"TOTK_DYNRES", "Dynamic resolution", "0", {{"0", "Off"}, {"1", "On (holds the frame rate)"}}},
        {"TOTK_SSAA", "3D upscaler", "", {{"", "Bicubic (recomp)"}, {"fetch", "FSR 1 (the game's)"}}},
        {"TOTK_UPSCALE", "Window upscaler", "", {{"", "Bilinear"}, {"fsr", "FSR 1 (window above 1080p)"}}},
        {"TOTK_SHARPEN", "Sharpening", "", {{"", "Off"}, {"1", "FSR 1 RCAS low"}, {"0.5", "FSR 1 RCAS medium"}, {"0.2", "FSR 1 RCAS high"}}},
        {"TOTK_FPS", "Frame rate", "0", {{"0", "30 (the game's own)"}, {"60", "60"}, {"90", "90"}, {"120", "120"}}},
    };
    return v;
}
static std::string settings_path() {
    if (const char* f = getenv("TOTK_SETTINGS_FILE")) return f;
    const char* u = getenv("TOTK_USERDATA");
#ifdef _WIN32
    std::string d = u && *u ? u : "C:/totk/userdata";
#else
    const char* x = getenv("XDG_DATA_HOME"); const char* hm = getenv("HOME");
    std::string d = u && *u ? u : (x && *x ? std::string(x) : std::string(hm && *hm ? hm : ".") + "/.local/share") + "/totk-recomp/userdata";
#endif
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    size_t s = d.find_last_of("/\\");
    return (s == std::string::npos ? std::string(".") : d.substr(0, s)) + "/recomp_settings.ini";
}
static void env_put(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());   // ("" removes it)
#else
    if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
}
static std::string g_launch_val[8];   // each item's value this run started with
static int g_chosen[8];               // option picked in the menu (-1: a value the menu does not list)
static std::atomic<int> g_menu_open{0}, g_menu_row{0}, g_menu_gen{1};
static int opt_index(const MenuItem& it, const std::string& v) {
    for (size_t i = 0; i < it.opts.size(); i++) if (v == it.opts[i].val) return (int)i;
    return -1;
}
// Before main (the function-local statics elsewhere read these variables on first use, after main starts).
static struct SettingsLoad {
    SettingsLoad() {
        const auto& items = menu_items();
        const char* off = getenv("TOTK_SETTINGS");
        int n = 0;
        if (!(off && off[0] == '0'))
            if (FILE* f = fopen(settings_path().c_str(), "r")) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    std::string s(line);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                    size_t eq = s.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = s.substr(0, eq), v = s.substr(eq + 1);
                    for (const auto& it : items)   // (only the menu's own variables, only listed values)
                        if (k == it.key && opt_index(it, v) >= 0) { env_put(it.key, v); n++; }
                }
                fclose(f);
            }
        for (size_t i = 0; i < items.size() && i < 8; i++) {
            const char* e = getenv(items[i].key);
            g_launch_val[i] = e ? e : items[i].def;
            g_chosen[i] = opt_index(items[i], g_launch_val[i]);
        }
        if (n) printf("[settings] %d from %s\n", n, settings_path().c_str());
    }
} g_settings_load;
static void settings_save() {
    const auto& items = menu_items();
    std::string p = settings_path();
    const char* off = getenv("TOTK_SETTINGS");
    if (off && off[0] == '0') { printf("[settings] not saved (TOTK_SETTINGS=0)\n"); return; }   // (test runs leave the player's file alone)
    FILE* f = fopen(p.c_str(), "w");
    if (!f) { printf("[settings] can't write %s\n", p.c_str()); return; }
    fprintf(f, "# TotK recomp graphics settings (in game: L3 + R3 or F10). Read when the game starts.\n");
    for (size_t i = 0; i < items.size() && i < 8; i++)
        fprintf(f, "%s=%s\n", items[i].key, g_chosen[i] >= 0 ? items[i].opts[g_chosen[i]].val : g_launch_val[i].c_str());
    fclose(f);
    printf("[settings] saved %s\n", p.c_str());
}
extern "C" int osd_menu_is_open(void) { return g_menu_open.load(); }
// hle_hid.cpp host_input, on each input refresh: true while the menu has the controller (the game then sees nothing).
extern "C" bool osd_menu_input(uint64_t b, bool key) {
    const uint64_t A = 1u << 0, B = 1u << 1, LS = 1u << 4, RS = 1u << 5, Left = 1u << 12, Up = 1u << 13, Right = 1u << 14, Down = 1u << 15;
    static uint64_t was;
    static bool key_was;
    uint64_t pr = b & ~was;
    bool combo = (b & (LS | RS)) == (LS | RS) && (was & (LS | RS)) != (LS | RS);
    bool toggle = combo || (key && !key_was);
    was = b; key_was = key;
    if (!osd_menu_enabled()) return false;
    if (!g_menu_open.load()) {
        if (toggle) { g_menu_open = 1; g_menu_gen++; printf("[settings] menu opened\n"); }
        return toggle;
    }
    const auto& items = menu_items();
    int n = (int)(items.size() < 8 ? items.size() : 8), row = g_menu_row.load();
    if (toggle || (pr & B)) { g_menu_open = 0; g_menu_gen++; return true; }
    if (pr & Up) row = (row + n - 1) % n;
    if (pr & Down) row = (row + 1) % n;
    int d = (pr & (Right | A)) ? 1 : (pr & Left) ? -1 : 0;
    if (d) {
        int m = (int)items[row].opts.size(), c = g_chosen[row];
        g_chosen[row] = c < 0 ? (d > 0 ? 0 : m - 1) : (c + d + m) % m;
        settings_save();
    }
    if (pr) { g_menu_row = row; g_menu_gen++; }
    return true;
}
// The open menu as one panel (render thread, device.c vk_frame_end), or null when closed.
static OsdBlock g_menu;
extern "C" const uint32_t* osd_menu(int target_h, int* w, int* h) {
    if (!g_menu_open.load()) return nullptr;
    int fh = target_h / 30 < 14 ? 14 : target_h / 30;
    std::string key = std::to_string(g_menu_gen.load());
    if (g_menu.font_h != fh || g_menu.text != key) {
        const auto& items = menu_items();
        int n = (int)(items.size() < 8 ? items.size() : 8), row = g_menu_row.load();
        std::vector<OsdBlock> lines;
        auto add = [&](const std::string& t, COLORREF c) { lines.emplace_back(); render(lines.back(), t, fh, c); };
        add("Graphics settings (recomp)", RGB(150, 200, 255));
        bool restart = false;
        for (int i = 0; i < n; i++) {
            const MenuItem& it = items[i];
            std::string v = g_chosen[i] >= 0 ? it.opts[g_chosen[i]].name : g_launch_val[i];
            bool changed = (g_chosen[i] >= 0 ? std::string(it.opts[g_chosen[i]].val) : g_launch_val[i]) != g_launch_val[i];
            restart |= changed;
            add(std::string(i == row ? "> " : "   ") + it.label + ":   < " + v + " >" + (changed ? "   *" : ""),
                i == row ? RGB(255, 215, 90) : RGB(235, 235, 235));
        }
        add(restart ? "* applies the next time you start the game (saved)" : "Saved for the next start of the game", RGB(170, 170, 170));
        add("D-pad: choose / change    A: change    B: close", RGB(170, 170, 170));
        int W = 0, H = 0;
        for (auto& l : lines) { W = l.w > W ? l.w : W; H += l.h; }
        g_menu.px.assign((size_t)W * H, 0xff000000u | (12u << 16) | (14u << 8) | 16u);
        int y = 0;
        for (auto& l : lines) {
            for (int r = 0; r < l.h; r++) memcpy(&g_menu.px[(size_t)(y + r) * W], &l.px[(size_t)r * l.w], (size_t)l.w * 4);
            y += l.h;
        }
        g_menu.w = W; g_menu.h = H; g_menu.font_h = fh; g_menu.text = key;
        if (lines[0].px.empty()) g_menu.px.clear();   // (no font on Linux)
    }
    *w = g_menu.w; *h = g_menu.h;
    return g_menu.px.empty() ? nullptr : g_menu.px.data();
}
