// present.h: the window's presentation settings, for the overlay (pc_settings.cpp) and the rest of the runtime. C and
// C++; no Vulkan types. Every setter is safe from any thread and takes effect at the next presented frame (a mode or
// HDR change makes a new swapchain there).
//
// Start values come from the environment (off by default unless said):
//   LARECOMP_VK_PRESENT   mailbox (default) | fifo | fifo_relaxed | immediate
//   TOTK_MAX_QUEUED       0 (off) | 1..3, default 2: frames presented but not yet on screen (VK_KHR_present_wait)
//   TOTK_FRAMES_IN_FLIGHT 1..3, default 2: frames the GPU may be behind the CPU (3 = the engine as it was)
//   TOTK_SWAPCHAIN_IMAGES 2..4 (default: the surface's minimum + 1)
//   TOTK_HDR              off | hdr10 | scrgb;  TOTK_HDR_PEAK (nits, 1000), TOTK_HDR_PAPER (nits, 200),
//                         TOTK_HDR_EXPAND (exponent of the highlight expansion, 3), TOTK_HDR_GAMMA (srgb | 2.2)
//   TOTK_PRESENT_LOG      <file>: one CSV row per game present, window present and display time
//   TOTK_PACE_STATS       1: the "[pace]" line every 2 s (also on with TOTK_PRESENT_LOG)
// hle_vi.cpp: TOTK_VSYNC=display (start value only), TOTK_PACE_AT=frame (default: present) and TOTK_PACE_SPIN_US=<us>
// (start values; the display.pace_at / pace_spin rows change them live: vi_set_pace_at / vi_set_pace_spin).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Present modes: the values of VkPresentModeKHR.
enum { VKP_IMMEDIATE = 0, VKP_MAILBOX = 1, VKP_FIFO = 2, VKP_FIFO_RELAXED = 3 };
void vk_set_present_mode(int mode);          // device.c; then vk_request_swapchain_recreate()
void vk_request_swapchain_recreate(void);
int vk_present_mode_supported(int mode);     // 1 / 0 for the window's surface; -1 before the window exists
int vk_present_mode_active(void);            // the current swapchain's mode; -1 none

// Latency.
int vk_present_wait_supported(void);         // VK_KHR_present_wait is on (needed by max_queued and display times)
void vk_present_set_max_queued(int frames);  // 0 off, 1..3
int vk_present_max_queued(void);
void vk_present_set_frames_in_flight(int n); // 1..3
int vk_present_frames_in_flight(void);
// Low latency: NVIDIA Reflex's sleep (VK_NV_low_latency2, enabled with present wait when the driver offers it).
// TOTK_REFLEX=1 | boost | jit at start. 0 off, 1 on (without the extension: the engine's own sleep, any GPU), 2 on +
// boost, 3 markers only, 4 the engine's own sleep (present.c jit_sleep).
void vk_present_set_low_latency(int mode);
int vk_present_low_latency_supported(void);   // the extension is on
void vk_present_latency_sleep(void);          // the game thread, before each frame (hle_vi.cpp)

// HDR output: the game's SDR image (and everything drawn over it) is expanded to `peak_nits` with SDR white at
// `paper_nits` and encoded for the swapchain.
enum { VKP_HDR_OFF = 0, VKP_HDR10 = 1, VKP_HDR_SCRGB = 2 };
void vk_present_set_hdr(int mode, float peak_nits, float paper_nits);
int vk_present_hdr_supported(int mode);      // 1 / 0: the surface offers that format; -1 before the window exists
int vk_present_hdr_active(void);             // mode of the current swapchain (VKP_HDR_OFF if the request fell back)

#ifdef __cplusplus
}
#endif
