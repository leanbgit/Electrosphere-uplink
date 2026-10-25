/*
 * hw_webgpu.c -- the GPU half of the enhanced renderer, on WebGPU.
 *
 * The API-independent half (primitive recording, dirty tracking, the per-frame
 * command list) is renderer/hw/hw.c, compiled with -DAC3_HW_WEBGPU. This file
 * replaces what renderer/hw/hw.c does with Vulkan: creating the HD targets and
 * pipelines, turning the command list into GPU work (hw_record_webgpu), and
 * drawing the HD picture or the original 15/24-bit picture into the canvas.
 *
 * Vulkan -> WebGPU differences handled here:
 *   vkCmdClearAttachments (partial clears)  -> "fill" pipelines: a solid quad
 *                                              inside a scissor rectangle
 *   vkCmdBlitImage (VRAM upload, x scale)    -> "blit" pipeline: nearest texel
 *                                              lookup at pixel / scale
 *   push constants                           -> uniform buffers (dynamic offsets)
 *   barriers / image layouts                 -> implicit in WebGPU
 *   VRAM upload rows                         -> repacked to 256-byte strides
 */
#include "hw_private.h"
#include "wgsl_shaders.h"
#include <emscripten/html5_webgpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H hw_state
#define ALIGN256(x) (((x) + 255u) & ~255u)

/* ------------------------------------------------------------ small WGSL */

static const char wgsl_fullscreen_vs[] =
    "@vertex fn main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
    "  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* replaces vkCmdClearAttachments: colour + depth + order, scissored */
static const char wgsl_fill_fs[] =
    "struct Fill { color: vec4<f32> };\n"
    "@group(0) @binding(0) var<uniform> F: Fill;\n"
    "struct Out { @location(0) c: vec4<f32>, @location(1) z: f32, @location(2) o: f32 };\n"
    "@fragment fn main() -> Out { return Out(F.color, 0.0, 0.0); }\n";

/* replaces vkCmdBlitImage(upimg -> target/read, nearest, x scale) */
static const char wgsl_blit_fs[] =
    "struct B { scale: i32, p0: i32, p1: i32, p2: i32 };\n"
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    "@group(0) @binding(1) var<uniform> P: B;\n"
    "@fragment fn main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {\n"
    "  return textureLoad(src, vec2<i32>(floor(pos.xy)) / P.scale, 0);\n"
    "}\n";

/* ------------------------------------------------------------- helpers */

static WGPUFeatureName dummy_feature;  /* keeps webgpu.h enums referenced */

static void on_error(WGPUErrorType type, char const *msg, void *user) {
    (void)user;
    static int shown;
    if (shown++ < 20) fprintf(stderr, "ac3: webgpu error %d: %s\n", (int)type, msg);
}

static WGPUShaderModule module(const char *code) {
    WGPUShaderModuleWGSLDescriptor w = { 0 };
    w.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    w.code = code;
    WGPUShaderModuleDescriptor d = { 0 };
    d.nextInChain = &w.chain;
    return wgpuDeviceCreateShaderModule(H.dev, &d);
}

static WGPUTexture texture(WGPUTextureFormat fmt, u32 w, u32 h, WGPUTextureUsageFlags usage,
                           WGPUTextureView *view) {
    WGPUTextureDescriptor d = { 0 };
    d.usage = usage;
    d.dimension = WGPUTextureDimension_2D;
    d.size.width = w; d.size.height = h; d.size.depthOrArrayLayers = 1;
    d.format = fmt;
    d.mipLevelCount = 1;
    d.sampleCount = 1;
    WGPUTexture t = wgpuDeviceCreateTexture(H.dev, &d);
    *view = wgpuTextureCreateView(t, NULL);
    return t;
}

static WGPUBuffer buffer(u64 size, WGPUBufferUsageFlags usage) {
    WGPUBufferDescriptor d = { 0 };
    d.usage = usage;
    d.size = size;
    return wgpuDeviceCreateBuffer(H.dev, &d);
}

static WGPUBindGroupLayoutEntry tex_entry(u32 binding, WGPUTextureSampleType st) {
    WGPUBindGroupLayoutEntry e = { 0 };
    e.binding = binding;
    e.visibility = WGPUShaderStage_Fragment;
    e.texture.sampleType = st;
    e.texture.viewDimension = WGPUTextureViewDimension_2D;
    return e;
}

static WGPUBindGroupLayoutEntry smp_entry(u32 binding, WGPUSamplerBindingType t) {
    WGPUBindGroupLayoutEntry e = { 0 };
    e.binding = binding;
    e.visibility = WGPUShaderStage_Fragment;
    e.sampler.type = t;
    return e;
}

static WGPUBindGroupLayoutEntry ubo_entry(u32 binding, WGPUShaderStageFlags vis, int dynamic, u64 min) {
    WGPUBindGroupLayoutEntry e = { 0 };
    e.binding = binding;
    e.visibility = vis;
    e.buffer.type = WGPUBufferBindingType_Uniform;
    e.buffer.hasDynamicOffset = dynamic;
    e.buffer.minBindingSize = min;
    return e;
}

static WGPUBindGroupLayout bgl(const WGPUBindGroupLayoutEntry *e, size_t n) {
    WGPUBindGroupLayoutDescriptor d = { 0 };
    d.entryCount = n;
    d.entries = e;
    return wgpuDeviceCreateBindGroupLayout(H.dev, &d);
}

static WGPUBindGroup group(WGPUBindGroupLayout l, const WGPUBindGroupEntry *e, size_t n) {
    WGPUBindGroupDescriptor d = { 0 };
    d.layout = l;
    d.entryCount = n;
    d.entries = e;
    return wgpuDeviceCreateBindGroup(H.dev, &d);
}

static WGPUBindGroupEntry tex_bind(u32 b, WGPUTextureView v) { WGPUBindGroupEntry e = { 0 }; e.binding = b; e.textureView = v; return e; }
static WGPUBindGroupEntry smp_bind(u32 b, WGPUSampler s) { WGPUBindGroupEntry e = { 0 }; e.binding = b; e.sampler = s; return e; }
static WGPUBindGroupEntry buf_bind(u32 b, WGPUBuffer buf, u64 size) { WGPUBindGroupEntry e = { 0 }; e.binding = b; e.buffer = buf; e.size = size; return e; }

static WGPUPipelineLayout playout(const WGPUBindGroupLayout *g, size_t n) {
    WGPUPipelineLayoutDescriptor d = { 0 };
    d.bindGroupLayoutCount = n;
    d.bindGroupLayouts = g;
    return wgpuDeviceCreatePipelineLayout(H.dev, &d);
}

static WGPUSampler sampler(WGPUFilterMode f) {
    WGPUSamplerDescriptor d = { 0 };
    d.addressModeU = d.addressModeV = d.addressModeW = WGPUAddressMode_ClampToEdge;
    d.magFilter = d.minFilter = f;
    d.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    d.lodMaxClamp = 32.0f;
    d.maxAnisotropy = 1;
    return wgpuDeviceCreateSampler(H.dev, &d);
}

/* one render pipeline: vertex (+ optional vertex buffer layout), fragment
 * with `ntargets` colour targets */
static WGPURenderPipeline pipeline(WGPUPipelineLayout layout, WGPUShaderModule vs,
                                   const WGPUVertexBufferLayout *vb, WGPUShaderModule fs,
                                   const WGPUColorTargetState *targets, size_t ntargets) {
    WGPUFragmentState fsd = { 0 };
    fsd.module = fs; fsd.entryPoint = "main";
    fsd.targetCount = ntargets; fsd.targets = targets;
    WGPURenderPipelineDescriptor d = { 0 };
    d.layout = layout;
    d.vertex.module = vs; d.vertex.entryPoint = "main";
    d.vertex.bufferCount = vb ? 1 : 0; d.vertex.buffers = vb;
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.primitive.frontFace = WGPUFrontFace_CCW;
    d.primitive.cullMode = WGPUCullMode_None;
    d.multisample.count = 1;
    d.multisample.mask = 0xFFFFFFFFu;
    d.fragment = &fsd;
    return wgpuDeviceCreateRenderPipeline(H.dev, &d);
}

/* --------------------------------------------------------------- setup */

static WGPUTextureFormat canvas_format = WGPUTextureFormat_BGRA8Unorm;
static WGPUBindGroupLayout fill_layout_g, psx_tex_layout, psx_push_layout, disp_smp_layout, psx_smp_layout;
static WGPUPipelineLayout fill_playout, psx_playout;
static WGPUBindGroup fill_group, psx_tex_group, psx_push_group, psx_smp_group, disp_push_group;
static WGPURenderPipeline fill_o_pipe, psx_pipe;
static WGPUBuffer psx_buf, blit_buf;
static u32 fill_cap;          /* fill uniform slots allocated */
static u8 *repack; static u32 repack_cap;

void hw_webgpu_set_canvas_format(int fmt) { canvas_format = (WGPUTextureFormat)fmt; }

typedef struct { float inv_size[2]; int scale; int pass; } prim_push_t;

static int init_pipelines(void) {
    WGPUShaderModule pvs = module(wgsl_hw_prim_vert), pfs = module(wgsl_hw_prim_frag);
    WGPUShaderModule fvs = module(wgsl_fullscreen_vs), ffs = module(wgsl_fill_fs), bfs = module(wgsl_blit_fs);

    /* ---- primitives: group 0 textures, group 1 push (dynamic), group 2 samplers */
    WGPUBindGroupLayoutEntry te[3] = {
        tex_entry(0, WGPUTextureSampleType_Float),
        tex_entry(1, WGPUTextureSampleType_UnfilterableFloat),
        tex_entry(2, WGPUTextureSampleType_UnfilterableFloat),
    };
    WGPUBindGroupLayoutEntry se[3] = {
        smp_entry(0, WGPUSamplerBindingType_NonFiltering),
        smp_entry(1, WGPUSamplerBindingType_NonFiltering),
        smp_entry(2, WGPUSamplerBindingType_NonFiltering),
    };
    WGPUBindGroupLayoutEntry pe = ubo_entry(0, WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, 1, sizeof(prim_push_t));
    H.tex_layout = bgl(te, 3);
    H.smp_layout = bgl(se, 3);
    H.push_layout = bgl(&pe, 1);
    WGPUBindGroupLayout gl[3] = { H.tex_layout, H.push_layout, H.smp_layout };
    H.layout = playout(gl, 3);

    WGPUBindGroupEntry tb[3] = { tex_bind(0, H.read_view), tex_bind(1, H.zsnap_view), tex_bind(2, H.order_snap_view) };
    WGPUBindGroupEntry sb[3] = { smp_bind(0, H.nearest), smp_bind(1, H.nearest), smp_bind(2, H.nearest) };
    H.set_read = group(H.tex_layout, tb, 3);
    H.smp_nearest = group(H.smp_layout, sb, 3);
    H.push_buf = buffer(3 * 256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry pb = buf_bind(0, H.push_buf, sizeof(prim_push_t));
    H.push_group = group(H.push_layout, &pb, 1);

    WGPUVertexAttribute va[4] = {
        { WGPUVertexFormat_Float32x4, 0,  0 },
        { WGPUVertexFormat_Float32x2, 16, 1 },
        { WGPUVertexFormat_Unorm8x4,  24, 2 },
        { WGPUVertexFormat_Uint32x4,  28, 3 },
    };
    WGPUVertexBufferLayout vb = { sizeof(hw_vtx_t), WGPUVertexStepMode_Vertex, 4, va };

    for (int k = 0; k < P_COUNT; k++) {
        WGPUBlendState bs;
        bs.alpha.operation = WGPUBlendOperation_Add;
        bs.alpha.srcFactor = WGPUBlendFactor_One;
        bs.alpha.dstFactor = WGPUBlendFactor_Zero;
        bs.color = bs.alpha;
        int blend = 1;
        switch (k) {
        case P_OPAQUE: case P_COPY: blend = 0; break;
        case P_BLEND0: bs.color.srcFactor = WGPUBlendFactor_Constant; bs.color.dstFactor = WGPUBlendFactor_Constant; break;
        case P_BLEND1: bs.color.srcFactor = WGPUBlendFactor_One; bs.color.dstFactor = WGPUBlendFactor_One; break;
        case P_BLEND2: bs.color.srcFactor = WGPUBlendFactor_One; bs.color.dstFactor = WGPUBlendFactor_One;
                       bs.color.operation = WGPUBlendOperation_ReverseSubtract; break;
        case P_BLEND3: bs.color.srcFactor = WGPUBlendFactor_Constant; bs.color.dstFactor = WGPUBlendFactor_One; break;
        case P_OPAQUE_CHECK: case P_COPY_CHECK:
            bs.color.srcFactor = bs.alpha.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
            bs.color.dstFactor = bs.alpha.dstFactor = WGPUBlendFactor_DstAlpha;
            break;
        }
        WGPUColorWriteMaskFlags aux = WGPUColorWriteMask_Red;
        if ((k >= P_BLEND0 && k <= P_BLEND3) || k == P_COPY || k == P_COPY_CHECK) aux = WGPUColorWriteMask_None;
        WGPUColorTargetState ct[3] = { { 0 }, { 0 }, { 0 } };
        ct[0].format = WGPUTextureFormat_RGBA8Unorm; ct[0].blend = blend ? &bs : NULL; ct[0].writeMask = WGPUColorWriteMask_All;
        ct[1].format = WGPUTextureFormat_R32Float;  ct[1].writeMask = aux;
        ct[2].format = WGPUTextureFormat_R32Float;  ct[2].writeMask = aux;
        H.pipe[k] = pipeline(H.layout, pvs, &vb, pfs, ct, 3);
    }

    /* ---- fills: group 0 = colour uniform (dynamic offset) */
    WGPUBindGroupLayoutEntry fe = ubo_entry(0, WGPUShaderStage_Fragment, 1, 16);
    fill_layout_g = bgl(&fe, 1);
    fill_playout = playout(&fill_layout_g, 1);
    WGPUColorWriteMaskFlags masks[3][3] = {
        { WGPUColorWriteMask_All, WGPUColorWriteMask_Red, WGPUColorWriteMask_Red },   /* OP_FILL   */
        { WGPUColorWriteMask_None, WGPUColorWriteMask_Red, WGPUColorWriteMask_Red },  /* OP_ZCLEAR */
        { WGPUColorWriteMask_None, WGPUColorWriteMask_None, WGPUColorWriteMask_Red }, /* order only */
    };
    for (int v = 0; v < 3; v++) {
        WGPUColorTargetState ct[3] = { { 0 }, { 0 }, { 0 } };
        ct[0].format = WGPUTextureFormat_RGBA8Unorm; ct[0].writeMask = masks[v][0];
        ct[1].format = WGPUTextureFormat_R32Float;  ct[1].writeMask = masks[v][1];
        ct[2].format = WGPUTextureFormat_R32Float;  ct[2].writeMask = masks[v][2];
        WGPURenderPipeline p = pipeline(fill_playout, fvs, NULL, ffs, ct, 3);
        if (v < 2) H.fill_pipe[v] = p; else fill_o_pipe = p;
    }

    /* ---- blit: upimg -> target / read at x scale */
    WGPUBindGroupLayoutEntry be[2] = {
        tex_entry(0, WGPUTextureSampleType_UnfilterableFloat),
        ubo_entry(1, WGPUShaderStage_Fragment, 0, 16),
    };
    H.blit_tex_layout = bgl(be, 2);
    H.blit_layout = playout(&H.blit_tex_layout, 1);
    blit_buf = buffer(256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    int sc[4] = { H.S, 0, 0, 0 };
    wgpuQueueWriteBuffer(H.queue, blit_buf, 0, sc, sizeof sc);
    WGPUBindGroupEntry bb[2] = { tex_bind(0, H.up_view), buf_bind(1, blit_buf, 16) };
    H.blit_group = group(H.blit_tex_layout, bb, 2);
    WGPUColorTargetState bt = { 0 };
    bt.format = WGPUTextureFormat_RGBA8Unorm; bt.writeMask = WGPUColorWriteMask_All;
    H.blit_target = pipeline(H.blit_layout, fvs, NULL, bfs, &bt, 1);
    H.blit_read = H.blit_target;

    wgpuShaderModuleRelease(pvs); wgpuShaderModuleRelease(pfs);
    wgpuShaderModuleRelease(ffs); wgpuShaderModuleRelease(bfs);
    wgpuShaderModuleRelease(fvs);
    return H.pipe[P_OPAQUE] && H.fill_pipe[0] && H.blit_target;
}

static int init_display(void) {
    WGPUShaderModule vs = module(wgsl_psx_vert), dfs = module(wgsl_hw_display_frag), pfs = module(wgsl_psx_frag);
    WGPUColorTargetState ct = { 0 };
    ct.format = canvas_format; ct.writeMask = WGPUColorWriteMask_All;

    /* HD picture: target (filterable) + linear sampler + push uniform */
    WGPUBindGroupLayoutEntry dt = tex_entry(0, WGPUTextureSampleType_Float);
    WGPUBindGroupLayoutEntry dp = ubo_entry(0, WGPUShaderStage_Fragment, 0, 48);
    WGPUBindGroupLayoutEntry ds = smp_entry(0, WGPUSamplerBindingType_Filtering);
    H.disp_tex_layout = bgl(&dt, 1);
    H.disp_push_layout = bgl(&dp, 1);
    disp_smp_layout = bgl(&ds, 1);
    WGPUBindGroupLayout g[3] = { H.disp_tex_layout, H.disp_push_layout, disp_smp_layout };
    H.disp_layout = playout(g, 3);
    H.disp_pipe = pipeline(H.disp_layout, vs, NULL, dfs, &ct, 1);
    H.disp_buf = buffer(256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry a = tex_bind(0, H.target_view), b = buf_bind(0, H.disp_buf, 48), c = smp_bind(0, H.linear);
    H.disp_tex = group(H.disp_tex_layout, &a, 1);
    disp_push_group = group(H.disp_push_layout, &b, 1);
    H.disp_smp = group(disp_smp_layout, &c, 1);

    /* original picture (and 24-bit movies): raw VRAM as r16uint */
    H.vram16 = texture(WGPUTextureFormat_R16Uint, VRAM_W, VRAM_H,
                       WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, &H.vram16_view);
    WGPUBindGroupLayoutEntry pt = tex_entry(0, WGPUTextureSampleType_Uint);
    WGPUBindGroupLayoutEntry pp = ubo_entry(0, WGPUShaderStage_Fragment, 0, 40);
    WGPUBindGroupLayoutEntry ps = smp_entry(0, WGPUSamplerBindingType_NonFiltering);
    psx_tex_layout = bgl(&pt, 1);
    psx_push_layout = bgl(&pp, 1);
    psx_smp_layout = bgl(&ps, 1);
    WGPUBindGroupLayout g2[3] = { psx_tex_layout, psx_push_layout, psx_smp_layout };
    psx_playout = playout(g2, 3);
    psx_pipe = pipeline(psx_playout, vs, NULL, pfs, &ct, 1);
    psx_buf = buffer(256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    WGPUBindGroupEntry d = tex_bind(0, H.vram16_view), e = buf_bind(0, psx_buf, 40), f = smp_bind(0, H.nearest);
    psx_tex_group = group(psx_tex_layout, &d, 1);
    psx_push_group = group(psx_push_layout, &e, 1);
    psx_smp_group = group(psx_smp_layout, &f, 1);

    wgpuShaderModuleRelease(vs); wgpuShaderModuleRelease(dfs); wgpuShaderModuleRelease(pfs);
    return H.disp_pipe && psx_pipe;
}

int hw_init_webgpu(WGPUDevice dev, const hw_config_t *cfg) {
    memset(&H, 0, sizeof H);
    (void)dummy_feature;
    H.dev = dev;
    H.queue = wgpuDeviceGetQueue(dev);
    wgpuDeviceSetUncapturedErrorCallback(dev, on_error, NULL);
    H.S = cfg && cfg->scale > 0 ? cfg->scale : 1;
    H.persp = cfg ? cfg->perspective : 1;
    if (H.S > HW_MAX_SCALE) H.S = HW_MAX_SCALE;
    WGPUSupportedLimits lim = { 0 };
    if (wgpuDeviceGetLimits(dev, &lim))
        while (H.S > 1 && (u32)(VRAM_W * H.S) > lim.limits.maxTextureDimension2D) H.S--;
    H.W = (u32)(VRAM_W * H.S);
    H.Hs = (u32)(VRAM_H * H.S);

    const WGPUTextureUsageFlags rt = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                                     WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    H.target     = texture(WGPUTextureFormat_RGBA8Unorm, H.W, H.Hs, rt, &H.target_view);
    H.read       = texture(WGPUTextureFormat_RGBA8Unorm, H.W, H.Hs, rt, &H.read_view);
    H.order      = texture(WGPUTextureFormat_R32Float, H.W, H.Hs, rt, &H.order_view);
    H.order_snap = texture(WGPUTextureFormat_R32Float, H.W, H.Hs,
                           WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, &H.order_snap_view);
    H.zimg       = texture(WGPUTextureFormat_R32Float, H.W, H.Hs, rt, &H.z_view);
    H.zsnap      = texture(WGPUTextureFormat_R32Float, H.W, H.Hs,
                           WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, &H.zsnap_view);
    H.upimg      = texture(WGPUTextureFormat_RGBA8Unorm, VRAM_W, VRAM_H,
                           WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, &H.up_view);
    H.nearest = sampler(WGPUFilterMode_Nearest);
    H.linear  = sampler(WGPUFilterMode_Linear);

    float inv[2] = { 1.0f / (float)H.W, 1.0f / (float)H.Hs };
    if (!init_pipelines() || !init_display()) {
        fprintf(stderr, "ac3: webgpu renderer setup failed\n");
        return 0;
    }
    for (int pass = 0; pass < 3; pass++) {
        prim_push_t pc = { { inv[0], inv[1] }, H.S, pass };
        wgpuQueueWriteBuffer(H.queue, H.push_buf, (u64)pass * 256, &pc, sizeof pc);
    }
    H.ready = 1;
    if (!hw_fx_init())
        printf("renderer: world effects could not be set up; showing the game's own world\n");
    printf("renderer: enhanced (WebGPU), %dx internal resolution (%ux%u VRAM)\n", H.S, H.W, H.Hs);
    return 1;
}

void hw_shutdown(void) {
    if (!H.ready) return;
    hw_fx_shutdown();
    WGPUTexture t[] = { H.target, H.read, H.zimg, H.zsnap, H.order, H.order_snap, H.upimg, H.vram16 };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i]) wgpuTextureDestroy(t[i]);
    WGPUBuffer b[] = { H.push_buf, H.fill_buf, H.disp_buf, H.vbuf, H.sbuf, psx_buf, blit_buf };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) if (b[i]) wgpuBufferDestroy(b[i]);
    free(H.verts); free(H.staging); free(H.cmds); free(repack);
    repack = NULL; repack_cap = 0; fill_cap = 0;
    memset(&H, 0, sizeof H);
}

/* ------------------------------------------------------------ recording */

typedef struct {
    WGPUCommandEncoder enc;
    WGPURenderPassEncoder rp;
    int pipe;          /* P_* of the primitive pipeline bound, -1 none, -2 other */
    VkRect2D sc;
} rec_t;

static void begin_main(rec_t *R) {
    if (R->rp) return;
    WGPURenderPassColorAttachment at[3] = { { 0 }, { 0 }, { 0 } };
    WGPUTextureView v[3] = { H.target_view, H.z_view, H.order_view };
    for (int i = 0; i < 3; i++) {
        at[i].view = v[i];
        at[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        at[i].loadOp = WGPULoadOp_Load;
        at[i].storeOp = WGPUStoreOp_Store;
    }
    WGPURenderPassDescriptor d = { 0 };
    d.colorAttachmentCount = 3;
    d.colorAttachments = at;
    R->rp = wgpuCommandEncoderBeginRenderPass(R->enc, &d);
    wgpuRenderPassEncoderSetViewport(R->rp, 0, 0, (float)H.W, (float)H.Hs, 0, 1);
    wgpuRenderPassEncoderSetVertexBuffer(R->rp, 0, H.vbuf, 0, H.vcap);
    R->pipe = -1;
    R->sc.extent.width = 0;
}

static void end_main(rec_t *R) {
    if (!R->rp) return;
    wgpuRenderPassEncoderEnd(R->rp);
    wgpuRenderPassEncoderRelease(R->rp);
    R->rp = NULL;
}

static void set_scissor(rec_t *R, s32 x, s32 y, u32 w, u32 h) {
    if (x < 0) { w = (u32)((s32)w + x); x = 0; }
    if (y < 0) { h = (u32)((s32)h + y); y = 0; }
    if ((u32)x >= H.W || (u32)y >= H.Hs) { w = 0; h = 0; x = 0; y = 0; }
    if ((u32)x + w > H.W) w = H.W - (u32)x;
    if ((u32)y + h > H.Hs) h = H.Hs - (u32)y;
    wgpuRenderPassEncoderSetScissorRect(R->rp, (u32)x, (u32)y, w, h);
    R->sc.offset.x = x; R->sc.offset.y = y; R->sc.extent.width = w; R->sc.extent.height = h;
}

static void bind_prim(rec_t *R, int pipe) {
    if (R->pipe < 0) {
        wgpuRenderPassEncoderSetBindGroup(R->rp, 0, H.set_read, 0, NULL);
        wgpuRenderPassEncoderSetBindGroup(R->rp, 2, H.smp_nearest, 0, NULL);
    }
    wgpuRenderPassEncoderSetPipeline(R->rp, H.pipe[pipe]);
    if (pipe == P_BLEND0) { WGPUColor c = { 0.5, 0.5, 0.5, 0.5 }; wgpuRenderPassEncoderSetBlendConstant(R->rp, &c); }
    if (pipe == P_BLEND3) { WGPUColor c = { 0.25, 0.25, 0.25, 0.25 }; wgpuRenderPassEncoderSetBlendConstant(R->rp, &c); }
    R->pipe = pipe;
}

static void fill(rec_t *R, WGPURenderPipeline p, u32 slot, s32 x, s32 y, s32 w, s32 h) {
    begin_main(R);
    wgpuRenderPassEncoderSetPipeline(R->rp, p);
    u32 off = slot * 256u;
    wgpuRenderPassEncoderSetBindGroup(R->rp, 0, fill_group, 1, &off);
    set_scissor(R, x * H.S, y * H.S, (u32)(w * H.S), (u32)(h * H.S));
    wgpuRenderPassEncoderDraw(R->rp, 3, 1, 0, 0);
    R->pipe = -2;
}

static void ensure_gpu_buffers(u32 nfills, u32 staging_bytes) {
    u64 vneed = ALIGN256((u64)H.nverts * sizeof(hw_vtx_t) + 4);
    if (vneed > H.vcap) {
        u64 cap = H.vcap ? H.vcap : (1u << 20);
        while (cap < vneed) cap *= 2;
        if (H.vbuf) wgpuBufferDestroy(H.vbuf);
        H.vbuf = buffer(cap, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
        H.vcap = cap;
    }
    u64 sneed = ALIGN256((u64)staging_bytes + 4);
    if (sneed > H.scap) {
        u64 cap = H.scap ? H.scap : (1u << 20);
        while (cap < sneed) cap *= 2;
        if (H.sbuf) wgpuBufferDestroy(H.sbuf);
        H.sbuf = buffer(cap, WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
        H.scap = cap;
    }
    if (nfills + 1 > fill_cap) {
        u32 cap = fill_cap ? fill_cap : 64;
        while (cap < nfills + 1) cap *= 2;
        if (H.fill_buf) wgpuBufferDestroy(H.fill_buf);
        H.fill_buf = buffer((u64)cap * 256, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        WGPUBindGroupEntry e = buf_bind(0, H.fill_buf, 16);
        fill_group = group(fill_layout_g, &e, 1);
        fill_cap = cap;
    }
}

static void copy_rect(WGPUCommandEncoder enc, WGPUTexture src, WGPUTexture dst, u32 x, u32 y, u32 w, u32 h) {
    WGPUImageCopyTexture a = { 0 }, b = { 0 };
    a.texture = src; a.origin.x = x; a.origin.y = y; a.aspect = WGPUTextureAspect_All;
    b.texture = dst; b.origin.x = x; b.origin.y = y; b.aspect = WGPUTextureAspect_All;
    WGPUExtent3D e = { w, h, 1 };
    wgpuCommandEncoderCopyTextureToTexture(enc, &a, &b, &e);
}

static WGPURenderPassEncoder begin_single(WGPUCommandEncoder enc, WGPUTextureView v) {
    WGPURenderPassColorAttachment at = { 0 };
    at.view = v;
    at.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    at.loadOp = WGPULoadOp_Load;
    at.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor d = { 0 };
    d.colorAttachmentCount = 1;
    d.colorAttachments = &at;
    return wgpuCommandEncoderBeginRenderPass(enc, &d);
}

/* uploads: staged VRAM pixels -> upimg (1x), then blitted at x scale into the
 * HD target and its read copy; followed by the ZCLEARs of the same run */
static u32 do_uploads(rec_t *R, u32 i, const u32 *pad_off) {
    end_main(R);
    u32 j = i;
    for (; j < H.ncmds && (H.cmds[j].op == OP_UPLOAD || H.cmds[j].op == OP_ZCLEAR); j++) {
        const cmd_t *c = &H.cmds[j];
        if (c->op != OP_UPLOAD) continue;
        WGPUImageCopyBuffer src = { 0 };
        src.buffer = H.sbuf;
        src.layout.offset = pad_off[j];
        src.layout.bytesPerRow = ALIGN256((u32)c->w * 4u);
        src.layout.rowsPerImage = (u32)c->h;
        WGPUImageCopyTexture dst = { 0 };
        dst.texture = H.upimg; dst.origin.x = (u32)c->x; dst.origin.y = (u32)c->y; dst.aspect = WGPUTextureAspect_All;
        WGPUExtent3D e = { (u32)c->w, (u32)c->h, 1 };
        wgpuCommandEncoderCopyBufferToTexture(R->enc, &src, &dst, &e);
    }
    WGPUTextureView views[2] = { H.target_view, H.read_view };
    for (int t = 0; t < 2; t++) {
        WGPURenderPassEncoder rp = begin_single(R->enc, views[t]);
        wgpuRenderPassEncoderSetPipeline(rp, H.blit_target);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, H.blit_group, 0, NULL);
        for (u32 k = i; k < j; k++) {
            const cmd_t *c = &H.cmds[k];
            if (c->op != OP_UPLOAD) continue;
            wgpuRenderPassEncoderSetScissorRect(rp, (u32)(c->x * H.S), (u32)(c->y * H.S),
                                                (u32)(c->w * H.S), (u32)(c->h * H.S));
            wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(rp);
        wgpuRenderPassEncoderRelease(rp);
    }
    return j;
}

void hw_record_webgpu(WGPUCommandEncoder enc) {
    if (!H.ready) return;

    /* staging rows repacked to 256-byte strides (WebGPU copy rule) */
    u32 nfills = 0, padded = 0;
    u32 *pad_off = (u32 *)malloc((size_t)(H.ncmds ? H.ncmds : 1) * sizeof(u32));
    for (u32 i = 0; i < H.ncmds; i++) {
        const cmd_t *c = &H.cmds[i];
        if (c->op == OP_FILL || c->op == OP_ZCLEAR || c->op == OP_FX_BEGIN) nfills++;
        if (c->op == OP_UPLOAD) { pad_off[i] = padded; padded += ALIGN256((u32)c->w * 4u) * (u32)c->h; }
    }
    ensure_gpu_buffers(nfills, padded);
    if (padded > repack_cap) { free(repack); repack = (u8 *)malloc(padded); repack_cap = repack ? padded : 0; }
    if (padded && repack) {
        for (u32 i = 0; i < H.ncmds; i++) {
            const cmd_t *c = &H.cmds[i];
            if (c->op != OP_UPLOAD) continue;
            u32 row = (u32)c->w * 4u, stride = ALIGN256(row);
            for (s32 y = 0; y < c->h; y++)
                memcpy(repack + pad_off[i] + (u32)y * stride, H.staging + c->staging + (u32)y * row, row);
        }
        wgpuQueueWriteBuffer(H.queue, H.sbuf, 0, repack, padded);
    }
    if (H.nverts)  /* sizes passed to writeBuffer must be multiples of 4: 44-byte vertices are */
        wgpuQueueWriteBuffer(H.queue, H.vbuf, 0, H.verts, (size_t)H.nverts * sizeof(hw_vtx_t));
    /* fill colours: slot 0 = zeros (z/order clears), then one slot per OP_FILL */
    {
        u32 slot = 1;
        static float zero[64];
        wgpuQueueWriteBuffer(H.queue, H.fill_buf, 0, zero, 16);
        for (u32 i = 0; i < H.ncmds; i++) {
            const cmd_t *c = &H.cmds[i];
            if (c->op != OP_FILL) continue;
            wgpuQueueWriteBuffer(H.queue, H.fill_buf, (u64)slot * 256, c->color, 16);
            slot++;
        }
    }

    hw_fx_prepare_webgpu(enc);

    rec_t R = { enc, NULL, -1, { { 0, 0 }, { 0, 0 } } };
    u32 fill_slot = 1;
    for (u32 i = 0; i < H.ncmds; ) {
        const cmd_t *c = &H.cmds[i];
        switch (c->op) {
        case OP_DRAW: {
            begin_main(&R);
            if (R.pipe != c->pipe) bind_prim(&R, c->pipe);
            if (R.sc.offset.x != c->sc.offset.x || R.sc.offset.y != c->sc.offset.y ||
                R.sc.extent.width != c->sc.extent.width || R.sc.extent.height != c->sc.extent.height)
                set_scissor(&R, c->sc.offset.x, c->sc.offset.y, c->sc.extent.width, c->sc.extent.height);
            u32 off = (u32)c->pass * 256u;
            wgpuRenderPassEncoderSetBindGroup(R.rp, 1, H.push_group, 1, &off);
            wgpuRenderPassEncoderDraw(R.rp, c->count, 1, c->first, 0);
            i++;
            break;
        }
        case OP_FILL:
            fill(&R, H.fill_pipe[0], fill_slot++, c->x, c->y, c->w, c->h);
            i++;
            break;
        case OP_ZCLEAR:
            fill(&R, H.fill_pipe[1], 0, c->x, c->y, c->w, c->h);
            i++;
            break;
        case OP_UPLOAD: {
            u32 j = do_uploads(&R, i, pad_off);
            for (u32 k = i; k < j; k++)
                if (H.cmds[k].op == OP_ZCLEAR)
                    fill(&R, H.fill_pipe[1], 0, H.cmds[k].x, H.cmds[k].y, H.cmds[k].w, H.cmds[k].h);
            i = j;
            break;
        }
        case OP_SYNC:
            end_main(&R);
            for (; i < H.ncmds && H.cmds[i].op == OP_SYNC; i++) {
                const cmd_t *s = &H.cmds[i];
                copy_rect(enc, H.target, H.read, (u32)(s->x * H.S), (u32)(s->y * H.S),
                          (u32)(s->w * H.S), (u32)(s->h * H.S));
            }
            break;
        case OP_FX_BEGIN:
        case OP_FX_END:
            if (c->op == OP_FX_BEGIN) {
                fill(&R, fill_o_pipe, 0, 0, 0, VRAM_W, VRAM_H);
                end_main(&R);
            } else {
                end_main(&R);
                copy_rect(enc, H.order, H.order_snap, 0, 0, H.W, H.Hs);
            }
            hw_fx_record_webgpu(enc, c);
            i++;
            break;
        default:
            i++;
            break;
        }
    }
    end_main(&R);
    free(pad_off);

    H.nverts = H.nstaging = H.ncmds = 0;
    hw_fx_frame_done();
    H.last_prims = H.prims;
    H.last_precise = H.precise;
    H.prims = H.precise = 0;
}

/* ------------------------------------------------------------- display */

typedef struct {
    float origin[2], size[2], view_pos[2], view_size[2], inv_vram[2];
    float half_texel, pad;
} disp_push_t;

void hw_display_webgpu(WGPURenderPassEncoder rp, const gpu_display_t *d,
                       float vx, float vy, float vw, float vh) {
    if (!H.ready || !d) return;
    disp_push_t pc = {
        { (float)d->x, (float)d->y }, { (float)d->w, (float)d->h },
        { vx, vy }, { vw, vh },
        { 1.0f / VRAM_W, 1.0f / VRAM_H },
        0.5f / (float)H.S, 0,
    };
    wgpuQueueWriteBuffer(H.queue, H.disp_buf, 0, &pc, sizeof pc);
    wgpuRenderPassEncoderSetViewport(rp, vx, vy, vw, vh, 0, 1);
    wgpuRenderPassEncoderSetScissorRect(rp, (u32)vx, (u32)vy, (u32)vw, (u32)vh);
    wgpuRenderPassEncoderSetPipeline(rp, H.disp_pipe);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, H.disp_tex, 0, NULL);
    wgpuRenderPassEncoderSetBindGroup(rp, 1, disp_push_group, 0, NULL);
    wgpuRenderPassEncoderSetBindGroup(rp, 2, H.disp_smp, 0, NULL);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
}

typedef struct {
    int origin[2], size[2];
    float view_pos[2], view_size[2];
    u32 rgb24, pad;
} psx_push_t;

/* the original picture straight from VRAM: used for 24-bit movies */
void hw_display_vram_webgpu(WGPURenderPassEncoder rp, const gpu_display_t *d, const u16 *vram,
                            float vx, float vy, float vw, float vh) {
    if (!H.ready || !d || !vram) return;
    WGPUImageCopyTexture dst = { 0 };
    dst.texture = H.vram16; dst.aspect = WGPUTextureAspect_All;
    WGPUTextureDataLayout lay = { 0 };
    lay.bytesPerRow = VRAM_W * 2; lay.rowsPerImage = VRAM_H;
    WGPUExtent3D e = { VRAM_W, VRAM_H, 1 };
    wgpuQueueWriteTexture(H.queue, &dst, vram, VRAM_W * VRAM_H * 2, &lay, &e);
    psx_push_t pc = { { d->x, d->y }, { d->w, d->h }, { vx, vy }, { vw, vh }, d->rgb24 ? 1u : 0u, 0 };
    wgpuQueueWriteBuffer(H.queue, psx_buf, 0, &pc, sizeof pc);
    wgpuRenderPassEncoderSetViewport(rp, vx, vy, vw, vh, 0, 1);
    wgpuRenderPassEncoderSetScissorRect(rp, (u32)vx, (u32)vy, (u32)vw, (u32)vh);
    wgpuRenderPassEncoderSetPipeline(rp, psx_pipe);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, psx_tex_group, 0, NULL);
    wgpuRenderPassEncoderSetBindGroup(rp, 1, psx_push_group, 0, NULL);
    wgpuRenderPassEncoderSetBindGroup(rp, 2, psx_smp_group, 0, NULL);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
}
