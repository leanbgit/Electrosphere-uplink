/*
 * hw_vk_setup.c -- the enhanced renderer's Vulkan objects (native builds):
 * the HD copy of VRAM and its depth, the render pass, descriptors and the
 * pipelines for each blending mode. The web build does this in
 * renderer/webgpu/hw_webgpu.c.
 */
#include "hw_vk.h"
#ifndef AC3_HW_WEBGPU
#include "ac3_shaders.h"
#include "ac3_shaders_hw.h"
#include <stdlib.h>
#include <string.h>

#define H hw_state

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
