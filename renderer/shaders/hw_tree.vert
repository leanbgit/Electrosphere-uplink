#version 450
#extension GL_GOOGLE_include_directive : require

#define FX_MAP
#include "hw_fx_common.glsl"

layout(push_constant) uniform Push {
    float cell;
    float n;
    float r_in;
    float r_out;
} pc;

const float TREE_CELL = 44.0;

layout(location = 0) out vec3 v_col;
layout(location = 1) out vec3 v_n;
layout(location = 2) out float v_ao;
layout(location = 3) out float v_fog;

const int SIDES = 7;
const int VERTS_PER_CONE = SIDES * 3;

void hide() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_col = vec3(0); v_n = vec3(0, -1, 0); v_ao = 1.0; v_fog = 0.0; }

void main() {
    int N = int(pc.n);
    ivec2 ci = ivec2(gl_InstanceIndex % N, gl_InstanceIndex / N) - N / 2;
    float stride = pc.cell / TREE_CELL;
    vec2 cell = (floor(U.cam.xz / pc.cell) + vec2(ci)) * stride;
    float h1 = hash12(cell * 1.37 + 11.1);
    float h2 = hash12(cell.yx * 2.11 + 3.7);
    float h3 = hash12(cell * 0.73 + 7.9);
    vec2 xz = (cell + 0.15 + 0.7 * vec2(h1, h2)) * TREE_CELL;
    vec2 rel = xz - U.cam.xz;
    float r = max(abs(rel.x), abs(rel.y));
    if (r < pc.r_in || r >= pc.r_out || h3 > 0.86) { hide(); return; }
    bool shared_tree = mod(cell.x, 2.0) < 0.5 && mod(cell.y, 2.0) < 0.5;
    if ((ground_flags(xz) & 2u) == 0u) { hide(); return; }

    vec3 base = vec3(xz.x, ground_y(xz) + 6.0, xz.y);
    vec3 vb = U.world_to_view * (base - U.cam.xyz);
    if (vb.z < 40.0) { hide(); return; }
    float sx = U.proj.x * U.proj.y * vb.x / vb.z, sy = U.proj.x * vb.y / vb.z;
    if (abs(sx) > U.area.x * 0.5 + 64.0 || abs(sy) > U.area.y * 0.5 + 200.0) { hide(); return; }

    float fade = stride < 1.5 && !shared_tree ? 1.0 - smoothstep(pc.r_out * 0.78, pc.r_out, r)
               : stride < 1.5 ? 1.0 : 1.0 - smoothstep(pc.r_out * 0.8, pc.r_out, r);
    if (fade <= 0.0) { hide(); return; }
    float height = mix(75.0, 150.0, h1 * 0.6 + h2 * 0.4) * fade;
    float radius = height * mix(0.24, 0.36, h3 / 0.86);
    bool round_tree = h2 > 0.62;

    int v = gl_VertexIndex;
    int cone = v / VERTS_PER_CONE;
    int tri = (v % VERTS_PER_CONE) / 3;
    int k = v % 3;
    float spin = h3 * 6.2831;
    float a = (float(tri) + (k == 1 ? 1.0 : 0.0)) * (6.2831 / float(SIDES)) + spin;
    float y0 = cone == 0 ? 0.18 : 0.46, y1 = cone == 0 ? 0.74 : 1.0;
    float rr = cone == 0 ? radius : radius * 0.72;
    if (round_tree) { y1 = cone == 0 ? 0.62 : 0.9; rr *= 1.25; }
    vec3 local;
    vec3 n;
    float slope = rr / ((y1 - y0) * height);
    if (k == 2) {
        local = vec3(0.0, y1 * height, 0.0);
        float am = a - 3.14159 / float(SIDES);
        n = normalize(vec3(cos(am), slope * 1.3, sin(am)));
        v_ao = 1.0;
    } else {
        local = vec3(cos(a) * rr, y0 * height, sin(a) * rr);
        n = normalize(vec3(cos(a), slope, sin(a)));
        v_ao = cone == 0 ? 0.55 : 0.8;
    }
    float sway = sin(time_s() * (1.1 + h1 * 0.6) + h2 * 40.0) * 0.035 * local.y;
    local.xz += normalize(U.wind.zw + 1e-4) * sway;
    vec3 world = base + vec3(local.x, -local.y, local.z);
    n = vec3(n.x, -n.y, n.z);

    vec3 vv = U.world_to_view * (world - U.cam.xyz);
    if (vv.z < 20.0) { hide(); return; }
    gl_Position = clip_of(vv);
    v_fog = game_fog_amount(vv.z);

    vec3 g = ground_color(xz).rgb;
    float tint = mix(0.82, 1.18, h1);
    v_col = g * tint * (round_tree ? vec3(1.05, 1.08, 0.9) : vec3(0.92, 1.0, 0.95));
    v_n = n;
}
