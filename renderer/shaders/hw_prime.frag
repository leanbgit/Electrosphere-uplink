#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(set = 0, binding = 2) uniform sampler2D t_z;

void main() {
    float z = texelFetch(t_z, ivec2(gl_FragCoord.xy), 0).r;
    if (z == 0.0) gl_FragDepth = 1.0;
    else if (z >= Z_SKY * 0.5) gl_FragDepth = 0.0;
    else gl_FragDepth = clamp(NEAR_Z / scene_depth(z), 0.0, 1.0);
}
