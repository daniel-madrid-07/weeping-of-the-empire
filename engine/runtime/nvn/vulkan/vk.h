// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, dmabuf_import;
    bool gpl;  // VK_EXT_graphics_pipeline_library with fast linking (exec.c: per-program shader libraries, linked per draw state)
    VkShaderStageFlags sg32_stages;  // stages whose warp-op shaders require 32-wide subgroups (device.c; exec.c sg_pin)
    uint32_t max_wg_subgroups;       // maxComputeWorkgroupSubgroups: a pinned compute workgroup holds at most this many subgroups
    bool maint5;  // VK_KHR_maintenance5 enabled (device.c: unwritten point sizes are 1.0)
    bool astc_ldr;   // textureCompressionASTC_LDR enabled: ASTC images are created natively (textures.c), else decoded to RGBA8
    bool fill_nonsolid, dual_src_blend, depth_clamp;  // core features the device has (device.c; exec.c falls back without)
    bool feedback_loop;  // attachment feedback loops declared to the driver (device.c: EXT layout + dynamic state, unified layouts)
    bool fbl_layout;     // the feedback-loop LAYOUT extension alone (AMD Windows): live reads in FEEDBACK_LOOP_OPTIMAL (exec.c)
    bool depth_unrestricted;  // VK_EXT_depth_range_unrestricted enabled (textures.c depth twins: D32 values outside [0,1])
    bool host_qreset;     // Vulkan 1.2 hostQueryReset enabled (exec.c: occlusion queries reset from the CPU)
    bool sep_ds_layouts;  // Vulkan 1.2 separateDepthStencilLayouts enabled (textures.c: S8_UINT substitute)
    bool sampler_aniso, mirror_clamp;  // samplerAnisotropy / samplerMirrorClampToEdge enabled (textures.c samplers fall back)
    bool mem_budget;  // VK_EXT_memory_budget enabled: per-heap budget / usage readable (device.c [vram], residency)
    bool checkpoints;  // VK_NV_device_diagnostic_checkpoints: exec.c marks every GPU operation (hang reports)
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq_at(uint64_t seq, const char *file, int line);   // (stats: GPU wait time per call site, "| waits")
#define vk_wait_seq(seq) vk_wait_seq_at((seq), __FILE__, __LINE__)
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic, cpu_dirty, zero_on_write;  // zero_on_write: recycled depth image, cleared at its first pass  // cpu_dirty: WriteTexels since the last upload check
    uint64_t data_hash, sample_hash, checked_frame, full_frame, ww_seen;  // ww_seen: heap write-watch epoch (textures.c)
    uint64_t full_seq;   // the oldest submission whose heap writes may not have landed at the last full hash (LARECOMP_TEX_GPUW: GPU writes since, exec.c)
    uint8_t gpuw_skips;  // LARECOMP_TEX_GPUW=1: periodic rehashes skipped since the last full hash (every 4th still runs)
    bool scalable, presented;
    uint8_t scale_kind;  // (scalable) 1 = 16:9 target (TOTK_RES_SCALE), 2 = square depth target = shadow map (TOTK_SHADOW_SCALE)
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    VkImageUsageFlags usage;  // as created (textures.c): render views need COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT
    VkImageCreateFlags cflags;
    struct TexBackend *blk;  // block-compressed image rendered through an uncompressed view: the uncompressed shadow image
                             // the passes draw into (one texel per block), copied into this image after each pass
    uint64_t wb_frame;       // textures.c write-back: frame + 1 of the render that queued it
    uint8_t wb_state;        // 0 none, 1 queued in this execute, 2 written back, 3 excluded (not a candidate / per-frame)
    uint32_t wr_epoch;       // exec.c LARECOMP_BARRIERS=tracked: barrier epoch in which a pass last wrote it
    uint64_t last_use;       // vk_frame_counter() of its last use (tex_backend): VRAM residency's LRU order (textures.c)
    bool evicted;            // image dropped by VRAM residency; tex_backend re-creates it and the next use re-uploads it
    bool fbl_in;             // exec.c g_vk.fbl_layout: in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL for the open pass (else GENERAL)
    struct TexBackend *twin; // a colour target sampled with depth compare: its depth-format copy (textures.c vk_depth_twin)
    bool twin_valid;         // the twin holds this image's latest rendering (copied after each pass that drew into it)
    uint32_t rd_epoch;       // exec.c LARECOMP_PASS_ELIDE=2: barrier epoch in which a draw last sampled it
    uint64_t snap_tag;       // exec.c LARECOMP_SNAP_DELTA: the feedback snapshot that mirrors this image (0: none), and the box
    int32_t snap_box[4];     // draws wrote in it since (level 0, x0 y0 x1 y1; empty when x1 <= x0)
    bool preuploaded;        // preupload.c: uploaded on the loading screen; its first real use checks it (upload_texture_)
} TexBackend;
VkImageView vk_depth_twin_view(TexBackend *b, const NvnTextureView *v, bool has_view, int textype);  // textures.c: twin view once filled
bool vk_depth_twin_stale(TexBackend *b);  // textures.c: b is compare-sampled through a twin that needs a copy now (outside a pass)
uint64_t vk_residency_need_ns(void);  // textures.c: time of the last measurement that found pressure (0 = never)
uint64_t vk_residency_pressure(bool *commit);  // textures.c: bytes over the VRAM budget / under the commit floor (0 = none)
void vk_depth_twin_copy(VkCommandBuffer cb, TexBackend *b);  // after a pass into b: refresh its twin (exec.c blk_copy_back)
void vk_writeback_flush(void);  // textures.c: rendered CPU-movable textures' texels -> their guest storage (end of execute)
TexBackend *vk_render_backend(NvnTexture *t);  // the image a pass on t really draws into (t's own, or its block shadow)

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

// ---- presentation (present.c; its settings API is present.h): device.c's hooks
int vk_present_instance_exts(const char **ext, int room);    // instance extensions to add (returns how many)
void vk_present_device_exts(const VkExtensionProperties *ep, uint32_t ne, const char **ext, uint32_t *n_ext);
void *vk_present_features(void *next);                       // its feature structs chained in front of next
void vk_present_device_ready(void);
// latch.c: the mouse late latch, LARECOMP_LATE_LATCH=1 (the canvas re-aimed before the HUD)
bool vk_late_latch_on(void);
void vk_late_latch_sample(int64_t pushed);   // game thread, at a present push (pushed: presents pushed before it; -1 after its throttle)
void vk_late_latch_camera(const float *cb8, uint64_t size, uint64_t pres_done);   // the frame's first G-buffer draw
void vk_late_latch_warp(VkCommandBuffer cb, TexBackend *canvas, uint64_t pres_done, bool flip_y, bool at_present);   // outside a pass
void vk_late_latch_present(void);            // exec.c: backend_present, a frame whose upscale had no later draw into the canvas
uint64_t backend_async_presents_done(void);  // exec.c
// ngx.c: NVIDIA NGX (DLSS SR / FG) on our instance and device, TOTK_DLSS=1 only
bool vk_ngx_on(void);
bool vk_ngx_ready(void);
uint32_t vk_ngx_instance_exts(const char **ext, uint32_t n, uint32_t room);
void vk_ngx_device_exts(const VkExtensionProperties *ep, uint32_t ne, const char **ext, uint32_t *n_ext, uint32_t room);
void vk_ngx_device_ready(void);                                 // after vkCreateDevice
void vk_ngx_gbuffer_draw(const float *cb8, uint64_t size, uint32_t depth_w, uint32_t depth_h, uint64_t frame);
typedef struct { VkImage image; VkImageView view; VkFormat format; uint32_t w, h; } VkNgxImage;
void vk_ngx_sr_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy);  // render pixels
float vk_ngx_sr_mip_bias(uint32_t render_w, uint32_t out_w);
bool vk_ngx_sr_evaluate(VkCommandBuffer cb, const VkNgxImage *color, const VkNgxImage *depth, const VkNgxImage *mv,
                        const VkNgxImage *out, float jx, float jy, float mv_scale_x, float mv_scale_y, bool reset,
                        bool depth_inverted, bool hdr, const VkNgxImage *bias);   // bias: the bias-current-colour mask or NULL
bool vk_ngx_sr_on(void);   // TOTK_DLSS=sr and NGX up
bool vk_ngx_sr_frame_jitter(uint64_t frame, uint32_t render_w, uint32_t out_w, float *jx, float *jy);
bool vk_ngx_sr_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *out, uint64_t frame,
                       bool flip_y, bool zminus);
bool vk_ngx_fg_on(void);                                   // TOTK_DLSS=fg / fgtest
int vk_ngx_fg_frame(VkCommandBuffer cb, TexBackend *b);    // backend_present: b = the frame's final image; returns how
                                                           // many generated frames wait (fg), shown with vk_ngx_fg_show
void vk_ngx_fg_show(VkCommandBuffer cb, TexBackend *b, int generated);   // generated k (1..n) or 0: the real frame
void vk_ngx_fg_keep_hudless(VkCommandBuffer cb, TexBackend *out, uint64_t frame);   // the canvas right after the scene
                                                                                  // went in (outside a render pass)
uint32_t vk_present_image_count(const VkSurfaceCapabilitiesKHR *caps);
void vk_present_pick_format(const VkSurfaceFormatKHR *fmts, uint32_t nf, VkSurfaceFormatKHR *sf);   // HDR request
void vk_present_swapchain_retire(void);                      // after vkCreateSwapchainKHR, before the old views go
void vk_present_swapchain_ready(VkSurfaceFormatKHR sf, int mode);   // g_vk.sc_* filled (HDR: the canvas replaces them)
bool vk_present_hdr_encode(VkCommandBuffer cb, uint32_t image);     // instead of the PRESENT_SRC barrier; false: HDR off
uint64_t vk_present_chain(VkPresentInfoKHR *pi, bool generated);   // present id for this present (0: none)
void vk_present_note(uint64_t id, uint64_t game_ns, bool generated);   // TOTK_PRESENT_LOG "l" row
VkResult vk_present_acquire(VkSemaphore sem, uint64_t timeout_ns, uint32_t *image);   // vkAcquireNextImageKHR, under the lock
void vk_swapchain_lock(void);                                // around vkQueuePresentKHR / swapchain re-create (external sync)
void vk_swapchain_unlock(void);
void vk_swapchain_life_lock(void);                           // around making / destroying a swapchain (present.c)
void vk_swapchain_life_unlock(void);
void vk_present_done(uint64_t frame, uint64_t id, uint64_t seq, VkResult r, uint64_t t_submit, uint64_t t_paced);
void vk_present_skipped(uint64_t frame);                     // a frame not shown (held / minimised)
bool vk_present_queue_wanted(void);                          // TOTK_FG_PRESENT_THREAD=1 (device.c: a second queue for it)
void vk_present_queue_init(VkQueue q);                       // FG present thread (present.c) on this queue (NULL: none)
bool vk_present_queue_on(void);                              // DLSS-G on and the thread there: presents go through it
void vk_present_queue_push(uint32_t img, VkSemaphore wait, uint64_t id, uint64_t frame, uint64_t seq, uint64_t t_submit,
                           int kind, int k, int n);          // instead of vkQueuePresentKHR (kind 0 other, 1 real, 2 generated k of n)
void vk_present_queue_drain(void);                           // before a swapchain re-create / vkDeviceWaitIdle
VkResult vk_present_queue_result(void);                      // out of date / suboptimal / lost since the last call (0: none)
void vi_pace_present_point(void);                            // hle_vi.cpp: TOTK_PACE_AT=present waits here
const void *vk_present_swapchain_pnext(void);                // swapchain create chain (exclusive fullscreen) or NULL
void vk_present_lost(void);                                  // VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT seen

#include "vkq.h"   // (vkCmd* calls -> the deferred recorder when LARECOMP_VKQ=1)
