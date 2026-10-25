#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in uint a_tex;
layout(location = 4) in vec4 a_avg;
layout(location = 5) in vec2 a_org;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_col;
layout(location = 2) flat out uint v_tex;
layout(location = 3) out vec3 v_avg;
layout(location = 4) out float v_z;

void main() {
    vec3 world = vec3(a_org.x + a_pos.x, a_pos.y, a_org.y + a_pos.z);
    vec3 vv = U.world_to_view * (world - U.cam.xyz);
    gl_Position = clip_of(vv);
    if (U.ocean[1].w > 0.5 && a_avg.a < 0.5) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_uv = a_uv;
    v_col = a_col.rgb;
    v_tex = a_tex;
    v_avg = a_avg.rgb;
    v_z = vv.z;
}
