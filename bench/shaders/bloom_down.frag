#version 450
// 13-tap bloom downsample (Jimenez, "Next Generation Post Processing in Call of Duty").
// The first pass also applies a soft threshold and a Karis average against fireflies.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec4 params; } pc;  // xy: source texel size, z: threshold, w: 1 = first pass

vec3 tap(vec2 offset) { return min(texture(src, vUV + offset * pc.params.xy).rgb, vec3(65000.0)); }

float karisWeight(vec3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); }

vec3 karisAverage(vec3 a, vec3 b, vec3 c, vec3 d) {
    float wa = karisWeight(a), wb = karisWeight(b), wc = karisWeight(c), wd = karisWeight(d);
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

vec3 softThreshold(vec3 c, float threshold) {
    float br = max(c.r, max(c.g, c.b));
    float knee = threshold * 0.5;
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    return c * (max(soft, br - threshold) / max(br, 1e-5));
}

void main() {
    vec3 a = tap(vec2(-2, -2)), b = tap(vec2(0, -2)), c = tap(vec2(2, -2));
    vec3 d = tap(vec2(-2, 0)), e = tap(vec2(0, 0)), f = tap(vec2(2, 0));
    vec3 g = tap(vec2(-2, 2)), h = tap(vec2(0, 2)), i = tap(vec2(2, 2));
    vec3 j = tap(vec2(-1, -1)), k = tap(vec2(1, -1)), l = tap(vec2(-1, 1)), m = tap(vec2(1, 1));
    vec3 result;
    if (pc.params.w > 0.5) {
        result = karisAverage(j, k, l, m) * 0.5 + karisAverage(a, b, d, e) * 0.125 + karisAverage(b, c, e, f) * 0.125 +
                 karisAverage(d, e, g, h) * 0.125 + karisAverage(e, f, h, i) * 0.125;
        result = softThreshold(result, pc.params.z);
    } else {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outColor = vec4(result, 1.0);
}
