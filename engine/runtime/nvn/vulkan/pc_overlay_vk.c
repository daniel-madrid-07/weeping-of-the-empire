// pc_overlay_vk.c: renders nxrecomp's PC overlay on the engine's device. A small Dear ImGui renderer of our own
// instead of imgui_impl_vulkan: it records through vk.h, so every command goes through the vkq recorder in order with
// the rest of the frame (imgui_impl_vulkan would write the command buffer directly, beside the record thread), and it
// takes its per-frame vertex and index data from the transient ring. Called from vk_frame_end after the window
// image's rendering ended: texture uploads first (copies are not allowed inside a rendering), then vk_frame_resume
// (rendering again, LOAD), the ImGui draws, vk_frame_suspend. Shaders: imgui_impl_vulkan's (Dear ImGui 1.93.0, MIT,
// third_party/imgui/LICENSE.txt), the SPIR-V copied as is.
#include "vk.h"
#include "../../pc_overlay.h"
#include <string.h>

void vk_frame_suspend(void);
void vk_frame_resume(void);

// glsl_shader.vert: aPos (0), aUV (1), aColor (2); push constant { vec2 uScale; vec2 uTranslate; }
static const uint32_t vert_spv[] = {
    0x07230203,0x00010000,0x0008000b,0x0000002e,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x000a000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x0000000f,0x00000015,
    0x0000001b,0x0000001c,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00030005,0x00000009,0x00000000,0x00050006,0x00000009,0x00000000,0x6f6c6f43,
    0x00000072,0x00040006,0x00000009,0x00000001,0x00005655,0x00030005,0x0000000b,0x0074754f,
    0x00040005,0x0000000f,0x6c6f4361,0x0000726f,0x00030005,0x00000015,0x00565561,0x00060005,
    0x00000019,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x00000019,0x00000000,
    0x505f6c67,0x7469736f,0x006e6f69,0x00030005,0x0000001b,0x00000000,0x00040005,0x0000001c,
    0x736f5061,0x00000000,0x00060005,0x0000001e,0x73755075,0x6e6f4368,0x6e617473,0x00000074,
    0x00050006,0x0000001e,0x00000000,0x61635375,0x0000656c,0x00060006,0x0000001e,0x00000001,
    0x61725475,0x616c736e,0x00006574,0x00030005,0x00000020,0x00006370,0x00040047,0x0000000b,
    0x0000001e,0x00000000,0x00040047,0x0000000f,0x0000001e,0x00000002,0x00040047,0x00000015,
    0x0000001e,0x00000001,0x00030047,0x00000019,0x00000002,0x00050048,0x00000019,0x00000000,
    0x0000000b,0x00000000,0x00040047,0x0000001c,0x0000001e,0x00000000,0x00030047,0x0000001e,
    0x00000002,0x00050048,0x0000001e,0x00000000,0x00000023,0x00000000,0x00050048,0x0000001e,
    0x00000001,0x00000023,0x00000008,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
    0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040017,
    0x00000008,0x00000006,0x00000002,0x0004001e,0x00000009,0x00000007,0x00000008,0x00040020,
    0x0000000a,0x00000003,0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000003,0x00040015,
    0x0000000c,0x00000020,0x00000001,0x0004002b,0x0000000c,0x0000000d,0x00000000,0x00040020,
    0x0000000e,0x00000001,0x00000007,0x0004003b,0x0000000e,0x0000000f,0x00000001,0x00040020,
    0x00000011,0x00000003,0x00000007,0x0004002b,0x0000000c,0x00000013,0x00000001,0x00040020,
    0x00000014,0x00000001,0x00000008,0x0004003b,0x00000014,0x00000015,0x00000001,0x00040020,
    0x00000017,0x00000003,0x00000008,0x0003001e,0x00000019,0x00000007,0x00040020,0x0000001a,
    0x00000003,0x00000019,0x0004003b,0x0000001a,0x0000001b,0x00000003,0x0004003b,0x00000014,
    0x0000001c,0x00000001,0x0004001e,0x0000001e,0x00000008,0x00000008,0x00040020,0x0000001f,
    0x00000009,0x0000001e,0x0004003b,0x0000001f,0x00000020,0x00000009,0x00040020,0x00000021,
    0x00000009,0x00000008,0x0004002b,0x00000006,0x00000028,0x00000000,0x0004002b,0x00000006,
    0x00000029,0x3f800000,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,
    0x00000005,0x0004003d,0x00000007,0x00000010,0x0000000f,0x00050041,0x00000011,0x00000012,
    0x0000000b,0x0000000d,0x0003003e,0x00000012,0x00000010,0x0004003d,0x00000008,0x00000016,
    0x00000015,0x00050041,0x00000017,0x00000018,0x0000000b,0x00000013,0x0003003e,0x00000018,
    0x00000016,0x0004003d,0x00000008,0x0000001d,0x0000001c,0x00050041,0x00000021,0x00000022,
    0x00000020,0x0000000d,0x0004003d,0x00000008,0x00000023,0x00000022,0x00050085,0x00000008,
    0x00000024,0x0000001d,0x00000023,0x00050041,0x00000021,0x00000025,0x00000020,0x00000013,
    0x0004003d,0x00000008,0x00000026,0x00000025,0x00050081,0x00000008,0x00000027,0x00000024,
    0x00000026,0x00050051,0x00000006,0x0000002a,0x00000027,0x00000000,0x00050051,0x00000006,
    0x0000002b,0x00000027,0x00000001,0x00070050,0x00000007,0x0000002c,0x0000002a,0x0000002b,
    0x00000028,0x00000029,0x00050041,0x00000011,0x0000002d,0x0000001b,0x0000000d,0x0003003e,
    0x0000002d,0x0000002c,0x000100fd,0x00010038
};
// glsl_shader.frag: texture2D at set 0 binding 0, sampler at set 1 binding 0
static const uint32_t frag_spv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000023,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000d,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00040005,0x00000009,0x6c6f4366,0x0000726f,0x00030005,0x0000000b,0x00000000,
    0x00050006,0x0000000b,0x00000000,0x6f6c6f43,0x00000072,0x00040006,0x0000000b,0x00000001,
    0x00005655,0x00030005,0x0000000d,0x00006e49,0x00050005,0x00000015,0x7865545f,0x65727574,
    0x00000000,0x00050005,0x00000019,0x6d61535f,0x72656c70,0x00000000,0x00040047,0x00000009,
    0x0000001e,0x00000000,0x00040047,0x0000000d,0x0000001e,0x00000000,0x00040047,0x00000015,
    0x00000021,0x00000000,0x00040047,0x00000015,0x00000022,0x00000000,0x00040047,0x00000019,
    0x00000021,0x00000000,0x00040047,0x00000019,0x00000022,0x00000001,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,0x00000008,
    0x00000009,0x00000003,0x00040017,0x0000000a,0x00000006,0x00000002,0x0004001e,0x0000000b,
    0x00000007,0x0000000a,0x00040020,0x0000000c,0x00000001,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000001,0x00040015,0x0000000e,0x00000020,0x00000001,0x0004002b,0x0000000e,
    0x0000000f,0x00000000,0x00040020,0x00000010,0x00000001,0x00000007,0x00090019,0x00000013,
    0x00000006,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x00040020,
    0x00000014,0x00000000,0x00000013,0x0004003b,0x00000014,0x00000015,0x00000000,0x0002001a,
    0x00000017,0x00040020,0x00000018,0x00000000,0x00000017,0x0004003b,0x00000018,0x00000019,
    0x00000000,0x0003001b,0x0000001b,0x00000013,0x0004002b,0x0000000e,0x0000001d,0x00000001,
    0x00040020,0x0000001e,0x00000001,0x0000000a,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x00050041,0x00000010,0x00000011,0x0000000d,0x0000000f,
    0x0004003d,0x00000007,0x00000012,0x00000011,0x0004003d,0x00000013,0x00000016,0x00000015,
    0x0004003d,0x00000017,0x0000001a,0x00000019,0x00050056,0x0000001b,0x0000001c,0x00000016,
    0x0000001a,0x00050041,0x0000001e,0x0000001f,0x0000000d,0x0000001d,0x0004003d,0x0000000a,
    0x00000020,0x0000001f,0x00050057,0x00000007,0x00000021,0x0000001c,0x00000020,0x00050085,
    0x00000007,0x00000022,0x00000012,0x00000021,0x0003003e,0x00000009,0x00000022,0x000100fd,
    0x00010038
};

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkDescriptorSet set; uint32_t w, h; } Tex;
typedef struct { Tex t; uint64_t seq; } Dead;   // freed once the submission that last used it is complete

static VkPipeline pipe;
static VkFormat pipe_fmt;
static VkPipelineLayout layout;
static VkDescriptorSetLayout dsl_tex, dsl_smp;
static VkDescriptorPool pool;
static VkDescriptorSet set_smp;
static VkSampler smp;
static Tex tex[PC_TEX_SLOTS];
static Dead dead[2 * PC_TEX_SLOTS];
static int ndead;
static bool broken;   // a setup step failed: the overlay stays off for the run (logged once)

static void tex_free(Tex *t) {
    if (t->set) vkFreeDescriptorSets(g_vk.dev, pool, 1, &t->set);
    if (t->view) vkDestroyImageView(g_vk.dev, t->view, NULL);
    if (t->img) vmaDestroyImage(g_vk.vma, t->img, t->mem);
    memset(t, 0, sizeof *t);
}

static void tex_retire(Tex *t) {
    if (!t->img) return;
    if (ndead == (int)(sizeof dead / sizeof dead[0])) { vk_wait_seq(dead[0].seq); tex_free(&dead[0].t); dead[0] = dead[--ndead]; }
    dead[ndead].t = *t;
    dead[ndead++].seq = vk_pending_seq();
    memset(t, 0, sizeof *t);
}

static void reap(void) {
    uint64_t done = vk_completed_seq();
    for (int i = 0; i < ndead;)
        if (dead[i].seq <= done) { tex_free(&dead[i].t); dead[i] = dead[--ndead]; }
        else i++;
}

static bool setup_common(void) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .minLod = -1000, .maxLod = 1000, .maxAnisotropy = 1.0f};
    if (vkCreateSampler(g_vk.dev, &si, NULL, &smp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding bt = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutBinding bs = {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bt};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &dsl_tex) != VK_SUCCESS) return false;
    li.pBindings = &bs;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &dsl_smp) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * PC_TEX_SLOTS + 4}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                     .maxSets = 2 * PC_TEX_SLOTS + 5, .poolSizeCount = 2, .pPoolSizes = ps};
    if (vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl_smp};
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, &set_smp) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii = {.sampler = smp};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set_smp, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    VkDescriptorSetLayout sl[2] = {dsl_tex, dsl_smp};
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    return vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &layout) == VK_SUCCESS;
}

static bool make_pipeline(VkFormat fmt) {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vert_spv, .pCode = vert_spv};
    bool ok = vkCreateShaderModule(g_vk.dev, &mi, NULL, &vs) == VK_SUCCESS;
    mi.codeSize = sizeof frag_spv; mi.pCode = frag_spv;
    ok = ok && vkCreateShaderModule(g_vk.dev, &mi, NULL, &fs) == VK_SUCCESS;
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(pc_vtx), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pc_vtx, x)},
                                               {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(pc_vtx, u)},
                                               {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(pc_vtx, col)}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    ok = ok && vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p) == VK_SUCCESS;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (!ok) return false;
    if (pipe) { vk_wait_seq(vk_pending_seq() - 1); vkDestroyPipeline(g_vk.dev, pipe, NULL); }   // format change: rare
    pipe = p;
    pipe_fmt = fmt;
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
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

// Whole-image upload into slot s: the same image again when the size matches (the barrier orders the copy after
// earlier frames' reads on this queue), else a new one and the old image retired.
static bool upload(VkCommandBuffer cb, const pc_tex_req *r) {
    Tex *t = &tex[r->slot];
    if (t->img && (t->w != r->w || t->h != r->h)) tex_retire(t);
    VkImageLayout from = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!t->img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {r->w, r->h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) { memset(t, 0, sizeof *t); return false; }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                           .descriptorSetCount = 1, .pSetLayouts = &dsl_tex};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &t->view) != VK_SUCCESS ||
            vkAllocateDescriptorSets(g_vk.dev, &dai, &t->set) != VK_SUCCESS) { tex_free(t); return false; }
        VkDescriptorImageInfo ii = {.imageView = t->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii};
        vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
        t->w = r->w; t->h = r->h;
        from = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    VkSpan s = vk_transient_copy(r->rgba, (uint64_t)r->w * r->h * 4, 16);
    barrier(cb, t->img, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {r->w, r->h, 1}};
    vkCmdCopyBufferToImage(cb, s.buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Device-local heaps' usage and budget, as device.c's [vram] line reads them (0 without VK_EXT_memory_budget)
static void note_vram(void) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            .pNext = g_vk.mem_budget ? &b : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp);
    uint64_t used = 0, budget = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) { used += b.heapUsage[i]; budget += b.heapBudget[i]; }
    pc_overlay_note_vram((uint32_t)(used >> 20), (uint32_t)(budget >> 20));
}

void pc_overlay_vk_record(void) {
    if (broken) return;
    static bool gpu_noted;
    if (!gpu_noted) {   // (the first-run screen recommends a preset from it)
        gpu_noted = true;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        uint64_t local = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && mp.memoryHeaps[i].size > local) local = mp.memoryHeaps[i].size;
        pc_overlay_note_gpu(g_vk.props.deviceName, (uint32_t)(local >> 20));
    }
    const uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    const pc_draw *d = pc_overlay_build(w, h);
    reap();
    if (!d) return;
    static uint32_t frames;
    if (frames++ % 30 == 0) note_vram();   // (only while something is shown)
    if (!layout && !setup_common()) { broken = true; LOG("[pc-overlay] Vulkan setup failed: overlay off\n"); return; }
    if ((!pipe || pipe_fmt != g_vk.sc_format) && !make_pipeline(g_vk.sc_format)) {
        broken = true; LOG("[pc-overlay] pipeline for format %d failed: overlay off\n", (int)g_vk.sc_format); return;
    }
    VkCommandBuffer cb = vk_cmd();
    for (uint32_t i = 0; i < d->ntex; i++) {
        const pc_tex_req *r = &d->tex[i];
        if (r->slot >= PC_TEX_SLOTS) continue;
        if (r->op == PC_TEX_DESTROY) tex_retire(&tex[r->slot]);
        else if (r->op == PC_TEX_UPLOAD && r->rgba && r->w && r->h && !upload(cb, r)) LOG("[pc-overlay] texture %ux%u upload failed\n", r->w, r->h);
    }
    if (!d->ncmd || !d->nvtx || !d->nidx) return;
    VkSpan vb = vk_transient_copy(d->vtx, (uint64_t)d->nvtx * sizeof(pc_vtx), 16);
    VkSpan ib = vk_transient_copy(d->idx, (uint64_t)d->nidx * sizeof(uint16_t), 16);
    vk_frame_resume();   // window image, LOAD; full viewport and scissor
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize vo = vb.off;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb.buf, &vo);
    vkCmdBindIndexBuffer(cb, ib.buf, ib.off, VK_INDEX_TYPE_UINT16);
    const float pc[4] = {2.0f / w, 2.0f / h, -1.0f, -1.0f};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    uint32_t bound = UINT32_MAX;
    for (uint32_t i = 0; i < d->ncmd; i++) {
        const pc_cmd *c = &d->cmd[i];
        if (c->tex >= PC_TEX_SLOTS || !tex[c->tex].set) continue;
        if (c->tex != bound) {
            VkDescriptorSet sets[2] = {tex[c->tex].set, set_smp};
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, NULL);
            bound = c->tex;
        }
        VkRect2D sc = {{(int32_t)c->clip[0], (int32_t)c->clip[1]},
                       {(uint32_t)(c->clip[2] - (int32_t)c->clip[0]), (uint32_t)(c->clip[3] - (int32_t)c->clip[1])}};
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdDrawIndexed(cb, c->idx_count, 1, c->idx_off, (int32_t)c->vtx_off, 0);
    }
    vk_frame_suspend();
}
