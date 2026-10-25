#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 2)  uniform sampler2D t_z;
layout(set = 0, binding = 18) uniform sampler3D t_noise;
layout(set = 0, binding = 19) uniform sampler2D t_weather;

layout(location = 0) out vec4 o_cloud;
layout(location = 1) out vec2 o_z;

const float SHAPE_TEXEL   = 16000.0 / 64.0;
const float DETAIL_TEXEL  = 3200.0 / 64.0;
const float WEATHER_TEXEL = 110000.0 / 256.0;

float remap(float v, float a, float b, float c, float d) {
    return c + (v - a) / max(b - a, 1e-4) * (d - c);
}

float lod_for(float fp, float texel) {
    return log2(max(fp / texel, 1.0));
}

float coverage_at(vec2 xz, float fp) {
    float w = textureLod(t_weather, (xz + U.wind.xy) / 110000.0, lod_for(fp, WEATHER_TEXEL)).r;
    float c = U.cloud.z;
    return smoothstep(1.0 - c - 0.12, 1.0 - c + 0.28, w);
}

float density(vec3 p, bool detail, float fp) {
    float hf = (U.cloud.x - p.y) / (U.cloud.x - U.cloud.y);
    if (hf <= 0.0 || hf >= 1.0) return 0.0;
    vec2 wxz = p.xz + U.wind.xy;
    float cov = coverage_at(p.xz, fp);
    if (cov <= 0.01) return 0.0;
    float prof = smoothstep(0.0, 0.1, hf) * smoothstep(1.0, 0.45, hf);
    vec4 n = textureLod(t_noise, vec3(wxz.x, p.y * 1.1, wxz.y) / 16000.0, lod_for(fp, SHAPE_TEXEL));
    float shape = remap(n.r, n.g * 0.6 - 0.6, 1.0, 0.0, 1.0);
    float d = remap(shape * prof, 1.0 - cov, 1.0, 0.0, 1.0);
    if (d <= 0.0) return 0.0;
    if (detail) {
        vec4 dn = textureLod(t_noise, vec3(wxz.x + time_s() * 6.0, p.y, wxz.y) / 3200.0,
                             lod_for(fp, DETAIL_TEXEL));
        float det = dn.g * 0.625 + dn.b * 0.25 + dn.a * 0.125;
        d = remap(d, det * mix(0.45, 0.2, hf), 1.0, 0.0, 1.0);
    }
    return clamp(d, 0.0, 1.0) * U.cloud.w;
}

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 fp = hp * 2;
    float z = Z_SKY;
    for (int i = 0; i < 4; i++) {
        float zz = scene_depth(texelFetch(t_z, fp + ivec2(i & 1, i >> 1), 0).r);
        z = min(z, zz);
    }
    vec3 d = view_ray(vec2(fp) + 1.0);
    float dl = length(d);
    vec3 dir = d / dl;
    vec3 c = U.cam.xyz;
    float tscene = z <= 0.0 ? 0.0 : (z >= Z_SKY * 0.5 ? 1.0e7 : z);

    float ta, tb;
    if (abs(d.y) < 1e-7) {
        if (c.y > U.cloud.y && c.y < U.cloud.x) { ta = 0.0; tb = 1.0e7; }
        else { o_cloud = vec4(0.0, 0.0, 0.0, 1.0); o_z = vec2(z, Z_SKY); return; }
    } else {
        float t0 = (U.cloud.y - c.y) / d.y, t1 = (U.cloud.x - c.y) / d.y;
        ta = max(min(t0, t1), 0.0);
        tb = max(t0, t1);
    }
    tb = min(min(tb, tscene), 200000.0 / dl);
    if (tb <= ta) { o_cloud = vec4(0.0, 0.0, 0.0, 1.0); o_z = vec2(z, Z_SKY); return; }

    const int STEPS = 128;
    float span = (tb - ta) * dl;
    float grow = mix(0.03, 0.07, smoothstep(25000.0, 120000.0, span));
    float step_max = mix(3000.0, 6000.0, smoothstep(25000.0, 120000.0, span));
    float jitter = fract(ign(vec2(hp)) + 0.618034 * U.cloud_tc.x);
    float pix = 2.0 / (U.proj.x * U.proj.z);
    float mu = dot(dir, U.sun.xyz);
    float phase = mix(hg(mu, 0.45), hg(mu, -0.15), 0.3) * 4.0 * PI;
    vec3 sunc = U.sun_col.rgb * U.sun.w;
    vec3 amb_top = mix(sky_at(1.1), vec3(luma(sky_at(1.1))), 0.3) * 1.15;
    vec3 amb_bot = mix(U.fog.rgb, sky_at(0.1), 0.5) * 0.8;
    const float SIGMA = 0.0045;

    float T = 1.0;
    vec3 S = vec3(0.0);
    float tw = 0.0, wsum = 0.0;
    float t = ta;
    float dt0 = clamp(ta * dl * grow, 40.0, step_max) / dl;
    t += dt0 * jitter;
    for (int i = 0; i < STEPS && t < tb; i++) {
        float dt = clamp(t * dl * grow, 40.0, step_max) / dl;
        vec3 p = c + d * t;
        float foot = max(pix * t, dt * dl * 0.15);
        float dens = density(p, true, foot);
        if (dens > 0.003) {
            float od = 0.0, ls = 80.0;
            for (int j = 0; j < 5; j++) {
                od += density(p + U.sun.xyz * (ls * 0.75), false, max(foot, ls * 0.5)) * ls;
                ls *= 2.0;
            }
            float tl = exp(-od * SIGMA) * 0.8 + exp(-od * SIGMA * 0.2) * 0.2;
            float powder = 1.0 - exp(-dens * 160.0 * SIGMA * 2.0);
            float hf = clamp((U.cloud.x - p.y) / (U.cloud.x - U.cloud.y), 0.0, 1.0);
            vec3 lum = min(sunc * tl * phase * mix(0.55, 1.0, powder), vec3(2.2)) +
                       mix(amb_bot, amb_top, hf) * 0.85;
            float seg = min(dt, tb - t);
            float st = exp(-dens * SIGMA * seg * dl);
            S += T * lum * (1.0 - st);
            tw += T * (1.0 - st) * t;
            wsum += T * (1.0 - st);
            T *= st;
            if (T < 0.01) break;
        }
        t += dt;
    }
    if (wsum > 0.0) {
        float dist = tw / wsum * dl;
        float fade = exp(-dist / 85000.0);
        vec3 haze = mix(sky_at(0.03), U.fog.rgb, 0.3);
        S = S * fade + haze * (1.0 - T) * (1.0 - fade);
    }
    o_cloud = vec4(S, T);
    o_z = vec2(z, wsum > 0.0 ? tw / wsum : ta);
}
