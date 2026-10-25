#version 450

layout(set = 0, binding = 0) uniform sampler2D vram;

layout(push_constant) uniform Push {
    vec2  origin;
    vec2  size;
    vec2  view_pos;
    vec2  view_size;
    vec2  inv_vram;
    float half_texel;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 rel = clamp((gl_FragCoord.xy - pc.view_pos) / pc.view_size, 0.0, 1.0);
    vec2 p = pc.origin + rel * pc.size;
    vec2 lo = pc.origin + pc.half_texel, hi = pc.origin + pc.size - pc.half_texel;
    p = clamp(p, lo, hi);
    outColor = vec4(textureLod(vram, p * pc.inv_vram, 0.0).rgb, 1.0);
}
