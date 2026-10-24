/* Masked clears: a colour clear that leaves some channels alone, and a stencil clear under a write mask.
   vkCmdClearAttachments clears every channel and every stencil bit, so these are drawn: a full-screen triangle (one
   instance per layer of a layered target, the vertex shader sends it to that layer) inside the open pass, with the
   channel write mask (or the stencil write mask) doing the masking. The shaders are tiny (tools/build_clear_shaders.py).
   Pipelines are keyed by the pass's formats and the mask, and made when first needed. A failure returns 0 and the
   caller clears everything instead (what it did before). */
#include "gfx/vk/vk_draw.h"
#include "gfx/vk/vk_clear_spv.h"

enum { K_FLOAT, K_UINT, K_STENCIL };
typedef struct { int kind, n, index; uint32_t mask, samples; VkFormat color[8], depth, stencil; VkPipeline p; } entry;
static entry g_e[64]; static int g_ne;
static VkShaderModule g_vs, g_fs[2]; static VkPipelineLayout g_lay; static int g_init;   /* g_init: 1 ready, -1 failed */

static int module(const uint32_t *w, size_t words, VkShaderModule *m) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = w};
    return vkCreateShaderModule(vk_dev, &ci, NULL, m) == VK_SUCCESS;
}
static int init(void) {
    if (g_init) return g_init > 0;
    g_init = -1;
    VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (!module(vk_clear_vs, sizeof vk_clear_vs / 4, &g_vs) || !module(vk_clear_fs_float, sizeof vk_clear_fs_float / 4, &g_fs[0]) ||
        !module(vk_clear_fs_uint, sizeof vk_clear_fs_uint / 4, &g_fs[1]) || vkCreatePipelineLayout(vk_dev, &li, NULL, &g_lay) != VK_SUCCESS) {
        VLOG("masked clears: the clear shaders were not accepted by the GPU; channel and stencil masks are ignored");
        return 0;
    }
    g_init = 1;
    return 1;
}

/* integer render targets take the clear value as integer bits (uint output), the rest as float */
static int is_int(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 1;
    default: return 0;
    }
}

static VkPipeline pipeline_for(int kind, int index, uint32_t mask) {
    entry want; memset(&want, 0, sizeof want);   /* the key is compared bytewise */
    want.kind = kind; want.n = S.pass_fmt.ncolor; want.index = index; want.mask = mask; want.samples = S.pass_samples;
    want.depth = S.pass_fmt.depth; want.stencil = S.pass_fmt.stencil;
    memcpy(want.color, S.pass_fmt.color, sizeof want.color);
    for (int i = 0; i < g_ne; i++)
        if (!memcmp(&g_e[i], &want, offsetof(entry, p))) return g_e[i].p;   /* VK_NULL_HANDLE: tried and failed */
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_fs[kind == K_UINT], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};   /* counts are dynamic */
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(S.pass_samples ? S.pass_samples : 1)};
    VkStencilOpState so = {.failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE, .depthFailOp = VK_STENCIL_OP_REPLACE, .compareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .stencilTestEnable = kind == K_STENCIL, .front = so, .back = so};
    VkPipelineColorBlendAttachmentState ba[8];
    for (int i = 0; i < 8; i++) ba[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = kind != K_STENCIL && i == index ? mask : 0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = (uint32_t)S.pass_fmt.ncolor, .pAttachments = ba};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                         VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = (uint32_t)S.pass_fmt.ncolor,
        .pColorAttachmentFormats = S.pass_fmt.color, .depthAttachmentFormat = S.pass_fmt.depth, .stencilAttachmentFormat = S.pass_fmt.stencil};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = kind == K_STENCIL ? 1u : 2u, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g_lay};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk_dev, vk_pcache, 1, &ci, NULL, &p) != VK_SUCCESS) { p = VK_NULL_HANDLE; VK_ONCE("masked clear pipeline (kind %d) not created", kind); }
    if (g_ne < 64) { want.p = p; g_e[g_ne++] = want; }
    return p;
}

static void draw(VkPipeline p, VkRect2D rect, uint32_t layers) {
    VkCommandBuffer c = vk_cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkViewport v = {0, 0, (float)S.pass_w, (float)S.pass_h, 0, 1};
    vkCmdSetViewportWithCount(c, 1, &v);
    vkCmdSetScissorWithCount(c, 1, &rect);
    vkCmdDraw(c, 3, layers ? layers : 1, 0, 0);
}

/* Clear colour target `index` of the open pass to the value `bits` (four 32-bit words as the game gave them), in the
   channels of `mask` (bit 0 = R .. bit 3 = A), inside `rect`. 0: not done. */
int vx_clear_masked_color(int index, uint32_t mask, const uint32_t bits[4], VkRect2D rect, uint32_t layers) {
    if (index < 0 || index >= S.pass_fmt.ncolor || !init()) return 0;
    VkPipeline p = pipeline_for(is_int(S.pass_fmt.color[index]) ? K_UINT : K_FLOAT, index, mask & 0xf);
    if (!p) return 0;
    vkCmdPushConstants(vk_cmd(), g_lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, bits);
    draw(p, rect, layers);
    return 1;
}
/* Set the stencil bits of `wmask` to those of `value` in the open pass's stencil attachment, inside `rect`. 0: not done. */
int vx_clear_masked_stencil(uint32_t wmask, uint32_t value, VkRect2D rect, uint32_t layers) {
    if (S.pass_fmt.stencil == VK_FORMAT_UNDEFINED || !init()) return 0;
    VkPipeline p = pipeline_for(K_STENCIL, 0, 0);
    if (!p) return 0;
    VkCommandBuffer c = vk_cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_AND_BACK, wmask & 0xff);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, value & 0xff);
    draw(p, rect, layers);
    return 1;
}
