layout(set = 0, binding = 0) uniform sampler2D t_src;

layout(push_constant) uniform Push {
    vec2  src_texel;
    vec2  uv_max;
    float radius;
    float first;
} pc;

vec3 src_at(vec2 uv) {
    return texture(t_src, clamp(uv, vec2(0.0), pc.uv_max - pc.src_texel * 0.5)).rgb;
}
