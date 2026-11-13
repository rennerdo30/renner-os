#version 450
// Per-instance spin and orbit, computed from time in the vertex shader.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 iPosScale;   // xyz orbit position, w scale
layout(location = 3) in vec4 iAxisPhase;  // xyz spin axis, w phase
layout(location = 4) in vec4 iColor;      // rgb albedo, a emissive strength
layout(location = 5) in vec4 iParams;     // metallic, roughness, spin speed, orbit speed

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 params;  // x: time
    vec4 sunDir;
    vec4 sunColor;
    vec4 lightPos[8];
    vec4 lightColor[8];
} fr;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vColor;
layout(location = 3) out vec2 vMaterial;

vec3 rotateAxis(vec3 v, vec3 k, float a) {
    float c = cos(a), s = sin(a);
    return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

void main() {
    float t = fr.params.x;
    float spin = t * iParams.z + iAxisPhase.w;
    vec3 local = rotateAxis(inPos * iPosScale.w, iAxisPhase.xyz, spin);
    vec3 n = rotateAxis(inNormal, iAxisPhase.xyz, spin);
    float orbit = t * iParams.w;
    float c = cos(orbit), s = sin(orbit);
    vec3 center = vec3(c * iPosScale.x - s * iPosScale.z,
                       iPosScale.y + 0.6 * sin(t * 0.9 + iAxisPhase.w * 3.0),
                       s * iPosScale.x + c * iPosScale.z);
    vec3 wp = center + local;
    vWorldPos = wp;
    vNormal = n;
    vColor = iColor;
    vMaterial = iParams.xy;
    gl_Position = fr.viewProj * vec4(wp, 1.0);
}
