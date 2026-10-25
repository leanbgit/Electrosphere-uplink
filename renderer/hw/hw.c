#include "hw_private.h"
#ifndef AC3_HW_WEBGPU
#include "ac3_shaders.h"
#include "ac3_shaders_hw.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hw_state_t hw_state;
#define H hw_state

static inline s32 sx11(s32 v) { return (s32)((u32)v << 21) >> 21; }

#ifndef AC3_HW_WEBGPU   /* Vulkan helpers and setup; see renderer/webgpu/hw_webgpu.c */
#define VKOK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "ac3: hw: %s failed (%d) at %s:%d\n", #expr, (int)_r, \
            __FILE__, __LINE__); return 0; } } while (0)

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(H.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_image(VkFormat fmt, u32 w, u32 h, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(H.dev, &ii, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(H.dev, *img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return 0;
    VKOK(vkAllocateMemory(H.dev, &ai, NULL, mem));
    VKOK(vkBindImageMemory(H.dev, *img, *mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VKOK(vkCreateImageView(H.dev, &vi, NULL, view));
    return 1;
}

static int make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKOK(vkCreateBuffer(H.dev, &bi, NULL, buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(H.dev, *buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return 0;
    VKOK(vkAllocateMemory(H.dev, &ai, NULL, mem));
    VKOK(vkBindBufferMemory(H.dev, *buf, *mem, 0));
    VKOK(vkMapMemory(H.dev, *mem, 0, size, 0, map));
    return 1;
}

static void free_host_buffer(VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    if (*map) vkUnmapMemory(H.dev, *mem);
    if (*buf) vkDestroyBuffer(H.dev, *buf, NULL);
    if (*mem) vkFreeMemory(H.dev, *mem, NULL);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *map = NULL;
}

static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(H.dev, &ci, NULL, &m);
    return m;
}

static void barrier(VkCommandBuffer cb, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

uint32_t hw_find_mem(uint32_t bits, VkMemoryPropertyFlags want) { return find_mem(bits, want); }
int hw_make_image(VkFormat fmt, u32 w, u32 h, VkImageUsageFlags usage,
                  VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    return make_image(fmt, w, h, usage, img, mem, view);
}
int hw_make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    return make_host_buffer(size, usage, buf, mem, map);
}
void hw_free_host_buffer(VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    free_host_buffer(buf, mem, map);
}
VkShaderModule hw_shader(const uint32_t *code, size_t bytes) { return shader(code, bytes); }
void hw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                VkAccessFlags sa, VkAccessFlags da,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    barrier(cb, img, from, to, sa, da, ss, ds);
}

static int init_pass(void) {
    VkAttachmentDescription at[3] = { { 0 } };
    for (int i = 0; i < 3; i++) {
        at[i].samples = VK_SAMPLE_COUNT_1_BIT;
        at[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    at[1].format = at[2].format = VK_FORMAT_R32_SFLOAT;
    VkAttachmentReference refs[3] = {
        { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
    };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 3;
    sub.pColorAttachments = refs;
    VkSubpassDependency dep[2] = { { 0 }, { 0 } };
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ri.attachmentCount = 3;
    ri.pAttachments = at;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 2;
    ri.pDependencies = dep;
    VKOK(vkCreateRenderPass(H.dev, &ri, NULL, &H.pass));

    VkImageView views[3] = { H.target_view, H.z_view, H.order_view };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = H.pass;
    fi.attachmentCount = 3;
    fi.pAttachments = views;
    fi.width = H.W;
    fi.height = H.Hs;
    fi.layers = 1;
    VKOK(vkCreateFramebuffer(H.dev, &fi, NULL, &H.fb));
    return 1;
}

static int init_descriptors(void) {
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKOK(vkCreateSampler(H.dev, &si, NULL, &H.nearest));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VKOK(vkCreateSampler(H.dev, &si, NULL, &H.linear));

    VkDescriptorSetLayoutBinding b[3];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 3; i++) {
        b[i].binding = (u32)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3;
    li.pBindings = b;
    VKOK(vkCreateDescriptorSetLayout(H.dev, &li, NULL, &H.set_layout));

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VKOK(vkCreateDescriptorPool(H.dev, &pi, NULL, &H.pool));

    VkDescriptorSetLayout ls[2] = { H.set_layout, H.set_layout };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = H.pool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = ls;
    VKOK(vkAllocateDescriptorSets(H.dev, &dai, sets));
    H.set_read = sets[0];
    H.set_disp = sets[1];

    VkDescriptorImageInfo ii[2] = {
        { H.nearest, H.read_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { H.linear, H.target_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkDescriptorImageInfo zi = { H.nearest, H.zsnap_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo oi = { H.nearest, H.order_snap_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[6];
    memset(w, 0, sizeof w);
    for (int i = 0; i < 6; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = sets[i & 1];
        w[i].dstBinding = (u32)(i >> 1);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = i < 2 ? &ii[i] : i < 4 ? &zi : &oi;
    }
    vkUpdateDescriptorSets(H.dev, 6, w, 0, NULL);
    return 1;
}

typedef struct { float inv_size[2]; int scale; int pass; } prim_push_t;

static int init_pipelines(void) {
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(prim_push_t) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &H.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKOK(vkCreatePipelineLayout(H.dev, &pli, NULL, &H.layout));

    VkShaderModule vs = shader(spv_hw_prim_vert, sizeof spv_hw_prim_vert);
    VkShaderModule fs = shader(spv_hw_prim_frag, sizeof spv_hw_prim_frag);
    if (!vs || !fs) return 0;

    VkVertexInputBindingDescription bind = { 0, sizeof(hw_vtx_t), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 16 },
        { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24 },
        { 3, 0, VK_FORMAT_R32G32B32A32_UINT, 28 },
    };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 4;
    vin.pVertexAttributeDescriptions = attr;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    const VkColorComponentFlags RGBA = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    for (int k = 0; k < P_COUNT; k++) {
        VkPipelineColorBlendAttachmentState cb[3];
        memset(cb, 0, sizeof cb);
        cb[0].colorWriteMask = RGBA;
        cb[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        float consts[4] = { 0, 0, 0, 0 };
        cb[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb[0].alphaBlendOp = VK_BLEND_OP_ADD;
        cb[0].colorBlendOp = VK_BLEND_OP_ADD;
        switch (k) {
        case P_OPAQUE:
        case P_COPY:
            break;
        case P_BLEND0:
            cb[0].blendEnable = VK_TRUE;
            cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            consts[0] = consts[1] = consts[2] = consts[3] = 0.5f;
            break;
        case P_BLEND1:
            cb[0].blendEnable = VK_TRUE;
            cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case P_BLEND2:
            cb[0].blendEnable = VK_TRUE;
            cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb[0].colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            break;
        case P_BLEND3:
            cb[0].blendEnable = VK_TRUE;
            cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            consts[0] = consts[1] = consts[2] = consts[3] = 0.25f;
            break;
        case P_OPAQUE_CHECK:
        case P_COPY_CHECK:
            cb[0].blendEnable = VK_TRUE;
            cb[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            cb[0].dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            cb[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            cb[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            break;
        }
        if (k >= P_BLEND0 && k <= P_BLEND3) cb[1].colorWriteMask = 0;
        if (k == P_COPY || k == P_COPY_CHECK) cb[1].colorWriteMask = 0;

        VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb[2].colorWriteMask = cb[1].colorWriteMask;
        cbs.attachmentCount = 3;
        cbs.pAttachments = cb;
        memcpy(cbs.blendConstants, consts, sizeof consts);

        VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &ds;
        gp.layout = H.layout;
        gp.renderPass = H.pass;
        VKOK(vkCreateGraphicsPipelines(H.dev, VK_NULL_HANDLE, 1, &gp, NULL, &H.pipe[k]));
    }
    vkDestroyShaderModule(H.dev, vs, NULL);
    vkDestroyShaderModule(H.dev, fs, NULL);
    return 1;
}

int hw_init(VkPhysicalDevice phys, VkDevice dev, uint32_t qfamily,
            const hw_config_t *cfg) {
    memset(&H, 0, sizeof H);
    H.phys = phys;
    H.dev = dev;
    H.qfamily = qfamily;
    H.S = cfg && cfg->scale > 0 ? cfg->scale : 1;
    H.persp = cfg ? cfg->perspective : 1;
    if (H.S > HW_MAX_SCALE) H.S = HW_MAX_SCALE;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    while (H.S > 1 && ((u32)(VRAM_W * H.S) > props.limits.maxImageDimension2D ||
                       (u32)(VRAM_W * H.S) > props.limits.maxFramebufferWidth))
        H.S--;
    H.W = (u32)(VRAM_W * H.S);
    H.Hs = (u32)(VRAM_H * H.S);

    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, H.W, H.Hs, rt,
                    &H.target, &H.target_mem, &H.target_view) ||
        !make_image(VK_FORMAT_R8G8B8A8_UNORM, H.W, H.Hs,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &H.read, &H.read_mem, &H.read_view) ||
        !make_image(VK_FORMAT_R32_SFLOAT, H.W, H.Hs, rt,
                    &H.order, &H.order_mem, &H.order_view) ||
        !make_image(VK_FORMAT_R32_SFLOAT, H.W, H.Hs,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &H.order_snap, &H.order_snap_mem, &H.order_snap_view) ||
        !make_image(VK_FORMAT_R32_SFLOAT, H.W, H.Hs, rt,
                    &H.zimg, &H.z_mem, &H.z_view) ||
        !make_image(VK_FORMAT_R32_SFLOAT, H.W, H.Hs,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &H.zsnap, &H.zsnap_mem, &H.zsnap_view) ||
        !make_image(VK_FORMAT_R8G8B8A8_UNORM, VRAM_W, VRAM_H,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                    &H.upimg, &H.up_mem, &H.up_view) ||
        !init_pass() || !init_descriptors() || !init_pipelines()) {
        fprintf(stderr, "ac3: hw: renderer setup failed\n");
        return 0;
    }
    H.ready = 1;
    if (!hw_fx_init())
        fprintf(stderr, "ac3: hw: the world effects could not be set up; drawing without them\n");
    printf("renderer: enhanced, %dx internal resolution (%ux%u VRAM)\n",
           H.S, H.W, H.Hs);
    return 1;
}

void hw_shutdown(void) {
    if (!H.ready) return;
    vkDeviceWaitIdle(H.dev);
    hw_fx_shutdown();
    for (int i = 0; i < HW_FRAMES; i++) {
        free_host_buffer(&H.vbuf[i], &H.vmem[i], &H.vmap[i]);
        free_host_buffer(&H.sbuf[i], &H.smem[i], &H.smap[i]);
    }
    for (int k = 0; k < P_COUNT; k++) vkDestroyPipeline(H.dev, H.pipe[k], NULL);
    if (H.disp_pipe) vkDestroyPipeline(H.dev, H.disp_pipe, NULL);
    if (H.disp_layout) vkDestroyPipelineLayout(H.dev, H.disp_layout, NULL);
    vkDestroyPipelineLayout(H.dev, H.layout, NULL);
    vkDestroyDescriptorPool(H.dev, H.pool, NULL);
    vkDestroyDescriptorSetLayout(H.dev, H.set_layout, NULL);
    vkDestroySampler(H.dev, H.nearest, NULL);
    vkDestroySampler(H.dev, H.linear, NULL);
    vkDestroyFramebuffer(H.dev, H.fb, NULL);
    vkDestroyRenderPass(H.dev, H.pass, NULL);
    VkImage imgs[7] = { H.target, H.read, H.zimg, H.upimg, H.zsnap, H.order, H.order_snap };
    VkImageView views[7] = { H.target_view, H.read_view, H.z_view, H.up_view, H.zsnap_view, H.order_view, H.order_snap_view };
    VkDeviceMemory mems[7] = { H.target_mem, H.read_mem, H.z_mem, H.up_mem, H.zsnap_mem, H.order_mem, H.order_snap_mem };
    for (int i = 0; i < 7; i++) {
        vkDestroyImageView(H.dev, views[i], NULL);
        vkDestroyImage(H.dev, imgs[i], NULL);
        vkFreeMemory(H.dev, mems[i], NULL);
    }
    free(H.verts); free(H.staging); free(H.cmds);
    memset(&H, 0, sizeof H);
}

#endif /* !AC3_HW_WEBGPU */

int hw_scale(void) { return H.S; }

void hw_set_perspective(int on) { H.persp = on ? 1 : 0; }

void hw_frame_stats(u32 *prims, u32 *precise) {
    if (prims) *prims = H.last_prims;
    if (precise) *precise = H.last_precise;
}

static void dirty_add(rect_t r);

static void *grow(void *p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap) return p;
    u32 n = *cap ? *cap : 1024;
    while (n < need) n *= 2;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) return p;
    *cap = n;
    return q;
}

static cmd_t *new_cmd(u8 op) {
    H.cmds = grow(H.cmds, &H.cap_cmds, H.ncmds + 1, sizeof(cmd_t));
    if (H.ncmds >= H.cap_cmds) return NULL;
    cmd_t *c = &H.cmds[H.ncmds++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

cmd_t *hw_new_cmd(u8 op) { return new_cmd(op); }

void hw_mark_dirty(s32 x0, s32 y0, s32 x1, s32 y1) {
    rect_t r = { x0, y0, x1, y1 };
    dirty_add(r);
}

static u32 push_verts(const hw_vtx_t *v, u32 n) {
    H.verts = grow(H.verts, &H.cap_verts, H.nverts + n, sizeof(hw_vtx_t));
    if (H.nverts + n > H.cap_verts) return UINT32_MAX;
    u32 first = H.nverts;
    memcpy(&H.verts[first], v, n * sizeof(hw_vtx_t));
    H.nverts += n;
    return first;
}

static int same_rect(const VkRect2D *a, const VkRect2D *b) {
    return a->offset.x == b->offset.x && a->offset.y == b->offset.y &&
           a->extent.width == b->extent.width && a->extent.height == b->extent.height;
}

static void add_draw(u8 pipe, u8 pass, VkRect2D sc, u32 first, u32 count) {
    if (H.ncmds) {
        cmd_t *l = &H.cmds[H.ncmds - 1];
        if (l->op == OP_DRAW && l->pipe == pipe && l->pass == pass &&
            same_rect(&l->sc, &sc) && l->first + l->count == first) {
            l->count += count;
            return;
        }
    }
    cmd_t *c = new_cmd(OP_DRAW);
    if (!c) return;
    c->pipe = pipe; c->pass = pass; c->sc = sc; c->first = first; c->count = count;
}

static void add_draw_pair(u8 pipe1, u8 pipe2, VkRect2D sc, u32 first, u32 count) {
    if (H.ncmds >= 2) {
        cmd_t *a = &H.cmds[H.ncmds - 2], *b = &H.cmds[H.ncmds - 1];
        if (a->op == OP_DRAW && b->op == OP_DRAW &&
            a->pipe == pipe1 && a->pass == 1 && b->pipe == pipe2 && b->pass == 2 &&
            same_rect(&a->sc, &sc) && same_rect(&b->sc, &sc) &&
            a->first == b->first && a->count == b->count &&
            a->first + a->count == first) {
            a->count += count;
            b->count += count;
            return;
        }
    }
    add_draw(pipe1, 1, sc, first, count);
    cmd_t *c = new_cmd(OP_DRAW);
    if (!c) return;
    c->pipe = pipe2; c->pass = 2; c->sc = sc; c->first = first; c->count = count;
}

static int split_wrap(s32 x, s32 y, s32 w, s32 h, rect_t *out) {
    int n = 0;
    if (w <= 0 || h <= 0) return 0;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    s32 xs[2] = { x, 0 }, ws[2] = { w, 0 };
    if (x + w > VRAM_W) { ws[0] = VRAM_W - x; ws[1] = w - ws[0]; }
    s32 ys[2] = { y, 0 }, hs[2] = { h, 0 };
    if (y + h > VRAM_H) { hs[0] = VRAM_H - y; hs[1] = h - hs[0]; }
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
            if (ws[i] > 0 && hs[j] > 0) {
                out[n].x0 = xs[i]; out[n].y0 = ys[j];
                out[n].x1 = xs[i] + ws[i]; out[n].y1 = ys[j] + hs[j];
                n++;
            }
    return n;
}

static int overlap(const rect_t *a, const rect_t *b) {
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

static void dirty_add(rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > VRAM_W) r.x1 = VRAM_W;
    if (r.y1 > VRAM_H) r.y1 = VRAM_H;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    for (int i = 0; i < H.ndirty; ) {
        rect_t *d = &H.dirty[i];
        if (overlap(d, &r)) {
            if (d->x0 < r.x0) r.x0 = d->x0;
            if (d->y0 < r.y0) r.y0 = d->y0;
            if (d->x1 > r.x1) r.x1 = d->x1;
            if (d->y1 > r.y1) r.y1 = d->y1;
            H.dirty[i] = H.dirty[--H.ndirty];
            i = 0;
            continue;
        }
        i++;
    }
    if (H.ndirty == MAX_DIRTY) {
        for (int i = 0; i < H.ndirty; i++) {
            if (H.dirty[i].x0 < r.x0) r.x0 = H.dirty[i].x0;
            if (H.dirty[i].y0 < r.y0) r.y0 = H.dirty[i].y0;
            if (H.dirty[i].x1 > r.x1) r.x1 = H.dirty[i].x1;
            if (H.dirty[i].y1 > r.y1) r.y1 = H.dirty[i].y1;
        }
        H.ndirty = 0;
    }
    H.dirty[H.ndirty++] = r;
}

static void sync_for(const rect_t *fp, int n) {
    for (int i = 0; i < H.ndirty; ) {
        int hit = 0;
        for (int j = 0; j < n; j++)
            if (overlap(&H.dirty[i], &fp[j])) { hit = 1; break; }
        if (!hit) { i++; continue; }
        cmd_t *c = new_cmd(OP_SYNC);
        if (!c) return;
        c->x = (s16)H.dirty[i].x0; c->y = (s16)H.dirty[i].y0;
        c->w = (s16)(H.dirty[i].x1 - H.dirty[i].x0);
        c->h = (s16)(H.dirty[i].y1 - H.dirty[i].y0);
        H.dirty[i] = H.dirty[--H.ndirty];
    }
}

static int tex_footprint(u16 tpage, u16 clut, rect_t *fp) {
    s32 px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256;
    u32 depth = (tpage >> 7) & 3u;
    if (depth == 3) depth = 2;
    s32 w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    int n = split_wrap(px, py, w, 256, fp);
    if (depth < 2)
        n += split_wrap((clut & 63) * 16, (clut >> 6) & 511, depth == 0 ? 16 : 256, 1, fp + n);
    return n;
}

static int scissor_of(const gpu_sw_state_t *st, VkRect2D *sc) {
    s32 x0 = st->area_x0, y0 = st->area_y0, x1 = st->area_x1, y1 = st->area_y1;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1;
    if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x1 < x0 || y1 < y0) return 0;
    sc->offset.x = x0 * H.S;
    sc->offset.y = y0 * H.S;
    sc->extent.width = (u32)(x1 - x0 + 1) * (u32)H.S;
    sc->extent.height = (u32)(y1 - y0 + 1) * (u32)H.S;
    return 1;
}

static void twin_word(u16 tw_mask, u16 tw_off, u32 *out) {
    u32 mu = tw_mask & 0xFFu, mv = tw_mask >> 8;
    u32 ou = tw_off & 0xFFu,  ov = tw_off >> 8;
    *out = (~mu & 0xFFu) | ((ou & mu) << 8) | ((~mv & 0xFFu) << 16) | ((ov & mv) << 24);
}

static void set_vtx(hw_vtx_t *o, float x, float y, float z, int persp,
                    float u, float v, u8 r, u8 g, u8 b, const u32 info[4]) {
    o->x = x * (float)H.S;
    o->y = y * (float)H.S;
    o->z = z;
    o->w = persp ? 1.0f : 0.0f;
    o->u = u; o->v = v;
    o->r = r; o->g = g; o->b = b; o->a = 255;
    memcpy(o->info, info, sizeof o->info);
}

static void emit(const gpu_sw_state_t *st, int textured, int semitrans, u16 tpage,
                 u16 clut, const hw_vtx_t *v, u32 n, rect_t bbox) {
    VkRect2D sc;
    if (!n || !scissor_of(st, &sc)) return;
    rect_t area = { st->area_x0, st->area_y0, st->area_x1 + 1, st->area_y1 + 1 };
    if (bbox.x0 < area.x0) bbox.x0 = area.x0;
    if (bbox.y0 < area.y0) bbox.y0 = area.y0;
    if (bbox.x1 > area.x1) bbox.x1 = area.x1;
    if (bbox.y1 > area.y1) bbox.y1 = area.y1;
    if (textured) {
        rect_t fp[8];
        sync_for(fp, tex_footprint(tpage, clut, fp));
    }
    if (st->mask_and) sync_for(&bbox, 1);
    u32 first = push_verts(v, n);
    if (first == UINT32_MAX) return;
    u8 blend = (u8)(P_BLEND0 + ((tpage >> 5) & 3));
    if (!semitrans)      add_draw(P_OPAQUE, 0, sc, first, n);
    else if (!textured)  add_draw(blend, 0, sc, first, n);
    else                 add_draw_pair(P_OPAQUE, blend, sc, first, n);
    dirty_add(bbox);
}

static void bbox_add(rect_t *b, float x, float y) {
    s32 x0 = (s32)x - 1, y0 = (s32)y - 1, x1 = (s32)x + 2, y1 = (s32)y + 2;
    if (x0 < b->x0) b->x0 = x0;
    if (y0 < b->y0) b->y0 = y0;
    if (x1 > b->x1) b->x1 = x1;
    if (y1 > b->y1) b->y1 = y1;
}

static void prim_info(u32 info[4], int textured, int raw, int flat, u16 tpage,
                      u16 clut, u16 tw_mask, u16 tw_off, const gpu_sw_state_t *st) {
    info[0] = tpage;
    info[1] = clut;
    twin_word(tw_mask, tw_off, &info[2]);
    info[3] = (textured ? F_TEXTURED : 0u) | (raw ? F_RAW : 0u) |
              (st->mask_or ? F_MASK_OR : 0u) | (flat ? F_FLAT : 0u) |
              (st->mask_and ? F_CHECK : 0u) | (H.ztest ? F_ZTEST : 0u) | (H.draw_order << 7);
}

void hw_effect_depth_test(int on) { H.ztest = on; }
void hw_draw_order(u32 order) { H.draw_order = order; }

void hw_poly(const gpu_sw_state_t *st, const gpu_poly_t *p, const hw_pv_t pv[4]) {
    if (!H.ready) return;
    int n = p->nverts == 4 ? 4 : 3;
    int textured = p->textured != 0;
    int raw = textured && p->raw;
    u32 info[4];
    prim_info(info, textured, raw, !p->gouraud, p->tpage, p->clut, p->twin_mask,
              p->twin_off, st);

    s32 ix[4], iy[4];
    float fx[4], fy[4], fz[4];
    int ok[4];
    for (int i = 0; i < n; i++) {
        ix[i] = sx11(p->x[i]) + st->ofs_x;
        iy[i] = sx11(p->y[i]) + st->ofs_y;
        ok[i] = pv && pv[i].ok;
        if (ok[i]) {
            fx[i] = pv[i].v.sx + (float)st->ofs_x;
            fy[i] = pv[i].v.sy + (float)st->ofs_y;
            fz[i] = pv[i].v.vz;
        } else {
            fx[i] = (float)ix[i];
            fy[i] = (float)iy[i];
            fz[i] = 0.0f;
        }
    }

    hw_vtx_t v[6];
    u32 nv = 0;
    rect_t bb = { 1 << 20, 1 << 20, -(1 << 20), -(1 << 20) };
    static const int tri[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    for (int t = 0; t < (n == 4 ? 2 : 1); t++) {
        const int *k = tri[t];
        s32 minx = ix[k[0]], maxx = minx, miny = iy[k[0]], maxy = miny;
        for (int j = 1; j < 3; j++) {
            if (ix[k[j]] < minx) minx = ix[k[j]];
            if (ix[k[j]] > maxx) maxx = ix[k[j]];
            if (iy[k[j]] < miny) miny = iy[k[j]];
            if (iy[k[j]] > maxy) maxy = iy[k[j]];
        }
        if (miny == maxy || maxy - miny >= 512 || maxx - minx >= 1024) continue;
        s32 area = (ix[k[1]] - ix[k[0]]) * (iy[k[2]] - iy[k[1]]) -
                   (ix[k[2]] - ix[k[1]]) * (iy[k[1]] - iy[k[0]]);
        if (!area) continue;
        int precise = ok[k[0]] && ok[k[1]] && ok[k[2]];
        float px[3], py[3];
        for (int j = 0; j < 3; j++) {
            px[j] = precise ? fx[k[j]] : (float)ix[k[j]];
            py[j] = precise ? fy[k[j]] : (float)iy[k[j]];
        }
        float du = 0.0f, dv = 0.0f;
        if (textured) {
            float ex1 = px[1] - px[0], ey1 = py[1] - py[0];
            float ex2 = px[2] - px[0], ey2 = py[2] - py[0];
            float det = ex1 * ey2 - ex2 * ey1;
            if (det != 0.0f) {
                float u1 = (float)p->u[k[1]] - p->u[k[0]], u2 = (float)p->u[k[2]] - p->u[k[0]];
                float v1 = (float)p->v[k[1]] - p->v[k[0]], v2 = (float)p->v[k[2]] - p->v[k[0]];
                float ux = (u1 * ey2 - u2 * ey1) / det, uy = (ex1 * u2 - ex2 * u1) / det;
                float vx = (v1 * ey2 - v2 * ey1) / det, vy = (ex1 * v2 - ex2 * v1) / det;
                du = 0.5f - 0.5f * (ux + uy);
                dv = 0.5f - 0.5f * (vx + vy);
            }
        }
        for (int j = 0; j < 3; j++) {
            int i = k[j];
            set_vtx(&v[nv++], px[j], py[j], precise ? fz[i] : 0.0f,
                    precise && H.persp, (float)p->u[i] + du, (float)p->v[i] + dv,
                    p->r[i], p->g[i], p->b[i], info);
            bbox_add(&bb, px[j], py[j]);
        }
        H.prims++;
        if (precise) H.precise++;
    }
    emit(st, textured, p->semitrans, p->tpage, p->clut, v, nv, bb);
}

void hw_rect(const gpu_sw_state_t *st, const gpu_rect_t *r) {
    if (!H.ready) return;
    int textured = r->textured != 0;
    s32 x = sx11(sx11(r->x) + st->ofs_x);
    s32 y = sx11(sx11(r->y) + st->ofs_y);
    s32 w = r->w & 0x3FF, h = r->h & 0x1FF;
    if (!w || !h) return;
    u32 info[4];
    prim_info(info, textured, textured && r->raw, 1, r->tpage, r->clut,
              r->twin_mask, r->twin_off, st);

    float u0 = r->u, u1 = (float)r->u + (float)w;
    float v0 = r->v, v1 = (float)r->v + (float)h;
    if (textured && (st->texpage & 0x1000u)) {
        u0 = (float)(r->u | 1) + 1.0f; u1 = u0 - (float)w;
    }
    if (textured && (st->texpage & 0x2000u)) {
        v0 = (float)r->v + 1.0f; v1 = v0 - (float)h;
    }
    hw_vtx_t v[6];
    float X0 = (float)x, Y0 = (float)y, X1 = (float)(x + w), Y1 = (float)(y + h);
    set_vtx(&v[0], X0, Y0, 0, 0, u0, v0, r->r, r->g, r->b, info);
    set_vtx(&v[1], X1, Y0, 0, 0, u1, v0, r->r, r->g, r->b, info);
    set_vtx(&v[2], X0, Y1, 0, 0, u0, v1, r->r, r->g, r->b, info);
    v[3] = v[1];
    v[4] = v[2];
    set_vtx(&v[5], X1, Y1, 0, 0, u1, v1, r->r, r->g, r->b, info);
    rect_t bb = { x, y, x + w, y + h };
    H.prims++;
    emit(st, textured, r->semitrans, r->tpage, r->clut, v, 6, bb);
}

void hw_line(const gpu_sw_state_t *st, const gpu_line_t *l) {
    if (!H.ready || l->nverts < 2) return;
    u32 info[4];
    prim_info(info, 0, 0, !l->gouraud, l->tpage, 0, 0, 0, st);
    for (int i = 0; i + 1 < l->nverts; i++) {
        s32 x0 = sx11(l->x[i]) + st->ofs_x, y0 = sx11(l->y[i]) + st->ofs_y;
        s32 x1 = sx11(l->x[i + 1]) + st->ofs_x, y1 = sx11(l->y[i + 1]) + st->ofs_y;
        s32 dx = abs(x1 - x0), dy = abs(y1 - y0);
        if (dx >= 1024 || dy >= 512) continue;
        u8 r0 = l->r[i], g0 = l->g[i], b0 = l->b[i];
        u8 r1 = l->gouraud ? l->r[i + 1] : r0;
        u8 g1 = l->gouraud ? l->g[i + 1] : g0;
        u8 b1 = l->gouraud ? l->b[i + 1] : b0;
        float ax, ay, bx, by, cx, cy, ex, ey;
        if (dx >= dy) {
            if (x0 > x1) { s32 t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
                           u8 c = r0; r0 = r1; r1 = c; c = g0; g0 = g1; g1 = c;
                           c = b0; b0 = b1; b1 = c; }
            ax = (float)x0;       ay = (float)y0;
            bx = (float)x1 + 1;   by = (float)y1;
            cx = (float)x0;       cy = (float)y0 + 1;
            ex = (float)x1 + 1;   ey = (float)y1 + 1;
        } else {
            if (y0 > y1) { s32 t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
                           u8 c = r0; r0 = r1; r1 = c; c = g0; g0 = g1; g1 = c;
                           c = b0; b0 = b1; b1 = c; }
            ax = (float)x0;       ay = (float)y0;
            bx = (float)x0 + 1;   by = (float)y0;
            cx = (float)x1;       cy = (float)y1 + 1;
            ex = (float)x1 + 1;   ey = (float)y1 + 1;
        }
        hw_vtx_t v[6];
        set_vtx(&v[0], ax, ay, 0, 0, 0, 0, r0, g0, b0, info);
        set_vtx(&v[1], bx, by, 0, 0, 0, 0, dx >= dy ? r1 : r0, dx >= dy ? g1 : g0, dx >= dy ? b1 : b0, info);
        set_vtx(&v[2], cx, cy, 0, 0, 0, 0, dx >= dy ? r0 : r1, dx >= dy ? g0 : g1, dx >= dy ? b0 : b1, info);
        v[3] = v[1];
        v[4] = v[2];
        set_vtx(&v[5], ex, ey, 0, 0, 0, 0, r1, g1, b1, info);
        rect_t bb = { (x0 < x1 ? x0 : x1), (y0 < y1 ? y0 : y1),
                      (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1 };
        H.prims++;
        emit(st, 0, l->semitrans, l->tpage, 0, v, 6, bb);
    }
}

static float c5(u16 p, int shift) {
    return (float)((p >> shift) & 31u) / 31.0f;
}

void hw_fill(const PSX_RECT *r, u16 pixel) {
    if (!H.ready || !r) return;
    rect_t pieces[4];
    int n = split_wrap(r->x, r->y, r->w, r->h, pieces);
    for (int i = 0; i < n; i++) {
        cmd_t *c = new_cmd(OP_FILL);
        if (!c) return;
        c->x = (s16)pieces[i].x0; c->y = (s16)pieces[i].y0;
        c->w = (s16)(pieces[i].x1 - pieces[i].x0);
        c->h = (s16)(pieces[i].y1 - pieces[i].y0);
        c->color[0] = c5(pixel, 0);
        c->color[1] = c5(pixel, 5);
        c->color[2] = c5(pixel, 10);
        c->color[3] = (pixel & 0x8000u) ? 1.0f : 0.0f;
        dirty_add(pieces[i]);
    }
}

void hw_copy(const gpu_sw_state_t *st, const PSX_RECT *src, s32 dx, s32 dy) {
    if (!H.ready || !src || src->w <= 0 || src->h <= 0) return;
    rect_t fp[4];
    sync_for(fp, split_wrap(src->x, src->y, src->w, src->h, fp));

    u32 info[4] = { 0, 0, 0, F_COPY | (st->mask_or ? F_MASK_OR : 0u) |
                                   (st->mask_and ? F_CHECK : 0u) };
    VkRect2D full = { { 0, 0 }, { H.W, H.Hs } };
    u8 pipe = P_COPY;
    rect_t d[4];
    int n = split_wrap(dx, dy, src->w, src->h, d);
    s32 bx = dx & (VRAM_W - 1), by = dy & (VRAM_H - 1);
    for (int i = 0; i < n; i++) {
        s32 ox = (d[i].x0 - bx + VRAM_W) & (VRAM_W - 1);
        s32 oy = (d[i].y0 - by + VRAM_H) & (VRAM_H - 1);
        float su = (float)((src->x + ox) * H.S), sv = (float)((src->y + oy) * H.S);
        float w = (float)(d[i].x1 - d[i].x0), h = (float)(d[i].y1 - d[i].y0);
        float S = (float)H.S;
        hw_vtx_t v[6];
        set_vtx(&v[0], (float)d[i].x0, (float)d[i].y0, 0, 0, su, sv, 0, 0, 0, info);
        set_vtx(&v[1], (float)d[i].x1, (float)d[i].y0, 0, 0, su + w * S, sv, 0, 0, 0, info);
        set_vtx(&v[2], (float)d[i].x0, (float)d[i].y1, 0, 0, su, sv + h * S, 0, 0, 0, info);
        v[3] = v[1];
        v[4] = v[2];
        set_vtx(&v[5], (float)d[i].x1, (float)d[i].y1, 0, 0, su + w * S, sv + h * S, 0, 0, 0, info);
        if (st->mask_and) sync_for(&d[i], 1);
        u32 first = push_verts(v, 6);
        if (first == UINT32_MAX) return;
        add_draw(pipe, 0, full, first, 6);
        dirty_add(d[i]);
    }
}

void hw_upload(const PSX_RECT *r, const u16 *vram) {
    if (!H.ready || !r || !vram) return;
    rect_t pieces[4];
    int n = split_wrap(r->x, r->y, r->w, r->h, pieces);
    for (int i = 0; i < n; i++) {
        u32 w = (u32)(pieces[i].x1 - pieces[i].x0), h = (u32)(pieces[i].y1 - pieces[i].y0);
        u32 need = H.nstaging + w * h * 4u;
        H.staging = grow(H.staging, &H.cap_staging, need, 1);
        if (need > H.cap_staging) return;
        u8 *o = H.staging + H.nstaging;
        for (u32 yy = 0; yy < h; yy++)
            for (u32 xx = 0; xx < w; xx++) {
                u16 p = vram[(u32)(pieces[i].y0 + (s32)yy) * VRAM_W + (u32)(pieces[i].x0 + (s32)xx)];
                u32 r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
                o[0] = (u8)((r5 << 3) | (r5 >> 2));
                o[1] = (u8)((g5 << 3) | (g5 >> 2));
                o[2] = (u8)((b5 << 3) | (b5 >> 2));
                o[3] = (p & 0x8000u) ? 255 : 0;
                o += 4;
            }
        cmd_t *c = new_cmd(OP_UPLOAD);
        if (!c) return;
        c->x = (s16)pieces[i].x0; c->y = (s16)pieces[i].y0;
        c->w = (s16)w; c->h = (s16)h;
        c->staging = H.nstaging;
        H.nstaging = need;
        cmd_t *z = new_cmd(OP_ZCLEAR);
        if (!z) return;
        z->x = c->x; z->y = c->y; z->w = c->w; z->h = c->h;
    }
}

#ifndef AC3_HW_WEBGPU   /* GPU half (Vulkan); the WebGPU build uses renderer/webgpu/hw_webgpu.c */
static int ensure_buffers(int slot) {
    VkDeviceSize vneed = (VkDeviceSize)H.nverts * sizeof(hw_vtx_t);
    VkDeviceSize sneed = H.nstaging;
    if (vneed > H.vcap[slot] || !H.vbuf[slot]) {
        VkDeviceSize cap = H.vcap[slot] ? H.vcap[slot] : (1u << 20);
        while (cap < vneed) cap *= 2;
        free_host_buffer(&H.vbuf[slot], &H.vmem[slot], &H.vmap[slot]);
        if (!make_host_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &H.vbuf[slot], &H.vmem[slot], &H.vmap[slot])) return 0;
        H.vcap[slot] = cap;
    }
    if (sneed > H.scap[slot] || !H.sbuf[slot]) {
        VkDeviceSize cap = H.scap[slot] ? H.scap[slot] : (1u << 20);
        while (cap < sneed) cap *= 2;
        free_host_buffer(&H.sbuf[slot], &H.smem[slot], &H.smap[slot]);
        if (!make_host_buffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              &H.sbuf[slot], &H.smem[slot], &H.smap[slot])) return 0;
        H.scap[slot] = cap;
    }
    return 1;
}

static VkRect2D scaled(s32 x, s32 y, s32 w, s32 h) {
    VkRect2D r = { { x * H.S, y * H.S }, { (u32)(w * H.S), (u32)(h * H.S) } };
    return r;
}

typedef struct { VkCommandBuffer cb; int in_pass; int pipe; VkRect2D sc; int slot; } rec_t;

static void begin_pass(rec_t *R) {
    if (R->in_pass) return;
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = H.pass;
    rp.framebuffer = H.fb;
    rp.renderArea.extent.width = H.W;
    rp.renderArea.extent.height = H.Hs;
    vkCmdBeginRenderPass(R->cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)H.W, (float)H.Hs, 0, 1 };
    vkCmdSetViewport(R->cb, 0, 1, &vp);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(R->cb, 0, 1, &H.vbuf[R->slot], &off);
    vkCmdBindDescriptorSets(R->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.layout,
                            0, 1, &H.set_read, 0, NULL);
    R->in_pass = 1;
    R->pipe = -1;
    R->sc.extent.width = 0;
}

static void end_pass(rec_t *R) {
    if (!R->in_pass) return;
    vkCmdEndRenderPass(R->cb);
    R->in_pass = 0;
}

#define ST_ATT  (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
#define ST_XFER (VK_PIPELINE_STAGE_TRANSFER_BIT)
#define ST_FRAG (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define L_ATT   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
#define L_READ  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
#define L_SRC   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
#define L_DST   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
#define A_ATTW  (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)

static u32 do_syncs(rec_t *R, u32 i) {
    VkImageCopy regions[MAX_DIRTY * 2];
    u32 n = 0;
    for (; i < H.ncmds && H.cmds[i].op == OP_SYNC && n < MAX_DIRTY * 2; i++) {
        const cmd_t *c = &H.cmds[i];
        VkImageCopy *g = &regions[n++];
        memset(g, 0, sizeof *g);
        g->srcSubresource.aspectMask = g->dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        g->srcSubresource.layerCount = g->dstSubresource.layerCount = 1;
        g->srcOffset.x = g->dstOffset.x = c->x * H.S;
        g->srcOffset.y = g->dstOffset.y = c->y * H.S;
        g->extent.width = (u32)(c->w * H.S);
        g->extent.height = (u32)(c->h * H.S);
        g->extent.depth = 1;
    }
    barrier(R->cb, H.target, L_ATT, L_SRC, A_ATTW, VK_ACCESS_TRANSFER_READ_BIT, ST_ATT, ST_XFER);
    barrier(R->cb, H.read, L_READ, L_DST, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, ST_FRAG, ST_XFER);
    vkCmdCopyImage(R->cb, H.target, L_SRC, H.read, L_DST, n, regions);
    barrier(R->cb, H.target, L_SRC, L_ATT, VK_ACCESS_TRANSFER_READ_BIT, A_ATTW, ST_XFER, ST_ATT);
    barrier(R->cb, H.read, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
    return i;
}

static u32 do_uploads(rec_t *R, u32 i) {
    barrier(R->cb, H.upimg, VK_IMAGE_LAYOUT_UNDEFINED, L_DST, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ST_XFER, ST_XFER);
    u32 j = i;
    for (; j < H.ncmds && (H.cmds[j].op == OP_UPLOAD || H.cmds[j].op == OP_ZCLEAR); j++) {
        const cmd_t *c = &H.cmds[j];
        if (c->op != OP_UPLOAD) continue;
        VkBufferImageCopy b = { 0 };
        b.bufferOffset = c->staging;
        b.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.imageSubresource.layerCount = 1;
        b.imageOffset.x = c->x; b.imageOffset.y = c->y;
        b.imageExtent.width = (u32)c->w; b.imageExtent.height = (u32)c->h;
        b.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(R->cb, H.sbuf[R->slot], H.upimg, L_DST, 1, &b);
    }
    barrier(R->cb, H.upimg, L_DST, L_SRC, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, ST_XFER, ST_XFER);
    barrier(R->cb, H.target, L_ATT, L_DST, A_ATTW, VK_ACCESS_TRANSFER_WRITE_BIT, ST_ATT, ST_XFER);
    barrier(R->cb, H.read, L_READ, L_DST, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, ST_FRAG, ST_XFER);
    for (u32 k = i; k < j; k++) {
        const cmd_t *c = &H.cmds[k];
        if (c->op != OP_UPLOAD) continue;
        VkImageBlit bl = { 0 };
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
        bl.srcOffsets[0].x = c->x; bl.srcOffsets[0].y = c->y;
        bl.srcOffsets[1].x = c->x + c->w; bl.srcOffsets[1].y = c->y + c->h; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[0].x = c->x * H.S; bl.dstOffsets[0].y = c->y * H.S;
        bl.dstOffsets[1].x = (c->x + c->w) * H.S; bl.dstOffsets[1].y = (c->y + c->h) * H.S;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(R->cb, H.upimg, L_SRC, H.target, L_DST, 1, &bl, VK_FILTER_NEAREST);
        vkCmdBlitImage(R->cb, H.upimg, L_SRC, H.read, L_DST, 1, &bl, VK_FILTER_NEAREST);
    }
    barrier(R->cb, H.target, L_DST, L_ATT, VK_ACCESS_TRANSFER_WRITE_BIT, A_ATTW, ST_XFER, ST_ATT);
    barrier(R->cb, H.read, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
    begin_pass(R);
    for (u32 k = i; k < j; k++) {
        const cmd_t *c = &H.cmds[k];
        if (c->op != OP_ZCLEAR) continue;
        VkClearAttachment a = { VK_IMAGE_ASPECT_COLOR_BIT, 1, { { { 0, 0, 0, 0 } } } };
        VkClearRect cr = { scaled(c->x, c->y, c->w, c->h), 0, 1 };
        vkCmdClearAttachments(R->cb, 1, &a, 1, &cr);
        a.colorAttachment = 2;
        vkCmdClearAttachments(R->cb, 1, &a, 1, &cr);
    }
    return j;
}

void hw_record(VkCommandBuffer cb, int slot) {
    if (!H.ready) return;
    if (slot < 0 || slot >= HW_FRAMES) slot = 0;
    if (!ensure_buffers(slot)) { H.nverts = H.nstaging = H.ncmds = 0; return; }
    if (H.nverts) memcpy(H.vmap[slot], H.verts, (size_t)H.nverts * sizeof(hw_vtx_t));
    if (H.nstaging) memcpy(H.smap[slot], H.staging, H.nstaging);

    if (!H.images_cleared) {
        VkImage imgs[6] = { H.target, H.read, H.zimg, H.zsnap, H.order, H.order_snap };
        VkClearColorValue zero = { { 0, 0, 0, 0 } };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < 6; i++) {
            barrier(cb, imgs[i], VK_IMAGE_LAYOUT_UNDEFINED, L_DST, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, ST_XFER, ST_XFER);
            vkCmdClearColorImage(cb, imgs[i], L_DST, &zero, 1, &rg);
        }
        barrier(cb, H.target, L_DST, L_ATT, VK_ACCESS_TRANSFER_WRITE_BIT, A_ATTW, ST_XFER, ST_ATT);
        barrier(cb, H.zimg, L_DST, L_ATT, VK_ACCESS_TRANSFER_WRITE_BIT, A_ATTW, ST_XFER, ST_ATT);
        barrier(cb, H.read, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
        barrier(cb, H.zsnap, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
        barrier(cb, H.order, L_DST, L_ATT, VK_ACCESS_TRANSFER_WRITE_BIT, A_ATTW, ST_XFER, ST_ATT);
        barrier(cb, H.order_snap, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
        H.images_cleared = 1;
    } else {
        barrier(cb, H.target, L_READ, L_ATT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                A_ATTW, ST_FRAG | ST_XFER, ST_ATT);
        barrier(cb, H.zimg, L_READ, L_ATT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                A_ATTW, ST_FRAG | ST_XFER, ST_ATT);
    }

    hw_fx_prepare(cb, slot);

    rec_t R = { cb, 0, -1, { { 0, 0 }, { 0, 0 } }, slot };
    float inv[2] = { 1.0f / (float)H.W, 1.0f / (float)H.Hs };
    for (u32 i = 0; i < H.ncmds; ) {
        const cmd_t *c = &H.cmds[i];
        switch (c->op) {
        case OP_DRAW: {
            begin_pass(&R);
            if (R.pipe != c->pipe) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.pipe[c->pipe]);
                R.pipe = c->pipe;
            }
            if (!same_rect(&R.sc, &c->sc)) {
                vkCmdSetScissor(cb, 0, 1, &c->sc);
                R.sc = c->sc;
            }
            prim_push_t pc = { { inv[0], inv[1] }, H.S, c->pass };
            vkCmdPushConstants(cb, H.layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof pc, &pc);
            vkCmdDraw(cb, c->count, 1, c->first, 0);
            i++;
            break;
        }
        case OP_FILL: {
            begin_pass(&R);
            VkClearAttachment a[3] = {
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, { { { c->color[0], c->color[1], c->color[2], c->color[3] } } } },
                { VK_IMAGE_ASPECT_COLOR_BIT, 1, { { { 0, 0, 0, 0 } } } },
            };
            VkClearRect cr = { scaled(c->x, c->y, c->w, c->h), 0, 1 };
            a[2] = a[1]; a[2].colorAttachment = 2;
            vkCmdClearAttachments(cb, 3, a, 1, &cr);
            i++;
            break;
        }
        case OP_ZCLEAR: {
            begin_pass(&R);
            VkClearAttachment a = { VK_IMAGE_ASPECT_COLOR_BIT, 1, { { { 0, 0, 0, 0 } } } };
            VkClearRect cr = { scaled(c->x, c->y, c->w, c->h), 0, 1 };
            vkCmdClearAttachments(cb, 1, &a, 1, &cr);
            a.colorAttachment = 2;
            vkCmdClearAttachments(cb, 1, &a, 1, &cr);
            i++;
            break;
        }
        case OP_UPLOAD:
            end_pass(&R);
            i = do_uploads(&R, i);
            break;
        case OP_SYNC:
            end_pass(&R);
            i = do_syncs(&R, i);
            break;
        case OP_FX_BEGIN:
        case OP_FX_END:
            end_pass(&R);
            if (c->op == OP_FX_BEGIN) {
                barrier(cb, H.order, L_ATT, L_DST, A_ATTW, VK_ACCESS_TRANSFER_WRITE_BIT, ST_ATT, ST_XFER);
                VkClearColorValue zero = { { 0, 0, 0, 0 } };
                VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(cb, H.order, L_DST, &zero, 1, &rg);
                barrier(cb, H.order, L_DST, L_ATT, VK_ACCESS_TRANSFER_WRITE_BIT, A_ATTW, ST_XFER, ST_ATT);
            } else {
                barrier(cb, H.order, L_ATT, L_SRC, A_ATTW, VK_ACCESS_TRANSFER_READ_BIT, ST_ATT, ST_XFER);
                barrier(cb, H.order_snap, L_READ, L_DST, VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, ST_FRAG, ST_XFER);
                VkImageCopy cp = { 0 };
                cp.srcSubresource.aspectMask = cp.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                cp.srcSubresource.layerCount = cp.dstSubresource.layerCount = 1;
                cp.extent.width = H.W; cp.extent.height = H.Hs; cp.extent.depth = 1;
                vkCmdCopyImage(cb, H.order, L_SRC, H.order_snap, L_DST, 1, &cp);
                barrier(cb, H.order, L_SRC, L_ATT, VK_ACCESS_TRANSFER_READ_BIT, A_ATTW, ST_XFER, ST_ATT);
                barrier(cb, H.order_snap, L_DST, L_READ, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
            }
            hw_fx_record(cb, slot, c);
            i++;
            break;
        default:
            i++;
            break;
        }
    }
    end_pass(&R);
    barrier(cb, H.target, L_ATT, L_READ, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, ST_ATT, ST_FRAG);
    barrier(cb, H.zimg, L_ATT, L_READ, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, ST_ATT, ST_FRAG);

    H.nverts = H.nstaging = H.ncmds = 0;
    hw_fx_frame_done();
    H.last_prims = H.prims;
    H.last_precise = H.precise;
    H.prims = H.precise = 0;
}

typedef struct {
    float origin[2], size[2], view_pos[2], view_size[2], inv_vram[2];
    float half_texel;
} disp_push_t;

int hw_display_init(VkRenderPass pass) {
    if (!H.ready) return 0;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(disp_push_t) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &H.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VKOK(vkCreatePipelineLayout(H.dev, &pli, NULL, &H.disp_layout));

    VkShaderModule vs = shader(spv_psx_vert, sizeof spv_psx_vert);
    VkShaderModule fs = shader(spv_hw_display_frag, sizeof spv_hw_display_frag);
    if (!vs || !fs) return 0;
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb = { 0 };
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &ds;
    gp.layout = H.disp_layout;
    gp.renderPass = pass;
    VKOK(vkCreateGraphicsPipelines(H.dev, VK_NULL_HANDLE, 1, &gp, NULL, &H.disp_pipe));
    vkDestroyShaderModule(H.dev, vs, NULL);
    vkDestroyShaderModule(H.dev, fs, NULL);
    return 1;
}

void hw_display_draw(VkCommandBuffer cb, const gpu_display_t *d, const VkViewport *vpt) {
    if (!H.ready || !H.disp_pipe || !d) return;
    VkRect2D sc = { { (int32_t)vpt->x, (int32_t)vpt->y },
                    { (uint32_t)vpt->width, (uint32_t)vpt->height } };
    vkCmdSetViewport(cb, 0, 1, vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    disp_push_t pc = {
        { (float)d->x, (float)d->y }, { (float)d->w, (float)d->h },
        { vpt->x, vpt->y }, { vpt->width, vpt->height },
        { 1.0f / VRAM_W, 1.0f / VRAM_H },
        0.5f / (float)H.S,
    };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.disp_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.disp_layout,
                            0, 1, &H.set_disp, 0, NULL);
    vkCmdPushConstants(cb, H.disp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

static int readback_image(VkQueue q, VkCommandPool pool, VkImage img, u32 bpp,
                          int x, int y, int w, int h, void *out) {
    if (!H.ready || w <= 0 || h <= 0) return 0;
    u32 W = (u32)(w * H.S), Hh = (u32)(h * H.S);
    VkDeviceSize bytes = (VkDeviceSize)W * Hh * bpp;
    VkBuffer buf; VkDeviceMemory mem; void *map = NULL;
    if (!make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map))
        return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(H.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    barrier(cb, img, L_READ, L_SRC, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, ST_FRAG, ST_XFER);
    VkBufferImageCopy rg = { 0 };
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.x = x * H.S;
    rg.imageOffset.y = y * H.S;
    rg.imageExtent.width = W;
    rg.imageExtent.height = Hh;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, img, L_SRC, buf, 1, &rg);
    barrier(cb, img, L_SRC, L_READ, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, ST_XFER, ST_FRAG);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    memcpy(out, map, (size_t)bytes);
    vkFreeCommandBuffers(H.dev, pool, 1, &cb);
    free_host_buffer(&buf, &mem, &map);
    return 1;
}

int hw_readback(VkQueue q, VkCommandPool pool, int x, int y, int w, int h, u8 *rgba) {
    return readback_image(q, pool, H.target, 4, x, y, w, h, rgba);
}

int hw_readback_depth(VkQueue q, VkCommandPool pool, int x, int y, int w, int h, float *z) {
    return readback_image(q, pool, H.zimg, 4, x, y, w, h, z);
}

#endif /* !AC3_HW_WEBGPU */