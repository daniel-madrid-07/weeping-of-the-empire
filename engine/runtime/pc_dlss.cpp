// DLSS rows for the settings registry (pc_settings.h; the in-game Options screen shows them). Both act through the
// device: NGX starts and its features are created with it (runtime/nvn/vulkan/ngx.c), so a change takes a restart.
// Default off. Each row has its own variable, so neither depends on the order the rows are applied in:
//   graphics.dlss      -> TOTK_DLSS_SR=1: DLSS Super Resolution in place of the game's upscale pass (DLAA by itself
//                         when the scene renders at the output size); a scene larger than the output keeps the game's pass
//   graphics.frame_gen -> TOTK_DLSS_FG=1: DLSS Frame Generation, one generated frame between real frames; works with or
//                         without graphics.dlss (it makes its own motion vectors and depth from the game's)
// On a GPU without DLSS, or off Windows, they do nothing and the log says so ("[dlss] ... unavailable").
#include "pc_settings.h"

namespace {

const char *const V_DLSS[] = {"", "1", nullptr};
const char *const N_DLSS[] = {"Off", "DLSS", nullptr};
const char *const V_FG[] = {"", "1", nullptr};
const char *const N_FG[] = {"Off", "On", nullptr};

const pc_setting k_dlss[] = {
    {"graphics.dlss", "Graphics", "Rendering", "DLSS Super Resolution", "TOTK_DLSS_SR. NVIDIA RTX only: DLSS upscales the "
     "3D scene to the output in place of the game's upscaler (DLAA when Render resolution matches the output).", "",
     V_DLSS, N_DLSS, 0, 0, 0, PC_SET_RESTART, "TOTK_DLSS_SR", nullptr, nullptr, nullptr},
    {"graphics.frame_gen", "Graphics", "Rendering", "DLSS Frame Generation", "TOTK_DLSS_FG. NVIDIA RTX 40 series and "
     "newer: one generated frame between real frames, about twice the frames shown, at the cost of some input latency.",
     "", V_FG, N_FG, 0, 0, 0, PC_SET_RESTART, "TOTK_DLSS_FG", nullptr, nullptr, nullptr},
};

struct AddDlssRows {
    AddDlssRows() { pc_settings_add(k_dlss, (int)(sizeof k_dlss / sizeof k_dlss[0])); }
} g_add_dlss_rows;

}  // namespace
