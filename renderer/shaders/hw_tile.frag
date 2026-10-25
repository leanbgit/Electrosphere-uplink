#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D t_vram;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_col;
layout(location = 2) flat in uint v_tex;
layout(location = 3) in vec3 v_avg;
layout(location = 4) in float v_z;

layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_z;

int page_x, page_y, cx, cy;
uint depth;

uint vram16(int x, int y) {
    ivec2 p = ivec2(x & 1023, y & 511) * int(U.proj.z);
    uvec4 c = uvec4(texelFetch(t_vram, p, 0) * 255.0 + 0.5);
    return (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | (c.a > 127u ? 0x8000u : 0u);
}

uint texel(vec2 uv) {
    uint u = uint(int(floor(uv.x))) & 255u, v = uint(int(floor(uv.y))) & 255u;
    if (depth == 0u) {
        uint w = vram16(page_x + int(u >> 2), page_y + int(v));
        return vram16(cx + int((w >> ((u & 3u) * 4u)) & 15u), cy);
    }
    if (depth == 1u) {
        uint w = vram16(page_x + int(u >> 1), page_y + int(v));
        return vram16(cx + int((w >> ((u & 1u) * 8u)) & 255u), cy);
    }
    return vram16(page_x + int(u), page_y + int(v));
}

vec3 rgb5(uint t) {
    return vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u));
}

void main() {
    uint tp = v_tex >> 16, clut = v_tex & 0xFFFFu;
    page_x = int(tp & 15u) * 64;
    page_y = int((tp >> 4) & 1u) * 256;
    depth = (tp >> 7) & 3u;
    cx = int(clut & 63u) * 16;
    cy = int((clut >> 6) & 511u);
    if (U.pal.z > 0.0 && float(cx) >= U.pal.x && float(cx) < U.pal.x + U.pal.z &&
        float(cy) >= U.pal.y && float(cy) < U.pal.y + U.pal.w) {
        cx += int(U.pal_src.x - U.pal.x);
        cy += int(U.pal_src.y - U.pal.y);
    }

    vec2 dx = dFdx(v_uv), dy = dFdy(v_uv);
    float fw = max(length(dx), length(dy));
    vec3 t5;
    if (fw < 1.25) {
        uint t = texel(v_uv);
        if (t == 0u) discard;
        t5 = rgb5(t);
    } else {
        vec3 acc = vec3(0.0);
        float n = 0.0, clear = 0.0;
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                vec2 o = (vec2(float(i), float(j)) - 1.0) / 3.0;
                uint t = texel(v_uv + dx * o.x + dy * o.y);
                if (t == 0u) { clear += 1.0; continue; }
                acc += rgb5(t);
                n += 1.0;
            }
        if (n < 0.5 || clear > 4.5) discard;
        t5 = acc / n;
    }
    vec3 rgb = min(t5 * (v_col * 255.0) / (128.0 * 31.0), vec3(1.0));
    rgb = mix(rgb, v_avg, smoothstep(10.0, 24.0, fw));
    rgb = game_fog(rgb, v_z);
    o_col = vec4(rgb, 0.0);
    o_z = v_z;
}
