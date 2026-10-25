#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_bloom.glsl"

layout(location = 0) out vec4 o_col;

void main() {
    vec2 uv = (gl_FragCoord.xy * 0.5) * pc.src_texel;
    vec2 t = pc.src_texel * pc.radius;
    vec3 s = src_at(uv) * 4.0;
    s += (src_at(uv + vec2(-t.x, 0)) + src_at(uv + vec2(t.x, 0)) +
          src_at(uv + vec2(0, -t.y)) + src_at(uv + vec2(0, t.y))) * 2.0;
    s += src_at(uv + vec2(-t.x, -t.y)) + src_at(uv + vec2(t.x, -t.y)) +
         src_at(uv + vec2(-t.x, t.y)) + src_at(uv + vec2(t.x, t.y));
    o_col = vec4(s / 16.0, 1.0);
}
