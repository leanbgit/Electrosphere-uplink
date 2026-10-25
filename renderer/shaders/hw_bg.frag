#version 450
#extension GL_GOOGLE_include_directive : require

#define FX_MAP
#include "hw_fx_common.glsl"

layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_z;

bool tiles_cover(vec2 xz) {
    vec2 d = abs(xz - U.cam.xz);
    return U.pal_src.w > 0.0 && max(d.x, d.y) < U.pal_src.w;
}

void main() {
    if (!fx_on(FX_FAR) || U.map.x < 0.5) discard;
    vec3 d = view_ray(gl_FragCoord.xy);
    vec3 c = U.cam.xyz;
    float top = U.map.y;
    float L2 = dot(d.xz, d.xz);
    float k = EARTH_INV_2R * L2;

    float t = U.map.z, prev = t;
    float hit = 0.0;
    for (int i = 0; i < 480; i++) {
        if (t > U.map.w) break;
        float q = c.y + d.y * t - k * t * t;
        float dq = d.y - 2.0 * k * t;
        if (q < top) {
            if (dq <= 0.0) break;
            prev = t;
            t += max((top - q) / dq, 32.0);
            continue;
        }
        vec2 xz = c.xz + d.xz * t;
        if (q >= ground_y(xz)) {
            uint fl = ground_flags(xz);
            if ((fl & 1u) != 0u && U.ocean[1].w > 0.5) discard;
            if (tiles_cover(xz) && (fl & 1u) == 0u) discard;
            float lo = prev, hi = t;
            for (int j = 0; j < 8; j++) {
                float m = 0.5 * (lo + hi);
                float qm = c.y + d.y * m - k * m * m;
                if (qm >= ground_y(c.xz + d.xz * m)) hi = m; else lo = m;
            }
            hit = 0.5 * (lo + hi);
            break;
        }
        prev = t;
        t += max(40.0, t * 0.012);
    }
    if (hit <= 0.0) discard;

    vec2 xz = c.xz + d.xz * hit;
    vec4 gc = ground_color(xz);
    uint flags = uint(gc.a * 255.0 + 0.5);
    vec3 col = gc.rgb;
    if ((flags & 1u) == 0u) {
        float e = 64.0;
        float gx = (ground_y(xz + vec2(e, 0.0)) - ground_y(xz - vec2(e, 0.0))) / (2.0 * e);
        float gz = (ground_y(xz + vec2(0.0, e)) - ground_y(xz - vec2(0.0, e))) / (2.0 * e);
        vec3 n = normalize(vec3(gx, -1.0, gz));
        float flat_l = max(-U.sun.y, 0.05);
        float l = max(dot(n, U.sun.xyz), 0.0);
        col *= clamp(0.8 + 0.6 * (l - flat_l), 0.45, 1.35);
    } else {
        col = U.water.rgb;
    }
    o_col = vec4(game_fog(col, hit), 0.0);
    o_z = hit;
    gl_FragDepth = clamp(NEAR_Z / hit, 0.0, 1.0);
}
