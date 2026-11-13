#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// HDR scene + bloom -> exposure -> ACES -> vignette -> sRGB-encoded LDR with luma in alpha (for FXAA).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(push_constant) uniform Push { vec4 params; } pc;  // x: exposure, y: bloom strength

void main() {
    vec3 hdr = texture(hdrTex, vUV).rgb;
    vec3 bloom = texture(bloomTex, vUV).rgb;
    vec3 c = acesFitted((hdr + bloom * pc.params.y) * pc.params.x);
    vec2 d = vUV - 0.5;
    c *= 1.0 - dot(d, d) * 0.6;
    vec3 g = linearToSrgb(c);
    outColor = vec4(g, dot(g, vec3(0.299, 0.587, 0.114)));
}
