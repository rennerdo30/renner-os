#version 450
layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vLocal, vLocal);
    float a = exp(-r2 * 4.0) * clamp(1.0 - r2, 0.0, 1.0);
    outColor = vec4(vColor * a, 0.0);  // additive blend
}
