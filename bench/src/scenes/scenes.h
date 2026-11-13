// Scene interface, small math helpers and the instanced-mesh renderer shared by two scenes.
#pragma once

#include "vk_context.h"

#include <cmath>
#include <memory>
#include <string>

class Scene {
public:
    virtual ~Scene() = default;
    virtual const char* name() const = 0;
    // Size-independent resources (meshes, buffers, layouts).
    virtual void init(VkContext& ctx) = 0;
    // Resources that depend on the swapchain (render targets, render passes, pipelines).
    virtual void createSized(VkContext& ctx) = 0;
    virtual void record(VkContext& ctx, const FrameInfo& fi) = 0;
    void destroySized() { sized.flush(); }
    void destroy() {
        sized.flush();
        persistent.flush();
    }

protected:
    ResourceList persistent;
    ResourceList sized;
};

std::unique_ptr<Scene> makeRaymarchScene();
std::unique_ptr<Scene> makeInstancedScene();
std::unique_ptr<Scene> makeParticlesScene();
std::unique_ptr<Scene> makePostfxScene(bool fxaa);

// ---------------------------------------------------------------------------------------------
// Math (column-major matrices, Vulkan clip space: y down, depth 0..1)

struct Vec3 {
    float x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0.0f ? a * (1.0f / l) : a;
}

struct Mat4 {
    float m[16];
};
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
inline Mat4 perspective(float fovyRad, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;  // Vulkan clip space has y pointing down
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = zNear * zFar / (zNear - zFar);
    return r;
}
inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = normalize(center - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r{};
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    r.m[15] = 1.0f;
    return r;
}

// Deterministic random numbers for procedural content.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = static_cast<uint32_t>(((state >> 18u) ^ state) >> 27u);
        uint32_t rot = static_cast<uint32_t>(state >> 59u);
        return (x >> rot) | (x << ((32u - rot) & 31u));
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
};

// ---------------------------------------------------------------------------------------------
// Instanced PBR meshes (icospheres and tori), used by "instanced" and as the "postfx" scene.

class InstancedRenderer {
public:
    void init(VkContext& ctx, ResourceList& rl, uint32_t instanceCount);
    VkPipeline createPipeline(VkContext& ctx, ResourceList& rl, VkRenderPass pass, VkSampleCountFlagBits samples,
                              bool hdrOut);
    void update(uint32_t slot, double time, float aspect);
    void draw(VkCommandBuffer cmd, uint32_t slot, VkPipeline pipeline);
    uint32_t instanceCount() const { return sphereInstances_ + torusInstances_; }

private:
    struct Mesh {
        Buffer vertices, indices;
        uint32_t indexCount = 0;
    };
    Mesh sphere_, torus_;
    Buffer instances_;
    uint32_t sphereInstances_ = 0, torusInstances_ = 0;
    Buffer ubo_[kFramesInFlight];
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kFramesInFlight]{};
};
