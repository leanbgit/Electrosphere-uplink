#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_bloom.glsl"

layout(location = 0) out vec4 o_col;

float karis(vec3 c) { return 1.0 / (1.0 + max(max(c.r, c.g), c.b)); }

vec3 bright(vec3 c) {
    float m = max(max(c.r, c.g), c.b);
    float knee = 0.35;
    float soft = clamp(m - 1.0 + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float w = max(soft, m - 1.0) / max(m, 1e-4);
    return c * w;
}

void main() {
    vec2 uv = (gl_FragCoord.xy * 2.0) * pc.src_texel;
    vec2 t = pc.src_texel;
    vec3 a = src_at(uv + t * vec2(-2, -2)), b = src_at(uv + t * vec2(0, -2)), c = src_at(uv + t * vec2(2, -2));
    vec3 d = src_at(uv + t * vec2(-2, 0)),  e = src_at(uv),                   f = src_at(uv + t * vec2(2, 0));
    vec3 g = src_at(uv + t * vec2(-2, 2)),  h = src_at(uv + t * vec2(0, 2)),  i = src_at(uv + t * vec2(2, 2));
    vec3 j = src_at(uv + t * vec2(-1, -1)), k = src_at(uv + t * vec2(1, -1));
    vec3 l = src_at(uv + t * vec2(-1, 1)),  m = src_at(uv + t * vec2(1, 1));
    vec3 col;
    if (pc.first > 0.5) {
        vec3 g0 = bright((j + k + l + m) * 0.25), g1 = bright((a + b + d + e) * 0.25);
        vec3 g2 = bright((b + c + e + f) * 0.25), g3 = bright((d + e + g + h) * 0.25);
        vec3 g4 = bright((e + f + h + i) * 0.25);
        float w0 = karis(g0) * 0.5, w1 = karis(g1) * 0.125, w2 = karis(g2) * 0.125;
        float w3 = karis(g3) * 0.125, w4 = karis(g4) * 0.125;
        col = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        col = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    o_col = vec4(col, 1.0);
}
