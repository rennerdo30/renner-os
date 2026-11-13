// Shared helpers. Specialization constants are set by VkContext::createGraphicsPipeline.
layout(constant_id = 0) const int MANUAL_GAMMA = 0;  // 1: swapchain is UNORM, encode sRGB in the shader
layout(constant_id = 1) const int HDR_OUT = 0;       // 1: write linear HDR to an RGBA16F target

const float PI = 3.14159265359;

vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// ACES filmic tonemapping, RRT+ODT fit by Stephen Hill.
vec3 rrtAndOdtFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 acesFitted(vec3 color) {
    const mat3 inputMat = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    color = inputMat * color;
    color = rrtAndOdtFit(color);
    color = outputMat * color;
    return clamp(color, 0.0, 1.0);
}

// Final color for a pass that writes either an HDR target or the swapchain.
vec4 finalOutput(vec3 hdr) {
    if (HDR_OUT != 0) return vec4(hdr, 1.0);
    vec3 c = acesFitted(hdr);
    if (MANUAL_GAMMA != 0) c = linearToSrgb(c);
    return vec4(c, 1.0);
}

float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float uhash(uint v) { return float(pcgHash(v)) * (1.0 / 4294967296.0); }
