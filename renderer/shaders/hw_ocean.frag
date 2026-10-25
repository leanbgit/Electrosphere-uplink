#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(location = 0) in float v_z;

layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_z;

void main() {
    o_col = vec4(U.water.rgb, 0.0);
    o_z = -max(v_z, 1.0);
}
