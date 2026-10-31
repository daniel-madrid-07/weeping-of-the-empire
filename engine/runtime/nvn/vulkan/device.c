// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../shim/las.h"  // TotK: was ../../paths.h
#include "../shim/las.h"  // TotK: was ../../settings.h
#include "../shim/las.h"  // TotK: was ../../ui/menu.h
#include "vk.h"
#include "../../pc_overlay.h"
#include "../output_scale.h"
#include "../../pc.h"     // nxrecomp: F12 screenshots
#include "../../cpu_place.h"   // nxrecomp: the cache saver on the helper cores
static void *pcache_saver(void *arg);  // (pipeline cache saver thread, below vk_pipeline_cache_save)
// Hang watch (Linux: rendering stopped at 274 s without a GPU-hang report and the stuck process later kept the Deck from
// suspending). The present path stamps every frame; the last blocking Vulkan call is noted.
static _Atomic uint64_t g_last_frame_ns;
// Paused on focus loss: no frames on purpose, so not a hang either
static _Atomic int g_frames_held;
void vk_frames_held(int held) { atomic_store(&g_frames_held, held); }
const char *volatile g_rt_op;      // last blocking Vulkan call started (exec.c / here), NULL when it returned
volatile uint64_t g_rt_op_ns;
static volatile int g_present_tid;
#ifndef _WIN32
#include <sys/syscall.h>
#include "../../linux/plat_posix.h"
static void *hang_watch(void *arg);
#endif
// Pipelines made with dynamic uniform buffers (exec.c make_dynamic_ubos, default) never match entries made with plain
// ones: their own file, so the old 3.2 GB one is not loaded for nothing. LARECOMP_VK_DYN_UBO=0 keeps using
// cache/vk_pipelines.bin.
static const char *pcache_file(void) {
    const char *e = getenv("LARECOMP_VK_DYN_UBO");
    return e && !strcmp(e, "0") ? "cache/vk_pipelines.bin" : "cache/vk_pipelines_dynubo.bin";
}
static size_t pcache_saved;  // size of the last file written / loaded (an unchanged 1.8 GB cache was rewritten every boot)
// Two boots of one exe share cache/ (boot.py --cache shared) and used to write one cache/<file>.tmp: a body spliced from
// both under a valid driver header could be published. Now each process writes its own.<pid>.tmp, replaces the file in
// one step, and the file ends with our trailer (size + hash of the driver's data), checked at load before the driver
// sees the data. A file without the trailer (an older build's) is still loaded.
typedef struct { char magic[8]; uint64_t size, hash; } PcacheTrailer;
static const char PCACHE_MAGIC[8] = "NXRPC1";
static uint64_t pcache_hash(const void *p, size_t n) {   // 4 independent lanes: ~1 s for a 2.6 GB cache with one
    const uint8_t *b = p;
    uint64_t h[4] = {0x9e3779b97f4a7c15ull ^ n, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            memcpy(&w, b + i + 8 * k, 8);
            h[k] = (h[k] ^ w) * 0xff51afd7ed558ccdull;
            h[k] ^= h[k] >> 29;
        }
    uint64_t r = h[0] ^ h[1] * 3 ^ h[2] * 5 ^ h[3] * 7;
    for (; i < n; i++) r = (r ^ b[i]) * 0x100000001b3ull;
    return r ^ r >> 31;
}
// The pipeline cache file's bytes, read and checked against its trailer (data NULL / sz 0: none or bad). Timed, which
// were serial before the first present; now ui_window_init starts it on a thread (vk_pipeline_cache_prefetch) and
// create_device takes the result (the device's header check and vkCreatePipelineCache stay there), or reads it itself
// when nothing was started. (path: pcache_file()'s relative name, taken by the caller, so the thread reads no
// environment (the CRT's getenv races pc_settings' _putenv_s). Relative on purpose: an absolute one from _fullpath is
// in the ANSI code page, lossy for a game folder named outside it, and the fopen would fail; nothing changes the cwd.)
typedef struct { char path[64]; void *data; size_t sz; uint64_t read_ns, check_ns; } PcacheRead;
static PcacheRead g_pc_read;
static pthread_t g_pc_thread;
static bool g_pc_thread_on;
static void pcache_path(char *out, size_t n) { snprintf(out, n, "%s", pcache_file()); }
static void *pcache_read(void *arg) {
    PcacheRead *r = (PcacheRead *)arg;
    const uint64_t t0 = SDL_GetTicksNS();
    r->data = NULL;
    r->sz = 0;
    r->read_ns = r->check_ns = 0;
    FILE *fp = fopen(r->path, "rb");
    if (!fp) { r->read_ns = SDL_GetTicksNS() - t0; return NULL; }
    // 64-bit size: with the effect shaders warmed the cache passed 2 GB and ftell's 32-bit long made every later
    // boot die here.
#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    long long end = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
#else
    fseeko(fp, 0, SEEK_END);
    long long end = (long long)ftello(fp);
    fseeko(fp, 0, SEEK_SET);
#endif
    size_t sz = end > 0 ? (size_t)end : 0;
    void *data = sz ? malloc(sz) : NULL;
    if (!data || fread(data, 1, sz, fp) != sz) {
        if (sz) LOG("[vk] pipeline cache: %s (%lld bytes) not read: starting empty\n", r->path, end);
        sz = 0;
    }
    fclose(fp);
    const uint64_t t1 = SDL_GetTicksNS();
    PcacheTrailer tr;
    if (sz >= sizeof tr) memcpy(&tr, (char *)data + sz - sizeof tr, sizeof tr);
    if (sz >= sizeof tr && !memcmp(tr.magic, PCACHE_MAGIC, sizeof tr.magic)) {
        if (tr.size != sz - sizeof tr || pcache_hash(data, (size_t)tr.size) != tr.hash) {
            LOG("[vk] pipeline cache: %s (%zu KB) fails its size / hash check (a torn or spliced write): starting empty\n",
                r->path, sz >> 10);
            sz = 0;
        } else sz = (size_t)tr.size;
    } else if (sz) LOG("[vk] pipeline cache: %s has no trailer (an older build's): loaded unchecked\n", r->path);
    if (!sz) { free(data); data = NULL; }
    r->data = data;
    r->sz = sz;
    r->read_ns = t1 - t0;
    r->check_ns = SDL_GetTicksNS() - t1;
    return NULL;
}
static void vk_pipeline_cache_prefetch(void) {   // (ui_window_init, once: create_device joins it)
    if (g_pc_thread_on) return;
    pcache_path(g_pc_read.path, sizeof g_pc_read.path);
    if (pthread_create(&g_pc_thread, NULL, pcache_read, &g_pc_read) == 0) g_pc_thread_on = true;
}
#ifdef _WIN32
extern int shim_file_replace(const char *src, const char *dst);         // (shim_win.c: MoveFileEx, replace existing)
extern void shim_remove_old_files(const char *pattern, int min_age_s);
#else
#include <glob.h>
static int shim_file_replace(const char *src, const char *dst) { return rename(src, dst); }
static void shim_remove_old_files(const char *pattern, int min_age_s) {
    glob_t g;
    if (glob(pattern, 0, NULL, &g)) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        struct stat sb;
        if (!stat(g.gl_pathv[i], &sb) && time(NULL) - sb.st_mtime >= min_age_s) remove(g.gl_pathv[i]);
    }
    globfree(&g);
}
#endif
size_t g_vk_pcache_loaded;   // bytes of cache/vk_pipelines.bin the driver accepted (0: started empty; exec.c pa_warm_load)
extern uint32_t vk_archive_warm_snapshot(void);
extern void vk_archive_warm_commit(uint32_t n);
#include "../shim/las.h"  // TotK: was ../renderer.h

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    // first 3 of each message ID (a few early recurring VUIDs filled the old 500-line cap long before gameplay)
    static int32_t ids[1024];
    static uint8_t cnt[1024];
    static int n;
    if (sev < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT || n >= 5000) return VK_FALSE;
    uint32_t k = (uint32_t)d->messageIdNumber * 2654435761u >> 22;
    while (cnt[k] && ids[k] != d->messageIdNumber) k = (k + 1) & 1023;
    ids[k] = d->messageIdNumber;
    if (cnt[k] < 255 && cnt[k]++ < 3) { n++; LOG("[vk] %s\n", d->pMessage); }
    return VK_FALSE;
}

// A GPU or driver that cannot run the game: one message box, Spanish and English, naming the GPU and what it lacks,
// instead of a window that closes with only a log line. `gpu` may be NULL (no device picked yet); `what` is the
// technical reason (English, for the log and the box). LARECOMP_HIDDEN / LARECOMP_NO_DIALOG: log only.
_Noreturn static void gpu_unsupported(const char *gpu, const char *what) {
    LOG("[vk] this GPU cannot run the game: %s%s%s\n", what, gpu ? " on " : "", gpu ? gpu : "");
    if (!getenv("LARECOMP_HIDDEN") && !getenv("LARECOMP_NO_DIALOG")) {
        char msg[2048];
        snprintf(msg, sizeof msg,
                 "Tu tarjeta gr\xc3\xa1" "fica no puede ejecutar el juego.\n"
                 "GPU: %s\n"
                 "Motivo: %s\n\n"
                 "Se necesita una GPU con Vulkan 1.3: NVIDIA GTX 900 o posterior, AMD RX 400 o posterior, "
                 "Intel Arc o Iris Xe. Actualiza el controlador de la GPU desde la web de NVIDIA, AMD o Intel y vuelve a "
                 "intentarlo. En un port\xc3\xa1til con dos GPU, elige la dedicada en Configuraci\xc3\xb3n de Windows > "
                 "Pantalla > Gr\xc3\xa1" "ficos.\n\n"
                 "Your graphics card cannot run the game.\n"
                 "GPU: %s\n"
                 "Reason: %s\n\n"
                 "A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or newer, AMD RX 400 or newer, Intel Arc or "
                 "Iris Xe. Update the GPU driver from the NVIDIA, AMD or Intel website and try again. On a laptop with two "
                 "GPUs, pick the dedicated one in Windows Settings > Display > Graphics.",
                 gpu ? gpu : "?", what, gpu ? gpu : "?", what);
        if (window) SDL_HideWindow(window);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TotK: GPU no compatible / GPU not supported", msg, NULL);
    }
    // (the player launcher shows no second dialog for this code and tries no older package: launcher.c GPU_UNSUPPORTED_CODE;
    // _exit: nothing of the half-made device is saved at exit)
    fflush(stdout);
    fprintf(stderr, "[fatal] GPU not supported: %s\n", what);
    fflush(stderr);
    _exit((int)0x47505530u);   // "GPU0"
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[24];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    n_ext = vk_ngx_instance_exts(ext, n_ext, 22);   // ngx.c: what DLSS asks for (TOTK_DLSS only)
    // present.c: HDR colour spaces (before debug utils: the retry below drops the last extension)
    n_ext += (uint32_t)vk_present_instance_exts(ext + n_ext, 23 - (int)n_ext);
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) {
        char why[96];
        snprintf(why, sizeof why, r == VK_ERROR_INCOMPATIBLE_DRIVER ? "no Vulkan 1.3 driver is installed (vkCreateInstance %d)"
                                                                    : "the Vulkan driver did not start (vkCreateInstance %d)", r);
        gpu_unsupported(NULL, why);
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}
static void drop_ext(const char **ext, uint32_t *n, const char *name) {   // (an extension we cannot enable after all)
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], name)) { memmove(&ext[i], &ext[i + 1], (*n - i - 1) * sizeof *ext); (*n)--; return; }
}

// Why a device cannot run the game (NULL: it can), and its graphics + compute queue that presents to the window. The
// requirements with no fallback: Vulkan 1.3, that queue, the swapchain, host-pointer imports (the guest heap is
// imported GPU memory, exec.c; without them shader memory reads return zero), 64-bit integers and subgroup
// operations in shaders.
static const char *device_unfit(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *p, uint32_t *qfam) {
    static char why[128];
    if (p->apiVersion < VK_API_VERSION_1_3) {
        snprintf(why, sizeof why, "its driver offers Vulkan %u.%u, 1.3 is needed", VK_API_VERSION_MAJOR(p->apiVersion),
                 VK_API_VERSION_MINOR(p->apiVersion));
        return why;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    *qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && *qfam == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) *qfam = i;
    }
    free(qp);
    if (*qfam == UINT32_MAX) return "no graphics queue that can show the game's window (on a laptop, the GPU not wired to the screen)";
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    const char *miss = !has_ext(ep, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ? VK_KHR_SWAPCHAIN_EXTENSION_NAME
                     : !has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ? VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME : NULL;
    free(ep);
    if (miss) { snprintf(why, sizeof why, "its driver lacks %s", miss); return why; }
    // what every translated shader needs (translate.c's GLSL prelude): 64-bit integers, and the subgroup operations of
    // Maxwell's VOTE / SHFL in the vertex, fragment and compute stages (SwiftShader has neither)
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(pd, &f);
    if (!f.shaderInt64) return "no 64-bit integers in shaders (shaderInt64)";
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                       VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    if ((sg.supportedOperations & ops) != ops || (sg.supportedStages & stages) != stages) {
        snprintf(why, sizeof why, "too few subgroup operations in shaders (ops 0x%x in stages 0x%x)", sg.supportedOperations, sg.supportedStages);
        return why;
    }
    return NULL;
}

// The best device that can run the game: discrete first, then integrated, then anything else (a software driver last);
// LARECOMP_GPU=<index> picks one (if it can run the game). None can: the message box, naming the best one and why.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) gpu_unsupported(NULL, "the Vulkan driver lists no GPU");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1, rej_score = -1;
    uint32_t best_q = UINT32_MAX;
    char rej_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "", rej_why[128] = "";
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        uint32_t q = UINT32_MAX;
        const char *why = device_unfit(pd[i], &p, &q);
        LOG("[vk] gpu %u: %s (api %u.%u.%u, vendor %04x)%s%s\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.vendorID, why ? ": cannot run the game: " : "",
            why ? why : "");
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1 : 2;
        if ((int)i == want) score = 100;
        if (why) {   // (the message names the most capable device that was turned down)
            if (score > rej_score) { rej_score = score; snprintf(rej_name, sizeof rej_name, "%s", p.deviceName); snprintf(rej_why, sizeof rej_why, "%s", why); }
            continue;
        }
        if (score > best_score) { best_score = score; best = (int)i; best_q = q; }
    }
    if (best < 0) gpu_unsupported(rej_name, rej_why);
    if (want >= 0 && best != want) LOG("[vk] LARECOMP_GPU=%d cannot run the game: using gpu %d\n", want, best);
    g_vk.phys = pd[best];
    g_vk.qfam = best_q;
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext, g_fault_binary;
static void fault_info(void);
// The launcher's "> play.log" empties the log at the next launch: keep a copy of a crashed session's log as
// crash_<unix time>.log next to the driver dump (user 2026-10-19: hangs are random, relaunching lost the 16:10 one)
static void save_log_copy(void) {
#ifdef _WIN32
    fflush(stdout);
    fflush(stderr);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    wchar_t src[MAX_PATH];
    DWORD n = out && GetFileType(out) == FILE_TYPE_DISK ? GetFinalPathNameByHandleW(out, src, MAX_PATH, 0) : 0;
    if (!n || n >= MAX_PATH) return;
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return;
    char name[64];
    snprintf(name, sizeof name, "crash_%lld.log", (long long)time(NULL));
    FILE *fp = fopen(name, "wb");
    char buf[1 << 16];
    DWORD got;
    while (fp && ReadFile(in, buf, sizeof buf, &got, NULL) && got) fwrite(buf, 1, got, fp);
    CloseHandle(in);
    if (fp) { fclose(fp); LOG("[vk] log copied to %s\n", name); }
#endif
}
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log; then the log copy.
void vk_device_fault_report(void) {
    static int once;
    if (once++) return;
    if (g_fault_ext) fault_info();
    save_log_copy();
}
static void fault_info(void) {
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    void *bin = g_fault_binary && c.vendorBinarySize ? malloc(c.vendorBinarySize) : NULL;
    if (!bin) c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend,
                                 .pVendorBinaryData = bin};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    if (bin && c.vendorBinarySize) {   // for the vendor's crash-dump tools (NVIDIA: Nsight Graphics / Aftermath)
        char name[64];
        snprintf(name, sizeof name, "gpu_fault_%lld.bin", (long long)time(NULL));
        FILE *fp = fopen(name, "wb");
        size_t w = fp ? fwrite(bin, 1, c.vendorBinarySize, fp) : 0;
        if (fp) fclose(fp);
        LOG("[vk] device fault: driver crash dump %s (%zu of %llu bytes)\n", name, w, (unsigned long long)c.vendorBinarySize);
    }
    free(bin);
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

bool vk_dyn_clamp;   // depth clamp is dynamic state in the GPL pre-rasterization libraries (create_device; exec.c)
static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[40];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    else LOG("[vk] no VK_EXT_custom_border_color: samplers with a border colour other than transparent / opaque black or "
             "opaque white use transparent black (textures.c; a visible difference wherever the game samples past an edge)\n");
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    // LARECOMP_VK_NO_HOST_IMPORT=1: no host-pointer imports (diagnostic A/B on amdgpu, where an import is a userptr buffer;
    // shader global memory then reads zero, so the picture is wrong)
    if (getenv("LARECOMP_VK_NO_HOST_IMPORT")) { g_vk.host_import = false; LOG("[vk] host-pointer imports off (LARECOMP_VK_NO_HOST_IMPORT)\n"); }
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    // dma-buf imports (Linux, amdgpu): the guest heap / alias window as driver-owned buffer objects instead of userptr host
    // imports (runtime/linux/plat_posix.cpp rt_gpu_bo_map, exec.c host_import)
    g_vk.dmabuf_import = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (g_vk.dmabuf_import) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    // VK_EXT_memory_budget (optional): the OS budget of each heap, for the [vram] line, the overlay and VRAM residency.
    // Without it the budget struct must not be chained (VUID); the readouts then report 0.
    g_vk.mem_budget = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (g_vk.mem_budget) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    // VK_EXT_depth_range_unrestricted (optional): depth values outside [0,1] stay defined in D32_SFLOAT copies
    g_vk.depth_unrestricted = has_ext(ep, ne, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    if (g_vk.depth_unrestricted) ext[n_ext++] = VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // NVIDIA checkpoints: exec.c marks every draw / dispatch / pass / copy; after a GPU hang the driver reports the last
    // mark each pipeline stage reached (exec.c hang_report). LARECOMP_CHECKPOINTS=0: off.
    g_vk.checkpoints = has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) &&
                       !(getenv("LARECOMP_CHECKPOINTS") && !strcmp(getenv("LARECOMP_CHECKPOINTS"), "0"));
    if (g_vk.checkpoints) ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    LOG("[vk] GPU checkpoints (hang reports): %s\n", g_vk.checkpoints ? "on" : "off");
    // shader libraries + fast link (DXVK's stutter fix): each program's shaders compile once into libraries, a new draw
    // state only links them (exec.c). LARECOMP_NO_GPL=1: monolithic pipelines only (old).
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sgp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &p13};
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &sgp};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, .pNext = &drv};
    const bool has_gpl = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    // (the GPL properties struct only when the device has the extension: VUID-VkPhysicalDeviceProperties2-pNext)
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = has_gpl ? (void *)&gplp : (void *)&drv};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    // NVIDIA's compiler miscompiles some programs in one shader form: those take the other (translate.c nv_pcloop)
    extern bool maxwell_nvidia;
    maxwell_nvidia = drv.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    LOG("[vk] driver %s %s%s\n", drv.driverName, drv.driverInfo, maxwell_nvidia ? " (NVIDIA shader-form list on)" : "");
    {   // translate.c requires GL_KHR_shader_subgroup_{basic,vote,ballot,shuffle,shuffle_relative} (Maxwell VOTE / SHFL)
        const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                           VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        if ((sgp.supportedOperations & ops) != ops || (sgp.supportedStages & stages) != stages)
            LOG("[vk] warning: subgroup operations 0x%x in stages 0x%x (size %u): shaders using vote / ballot / shuffle outside "
                "them will not build\n", sgp.supportedOperations, sgp.supportedStages, sgp.subgroupSize);
        else LOG("[vk] subgroups: size %u, the operations the translator uses in every stage\n", sgp.subgroupSize);
    }
    // Maxwell warps are 32 lanes (translate.c LANEID / lane masks / VOTE / SHFL). Where subgroups may be another size
    // (AMD 64 or 32, Intel 8-32; NVIDIA is always 32 and nothing changes there), the stages whose shaders use warp ops
    // require 32 (subgroup size control, exec.c sg_pin) and the translator's wave-safe forms keep a warp inside its own
    // 32-lane half, right at 32 and at 64. A stage that cannot be pinned and may run narrower than 32 is logged: warp ops
    // there are wrong. LARECOMP_WAVE_SAFE=1: the wave-safe forms on any GPU (a check on NVIDIA).
    extern bool maxwell_wave_safe;
    const VkShaderStageFlags sg_used = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    const bool sg_fixed32 = p13.minSubgroupSize == 32 && p13.maxSubgroupSize == 32;
    maxwell_wave_safe = !sg_fixed32 || (getenv("LARECOMP_WAVE_SAFE") && strcmp(getenv("LARECOMP_WAVE_SAFE"), "0"));
    VkShaderStageFlags sg_pinnable = !sg_fixed32 && p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32
                                     ? p13.requiredSubgroupSizeStages & sg_used : 0;
    // (independent interpolation: our vertex outputs carry no interpolation qualifiers while the fragment inputs do, and
    // the two libraries compile apart; without it, monolithic pipelines)
    g_vk.gpl = has_gpl && gplp.graphicsPipelineLibraryFastLinking && gplp.graphicsPipelineLibraryIndependentInterpolationDecoration &&
               !getenv("LARECOMP_NO_GPL");
    if (g_vk.gpl) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    LOG("[vk] graphics pipeline library: %s\n", g_vk.gpl ? "on (fast link)" : "off");
    // Depth clamp as dynamic state of the GPL pre-rasterization library (U21, 2026-10-20: TotK's shadow casters draw with
    // depth clamp; a static clamp kept those keys off the fast link, so their draws were skipped until a monolithic build
    // on a worker finished = shadows "spawn in"). exec.c sets it per draw. LARECOMP_NO_DYN_CLAMP=1: off (old).
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    vk_dyn_clamp = g_vk.gpl && has_ext(ep, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && !getenv("LARECOMP_NO_DYN_CLAMP");
    if (vk_dyn_clamp) {
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &eds3};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        vk_dyn_clamp = eds3.extendedDynamicState3DepthClampEnable && q.features.depthClamp;
    }
    if (vk_dyn_clamp) ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    LOG("[vk] dynamic depth clamp (GPL fast link for clamped draws): %s\n", vk_dyn_clamp ? "on" : "off");
    // Attachment feedback loops: TotK's G-buffer decals sample the targets they render to; exec.c declares those loops
    // (feedback-loop image usage, VkAttachmentFeedbackLoopInfoEXT, dynamic state) instead of leaving them undefined.
    // Layouts stay GENERAL (unified image layouts). LARECOMP_NO_FEEDBACK_LOOP_EXT=1: off.
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR uil = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT fld = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT, .pNext = &uil};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fll = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, .pNext = &fld};
    g_vk.feedback_loop = has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
                         has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME) &&
                         has_ext(ep, ne, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME) && !getenv("LARECOMP_NO_FEEDBACK_LOOP_EXT");
    if (g_vk.feedback_loop) {
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fll};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        g_vk.feedback_loop = fll.attachmentFeedbackLoopLayout && fld.attachmentFeedbackLoopDynamicState && uil.unifiedImageLayouts;
        uil.unifiedImageLayoutsVideo = VK_FALSE;   // (no video queues)
    }
    // LARECOMP_FBL_LAYOUT_TEST=1: the layout-only mode on a driver that has the trio (to test AMD's path on NVIDIA)
    if (g_vk.feedback_loop && getenv("LARECOMP_FBL_LAYOUT_TEST")) g_vk.feedback_loop = false;
    if (g_vk.feedback_loop) {
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    } else if (has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) && !getenv("LARECOMP_NO_FEEDBACK_LOOP_EXT") &&
               !getenv("LARECOMP_NO_FBL_LAYOUT")) {
        // The layout extension alone: a draw samples a bound colour attachment live in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL,
        // the attachment moved to that layout only while such a pass runs, the pass's pipelines built with the
        // feedback-loop flag (exec.c resolve_feedback / begin_pass); otherwise every such read was a full-target snapshot
        // copy (4.5-8 % of an AMD iGPU frame). LARECOMP_NO_FBL_LAYOUT=1: copies (old).
        VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT f1 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f1};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        g_vk.fbl_layout = f1.attachmentFeedbackLoopLayout;
        if (g_vk.fbl_layout) {
            ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
            fll.pNext = NULL;   // (enabled with the device features below: this one alone)
        }
    }
    LOG("[vk] attachment feedback loops: %s\n", g_vk.feedback_loop ? "declared (EXT layout + dynamic state, unified layouts)"
                                                : g_vk.fbl_layout ? "layout extension only (FEEDBACK_LOOP_OPTIMAL while sampled)" : "off");
    // VK_KHR_maintenance5: points drawn by a VS that writes no PointSize are 1.0 wide with it, undefined without
    // (AMD / Intel may draw nothing or large points); without it the translator writes 1.0 itself
    // (maxwell_vs_pointsize). Its feature bit is checked with the others below.
    g_vk.maint5 = has_ext(ep, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    if (g_vk.maint5) ext[n_ext++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    vk_present_device_exts(ep, ne, ext, &n_ext);   // present.c: present id / wait, HDR metadata (when used)
    vk_ngx_device_exts(ep, ne, ext, &n_ext, 40);   // ngx.c: what DLSS asks for on this GPU (TOTK_DLSS only)
    free(ep);

    // features the backend relies on. Every bit enabled below is one the device reports (present's audit, 2026-10-27: an
    // unsupported bit fails vkCreateDevice = no game): an extension whose feature is missing is dropped to its fallback,
    // a core feature takes the device's value (warned when the translator's output needs it).
    VkPhysicalDeviceFaultFeaturesEXT have_ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT have_cbc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT have_div = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have13};
    VkPhysicalDeviceVulkan11Features have11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &have12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have11};
    // (an extension's struct only when the device has the extension: VUID-VkPhysicalDeviceFeatures2-pNext)
    if (fault) { have_ff.pNext = have13.pNext; have13.pNext = &have_ff; }
    if (g_vk.custom_border) { have_cbc.pNext = have13.pNext; have13.pNext = &have_cbc; }
    if (g_vk.divisor) { have_div.pNext = have13.pNext; have13.pNext = &have_div; }
    if (g_vk.gpl) { have_gpl.pNext = have13.pNext; have13.pNext = &have_gpl; }
    VkPhysicalDeviceMaintenance5FeaturesKHR have_m5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if (g_vk.maint5) { have_m5.pNext = have13.pNext; have13.pNext = &have_m5; }
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    if (g_vk.maint5 && !have_m5.maintenance5) { g_vk.maint5 = false; drop_ext(ext, &n_ext, VK_KHR_MAINTENANCE_5_EXTENSION_NAME); }
    extern bool maxwell_vs_pointsize;
    maxwell_vs_pointsize = !g_vk.maint5;
    LOG("[vk] maintenance5: %s\n", g_vk.maint5 ? "on (unwritten point sizes are 1.0)" : "off: vertex shaders write gl_PointSize 1.0");
    if (g_vk.custom_border && !(have_cbc.customBorderColors && have_cbc.customBorderColorWithoutFormat)) {
        g_vk.custom_border = false;
        drop_ext(ext, &n_ext, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        LOG("[vk] custom border colours: the extension without its features: off (built-in border colours)\n");
    }
    if (g_vk.divisor && !have_div.vertexAttributeInstanceRateDivisor) {
        g_vk.divisor = false;
        drop_ext(ext, &n_ext, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        LOG("[vk] vertex attribute divisor: the extension without its feature: off\n");
    }
    if (g_vk.gpl && !have_gpl.graphicsPipelineLibrary) {
        g_vk.gpl = false;
        drop_ext(ext, &n_ext, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        drop_ext(ext, &n_ext, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (vk_dyn_clamp) { vk_dyn_clamp = false; drop_ext(ext, &n_ext, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME); }
        LOG("[vk] graphics pipeline library: the extension without its feature: off (monolithic pipelines, no dynamic depth clamp)\n");
    }
    if (fault && !have_ff.deviceFault) {
        fault = g_fault_ext = false;
        drop_ext(ext, &n_ext, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    {   // the 1.1-1.3 features the backend and the translator's output use
        // (no emitted shader uses 8- or 16-bit types: shaderInt8 and the 8 / 16-bit storage bits are not enabled)
        static const char *const nm[] = {"dynamicRendering", "synchronization2", "shaderDemoteToHelperInvocation",
                                         "shaderTerminateInvocation", "maintenance4", "timelineSemaphore",
                                         "samplerMirrorClampToEdge", "scalarBlockLayout"};
        const VkBool32 v[] = {have13.dynamicRendering, have13.synchronization2, have13.shaderDemoteToHelperInvocation,
                              have13.shaderTerminateInvocation, have13.maintenance4, have12.timelineSemaphore,
                              have12.samplerMirrorClampToEdge, have12.scalarBlockLayout};
        for (int i = 0; i < (int)(sizeof v / sizeof v[0]); i++)
            if (!v[i]) LOG("[vk] warning: the device has no %s (left off; %s)\n", nm[i],
                           i < 6 ? "the backend needs it: expect failures" : "shaders that use it will not build");
    }
    g_vk.sg32_stages = have13.subgroupSizeControl ? sg_pinnable : 0;
    g_vk.max_wg_subgroups = p13.maxComputeWorkgroupSubgroups;
    if (!sg_fixed32) {
        VkShaderStageFlags narrow = p13.minSubgroupSize < 32 ? sg_used & ~g_vk.sg32_stages : 0;
        LOG("[vk] subgroups %u..%u wide: warp ops required 32 wide in stages 0x%x, wave-safe forms elsewhere%s\n", p13.minSubgroupSize,
            p13.maxSubgroupSize, g_vk.sg32_stages, narrow ? "" : " (every stage covered)");
        if (narrow) LOG("[vk] warning: stages 0x%x may run subgroups narrower than 32: shaders with warp ops (vote / ballot / shuffle / "
                        "lane masks) are wrong there\n", narrow);
    }
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = have13.dynamicRendering, .synchronization2 = have13.synchronization2,
                                            .shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation,
                                            .shaderTerminateInvocation = have13.shaderTerminateInvocation,
                                            .maintenance4 = have13.maintenance4, .subgroupSizeControl = g_vk.sg32_stages != 0,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    // separate depth / stencil layouts (core 1.2, optional; every desktop 1.3 driver has it): stencil-only barriers on a
    // combined depth + stencil image, which textures.c's S8_UINT substitute (D32S8 / D24S8 read through its stencil
    // aspect) needs; without it that substitute is not made
    // (have12, queried above with the other features)
    g_vk.sep_ds_layouts = have12.separateDepthStencilLayouts;
    g_vk.host_qreset = have12.hostQueryReset;
    g_vk.mirror_clamp = have12.samplerMirrorClampToEdge;   // (textures.c: MIRROR_CLAMP_TO_EDGE samplers mirror-repeat without it)
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = have12.timelineSemaphore, .hostQueryReset = g_vk.host_qreset,
                                            .samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge,
                                            .scalarBlockLayout = have12.scalarBlockLayout, .separateDepthStencilLayouts = g_vk.sep_ds_layouts,
                                            .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .pNext = &f12};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE};
    if (g_vk.gpl) { gplf.pNext = f13.pNext; f13.pNext = &gplf; }
    if (g_vk.feedback_loop) { uil.pNext = f13.pNext; f13.pNext = &fll; }   // (fll -> fld -> uil, all queried true)
    else if (g_vk.fbl_layout) { fll.attachmentFeedbackLoopLayout = VK_TRUE; fll.pNext = f13.pNext; f13.pNext = &fll; }   // (the layout feature alone)
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
                                                              .extendedDynamicState3DepthClampEnable = VK_TRUE};
    if (vk_dyn_clamp) { eds3f.pNext = f13.pNext; f13.pNext = &eds3f; }
    VkPhysicalDeviceMaintenance5FeaturesKHR m5f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .maintenance5 = VK_TRUE};
    if (g_vk.maint5) { m5f.pNext = f13.pNext; f13.pNext = &m5f; }
    f13.pNext = vk_present_features(f13.pNext);
    // vendor binary = the driver's own GPU crash dump (NVIDIA 617: supported), saved by vk_device_fault_report
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .deviceFaultVendorBinary = have_ff.deviceFaultVendorBinary, .pNext = &f11};
    g_fault_binary = have_ff.deviceFaultVendorBinary;
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    g_vk.sampler_aniso = h->samplerAnisotropy;   // (textures.c: no anisotropic filtering without it)
    b->textureCompressionBC = h->textureCompressionBC;
    // ASTC sampled natively where the GPU has it (Intel integrated GPUs: no decode, 4-8x less VRAM than the RGBA8 copy);
    // creating ASTC images needs the feature enabled (until now only the format query was checked, which is invalid
    // usage). LARECOMP_ASTC_NATIVE=0: decode to RGBA8 everywhere, as on desktop GPUs.
    g_vk.astc_ldr = h->textureCompressionASTC_LDR && !(getenv("LARECOMP_ASTC_NATIVE") && *getenv("LARECOMP_ASTC_NATIVE") == '0');
    b->textureCompressionASTC_LDR = g_vk.astc_ldr;
    b->shaderInt64 = h->shaderInt64;
    b->shaderFloat64 = h->shaderFloat64;  // DADD/DMUL/DFMA/F2F.F64 ('material' VS with double-precision positions)
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    g_vk.fill_nonsolid = h->fillModeNonSolid;   // (exec.c: wireframe draws fill without it)
    g_vk.dual_src_blend = h->dualSrcBlend;      // (exec.c: SRC1 blend factors fall back to SRC0's)
    g_vk.depth_clamp = h->depthClamp;           // (exec.c: clamped draws clip without it)
    if (!h->fillModeNonSolid || !h->dualSrcBlend || !h->depthClamp)
        LOG("[vk] warning: no%s%s%s: those draws use a fallback\n", h->fillModeNonSolid ? "" : " fillModeNonSolid",
            h->dualSrcBlend ? "" : " dualSrcBlend", h->depthClamp ? "" : " depthClamp");
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    LOG("[vk] ASTC textures: %s\n", g_vk.astc_ldr ? "native (textureCompressionASTC_LDR)"
        : h->textureCompressionASTC_LDR ? "decoded to RGBA8 (LARECOMP_ASTC_NATIVE=0)" : "decoded to RGBA8 (no ASTC on this GPU)");
    extern bool maxwell_soft_f64;   // (translate.c: F64 ops as soft_f64.h's IEEE functions on uint64_t)
    maxwell_soft_f64 = !h->shaderFloat64 || (getenv("LARECOMP_SOFT_F64") && strcmp(getenv("LARECOMP_SOFT_F64"), "0"));
    if (maxwell_soft_f64)
        LOG("[vk] double precision in shaders: software (soft_f64.h, %s)\n", h->shaderFloat64 ? "LARECOMP_SOFT_F64" : "no shaderFloat64");
    extern uint32_t maxwell_smem_bytes;   // (translate.c: the compute shared memory each program declares)
    if ((g_vk.props.limits.maxComputeSharedMemorySize & ~15u) < maxwell_smem_bytes) {
        maxwell_smem_bytes = g_vk.props.limits.maxComputeSharedMemorySize & ~15u;
        LOG("[vk] compute shared memory: %u KiB on this GPU (Maxwell: 48): programs declare that much\n", maxwell_smem_bytes >> 10);
    }
    float prio[2] = {1.0f, 1.0f};
    // with the FG present thread asked for (present.c, TOTK_FG_PRESENT_THREAD=1, with DLSS), a second queue of the
    // family: its presents never share the render thread's queue
    uint32_t nqueue = 1;
    if ((getenv("TOTK_DLSS") || getenv("TOTK_DLSS_FG")) && vk_present_queue_wanted()) {
        VkQueueFamilyProperties fp[16];
        uint32_t nf = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nf, fp);
        if (g_vk.qfam < nf && fp[g_vk.qfam].queueCount >= 2) nqueue = 2;
    }
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = nqueue, .pQueuePriorities = prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VkResult dr = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev);
    if (dr != VK_SUCCESS) {   // (which request: every feature bit above is a queried one, so this names the culprit kind)
        char el[2048] = "";
        for (uint32_t i = 0, o = 0; i < n_ext && o < sizeof el - 80; i++) o += (uint32_t)snprintf(el + o, sizeof el - o, " %s", ext[i]);
        LOG("[vk] vkCreateDevice failed: %d (%s); extensions:%s\n", dr,
            dr == VK_ERROR_FEATURE_NOT_PRESENT ? "a feature bit the device does not report" :
            dr == VK_ERROR_EXTENSION_NOT_PRESENT ? "an extension the device does not have" : "see the driver", el);
        char why[96];
        snprintf(why, sizeof why, "its driver refused the device the game needs (vkCreateDevice %d)", dr);
        gpu_unsupported(g_vk.props.deviceName, why);
    }
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    VkQueue pq = VK_NULL_HANDLE;
    if (nqueue == 2) vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 1, &pq);
    vk_present_queue_init(pq);
    vk_present_device_ready();
    vk_ngx_device_ready();   // ngx.c: NGX on our device, capability map logged (TOTK_DLSS only)

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save): the file read and checked by
    // pcache_read, on its thread since ui_window_init (overlapping the window, the instance and the device) or here
    extern bool vk_archive_warm_stale(void);   // (exec.c: new shader generation -> the old entries are dead weight)
    extern void vk_archive_warm_reset(void);
    bool stale = vk_archive_warm_stale();
    const bool prefetched = g_pc_thread_on;
    const uint64_t pc_w0 = SDL_GetTicksNS();
    if (prefetched) { pthread_join(g_pc_thread, NULL); g_pc_thread_on = false; }
    else { pcache_path(g_pc_read.path, sizeof g_pc_read.path); pcache_read(&g_pc_read); }
    const uint64_t pc_w1 = SDL_GetTicksNS();
    void *data = g_pc_read.data;
    size_t sz = g_pc_read.sz;
    g_pc_read.data = NULL;
    if (stale) {   // (read meanwhile on the thread: dropped unseen)
        LOG("[vk] pipeline cache: archive warm-up stamp changed: %s not loaded (previous shader generation), starting empty\n", pcache_file());
        free(data);
        data = NULL;
        sz = 0;
        remove(pcache_file());     // (dead weight; this session's saves replace it)
        vk_archive_warm_reset();   // so the next launch loads what this session saves
    }
    {
        // Written by another GPU or driver (an update, a second GPU): the driver would drop it in silence, so say so and
        // start empty; this session's saves replace the file (Vulkan spec: VkPipelineCacheHeaderVersionOne)
        VkPipelineCacheHeaderVersionOne h;
        if (sz && sz >= sizeof h) memcpy(&h, data, sizeof h);
        if (sz && (sz < sizeof h || h.headerSize < sizeof h || h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                   h.vendorID != g_vk.props.vendorID || h.deviceID != g_vk.props.deviceID ||
                   memcmp(h.pipelineCacheUUID, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE))) {
            LOG("[vk] pipeline cache: %s (%zu KB) is from another GPU or driver (vendor %04x device %04x): starting empty\n",
                pcache_file(), sz >> 10, sz >= sizeof h ? h.vendorID : 0, sz >= sizeof h ? h.deviceID : 0);
            sz = 0;
        }
    }
    const uint64_t pc_t2 = SDL_GetTicksNS();
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    const uint64_t pc_t3 = SDL_GetTicksNS();
    // (read and check: on the thread when prefetched; waited: what of them was still left at this point)
    LOG("[vk] pipeline cache: %zu KB loaded from %s (read %.1f ms, check %.1f ms%s, waited %.1f ms, driver %.1f ms)\n",
        sz >> 10, pcache_file(), g_pc_read.read_ns / 1e6, g_pc_read.check_ns / 1e6,
        prefetched ? " on a thread from window init" : "", (pc_w1 - pc_w0) / 1e6, (pc_t3 - pc_t2) / 1e6);
    pcache_saved = sz;
    if (pc.initialDataSize) g_vk_pcache_loaded = sz;
    free(data);
    {   // tmp files of killed boots (one per pid now, up to the cache's size each); 15 min old = no live writer
        char pat[96];
        snprintf(pat, sizeof pat, "%s.*tmp", pcache_file());
        shim_remove_old_files(pat, 15 * 60);
    }
    // The cache used to be written only by quick_exit_handler: test runs end by timeout and the user's window close did
    // not reach it, so cache/vk_pipelines.bin never existed (2026-10-15: only a half-written.tmp) and every launch
    // compiled every pipeline again. Saved from a background thread when the cache grew, on pcache_saver's schedule
    // (VkPipelineCache is internally synchronized).
    static pthread_t saver;
    pthread_create(&saver, NULL, pcache_saver, NULL);
    pthread_detach(saver);
#ifndef _WIN32
    static pthread_t watch;
    pthread_create(&watch, NULL, hang_watch, NULL);
    pthread_detach(watch);
#endif
}

static pthread_mutex_t pcache_save_lock = PTHREAD_MUTEX_INITIALIZER;

void vk_pipeline_cache_save(void) {
    pthread_mutex_lock(&pcache_save_lock);
    size_t sz = 0;
    uint32_t warm_n = vk_archive_warm_snapshot();   // archive programs warmed so far: inside the data read below
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz || sz == pcache_saved) {
        if (sz && sz == pcache_saved && pcache_saved) vk_archive_warm_commit(warm_n);   // unchanged since the file
        pthread_mutex_unlock(&pcache_save_lock);
        return;
    }
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", pcache_file(), (unsigned long)getpid());
        FILE *f = fopen(tmp, "wb");
        if (f) {
            PcacheTrailer tr = {.size = sz, .hash = pcache_hash(data, sz)};
            memcpy(tr.magic, PCACHE_MAGIC, sizeof tr.magic);
            bool ok = fwrite(data, 1, sz, f) == sz && fwrite(&tr, sizeof tr, 1, f) == 1;
            ok = !fclose(f) && ok;
            // one step (no remove + rename window); fails while another boot has the file open: retried next tick
            if (ok) ok = shim_file_replace(tmp, pcache_file()) == 0;
            if (!ok) remove(tmp);
            if (ok) { pcache_saved = sz; vk_archive_warm_commit(warm_n); }
            static int nlog;
            if (nlog++ < 8) LOG("[vk] pipeline cache saved: %zu KB%s\n", sz >> 10, ok ? "" : " (FAILED)");
        }
    }
    free(data);
    pthread_mutex_unlock(&pcache_save_lock);
}

static void *pcache_saver(void *arg) {
    (void)arg;
    cpu_place_thread(CPU_HELPER);   // (file I/O nothing waits on: the E-cores / the other CCD, cpu_place.h)
#ifndef _WIN32
    { extern int pthread_setname_np(pthread_t, const char *); pthread_setname_np(pthread_self(), "vk-pcache-save"); }   // ([cpu] line)
#endif
    extern int vk_precompile_progress(int *done, int *total);
    extern int vk_precompile_fills_pcache(void);   // (the archive's warm-up into the driver's cache: not "running" here)
    // Every save copies the whole cache into RAM and rewrites the file: 1.8 GB, 2.6 GB with the effect shaders half warmed.
    // The user's 21:26 session wrote 1.8 GB 8 times in ~6 min of play (5 of the 8 stats windows after a save under 55 fps vs
    // 30 % overall: weak but suspicious). Now: every 3 min while the precompile fills this cache (the recorded part; the
    // archive's warm-up only as =pcache / =full: exec.c vk_precompile_fills_pcache), once when it ends, then every 2 min
    // only if the cache grew >= 16 MB (any growth: 10 min). The exit handler still saves. Startup (present workstream,
    // 2026-10-24): a precompile from a warm cache ends in ~1.3 s, between two 10-s ticks, so its end was never seen, and a
    // session closed or killed in its first 2 minutes saved nothing (every 90-s boot loaded 0 KB). Now 2-s ticks: the end of
    // any precompile (pc_running 0 with work done) saves, and in the first 5 minutes every 30 s saves when the cache grew by
    // 2 MB or 1/16 of the file (a 1.8 GB cache: 112 MB).
    bool was_running = false, pc_saved = false;
    int since = 0;   // 2-s ticks since the last save
    for (int n = 0;; n++) {
        SDL_Delay(2000);
        since++;
        int d, t;
        if (vk_precompile_progress(&d, &t) && vk_precompile_fills_pcache()) {
            was_running = true;
            if (n % 90 == 89) { vk_pipeline_cache_save(); since = 0; }
            continue;
        }
        if (was_running || (t > 0 && d >= t && !pc_saved)) {
            was_running = false; pc_saved = true;
            vk_pipeline_cache_save(); since = 0;
            continue;
        }
        const bool early = n < 150;
        if (since < (early ? 15 : 60) || since % 5) continue;   // (the size query every 10 s at most)
        size_t sz = 0;
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS) continue;
        size_t step = early ? (pcache_saved / 16 > ((size_t)2 << 20) ? pcache_saved / 16 : (size_t)2 << 20) : (size_t)16 << 20;
        if (sz >= pcache_saved + step || (sz != pcache_saved && since >= 300)) { vk_pipeline_cache_save(); since = 0; }
    }
    return NULL;
}

// ================================================================== swapchain
// nxrecomp overlay (pc_settings.cpp): the mode chosen in game, over LARECOMP_VK_PRESENT (-1: none), so the overlay
// never edits the environment while the game runs
static atomic_int g_present_override = -1;
void vk_set_present_mode(int mode) { atomic_store(&g_present_override, mode); }

static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission queued
    // behind the one waiting for a window image, so the game's fence waits followed the presentation engine: a
    // throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox: 60).
    // No tearing. LARECOMP_VK_PRESENT=fifo|fifo_relaxed|immediate|mailbox to choose (immediate and fifo_relaxed
    // tear: only when asked).
    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                          : !strcmp(e, "fifo_relaxed") ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (atomic_load(&g_present_override) >= 0) want = (VkPresentModeKHR)atomic_load(&g_present_override);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkImageUsageFlags g_sc_usage;   // what the window images allow (create_swapchain: the surface's supported bits)
static void create_swapchain(void) {
    vk_present_queue_drain();   // (FG present thread: nothing of the old swapchain left to present)
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    // (B8G8R8A8_UNORM everywhere on Windows; R8G8B8A8_UNORM, also 8-bit UNORM, before whatever the driver lists first)
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = fmts[i];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    vk_present_pick_format(fmts, nf, &sf);   // present.c: HDR10 / scRGB when asked and offered
    uint32_t count = vk_present_image_count(&caps);   // (TOTK_SWAPCHAIN_IMAGES; default minImageCount + 1, at most 8)
    VkSwapchainKHR old = g_vk.swapchain;
    VkPresentModeKHR mode = present_mode();
    // (what the surface allows: every Windows driver has these, but say so where one does not instead of failing
    // vkCreateSwapchainKHR; the screenshot / window-shot paths copy from the images, the present pass may blit into them)
    VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & use) != use) {
        static int said;
        if (!said++) LOG("[vk] warning: the window surface allows image usage 0x%x of 0x%x: screenshots / blits into the window may fail\n",
                         caps.supportedUsageFlags & use, use);
        use &= caps.supportedUsageFlags | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .pNext = vk_present_swapchain_pnext(),   // (exclusive fullscreen)
        .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = (g_sc_usage = use),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = old};
    vk_swapchain_life_lock();   // (present.c: the low-latency calls on the game thread use the swapchain shared)
    vk_swapchain_lock();   // (oldSwapchain is externally synchronised too: not while the waiter polls it)
    VkResult cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    if (cr != VK_SUCCESS && (ci.pNext || old)) {
        // A refused exclusive-capable swapchain was fatal. Once more as a plain (borderless) one: the chain without
        // exclusive, and no oldSwapchain (the failed call retired it; it is destroyed below as usual). Exclusive is tried
        // again when the window is next activated.
        LOG("[vk] vkCreateSwapchainKHR %d: again without exclusive and without the old swapchain\n", cr);
        extern void vk_present_exclusive_refused(void);
        vk_present_exclusive_refused();
        ci.pNext = vk_present_swapchain_pnext(), ci.oldSwapchain = VK_NULL_HANDLE;
        cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    }
    // A driver may make more images than asked for (minImageCount is a minimum); the per-image arrays here and in
    // present.c hold 8: ask again for fewer, down to the surface's minimum
    for (uint32_t got = 0; cr == VK_SUCCESS && vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &got, NULL) == VK_SUCCESS && got > 8;) {
        LOG("[vk] the driver made %u window images for %u asked (8 at most here): asking for fewer\n", got, ci.minImageCount);
        if (ci.minImageCount <= caps.minImageCount) FATAL("the window surface needs more than 8 swapchain images (%u)\n", got);
        uint32_t less = got - 8 < ci.minImageCount - caps.minImageCount ? got - 8 : ci.minImageCount - caps.minImageCount;
        ci.minImageCount -= less;
        ci.oldSwapchain = g_vk.swapchain;
        VkSwapchainKHR more = g_vk.swapchain;
        cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
        vkDestroySwapchainKHR(g_vk.dev, more, NULL);
    }
    vk_swapchain_unlock();
    VK_CHECK(cr);
    vk_present_swapchain_retire();   // (no present wait left on the old one; HDR: the real views back in sc_views)
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) { vk_swapchain_lock(); vkDestroySwapchainKHR(g_vk.dev, old, NULL); vk_swapchain_unlock(); }
    vk_swapchain_life_unlock();
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
    vk_present_swapchain_ready(sf, (int)mode);
}

// nxrecomp overlay (pc_settings.cpp): a new presentation mode (LARECOMP_VK_PRESENT) takes a new swapchain
static atomic_int g_sc_recreate;
void vk_request_swapchain_recreate(void) { atomic_store(&g_sc_recreate, 1); }

extern void vkq_drain(void);   // (LARECOMP_VKQ_SUBMIT: the record thread may hold queued submits; VkQueue is externally synchronised)
static void recreate_swapchain(void) {
    vk_present_queue_drain();   // (vkDeviceWaitIdle: every queue to itself)
    vkq_drain();
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) { atomic_store(&g_last_frame_ns, SDL_GetTicksNS()); return false; }  // minimised (not a hang)
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        atomic_exchange(&g_sc_recreate, 0)) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r;
    for (uint64_t a0 = SDL_GetTicksNS();;) {   // (UINT64_MAX before: a compositor that stops releasing images = a silent hang)
        // (present.c: polled under the swapchain lock, which the FG present thread's presents take too)
        g_rt_op = "vkAcquireNextImageKHR", g_rt_op_ns = SDL_GetTicksNS();
        r = vk_present_acquire(acquire_sem[slot], 2000000000ull, &cur_frame.image);
        g_rt_op = NULL;
        if (r != VK_TIMEOUT && r != VK_NOT_READY) break;
        LOG("[hang] no window image from the compositor for %.0f s (vkAcquireNextImageKHR)\n", (SDL_GetTicksNS() - a0) / 1e9);
        fflush(stdout);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        if (r != VK_ERROR_OUT_OF_DATE_KHR) vk_present_lost();
        recreate_swapchain();
        r = vk_present_acquire(acquire_sem[slot], UINT64_MAX, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    { extern void vk_dyn_invalidate(void); vk_dyn_invalidate(); }
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    { extern void vk_dyn_invalidate(void); vk_dyn_invalidate(); }
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// Copies a BGRA text block (hle_osd.cpp) into the window image at (x, y); skipped when it does not fit.
static void osd_blit(VkCommandBuffer cb, const uint32_t *px, int bw, int bh, int x, int y) {
    if (x < 0 || y < 0 || x + bw > (int)g_vk.sc_extent.width || y + bh > (int)g_vk.sc_extent.height) return;
    if (!(g_sc_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;   // (a surface whose images cannot be copied into)
    VkSpan s = vk_transient_copy(px, (uint64_t)bw * bh * 4, 16);
    if (g_vk.sc_format != VK_FORMAT_B8G8R8A8_UNORM && g_vk.sc_format != VK_FORMAT_B8G8R8A8_SRGB)
        for (uint32_t *q = (uint32_t *)s.ptr, *e = q + (size_t)bw * bh; q < e; q++)   // RGBA window image
            *q = (*q & 0xff00ff00u) | (*q >> 16 & 0xffu) | (*q & 0xffu) << 16;
    VkImage im = g_vk.sc_images[cur_frame.image];
    image_barrier(cb, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {x, y, 0}, .imageExtent = {(uint32_t)bw, (uint32_t)bh, 1}};
    vkCmdCopyBufferToImage(cb, s.buf, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    image_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
static char g_fps_text[96];   // backend_present's 0.5 s fps line (window title + fps readout)

static uint64_t total_frames;   // (tentative: the frame counters are below, with backend_present)
static bool g_game_present;     // vk_frame_end called from backend_present (a game frame; not backend_clear_window)
static uint64_t g_note_game_ns;  // backend_present: the game present being shown (0: none, a re-present)
static bool g_note_gen;          // and whether this window present is DLSS-G's generated frame
static int g_fg_k, g_fg_n;       // (generated frame k of n; for the real one, n = how many came before it)

// Periodic shots (TOTK_SHOT_EVERY, boot.py --shots) are converted and written on their own thread: SDL_SavePNG on the
// render thread made the present wait for the disk (presents of 0.5 s and 3.9 s, each on the frame before a saved
// window_<N>.png; the §9 45-189 ms "screenshot saves"). One shot in flight: one asked for while the previous is still
// being written is skipped and logged, as F12's pc_shot.cpp does. Written to <path>.tmp and renamed, so a boot ended
// mid-write leaves no half PNG for boot.py's compress_shots. LARECOMP_SHOT_SYNC=1: as before. The log keeps its
// contract: "saved" is printed at the capture, on the render thread, and "[vk] written <path>" when the PNG is on disk.
// TOTK_SHOT_RANGE's frames and LARECOMP_WINDOW_SHOT's shot are saved synchronously (g_shot_async false).
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
static bool g_shot_async;   // render thread: the shot vk_frame_end takes now is a periodic one (TOTK_SHOT_EVERY)
static pthread_mutex_t shotw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shotw_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *shotw_px;   // the shot being written (NULL: the writer is free)
static uint32_t shotw_w, shotw_h;
static bool shotw_bgr;
static char shotw_path[512];
static void shot_convert(uint8_t *px, uint32_t w, uint32_t h, bool bgr) {   // BGRA -> RGBA, opaque
    for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
}
static void *shotw_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&shotw_mu);
        while (!shotw_px) pthread_cond_wait(&shotw_cv, &shotw_mu);
        uint8_t *px = shotw_px;
        uint32_t w = shotw_w, h = shotw_h;
        bool bgr = shotw_bgr;
        char path[512], tmp[520];
        memcpy(path, shotw_path, sizeof path);
        pthread_mutex_unlock(&shotw_mu);
        shot_convert(px, w, h, bgr);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, px, (int)w * 4);
        if (!s || !SDL_SavePNG(s, tmp)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
        else {
            remove(path);
            if (rename(tmp, path)) LOG("[vk] saving %s failed: rename\n", path);
            else LOG("[vk] written %s\n", path);   // ("saved" was logged at the capture: shot_save)
        }
        SDL_DestroySurface(s);
        free(px);
        pthread_mutex_lock(&shotw_mu);
        shotw_px = NULL;
        pthread_mutex_unlock(&shotw_mu);
    }
    return NULL;
}
static void shot_save(const char *path, uint8_t *px, uint32_t w, uint32_t h, bool bgr) {
    static int sync = -1;
    if (sync < 0) sync = getenv("LARECOMP_SHOT_SYNC") && *getenv("LARECOMP_SHOT_SYNC") == '1';
    if (sync || !g_shot_async) { shot_convert(px, w, h, bgr); vk_save_png(path, px, w, h); return; }
    static bool started;
    pthread_mutex_lock(&shotw_mu);
    if (shotw_px) {
        pthread_mutex_unlock(&shotw_mu);
        static uint64_t skipped;
        if (skipped++ < 20) LOG("[vk] shot %s skipped: the previous one is still being written\n", path);
        return;
    }
    uint8_t *copy = malloc((size_t)w * h * 4);
    if (!copy) { pthread_mutex_unlock(&shotw_mu); LOG("[vk] shot %s skipped: no memory\n", path); return; }
    memcpy(copy, px, (size_t)w * h * 4);
    if (!started) {
        pthread_t th;
        started = pthread_create(&th, NULL, shotw_main, NULL) == 0;
        if (started) pthread_detach(th);
        else { pthread_mutex_unlock(&shotw_mu); shot_convert(copy, w, h, bgr); vk_save_png(path, copy, w, h); free(copy); return; }
    }
    shotw_px = copy, shotw_w = w, shotw_h = h, shotw_bgr = bgr;
    snprintf(shotw_path, sizeof shotw_path, "%s", path);
    pthread_cond_signal(&shotw_cv);
    pthread_mutex_unlock(&shotw_mu);
    LOG("[vk] saved %s (%ux%u)\n", path, w, h);   // (vk_save_png's line, at the capture: the step it follows)
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!(g_sc_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && shot_path) {   // (window images that cannot be copied from: no shot)
        static int said;
        if (!said++) LOG("[vk] window shots skipped: the window surface does not allow copies out of its images\n");
        shot_path = NULL;
    }
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    // nxrecomp: F12 (pc_shot.cpp) saves the game image as it is before the banner, fps readout and menu are drawn on it.
    // vk_readback is one buffer at offset 0, so a frame that saves shot_path keeps the F12 request for the next frame.
    VkSpan pc_shot = {0};
    bool pc = !shot_path && pc_shot_take() && (g_sc_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);   // (see the window shots above)
    uint32_t pc_w = g_vk.sc_extent.width, pc_h = g_vk.sc_extent.height;   // (a swapchain recreate at present changes them)
    if (pc) save_window_image(cb, &pc_shot);
    {   // runtime option banner (hle_osd.cpp: "Skip opening: ON/OFF" while the title screen is up), bottom left
        extern const uint32_t *osd_banner(int target_h, int *w, int *h);
        int bw = 0, bh = 0;
        const uint32_t *px = osd_banner((int)g_vk.sc_extent.height, &bw, &bh);
        if (px) osd_blit(cb, px, bw, bh, bh / 2, (int)g_vk.sc_extent.height - bh - bh / 2);
    }
    {   // shader precompile progress (hle_osd.cpp osd_precompile), bottom right
        extern const uint32_t *osd_precompile(int target_h, int *w, int *h);
        int bw = 0, bh = 0;
        const uint32_t *px = osd_precompile((int)g_vk.sc_extent.height, &bw, &bh);
        if (px) osd_blit(cb, px, bw, bh, (int)g_vk.sc_extent.width - bw - bh / 3, (int)g_vk.sc_extent.height - bh - bh / 3);
    }
    {   // fps readout, top right (top left covers the hearts): while fullscreen (no title bar) or after F3 (shim_win.c)
        extern bool shim_fps_overlay(void);
        extern const uint32_t *osd_fps(const char *text, int target_h, int *w, int *h);
        int bw = 0, bh = 0;
        const uint32_t *px = shim_fps_overlay() ? osd_fps(g_fps_text, (int)g_vk.sc_extent.height, &bw, &bh) : NULL;
        if (px) osd_blit(cb, px, bw, bh, (int)g_vk.sc_extent.width - bw - bh / 3, bh / 3);
    }
    {   // Graphics menu (hle_osd.cpp: L3 + R3 / F10), centred
        extern const uint32_t *osd_menu(int target_h, int *w, int *h);
        int bw = 0, bh = 0;
        const uint32_t *px = osd_menu((int)g_vk.sc_extent.height, &bw, &bh);
        if (px) osd_blit(cb, px, bw, bh, ((int)g_vk.sc_extent.width - bw) / 2, ((int)g_vk.sc_extent.height - bh) / 2);
    }
    if (pc_overlay_enabled()) pc_overlay_vk_record();   // nxrecomp's F10 settings / F9 perf overlay, over the OSD (NXR_PC_OVERLAY=1)
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    // HDR probe (present.c, TOTK_HDR_PROBE=<frame>,...): the encode on this frame into offscreen HDR targets, saved below
    extern bool vk_present_hdr_probe(VkCommandBuffer cb, VkImage src, VkFormat src_fmt, uint64_t frame, bool busy);
    extern void vk_present_hdr_probe_save(void);
    const bool hdr_probe = vk_present_hdr_probe(cb, g_vk.sc_images[cur_frame.image], g_vk.sc_format, total_frames, shot_path || pc);
    // HDR (present.c): the frame above went into the SDR canvas; the encode pass writes the swapchain image
    if (!vk_present_hdr_encode(cb, cur_frame.image))
        image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    uint64_t t_submit = SDL_GetTicksNS();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    extern void vk_present_latency_present(int stage);   // (present.c: low latency markers of a game frame)
    if (g_game_present) vk_present_latency_present(0);
    if (g_game_present) vi_pace_present_point();   // pacing at the present (TOTK_PACE_AT): the cap's wait between the GPU submit and the present
    uint64_t t_paced = SDL_GetTicksNS();
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    uint64_t pid = vk_present_chain(&pi, g_note_gen);
    vk_present_note(pid, g_note_game_ns, g_note_gen);
    VkResult r;
    if (vk_present_queue_on()) {   // DLSS-G: the FG present thread presents it at its due time (present.c)
        vkq_drain();   // (the submit that signals done_sem is in the queue before the present waits on it)
        vk_present_queue_push(img, done_sem[img], pid, total_frames, seq, t_submit, g_game_present ? 1 : g_note_gen ? 2 : 0,
                              g_fg_k, g_fg_n);
        r = vk_present_queue_result();   // (what its earlier presents returned: out of date, suboptimal, lost)
    } else {
        g_rt_op = "vkQueuePresentKHR", g_rt_op_ns = SDL_GetTicksNS();
        if (g_game_present) vk_present_latency_present(1);
        vkq_drain();   // (LARECOMP_VKQ_SUBMIT: no deferred submit may race this queue call; returns at once when the ring is empty)
        vk_swapchain_lock();  // (the present-wait thread polls the same swapchain: present.c)
        r = vkQueuePresentKHR(g_vk.queue, &pi);
        vk_swapchain_unlock();
        if (g_game_present) vk_present_latency_present(2);
        g_rt_op = NULL;
        vk_present_done(total_frames, pid, seq, r, t_submit, t_paced);   // timing; queued-frame and frames-in-flight limits
    }
    if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) vk_present_lost();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) recreate_swapchain();
    cur_frame.ok = false;
    if (hdr_probe) { vk_wait_seq(seq); vk_present_hdr_probe_save(); }
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        shot_save(shot_path, shot.ptr, w, h, bgr);
    }
    if (pc) {
        vk_wait_seq(seq);
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        pc_shot_write(pc_shot.ptr, pc_w, pc_h, pc_w * 4, !bgr);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static uint64_t g_held_window;            // frames not shown in this stats window (backend_present hold)
static uint64_t g_shown_with_skips;       // frames shown although draws / dispatches were skipped (not held: LARECOMP_HOLD_MIN, a forced frame past 3 s)
static double g_last_shown_t, g_shown_gap_max;  // seconds: last frame actually presented, longest gap between them
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
static bool g_representing;
void backend_represent(void) {
    g_representing = true;
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
    g_representing = false;
}

// LARECOMP_MEMREPORT_AT=<frame>[,<frame>...] (Steam Deck: 16 GB shared by CPU and GPU, 2026-10-16): where the memory
// goes. Vulkan heaps (VMA blocks per heap; imported host memory such as the guest heap is not in VMA) and the process's
// committed memory: on Windows per reservation (top 14 private ones, so the guest heap, the alias window, the driver and
// host malloc can be told apart by base address), on Linux /proc/self/status.
#ifdef _WIN32
#include <psapi.h>
#endif
uint32_t vma_shim_heaps(VmaAllocator a, uint64_t *bytes, uint32_t *count, uint64_t *size, uint32_t *flags);  // shim_vma.c
static void vk_mem_report(void) {
    uint64_t hb[VK_MAX_MEMORY_HEAPS], hs[VK_MAX_MEMORY_HEAPS];
    uint32_t hc[VK_MAX_MEMORY_HEAPS], hf[VK_MAX_MEMORY_HEAPS];
    uint32_t nh = vma_shim_heaps(g_vk.vma, hb, hc, hs, hf);
    for (uint32_t h = 0; h < nh; h++)
        LOG("[mem] vk heap %u%s: %llu MB in %u allocations (shim; imported host memory not included), heap %llu MB\n", h,
            (hf[h] & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : " host", (unsigned long long)(hb[h] >> 20), hc[h],
            (unsigned long long)(hs[h] >> 20));
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pc = {.cb = sizeof pc};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc))
        LOG("[mem] process: private %llu MB (peak commit %llu MB), working set %llu MB (peak %llu MB)\n",
            (unsigned long long)(pc.PrivateUsage >> 20), (unsigned long long)(pc.PeakPagefileUsage >> 20),
            (unsigned long long)(pc.WorkingSetSize >> 20), (unsigned long long)(pc.PeakWorkingSetSize >> 20));
    enum { NTOP = 14 };
    struct { uintptr_t base; uint64_t commit, res; } top[NTOP] = {{0}}, cur = {0};
    uint64_t priv = 0, mapped = 0, image = 0, small = 0, nsmall = 0;
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000;; a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        int ok = VirtualQuery((void *)a, &m, sizeof m) == sizeof m && (uintptr_t)m.BaseAddress + m.RegionSize > a;
        uintptr_t ab = ok && m.State != MEM_FREE ? (uintptr_t)m.AllocationBase : 0;
        if (!ok || (ab && ab != cur.base)) {   // a reservation ended: rank it
            if (cur.commit) {
                if (cur.commit < (64ull << 20)) { small += cur.commit; nsmall++; }
                int i = NTOP;
                while (i > 0 && top[i - 1].commit < cur.commit) { if (i < NTOP) top[i] = top[i - 1]; i--; }
                if (i < NTOP) top[i] = cur;
            }
            cur.base = ab; cur.commit = cur.res = 0;
        }
        if (!ok) break;
        if (!ab) continue;
        cur.res += m.RegionSize;
        if (m.State != MEM_COMMIT) continue;
        if (m.Type == MEM_PRIVATE) { priv += m.RegionSize; cur.commit += m.RegionSize; }
        else if (m.Type == MEM_MAPPED) mapped += m.RegionSize;
        else image += m.RegionSize;
    }
    LOG("[mem] committed: private %llu MB (%llu MB in %llu reservations under 64 MB each), mapped %llu MB, image %llu MB\n",
        (unsigned long long)(priv >> 20), (unsigned long long)(small >> 20), (unsigned long long)nsmall,
        (unsigned long long)(mapped >> 20), (unsigned long long)(image >> 20));
    for (int i = 0; i < NTOP && top[i].commit; i++)
        LOG("[mem]   reservation %llx: %llu MB committed of %llu MB\n", (unsigned long long)top[i].base,
            (unsigned long long)(top[i].commit >> 20), (unsigned long long)(top[i].res >> 20));
    // heap blocks (malloc = the CRT's process heap; drivers may keep their own heaps or VirtualAlloc directly): busy bytes
    // per heap, and for blocks >= 1 MB the most common sizes (64 KB buckets), to tell what the many 4-5 MB reservations are
    HANDLE heaps[64];
    DWORD nheap = GetProcessHeaps(64, heaps);
    for (DWORD hi = 0; hi < nheap && hi < 64; hi++) {
        enum { NB = 4096 };
        static uint32_t bucket[NB];
        memset(bucket, 0, sizeof bucket);
        uint64_t busy = 0, nbusy = 0, big = 0, nbig = 0;
        PROCESS_HEAP_ENTRY e = {0};
        if (!HeapLock(heaps[hi])) continue;
        while (HeapWalk(heaps[hi], &e))
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                busy += e.cbData; nbusy++;
                if (e.cbData >= (1u << 20)) { big += e.cbData; nbig++; uint64_t b = e.cbData >> 16; bucket[b < NB ? b : NB - 1]++; }
            }
        HeapUnlock(heaps[hi]);
        if (busy < (16ull << 20)) continue;
        LOG("[mem] heap %p%s: %llu MB busy in %llu blocks, %llu MB in %llu blocks >= 1 MB\n", heaps[hi],
            heaps[hi] == GetProcessHeap() ? " (process heap)" : "", (unsigned long long)(busy >> 20), (unsigned long long)nbusy,
            (unsigned long long)(big >> 20), (unsigned long long)nbig);
        for (int k = 0; k < 6; k++) {
            int bi = -1;
            for (int b = 16; b < NB; b++) if (bucket[b] && (bi < 0 || bucket[b] > bucket[bi])) bi = b;
            if (bi < 0) break;
            LOG("[mem]   %u blocks of %llu-%llu KB\n", bucket[bi], (unsigned long long)bi * 64, (unsigned long long)bi * 64 + 63);
            bucket[bi] = 0;
        }
    }
#else
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS", 5) || !strncmp(line, "VmHWM", 5) || !strncmp(line, "RssAnon", 7) || !strncmp(line, "RssFile", 7) ||
            !strncmp(line, "RssShmem", 8) || !strncmp(line, "VmSwap", 6) || !strncmp(line, "VmData", 6))
            LOG("[mem] process %s", line);
    if (f) fclose(f);
#endif
}

void backend_present(NvnTexture *t) {
    extern uint64_t g_op_push_ns;   // (exec.c: push time of the render thread's current op, here the game's present)
    extern bool backend_async_on(void);
    uint64_t game_ns = g_representing ? 0 : backend_async_on() ? g_op_push_ns : SDL_GetTicksNS();
    extern void vk_preupload_frame(void);
    if (!g_representing) vk_preupload_frame();   // (preupload.c: the loading screen's textures, before this frame's submit)
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    {   // TOTK_DLSS_TRACE (exec.c): the image this frame presents
        static long tr = -2;
        if (tr == -2) { const char *e = getenv("TOTK_DLSS_TRACE"); tr = e ? atol(e) : -1; }
        if (tr >= 0 && renderer_present_count() >= (uint64_t)tr && renderer_present_count() <= (uint64_t)tr + 1)
            LOG("[dlss-trace] present %llu: presents %p %ux%u format %d\n", (unsigned long long)renderer_present_count(), (void *)t,
                b->width, b->height, (int)b->format);
    }
    b->presented = true;
    vk_late_latch_present();   // (latch.c: the frame's canvas, when no HUD draw came after its upscale)
    vk_end_pass();
    // ngx.c DLSS-G: TOTK_DLSS=fgtest replaces b's contents with the generated frame; TOTK_DLSS=fg presents the ngen
    // generated frames first, generated frame k at k / (ngen + 1) of a real-frame interval after the last real present,
    // then this frame as usual
    int ngen = vk_ngx_fg_frame(vk_cmd(), b);
    bool gen = ngen > 0;
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    // A frame that skipped draws or dispatches whose shaders / pipelines were still being built is not shown: the window
    // keeps the previous frame. Showing it made whole frames black when the skipped draw was a final pass; building
    // inline instead stalled 1.9-2.7 s. Workers build in parallel, so a burst of new pipelines costs a few held frames.
    // At most 30 frames in a row are held. LARECOMP_NO_HOLD=1: show them anyway (A/B). "[hold]" lines + "held N, shown
    // gap max" in the stats line.
    extern uint64_t g_skip_prog_frame, g_skip_pipe_frame, g_skip_comp_frame, g_draws_frame;
    static int hold_on = -1;
    if (hold_on < 0) hold_on = getenv("LARECOMP_NO_HOLD") == NULL;
    static bool shown_any;
    static uint64_t held_run, held_from, held_logs, held_sk[3];  // held_sk: skipped programs / pipelines / computes in the run
    // Only frames whose missing work matters are held (user 2026-10-15 "micro stutters": 1-2 skipped small draws held the
    // whole frame = a repeated frame). Held: any program / compute skip, any skipped fullscreen-ish draw (<= 6 vertices:
    // post passes, tile lighting), or more than LARECOMP_HOLD_MIN skipped pipelines. Default 0 since 2026-10-27: a frame
    // shown with draws missing is a visual change ("buildings disappear", 10-20), a held one only timing; exec.c fast-links
    // queued pipelines whose libraries are ready (LARECOMP_LINK_CLAIM) so fewer frames have a skip to hold for.
    // LARECOMP_HOLD_MIN=2: the 10-15 rule (up to 2 skipped pipelines shown).
    extern uint64_t g_skip_full_frame;
    static long hold_min = -1;
    if (hold_min < 0) { const char *e = getenv("LARECOMP_HOLD_MIN"); hold_min = e ? atol(e) : 0; }
    // TOTK_HITCH=skip (exec.c hitch_skip): only what cannot be skipped holds the frame: a fullscreen / tile pass (<= 6
    // vertices) missing its program or pipeline, or a compute dispatch; other draws are simply missing for a frame or two.
    extern int hitch_skip(void);
    extern uint64_t g_skip_prog_full;
    bool must = hitch_skip() ? (g_skip_comp_frame || g_skip_full_frame || g_skip_prog_full)
                             : (g_skip_prog_frame || g_skip_comp_frame || g_skip_full_frame || g_skip_pipe_frame > (uint64_t)hold_min);
    // After 30 held frames in a row the next frame is FORCED (exec.c g_hold_force): its draws and dispatches build or wait
    // for their programs / pipelines (<= 3 s each, "[bake] timeout" past that) and it is shown, complete, instead of the
    // old rule's 31st frame shown with its skips. A forced frame is always shown. LARECOMP_HOLD_FORCE=0: the old rule.
    static int hold_force = -1;
    if (hold_force < 0) { const char *e = getenv("LARECOMP_HOLD_FORCE"); hold_force = !e || strcmp(e, "0"); }
    extern bool g_hold_force;
    extern uint64_t g_hold_force_wait_ns;
    static uint64_t force_t0;   // (the present that armed the forced frame: its wall time runs from there to here)
    bool forced = g_hold_force;
    g_hold_force = false;
    bool held = hold_on && shown_any && must && !forced && (held_run < 30 || hold_force);
    if (!held && (g_skip_pipe_frame || g_skip_prog_frame || g_skip_comp_frame)) g_shown_with_skips++;
    static uint64_t forced_n;
    if (forced && forced_n++ < 200)  // (the freeze the zero-visual rule trades for: the §10 reads it from here)
        LOG("[hold] frame %llu forced after %llu held: shown %s, forced frame %.1f ms (waits / inline builds %.1f ms)\n",
            (unsigned long long)total_frames, (unsigned long long)held_run,
            must ? "with skips (a build past 3 s, a timed-out one or a failed one)" : "complete", (t0 - force_t0) / 1e6,
            g_hold_force_wait_ns / 1e6);
    g_hold_force_wait_ns = 0;
    extern uint64_t g_hold_keys[8];
    extern char g_hold_kinds[8];
    extern int g_hold_nkeys;
    static uint64_t run_keys[8];   // (the keys as of the run's last held frame: the shown frame that ends the run executes
    static char run_kinds[8];  // before this decision, and its own skips held nothing
    static int run_nkeys;
    if (held) {
        if (!held_run++) { held_from = total_frames; held_sk[0] = held_sk[1] = held_sk[2] = 0; }
        if (hold_force && held_run >= 30) { g_hold_force = true; force_t0 = t0; }   // (the next frame builds what it needs)
        held_sk[0] += g_skip_prog_frame; held_sk[1] += g_skip_pipe_frame; held_sk[2] += g_skip_comp_frame;
        g_held_window++;
        run_nkeys = g_hold_nkeys;
        memcpy(run_keys, g_hold_keys, sizeof run_keys);
        memcpy(run_kinds, g_hold_kinds, sizeof run_kinds);
    } else if (held_run) {
        if (held_logs++ < 400) {   // (+ what it waited for: kind and seed record key, exec.c hold_key_note; compute first;
                                   // 60 cut a 160 s fight boot off at frame ~3000 of 9000, the [vk] "held N" counts all)
            char w[8 * 20 + 1] = "";
            int o = 0;
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < run_nkeys; i++)
                    if ((run_kinds[i] == 'c') == !pass)
                        o += snprintf(w + o, sizeof w - o, " %c:%016llx", run_kinds[i], (unsigned long long)run_keys[i]);
            LOG("[hold] frames %llu..%llu not shown (%llu frames; skipped: program %llu, pipeline %llu, compute %llu)%s%s\n",
                (unsigned long long)held_from, (unsigned long long)total_frames - 1, (unsigned long long)held_run,
                (unsigned long long)held_sk[0], (unsigned long long)held_sk[1], (unsigned long long)held_sk[2],
                run_nkeys ? "; waited for" : "", w);
        }
        held_run = 0;
    }
    if (!held) g_hold_nkeys = 0;   // (a run collects its held frames' keys; shown frames start over)
    static uint64_t fg_last_real, fg_interval;   // (ns: last real present, running mean of the real intervals)
    // ("[dlss] FG presents" every 10 s: generated and real presents, the wait before a generated one, how many came late,
    // and the generated -> real gap, which should be about half the real interval)
    static uint64_t fg_n_gen, fg_n_real, fg_n_late, fg_wait_ns, fg_gap_ns, fg_gen_t, fg_log_t;
    int npass = gen && !held ? ngen : 0;   // (generated presents before the real one)
    if (npass) renderer_submit();   // (the frame and the generation run on the GPU while we wait to present)
    // FG present thread (present.c, TOTK_FG_PRESENT_THREAD=1): it places each present itself; nothing waits here
    const bool pq = vk_present_queue_on();
    bool have = false;
    for (int pass = 0; pass <= npass; pass++) {   // (pass < npass: generated frame pass + 1, TOTK_DLSS=fg; then the real one)
    bool real = pass == npass;
    if (npass) vk_ngx_fg_show(vk_cmd(), b, real ? 0 : pass + 1);
    g_fg_k = real ? 0 : pass + 1, g_fg_n = npass;
    if (!real && !pq && fg_last_real && fg_interval) {
        uint64_t due = fg_last_real + fg_interval * (uint64_t)(pass + 1) / (uint64_t)(npass + 1), now = SDL_GetTicksNS();
        if (due > now && due - now < 100000000ull) {   // (sleep to 2 ms before, then spin: our earlier Rust renderer's presenter)
            while (SDL_GetTicksNS() + 2000000ull < due) SDL_Delay(1);
            while (SDL_GetTicksNS() < due) {}
            fg_wait_ns += SDL_GetTicksNS() - now;
        } else fg_n_late++;
    }
    have = !held && vk_frame_begin(black);
    if (have && real) {
        shown_any = true;
        double ts = SDL_GetTicksNS() / 1e9;
        if (g_last_shown_t > 0 && ts - g_last_shown_t > g_shown_gap_max) g_shown_gap_max = ts - g_last_shown_t;
        g_last_shown_t = ts;
    }
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        if (real) widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        // the window's crop rect (nvnWindowSetCrop, guest pixels): TotK in handheld mode (TOTK_HANDHELD=1) renders 1280x720 into
        // the top-left of its 1920x1080 window textures and crops to it; without this the window showed that corner 1:1
        extern int g_present_crop[4];
        int pc[4]; memcpy(pc, g_present_crop, sizeof pc);
        if (pc[2] > 0 && pc[3] > 0 && t->width && t->height &&
            (pc[0] || pc[1] || (uint32_t)pc[2] != t->width || (uint32_t)pc[3] != t->height)) {
            crop[0] = (float)pc[0] / t->width; crop[1] = (float)pc[1] / t->height;
            crop[2] = (float)pc[2] / t->width; crop[3] = (float)pc[3] / t->height;
        }
        {  // the presented image's own shape: output_scale fits a 16:9 rect from the window alone
            // ultrawide canvases were squeezed into it (uw0-21: ~25 % thinner). Now a 21:9 / 32:9 image fills a window of
            // its shape, and any image keeps its aspect in any window (a 16:9 one pillarboxed in a wider window, as
            // before). 16:9 images keep output_scale's rect exactly. The fit is totk_hooks.c's output_fit_image, which the
            // menu cursor maps the pointer through too.
            output_set_image_aspect(t->width && t->height && crop[3] > 0 ? (double)t->width * crop[2] / ((double)t->height * crop[3]) : 0);
            OutScale f = {.vx = vx, .vy = vy, .vw = vw, .vh = vh};
            output_fit_image(dw, dh, &f);
            vx = f.vx, vy = f.vy, vw = f.vw, vh = f.vh;
        }
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        {   // TOTK_OUT_SCALE: the 1920x1080 output canvas at the window's resolution (textures.c kind 3)
            extern void renderer_set_out_scale(float s);
            double s = vh / 1080.0;
            renderer_set_out_scale((float)(s < 1.0 ? 1.0 : s > 4.0 ? 4.0 : s));
        }
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && real && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = real && win_shot && (long)total_frames + 1 == win_shot;
        // TotK: TOTK_SHOT_EVERY=N also saves build/window_<frame>.png every N frames (watch a scene progress in one run)
        static long shot_every = -1;
        if (shot_every < 0) shot_every = getenv("TOTK_SHOT_EVERY") ? atol(getenv("TOTK_SHOT_EVERY")) : 0;
        static char shot_path[64];
        const char *shot_to = shot ? "build/window.png" : NULL;
        // TOTK_SHOT_RANGE=a-b: every frame from a to b too (flicker that changes each frame)
        static long range_a = -1, range_b;
        if (range_a < 0) { const char *e = getenv("TOTK_SHOT_RANGE"); range_a = e ? strtol(e, (char **)&e, 10) : 0; range_b = e && *e == '-' ? atol(e + 1) : 0; }
        const bool in_range = range_b && (long)total_frames + 1 >= range_a && (long)total_frames + 1 <= range_b;
        if (!shot_to && real && ((shot_every > 0 && ((long)total_frames + 1) % shot_every == 0) || in_range)) {
            snprintf(shot_path, sizeof shot_path, "build/window_%05ld.png", (long)total_frames + 1);
            shot_to = shot_path;
        }
        // (only the periodic shots go to the writer thread: a range wants every frame and the one window shot is the
        // test's own, so both stay synchronous rather than lose frames to the one-in-flight writer)
        g_shot_async = shot_to == shot_path && !in_range;
        g_game_present = real;   // (a generated frame skips the cap's wait: it is paced above)
        g_note_game_ns = game_ns; g_note_gen = !real;
        vk_frame_end(shot_to);
        g_game_present = false;
        g_note_game_ns = 0;
        if (!real) { fg_n_gen++; fg_gen_t = SDL_GetTicksNS(); }
        if (real && gen) {
            uint64_t now = SDL_GetTicksNS();
            fg_n_real++;
            fg_gap_ns += now - fg_gen_t;
            if (!fg_log_t) fg_log_t = now;
            if (now - fg_log_t >= 10000000000ull) {
                if (!pq) {   // (the present thread logs its own line)
                    LOG("[dlss] FG presents in %.1f s: %llu generated + %llu real; wait before a generated one %.2f ms avg, %llu late; "
                        "last generated -> real %.2f ms avg; real interval %.2f ms\n", (now - fg_log_t) / 1e9, (unsigned long long)fg_n_gen,
                        (unsigned long long)fg_n_real, fg_n_gen ? fg_wait_ns / 1e6 / fg_n_gen : 0.0, (unsigned long long)fg_n_late,
                        fg_n_real ? fg_gap_ns / 1e6 / fg_n_real : 0.0, fg_interval / 1e6);
                }
                fg_n_gen = fg_n_real = fg_n_late = fg_wait_ns = fg_gap_ns = 0;
                fg_log_t = now;
            }
        }
        if (real) {
            uint64_t now = SDL_GetTicksNS();
            if (fg_last_real) { uint64_t d = now - fg_last_real; fg_interval = d > 100000000ull ? fg_interval : fg_interval ? (fg_interval * 4 + d) / 5 : d; }
            fg_last_real = now;
        }
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stdout); fflush(stderr); _exit(0); }
    } else if (real) {
        renderer_submit();
        vi_pace_present_point();   // (pacing at the present: a held frame is paced too)
        vk_present_skipped(total_frames);
    }
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    atomic_store(&g_last_frame_ns, SDL_GetTicksNS());
#ifndef _WIN32
    if (!g_present_tid) g_present_tid = (int)syscall(SYS_gettid);
#endif
    g_frame_counter++;
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    extern void vk_residency_tick(void);   // (textures.c: VRAM residency against the OS budget; render thread, between executes)
    vk_residency_tick();
    extern void vk_mirror_release_tick(void);   // (exec.c: the heap / alias mirrors' idle blocks, LARECOMP_MIRROR_RELEASE)
    vk_mirror_release_tick();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    // LARECOMP_DEBUG_FRAME=<frame>[,<frame>...] (ascending, up to 8): debug-dump those frames
    static long debug_at = -1, debug_list[8];
    static int debug_cnt, debug_i;
    if (debug_at < 0) {
        const char *e = getenv("LARECOMP_DEBUG_FRAME");
        for (const char *p = e; p && *p && debug_cnt < 8;) {
            debug_list[debug_cnt++] = strtol(p, (char **)&p, 10);
            if (*p == ',') p++; else break;
        }
        debug_at = debug_cnt ? debug_list[0] : 0;
    }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    while (debug_i + 1 < debug_cnt && debug_at && total_frames >= (uint64_t)debug_at + (uint64_t)debug_n) debug_at = debug_list[++debug_i];
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        extern volatile int g_gpuprof_request;   // (exec.c vk_prof_frame: LARECOMP_GPUPROF_SLOW)
        if (g_gpuprof_request) {
            g_gpuprof_request = 0;
            g_gpuprof_frame = true;
            vk_mem_report();
            mkdir("build", 0755);
            mkdir("build/frames", 0755);
            char path[256];
            snprintf(path, sizeof path, "build/frames/slow_%05llu.png", (unsigned long long)total_frames);
            if (b) renderer_save_texture_tex(b, path);
        }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    {   // LARECOMP_MEMREPORT_AT=<frame>[,<frame>...]: memory breakdown ("[mem]" in the log; vk_mem_report above)
        const char *e = getenv("LARECOMP_MEMREPORT_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { vk_mem_report(); break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) { mkdir("build", 0755); mkdir("build/frames", 0755); } }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    {  // LARECOMP_BLACKCHECK=1 (user 22:23: "the screen randomly goes completely black for like.2 of a second"): read a 4x4
        // grid of the presented game image every frame (one GPU sync per frame) and log frames that are all black
        static int on = -1;
        if (on < 0) on = getenv("LARECOMP_BLACKCHECK") != NULL;
        if (on && !held && b && b->image && b->width >= 8 && b->height >= 8 && !(b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
            VkSpan s = vk_readback(16 * 16);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            VkBufferImageCopy rg[16];
            for (int i = 0; i < 16; i++)
                rg[i] = (VkBufferImageCopy){.bufferOffset = s.off + (VkDeviceSize)i * 16, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                            .imageOffset = {(int32_t)(b->width * (1 + 2 * (i & 3)) / 8), (int32_t)(b->height * (1 + 2 * (i >> 2)) / 8), 0},
                                            .imageExtent = {1, 1, 1}};
            vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, 16, rg);
            renderer_flush();
            int mx = 0;
            for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) { int v = ((const uint8_t *)s.ptr)[i * 16 + k]; if (v > mx) mx = v; }
            static uint64_t run_start, nblack;
            if (mx <= 2) {
                if (!run_start) {
                    run_start = total_frames;
                    // LARECOMP_BLACKCHECK=capture: the first 2 black runs after boot (frame > 1000) also dump the next frame
                    // (as F9 does): a flash lasts ~4 frames, so that frame is still black and its passes show where it goes black
                    static int captures;
                    const char *e = getenv("LARECOMP_BLACKCHECK");
                    if (e && !strcmp(e, "capture") && total_frames > 1000 && captures < 2) {
                        extern volatile int g_debug_frame_request;
                        g_debug_frame_request = 1;
                        captures++;
                        LOG("[black] frame %llu black: capturing the next frame\n", (unsigned long long)total_frames);
                    }
                }
                nblack++;
                extern uint64_t g_skip_prog_frame, g_skip_pipe_frame, g_draws_frame;  // (exec.c, this frame)
                if (total_frames > 1000)
                    LOG("[black] frame %llu: %llu draws, skipped (program not built %llu, pipeline not built %llu)\n",
                        (unsigned long long)total_frames, (unsigned long long)g_draws_frame, (unsigned long long)g_skip_prog_frame,
                        (unsigned long long)g_skip_pipe_frame);
            } else if (run_start) {
                LOG("[black] frames %llu..%llu all black (%llu frames; %llu black frames so far)\n", (unsigned long long)run_start,
                    (unsigned long long)total_frames - 1, (unsigned long long)(total_frames - run_start), (unsigned long long)nblack);
                run_start = 0;
            }
        }
    }
    {   // LARECOMP_FLICKCHECK=1 (user 2026-10-19: UI / mesh flicker seen live, hidden by captures that sync CPU and GPU): a
        // 48x27 grid of the presented game image goes into a host ring every frame WITHOUT waiting; once its submission is done
        // (a few frames later) a frame whose samples jump away from both neighbours while those two agree is logged ("[flick]").
        enum { FX = 64, FY = 36, FN = FX * FY, FK = 8, FT = 48 };
        static int on = -1;
        static VkBuffer fbuf;
        static VmaAllocation fmem;
        static uint8_t *fptr;
        static uint64_t fseq[FK], fframe[FK], fhead, ftail, hframe[3], nflick, nchecked, win_from;
        static uint8_t hist[3][FN * 3];
        static int nh;
        static FILE *fdump;  // LARECOMP_FLICKCHECK=dump: every checked frame's samples -> build/flick.bin (u64 frame + FN x RGB)
        // LARECOMP_FLICKCROP=<cx>,<cy> (grid cell): a CW x CH crop around that cell rides along each frame; when the cell flips,
        // the crops of the frames before / at / after go to build/flick_crops.bin (u64 frame, u32 w, u32 h, RGBA x3), max 24
        enum { CW = 320, CH = 160 };
        static int ccx = -1, ccy = -1, ncrops;
        static VkBuffer cbuf;
        static VmaAllocation cmem;
        static uint8_t *cptr, chist[3][CW * CH * 4];
        static FILE *fcrop;
        if (on < 0) {
            on = getenv("LARECOMP_FLICKCHECK") != NULL;
            if (on && !strcmp(getenv("LARECOMP_FLICKCHECK"), "dump")) { mkdir("build", 0755); fdump = fopen("build/flick.bin", "wb"); }
            const char *ce = getenv("LARECOMP_FLICKCROP");
            if (on && ce && sscanf(ce, "%d,%d", &ccx, &ccy) == 2 && ccx >= 0 && ccx < FX && ccy >= 0 && ccy < FY) {
                VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)FK * CW * CH * 4,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                              .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
                VmaAllocationInfo info;
                if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &cbuf, &cmem, &info) == VK_SUCCESS) cptr = info.pMappedData;
                mkdir("build", 0755);
                fcrop = fopen("build/flick_crops.bin", "wb");
            } else ccx = -1;
            if (on) {
                VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)FK * FN * 16,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                              .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
                VmaAllocationInfo info;
                if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &fbuf, &fmem, &info) == VK_SUCCESS) fptr = info.pMappedData;
                LOG("[flick] check on (%dx%d samples per frame, threshold %d)\n", FX, FY, FT);
            }
        }
        if (on && fptr) {
            extern uint64_t vk_completed_seq(void);
            extern uint64_t renderer_fence_seq(void);
            while (ftail < fhead && vk_completed_seq() >= fseq[ftail % FK]) {
                const uint8_t *p = fptr + (ftail % FK) * (uint64_t)FN * 16;
                memmove(hist[0], hist[1], sizeof hist[0] * 2);
                hframe[0] = hframe[1], hframe[1] = hframe[2], hframe[2] = fframe[ftail % FK];
                for (int i = 0; i < FN; i++) memcpy(&hist[2][i * 3], p + i * 16, 3);
                if (fdump) { fwrite(&hframe[2], 8, 1, fdump); fwrite(hist[2], FN * 3, 1, fdump); }
                if (cptr) {
                    memmove(chist[0], chist[1], sizeof chist[0] * 2);
                    memcpy(chist[2], cptr + (ftail % FK) * (uint64_t)CW * CH * 4, sizeof chist[2]);
                }
                ftail++;
                if (++nh < 3 || hframe[1] != hframe[0] + 1 || hframe[2] != hframe[1] + 1) continue;
                int n = 0, x0 = FX, y0 = FY, x1 = -1, y1 = -1;
                for (int i = 0; i < FN; i++) {
                    int dab = 0, dbc = 0, dac = 0;
                    for (int k = 0; k < 3; k++) {
                        int a = hist[0][i * 3 + k], bb = hist[1][i * 3 + k], cc = hist[2][i * 3 + k];
                        dab += abs(bb - a), dbc += abs(bb - cc), dac += abs(a - cc);
                    }
                    if (dab > FT && dbc > FT && dac <= FT / 2) {
                        n++;
                        int x = i % FX, y = i / FX;
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
                }
                nchecked++;
                if (cptr && fcrop && ncrops < 24) {
                    int i = ccy * FX + ccx, dab = 0, dbc = 0, dac = 0;
                    for (int k = 0; k < 3; k++) {
                        int a = hist[0][i * 3 + k], bb = hist[1][i * 3 + k], cc = hist[2][i * 3 + k];
                        dab += abs(bb - a), dbc += abs(bb - cc), dac += abs(a - cc);
                    }
                    if (dab > FT && dbc > FT && dac <= FT / 2) {
                        uint32_t wh[2] = {CW, CH};
                        fwrite(&hframe[1], 8, 1, fcrop);
                        fwrite(wh, 4, 2, fcrop);
                        fwrite(chist, sizeof chist, 1, fcrop);
                        fflush(fcrop);
                        ncrops++;
                    }
                }
                if (n >= 3) {
                    static int nlog;
                    nflick++;
                    if (nlog++ < 400)
                        LOG("[flick] frame %llu: %d/%d samples transient, grid box (%d,%d)-(%d,%d)\n", (unsigned long long)hframe[1], n, FN,
                            x0, y0, x1, y1);
                }
                if (hframe[1] - win_from >= 600) {
                    LOG("[flick] frames %llu..%llu: %llu flicker frames of %llu checked\n", (unsigned long long)win_from,
                        (unsigned long long)hframe[1], (unsigned long long)nflick, (unsigned long long)nchecked);
                    win_from = hframe[1] + 1, nflick = nchecked = 0;
                }
            }
            bool fmt4 = b && (b->format == VK_FORMAT_R8G8B8A8_UNORM || b->format == VK_FORMAT_R8G8B8A8_SRGB ||
                              b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB ||
                              b->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            if (b && !fmt4) { static int told; if (!told++) LOG("[flick] presented format %d not checked\n", (int)b->format); }
            if (!held && fmt4 && b->image && b->width >= FX && b->height >= FY && fhead - ftail < FK) {
                VkCommandBuffer cb = vk_cmd();
                vk_full_barrier(cb);
                static VkBufferImageCopy rg[FN];
                uint64_t base = (fhead % FK) * (uint64_t)FN * 16;
                for (int i = 0; i < FN; i++)
                    rg[i] = (VkBufferImageCopy){.bufferOffset = base + (VkDeviceSize)i * 16, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                .imageOffset = {(int32_t)(b->width * (1 + 2 * (i % FX)) / (2 * FX)),
                                                                (int32_t)(b->height * (1 + 2 * (i / FX)) / (2 * FY)), 0},
                                                .imageExtent = {1, 1, 1}};
                vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, fbuf, FN, rg);
                if (cptr && b->width >= CW && b->height >= CH) {
                    int32_t x = (int32_t)(b->width * (1 + 2 * ccx) / (2 * FX)) - CW / 2, y = (int32_t)(b->height * (1 + 2 * ccy) / (2 * FY)) - CH / 2;
                    if (x < 0) x = 0;
                    if (y < 0) y = 0;
                    if (x + CW > (int32_t)b->width) x = (int32_t)b->width - CW;
                    if (y + CH > (int32_t)b->height) y = (int32_t)b->height - CH;
                    VkBufferImageCopy cr = {.bufferOffset = (fhead % FK) * (VkDeviceSize)CW * CH * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                            .imageOffset = {x, y, 0}, .imageExtent = {CW, CH, 1}};
                    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, cbuf, 1, &cr);
                }
                fseq[fhead % FK] = renderer_fence_seq();
                fframe[fhead % FK] = total_frames;
                fhead++;
            }
        }
    }
    {   // LARECOMP_FLICKVID=<from>-<to> (user 2026-10-19: "3d mesh flickering", too small for the 64x36 grid): every presented
        // frame in that range, filtered down to 320x180 RGBA8 without waiting, -> build/flickvid.bin (u64 frame + VW*VH*4)
        // once its submission is done. Frames are dropped (gap in the numbers) if the GPU falls VQ frames behind.
        enum { VW = 320, VH = 180, VQ = 16 };
        static long va = -1, vb;
        static VkImage vimg;
        static VmaAllocation vimem, vbmem;
        static VkBuffer vbuf;
        static uint8_t *vptr;
        static uint64_t vseq[VQ], vfr[VQ], vhead, vtail;
        static FILE *vf;
        extern uint64_t vk_completed_seq(void);
        extern uint64_t renderer_fence_seq(void);
        if (va < 0) {
            const char *e = getenv("LARECOMP_FLICKVID");
            if (!e || sscanf(e, "%ld-%ld", &va, &vb) != 2 || vb <= va) va = vb = 0;
            if (vb) {
                VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                        .extent = {VW, VH, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
                VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)VQ * VW * VH * 4,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
                VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                               .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
                VmaAllocationInfo info;
                mkdir("build", 0755);
                if (vmaCreateImage(g_vk.vma, &ci, &ai, &vimg, &vimem, NULL) != VK_SUCCESS ||
                    vmaCreateBuffer(g_vk.vma, &bi, &bai, &vbuf, &vbmem, &info) != VK_SUCCESS || !(vf = fopen("build/flickvid.bin", "wb")))
                    va = vb = 0;
                else {
                    vptr = info.pMappedData;
                    vk_image_init_layout(vk_cmd(), vimg, VK_IMAGE_ASPECT_COLOR_BIT);
                    LOG("[flickvid] frames %ld..%ld at %dx%d -> build/flickvid.bin\n", va, vb, VW, VH);
                }
            }
        }
        if (vb && vf) {
            while (vtail < vhead && vk_completed_seq() >= vseq[vtail % VQ]) {
                fwrite(&vfr[vtail % VQ], 8, 1, vf);
                fwrite(vptr + (vtail % VQ) * (uint64_t)VW * VH * 4, (size_t)VW * VH * 4, 1, vf);
                vtail++;
            }
            bool ok = b && b->image && (b->format == VK_FORMAT_R8G8B8A8_UNORM || b->format == VK_FORMAT_R8G8B8A8_SRGB ||
                                        b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB ||
                                        b->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            if (!held && ok && (long)total_frames >= va && (long)total_frames < vb && vhead - vtail < VQ) {
                VkCommandBuffer cb = vk_cmd();
                vk_full_barrier(cb);
                VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {VW, VH, 1}}};
                vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, vimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
                vk_full_barrier(cb);
                VkBufferImageCopy c = {.bufferOffset = (vhead % VQ) * (VkDeviceSize)VW * VH * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {VW, VH, 1}};
                vkCmdCopyImageToBuffer(cb, vimg, VK_IMAGE_LAYOUT_GENERAL, vbuf, 1, &c);
                vseq[vhead % VQ] = renderer_fence_seq();
                vfr[vhead % VQ] = total_frames;
                vhead++;
            }
            if ((long)total_frames >= vb && vtail == vhead) {
                fclose(vf);
                vf = NULL;
                LOG("[flickvid] done, %llu frames written\n", (unsigned long long)vtail);
            }
        }
    }
    {   // per-frame counters for the black check above
        g_skip_prog_frame = g_skip_pipe_frame = g_skip_comp_frame = g_draws_frame = g_skip_full_frame = g_skip_prog_full = 0;
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    {   // window title: shown fps (held frames excluded) and the worst gap between shown frames, every 0.5 s
        // (user 2026-10-15: "add an fps counter to the window top bar so i can monitor the fps")
        static const char *cap;
        static char cap_buf[32];
        static int cap_key = -1;
        extern int vi_fps_setting(void), vi_unlock_setting(void);   // hle_vi.cpp: the frame-rate setting, live
        const int un = vi_unlock_setting(), f = vi_fps_setting();
        if (un * 100000 + f != cap_key) {
            cap_key = un * 100000 + f;
            // (hle_vi.cpp: unlocked = up to the monitor's refresh in gameplay, loads at the game's interval; raw = unpaced)
            if (un) cap = un == 1 ? "unlocked" : "unpaced";
            else if (f > 0) { snprintf(cap_buf, sizeof cap_buf, "cap %d", f); cap = cap_buf; }
            else cap = "cap 30 (Switch)";
        }
        static double t_from, last_t, worst;
        static uint64_t shown_n;
        double tn = SDL_GetTicksNS() / 1e9;
        if (have) {
            shown_n++;
            if (last_t > 0 && tn - last_t > worst) worst = tn - last_t;
            last_t = tn;
        }
        if (t_from == 0) t_from = tn;
        else if (tn - t_from >= 0.5) {
            char title[200];
            if (last_t > 0 && tn - last_t > worst) worst = tn - last_t;  // nothing shown lately (frames held): the stall so far
            int pdone, ptotal;
            extern int vk_precompile_progress(int *done, int *total);
            char pre[48] = "";
            if (vk_precompile_progress(&pdone, &ptotal) && ptotal)
                snprintf(pre, sizeof pre, "  |  precompiling shaders %d %%", pdone * 100 / ptotal);
            snprintf(title, sizeof title, "TotK recomp  |  %.1f fps  |  worst frame %.0f ms  |  %s%s", shown_n / (tn - t_from),
                     worst * 1000.0, cap, pre);
            SDL_SetWindowTitle(window, title);
            snprintf(g_fps_text, sizeof g_fps_text, "%.0f fps  |  worst %.0f ms", shown_n / (tn - t_from), worst * 1000.0);
            t_from = tn, shown_n = 0, worst = 0;
        }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms held %llu (not held %llu) shown %.1f fps gap %.0fms level %s (%s)\n", now,
                (unsigned long long)total_frames, frames / (now - last_fps_time), max_gap * 1000.0, (unsigned long long)g_held_window,
                (unsigned long long)g_shown_with_skips, (frames - g_held_window) / (now - last_fps_time), g_shown_gap_max * 1000.0, game_level_name(), renderer_stats());
#ifndef _WIN32
        if (last_fps_time > 0) {
            char sysb[640]; rt_sys_line(sysb, sizeof sysb); LOG("[sys] %s\n", sysb);
            extern uint64_t vma_shim_tag_bytes(int tag, uint32_t *count);
            uint32_t c1, c2, c3, c0;
            uint64_t b1 = vma_shim_tag_bytes(1, &c1), b2 = vma_shim_tag_bytes(2, &c2), b3 = vma_shim_tag_bytes(3, &c3), b0 = vma_shim_tag_bytes(0, &c0);
            LOG("[mem] live: astc->rgba8 %llu MB (%u images), other textures %llu MB (%u), block shadows %llu MB (%u), rest %llu MB (%u)\n",
                (unsigned long long)(b1 >> 20), c1, (unsigned long long)(b2 >> 20), c2, (unsigned long long)(b3 >> 20), c3,
                (unsigned long long)(b0 >> 20), c0);
            rt_cpu_line(sysb, sizeof sysb); LOG("[cpu] %s\n", sysb);
        }
#else
        if (last_fps_time > 0) {   // "[host]" (shim_win.c): host RAM and paging, as [sys] on Linux
            extern void shim_host_line(char *out, size_t n);
            char hb[256]; shim_host_line(hb, sizeof hb); LOG("[host] %s\n", hb);
        }
#endif
        if (last_fps_time > 0) {  // "[vram]": the OS
            // budget / usage of each device-local heap (VK_EXT_memory_budget when the driver has it, else 0) + ours
            VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                    .pNext = g_vk.mem_budget ? &bud : NULL};
            vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp);
            uint64_t hb[VK_MAX_MEMORY_HEAPS], hs[VK_MAX_MEMORY_HEAPS];
            uint32_t hc[VK_MAX_MEMORY_HEAPS], hf[VK_MAX_MEMORY_HEAPS];
            uint32_t nh = vma_shim_heaps(g_vk.vma, hb, hc, hs, hf);
            extern uint32_t vma_shim_live(void), vma_shim_sub(void);
            extern uint64_t prof_alias_writer_changes;
            for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount && h < nh; h++)
                if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    LOG("[vram] heap %u: usage %llu MB of budget %llu MB (heap %llu MB); ours %llu MB in %u allocations; "
                        "%u vkAllocateMemory live (%u resources in blocks); alias writer changes %llu\n", h,
                        (unsigned long long)(bud.heapUsage[h] >> 20), (unsigned long long)(bud.heapBudget[h] >> 20),
                        (unsigned long long)(mp.memoryProperties.memoryHeaps[h].size >> 20), (unsigned long long)(hb[h] >> 20), hc[h],
                        vma_shim_live(), vma_shim_sub(), (unsigned long long)prof_alias_writer_changes);
            extern uint64_t vma_shim_tag_bytes(int tag, uint32_t *count);
            extern uint64_t vk_recycle_bytes(int *n);
            extern uint64_t vk_mirror_bytes(uint64_t *alias, int used[4]);
            uint32_t c1, c2, c3;
            int nrec, mu[4];
            uint64_t al, hm = vk_mirror_bytes(&al, mu), rec = vk_recycle_bytes(&nrec);
            LOG("[vram] mirrors used: heap %d MB bound in %d blocks of 16 MB, alias %d MB in %d blocks\n", mu[0], mu[2], mu[1], mu[3]);
            uint64_t b1 = vma_shim_tag_bytes(1, &c1), b2 = vma_shim_tag_bytes(2, &c2), b3 = vma_shim_tag_bytes(3, &c3);
            LOG("[vram] ours: astc->rgba8 %llu MB (%u), other images %llu MB (%u) of which recycle pool %llu MB (%d), block shadows "
                "%llu MB (%u), heap mirror %llu MB, alias mirror %llu MB\n", (unsigned long long)(b1 >> 20), c1, (unsigned long long)(b2 >> 20), c2,
                (unsigned long long)(rec >> 20), nrec, (unsigned long long)(b3 >> 20), c3, (unsigned long long)(hm >> 20), (unsigned long long)(al >> 20));
            {   // (textures.c: live game textures by kind; idle = unused for LARECOMP_VRAM_IDLE_MS, what residency may evict)
                extern void vk_tex_vram_split(uint64_t idle_frames, uint64_t bytes[3], uint32_t count[4]);
                extern uint64_t vk_residency_idle_frames(void);
                uint64_t tb[3];
                uint32_t tc[4];
                vk_tex_vram_split(vk_residency_idle_frames(), tb, tc);
                LOG("[vram] textures: sampled %llu MB (%u) of which idle %llu MB (%u), rendered / GPU-written %llu MB (%u), evicted %u\n",
                    (unsigned long long)(tb[0] >> 20), tc[0], (unsigned long long)(tb[1] >> 20), tc[1], (unsigned long long)(tb[2] >> 20), tc[2], tc[3]);
            }
            {  // (textures.c depth twins: copies of compare-sampled colour targets)
                extern uint64_t vk_twin_copies;
                extern void vk_depth_twin_check_poll(void);
                vk_depth_twin_check_poll();   // (LARECOMP_DEPTH_TWIN_CHECK: read-backs whose submission completed)
                static uint64_t l_tw;
                if (vk_twin_copies != l_tw) LOG("[tex] depth twin copies: %llu since the last line\n", (unsigned long long)(vk_twin_copies - l_tw));
                l_tw = vk_twin_copies;
            }
            extern uint64_t cum_img_n, cum_img_reused, cum_img_unpooled;   // (textures.c)
            static uint64_t l_n, l_r, l_u;
            LOG("[vram] images since the last line: created %llu, from the recycle pool %llu, finalized but freed (soft limit, "
                "LARECOMP_VK_SOFT_EVICT=0) %llu\n",(unsigned long long)(cum_img_n - l_n), (unsigned long long)(cum_img_reused - l_r),
                (unsigned long long)(cum_img_unpooled - l_u));
            l_n = cum_img_n; l_r = cum_img_reused; l_u = cum_img_unpooled;
        }
        max_gap = 0;
        g_held_window = 0;
        g_shown_with_skips = 0;
        g_shown_gap_max = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static _Atomic uint64_t g_exit_req_ns;   // quick_exit_handler started (hang_watch ends a stuck exit)
#ifndef _WIN32
// Reports a stall once (no frame for LARECOMP_HANG_REPORT_S = 6 s): the last blocking Vulkan call, memory, recent submissions,
// every thread's kernel wait and the presenting thread's backtrace. Closes the game after LARECOMP_HANG_EXIT_S = 30 s without
// a frame (0 = never), and ends an exit that is stuck waiting for the GPU after 10 s.
static void *hang_watch(void *arg) {
    (void)arg;
#ifndef _WIN32
    { extern int pthread_setname_np(pthread_t, const char *); pthread_setname_np(pthread_self(), "vk-hang-watch"); }   // ([cpu] line)
#endif
    const char *e = getenv("LARECOMP_HANG_REPORT_S");
    double report_s = e ? atof(e) : 6.0;
    e = getenv("LARECOMP_HANG_EXIT_S");
    double exit_s = e ? atof(e) : 30.0;
    bool reported = false;
    for (;;) {
        SDL_Delay(500);
        uint64_t now = SDL_GetTicksNS(), x = atomic_load(&g_exit_req_ns);
        if (x && now - x > 10000000000ull) {
            LOG("[hang] the game has been closing for 10 s (waiting for the GPU): ending it now\n");
            fflush(stdout);
            fflush(stderr);
            _exit(76);
        }
        if (atomic_load(&g_frames_held)) { atomic_store(&g_last_frame_ns, now); continue; }   // (paused: not a hang)
        uint64_t last = atomic_load(&g_last_frame_ns);
        if (!last || x) continue;
        double idle = (now - last) / 1e9;
        if (idle < report_s) {
            if (reported) LOG("[hang] frames again\n");
            reported = false;
            continue;
        }
        if (!reported) {
            reported = true;
            const char *op = g_rt_op;
            LOG("[hang] no frame for %.1f s (after frame %llu); blocking Vulkan call in progress: %s", idle, (unsigned long long)total_frames,
                op ? op : "none");
            if (op) LOG(" (for %.1f s)", (now - g_rt_op_ns) / 1e9);
            char sysb[640];
            rt_sys_line(sysb, sizeof sysb);
            LOG("\n[hang] %s\n", sysb);
            extern void vk_devlost_dump(void);
            vk_devlost_dump();
            rt_threads_dump(g_present_tid);
            fflush(stdout);
        }
        if (exit_s > 0 && idle >= exit_s) {
            LOG("[hang] no frame for %.0f s: closing the game (LARECOMP_HANG_EXIT_S=0 keeps it running)\n", exit_s);
            char sysb[640];
            rt_sys_line(sysb, sizeof sysb);
            LOG("[hang] %s\n", sysb);
            rt_threads_dump(0);
            fflush(stdout);
            fflush(stderr);
            _exit(75);
        }
    }
    return NULL;
}
#endif

// Run last on the way out, after the GPU and the pipeline cache (pc_overlay.cpp: "Restart the game now" starts the
// exe again here)
void (*volatile g_vk_exit_hook)(void);
static void quick_exit_handler(void) {
    atomic_store(&g_exit_req_ns, SDL_GetTicksNS());
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vk_present_queue_drain();
        vkq_drain();
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    if (g_vk_exit_hook) g_vk_exit_hook();
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    vk_pipeline_cache_prefetch();   // (read and checked while the window, the instance and the device are made)
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("TotK recomp", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows,
    // unless TOTK_WINDOW_MODE asks for a fullscreen mode by name (a window-mode test boot)
    const char *wm = getenv("TOTK_WINDOW_MODE");
    bool named = wm && (!strcmp(wm, "borderless") || !strcmp(wm, "exclusive"));
    if (g_settings.fullscreen && (named || !getenv("LARECOMP_WINDOW")) && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    {   // (shim: the start's mode is applied after this request: textures.c's TOTK_OUT_SCALE choice waits for it)
        extern void shim_window_start_done(void);
        shim_window_start_done();
    }
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    vk_present_queue_drain();
    vkq_drain();
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                // Not exit(): it ran static destructors under the still-running guest threads, so every close (Steam's
                // SIGTERM, window X) crashed in HLE code (Deck runs A-E, R1-R3 of 2026-10-20: SIGSEGV / SIGABRT after
                // "window closed") before the pipeline cache was saved. This waits for the GPU, saves, then _exit.
                quick_exit_handler();
            }
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
    }
}
