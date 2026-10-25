#version 450

layout(location = 0) in vec4  a_pos;
layout(location = 1) in vec2  a_uv;
layout(location = 2) in vec4  a_col;
layout(location = 3) in uvec4 a_info;

layout(push_constant) uniform Push {
    vec2 inv_size;
    int  scale;
    int  pass;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;
layout(location = 2) flat out uvec4 v_info;
layout(location = 3) out float v_z;

void main() {
    vec2 ndc = a_pos.xy * pc.inv_size * 2.0 - 1.0;
    float w = a_pos.w > 0.5 ? a_pos.z : 1.0;
    gl_Position = vec4(ndc * w, 0.0, w);
    v_uv = a_uv;
    v_col = a_col;
    v_info = a_info;
    v_z = a_pos.z;
}
