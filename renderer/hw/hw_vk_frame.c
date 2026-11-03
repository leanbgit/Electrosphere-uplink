/*
 * hw_vk_frame.c -- each frame on the GPU (native builds): the command list
 * that hw.c built from the game's drawing is recorded into a Vulkan command
 * buffer (hw_record), then shown (hw_display_*) or read back (the replay tool
 * and the self-test). The web build does this in renderer/webgpu/hw_webgpu.c.
 */
#include "hw_vk.h"
#ifndef AC3_HW_WEBGPU
#include "ac3_shaders.h"
#include "ac3_shaders_hw.h"
#include <stdlib.h>
#include <string.h>

#define H hw_state

/* helpers from hw_vk_setup.c */
#define make_host_buffer hw_make_host_buffer
#define free_host_buffer hw_free_host_buffer
#define barrier          hw_barrier
#define shader           hw_shader

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
