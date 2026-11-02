// Upscaler and frame generation rows for the settings registry (pc_settings.h; the in-game Options screen shows them):
// NVIDIA DLSS (NGX) or AMD FSR 3.1 (the ffx.c, any Vulkan GPU) on the same inputs (runtime/nvn/vulkan/ngx.c). Live: the
// start's value goes into its variables, and a change in game is posted to ngx.c (vk_ngx_request), which turns the path on
// or off, or moves it to the other backend, between frames on the render thread (NGX started then if it was not: ~1 s,
// once; ngx.c apply_request). Default off. One row per path, so DLSS and FSR are never both asked for one; the keys stay
// v0.1.7's, whose saved "1" keeps meaning DLSS (x2 for frame generation):
//   graphics.dlss      -> "1": TOTK_DLSS_SR=1, DLSS Super Resolution in place of the game's upscale pass (DLAA by
//                         itself when the scene renders at the output size); "dlaa": that and TOTK_DLSS_DLAA=1, the scene
//                         at the output's size whatever Render resolution says (ngx.c vk_ngx_scene_scale, polled by
//                         totk_hooks.c output_scale); "fsr": TOTK_FSR_SR=1, FSR 3.1 there instead.
//                         A scene larger than the output keeps the game's pass. The game's FXAA goes with it (ngx.c)
//   graphics.frame_gen -> "1".."3": TOTK_DLSS_FG=1 + TOTK_DLSS_FG_FRAMES=<n>, DLSS Frame Generation, n generated frames
//                         between real frames (x2 = 1, x3 = 2, x4 = 3); more than one only where NGX reports multi-frame
//                         generation (RTX 50), else x2 and the log says so. "fsr": TOTK_FSR_FG=1, FSR 3.1 frame
//                         generation, x2. Either works with or without an upscaler (motion vectors and depth are made
//                         from the game's)
// On a GPU without the backend asked for, or off Windows, a path does nothing and the log says so.
#include "pc_settings.h"

#include <stdlib.h>
#include <string.h>

extern "C" void vk_ngx_request(int sr, int fg);   // nvn/vulkan/ngx.c (-1: unchanged)

namespace {

constexpr int FG_FSR = 16;   // (vk.h VK_NGX_FG_FSR)

const char *const V_DLSS[] = {"", "1", "dlaa", "fsr", nullptr};
const char *const N_DLSS[] = {"Off", "DLSS (NVIDIA RTX)", "DLAA (NVIDIA RTX)", "FSR (any GPU)", nullptr};
const char *const V_FG[] = {"", "1", "2", "3", "fsr", nullptr};
const char *const N_FG[] = {"Off", "DLSS x2", "DLSS x3", "DLSS x4", "FSR x2", nullptr};

void sr_to_env(const char *v) {   // (v: one of V_DLSS)
    _putenv_s("TOTK_DLSS_SR", !strcmp(v, "1") || !strcmp(v, "dlaa") ? "1" : "");
    _putenv_s("TOTK_DLSS_DLAA", !strcmp(v, "dlaa") ? "1" : "");
    _putenv_s("TOTK_FSR_SR", !strcmp(v, "fsr") ? "1" : "");
}
void fg_to_env(const char *v) {   // (v: one of V_FG; Off removes them all, so a hand-set TOTK_DLSS=fg still works)
    const bool fsr = !strcmp(v, "fsr");
    _putenv_s("TOTK_DLSS_FG", *v && !fsr ? "1" : "");
    _putenv_s("TOTK_DLSS_FG_FRAMES", fsr ? "" : v);
    _putenv_s("TOTK_FSR_FG", fsr ? "1" : "");
}
void sr_apply(const char *v) { vk_ngx_request(!*v ? 0 : !strcmp(v, "fsr") ? 2 : !strcmp(v, "dlaa") ? 3 : 1, -1); }
void fg_apply(const char *v) { vk_ngx_request(-1, !*v ? 0 : !strcmp(v, "fsr") ? FG_FSR : atoi(v)); }

const pc_setting k_dlss[] = {
    {"graphics.dlss", "Graphics", "Rendering", "Upscaler", "TOTK_DLSS_SR / TOTK_FSR_SR. DLSS (NVIDIA "
     "RTX) or FSR (any GPU) upscales the 3D scene to the output in place of the game's upscaler and its FXAA (DLAA / "
     "native AA when Render resolution matches the output).", "",
     V_DLSS, N_DLSS, 0, 0, 0, 0, nullptr, sr_to_env, sr_apply, nullptr},
    {"graphics.frame_gen", "Graphics", "Rendering", "Frame generation", "TOTK_DLSS_FG / TOTK_FSR_FG. Generated "
     "frames between real ones: DLSS x2 on NVIDIA RTX 40 series and newer, x3 and x4 on RTX 50; FSR x2 on any GPU. The "
     "frame cap counts the frames shown (at a 30 cap: x2, 60 shown). Adds some input latency.",
     "", V_FG, N_FG, 0, 0, 0, 0, nullptr, fg_to_env, fg_apply, nullptr},
};

struct AddDlssRows {
    AddDlssRows() { pc_settings_add(k_dlss, (int)(sizeof k_dlss / sizeof k_dlss[0])); }
} g_add_dlss_rows;

}  // namespace
