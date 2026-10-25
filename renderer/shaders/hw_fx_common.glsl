layout(set = 0, binding = 0, std140) uniform Scene {
    mat3  view_to_world;
    mat3  world_to_view;
    vec4  cam;
    vec4  proj;
    vec4  ofs;
    vec4  area;
    vec4  sun;
    vec4  sun_col;
    vec4  fog;
    vec4  map;
    vec4  cloud;
    vec4  wind;
    vec4  water;
    vec4  misc;
    vec4  pal;
    vec4  pal_src;
    vec4  hdr;
    vec4  ocean[9];
    vec4  sky[32];
    mat3  prev_wtv;
    vec4  prev_cam;
    vec4  prev_proj;
    vec4  prev_area;
    vec4  cloud_tc;
} U;

const uint FX_FAR    = 1u;
const uint FX_OCEAN  = 2u;
const uint FX_CLOUDS = 4u;
const uint FX_FOREST = 8u;
const uint FX_ATMO   = 16u;
const uint FX_GI     = 32u;
const uint FX_SUN    = 64u;

const float Z_SKY = 1.0e9;
const float PI = 3.14159265;
const float NEAR_Z = 16.0;

uint fx_flags() { return uint(U.misc.w + 0.5); }
bool fx_on(uint f) { return (fx_flags() & f) != 0u; }

float time_s() { return U.cam.w; }
float sea_y() { return U.proj.w; }
float altitude(vec3 p) { return sea_y() - p.y; }

vec3 view_ray(vec2 frag) {
    vec2 s = frag / U.proj.z - U.ofs.xy;
    vec3 dv = vec3(s.x / (U.proj.x * U.proj.y), s.y / U.proj.x, 1.0);
    return U.view_to_world * dv;
}

vec4 clip_of(vec3 vv) {
    float X = (U.proj.x * U.proj.y * vv.x + U.ofs.x * vv.z) * U.proj.z * U.misc.x * 2.0 - vv.z;
    float Y = (U.proj.x * vv.y + U.ofs.y * vv.z) * U.proj.z * U.misc.y * 2.0 - vv.z;
    return vec4(X, Y, NEAR_Z, vv.z);
}

vec3 sky_at(float e) {
    float f = clamp((e / PI + 0.5) * 31.0, 0.0, 30.999);
    int i = int(f);
    return mix(U.sky[i].rgb, U.sky[i + 1].rgb, f - float(i));
}

float elevation(vec3 dir) {
    return asin(clamp(-dir.y / max(length(dir), 1e-6), -1.0, 1.0));
}

float luma(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

// Original fog, measured from the game by hw_glue.c: U.area.z and U.area.w are
// the fade's start and end in view depth (end 0 = off), U.sky[0..2].w the
// colour the game fades to.
vec3 game_fog(vec3 col, float z) {
    if (U.area.w <= 0.0) return col;
    float f = clamp((z - U.area.z) / max(U.area.w - U.area.z, 1.0), 0.0, 1.0);
    return mix(col, vec3(U.sky[0].w, U.sky[1].w, U.sky[2].w), f);
}
float game_fog_amount(float z) {
    if (U.area.w <= 0.0) return 0.0;
    return clamp((z - U.area.z) / max(U.area.w - U.area.z, 1.0), 0.0, 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float hg(float mu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

const float SUN_R = 0.0085;

float sun_moon() { return 1.0 - smoothstep(0.22, 0.4, U.sun.w); }

vec3 sun_tint() {
    float e = clamp(-U.sun.y, 0.0, 1.0);
    vec3 day = mix(U.sun_col.rgb, vec3(1.0, 0.97, 0.9), 0.5);
    vec3 low = mix(day, normalize(sky_at(0.02) + 1e-3) * 1.7, 0.55);
    vec3 c = mix(low, day, smoothstep(0.02, 0.25, e));
    return mix(c, vec3(0.85, 0.9, 1.0), sun_moon());
}

vec3 sun_radiance() {
    float k = mix(55.0 * clamp(U.sun.w, 0.0, 1.6), 3.0, sun_moon());
    return sun_tint() * k;
}

vec3 sun_in_sky(vec3 dir) {
    if (!fx_on(FX_SUN) || U.sun.w <= 0.0) return vec3(0.0);
    float mu = dot(dir, U.sun.xyz);
    float a = acos(clamp(mu, -1.0, 1.0));
    float disc = 1.0 - smoothstep(SUN_R * 0.92, SUN_R * 1.06, a);
    float x = clamp(a / SUN_R, 0.0, 1.0);
    float limb = 1.0 - 0.55 * (1.0 - sqrt(max(1.0 - x * x, 0.0)));
    vec3 c = sun_radiance() * disc * limb;
    float aure = exp(-a / 0.02) * 0.9 + exp(-a / 0.09) * 0.25;
    c += sun_tint() * aure * clamp(U.sun.w, 0.0, 1.6) * (1.0 - 0.8 * sun_moon());
    return c;
}

vec3 sun_screen() {
    vec3 v = U.world_to_view * U.sun.xyz;
    if (v.z <= 0.02) return vec3(0.0, 0.0, 0.0);
    vec2 s = vec2(U.proj.x * U.proj.y * v.x / v.z, U.proj.x * v.y / v.z) + U.ofs.xy;
    return vec3(s * U.proj.z, 1.0);
}

const float EARTH_INV_2R = 1.0 / (2.0 * 6.371e6 * 5.0);

bool  ocean_depth(float z) { return z < 0.0; }
float scene_depth(float z) { return abs(z); }

#ifdef FX_OCEAN_TEX
layout(set = 0, binding = 14) uniform sampler2DArray t_odisp;
layout(set = 0, binding = 15) uniform sampler2DArray t_oderiv;

bool ocean_drawn() { return U.ocean[1].w > 0.5; }

const vec2 OC_ROT[3] = vec2[3](vec2(1.0, 0.0), vec2(0.990268, 0.139173), vec2(0.974370, -0.224951));
vec2 oc_to(vec2 v, int c) { vec2 r = OC_ROT[c]; return vec2(r.x * v.x - r.y * v.y, r.y * v.x + r.x * v.y); }
vec2 oc_from(vec2 v, int c) { vec2 r = OC_ROT[c]; return vec2(r.x * v.x + r.y * v.y, -r.y * v.x + r.x * v.y); }
vec3 oc_uv(vec2 xz, int c) { return vec3(oc_to(xz, c) / U.ocean[0][c], float(c)); }
float oc_lod(float fp, int c) { return log2(max(fp * 256.0 / U.ocean[0][c], 1e-3)); }

float ocean_mesh_fp(float d) { return d / 64.0; }

vec3 ocean_disp(vec2 xz, float fp) {
    vec3 d = vec3(0.0);
    for (int c = 0; c < 3; c++) {
        vec3 t = textureLod(t_odisp, oc_uv(xz, c), oc_lod(fp, c)).xyz;
        vec2 s = oc_from(t.xz, c);
        d += vec3(s.x, -t.y, s.y);
    }
    return d * U.ocean[1].y;
}
#endif

#ifdef FX_MAP
layout(set = 0, binding = 5) uniform usampler2D t_grid;
layout(set = 0, binding = 6) uniform sampler2D  t_height;
layout(set = 0, binding = 7) uniform sampler2D  t_colors;
layout(set = 0, binding = 8) uniform sampler2D  t_top;

int wrap_cell(int c) {
    if (c < 0) return c & 7;
    if (c >= 256) return (c & 7) | 0xF8;
    return c;
}

uint tile_at(vec2 xz) {
    ivec2 tc = ivec2(floor(xz / 2048.0 + 128.0));
    return texelFetch(t_grid, ivec2(wrap_cell(tc.x), wrap_cell(tc.y)), 0).r;
}

float tile_top(vec2 xz) {
    uint t = tile_at(xz);
    return texelFetch(t_top, ivec2(int(t), 0), 0).r;
}

void atlas_coords(vec2 xz, out ivec2 base, out ivec2 i0, out ivec2 i1, out vec2 a) {
    vec2 f = xz / 2048.0 + 128.0;
    ivec2 tc = ivec2(floor(f));
    uint t = texelFetch(t_grid, ivec2(wrap_cell(tc.x), wrap_cell(tc.y)), 0).r;
    vec2 l = clamp((f - vec2(tc)) * 32.0 - 0.5, vec2(0.0), vec2(31.0));
    i0 = ivec2(l);
    i1 = min(i0 + 1, ivec2(31));
    a = l - vec2(i0);
    base = ivec2(int(t % 16u), int(t / 16u)) * 32;
}

float ground_y(vec2 xz) {
    ivec2 b, i0, i1; vec2 a;
    atlas_coords(xz, b, i0, i1, a);
    float h00 = texelFetch(t_height, b + i0, 0).r;
    float h10 = texelFetch(t_height, b + ivec2(i1.x, i0.y), 0).r;
    float h01 = texelFetch(t_height, b + ivec2(i0.x, i1.y), 0).r;
    float h11 = texelFetch(t_height, b + i1, 0).r;
    return mix(mix(h00, h10, a.x), mix(h01, h11, a.x), a.y);
}

vec4 ground_color(vec2 xz) {
    ivec2 b, i0, i1; vec2 a;
    atlas_coords(xz, b, i0, i1, a);
    vec4 c00 = texelFetch(t_colors, b + i0, 0);
    vec4 c10 = texelFetch(t_colors, b + ivec2(i1.x, i0.y), 0);
    vec4 c01 = texelFetch(t_colors, b + ivec2(i0.x, i1.y), 0);
    vec4 c11 = texelFetch(t_colors, b + i1, 0);
    return mix(mix(c00, c10, a.x), mix(c01, c11, a.x), a.y);
}

uint ground_flags(vec2 xz) {
    ivec2 b, i0, i1; vec2 a;
    atlas_coords(xz, b, i0, i1, a);
    ivec2 n = ivec2(a.x < 0.5 ? i0.x : i1.x, a.y < 0.5 ? i0.y : i1.y);
    return uint(texelFetch(t_colors, b + n, 0).a * 255.0 + 0.5);
}
#endif

#if defined(FX_MAP) && defined(FX_OCEAN_TEX)
float ocean_open(vec2 xz) {
    float w = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(i == 0 ? 64.0 : i == 1 ? -64.0 : 0.0, i == 2 ? 64.0 : i == 3 ? -64.0 : 0.0);
        if ((ground_flags(xz + o) & 1u) != 0u) w += 0.25;
    }
    return w;
}

vec3 ocean_surface_disp(vec2 xz, float fp) {
    float open = ocean_open(xz);
    return ocean_disp(xz, fp) * mix(0.15, 1.0, open) + vec3(0.0, (1.0 - open) * 5.0, 0.0);
}
#endif
