#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 11) uniform sampler2D t_hdr;
layout(set = 0, binding = 12) uniform sampler2D t_bloom;
layout(set = 0, binding = 13) uniform sampler2D t_sunvis;

layout(location = 0) out vec4 o_col;

vec3 tonemap(vec3 c) {
    c = max(c, vec3(0.0));
    float m = max(max(c.r, c.g), c.b);
    if (m <= 0.8) return c;
    float mm = 0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2));
    return mix(c * (mm / m), vec3(mm), clamp((m - 1.0) / 8.0, 0.0, 0.85));
}

float ghost(vec2 d, float r) {
    float l = length(d);
    return (1.0 - smoothstep(r * 0.8, r, l)) * (0.55 + 0.45 * smoothstep(r * 0.3, r * 0.95, l));
}

vec3 lens_flare(vec2 p) {
    float vis = texelFetch(t_sunvis, ivec2(0), 0).r;
    vec3 s = sun_screen();
    if (vis <= 0.002 || s.z < 0.5 || U.hdr.w <= 0.0) return vec3(0.0);
    float S = U.proj.z;
    vec2 lo = U.ofs.zw * S, size = U.area.xy * S;
    vec2 centre = lo + size * 0.5;
    vec2 axis = centre - s.xy;
    float unit = size.y;
    vec2 k = vec2(1.0 / U.proj.y, 1.0);
    vec3 tint = sun_tint();
    vec3 acc = vec3(0.0);
    const float T[7] = float[](0.28, 0.52, 0.74, 1.12, 1.38, 1.62, 1.96);
    const float R[7] = float[](0.025, 0.06, 0.018, 0.1, 0.04, 0.14, 0.055);
    const vec3 C[7] = vec3[](vec3(1.0, 0.6, 0.3), vec3(0.4, 0.8, 0.5), vec3(0.9, 0.9, 1.0),
                             vec3(0.35, 0.5, 1.0), vec3(1.0, 0.45, 0.6), vec3(0.3, 0.6, 0.9),
                             vec3(0.8, 0.7, 0.4));
    for (int i = 0; i < 7; i++) {
        vec2 g = s.xy + axis * T[i];
        acc += C[i] * ghost((p - g) * k, R[i] * unit) * 0.045;
    }
    vec2 dc = (p - centre) * k;
    float ring = exp(-pow((length(dc) - length(axis * k) * 0.95) / (unit * 0.03), 2.0));
    acc += tint * ring * 0.02;
    vec2 ds = (p - s.xy) * k;
    float l = length(ds) / unit;
    float ang = atan(ds.y, ds.x);
    float star = pow(abs(cos(ang * 3.0 + 0.4)), 60.0) + 0.5 * pow(abs(cos(ang * 3.0 + 1.45)), 90.0);
    acc += tint * star * exp(-l / 0.09) * 0.35;
    acc += tint * exp(-l / 0.035) * 0.25;
    float day = 1.0 - sun_moon();
    return acc * vis * U.hdr.w * clamp(U.sun.w, 0.0, 1.2) * day;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 h = p - ivec2(U.ofs.zw * U.proj.z);
    vec3 c = texelFetch(t_hdr, h, 0).rgb;
    vec2 buv = (vec2(h) + 0.5) / U.hdr.xy;
    c += texture(t_bloom, buv).rgb * U.hdr.z;
    c = tonemap(c);
    c += lens_flare(vec2(p) + 0.5);
    o_col = vec4(clamp(c, 0.0, 1.0), 0.0);
}
