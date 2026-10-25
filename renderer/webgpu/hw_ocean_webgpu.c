/*
 * hw_ocean_webgpu.c -- the FFT ocean (renderer/hw/hw_ocean.c) on WebGPU.
 *
 * Same simulation, same numbers; storage changes because core WebGPU cannot
 * read and write a 4-channel float image in the same shader:
 *   h0, P, Q          rgba32f image arrays     -> storage buffers of vec4
 *   deriv.a (foam)    read-modify-write        -> its own f32 storage buffer
 *   mip chain         vkCmdBlitImage (linear)  -> 2x2-average compute pass
 * The compute shaders below are line-for-line translations of
 * renderer/shaders/ocean_spectrum.comp, ocean_fft.comp, ocean_finalize.comp.
 * disp / deriv stay rgba16f 2D arrays with 9 mips, sampled by hw_ocean.vert
 * and hw_post.frag exactly as natively.
 */
#include "hw_ocean_webgpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS hw_state
#define ON HW_OCEAN_N
#define NC HW_OCEAN_CASCADES
#define NM HW_OCEAN_MIPS

static const float PATCH_M[NC] = { 500.0f, 85.0f, 17.0f };   /* hw_ocean.c */

static const char wgsl_spectrum[] =
    "struct Sp { patch_m: vec4<f32>, time: f32, n: f32, w0: f32, pad: f32 };\n"
    "@group(0) @binding(0) var<storage, read> h0: array<vec4<f32>>;\n"
    "@group(0) @binding(1) var<storage, read_write> P: array<vec4<f32>>;\n"
    "@group(0) @binding(2) var<storage, read_write> Q: array<vec4<f32>>;\n"
    "@group(1) @binding(0) var<uniform> pc: Sp;\n"
    "fn cmul(a: vec2<f32>, b: vec2<f32>) -> vec2<f32> { return vec2<f32>(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }\n"
    "fn mul_i(a: vec2<f32>) -> vec2<f32> { return vec2<f32>(-a.y, a.x); }\n"
    "@compute @workgroup_size(16, 16)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "  let id = vec3<i32>(gid);\n"
    "  let N = i32(pc.n);\n"
    "  let L = pc.patch_m[id.z];\n"
    "  let k = 6.2831853 * (vec2<f32>(id.xy) - f32(N / 2)) / L;\n"
    "  let kl = length(k);\n"
    "  let idx = (id.z * N + id.y) * N + id.x;\n"
    "  let s = h0[idx];\n"
    "  let w = floor(sqrt(9.81 * kl) / pc.w0) * pc.w0;\n"
    "  let e = vec2<f32>(cos(w * pc.time), sin(w * pc.time));\n"
    "  let h = cmul(s.xy, e) + cmul(s.zw, vec2<f32>(e.x, -e.y));\n"
    "  var kn = vec2<f32>(0.0);\n"
    "  var ik = 0.0;\n"
    "  if (kl > 1e-6) { kn = k / kl; ik = 1.0 / kl; }\n"
    "  let dx = mul_i(h) * kn.x;\n"
    "  let dz = mul_i(h) * kn.y;\n"
    "  let sx = mul_i(h) * k.x;\n"
    "  let sz = mul_i(h) * k.y;\n"
    "  let jxx = h * (-k.x * k.x * ik);\n"
    "  let jzz = h * (-k.y * k.y * ik);\n"
    "  let jxz = h * (-k.x * k.y * ik);\n"
    "  P[idx] = vec4<f32>(h + mul_i(dx), dz + mul_i(sx));\n"
    "  Q[idx] = vec4<f32>(sz + mul_i(jxx), jzz + mul_i(jxz));\n"
    "}\n";

static const char wgsl_fft[] =
    "struct Fp { columns: i32, layers: i32, pad0: i32, pad1: i32 };\n"
    "@group(0) @binding(1) var<storage, read_write> P: array<vec4<f32>>;\n"
    "@group(0) @binding(2) var<storage, read_write> Q: array<vec4<f32>>;\n"
    "@group(1) @binding(0) var<uniform> pc: Fp;\n"
    "var<workgroup> buf: array<vec4<f32>, 256>;\n"
    "fn cmul(a: vec2<f32>, b: vec2<f32>) -> vec2<f32> { return vec2<f32>(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }\n"
    "fn at(line: u32, i: u32, layer: u32) -> u32 {\n"
    "  if (pc.columns != 0) { return (layer * 256u + i) * 256u + line; }\n"
    "  return (layer * 256u + line) * 256u + i;\n"
    "}\n"
    "@compute @workgroup_size(256)\n"
    "fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>) {\n"
    "  let i = lid.x;\n"
    "  let line = wid.y;\n"
    "  let which = wid.z;\n"
    "  let layer = which % u32(pc.layers);\n"
    "  let isq = which >= u32(pc.layers);\n"
    "  var v: vec4<f32>;\n"
    "  if (isq) { v = Q[at(line, i, layer)]; } else { v = P[at(line, i, layer)]; }\n"
    "  buf[reverseBits(i) >> 24u] = v;\n"
    "  workgroupBarrier();\n"
    "  for (var s = 1u; s <= 8u; s++) {\n"
    "    let m = 1u << s;\n"
    "    let half_ = m >> 1u;\n"
    "    var a = vec4<f32>(0.0);\n"
    "    var bw = vec4<f32>(0.0);\n"
    "    var top = 0u;\n"
    "    var bot = 0u;\n"
    "    if (i < 128u) {\n"
    "      let k = i % half_;\n"
    "      top = (i / half_) * m + k;\n"
    "      bot = top + half_;\n"
    "      let ang = 6.2831853 * f32(k) / f32(m);\n"
    "      let w = vec2<f32>(cos(ang), sin(ang));\n"
    "      a = buf[top];\n"
    "      let b = buf[bot];\n"
    "      bw = vec4<f32>(cmul(b.xy, w), cmul(b.zw, w));\n"
    "    }\n"
    "    workgroupBarrier();\n"
    "    if (i < 128u) {\n"
    "      buf[top] = a + bw;\n"
    "      buf[bot] = a - bw;\n"
    "    }\n"
    "    workgroupBarrier();\n"
    "  }\n"
    "  if (isq) { Q[at(line, i, layer)] = buf[i]; } else { P[at(line, i, layer)] = buf[i]; }\n"
    "}\n";

static const char wgsl_finalize[] =
    "struct Fin { choppy: f32, foam_decay: f32, first: f32, pad: f32 };\n"
    "@group(0) @binding(1) var<storage, read> P: array<vec4<f32>>;\n"
    "@group(0) @binding(2) var<storage, read> Q: array<vec4<f32>>;\n"
    "@group(0) @binding(3) var disp: texture_storage_2d_array<rgba16float, write>;\n"
    "@group(0) @binding(4) var deriv: texture_storage_2d_array<rgba16float, write>;\n"
    "@group(0) @binding(5) var<storage, read_write> foam: array<f32>;\n"
    "@group(1) @binding(0) var<uniform> pc: Fin;\n"
    "@compute @workgroup_size(16, 16)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "  let id = vec3<i32>(gid);\n"
    "  let idx = (gid.z * 256u + gid.y) * 256u + gid.x;\n"
    "  var sg = -1.0;\n"
    "  if (((id.x + id.y) & 1) == 0) { sg = 1.0; }\n"
    "  let p = P[idx] * sg;\n"
    "  let q = Q[idx] * sg;\n"
    "  let h = p.x; let dx = p.y; let dz = p.z; let sx = p.w;\n"
    "  let sz = q.x; let jxx = q.y; let jzz = q.z; let jxz = q.w;\n"
    "  let l = pc.choppy;\n"
    "  textureStore(disp, id.xy, id.z, vec4<f32>(dx * l, h, dz * l, 0.0));\n"
    "  let ax = 1.0 + l * jxx;\n"
    "  let az = 1.0 + l * jzz;\n"
    "  let J = ax * az - (l * jxz) * (l * jxz);\n"
    "  let slope = vec2<f32>(sx / max(ax, 0.2), sz / max(az, 0.2));\n"
    "  var old = foam[idx];\n"
    "  if (pc.first > 0.5) { old = 0.0; }\n"
    "  let squeeze = max(old * pc.foam_decay, max(1.0 - J, 0.0));\n"
    "  foam[idx] = squeeze;\n"
    "  textureStore(deriv, id.xy, id.z, vec4<f32>(slope, J - 1.0, squeeze));\n"
    "}\n";

/* one mip level from the previous one: a linear 2:1 blit is a 2x2 average */
static const char wgsl_down[] =
    "@group(0) @binding(0) var src: texture_2d_array<f32>;\n"
    "@group(0) @binding(1) var dst: texture_storage_2d_array<rgba16float, write>;\n"
    "@compute @workgroup_size(8, 8)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "  let d = textureDimensions(dst);\n"
    "  if (gid.x >= d.x || gid.y >= d.y) { return; }\n"
    "  let p = vec2<i32>(gid.xy) * 2;\n"
    "  let l = i32(gid.z);\n"
    "  let v = textureLoad(src, p, l, 0) + textureLoad(src, p + vec2<i32>(1, 0), l, 0) +\n"
    "          textureLoad(src, p + vec2<i32>(0, 1), l, 0) + textureLoad(src, p + vec2<i32>(1, 1), l, 0);\n"
    "  textureStore(dst, vec2<i32>(gid.xy), l, v * 0.25);\n"
    "}\n";

static struct {
    int ready, first;
    WGPUBuffer h0, P, Q, foam, idx, nodes;
    WGPUBuffer u_spec, u_fft[2], u_fin;
    WGPUTexture disp, deriv;
    WGPUTextureView disp_view, deriv_view;          /* all mips, for sampling */
    WGPUSampler samp;
    WGPUComputePipeline p_spec, p_fft, p_fin, p_down;
    WGPUBindGroup g_spec, g_fft, g_fin, u_g_spec, u_g_fft[2], u_g_fin;
    WGPUBindGroup g_down[2][NM];                     /* [texture][dst mip] */
    u32 nidx;
} O;

static WGPUBuffer buffer(u64 size, WGPUBufferUsageFlags usage) {
    WGPUBufferDescriptor d = { 0 };
    d.usage = usage; d.size = size;
    return wgpuDeviceCreateBuffer(HS.dev, &d);
}

static WGPUShaderModule module(const char *code) {
    WGPUShaderModuleWGSLDescriptor w = { 0 };
    w.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    w.code = code;
    WGPUShaderModuleDescriptor d = { 0 };
    d.nextInChain = &w.chain;
    return wgpuDeviceCreateShaderModule(HS.dev, &d);
}

static WGPUBindGroupLayoutEntry sbuf(u32 b, int ro) {
    WGPUBindGroupLayoutEntry e = { 0 };
    e.binding = b; e.visibility = WGPUShaderStage_Compute;
    e.buffer.type = ro ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
    return e;
}
static WGPUBindGroupLayoutEntry stex(u32 b) {
    WGPUBindGroupLayoutEntry e = { 0 };
    e.binding = b; e.visibility = WGPUShaderStage_Compute;
    e.storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    e.storageTexture.format = WGPUTextureFormat_RGBA16Float;
    e.storageTexture.viewDimension = WGPUTextureViewDimension_2DArray;
    return e;
}
static WGPUBindGroupEntry bbuf(u32 b, WGPUBuffer buf) {
    WGPUBindGroupEntry e = { 0 };
    e.binding = b; e.buffer = buf; e.size = WGPU_WHOLE_SIZE;
    return e;
}
static WGPUBindGroupEntry btex(u32 b, WGPUTextureView v) {
    WGPUBindGroupEntry e = { 0 };
    e.binding = b; e.textureView = v;
    return e;
}

static WGPUBindGroupLayout bgl(const WGPUBindGroupLayoutEntry *e, size_t n) {
    WGPUBindGroupLayoutDescriptor d = { 0 };
    d.entryCount = n; d.entries = e;
    return wgpuDeviceCreateBindGroupLayout(HS.dev, &d);
}
static WGPUBindGroup group(WGPUBindGroupLayout l, const WGPUBindGroupEntry *e, size_t n) {
    WGPUBindGroupDescriptor d = { 0 };
    d.layout = l; d.entryCount = n; d.entries = e;
    return wgpuDeviceCreateBindGroup(HS.dev, &d);
}

/* a compute pipeline with group 0 = `e0` and group 1 = one uniform (16..32 bytes) */
static WGPUComputePipeline cpipe(const char *code, const WGPUBindGroupLayoutEntry *e0, size_t n0,
                                 WGPUBindGroupLayout *l0_out, WGPUBindGroupLayout *l1_out) {
    WGPUBindGroupLayout l0 = bgl(e0, n0), l1 = NULL;
    WGPUBindGroupLayout ls[2] = { l0, NULL };
    size_t nl = 1;
    if (l1_out) {
        WGPUBindGroupLayoutEntry u = { 0 };
        u.binding = 0; u.visibility = WGPUShaderStage_Compute;
        u.buffer.type = WGPUBufferBindingType_Uniform;
        l1 = bgl(&u, 1);
        ls[1] = l1; nl = 2;
    }
    WGPUPipelineLayoutDescriptor pd = { 0 };
    pd.bindGroupLayoutCount = nl; pd.bindGroupLayouts = ls;
    WGPUShaderModule m = module(code);
    WGPUComputePipelineDescriptor cd = { 0 };
    cd.layout = wgpuDeviceCreatePipelineLayout(HS.dev, &pd);
    cd.compute.module = m; cd.compute.entryPoint = "main";
    WGPUComputePipeline p = wgpuDeviceCreateComputePipeline(HS.dev, &cd);
    wgpuShaderModuleRelease(m);
    *l0_out = l0;
    if (l1_out) *l1_out = l1;
    return p;
}

static WGPUBindGroup ugroup(WGPUBindGroupLayout l1, WGPUBuffer u) {
    WGPUBindGroupEntry e = bbuf(0, u);
    return group(l1, &e, 1);
}

static WGPUTextureView mip_view(WGPUTexture t, u32 base, u32 count) {
    WGPUTextureViewDescriptor vd = { 0 };
    vd.format = WGPUTextureFormat_RGBA16Float;
    vd.dimension = WGPUTextureViewDimension_2DArray;
    vd.baseMipLevel = base; vd.mipLevelCount = count;
    vd.baseArrayLayer = 0; vd.arrayLayerCount = NC;
    vd.aspect = WGPUTextureAspect_All;
    return wgpuTextureCreateView(t, &vd);
}

int hw_ocean_webgpu_init(void) {
    memset(&O, 0, sizeof O);
    const u64 N4 = (u64)NC * ON * ON * 16;
    const WGPUBufferUsageFlags SB = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    O.h0 = buffer(N4, SB);
    O.P = buffer(N4, SB);
    O.Q = buffer(N4, SB);
    O.foam = buffer((u64)NC * ON * ON * 4, SB);
    O.u_spec = buffer(32, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    O.u_fft[0] = buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    O.u_fft[1] = buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    O.u_fin = buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    for (int col = 0; col < 2; col++) {
        s32 fp[4] = { col, NC, 0, 0 };
        wgpuQueueWriteBuffer(HS.queue, O.u_fft[col], 0, fp, sizeof fp);
    }

    WGPUTextureDescriptor td = { 0 };
    td.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = ON; td.size.height = ON; td.size.depthOrArrayLayers = NC;
    td.format = WGPUTextureFormat_RGBA16Float;
    td.mipLevelCount = NM;
    td.sampleCount = 1;
    O.disp = wgpuDeviceCreateTexture(HS.dev, &td);
    O.deriv = wgpuDeviceCreateTexture(HS.dev, &td);
    O.disp_view = mip_view(O.disp, 0, NM);
    O.deriv_view = mip_view(O.deriv, 0, NM);
    {
        WGPUSamplerDescriptor d = { 0 };
        d.addressModeU = d.addressModeV = d.addressModeW = WGPUAddressMode_Repeat;
        d.magFilter = d.minFilter = WGPUFilterMode_Linear;
        d.mipmapFilter = WGPUMipmapFilterMode_Linear;
        d.lodMaxClamp = (float)(NM - 1);
        d.maxAnisotropy = 1;
        O.samp = wgpuDeviceCreateSampler(HS.dev, &d);
    }

    /* initial spectrum (same seed as native) */
    float *h0 = (float *)malloc((size_t)N4);
    if (!h0) return 0;
    hw_ocean_spectrum(h0, NULL);
    wgpuQueueWriteBuffer(HS.queue, O.h0, 0, h0, (size_t)N4);
    free(h0);

    /* patch grid indices (hw_ocean_init) */
    const u32 Q = HW_OCEAN_QUADS, V = Q + 1;
    O.nidx = Q * Q * 6;
    u16 *ix = (u16 *)malloc((size_t)O.nidx * 2), *w = ix;
    for (u32 j = 0; j < Q; j++)
        for (u32 i = 0; i < Q; i++) {
            u16 a = (u16)(j * V + i), b = (u16)(a + 1), c = (u16)(a + V), d = (u16)(c + 1);
            *w++ = a; *w++ = c; *w++ = b;
            *w++ = b; *w++ = c; *w++ = d;
        }
    O.idx = buffer((u64)O.nidx * 2, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst);
    wgpuQueueWriteBuffer(HS.queue, O.idx, 0, ix, (size_t)O.nidx * 2);
    free(ix);
    O.nodes = buffer((u64)HW_OCEAN_MAX_NODES * 2 * sizeof(hw_ocean_node_t),
                     WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst);

    /* pipelines + bind groups */
    WGPUBindGroupLayout l0, l1;
    WGPUBindGroupLayoutEntry es[3] = { sbuf(0, 1), sbuf(1, 0), sbuf(2, 0) };
    O.p_spec = cpipe(wgsl_spectrum, es, 3, &l0, &l1);
    WGPUBindGroupEntry gs[3] = { bbuf(0, O.h0), bbuf(1, O.P), bbuf(2, O.Q) };
    O.g_spec = group(l0, gs, 3);
    O.u_g_spec = ugroup(l1, O.u_spec);

    WGPUBindGroupLayoutEntry ef[2] = { sbuf(1, 0), sbuf(2, 0) };
    O.p_fft = cpipe(wgsl_fft, ef, 2, &l0, &l1);
    WGPUBindGroupEntry gf[2] = { bbuf(1, O.P), bbuf(2, O.Q) };
    O.g_fft = group(l0, gf, 2);
    O.u_g_fft[0] = ugroup(l1, O.u_fft[0]);
    O.u_g_fft[1] = ugroup(l1, O.u_fft[1]);

    WGPUBindGroupLayoutEntry en[5] = { sbuf(1, 1), sbuf(2, 1), stex(3), stex(4), sbuf(5, 0) };
    O.p_fin = cpipe(wgsl_finalize, en, 5, &l0, &l1);
    WGPUTextureView d0 = mip_view(O.disp, 0, 1), v0 = mip_view(O.deriv, 0, 1);
    WGPUBindGroupEntry gn[5] = { bbuf(1, O.P), bbuf(2, O.Q), btex(3, d0), btex(4, v0), bbuf(5, O.foam) };
    O.g_fin = group(l0, gn, 5);
    O.u_g_fin = ugroup(l1, O.u_fin);

    WGPUBindGroupLayoutEntry ed[2] = { { 0 }, stex(1) };
    ed[0].binding = 0; ed[0].visibility = WGPUShaderStage_Compute;
    ed[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    ed[0].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    O.p_down = cpipe(wgsl_down, ed, 2, &l0, NULL);
    for (int t = 0; t < 2; t++)
        for (u32 m = 1; m < NM; m++) {
            WGPUTexture tex = t ? O.deriv : O.disp;
            WGPUBindGroupEntry gd[2] = { btex(0, mip_view(tex, m - 1, 1)), btex(1, mip_view(tex, m, 1)) };
            O.g_down[t][m] = group(l0, gd, 2);
        }

    O.ready = O.p_spec && O.p_fft && O.p_fin && O.p_down;
    O.first = 1;
    if (!O.ready) fprintf(stderr, "ac3: ocean: compute pipelines failed; calm water\n");
    return O.ready;
}

void hw_ocean_webgpu_shutdown(void) {
    WGPUBuffer b[] = { O.h0, O.P, O.Q, O.foam, O.idx, O.nodes, O.u_spec, O.u_fft[0], O.u_fft[1], O.u_fin };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) if (b[i]) wgpuBufferDestroy(b[i]);
    if (O.disp) wgpuTextureDestroy(O.disp);
    if (O.deriv) wgpuTextureDestroy(O.deriv);
    memset(&O, 0, sizeof O);
}

int hw_ocean_webgpu_ready(void) { return O.ready; }
WGPUTextureView hw_ocean_webgpu_disp(void) { return O.disp_view; }
WGPUTextureView hw_ocean_webgpu_deriv(void) { return O.deriv_view; }
WGPUSampler hw_ocean_webgpu_sampler(void) { return O.samp; }
WGPUBuffer hw_ocean_webgpu_indices(u32 *count) { *count = O.nidx; return O.idx; }
WGPUBuffer hw_ocean_webgpu_nodes(void) { return O.nodes; }

/* hw_ocean_simulate: spectrum -> FFT (rows, columns) -> finalize -> mips */
void hw_ocean_webgpu_simulate(WGPUCommandEncoder enc, float t, float dt) {
    if (!O.ready) return;
    struct { float patch_m[4]; float time, n, w0, pad; } sp;
    for (int c = 0; c < 4; c++) sp.patch_m[c] = c < NC ? PATCH_M[c] : 1.0f;
    sp.time = t;
    sp.n = (float)ON;
    sp.w0 = (float)(6.283185307179586 / HW_OCEAN_REPEAT_S);
    sp.pad = 0.0f;
    wgpuQueueWriteBuffer(HS.queue, O.u_spec, 0, &sp, sizeof sp);
    struct { float choppy, decay, first, pad; } fp = {
        HW_OCEAN_CHOPPY, (float)exp(-(double)dt / HW_OCEAN_FOAM_LIFE), O.first ? 1.0f : 0.0f, 0.0f };
    wgpuQueueWriteBuffer(HS.queue, O.u_fin, 0, &fp, sizeof fp);
    O.first = 0;

    /* one compute pass: WebGPU makes each dispatch's writes visible to the next one */
    WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, NULL);
    wgpuComputePassEncoderSetPipeline(cp, O.p_spec);
    wgpuComputePassEncoderSetBindGroup(cp, 0, O.g_spec, 0, NULL);
    wgpuComputePassEncoderSetBindGroup(cp, 1, O.u_g_spec, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(cp, ON / 16, ON / 16, NC);
    wgpuComputePassEncoderSetPipeline(cp, O.p_fft);
    wgpuComputePassEncoderSetBindGroup(cp, 0, O.g_fft, 0, NULL);
    for (int col = 0; col < 2; col++) {
        wgpuComputePassEncoderSetBindGroup(cp, 1, O.u_g_fft[col], 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(cp, 1, ON, 2 * NC);
    }
    wgpuComputePassEncoderSetPipeline(cp, O.p_fin);
    wgpuComputePassEncoderSetBindGroup(cp, 0, O.g_fin, 0, NULL);
    wgpuComputePassEncoderSetBindGroup(cp, 1, O.u_g_fin, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(cp, ON / 16, ON / 16, NC);
    wgpuComputePassEncoderSetPipeline(cp, O.p_down);
    for (u32 m = 1; m < NM; m++) {
        u32 n = (ON >> m) < 8 ? 1 : (ON >> m) / 8;
        for (int tx = 0; tx < 2; tx++) {
            wgpuComputePassEncoderSetBindGroup(cp, 0, O.g_down[tx][m], 0, NULL);
            wgpuComputePassEncoderDispatchWorkgroups(cp, n, n, NC);
        }
    }
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
}

