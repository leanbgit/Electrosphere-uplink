#version 450

layout(set = 0, binding = 0) uniform sampler2D vram;
layout(set = 0, binding = 1) uniform sampler2D zsnap;

layout(set = 0, binding = 2) uniform sampler2D order_snap;

layout(push_constant) uniform Push {
    vec2 inv_size;
    int  scale;
    int  pass;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 2) flat in uvec4 v_info;
layout(location = 3) in float v_z;

layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_z;
layout(location = 2) out float o_order;

const uint F_TEXTURED = 1u;
const uint F_RAW      = 2u;
const uint F_MASK_OR  = 4u;
const uint F_COPY     = 8u;
const uint F_FLAT     = 16u;
const uint F_CHECK    = 32u;
const uint F_ZTEST    = 64u;

uint vram16(int x, int y) {
    ivec2 p = ivec2(x & 1023, y & 511) * pc.scale;
    uvec4 c = uvec4(texelFetch(vram, p, 0) * 255.0 + 0.5);
    return (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) |
           (c.a > 127u ? 0x8000u : 0u);
}

void main() {
    uint flags = v_info.w;
    o_order = float(flags >> 7);
    if ((flags & F_ZTEST) != 0u && o_order > 0.0) {
        float later = texelFetch(order_snap, ivec2(gl_FragCoord.xy), 0).r;
        if (later > o_order) discard;
    }

    if ((flags & F_ZTEST) != 0u && v_z > 0.0) {
        float zs = abs(texelFetch(zsnap, ivec2(gl_FragCoord.xy), 0).r);
        if (zs > 0.0 && zs < 5.0e8 && zs < v_z * 0.985 - 4.0) discard;
    }

    if ((flags & F_CHECK) != 0u &&
        texelFetch(vram, ivec2(gl_FragCoord.xy), 0).a > 0.5)
        discard;

    if ((flags & F_COPY) != 0u) {
        ivec2 size = ivec2(1024, 512) * pc.scale;
        ivec2 s = ivec2(floor(v_uv));
        s = ((s % size) + size) % size;
        vec4 c = texelFetch(vram, s, 0);
        o_col = vec4(c.rgb, ((flags & F_MASK_OR) != 0u || c.a > 0.5) ? 1.0 : 0.0);
        o_z = 0.0;
        return;
    }

    vec3 rgb;
    bool mbit = false;
    if ((flags & F_TEXTURED) != 0u) {
        uint tp = v_info.x;
        int page_x = int(tp & 15u) * 64;
        int page_y = int((tp >> 4) & 1u) * 256;
        uint depth = (tp >> 7) & 3u;
        uint clut = v_info.y;
        int clut_x = int(clut & 63u) * 16;
        int clut_y = int((clut >> 6) & 511u);

        uint tw = v_info.z;
        int fu = int(floor(v_uv.x)), fv = int(floor(v_uv.y));
        if ((tp & 0x8000u) != 0u) {      // the texels the console would sample (hw.c: hw_poly)
            fu = clamp(fu, int((tp >> 16) & 255u), int(tp >> 24));
            fv = clamp(fv, int((clut >> 16) & 255u), int(clut >> 24));
        }
        uint u = uint(fu) & 255u;
        uint v = uint(fv) & 255u;
        u = (u & (tw & 255u)) | ((tw >> 8) & 255u);
        v = (v & ((tw >> 16) & 255u)) | ((tw >> 24) & 255u);

        uint t;
        if (depth == 0u) {
            uint w = vram16(page_x + int(u >> 2), page_y + int(v));
            t = vram16(clut_x + int((w >> ((u & 3u) * 4u)) & 15u), clut_y);
        } else if (depth == 1u) {
            uint w = vram16(page_x + int(u >> 1), page_y + int(v));
            t = vram16(clut_x + int((w >> ((u & 1u) * 8u)) & 255u), clut_y);
        } else {
            t = vram16(page_x + int(u), page_y + int(v));
        }
        if (t == 0u) discard;
        mbit = (t & 0x8000u) != 0u;
        if (pc.pass == 1 && mbit) discard;
        if (pc.pass == 2 && !mbit) discard;

        vec3 t5 = vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u));
        if ((flags & F_RAW) != 0u)
            rgb = t5 / 31.0;
        else
            rgb = min(t5 * (v_col.rgb * 255.0) / (128.0 * 31.0), vec3(1.0));
    } else if ((flags & F_FLAT) != 0u) {
        rgb = floor(v_col.rgb * 255.0 / 8.0 + 0.001) / 31.0;
    } else {
        rgb = v_col.rgb;
    }
    o_col = vec4(rgb, (mbit || (flags & F_MASK_OR) != 0u) ? 1.0 : 0.0);
    o_z = v_z;
}
