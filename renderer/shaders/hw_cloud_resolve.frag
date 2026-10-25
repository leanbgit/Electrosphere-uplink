#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 4)  uniform sampler2D t_cloudz;
layout(set = 0, binding = 16) uniform sampler2D t_raw;
layout(set = 0, binding = 17) uniform sampler2D t_hist;

layout(location = 0) out vec4 o_cloud;

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(t_raw, hp, 0);
    float w = U.cloud_tc.y;
    if (w <= 0.0) { o_cloud = c; return; }

    ivec2 lo = ivec2(U.ofs.zw * U.proj.z) / 2;
    ivec2 hi = max(ivec2((U.ofs.zw + U.area.xy) * U.proj.z) / 2 - 1, lo);
    vec4 m1 = vec4(0.0), m2 = vec4(0.0), mn = c, mx = c;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec4 v = texelFetch(t_raw, clamp(hp + ivec2(i, j), lo, hi), 0);
            m1 += v;
            m2 += v * v;
            mn = min(mn, v);
            mx = max(mx, v);
        }
    m1 /= 9.0;
    vec4 sd = sqrt(max(m2 / 9.0 - m1 * m1, vec4(0.0)));
    vec4 blo = max(mn, m1 - 1.5 * sd), bhi = min(mx, m1 + 1.5 * sd);

    float tc = texelFetch(t_cloudz, hp, 0).g;
    vec3 ray = view_ray(vec2(hp) * 2.0 + 1.0);
    vec3 v = tc >= Z_SKY * 0.5 ? U.prev_wtv * ray
                               : U.prev_wtv * (U.cam.xyz + ray * tc - U.prev_cam.xyz);
    if (v.z <= 1e-3) { o_cloud = c; return; }
    vec2 s = vec2(U.prev_proj.x * U.prev_proj.y * v.x / v.z, U.prev_proj.x * v.y / v.z) + U.prev_proj.zw;
    vec2 h = s * U.proj.z * 0.5;
    vec2 plo = U.prev_area.xy * U.proj.z * 0.5 + 0.5;
    vec2 phi = (U.prev_area.zw + 1.0) * U.proj.z * 0.5 - 0.5;
    if (any(lessThan(h, plo)) || any(greaterThan(h, phi))) { o_cloud = c; return; }
    vec4 hist = texture(t_hist, h / vec2(textureSize(t_hist, 0)));
    o_cloud = mix(c, clamp(hist, blo, bhi), w);
}
