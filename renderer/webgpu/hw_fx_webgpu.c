/*
 * hw_fx_webgpu.c -- the GPU half of the world effects, in the render worker.
 *
 * WebGPU version of renderer/hw/hw_fx.c's GPU work: the same images, passes
 * and shaders (converted to WGSL by tools/shaders_to_wgsl.py). The per-field
 * inputs (scene uniforms, far-terrain tiles, map) are computed in the game
 * worker by hw_fx_game.c and arrive in the packet (hw_fx_web.h).
 *
 * Differences from Vulkan, all mechanical:
 *  - one descriptor set with 19 images -> a bind group per pipeline holding
 *    only what its shaders use, because WebGPU forbids a bound texture that is
 *    also a render target of the same pass, even when the shader ignores it
 *  - combined image samplers -> texture in group 0, sampler in group 2
 *    (same binding number); push constants -> uniform buffer in group 1
 *  - render-area clears / vkCmdClearAttachments -> a full-attachment clear
 *    where only the effects use the image (depth), and a small scissored
 *    pipeline for the depth-ordering image the game's polygons share
 *  - 32-bit float images are only read with nearest sampling here, so they are
 *    declared unfilterable (no optional "float32-filterable" feature needed)
 *  - samplers get Vulkan's defaults explicitly: no mipmaps unless "mip"
 * The ocean's simulation is in hw_ocean_webgpu.c; it is drawn here (record_begin).
 */
#include "hw_fx_web.h"
#include "wgsl_shaders.h"
#include "hw_ocean_webgpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS hw_state

#define NOISE_N      64
#define NOISE_MIPS   7
#define WEATHER_N    256
#define WEATHER_MIPS 9
#define NBIND        20
#define PUSH_SLOTS   (2 + FX_MAX_SCENES * 2 * FX_BLOOM_MIPS)

typedef struct { WGPUTexture tex; WGPUTextureView view; } img_t;

enum { P_BG, P_TILE, P_PRIME, P_TREE, P_CLOUD, P_CRES, P_POST, P_SUNVIS, P_COMP, P_ZCLEAR, P_OCEAN, P_BDOWN, P_BUP, NPIPE };

typedef struct { float src_texel[2], uv_max[2], radius, first, pad[2]; } bloom_push_t;

static struct {
    int ready, cleared;
    u32 CW, CH, HDW, HDH;
    img_t depth, cloud, cloudz, cloud_raw, cloud_hist, grid, height, colors, top, noise, weather;
    img_t hdr, sunvis, odisp, oderiv;
    WGPUTexture bloom; WGPUTextureView bloom_view[FX_BLOOM_MIPS];
    WGPUSampler s_near, s_lin, s_rep, s_mip;
    WGPUBuffer ubo, push, inst, mesh;
    WGPUBindGroupLayout empty_l;
    WGPUBindGroup empty_g;
    WGPURenderPipeline pipe[NPIPE];
    WGPUBindGroup g0[NPIPE], g1[NPIPE], g2[NPIPE];
    WGPUBindGroup bloom_src[FX_BLOOM_MIPS + 1];   /* hdr, bloom mip 0..5 */
    u32 push_next;
    /* this field, from the packet */
    fx_pkt_t h;
    tile_draw_t draws[2][FX_MAX_DRAWS];
    u32 inst_base[2];
} F;

/* --------------------------------------------------------------- helpers */

static WGPUShaderModule module(const char *code) {
    WGPUShaderModuleWGSLDescriptor w = { 0 };
    w.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    w.code = code;
    WGPUShaderModuleDescriptor d = { 0 };
    d.nextInChain = &w.chain;
    return wgpuDeviceCreateShaderModule(HS.dev, &d);
}

static img_t image(WGPUTextureDimension dim, WGPUTextureFormat fmt, u32 w, u32 h, u32 d, u32 mips,
                   WGPUTextureUsageFlags usage) {
    WGPUTextureDescriptor td = { 0 };
    td.usage = usage;
    td.dimension = dim;
    td.size.width = w; td.size.height = h; td.size.depthOrArrayLayers = d;
    td.format = fmt;
    td.mipLevelCount = mips;
    td.sampleCount = 1;
    img_t o;
    o.tex = wgpuDeviceCreateTexture(HS.dev, &td);
    o.view = wgpuTextureCreateView(o.tex, NULL);
    return o;
}

static WGPUSampler sampler(WGPUFilterMode f, WGPUAddressMode a, int mips) {
    WGPUSamplerDescriptor d = { 0 };
    d.addressModeU = d.addressModeV = d.addressModeW = a;
    d.magFilter = d.minFilter = f;
    d.mipmapFilter = mips ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    d.lodMinClamp = 0.0f;
    d.lodMaxClamp = mips ? 32.0f : 0.0f;      /* Vulkan's maxLod 0 unless "mip" */
    d.maxAnisotropy = 1;
    return wgpuDeviceCreateSampler(HS.dev, &d);
}

static WGPUBuffer buffer(u64 size, WGPUBufferUsageFlags usage) {
    WGPUBufferDescriptor d = { 0 };
    d.usage = usage;
    d.size = size;
    return wgpuDeviceCreateBuffer(HS.dev, &d);
}

/* ------------------------------------------------- bindings (hw_fx.c's set) */

typedef struct {
    WGPUTextureView view;
    WGPUTextureSampleType st;
    WGPUTextureViewDimension dim;
    WGPUSampler smp;
    WGPUSamplerBindingType sbt;
} bdesc_t;
static bdesc_t BD[NBIND];

static void binding_table(void) {
    const WGPUTextureSampleType FL = WGPUTextureSampleType_Float, UF = WGPUTextureSampleType_UnfilterableFloat,
                                UI = WGPUTextureSampleType_Uint;
    const WGPUTextureViewDimension D2 = WGPUTextureViewDimension_2D, D3 = WGPUTextureViewDimension_3D,
                                   DA = WGPUTextureViewDimension_2DArray;
    const WGPUSamplerBindingType SF = WGPUSamplerBindingType_Filtering, SN = WGPUSamplerBindingType_NonFiltering;
    bdesc_t t[NBIND] = {
        { 0 },
        { HS.read_view,    FL, D2, F.s_near, SF },   /* 1  game picture (copy) */
        { HS.z_view,       UF, D2, F.s_near, SN },   /* 2  depth of the game's polygons */
        { F.cloud.view,    FL, D2, F.s_near, SF },   /* 3 */
        { F.cloudz.view,   UF, D2, F.s_near, SN },   /* 4 */
        { F.grid.view,     UI, D2, F.s_near, SN },   /* 5 */
        { F.height.view,   UF, D2, F.s_near, SN },   /* 6 */
        { F.colors.view,   FL, D2, F.s_near, SF },   /* 7 */
        { F.top.view,      UF, D2, F.s_near, SN },   /* 8 */
        { F.noise.view,    FL, D3, F.s_rep,  SF },   /* 9 */
        { F.weather.view,  FL, D2, F.s_rep,  SF },   /* 10 */
        { F.hdr.view,      FL, D2, F.s_near, SF },   /* 11 */
        { F.bloom_view[0], FL, D2, F.s_lin,  SF },   /* 12 */
        { F.sunvis.view,   FL, D2, F.s_near, SF },   /* 13 */
        { F.odisp.view,    FL, DA, F.s_lin,  SF },   /* 14 ocean displacement */
        { F.oderiv.view,   FL, DA, F.s_lin,  SF },   /* 15 ocean slopes + foam */
        { F.cloud_raw.view, FL, D2, F.s_near, SF },  /* 16 */
        { F.cloud_hist.view, FL, D2, F.s_lin, SF },  /* 17 */
        { F.noise.view,    FL, D3, F.s_mip,  SF },   /* 18 */
        { F.weather.view,  FL, D2, F.s_mip,  SF },   /* 19 */
    };
    memcpy(BD, t, sizeof t);
}

/* bind group layouts + groups for one pipeline: `mask` = bindings its shaders use */
static WGPUPipelineLayout groups_for(int p, u32 mask, int push) {
    WGPUBindGroupLayoutEntry le[NBIND], ls[NBIND];
    WGPUBindGroupEntry ge[NBIND], gs[NBIND];
    int nt = 0, ns = 0;
    const WGPUShaderStageFlags VF = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    if (mask & 1u) {
        WGPUBindGroupLayoutEntry e = { 0 };
        e.binding = 0; e.visibility = VF;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.hasDynamicOffset = 1;
        e.buffer.minBindingSize = sizeof(fx_ubo_t);
        le[nt] = e;
        WGPUBindGroupEntry g = { 0 };
        g.binding = 0; g.buffer = F.ubo; g.size = sizeof(fx_ubo_t);
        ge[nt++] = g;
    }
    for (u32 b = 1; b < NBIND; b++) {
        if (!(mask & (1u << b))) continue;
        WGPUBindGroupLayoutEntry e = { 0 };
        e.binding = b; e.visibility = VF;
        e.texture.sampleType = BD[b].st;
        e.texture.viewDimension = BD[b].dim;
        le[nt] = e;
        WGPUBindGroupEntry g = { 0 };
        g.binding = b; g.textureView = BD[b].view;
        ge[nt++] = g;
        WGPUBindGroupLayoutEntry s = { 0 };
        s.binding = b; s.visibility = VF;
        s.sampler.type = BD[b].sbt;
        ls[ns] = s;
        WGPUBindGroupEntry gsm = { 0 };
        gsm.binding = b; gsm.sampler = BD[b].smp;
        gs[ns++] = gsm;
    }
    WGPUBindGroupLayoutDescriptor d0 = { 0 }, d2 = { 0 };
    d0.entryCount = (size_t)nt; d0.entries = le;
    d2.entryCount = (size_t)ns; d2.entries = ls;
    WGPUBindGroupLayout l0 = wgpuDeviceCreateBindGroupLayout(HS.dev, &d0);
    WGPUBindGroupLayout l2 = wgpuDeviceCreateBindGroupLayout(HS.dev, &d2);
    WGPUBindGroupDescriptor b0 = { 0 }, b2 = { 0 };
    b0.layout = l0; b0.entryCount = (size_t)nt; b0.entries = ge;
    b2.layout = l2; b2.entryCount = (size_t)ns; b2.entries = gs;
    F.g0[p] = wgpuDeviceCreateBindGroup(HS.dev, &b0);
    F.g2[p] = wgpuDeviceCreateBindGroup(HS.dev, &b2);
    WGPUBindGroupLayout l1 = F.empty_l;
    F.g1[p] = F.empty_g;
    if (push) {
        WGPUBindGroupLayoutEntry e = { 0 };
        e.binding = 0; e.visibility = VF;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.hasDynamicOffset = 1;
        e.buffer.minBindingSize = 16;
        WGPUBindGroupLayoutDescriptor d1 = { 0 };
        d1.entryCount = 1; d1.entries = &e;
        l1 = wgpuDeviceCreateBindGroupLayout(HS.dev, &d1);
        WGPUBindGroupEntry g = { 0 };
        g.binding = 0; g.buffer = F.push; g.size = 32;
        WGPUBindGroupDescriptor b1 = { 0 };
        b1.layout = l1; b1.entryCount = 1; b1.entries = &g;
        F.g1[p] = wgpuDeviceCreateBindGroup(HS.dev, &b1);
    }
    WGPUBindGroupLayout all[3] = { l0, l1, l2 };
    WGPUPipelineLayoutDescriptor pd = { 0 };
    pd.bindGroupLayoutCount = 3; pd.bindGroupLayouts = all;
    return wgpuDeviceCreatePipelineLayout(HS.dev, &pd);
}

typedef struct {
    const char *vs, *fs;
    u32 mask;                 /* bindings used */
    int push;
    int ncolor;
    WGPUTextureFormat fmt[2];
    WGPUColorWriteMaskFlags wm[2];
    int depth;                /* 0 none, 1 test GREATER + write, 2 ALWAYS + write, 3 ALWAYS no write */
    int additive;
    const WGPUVertexBufferLayout *vb; size_t nvb;
} pdesc_t;

static void make_pipe(int p, const pdesc_t *d) {
    WGPUPipelineLayout layout = groups_for(p, d->mask, d->push);
    WGPUShaderModule vs = module(d->vs), fs = module(d->fs);
    WGPUBlendState add;
    add.color.operation = add.alpha.operation = WGPUBlendOperation_Add;
    add.color.srcFactor = add.color.dstFactor = WGPUBlendFactor_One;
    add.alpha = add.color;
    WGPUColorTargetState ct[2] = { { 0 }, { 0 } };
    for (int i = 0; i < d->ncolor; i++) {
        ct[i].format = d->fmt[i];
        ct[i].writeMask = d->wm[i];
        ct[i].blend = d->additive ? &add : NULL;
    }
    WGPUFragmentState fsd = { 0 };
    fsd.module = fs; fsd.entryPoint = "main";
    fsd.targetCount = (size_t)d->ncolor; fsd.targets = ct;
    WGPUDepthStencilState ds = { 0 };
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = d->depth == 1 || d->depth == 2;
    ds.depthCompare = d->depth == 1 ? WGPUCompareFunction_Greater : WGPUCompareFunction_Always;
    ds.stencilFront.compare = ds.stencilBack.compare = WGPUCompareFunction_Always;
    ds.stencilFront.failOp = ds.stencilBack.failOp = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = ds.stencilBack.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp = ds.stencilBack.passOp = WGPUStencilOperation_Keep;
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.layout = layout;
    pd.vertex.module = vs; pd.vertex.entryPoint = "main";
    pd.vertex.bufferCount = d->nvb; pd.vertex.buffers = d->vb;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.depthStencil = d->depth ? &ds : NULL;
    pd.fragment = &fsd;
    F.pipe[p] = wgpuDeviceCreateRenderPipeline(HS.dev, &pd);
    wgpuShaderModuleRelease(vs);
    wgpuShaderModuleRelease(fs);
}

/* --------------------------------------------- cloud noise (as in hw_fx.c) */

static u32 nhash(u32 x, u32 y, u32 z, u32 seed) {
    u32 h = x * 374761393u + y * 668265263u + z * 2246822519u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float grad3(u32 h, float x, float y, float z) {
    switch (h % 12u) {
    case 0: return x + y;   case 1: return -x + y;  case 2: return x - y;   case 3: return -x - y;
    case 4: return x + z;   case 5: return -x + z;  case 6: return x - z;   case 7: return -x - z;
    case 8: return y + z;   case 9: return -y + z;  case 10: return y - z;  default: return -y - z;
    }
}
static float fade5(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static float perlin3(float x, float y, float z, int period, u32 seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    float u = fade5(xf), v = fade5(yf), w = fade5(zf);
    float n[8];
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        u32 h = nhash((u32)(((xi + dx) % period + period) % period),
                      (u32)(((yi + dy) % period + period) % period),
                      (u32)(((zi + dz) % period + period) % period), seed);
        n[k] = grad3(h, xf - dx, yf - dy, zf - dz);
    }
    float x00 = n[0] + u * (n[1] - n[0]), x10 = n[2] + u * (n[3] - n[2]);
    float x01 = n[4] + u * (n[5] - n[4]), x11 = n[6] + u * (n[7] - n[6]);
    float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);
}
static float worley3(float x, float y, float z, int period, u32 seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                u32 h = nhash((u32)((cx % period + period) % period),
                              (u32)((cy % period + period) % period),
                              (u32)((cz % period + period) % period), seed);
                float px = cx + (float)(h & 1023u) / 1023.0f;
                float py = cy + (float)((h >> 10) & 1023u) / 1023.0f;
                float pz = cz + (float)((h >> 20) & 1023u) / 1023.0f;
                float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                if (d < best) best = d;
            }
    float d = sqrtf(best);
    return d > 1.0f ? 1.0f : d;
}
static u8 unorm8(float v) { if (v < 0) v = 0; if (v > 1) v = 1; return (u8)(v * 255.0f + 0.5f); }
static void make_noise(u8 *vol, u8 *weather) {
    for (int z = 0; z < NOISE_N; z++)
        for (int y = 0; y < NOISE_N; y++)
            for (int x = 0; x < NOISE_N; x++) {
                float fx = (float)x / NOISE_N, fy = (float)y / NOISE_N, fz = (float)z / NOISE_N;
                float p = 0, amp = 1, norm = 0;
                for (int o = 0; o < 4; o++) {
                    int f = 4 << o;
                    p += perlin3(fx * f, fy * f, fz * f, f, 11u + (u32)o) * amp;
                    norm += amp;
                    amp *= 0.5f;
                }
                p = p / norm * 0.8f + 0.5f;
                float w4 = 1.0f - worley3(fx * 4, fy * 4, fz * 4, 4, 21u);
                float w8 = 1.0f - worley3(fx * 8, fy * 8, fz * 8, 8, 22u);
                float w16 = 1.0f - worley3(fx * 16, fy * 16, fz * 16, 16, 23u);
                float w32 = 1.0f - worley3(fx * 32, fy * 32, fz * 32, 32, 24u);
                float wf = w4 * 0.625f + w8 * 0.25f + w16 * 0.125f;
                float pw = (p - (wf - 1.0f)) / (2.0f - wf);
                u8 *o = vol + (((size_t)z * NOISE_N + y) * NOISE_N + x) * 4;
                o[0] = unorm8(pw); o[1] = unorm8(w8); o[2] = unorm8(w16); o[3] = unorm8(w32);
            }
    for (int y = 0; y < WEATHER_N; y++)
        for (int x = 0; x < WEATHER_N; x++) {
            float fx = (float)x / WEATHER_N, fy = (float)y / WEATHER_N;
            float p = 0, amp = 1, norm = 0;
            for (int o = 0; o < 5; o++) {
                int f = 3 << o;
                p += perlin3(fx * f, fy * f, 0.37f, f, 31u + (u32)o) * amp;
                norm += amp;
                amp *= 0.55f;
            }
            p = p / norm;
            u8 *o = weather + ((size_t)y * WEATHER_N + x) * 4;
            o[0] = unorm8(p * 1.6f + 0.5f); o[1] = o[2] = 0; o[3] = 255;
        }
}
/* box-filtered mip chain, uploaded level by level (hw_fx.c: make_mips + upload_mips) */
static void upload_mips(WGPUTexture tex, const u8 *src, u32 n, int is3d, u32 levels) {
    u8 *prev = (u8 *)malloc((size_t)n * n * (is3d ? n : 1) * 4);
    memcpy(prev, src, (size_t)n * n * (is3d ? n : 1) * 4);
    u32 pn = n;
    for (u32 l = 0; l < levels; l++) {
        u32 m = n >> l, md = is3d ? m : 1;
        if (l) {
            u8 *cur = (u8 *)malloc((size_t)m * m * md * 4);
            u32 kz = is3d ? 2u : 1u;
            for (u32 z = 0; z < md; z++)
                for (u32 y = 0; y < m; y++)
                    for (u32 x = 0; x < m; x++)
                        for (u32 c = 0; c < 4; c++) {
                            u32 sum = 0;
                            for (u32 dz = 0; dz < kz; dz++)
                                for (u32 dy = 0; dy < 2; dy++)
                                    for (u32 dx = 0; dx < 2; dx++)
                                        sum += prev[((((size_t)(2 * z + dz) * pn) + 2 * y + dy) * pn + 2 * x + dx) * 4 + c];
                            u32 cnt = 4 * kz;
                            cur[(((size_t)z * m + y) * m + x) * 4 + c] = (u8)((sum + cnt / 2) / cnt);
                        }
            free(prev);
            prev = cur;
            pn = m;
        }
        WGPUImageCopyTexture dst = { 0 };
        dst.texture = tex; dst.mipLevel = l; dst.aspect = WGPUTextureAspect_All;
        WGPUTextureDataLayout lay = { 0 };
        lay.bytesPerRow = m * 4; lay.rowsPerImage = m;
        WGPUExtent3D e = { m, m, md };
        wgpuQueueWriteTexture(HS.queue, &dst, prev, (size_t)m * m * md * 4, &lay, &e);
    }
    free(prev);
}

static void write2d(WGPUTexture tex, const void *data, u32 w, u32 h, u32 bpp) {
    WGPUImageCopyTexture dst = { 0 };
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTextureDataLayout lay = { 0 };
    lay.bytesPerRow = w * bpp; lay.rowsPerImage = h;
    WGPUExtent3D e = { w, h, 1 };
    wgpuQueueWriteTexture(HS.queue, &dst, data, (size_t)w * h * bpp, &lay, &e);
}

/* ------------------------------------------------------------------ init */

/* WGSL for the depth-ordering clear in the world pass (hw_fx.c: vkCmdClearAttachments) */
static const char wgsl_zclear_fs[] =
    "struct Out { @location(0) c: vec4<f32>, @location(1) z: f32 };\n"
    "@fragment fn main() -> Out { return Out(vec4<f32>(0.0), 1.0e9); }\n";

int hw_fx_init(void) {
    memset(&F, 0, sizeof F);
    F.CW = HS.W / 2; F.CH = HS.Hs / 2;
    F.HDW = 512u * (u32)HS.S; F.HDH = 256u * (u32)HS.S;
    const WGPUTextureUsageFlags SM = WGPUTextureUsage_TextureBinding, CD = WGPUTextureUsage_CopyDst,
                                CS = WGPUTextureUsage_CopySrc, RA = WGPUTextureUsage_RenderAttachment;
    const WGPUTextureDimension T2 = WGPUTextureDimension_2D;
    F.depth      = image(T2, WGPUTextureFormat_Depth32Float, HS.W, HS.Hs, 1, 1, RA);
    F.cloud      = image(T2, WGPUTextureFormat_RGBA16Float, F.CW, F.CH, 1, 1, RA | SM | CS);
    F.cloud_raw  = image(T2, WGPUTextureFormat_RGBA16Float, F.CW, F.CH, 1, 1, RA | SM);
    F.cloud_hist = image(T2, WGPUTextureFormat_RGBA16Float, F.CW, F.CH, 1, 1, RA | SM | CD);
    F.cloudz     = image(T2, WGPUTextureFormat_RG32Float, F.CW, F.CH, 1, 1, RA | SM);
    F.grid       = image(T2, WGPUTextureFormat_R16Uint, HW_MAP_N, HW_MAP_N, 1, 1, SM | CD);
    F.height     = image(T2, WGPUTextureFormat_R32Float, HW_ATLAS_W, HW_ATLAS_H, 1, 1, SM | CD);
    F.colors     = image(T2, WGPUTextureFormat_RGBA8Unorm, HW_ATLAS_W, HW_ATLAS_H, 1, 1, SM | CD);
    F.top        = image(T2, WGPUTextureFormat_R32Float, 512, 1, 1, 1, SM | CD);
    F.noise      = image(WGPUTextureDimension_3D, WGPUTextureFormat_RGBA8Unorm, NOISE_N, NOISE_N, NOISE_N,
                         NOISE_MIPS, SM | CD);
    F.weather    = image(T2, WGPUTextureFormat_RGBA8Unorm, WEATHER_N, WEATHER_N, 1, WEATHER_MIPS, SM | CD);
    F.hdr        = image(T2, WGPUTextureFormat_RGBA16Float, F.HDW, F.HDH, 1, 1, RA | SM);
    F.sunvis     = image(T2, WGPUTextureFormat_R16Float, 1, 1, 1, 1, RA | SM);
    F.odisp      = image(T2, WGPUTextureFormat_RGBA16Float, 1, 1, 1, 1, SM);
    F.oderiv     = image(T2, WGPUTextureFormat_RGBA16Float, 1, 1, 1, 1, SM);
    {   /* the ocean shaders read 2D arrays */
        WGPUTextureViewDescriptor vd = { 0 };
        vd.format = WGPUTextureFormat_RGBA16Float;
        vd.dimension = WGPUTextureViewDimension_2DArray;
        vd.mipLevelCount = 1; vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        F.odisp.view = wgpuTextureCreateView(F.odisp.tex, &vd);
        F.oderiv.view = wgpuTextureCreateView(F.oderiv.tex, &vd);
    }
    {
        WGPUTextureDescriptor td = { 0 };
        td.usage = RA | SM;
        td.dimension = T2;
        td.size.width = F.HDW / 2; td.size.height = F.HDH / 2; td.size.depthOrArrayLayers = 1;
        td.format = WGPUTextureFormat_RGBA16Float;
        td.mipLevelCount = FX_BLOOM_MIPS;
        td.sampleCount = 1;
        F.bloom = wgpuDeviceCreateTexture(HS.dev, &td);
        for (int m = 0; m < FX_BLOOM_MIPS; m++) {
            WGPUTextureViewDescriptor vd = { 0 };
            vd.format = td.format;
            vd.dimension = WGPUTextureViewDimension_2D;
            vd.baseMipLevel = (u32)m; vd.mipLevelCount = 1;
            vd.arrayLayerCount = 1;
            vd.aspect = WGPUTextureAspect_All;
            F.bloom_view[m] = wgpuTextureCreateView(F.bloom, &vd);
        }
    }
    F.s_near = sampler(WGPUFilterMode_Nearest, WGPUAddressMode_ClampToEdge, 0);
    F.s_lin  = sampler(WGPUFilterMode_Linear, WGPUAddressMode_ClampToEdge, 0);
    F.s_rep  = sampler(WGPUFilterMode_Linear, WGPUAddressMode_Repeat, 0);
    F.s_mip  = sampler(WGPUFilterMode_Linear, WGPUAddressMode_Repeat, 1);
    F.ubo  = buffer((u64)FX_UBO_STRIDE * FX_MAX_SCENES, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    F.push = buffer((u64)256 * PUSH_SLOTS, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    F.inst = buffer((u64)FX_MAX_INST * 2 * 8, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
    F.mesh = buffer((u64)FX_MESH_CAP * sizeof(hw_tile_vtx_t), WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);
    {
        WGPUBindGroupLayoutDescriptor d = { 0 };
        F.empty_l = wgpuDeviceCreateBindGroupLayout(HS.dev, &d);
        WGPUBindGroupDescriptor g = { 0 };
        g.layout = F.empty_l;
        F.empty_g = wgpuDeviceCreateBindGroup(HS.dev, &g);
    }
    binding_table();
    if (hw_ocean_webgpu_init()) {     /* the real wave textures instead of the 1x1 placeholders */
        BD[14].view = hw_ocean_webgpu_disp();  BD[14].smp = hw_ocean_webgpu_sampler();
        BD[15].view = hw_ocean_webgpu_deriv(); BD[15].smp = hw_ocean_webgpu_sampler();
    }

    /* tree rings (hw_fx.c record_end): push slots 0 and 1 */
    float rings[2][4] = { { 44.0f, 292.0f, 0.0f, 6300.0f }, { 88.0f, 296.0f, 6300.0f, 12800.0f } };
    wgpuQueueWriteBuffer(HS.queue, F.push, 0, rings[0], 16);
    wgpuQueueWriteBuffer(HS.queue, F.push, 256, rings[1], 16);

    const WGPUTextureFormat RGBA8 = WGPUTextureFormat_RGBA8Unorm, R32 = WGPUTextureFormat_R32Float,
                            F16 = WGPUTextureFormat_RGBA16Float;
    const WGPUColorWriteMaskFlags RGB = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue,
                                  RGBA = WGPUColorWriteMask_All, R = WGPUColorWriteMask_Red,
                                  RG = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green;
#define B(n) (1u << (n))
    WGPUVertexAttribute ta[5] = {
        { WGPUVertexFormat_Float32x3, 0,  0 },
        { WGPUVertexFormat_Float32x2, 12, 1 },
        { WGPUVertexFormat_Unorm8x4,  20, 2 },
        { WGPUVertexFormat_Uint32,    24, 3 },
        { WGPUVertexFormat_Unorm8x4,  28, 4 },
    };
    WGPUVertexAttribute ia = { WGPUVertexFormat_Float32x2, 0, 5 };
    WGPUVertexBufferLayout tvb[2] = {
        { sizeof(hw_tile_vtx_t), WGPUVertexStepMode_Vertex, 5, ta },
        { 8, WGPUVertexStepMode_Instance, 1, &ia },
    };
    WGPUVertexAttribute oa = { WGPUVertexFormat_Float32x4, 0, 0 };
    WGPUVertexBufferLayout ovb = { sizeof(hw_ocean_node_t), WGPUVertexStepMode_Instance, 1, &oa };
    pdesc_t d[NPIPE] = {
        [P_BG]     = { wgsl_psx_vert, wgsl_hw_bg_frag, B(0) | B(5) | B(6) | B(7) | B(8), 0, 2, { RGBA8, R32 }, { RGB, R }, 1 },
        [P_TILE]   = { wgsl_hw_tile_vert, wgsl_hw_tile_frag, B(0) | B(1), 0, 2, { RGBA8, R32 }, { RGB, R }, 1, 0, tvb, 2 },
        [P_PRIME]  = { wgsl_psx_vert, wgsl_hw_prime_frag, B(0) | B(2), 0, 0, { 0 }, { 0 }, 2 },
        [P_TREE]   = { wgsl_hw_tree_vert, wgsl_hw_tree_frag, B(0) | B(5) | B(6) | B(7) | B(8), 1, 1, { RGBA8 }, { RGB }, 1 },
        [P_CLOUD]  = { wgsl_psx_vert, wgsl_hw_cloud_frag, B(0) | B(2) | B(18) | B(19), 0, 2,
                       { F16, WGPUTextureFormat_RG32Float }, { RGBA, RG } },
        [P_CRES]   = { wgsl_psx_vert, wgsl_hw_cloud_resolve_frag, B(0) | B(4) | B(16) | B(17), 0, 1, { F16 }, { RGBA } },
        [P_POST]   = { wgsl_psx_vert, wgsl_hw_post_frag,
                       B(0) | B(1) | B(2) | B(3) | B(4) | B(5) | B(6) | B(7) | B(8) | B(9) | B(10) | B(14) | B(15),
                       0, 1, { F16 }, { RGBA } },
        [P_SUNVIS] = { wgsl_psx_vert, wgsl_hw_sunvis_frag, B(0) | B(2) | B(3), 0, 1, { WGPUTextureFormat_R16Float }, { R } },
        [P_COMP]   = { wgsl_psx_vert, wgsl_hw_composite_frag, B(0) | B(11) | B(12) | B(13), 0, 1, { RGBA8 }, { RGB } },
        [P_ZCLEAR] = { wgsl_psx_vert, wgsl_zclear_fs, B(0), 0, 2, { RGBA8, R32 }, { WGPUColorWriteMask_None, R }, 3 },
        [P_OCEAN]  = { wgsl_hw_ocean_vert, wgsl_hw_ocean_frag, B(0) | B(5) | B(6) | B(7) | B(8) | B(14) | B(15), 0, 2,
                       { RGBA8, R32 }, { RGB, R }, 1, 0, &ovb, 1 },
    };
#undef B
    for (int p = 0; p < P_BDOWN; p++) make_pipe(p, &d[p]);

    /* bloom: own layout -- group 0 = source texture, group 1 = push, group 2 = linear sampler */
    {
        WGPUBindGroupLayoutEntry te = { 0 }, se = { 0 }, pe = { 0 };
        te.binding = 0; te.visibility = WGPUShaderStage_Fragment;
        te.texture.sampleType = WGPUTextureSampleType_Float; te.texture.viewDimension = WGPUTextureViewDimension_2D;
        se.binding = 0; se.visibility = WGPUShaderStage_Fragment; se.sampler.type = WGPUSamplerBindingType_Filtering;
        pe.binding = 0; pe.visibility = WGPUShaderStage_Fragment;
        pe.buffer.type = WGPUBufferBindingType_Uniform; pe.buffer.hasDynamicOffset = 1; pe.buffer.minBindingSize = 24;
        WGPUBindGroupLayoutDescriptor dt = { 0 }, ds = { 0 }, dp = { 0 };
        dt.entryCount = 1; dt.entries = &te;
        ds.entryCount = 1; ds.entries = &se;
        dp.entryCount = 1; dp.entries = &pe;
        WGPUBindGroupLayout lt = wgpuDeviceCreateBindGroupLayout(HS.dev, &dt);
        WGPUBindGroupLayout ls = wgpuDeviceCreateBindGroupLayout(HS.dev, &ds);
        WGPUBindGroupLayout lp = wgpuDeviceCreateBindGroupLayout(HS.dev, &dp);
        for (int i = 0; i <= FX_BLOOM_MIPS; i++) {
            WGPUBindGroupEntry g = { 0 };
            g.binding = 0; g.textureView = i == 0 ? F.hdr.view : F.bloom_view[i - 1];
            WGPUBindGroupDescriptor bd = { 0 };
            bd.layout = lt; bd.entryCount = 1; bd.entries = &g;
            F.bloom_src[i] = wgpuDeviceCreateBindGroup(HS.dev, &bd);
        }
        WGPUBindGroupEntry gs = { 0 }, gp = { 0 };
        gs.binding = 0; gs.sampler = F.s_lin;
        gp.binding = 0; gp.buffer = F.push; gp.size = 32;
        WGPUBindGroupDescriptor bs = { 0 }, bp = { 0 };
        bs.layout = ls; bs.entryCount = 1; bs.entries = &gs;
        bp.layout = lp; bp.entryCount = 1; bp.entries = &gp;
        F.g2[P_BDOWN] = F.g2[P_BUP] = wgpuDeviceCreateBindGroup(HS.dev, &bs);
        F.g1[P_BDOWN] = F.g1[P_BUP] = wgpuDeviceCreateBindGroup(HS.dev, &bp);
        WGPUBindGroupLayout all[3] = { lt, lp, ls };
        WGPUPipelineLayoutDescriptor pd = { 0 };
        pd.bindGroupLayoutCount = 3; pd.bindGroupLayouts = all;
        WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(HS.dev, &pd);
        for (int up = 0; up < 2; up++) {
            WGPUShaderModule vs = module(wgsl_psx_vert), fs = module(up ? wgsl_hw_bloom_up_frag : wgsl_hw_bloom_down_frag);
            WGPUBlendState add;
            add.color.operation = add.alpha.operation = WGPUBlendOperation_Add;
            add.color.srcFactor = add.color.dstFactor = WGPUBlendFactor_One;
            add.alpha = add.color;
            WGPUColorTargetState ct = { 0 };
            ct.format = F16; ct.writeMask = RGBA; ct.blend = up ? &add : NULL;
            WGPUFragmentState fsd = { 0 };
            fsd.module = fs; fsd.entryPoint = "main"; fsd.targetCount = 1; fsd.targets = &ct;
            WGPURenderPipelineDescriptor rd = { 0 };
            rd.layout = layout;
            rd.vertex.module = vs; rd.vertex.entryPoint = "main";
            rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            rd.multisample.count = 1; rd.multisample.mask = 0xFFFFFFFFu;
            rd.fragment = &fsd;
            F.pipe[up ? P_BUP : P_BDOWN] = wgpuDeviceCreateRenderPipeline(HS.dev, &rd);
            wgpuShaderModuleRelease(vs); wgpuShaderModuleRelease(fs);
        }
    }
    for (int p = 0; p < NPIPE; p++)
        if (!F.pipe[p]) { fprintf(stderr, "ac3: effects pipeline %d failed\n", p); return 0; }

    u8 *vol = (u8 *)malloc((size_t)NOISE_N * NOISE_N * NOISE_N * 4), *wea = (u8 *)malloc((size_t)WEATHER_N * WEATHER_N * 4);
    if (!vol || !wea) { free(vol); free(wea); return 0; }
    make_noise(vol, wea);
    upload_mips(F.noise.tex, vol, NOISE_N, 1, NOISE_MIPS);
    upload_mips(F.weather.tex, wea, WEATHER_N, 0, WEATHER_MIPS);
    free(vol); free(wea);
    F.ready = 1;
    return 1;
}

void hw_fx_shutdown(void) {
    if (!F.ready) return;
    img_t *im[] = { &F.depth, &F.cloud, &F.cloudz, &F.cloud_raw, &F.cloud_hist, &F.grid, &F.height, &F.colors,
                    &F.top, &F.noise, &F.weather, &F.hdr, &F.sunvis, &F.odisp, &F.oderiv };
    for (unsigned i = 0; i < sizeof im / sizeof im[0]; i++) if (im[i]->tex) wgpuTextureDestroy(im[i]->tex);
    if (F.bloom) wgpuTextureDestroy(F.bloom);
    hw_ocean_webgpu_shutdown();
    WGPUBuffer b[] = { F.ubo, F.push, F.inst, F.mesh };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) if (b[i]) wgpuBufferDestroy(b[i]);
    memset(&F, 0, sizeof F);
}

/* ------------------------------------------------- this field's inputs */

int hw_fx_unpack(const u8 *p, size_t len) {
    F.h.nscenes = 0;
    if (!F.ready || len < sizeof(fx_pkt_t)) return len == 0;
    memcpy(&F.h, p, sizeof F.h);
    const fx_pkt_t *h = &F.h;
    if (h->nscenes > FX_MAX_SCENES || h->ndraws[0] > FX_MAX_DRAWS || h->ndraws[1] > FX_MAX_DRAWS ||
        h->ninst[0] + h->ninst[1] > FX_MAX_INST * 2 || h->mesh_n > FX_MESH_CAP ||
        h->nnodes[0] > HW_OCEAN_MAX_NODES || h->nnodes[1] > HW_OCEAN_MAX_NODES) { F.h.nscenes = 0; return 0; }
    size_t need = sizeof *h + (size_t)h->nscenes * sizeof(fx_ubo_t) +
                  (size_t)(h->ndraws[0] + h->ndraws[1]) * sizeof(tile_draw_t) +
                  (size_t)(h->ninst[0] + h->ninst[1]) * 8 +
                  (size_t)(h->nnodes[0] + h->nnodes[1]) * sizeof(hw_ocean_node_t) + h->map_bytes;
    if (need != len || (h->map_bytes && h->map_bytes != FX_MAP_BYTES(h->mesh_n))) { F.h.nscenes = 0; return 0; }
    p += sizeof *h;
    for (u32 i = 0; i < h->nscenes; i++, p += sizeof(fx_ubo_t))
        wgpuQueueWriteBuffer(HS.queue, F.ubo, (u64)FX_UBO_STRIDE * i, p, sizeof(fx_ubo_t));
    for (int b = 0; b < 2; b++) {
        memcpy(F.draws[b], p, (size_t)h->ndraws[b] * sizeof(tile_draw_t));
        p += (size_t)h->ndraws[b] * sizeof(tile_draw_t);
    }
    u32 ni = h->ninst[0] + h->ninst[1];
    if (ni) wgpuQueueWriteBuffer(HS.queue, F.inst, 0, p, (size_t)ni * 8);
    p += (size_t)ni * 8;
    F.inst_base[0] = 0;
    F.inst_base[1] = h->ninst[0];
    for (int b = 0; b < 2; b++) {
        size_t nb = (size_t)h->nnodes[b] * sizeof(hw_ocean_node_t);
        if (nb && hw_ocean_webgpu_ready())
            wgpuQueueWriteBuffer(HS.queue, hw_ocean_webgpu_nodes(),
                                 (u64)b * HW_OCEAN_MAX_NODES * sizeof(hw_ocean_node_t), p, nb);
        p += nb;
    }
    if (h->map_bytes) {
        write2d(F.grid.tex, p, HW_MAP_N, HW_MAP_N, 2);           p += (size_t)HW_MAP_N * HW_MAP_N * 2;
        write2d(F.top.tex, p, 512, 1, 4);                        p += 512 * 4;
        write2d(F.height.tex, p, HW_ATLAS_W, HW_ATLAS_H, 4);     p += (size_t)HW_ATLAS_W * HW_ATLAS_H * 4;
        write2d(F.colors.tex, p, HW_ATLAS_W, HW_ATLAS_H, 4);     p += (size_t)HW_ATLAS_W * HW_ATLAS_H * 4;
        if (h->mesh_n) wgpuQueueWriteBuffer(HS.queue, F.mesh, 0, p, (size_t)h->mesh_n * sizeof(hw_tile_vtx_t));
        printf("renderer: world map received (%u terrain vertices)\n", h->mesh_n);
    }
    F.push_next = 2;
    return 1;
}

/* ------------------------------------------------------------ recording */

void hw_fx_frame_done(void) { F.h.nscenes = 0; }

static void clear_rgba(WGPUCommandEncoder enc, WGPUTextureView v) {
    WGPURenderPassColorAttachment a = { 0 };
    a.view = v; a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    a.loadOp = WGPULoadOp_Clear; a.storeOp = WGPUStoreOp_Store;
    a.clearValue = (WGPUColor){ 0, 0, 0, 1 };
    WGPURenderPassDescriptor d = { 0 };
    d.colorAttachmentCount = 1; d.colorAttachments = &a;
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &d);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
}

void hw_fx_prepare_webgpu(WGPUCommandEncoder enc) {
    if (!F.ready) return;
    if (F.h.ocean_sim) hw_ocean_webgpu_simulate(enc, F.h.ocean_t, F.h.ocean_dt);
    if (F.cleared) return;
    /* hw_fx.c clears the cloud images to (0,0,0,1) before first use */
    WGPUTextureView v[4] = { F.cloud.view, F.cloudz.view, F.cloud_raw.view, F.cloud_hist.view };
    for (int i = 0; i < 4; i++) clear_rgba(enc, v[i]);
    F.cleared = 1;
}

typedef struct { s32 x, y; u32 w, h; } rect_t2;

static rect_t2 area_rect(const s32 *a, int div) {
    s32 x0 = a[0], y0 = a[1], x1 = a[2] + 1, y1 = a[3] + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W) x1 = VRAM_W;
    if (y1 > VRAM_H) y1 = VRAM_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    rect_t2 r;
    r.x = x0 * HS.S / div;
    r.y = y0 * HS.S / div;
    r.w = (u32)((x1 * HS.S + div - 1) / div - r.x);
    r.h = (u32)((y1 * HS.S + div - 1) / div - r.y);
    return r;
}

/* a render pass with up to two colour attachments (loaded) and an optional depth */
static WGPURenderPassEncoder pass(WGPUCommandEncoder enc, WGPUTextureView c0, WGPUTextureView c1,
                                  WGPUTextureView depth, int clear_depth, u32 vw, u32 vh, rect_t2 sc) {
    WGPURenderPassColorAttachment a[2] = { { 0 }, { 0 } };
    WGPUTextureView cv[2] = { c0, c1 };
    int n = 0;
    for (int i = 0; i < 2; i++) {
        if (!cv[i]) continue;
        a[n].view = cv[i]; a[n].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        a[n].loadOp = WGPULoadOp_Load; a[n].storeOp = WGPUStoreOp_Store;
        n++;
    }
    WGPURenderPassDepthStencilAttachment da = { 0 };
    da.view = depth;
    da.depthLoadOp = clear_depth ? WGPULoadOp_Clear : WGPULoadOp_Load;
    da.depthStoreOp = WGPUStoreOp_Store;
    da.depthClearValue = 0.0f;
    WGPURenderPassDescriptor d = { 0 };
    d.colorAttachmentCount = (size_t)n; d.colorAttachments = n ? a : NULL;
    d.depthStencilAttachment = depth ? &da : NULL;
    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &d);
    wgpuRenderPassEncoderSetViewport(rp, 0, 0, (float)vw, (float)vh, 0, 1);
    wgpuRenderPassEncoderSetScissorRect(rp, (u32)sc.x, (u32)sc.y, sc.w, sc.h);
    return rp;
}

static void end_pass(WGPURenderPassEncoder rp) {
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
}

static void bind(WGPURenderPassEncoder rp, int p, u32 scene, int push_slot) {
    wgpuRenderPassEncoderSetPipeline(rp, F.pipe[p]);
    u32 off = FX_UBO_STRIDE * scene;
    wgpuRenderPassEncoderSetBindGroup(rp, 0, F.g0[p], 1, &off);
    u32 poff = (u32)(push_slot < 0 ? 0 : push_slot) * 256u;
    if (F.g1[p] == F.empty_g) wgpuRenderPassEncoderSetBindGroup(rp, 1, F.g1[p], 0, NULL);
    else wgpuRenderPassEncoderSetBindGroup(rp, 1, F.g1[p], 1, &poff);
    wgpuRenderPassEncoderSetBindGroup(rp, 2, F.g2[p], 0, NULL);
}

static void copy_rect(WGPUCommandEncoder enc, WGPUTexture src, WGPUTexture dst, rect_t2 r) {
    if (!r.w || !r.h) return;
    WGPUImageCopyTexture a = { 0 }, b = { 0 };
    a.texture = src; a.origin.x = (u32)r.x; a.origin.y = (u32)r.y; a.aspect = WGPUTextureAspect_All;
    b.texture = dst; b.origin.x = (u32)r.x; b.origin.y = (u32)r.y; b.aspect = WGPUTextureAspect_All;
    WGPUExtent3D e = { r.w, r.h, 1 };
    wgpuCommandEncoderCopyTextureToTexture(enc, &a, &b, &e);
}

/* hw_fx.c record_begin: far terrain tiles + sky/terrain background behind the game's polygons */
static void record_begin(WGPUCommandEncoder enc, u32 si) {
    rect_t2 sc = area_rect(F.h.area[si], 1);
    if (!sc.w || !sc.h) return;
    WGPURenderPassEncoder rp = pass(enc, HS.target_view, HS.z_view, F.depth.view, 1, HS.W, HS.Hs, sc);
    bind(rp, P_ZCLEAR, si, -1);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    int no = F.h.ocean_scene[0] == (s32)si ? 0 : F.h.ocean_scene[1] == (s32)si ? 1 : -1;
    if (no >= 0 && F.h.nnodes[no] && hw_ocean_webgpu_ready()) {
        u32 nidx;
        WGPUBuffer ib = hw_ocean_webgpu_indices(&nidx);
        bind(rp, P_OCEAN, si, -1);
        wgpuRenderPassEncoderSetVertexBuffer(rp, 0, hw_ocean_webgpu_nodes(),
                                             (u64)no * HW_OCEAN_MAX_NODES * sizeof(hw_ocean_node_t), WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(rp, ib, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(rp, nidx, F.h.nnodes[no], 0, 0, 0);
    }
    int nb = F.h.tiles_scene[0] == (s32)si ? 0 : F.h.tiles_scene[1] == (s32)si ? 1 : -1;
    if (nb >= 0 && F.h.ndraws[nb]) {
        bind(rp, P_TILE, si, -1);
        wgpuRenderPassEncoderSetVertexBuffer(rp, 0, F.mesh, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetVertexBuffer(rp, 1, F.inst, (u64)F.inst_base[nb] * 8, WGPU_WHOLE_SIZE);
        for (u32 i = 0; i < F.h.ndraws[nb]; i++) {
            const tile_draw_t *d = &F.draws[nb][i];
            wgpuRenderPassEncoderDraw(rp, d->nvtx, d->ninst, d->first_vtx, d->first_inst);
        }
    }
    if ((F.h.flags & FX_FAR) && F.h.map_valid) {
        bind(rp, P_BG, si, -1);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    }
    end_pass(rp);
}

/* hw_fx.c record_end: forest, clouds, atmosphere/HDR, sun, bloom, composite */
static void record_end(WGPUCommandEncoder enc, u32 si) {
    rect_t2 sc = area_rect(F.h.area[si], 1);
    if (!sc.w || !sc.h) return;
    WGPURenderPassEncoder rp;
    if ((F.h.flags & FX_FOREST) && F.h.map_valid && F.h.forest) {
        rp = pass(enc, NULL, NULL, F.depth.view, 1, HS.W, HS.Hs, sc);
        bind(rp, P_PRIME, si, -1);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        end_pass(rp);
        rp = pass(enc, HS.target_view, NULL, F.depth.view, 0, HS.W, HS.Hs, sc);
        for (int r = 0; r < 2; r++) {
            bind(rp, P_TREE, si, r);
            u32 n = r ? 296u : 292u;
            wgpuRenderPassEncoderDraw(rp, 42, n * n, 0, 0);
        }
        end_pass(rp);
    }
    copy_rect(enc, HS.target, HS.read, sc);

    if (F.h.flags & FX_CLOUDS) {
        rect_t2 hc = area_rect(F.h.area[si], 2);
        if (hc.x + (s32)hc.w > (s32)F.CW) hc.w = F.CW - (u32)hc.x;
        if (hc.y + (s32)hc.h > (s32)F.CH) hc.h = F.CH - (u32)hc.y;
        rp = pass(enc, F.cloud_raw.view, F.cloudz.view, NULL, 0, F.CW, F.CH, hc);
        bind(rp, P_CLOUD, si, -1);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        end_pass(rp);
        rp = pass(enc, F.cloud.view, NULL, NULL, 0, F.CW, F.CH, hc);
        bind(rp, P_CRES, si, -1);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        end_pass(rp);
        copy_rect(enc, F.cloud.tex, F.cloud_hist.tex, hc);
    }

    rect_t2 hs = { 0, 0, sc.w, sc.h };
    if (hs.w > F.HDW) hs.w = F.HDW;
    if (hs.h > F.HDH) hs.h = F.HDH;
    rp = pass(enc, F.hdr.view, NULL, NULL, 0, F.HDW, F.HDH, hs);
    bind(rp, P_POST, si, -1);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    end_pass(rp);

    rect_t2 one = { 0, 0, 1, 1 };
    rp = pass(enc, F.sunvis.view, NULL, NULL, 0, 1, 1, one);
    bind(rp, P_SUNVIS, si, -1);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    end_pass(rp);

    bloom_push_t bp;
    memset(&bp, 0, sizeof bp);
    bp.uv_max[0] = (float)hs.w / (float)F.HDW;
    bp.uv_max[1] = (float)hs.h / (float)F.HDH;
    bp.radius = 1.0f;
    for (int k = 0; k < 2 * FX_BLOOM_MIPS - 1; k++) {
        int up = k >= FX_BLOOM_MIPS;
        int m = up ? FX_BLOOM_MIPS - 2 - (k - FX_BLOOM_MIPS) : k;
        u32 w = (F.HDW / 2) >> m, h = (F.HDH / 2) >> m;
        if (up) { bp.src_texel[0] = 1.0f / (float)(w / 2); bp.src_texel[1] = 1.0f / (float)(h / 2); bp.first = 0.0f; }
        else    { bp.src_texel[0] = 1.0f / (float)(w * 2); bp.src_texel[1] = 1.0f / (float)(h * 2); bp.first = m == 0 ? 1.0f : 0.0f; }
        if (F.push_next >= PUSH_SLOTS) break;
        u32 slot = F.push_next++;
        wgpuQueueWriteBuffer(HS.queue, F.push, (u64)slot * 256, &bp, sizeof bp);
        rect_t2 r = { 0, 0, w, h };
        rp = pass(enc, F.bloom_view[m], NULL, NULL, 0, w, h, r);
        int p = up ? P_BUP : P_BDOWN;
        wgpuRenderPassEncoderSetPipeline(rp, F.pipe[p]);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, F.bloom_src[up ? m + 2 : m], 0, NULL);
        u32 off = slot * 256u;
        wgpuRenderPassEncoderSetBindGroup(rp, 1, F.g1[p], 1, &off);
        wgpuRenderPassEncoderSetBindGroup(rp, 2, F.g2[p], 0, NULL);
        wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
        end_pass(rp);
    }

    rp = pass(enc, HS.target_view, NULL, NULL, 0, HS.W, HS.Hs, sc);
    bind(rp, P_COMP, si, -1);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    end_pass(rp);

    copy_rect(enc, HS.zimg, HS.zsnap, sc);
}

void hw_fx_record_webgpu(WGPUCommandEncoder enc, const cmd_t *c) {
    if (!F.ready || c->first >= F.h.nscenes) return;
    if (c->op == OP_FX_BEGIN) record_begin(enc, c->first);
    else record_end(enc, c->first);
}
