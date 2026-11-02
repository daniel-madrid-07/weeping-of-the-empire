// pc_gpu_row.cpp: F10's "Graphics card" row (pc-ui 11-02, rival-parity: a laptop with an integrated and a dedicated GPU
// picks one without a variable). Its choices: Automatic (device.c's own pick: the dedicated GPU first) and the GPUs
// Windows has, by name (DXGI, read here before the game starts: the row's list must exist when the settings go into the
// environment, and Vulkan's own list, vk_gpu_names, comes only with the device). The name goes into LARECOMP_GPU, which
// device.c matches against the Vulkan names (whole, case-insensitive, else the first containing it); a GPU that cannot
// run the game is not picked there. Automatic leaves LARECOMP_GPU as it is. At the next start. Windows only.
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "pc_settings.h"
#include "pc_text.h"

namespace {
constexpr int kMaxGpus = 8;
std::string g_names[kMaxGpus], g_shown[kMaxGpus];
const char *V_GPU[kMaxGpus + 2], *N_GPU[kMaxGpus + 2];

std::string utf8(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The hardware adapters (no software renderer), each name once (two of the same card: LARECOMP_GPU's name match takes
// the first either way)
int list_gpus() {
    HMODULE dll = LoadLibraryW(L"dxgi.dll");   // (loaded, not linked: as pc_display_caps.cpp)
    using create_t = HRESULT(WINAPI *)(REFIID, void **);
    auto create = dll ? (create_t)(void *)GetProcAddress(dll, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1 *f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void **)&f))) return 0;
    int n = 0;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; n < kMaxGpus && f->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::string name = utf8(d.Description);
            bool seen = name.empty();
            for (int k = 0; k < n; k++) seen = seen || g_names[k] == name;
            if (!seen) {
                char b[320];
                const double gb = d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
                if (gb >= 0.5) snprintf(b, sizeof b, "%s (%.0f GB)", name.c_str(), gb);
                else snprintf(b, sizeof b, "%s", name.c_str());   // (an integrated GPU: shared memory, its name alone)
                g_names[n] = name;
                g_shown[n++] = b;
            }
        }
        a->Release();
    }
    f->Release();
    return n;
}

void gpu_env(const char *v) {   // (Automatic: LARECOMP_GPU as the launcher or the user left it)
    if (v && *v) SetEnvironmentVariableA("LARECOMP_GPU", v), _putenv_s("LARECOMP_GPU", v);
}

const pc_setting k_gpu[] = {
    {"graphics.gpu", "Graphics", "Display", "Graphics card", "The graphics card the game runs on, for a laptop with two. "
     "Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.", "", V_GPU,
     N_GPU, 0, 0, 0, PC_SET_RESTART, nullptr, gpu_env, nullptr, nullptr},
};

struct AddGpu {
    AddGpu() {
        const int n = list_gpus();
        V_GPU[0] = "";
        N_GPU[0] = PC_T("Automatic (a dedicated GPU first)");
        for (int i = 0; i < n; i++) V_GPU[i + 1] = g_names[i].c_str(), N_GPU[i + 1] = g_shown[i].c_str();
        V_GPU[n + 1] = N_GPU[n + 1] = nullptr;
        pc_settings_add(k_gpu, 1);
    }
} g_add_gpu;
}  // namespace
#endif
