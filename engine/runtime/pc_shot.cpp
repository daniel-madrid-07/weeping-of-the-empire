// F12 screenshots. The window thread's F12 (shim_win.c) asks; the present thread (device.c vk_frame_end) copies the
// window image before the fps readout and menu are drawn on it, waits for that copy, and hands the pixels here. They
// are written as a PNG by Windows Imaging Component (part of Windows, no library to ship) on a thread of their own,
// so the frame that asked is not held up by the encoding. Folder: the user's Screenshots folder
// (FOLDERID_Screenshots, normally %USERPROFILE%\Pictures\Screenshots, created if missing; TOTK_PC_SHOT_DIR overrides
// it); name "Tears of the Kingdom <date> <time>.png". On unless TOTK_PC_SHOT=0.
#include "pc.h"
#include "pc_text.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#pragma comment(lib, "windowscodecs.lib")

extern "C" void osd_toast(const char* text, int ms);   // hle_osd.cpp

namespace {
volatile LONG g_pending;
volatile int g_on = -1;   // (game.screenshots sets it live: pc_shot_set_enabled)

bool shot_on() {
    if (g_on < 0) { const char* e = getenv("TOTK_PC_SHOT"); g_on = !(e && *e == '0'); }   // on unless "0" (db-play2)
    return g_on;
}

struct Job { uint8_t* px; uint32_t w, h; int rgba; };
}  // namespace
// game.screenshots (pc_settings.cpp) while the game runs: the next F12 follows it
extern "C" void pc_shot_set_enabled(bool on) { g_on = on ? 1 : 0; }
namespace {

bool screenshot_path(wchar_t* out, size_t n) {
    PWSTR dir = nullptr;
    if (const wchar_t* e = _wgetenv(L"TOTK_PC_SHOT_DIR"); e && *e) {   // tests and the boot harness: their own folder
        dir = (PWSTR)CoTaskMemAlloc(MAX_PATH * sizeof(wchar_t));   // (wide: not getenv's ANSI read as UTF-8, #94)
        if (!dir) return false;
        wcsncpy(dir, e, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        CreateDirectoryW(dir, nullptr);
    } else if (FAILED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &dir))) {
        CoTaskMemFree(dir); dir = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &dir))) { CoTaskMemFree(dir); return false; }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    for (int k = 0; k < 100; k++) {   // two in the same second get a suffix
        if (k) swprintf(out, n, L"%s\\Tears of the Kingdom %04u-%02u-%02u %02u-%02u-%02u (%d).png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, k);
        else swprintf(out, n, L"%s\\Tears of the Kingdom %04u-%02u-%02u %02u-%02u-%02u.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        // CREATE_NEW reserves the name atomically; write_png overwrites it
        HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); CoTaskMemFree(dir); return true; }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    CoTaskMemFree(dir);
    return false;
}

HRESULT write_png(const wchar_t* path, const Job* j) {
    IWICImagingFactory* f = nullptr; IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&s);
    if (SUCCEEDED(hr)) hr = s->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e);
    if (SUCCEEDED(hr)) hr = e->Initialize(s, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = e->CreateNewFrame(&fr, &props);
    if (SUCCEEDED(hr)) hr = fr->Initialize(props);
    if (SUCCEEDED(hr)) hr = fr->SetSize(j->w, j->h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = fr->WritePixels(j->h, j->w * 4, j->w * j->h * 4, j->px);
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = e->Commit();
    if (props) props->Release();
    if (fr) fr->Release();
    if (e) e->Release();
    if (s) s->Release();
    if (f) f->Release();
    return hr;
}

volatile LONG g_writers;   // writer threads in flight: each holds a frame copy (33 MB at 4K), so at most kMaxWriters
constexpr LONG kMaxWriters = 2;

DWORD WINAPI writer(LPVOID p) {
    Job* j = (Job*)p;
    for (size_t i = 0, n = (size_t)j->w * j->h * 4; i < n; i += 4) {   // to BGRA, opaque (swapchain alpha is undefined)
        uint8_t* px = j->px + i;
        if (j->rgba) { uint8_t r = px[0]; px[0] = px[2]; px[2] = r; }
        px[3] = 255;
    }
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t path[MAX_PATH * 2];
    if (!screenshot_path(path, sizeof path / sizeof *path)) fprintf(stderr, "[shot] no Screenshots folder\n");
    else {
        HRESULT hr = write_png(path, j);
        char u8[MAX_PATH * 4]; WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof u8, nullptr, nullptr);
        const char* name = strrchr(u8, '\\') ? strrchr(u8, '\\') + 1 : u8;
        char note[MAX_PATH * 4 + 32];
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "[shot] %ux%u saved to %s\n", j->w, j->h, u8);
            snprintf(note, sizeof note, pc_tr("Screenshot saved: %s"), name);
        } else {
            fprintf(stderr, "[shot] writing %s failed (HRESULT %#lx)\n", u8, (unsigned long)hr); DeleteFileW(path);
            snprintf(note, sizeof note, pc_tr("Screenshot failed (%s)"), name);
        }
        osd_toast(note, 2500);   // hle_osd.cpp: over the banner; drawn after the shot is taken, so never in one
    }
    if (SUCCEEDED(co)) CoUninitialize();
    free(j->px); free(j);
    InterlockedDecrement(&g_writers);
    return 0;
}
}   // namespace

extern "C" void pc_shot_request(void) {
    if (shot_on()) InterlockedExchange(&g_pending, 1);
}

extern "C" bool pc_shot_take(void) {
    return g_pending && InterlockedExchange(&g_pending, 0);
}

extern "C" void pc_shot_write(const uint8_t* px, uint32_t w, uint32_t h, uint32_t row_bytes, int rgba) {
    if (InterlockedIncrement(&g_writers) > kMaxWriters) {   // F12 pressed faster than PNGs are written
        InterlockedDecrement(&g_writers);
        fprintf(stderr, "[shot] skipped: %ld still being written\n", (long)kMaxWriters);
        osd_toast(pc_tr("Screenshot skipped (still saving the previous ones)"), 2000);
        return;
    }
    Job* j = (Job*)malloc(sizeof *j);
    uint8_t* copy = j ? (uint8_t*)malloc((size_t)w * h * 4) : nullptr;
    if (!copy) { free(j); InterlockedDecrement(&g_writers); fprintf(stderr, "[shot] out of memory\n"); return; }
    for (uint32_t y = 0; y < h; y++) memcpy(copy + (size_t)y * w * 4, px + (size_t)y * row_bytes, (size_t)w * 4);
    *j = Job{copy, w, h, rgba};
    HANDLE t = CreateThread(nullptr, 0, writer, j, 0, nullptr);
    if (t) CloseHandle(t);
    else { free(copy); free(j); InterlockedDecrement(&g_writers); fprintf(stderr, "[shot] no writer thread\n"); }
}

#else
// Linux (linux-port): the same F12 path; the PNG is stb_image_write's (public domain / MIT, linux/stb_image_write.h),
// the folder the desktop's Pictures folder's Screenshots (XDG_PICTURES_DIR in ~/.config/user-dirs.dirs, else
// ~/Pictures), created if missing; TOTK_PC_SHOT_DIR overrides it as on Windows.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "linux/stb_image_write.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void osd_toast(const char* text, int ms);   // hle_osd.cpp

namespace {
std::atomic<bool> g_pending{false};
std::atomic<int> g_on{-1};
std::atomic<int> g_writers{0};
constexpr int kMaxWriters = 2;
bool shot_on() {
    if (g_on.load() < 0) { const char* e = getenv("TOTK_PC_SHOT"); g_on.store(!(e && *e == '0')); }
    return g_on.load() != 0;
}
std::string pictures_dir() {   // the desktop's Pictures folder (xdg-user-dirs), else ~/Pictures
    const char* home = getenv("HOME");
    std::string h = home && *home ? home : ".";
    const char* cfg = getenv("XDG_CONFIG_HOME");
    std::string f = (cfg && *cfg ? std::string(cfg) : h + "/.config") + "/user-dirs.dirs";
    if (FILE* fp = fopen(f.c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, fp)) {
            if (strncmp(line, "XDG_PICTURES_DIR=\"", 18)) continue;
            std::string v = line + 18;
            v = v.substr(0, v.find('"'));
            if (!v.compare(0, 5, "$HOME")) v = h + v.substr(5);
            fclose(fp);
            return v;
        }
        fclose(fp);
    }
    return h + "/Pictures";
}
bool screenshot_path(std::string* out) {
    std::string dir;
    if (const char* e = getenv("TOTK_PC_SHOT_DIR"); e && *e) dir = e;
    else dir = pictures_dir() + "/Screenshots";
    for (size_t at = 1; at <= dir.size(); at++)   // (mkdir -p)
        if (at == dir.size() || dir[at] == '/') mkdir(dir.substr(0, at).c_str(), 0755);
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    for (int k = 0; k < 100; k++) {   // two in the same second get a suffix; O_EXCL reserves the name
        char name[96];
        int n = snprintf(name, sizeof name, "Tears of the Kingdom %04d-%02d-%02d %02d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1,
                         t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        if (k) snprintf(name + n, sizeof name - n, " (%d)", k);
        *out = dir + "/" + name + ".png";
        const int fd = open(out->c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (fd >= 0) { close(fd); return true; }
        if (errno != EEXIST) break;
    }
    return false;
}
void write_shot(uint8_t* px, uint32_t w, uint32_t h, int rgba) {
    for (size_t i = 0, n = (size_t)w * h * 4; i < n; i += 4) {   // to RGBA, opaque (swapchain alpha is undefined)
        if (!rgba) { const uint8_t b = px[i]; px[i] = px[i + 2]; px[i + 2] = b; }
        px[i + 3] = 255;
    }
    std::string path;
    if (!screenshot_path(&path)) fprintf(stderr, "[shot] no Screenshots folder\n");
    else {
        const bool ok = stbi_write_png(path.c_str(), (int)w, (int)h, 4, px, (int)w * 4) != 0;
        const char* name = strrchr(path.c_str(), '/') ? strrchr(path.c_str(), '/') + 1 : path.c_str();
        char note[1200];
        if (ok) {
            fprintf(stderr, "[shot] %ux%u saved to %s\n", w, h, path.c_str());
            snprintf(note, sizeof note, pc_tr("Screenshot saved: %s"), name);
        } else {
            fprintf(stderr, "[shot] writing %s failed\n", path.c_str());
            unlink(path.c_str());
            snprintf(note, sizeof note, pc_tr("Screenshot failed (%s)"), name);
        }
        osd_toast(note, 2500);
    }
    free(px);
    g_writers.fetch_sub(1);
}
}  // namespace
extern "C" void pc_shot_set_enabled(bool on) { g_on.store(on ? 1 : 0); }
extern "C" void pc_shot_request(void) { if (shot_on()) g_pending.store(true); }
extern "C" bool pc_shot_take(void) { return g_pending.load() && g_pending.exchange(false); }
extern "C" void pc_shot_write(const uint8_t* px, uint32_t w, uint32_t h, uint32_t row_bytes, int rgba) {
    if (g_writers.fetch_add(1) >= kMaxWriters) {   // F12 pressed faster than PNGs are written
        g_writers.fetch_sub(1);
        fprintf(stderr, "[shot] skipped: %d still being written\n", kMaxWriters);
        osd_toast(pc_tr("Screenshot skipped (still saving the previous ones)"), 2000);
        return;
    }
    uint8_t* copy = (uint8_t*)malloc((size_t)w * h * 4);
    if (!copy) { g_writers.fetch_sub(1); fprintf(stderr, "[shot] out of memory\n"); return; }
    for (uint32_t y = 0; y < h; y++) memcpy(copy + (size_t)y * w * 4, px + (size_t)y * row_bytes, (size_t)w * 4);
    std::thread(write_shot, copy, w, h, rgba).detach();
}
#endif
