#version 450

layout(set = 0, binding = 0) uniform usampler2D vram;

layout(push_constant) uniform Push {
    ivec2 origin;
    ivec2 size;
    vec2  view_pos;
    vec2  view_size;
    uint  rgb24;
} pc;

layout(location = 0) out vec4 outColor;

uint vramRead(int x, int y) {
    return texelFetch(vram, ivec2(x & 1023, y & 511), 0).r;
}

vec3 decode555(uint c) {
    return vec3(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u))
         / 31.0;
}

void main() {
    vec2 rel = (gl_FragCoord.xy - pc.view_pos) / pc.view_size;
    ivec2 p = clamp(ivec2(floor(rel * vec2(pc.size))), ivec2(0), pc.size - 1);
    int y = pc.origin.y + p.y;

    if (pc.rgb24 != 0u) {
        int b  = p.x * 3;
        int wx = pc.origin.x + (b >> 1);
        uint w0 = vramRead(wx, y);
        uint w1 = vramRead(wx + 1, y);
        uvec3 c = ((b & 1) == 0)
                ? uvec3(w0 & 255u, w0 >> 8, w1 & 255u)
                : uvec3(w0 >> 8, w1 & 255u, w1 >> 8);
        outColor = vec4(vec3(c) / 255.0, 1.0);
    } else {
        outColor = vec4(decode555(vramRead(pc.origin.x + p.x, y)), 1.0);
    }
}
