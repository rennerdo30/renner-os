#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Cook-Torrance GGX: sun plus 8 point lights, hemispheric ambient, emissive, distance fog.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 params;
    vec4 sunDir;
    vec4 sunColor;
    vec4 lightPos[8];
    vec4 lightColor[8];
} fr;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec2 vMaterial;
layout(location = 0) out vec4 outColor;

float distributionGGX(float NdH, float a) {
    float a2 = a * a;
    float d = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySmith(float NdV, float NdL, float rough) {
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    return (NdV / (NdV * (1.0 - k) + k)) * (NdL / (NdL * (1.0 - k) + k));
}

vec3 fresnelSchlick(float VdH, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0); }

vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float metal, float rough) {
    float NdL = dot(N, L);
    if (NdL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 1e-4);
    float NdH = max(dot(N, H), 0.0);
    float VdH = max(dot(V, H), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metal);
    vec3 F = fresnelSchlick(VdH, F0);
    float D = distributionGGX(NdH, rough * rough);
    float G = geometrySmith(NdV, NdL, rough);
    vec3 spec = D * G * F / (4.0 * NdV * NdL + 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * NdL;
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 toCam = fr.camPos.xyz - vWorldPos;
    float dist = length(toCam);
    vec3 V = toCam / dist;
    vec3 albedo = vColor.rgb;
    float metal = vMaterial.x;
    float rough = clamp(vMaterial.y, 0.05, 1.0);

    vec3 color = brdf(N, V, fr.sunDir.xyz, albedo, metal, rough) * fr.sunColor.rgb;
    for (int i = 0; i < 8; i++) {
        vec3 Lv = fr.lightPos[i].xyz - vWorldPos;
        float d2 = dot(Lv, Lv);
        float r2 = fr.lightPos[i].w * fr.lightPos[i].w;
        float window = clamp(1.0 - (d2 / r2) * (d2 / r2), 0.0, 1.0);
        float atten = window * window / (d2 + 1.0);
        if (atten > 0.0) color += brdf(N, V, Lv * inversesqrt(d2), albedo, metal, rough) * fr.lightColor[i].rgb * atten;
    }
    vec3 hemi = mix(vec3(0.03, 0.025, 0.02), vec3(0.06, 0.08, 0.12), 0.5 + 0.5 * N.y);
    color += hemi * albedo * (1.0 - 0.5 * metal) + hemi * metal * 0.5;
    color += albedo * vColor.a;  // emissive
    color = mix(vec3(0.004, 0.006, 0.012), color, exp(-dist * 0.004));
    outColor = finalOutput(color);
}
