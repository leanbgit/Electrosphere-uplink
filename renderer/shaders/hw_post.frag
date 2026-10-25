#version 450
#extension GL_GOOGLE_include_directive : require

#define FX_MAP
#define FX_OCEAN_TEX
#include "hw_fx_common.glsl"

layout(set = 0, binding = 1)  uniform sampler2D t_color;
layout(set = 0, binding = 2)  uniform sampler2D t_z;
layout(set = 0, binding = 3)  uniform sampler2D t_cloud;
layout(set = 0, binding = 4)  uniform sampler2D t_cloudz;
layout(set = 0, binding = 9)  uniform sampler3D t_noise;
layout(set = 0, binding = 10) uniform sampler2D t_weather;

layout(location = 0) out vec4 o_col;

ivec2 lo_px, hi_px;

float depth_at(ivec2 p) {
    return scene_depth(texelFetch(t_z, clamp(p, lo_px, hi_px), 0).r);
}

bool is_geometry(float z) { return z > 0.0 && z < Z_SKY * 0.5; }

vec3 world_at(ivec2 p, float z) {
    return U.cam.xyz + view_ray(vec2(p) + 0.5) * z;
}

float ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float cloud_shadow(vec3 p) {
    if (!fx_on(FX_CLOUDS) || U.sun.y > -0.02) return 1.0;
    float mid = mix(U.cloud.x, U.cloud.y, 0.4);
    float t = (mid - p.y) / U.sun.y;
    if (t <= 0.0) return 1.0;
    vec2 q = p.xz + U.sun.xz * t + U.wind.xy;
    float cov = smoothstep(1.0 - U.cloud.z - 0.12, 1.0 - U.cloud.z + 0.28,
                           texture(t_weather, q / 110000.0).r);
    float n = texture(t_noise, vec3(q.x, mid * 1.6, q.y) / 16000.0).r;
    float d = clamp((n - (1.0 - cov)) / max(cov, 0.05), 0.0, 1.0);
    return 1.0 - 0.55 * smoothstep(0.05, 0.5, d * cov);
}

vec3 haze_colour(vec3 dir) {
    float e = clamp(elevation(dir), 0.0, 0.25);
    vec3 c = mix(sky_at(e), U.fog.rgb, 0.35);
    float mu = dot(dir, U.sun.xyz);
    c += U.sun_col.rgb * U.sun.w * hg(mu, 0.6) * 0.1;
    return c;
}

vec3 haze(vec3 col, vec3 dir, float dist, float h0, float h1) {
    float Hs = 7000.0;
    h0 = max(h0, 0.0); h1 = max(h1, 0.0);
    float d0 = U.fog.w;
    float dh = h1 - h0;
    float od;
    if (abs(dh) < 1.0) od = d0 * exp(-h0 / Hs) * dist;
    else od = d0 * dist * Hs * (exp(-h0 / Hs) - exp(-h1 / Hs)) / dh;
    float T = exp(-od);
    return col * T + haze_colour(dir) * (1.0 - T);
}

vec3 atmosphere(vec3 col, vec3 dir, float dist, vec3 wp) {
    return haze(col, dir, dist, altitude(U.cam.xyz), altitude(wp));
}

vec2 ocean_origin(vec2 p, out float height) {
    float alt = max(altitude(U.cam.xyz), 1.0);
    vec2 x0 = p;
    vec3 D = vec3(0.0);
    for (int i = 0; i < 3; i++) {
        vec2 rel = x0 - U.cam.xz;
        D = ocean_surface_disp(x0, ocean_mesh_fp(sqrt(dot(rel, rel) + alt * alt)));
        x0 = p - D.xz;
    }
    height = -D.y / U.ocean[1].y;
    return x0;
}

float oc_lost(int c, float lod) {
    lod = clamp(lod, 0.0, 8.0);
    int i0 = int(floor(lod));
    int i1 = min(i0 + 1, 8);
    int a = c * 9 + i0, b = c * 9 + i1;
    return mix(U.ocean[2 + a / 4][a % 4], U.ocean[2 + b / 4][b % 4], lod - float(i0));
}

vec4 ocean_surface(vec2 x0, float fp) {
    vec2 s = vec2(0.0);
    float squeeze = 0.0, lost = 0.0;
    for (int c = 0; c < 3; c++) {
        float lod = oc_lod(fp, c);
        vec4 d = textureLod(t_oderiv, oc_uv(x0, c), lod);
        s += oc_from(d.xy, c);
        squeeze += d.w * (c == 2 ? 0.6 : 1.0);
        lost += oc_lost(c, lod);
    }
    return vec4(s, squeeze, U.ocean[1].x + lost);
}

vec4 cloud_reflect(vec3 p, vec3 r) {
    if (!fx_on(FX_CLOUDS) || r.y > -0.01) return vec4(0.0, 0.0, 0.0, 1.0);
    float mid = mix(U.cloud.x, U.cloud.y, 0.3);
    float t = (mid - p.y) / r.y;
    if (t <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec2 q = p.xz + r.xz * t + U.wind.xy;
    float cov = smoothstep(1.0 - U.cloud.z - 0.12, 1.0 - U.cloud.z + 0.28,
                           texture(t_weather, q / 110000.0).r);
    float n = texture(t_noise, vec3(q.x, mid * 1.1, q.y) / 16000.0).r;
    float d = clamp((n - (1.0 - cov)) / max(cov, 0.05), 0.0, 1.0);
    float a = smoothstep(0.02, 0.4, d * cov) * 0.85 * (1.0 - smoothstep(80000.0, 200000.0, t));
    vec3 base = mix(U.fog.rgb, sky_at(0.3), 0.4) * 0.95 +
                U.sun_col.rgb * U.sun.w * max(-U.sun.y, 0.0) * 0.25;
    return vec4(base * a, 1.0 - a);
}

vec2 to_screen(vec3 q) {
    return (vec2(U.proj.x * U.proj.y * q.x, U.proj.x * q.y) / q.z + U.ofs.xy) * U.proj.z;
}

vec4 ssr(vec3 wp, vec3 r, float alpha) {
    if (alpha > 0.4) return vec4(0.0);
    vec3 o = U.world_to_view * (wp - U.cam.xyz);
    vec3 d = U.world_to_view * r;
    float t = max(o.z * 0.01, 6.0), prev = 0.0;
    for (int i = 0; i < 40; i++) {
        vec3 q = o + d * t;
        if (q.z < 24.0) break;
        ivec2 px = ivec2(to_screen(q));
        if (any(lessThan(px, lo_px)) || any(greaterThan(px, hi_px))) break;
        float zb = texelFetch(t_z, px, 0).r;
        if (zb > 0.0 && zb < Z_SKY * 0.5 && q.z > zb &&
            q.z - zb < max(zb * 0.025, 60.0) + (t - prev) * max(d.z, 0.0)) {
            float lo = prev, hi = t;
            for (int j = 0; j < 5; j++) {
                float m = 0.5 * (lo + hi);
                vec3 qm = o + d * m;
                float zm = qm.z < 24.0 ? 0.0 : texelFetch(t_z, clamp(ivec2(to_screen(qm)), lo_px, hi_px), 0).r;
                if (zm > 0.0 && zm < Z_SKY * 0.5 && qm.z > zm) hi = m; else lo = m;
            }
            vec3 qh = o + d * hi;
            ivec2 ph = clamp(ivec2(to_screen(qh)), lo_px, hi_px);
            vec3 c = texelFetch(t_color, ph, 0).rgb;
            if (fx_on(FX_ATMO))
                c = haze(c, r, hi, altitude(wp), altitude(U.cam.xyz + U.view_to_world * qh));
            vec2 e = min(vec2(ph - lo_px), vec2(hi_px - ph)) / max(vec2(hi_px - lo_px), vec2(1.0));
            float fade = smoothstep(0.0, 0.12, min(e.x, e.y)) * (1.0 - smoothstep(0.15, 0.35, alpha));
            return vec4(c, fade);
        }
        prev = t;
        t *= 1.18;
    }
    return vec4(0.0);
}

vec3 ocean(vec3 wc, vec3 wp, vec2 x0, float height, vec3 dir, float dist, float shadow, float fp) {
    fp = min(max(fp, dist / (U.proj.x * U.proj.z)), dist * 0.5);
    vec4 sf = ocean_surface(x0, fp);
    float gust = 0.75 + 0.5 * texture(t_weather, x0 / 53000.0 + vec2(0.37, 0.11)).r;
    vec2 slope = sf.xy * gust;
    vec3 n = normalize(vec3(-slope.x, -1.0, -slope.y));
    float alpha = clamp(sqrt(sf.w) * gust, 0.03, 0.7);
    vec3 v = -dir;
    float ndv = dot(n, v);
    if (ndv < 0.05) { n = normalize(n + v * (0.05 - ndv)); ndv = max(dot(n, v), 0.02); }
    float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

    vec3 r = reflect(dir, n);
    if (r.y > -0.004) r = normalize(vec3(r.x, -0.004, r.z));
    vec3 refl = sky_at(elevation(r));
    vec4 cr = cloud_reflect(wc, r);
    refl = refl * cr.a + cr.rgb;
    vec4 sr = ssr(wp, r, alpha);
    refl = mix(refl, sr.rgb, sr.a);

    vec3 L = U.sun.xyz;
    vec3 h = normalize(L + v);
    float ndl = max(dot(n, L), 0.0), ndh = max(dot(n, h), 0.0);
    float kg = alpha * 0.5;
    float G = ndl / (ndl * (1.0 - kg) + kg) * ndv / (ndv * (1.0 - kg) + kg);
    float Fh = 0.02 + 0.98 * pow(1.0 - max(dot(h, v), 0.0), 5.0);
    float spec = ggx(ndh, alpha) * G * Fh / (4.0 * ndv);
    float g = 6.0 * U.sun.w * spec;
    vec3 glint = sun_tint() * (g / (1.0 + g / 4.0)) * shadow;

    float sunh = max(-U.sun.y, 0.0);
    vec3 body = U.water.rgb * (0.5 + 0.6 * sunh) * mix(0.72, 1.0, shadow);
    float crest = clamp(height / 1.6, 0.0, 1.0);
    float back = pow(clamp(dot(normalize(dir.xz + 1e-5), normalize(L.xz + 1e-5)), 0.0, 1.0), 3.0);
    float near = 1.0 - smoothstep(4000.0, 20000.0, dist);
    body += U.water.rgb * vec3(0.35, 1.1, 0.95) *
            (crest * back * (0.35 + 0.65 * (1.0 - sunh)) * U.sun.w * shadow * 0.9 * near);
    vec3 col = body * (1.0 - F) + refl * F + glint;

    float foam = smoothstep(0.28, 0.45, sf.z * gust * gust);
    float fd = texture(t_noise, vec3(x0 / 260.0, time_s() * 0.015)).g;
    foam *= mix(0.45 + 1.1 * fd, 1.0, smoothstep(8.0, 40.0, fp));
    foam = max(clamp(foam, 0.0, 1.0), 0.03 * gust * smoothstep(60.0, 600.0, fp));
    float coast = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(i == 0 ? 90.0 : i == 1 ? -90.0 : 0.0, i == 2 ? 90.0 : i == 3 ? -90.0 : 0.0);
        if ((ground_flags(wc.xz + o) & 1u) == 0u) coast += 0.25;
    }
    if (coast > 0.0) {
        float fn = texture(t_noise, vec3(wc.x, time_s() * 20.0, wc.z) / 700.0).g;
        float band = smoothstep(0.45, 0.8, fn + 0.25 * sin(time_s() * 0.9 + wc.x * 0.004));
        foam = max(foam, coast * band * 0.6 * (1.0 - smoothstep(20000.0, 60000.0, dist)));
    }
    vec3 foam_col = vec3(0.9, 0.93, 0.95) *
                    (U.sun_col.rgb * U.sun.w * (0.25 + 0.75 * sunh) * 0.85 * shadow + sky_at(0.8) * 0.55);
    return mix(col, foam_col, foam);
}

vec3 normal_at(ivec2 p, float z, vec3 wp) {
    float zl = depth_at(p - ivec2(1, 0)), zr = depth_at(p + ivec2(1, 0));
    float zu = depth_at(p - ivec2(0, 1)), zd = depth_at(p + ivec2(0, 1));
    vec3 px = abs(zr - z) < abs(zl - z) && is_geometry(zr)
            ? world_at(p + ivec2(1, 0), zr) - wp : wp - world_at(p - ivec2(1, 0), zl);
    vec3 py = abs(zd - z) < abs(zu - z) && is_geometry(zd)
            ? world_at(p + ivec2(0, 1), zd) - wp : wp - world_at(p - ivec2(0, 1), zu);
    vec3 n = normalize(cross(py, px));
    if (dot(n, wp - U.cam.xyz) > 0.0) n = -n;
    return n;
}

float ambient_occlusion(ivec2 p, float z, vec3 wp, vec3 n) {
    float rw = 140.0;
    float rpx = clamp(rw * U.proj.x * U.proj.z / z, 2.0, 40.0);
    float occ = 0.0;
    float rot = hash12(vec2(p)) * 6.2831;
    for (int i = 0; i < 8; i++) {
        float a = rot + float(i) * (6.2831 / 8.0);
        float s = rpx * (0.35 + 0.65 * fract(float(i) * 0.618 + 0.3));
        ivec2 q = p + ivec2(cos(a) * s, sin(a) * s);
        float zq = depth_at(q);
        if (!is_geometry(zq)) continue;
        vec3 d = world_at(q, zq) - wp;
        float l = length(d);
        if (l < 1.0) continue;
        occ += clamp(dot(n, d / l) - 0.15, 0.0, 1.0) * (1.0 - smoothstep(rw, rw * 3.0, l));
    }
    return 1.0 - occ / 8.0;
}

vec3 indirect(vec3 col, ivec2 p, float z, vec3 wp, float shadow) {
    vec3 n = normal_at(p, z, wp);
    float ao = ambient_occlusion(p, z, wp, n);
    float up = clamp(-n.y, -1.0, 1.0);
    vec3 sky = sky_at(0.9);
    vec3 ground = mix(U.water.rgb, U.fog.rgb, 0.5) * 1.2;
    vec3 fill = mix(ground, sky, up * 0.5 + 0.5);
    vec3 tint = fill / max(luma(fill), 0.05);
    float sun_face = max(dot(n, U.sun.xyz), 0.0);
    vec3 lit = col * mix(vec3(1.0), tint, 0.12);
    lit *= mix(1.0, ao, 0.7);
    lit *= 0.94 + 0.12 * sun_face;
    lit *= shadow;
    return lit;
}

vec4 clouds_at(ivec2 p, float z) {
    vec2 hc = vec2(p) * 0.5 - 0.25;
    ivec2 b = ivec2(floor(hc));
    vec2 f = hc - vec2(b);
    ivec2 clo = lo_px / 2, chi = hi_px / 2;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int j = -1; j <= 2; j++)
        for (int i = -1; i <= 2; i++) {
            ivec2 q = clamp(b + ivec2(i, j), clo, chi);
            vec2 dd = vec2(float(i), float(j)) - f;
            float bw = max(0.0, 1.0 - abs(dd.x) * 0.5) * max(0.0, 1.0 - abs(dd.y) * 0.5);
            float zc = texelFetch(t_cloudz, q, 0).r;
            float sim;
            if (z >= Z_SKY * 0.5) sim = zc >= Z_SKY * 0.5 ? 1.0 : 0.01;
            else if (z <= 0.0) sim = zc <= 0.0 ? 1.0 : 0.01;
            else sim = 1.0 / (1.0 + 60.0 * abs(zc - z) / z);
            float w = bw * sim + 1e-5;
            acc += texelFetch(t_cloud, q, 0) * w;
            wsum += w;
        }
    return acc / wsum;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) + ivec2(U.ofs.zw * U.proj.z);
    lo_px = ivec2(U.ofs.zw * U.proj.z);
    hi_px = ivec2((U.ofs.zw + U.area.xy) * U.proj.z) - 1;
    vec4 c = texelFetch(t_color, p, 0);
    float zr = texelFetch(t_z, p, 0).r;
    bool owned = ocean_depth(zr);
    float z = scene_depth(zr);
    vec3 col = c.rgb;
    vec3 ray = view_ray(vec2(p) + 0.5);
    vec3 dir = normalize(ray);
    float tsea = ray.y > 1e-5 ? (sea_y() - U.cam.y) / ray.y : 1e7;
    vec2 sxz = U.cam.xz + ray.xz * min(tsea, 1e7);
    float fp_sea = max(length(dFdx(sxz)), length(dFdy(sxz)));

    if (is_geometry(z)) {
        vec3 wp = U.cam.xyz + ray * z;
        float dist = z * length(ray);
        vec2 dxz = wp.xz - U.cam.xz;
        vec3 wc = vec3(wp.x, wp.y - dot(dxz, dxz) * EARTH_INV_2R, wp.z);
        float shadow = cloud_shadow(wc);
        shadow = mix(shadow, 1.0, game_fog_amount(z));   // no cloud shadows on fogged-out ground
        bool water = owned || (fx_on(FX_OCEAN) && U.water.w > 0.5 &&
                               abs(wc.y - sea_y()) < 4.0 + dist * 0.004 &&
                               (ground_flags(wc.xz) & 1u) != 0u);
        if (water) {
            float height = 0.0;
            vec2 x0 = owned ? ocean_origin(wc.xz, height) : wc.xz;
            col = ocean(wc, wp, x0, height, dir, dist, shadow, fp_sea);
            col = game_fog(col, z);
        } else if (fx_on(FX_GI) && z < 80000.0) col = indirect(col, p, z, wp, shadow);
        else col *= shadow;
        if (fx_on(FX_ATMO)) col = atmosphere(col, dir, dist, wc);
    }
    if (z >= Z_SKY * 0.5) col += sun_in_sky(dir);
    if (fx_on(FX_CLOUDS) && z > 0.0) {
        vec4 cl = clouds_at(p, z);
        col = col * cl.a + cl.rgb;
    }
    o_col = vec4(max(col, vec3(0.0)), c.a);
}
