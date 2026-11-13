#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(push_constant) uniform Push { vec4 params; } pc;  // x: exposure

void main() {
    vec3 c = texture(hdrTex, vUV).rgb * pc.params.x;
    c += vec3(0.004, 0.005, 0.010) * (1.2 - vUV.y);  // faint backdrop
    outColor = finalOutput(c);
}
