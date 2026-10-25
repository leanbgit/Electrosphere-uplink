#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 2) uniform sampler2D t_z;
layout(set = 0, binding = 3) uniform sampler2D t_cloud;

layout(location = 0) out float o_vis;

void main() {
    vec3 s = sun_screen();
    if (s.z < 0.5 || U.sun.w <= 0.0 || !fx_on(FX_SUN)) { o_vis = 0.0; return; }
    vec2 lo = U.ofs.zw * U.proj.z, hi = (U.ofs.zw + U.area.xy) * U.proj.z - 1.0;
    float rpx = SUN_R * U.proj.x * U.proj.z;
    float vis = 0.0;
    const int N = 32;
    for (int i = 0; i < N; i++) {
        float r = sqrt((float(i) + 0.5) / float(N)) * rpx;
        float a = float(i) * 2.39996;
        vec2 p = s.xy + vec2(cos(a) * U.proj.y, sin(a)) * r;
        if (any(lessThan(p, lo)) || any(greaterThan(p, hi))) { vis += 1.0; continue; }
        float z = texelFetch(t_z, ivec2(p), 0).r;
        if (z < Z_SKY * 0.5) continue;
        vis += fx_on(FX_CLOUDS) ? texelFetch(t_cloud, ivec2(p * 0.5), 0).a : 1.0;
    }
    vis /= float(N);
    vec2 c = (lo + hi) * 0.5, half_ = (hi - lo) * 0.5;
    vec2 off = max(abs(s.xy - c) - half_, vec2(0.0)) / half_;
    vis *= 1.0 - smoothstep(0.0, 0.35, max(off.x, off.y));
    o_vis = vis;
}
