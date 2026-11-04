// The game key this totk.exe was linked against, as build.py's runtime link writes it at the file's end
// (tools/engine/exekey.py: "\0nxr-key=<16 hex>\0", then "nxr-patches=<the key's game-patch features joined by '+'>\0",
// empty when the key was built without patches), and the start-up check against the package's player.txt next to the exe. 11-03: a slot exe linked
// from the unpatched dev key 2756a946 ran under player.txt's patched key 11637b05: none of the game patches (realtime,
// aspect, resolution) were in its code, so the game ran 2.7x at 160 fps and 32:9 was pillarboxed, and nothing said so.
// A mismatch now logs '[build] WARNING' lines and names itself in the window title (exe_tag_title, device.c).
// The getters read the file on their first call, so they work before main too (fps360's speed_fix_default asks from
// totk_engine_boot). An exe linked before the tag existed has none: every getter answers NULL, nothing is compared.
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif
#include "exe_tag.h"

namespace {
struct ExeTag {
    bool tagged = false, has_patches = false;
    char key[17] = {}, patches[512] = {};
    std::string dir;   // the exe's folder (player.txt)
    char title[256] = {};
};

std::string exe_path() {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, w, (DWORD)(sizeof w / sizeof w[0]));
    if (!n || n >= sizeof w / sizeof w[0]) return {};
    char u[MAX_PATH * 6];
    int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, u, (int)sizeof u - 1, nullptr, nullptr);
    if (m <= 0) return {};
    u[m] = 0;
    return u;
#else
    char b[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return {};
    b[n] = 0;
    return b;
#endif
}

FILE *open_utf8(const std::string &p) {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, (int)(sizeof w / sizeof w[0]))) return nullptr;
    return _wfopen(w, L"rb");
#else
    return fopen(p.c_str(), "rb");
#endif
}

bool is_hex16(const char *s) {
    for (int i = 0; i < 16; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

ExeTag read_tag() {
    ExeTag t;
    const std::string p = exe_path();
    const size_t slash = p.find_last_of("\\/");
    t.dir = slash == std::string::npos ? "." : p.substr(0, slash);
    FILE *f = p.empty() ? nullptr : open_utf8(p);
    if (!f) return t;
    static char tail[4096];   // (exekey.TAIL: the tag is the file's last bytes)
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        const long from = size > (long)sizeof tail ? size - (long)sizeof tail : 0;
        if (size > 0 && fseek(f, from, SEEK_SET) == 0) n = fread(tail, 1, sizeof tail, f);
    }
    fclose(f);
    static const char kKey[] = "nxr-key=", kPatches[] = "nxr-patches=";
    const size_t lk = sizeof kKey - 1, lp = sizeof kPatches - 1;
    for (size_t i = 0; i + 1 + lk + 17 <= n; i++) {   // (the last tag wins, as exekey.read)
        if (tail[i] || memcmp(tail + i + 1, kKey, lk) || !is_hex16(tail + i + 1 + lk) || tail[i + 1 + lk + 16]) continue;
        t.tagged = true, t.has_patches = false;
        memcpy(t.key, tail + i + 1 + lk, 16), t.key[16] = 0;
        size_t j = i + 1 + lk + 17;   // (just past the key's closing NUL)
        if (j + lp <= n && !memcmp(tail + j, kPatches, lp)) {
            size_t e = j + lp;
            while (e < n && tail[e] && e - (j + lp) < sizeof t.patches - 1) e++;
            if (e < n && !tail[e]) {
                memcpy(t.patches, tail + j + lp, e - (j + lp)), t.patches[e - (j + lp)] = 0;
                t.has_patches = true;
            }
        }
    }
    snprintf(t.title, sizeof t.title, "Tears of the Kingdom");
    return t;
}

ExeTag &tag() {
    static ExeTag t = read_tag();   // (thread-safe first call)
    return t;
}

// player.txt's value of `name` (a "name=value" line), or "" when there is none
std::string player_value(const std::string &dir, const char *name) {
    FILE *f = open_utf8(dir + "/player.txt");
    if (!f) return {};
    char line[1024];
    std::string v;
    const size_t ln = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, name, ln) || line[ln] != '=') continue;
        v = line + ln + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        break;
    }
    fclose(f);
    return v;
}
}   // namespace

extern "C" const char *totk_link_key(void) { return tag().tagged ? tag().key : nullptr; }
extern "C" const char *totk_link_patches(void) { return tag().tagged && tag().has_patches ? tag().patches : nullptr; }
extern "C" const char *exe_tag_title(void) {
    const char *t = tag().title;
    return t[0] ? t : "Tears of the Kingdom";
}

extern "C" void exe_tag_check(void) {
    ExeTag &t = tag();
    if (!t.tagged) {
        printf("[build] this exe carries no key tag (linked before 11-03, or not by build.py): not checked against player.txt\n");
        fflush(stdout);
        return;
    }
    printf("[build] linked against key %s, patches %s\n", t.key, !t.has_patches ? "(not recorded)" : t.patches[0] ? t.patches : "none");
    const std::string pk = player_value(t.dir, "key");
    if (pk.empty()) { fflush(stdout); return; }   // (a dev boot: no package next to the exe)
    if (pk == t.key) {
        printf("[build] matches player.txt's key\n");
        fflush(stdout);
        return;
    }
    const std::string pp = player_value(t.dir, "patches");
    for (int i = 0; i < 3; i++)   // (loud: a log reader skimming for the speed / aspect cause has to see it)
        printf("[build] WARNING: this totk.exe was linked against key %s (patches %s), but player.txt says key %s%s%s: "
               "its game code does not match this package's images and patch set (game speed, aspect, resolution can be "
               "wrong); relink it with build.py runtime --key %s --patches ... --strict-patches\n",
               t.key, !t.has_patches ? "not recorded" : t.patches[0] ? t.patches : "none", pk.c_str(), pp.empty() ? "" : ", patches ",
               pp.c_str(), pk.c_str());
    fflush(stdout);
    snprintf(t.title, sizeof t.title, "Tears of the Kingdom - WRONG BUILD: exe key %s, package key %s", t.key, pk.c_str());
}
