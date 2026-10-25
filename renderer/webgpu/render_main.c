/*
 * render_main.c -- the render worker's program (web/render.wasm).
 *
 * It owns the WebGPU device and the visible canvas, and draws the fields the
 * game worker sends (renderer/webgpu/hw_packet.h) with hw_webgpu.c. Because
 * this worker returns to its event loop after every field, the browser
 * presents the canvas normally, GPU errors and device loss are reported, and
 * web/render-worker.js can tell the game when the GPU has finished a field
 * (the game then never gets more than two fields ahead, like the native
 * build's two frames in flight).
 */
#include "hw_packet.h"
#include "hw_fx_web.h"
#include <emscripten.h>
#include <emscripten/html5_webgpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hw_state_t hw_state;
#define H hw_state

EM_JS(int, rw_format, (void), { return WebGPU.TextureFormat.indexOf(RW.format); });
EM_JS(int, rw_view, (void), { return WebGPU.mgrTextureView.create(RW.ctx.getCurrentTexture().createView()); });

static WGPUDevice dev;
static WGPUQueue queue;
static u8 *pkt;
static size_t pkt_cap;

/* returns the internal scale actually used (it can be lowered to fit the GPU), 0 on failure */
EMSCRIPTEN_KEEPALIVE int render_init(int scale) {
    dev = emscripten_webgpu_get_device();
    if (!dev) return 0;
    queue = wgpuDeviceGetQueue(dev);
    hw_webgpu_set_canvas_format(rw_format());
    hw_config_t cfg = { scale, 1 };
    if (!hw_init_webgpu(dev, &cfg)) return 0;
    return H.S;
}

/* room for the next packet; JS copies it here, then calls render_frame */
EMSCRIPTEN_KEEPALIVE u8 *render_buffer(u32 size) {
    if (size > pkt_cap) {
        free(pkt);
        pkt = (u8 *)malloc(size);
        pkt_cap = pkt ? size : 0;
    }
    return pkt;
}

/* same letterboxing as renderer/gpu_vulkan.c: display_viewport() */
static void viewport(int cw, int ch, int wide, float *v) {
    float num = wide ? 16.0f : 4.0f, den = wide ? 9.0f : 3.0f;
    float w = (float)cw, h = (float)ch;
    if (w * den > h * num) w = floorf(h * num / den); else h = floorf(w * den / num);
    v[0] = floorf(((float)cw - w) * 0.5f); v[1] = floorf(((float)ch - h) * 0.5f); v[2] = w; v[3] = h;
}

/* returns the packet's field number, 0 if the packet was not valid */
EMSCRIPTEN_KEEPALIVE u32 render_frame(u32 len, int cw, int ch) {
    if (!pkt || len < sizeof(hw_packet_hdr_t) || len > pkt_cap) return 0;
    hw_packet_hdr_t *h = (hw_packet_hdr_t *)pkt;
    size_t c = (size_t)h->ncmds * sizeof(cmd_t), v = (size_t)h->nverts * sizeof(hw_vtx_t);
    size_t r = (h->flags & PKT_VRAM) ? (size_t)VRAM_W * VRAM_H * 2 : 0;
    if (h->magic != PKT_MAGIC || sizeof *h + c + v + h->nstaging + h->fx_bytes + r != len) return 0;
    u8 *p = pkt + sizeof *h;
    H.cmds = (cmd_t *)p;      H.ncmds = h->ncmds;      p += c;
    H.verts = (hw_vtx_t *)p;  H.nverts = h->nverts;    p += v;
    H.staging = p;            H.nstaging = h->nstaging; p += h->nstaging;
    if (!hw_fx_unpack(p, h->fx_bytes)) printf("renderer: bad effects data in field %u\n", h->seq);
    p += h->fx_bytes;
    const u16 *vram = r ? (const u16 *)p : NULL;

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, NULL);
    hw_record_webgpu(enc);     /* also replays uploads etc. when the picture is not shown */
    H.cmds = NULL; H.verts = NULL; H.staging = NULL;   /* they point into the packet */

    WGPUTextureView view = (WGPUTextureView)(uintptr_t)rw_view();
    WGPURenderPassColorAttachment at = { 0 };
    at.view = view;
    at.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    at.loadOp = WGPULoadOp_Clear;
    at.storeOp = WGPUStoreOp_Store;
    at.clearValue = (WGPUColor){ 0, 0, 0, 1 };
    WGPURenderPassDescriptor rd = { 0 };
    rd.colorAttachmentCount = 1;
    rd.colorAttachments = &at;
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rd);
    const gpu_display_t *d = &h->d;
    if (d->enabled && d->w > 0 && d->h > 0) {
        float vp[4];
        viewport(cw, ch, (h->flags & PKT_WIDE) != 0, vp);
        if (h->flags & PKT_HD) hw_display_webgpu(rp, d, vp[0], vp[1], vp[2], vp[3]);
        else if (vram)         hw_display_vram_webgpu(rp, d, vram, vp[0], vp[1], vp[2], vp[3]);
    }
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
    return h->seq;
}
