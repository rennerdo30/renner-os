#version 450
// 3x3 tent upsample of the smaller bloom level, added onto the larger one by additive blending.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec4 params; } pc;  // xy: source texel size, z: radius in texels

void main() {
    vec2 o = pc.params.xy * pc.params.z;
    vec3 s = texture(src, vUV + vec2(-o.x, -o.y)).rgb + texture(src, vUV + vec2(o.x, -o.y)).rgb +
             texture(src, vUV + vec2(-o.x, o.y)).rgb + texture(src, vUV + vec2(o.x, o.y)).rgb;
    s += 2.0 * (texture(src, vUV + vec2(0.0, -o.y)).rgb + texture(src, vUV + vec2(0.0, o.y)).rgb +
                texture(src, vUV + vec2(-o.x, 0.0)).rgb + texture(src, vUV + vec2(o.x, 0.0)).rgb);
    s += 4.0 * texture(src, vUV).rgb;
    outColor = vec4(s / 16.0, 1.0);
}
