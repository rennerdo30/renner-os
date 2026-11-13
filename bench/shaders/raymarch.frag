#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// SDF scene: a rotating menger sponge, twisted pillars repeated over the plane with blended caps,
// metaball orbs fused with a ring, and a rolling terrain. Soft shadows, AO, one reflection, fog.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Push {
    vec4 res;     // width, height, 1/width, 1/height
    vec4 params;  // x: time
} pc;

float T;
vec3 sunDir;

mat2 rot(float a) {
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s, c);
}

float sdBox(vec3 p, vec3 b) {
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float sdTorus(vec3 p, vec2 t) {
    vec2 q = vec2(length(p.xz) - t.x, p.y);
    return length(q) - t.y;
}

float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

float menger(vec3 p) {
    float d = sdBox(p, vec3(1.0));
    float s = 1.0;
    for (int m = 0; m < 4; m++) {
        vec3 a = mod(p * s, 2.0) - 1.0;
        s *= 3.0;
        vec3 r = abs(1.0 - 3.0 * abs(a));
        float da = max(r.x, r.y), db = max(r.y, r.z), dc = max(r.z, r.x);
        float c = (min(da, min(db, dc)) - 1.0) / s;
        d = max(d, c);
    }
    return d;
}

float terrain(vec3 p) {
    float h = 0.35 * sin(p.x * 0.31) * sin(p.z * 0.27) + 0.12 * sin(p.x * 1.1 + p.z * 0.7) +
              0.05 * sin(p.x * 2.9 - p.z * 3.1);
    return p.y + 1.6 - h;
}

vec2 opU(vec2 a, vec2 b) { return a.x < b.x ? a : b; }

// Returns (distance, material). Materials: 0 ground, 1 sponge, 2 pillars, 3 glowing orbs.
vec2 map(vec3 p) {
    vec2 res = vec2(terrain(p) * 0.75, 0.0);

    vec3 q = p - vec3(0.0, 0.9, 0.0);
    q.xz *= rot(T * 0.25);
    q.xy *= rot(0.3 * sin(T * 0.17));
    res = opU(res, vec2(menger(q / 1.5) * 1.5, 1.0));

    // Pillars, one per 10x10 cell, none in the center cell.
    vec2 cell = floor((p.xz + 5.0) / 10.0);
    float pillar;
    if (all(equal(cell, vec2(0.0)))) {
        pillar = 5.0 - max(abs(p.x), abs(p.z)) + 3.8;  // conservative distance to the neighbor cells
    } else {
        vec3 r = p;
        r.xz = mod(p.xz + 5.0, 10.0) - 5.0;
        float hc = hash13(vec3(cell, 7.0));
        r.xz *= rot(r.y * 0.4 + hc * 6.28);
        pillar = sdBox(r - vec3(0.0, 0.5, 0.0), vec3(0.4, 2.4, 0.4)) - 0.08;
        float cap = length(r - vec3(0.0, 3.3 + 0.35 * sin(T * 1.3 + hc * 6.28), 0.0)) - 0.7;
        pillar = smin(pillar, cap, 0.6);
    }
    res = opU(res, vec2(pillar * 0.9, 2.0));

    float orbs = 1e3;
    for (int i = 0; i < 4; i++) {
        float a = T * 0.7 + float(i) * 1.5708;
        vec3 c = vec3(cos(a) * 2.8, 1.0 + 0.8 * sin(T * 1.1 + float(i)), sin(a) * 2.8);
        orbs = smin(orbs, length(p - c) - 0.35, 0.8);
    }
    vec3 tp = p - vec3(0.0, 0.9, 0.0);
    tp.xy *= rot(0.4);
    orbs = smin(orbs, sdTorus(tp, vec2(2.8, 0.06)), 0.5);
    res = opU(res, vec2(orbs, 3.0));
    return res;
}

vec2 march(vec3 ro, vec3 rd, int maxSteps, float tmax) {
    float t = 0.02;
    float m = -1.0;
    for (int i = 0; i < maxSteps; i++) {
        vec2 h = map(ro + rd * t);
        if (abs(h.x) < 0.0006 * t) {
            m = h.y;
            break;
        }
        t += h.x * 0.9;
        if (t > tmax) break;
    }
    return vec2(t, m);
}

vec3 calcNormal(vec3 p, float t) {
    float e = 0.0005 * max(1.0, t);
    const vec2 k = vec2(1.0, -1.0);
    return normalize(k.xyy * map(p + k.xyy * e).x + k.yyx * map(p + k.yyx * e).x +
                     k.yxy * map(p + k.yxy * e).x + k.xxx * map(p + k.xxx * e).x);
}

float softShadow(vec3 ro, vec3 rd, float tmax, int steps) {
    float res = 1.0;
    float t = 0.03;
    for (int i = 0; i < steps; i++) {
        float h = map(ro + rd * t).x;
        res = min(res, 10.0 * h / t);
        t += clamp(h, 0.02, 0.4);
        if (res < 0.002 || t > tmax) break;
    }
    return clamp(res, 0.0, 1.0);
}

float calcAO(vec3 p, vec3 n) {
    float occ = 0.0, sca = 1.0;
    for (int i = 0; i < 5; i++) {
        float h = 0.02 + 0.12 * float(i);
        occ += (h - map(p + h * n).x) * sca;
        sca *= 0.9;
    }
    return clamp(1.0 - 2.5 * occ, 0.0, 1.0);
}

vec3 skyColor(vec3 rd) {
    vec3 col = mix(vec3(0.55, 0.60, 0.70), vec3(0.12, 0.22, 0.45), clamp(rd.y * 1.5 + 0.2, 0.0, 1.0));
    float s = max(dot(rd, sunDir), 0.0);
    col += vec3(1.0, 0.75, 0.45) * (pow(s, 600.0) * 30.0 + pow(s, 8.0) * 0.3);
    return col;
}

vec3 applyFog(vec3 col, float t, vec3 rd) {
    float s = max(dot(rd, sunDir), 0.0);
    vec3 fogCol = mix(vec3(0.50, 0.56, 0.66), vec3(1.0, 0.8, 0.6), pow(s, 6.0));
    return mix(fogCol, col, exp(-0.0012 * t * t));
}

vec3 albedo(float m, vec3 p) {
    if (m < 0.5) {
        float c = mod(floor(p.x) + floor(p.z), 2.0);
        return mix(vec3(0.18, 0.20, 0.23), vec3(0.32, 0.30, 0.27), c);
    }
    if (m < 1.5) return vec3(0.85, 0.62, 0.32);
    if (m < 2.5) return vec3(0.52, 0.55, 0.60);
    return vec3(1.0);
}

float reflectivity(float m) {
    if (m < 0.5) return 0.25;
    if (m < 1.5) return 0.6;
    if (m < 2.5) return 0.1;
    return 0.0;
}

vec3 shade(vec3 p, vec3 n, vec3 rd, float m, bool primary) {
    vec3 alb = albedo(m, p);
    if (m > 2.5) return vec3(4.0, 1.6, 0.6) * (1.6 + 0.5 * sin(T * 3.0));  // emissive orbs
    float dif = clamp(dot(n, sunDir), 0.0, 1.0);
    float sh = dif > 0.0 ? softShadow(p + n * 0.01, sunDir, 20.0, primary ? 64 : 24) : 0.0;
    float ao = primary ? calcAO(p, n) : 0.7;
    vec3 h = normalize(sunDir - rd);
    float spe = pow(clamp(dot(n, h), 0.0, 1.0), m < 1.5 && m > 0.5 ? 64.0 : 16.0) * dif * sh;
    float sky = 0.5 + 0.5 * n.y;
    vec3 lp = vec3(0.0, 1.0, 0.0) - p;
    float ld = length(lp);
    float orbLight = clamp(dot(n, lp / ld), 0.0, 1.0) / (1.0 + ld * ld * 0.25);
    vec3 col = alb * (vec3(1.6, 1.35, 1.1) * dif * sh + vec3(0.25, 0.35, 0.55) * sky * ao +
                      vec3(1.0, 0.45, 0.15) * orbLight * 3.0 * ao);
    col += vec3(1.6, 1.35, 1.1) * spe * 0.6;
    return col;
}

void main() {
    T = pc.params.x;
    sunDir = normalize(vec3(0.6 * cos(T * 0.05), 0.55, 0.6 * sin(T * 0.05) - 0.3));

    vec2 frag = vUV * pc.res.xy;
    vec2 uv = (2.0 * frag - pc.res.xy) / pc.res.y;
    uv.y = -uv.y;

    float a = T * 0.15;
    vec3 ro = vec3(6.5 * cos(a), 2.4 + 1.0 * sin(T * 0.23), 6.5 * sin(a));
    vec3 ta = vec3(0.0, 0.7, 0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0)));
    vec3 vv = cross(uu, ww);
    vec3 rd = normalize(uv.x * uu + uv.y * vv + 1.6 * ww);

    vec3 col = skyColor(rd);
    vec2 h = march(ro, rd, 160, 80.0);
    if (h.y >= 0.0) {
        vec3 p = ro + rd * h.x;
        vec3 n = calcNormal(p, h.x);
        col = shade(p, n, rd, h.y, true);
        float refl = reflectivity(h.y);
        if (refl > 0.0) {
            vec3 rr = reflect(rd, n);
            vec3 ro2 = p + n * 0.02;
            vec2 h2 = march(ro2, rr, 80, 40.0);
            vec3 rc = skyColor(rr);
            if (h2.y >= 0.0) {
                vec3 p2 = ro2 + rr * h2.x;
                rc = applyFog(shade(p2, calcNormal(p2, h2.x), rr, h2.y, false), h2.x, rr);
            }
            float fre = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 5.0);
            col = mix(col, rc, refl * (0.35 + 0.65 * fre));
        }
        col = applyFog(col, h.x, rd);
    }
    vec2 d = vUV - 0.5;
    col *= 1.0 - dot(d, d) * 0.5;  // vignette
    outColor = finalOutput(col);
}
