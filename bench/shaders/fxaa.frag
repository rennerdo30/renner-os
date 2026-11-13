#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// FXAA (Lottes' fast PC variant) on the sRGB-encoded LDR image, written to the swapchain.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D ldrTex;
layout(push_constant) uniform Push { vec4 params; } pc;  // xy: 1/size, z: 1 = FXAA on

const float FXAA_REDUCE_MIN = 1.0 / 128.0;
const float FXAA_REDUCE_MUL = 1.0 / 8.0;
const float FXAA_SPAN_MAX = 8.0;

vec3 fxaa(vec2 uv, vec2 rcp) {
    float lumaNW = texture(ldrTex, uv + vec2(-1.0, -1.0) * rcp).a;
    float lumaNE = texture(ldrTex, uv + vec2(1.0, -1.0) * rcp).a;
    float lumaSW = texture(ldrTex, uv + vec2(-1.0, 1.0) * rcp).a;
    float lumaSE = texture(ldrTex, uv + vec2(1.0, 1.0) * rcp).a;
    vec4 center = texture(ldrTex, uv);
    float lumaM = center.a;
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * rcp;
    vec3 rgbA = 0.5 * (texture(ldrTex, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(ldrTex, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(ldrTex, uv - dir * 0.5).rgb + texture(ldrTex, uv + dir * 0.5).rgb);
    float lumaB = dot(rgbB, vec3(0.299, 0.587, 0.114));
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

void main() {
    vec3 c = pc.params.z > 0.5 ? fxaa(vUV, pc.params.xy) : texture(ldrTex, vUV).rgb;
    outColor = vec4(MANUAL_GAMMA != 0 ? c : srgbToLinear(c), 1.0);
}
