// pc_overlay.h: nxrecomp's PC settings overlay on the engine. Dear ImGui (third_party/imgui) for the UI, drawn by our own
// small renderer on the engine's Vulkan device (nvn/vulkan/pc_overlay_vk.c), so the draws go through the vkq recorder
// like every other command. On unless NXR_PC_OVERLAY=0: F10 (or L3 + R3) opens the settings window in place of
// hle_osd.cpp's Graphics menu, and F9 the perf overlay. C interface shared by shim_win.c (window messages, UI thread),
// hle_hid.cpp (pad state, HID path) and device.c / pc_overlay_vk.c (present thread).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pc_overlay_enabled(void);   // unless NXR_PC_OVERLAY=0, read once
bool pc_overlay_open(void);      // the settings window is open: the game gets no input
void pc_overlay_open_keys(void);  // open the settings window at its Keys tab (the Options row; any thread)
void *pc_overlay_hwnd(void);     // the game window (HWND) once it has had a message, else NULL
bool pc_overlay_perf(void);      // the F9 perf overlay is shown
void pc_overlay_set_perf(bool on);
void pc_overlay_restart_notice(void);   // a restart row changed in the game's Options screen: a notice how to restart

// shim_win.c's window procedure (UI thread): true when the overlay consumed the message (F9, F10, and all keyboard,
// text and mouse input while the settings window is open). hwnd is the game window.
bool pc_wndproc(void *hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);

// hle_hid.cpp, once per input poll: the pad buttons (nn::hid NpadButton bits, as osd_menu_input gets them). L3 + R3
// toggles the settings window; while it is open the buttons drive ImGui's gamepad navigation. Returns
// pc_overlay_open() after the update, so the caller clears the game's input when it is true.
bool pc_overlay_pad(uint64_t buttons);

// ---- present thread: UI side (pc_overlay.cpp) -> renderer (pc_overlay_vk.c)
typedef struct { float x, y, u, v; uint32_t col; } pc_vtx;   // = ImDrawVert (pos, uv, col RGBA8)
typedef struct {
    float clip[4];                // x0, y0, x1, y1 in framebuffer pixels
    uint32_t tex;                 // texture slot (pc_tex_req.slot)
    uint32_t idx_off, idx_count;  // into pc_draw.idx
    uint32_t vtx_off;             // added to each index
} pc_cmd;
enum { PC_TEX_UPLOAD = 1, PC_TEX_DESTROY = 2 };
typedef struct {
    uint32_t slot, op;            // op: PC_TEX_UPLOAD (create or replace with the whole image) or PC_TEX_DESTROY
    uint32_t w, h;
    const void *rgba;             // w * h * 4 bytes, valid until the next pc_overlay_build
} pc_tex_req;
enum { PC_TEX_SLOTS = 8 };
typedef struct {
    const pc_vtx *vtx; uint32_t nvtx;
    const uint16_t *idx; uint32_t nidx;
    const pc_cmd *cmd; uint32_t ncmd;
    const pc_tex_req *tex; uint32_t ntex;
} pc_draw;

// Builds this frame's ImGui frame for a w x h window image. NULL when nothing is visible (then the renderer records
// nothing). Texture requests in the result must be carried out before its draws.
const pc_draw *pc_overlay_build(uint32_t w, uint32_t h);
// The renderer's VRAM reading for the perf overlay (device-local heaps, VK_EXT_memory_budget; 0 budget: unknown)
void pc_overlay_note_vram(uint32_t used_mb, uint32_t budget_mb);
// The device, once (the first-run screen's preset recommendation): its name and device-local memory
void pc_overlay_note_gpu(const char *name, uint32_t vram_mb);

// device.c, vk_frame_end, after the window image's rendering ended and before the OSD blits (present thread). The
// image is in COLOR_ATTACHMENT_OPTIMAL and stays so.
void pc_overlay_vk_record(void);

#ifdef __cplusplus
}
#endif
