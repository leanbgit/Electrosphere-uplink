#version 450
#extension GL_GOOGLE_include_directive : require

#define FX_MAP
#define FX_OCEAN_TEX
#include "hw_fx_common.glsl"

layout(location = 0) in vec4 a_node;

layout(location = 0) out float v_z;

void main() {
    const int V = 17;
    ivec2 g = ivec2(gl_VertexIndex % V, gl_VertexIndex / V);
    float cell = a_node.z, range = a_node.w;
    vec2 xz = a_node.xy + vec2(g) * cell;
    float alt = max(altitude(U.cam.xyz), 1.0);
    vec2 rel = xz - U.cam.xz;
    float d = sqrt(dot(rel, rel) + alt * alt);
    float k = clamp((d - 0.75 * range) / (0.25 * range), 0.0, 1.0);
    xz -= vec2(g & 1) * cell * k;

    rel = xz - U.cam.xz;
    float h2 = dot(rel, rel);
    vec3 disp = vec3(0.0, 40.0, 0.0);
    if ((ground_flags(xz) & 1u) != 0u) disp = ocean_surface_disp(xz, ocean_mesh_fp(sqrt(h2 + alt * alt)));
    vec3 world = vec3(xz.x + disp.x, sea_y() + disp.y + h2 * EARTH_INV_2R, xz.y + disp.z);
    vec3 vv = U.world_to_view * (world - U.cam.xyz);
    gl_Position = clip_of(vv);
    v_z = vv.z;
}
