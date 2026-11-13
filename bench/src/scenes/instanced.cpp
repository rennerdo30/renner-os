// Scene 2: 240k instances of procedural icospheres and tori, animated in the vertex shader, lit
// with GGX by a sun and 8 moving point lights, depth buffer and 4x MSAA. Vertex- and raster-heavy.
#include "scenes/scenes.h"

#include "instanced_frag.h"
#include "instanced_vert.h"

#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr uint32_t kInstancedSceneCount = 240000;
constexpr int kLights = 8;

struct Vertex {
    float pos[3];
    float normal[3];
};

struct Instance {
    float posScale[4];   // xyz orbit position, w scale
    float axisPhase[4];  // xyz spin axis, w phase
    float color[4];      // rgb albedo, a emissive strength
    float params[4];     // metallic, roughness, spin speed, orbit speed
};

struct FrameUniforms {
    float viewProj[16];
    float camPos[4];
    float params[4];  // time
    float sunDir[4];
    float sunColor[4];
    float lightPos[kLights][4];    // xyz, w radius
    float lightColor[kLights][4];  // rgb intensity
};

// Makes every triangle counter-clockwise when seen from the side its normals point to.
void fixWinding(const std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Vertex &a = v[idx[i]], &b = v[idx[i + 1]], &c = v[idx[i + 2]];
        Vec3 pa{a.pos[0], a.pos[1], a.pos[2]}, pb{b.pos[0], b.pos[1], b.pos[2]}, pc{c.pos[0], c.pos[1], c.pos[2]};
        Vec3 n{a.normal[0] + b.normal[0] + c.normal[0], a.normal[1] + b.normal[1] + c.normal[1],
               a.normal[2] + b.normal[2] + c.normal[2]};
        if (dot(cross(pb - pa, pc - pa), n) < 0.0f) std::swap(idx[i + 1], idx[i + 2]);
    }
}

void makeIcosphere(int subdivisions, std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Vec3> p = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (auto& q : p) q = normalize(q);
    std::vector<uint32_t> tri = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2,
                                 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5,
                                 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> cache;
        auto mid = [&](uint32_t a, uint32_t b) {
            auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;
            p.push_back(normalize((p[a] + p[b]) * 0.5f));
            uint32_t id = static_cast<uint32_t>(p.size() - 1);
            cache[key] = id;
            return id;
        };
        std::vector<uint32_t> next;
        for (size_t i = 0; i < tri.size(); i += 3) {
            uint32_t a = tri[i], b = tri[i + 1], c = tri[i + 2];
            uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        tri.swap(next);
    }
    verts.clear();
    for (auto& q : p) verts.push_back({{q.x, q.y, q.z}, {q.x, q.y, q.z}});
    idx = tri;
    fixWinding(verts, idx);
}

void makeTorus(int major, int minor, float R, float r, std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    verts.clear();
    idx.clear();
    const float twoPi = 6.28318530718f;
    for (int i = 0; i < major; ++i) {
        float u = twoPi * i / major;
        for (int j = 0; j < minor; ++j) {
            float v = twoPi * j / minor;
            Vec3 n{std::cos(v) * std::cos(u), std::sin(v), std::cos(v) * std::sin(u)};
            Vec3 c{R * std::cos(u), 0.0f, R * std::sin(u)};
            Vec3 q = c + n * r;
            verts.push_back({{q.x, q.y, q.z}, {n.x, n.y, n.z}});
        }
    }
    for (int i = 0; i < major; ++i)
        for (int j = 0; j < minor; ++j) {
            uint32_t a = i * minor + j, b = ((i + 1) % major) * minor + j;
            uint32_t c = ((i + 1) % major) * minor + (j + 1) % minor, d = i * minor + (j + 1) % minor;
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    fixWinding(verts, idx);
}

void hsvToRgb(float h, float s, float v, float* out) {
    float r = std::fabs(h * 6.0f - 3.0f) - 1.0f, g = 2.0f - std::fabs(h * 6.0f - 2.0f), b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    auto sat = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    out[0] = v * (1.0f - s + s * sat(r));
    out[1] = v * (1.0f - s + s * sat(g));
    out[2] = v * (1.0f - s + s * sat(b));
}

}  // namespace

void InstancedRenderer::init(VkContext& ctx, ResourceList& rl, uint32_t count) {
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    makeIcosphere(2, v, i);  // 162 vertices, 320 triangles
    sphere_.vertices = ctx.createDeviceBuffer(rl, v.data(), v.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    sphere_.indices = ctx.createDeviceBuffer(rl, i.data(), i.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    sphere_.indexCount = static_cast<uint32_t>(i.size());
    makeTorus(24, 12, 1.0f, 0.38f, v, i);  // 288 vertices, 576 triangles
    torus_.vertices = ctx.createDeviceBuffer(rl, v.data(), v.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    torus_.indices = ctx.createDeviceBuffer(rl, i.data(), i.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    torus_.indexCount = static_cast<uint32_t>(i.size());

    // A flat galaxy-like disc of objects; the first half are spheres, the second half tori.
    sphereInstances_ = count / 2;
    torusInstances_ = count - sphereInstances_;
    std::vector<Instance> inst(count);
    Rng rng(1234);
    for (uint32_t k = 0; k < count; ++k) {
        Instance& o = inst[k];
        float radius = 4.0f + 116.0f * std::sqrt(rng.uniform());
        float angle = rng.range(0.0f, 6.2831853f);
        float arm = std::sin(angle * 2.0f + radius * 0.08f) * 0.5f + 0.5f;  // spiral-arm density bias
        float height = rng.range(-1.0f, 1.0f) * (2.0f + 10.0f * (1.0f - radius / 130.0f)) * (0.4f + 0.6f * arm);
        o.posScale[0] = std::cos(angle) * radius;
        o.posScale[1] = height;
        o.posScale[2] = std::sin(angle) * radius;
        o.posScale[3] = rng.range(0.25f, 0.75f);
        Vec3 raw{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
        Vec3 axis = dot(raw, raw) < 0.01f ? Vec3{0, 1, 0} : normalize(raw);
        o.axisPhase[0] = axis.x;
        o.axisPhase[1] = axis.y;
        o.axisPhase[2] = axis.z;
        o.axisPhase[3] = rng.range(0.0f, 6.2831853f);
        hsvToRgb(std::fmod(1.5f + radius * 0.007f + rng.range(-0.12f, 0.12f), 1.0f), rng.range(0.3f, 0.8f),
                 rng.range(0.5f, 0.95f), o.color);
        o.color[3] = rng.uniform() < 0.02f ? rng.range(3.0f, 9.0f) : 0.0f;  // a few glowing objects
        o.params[0] = rng.uniform() < 0.35f ? 1.0f : 0.0f;                  // metallic
        o.params[1] = rng.range(0.15f, 0.85f);                              // roughness
        o.params[2] = rng.range(0.4f, 2.5f);                                // spin speed
        o.params[3] = 3.0f / (radius + 12.0f);                              // orbit speed (inner faster)
    }
    instances_ = ctx.createDeviceBuffer(rl, inst.data(), inst.size() * sizeof(Instance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    setLayout_ = ctx.createSetLayout(rl, {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    layout_ = ctx.createPipelineLayout(rl, {setLayout_}, 0, 0);
    pool_ = ctx.createDescriptorPool(rl, kFramesInFlight, {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}});
    for (uint32_t s = 0; s < kFramesInFlight; ++s) {
        ubo_[s] = ctx.createBuffer(rl, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        sets_[s] = ctx.allocateSet(pool_, setLayout_);
        ctx.writeBuffer(sets_[s], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ubo_[s].buffer, sizeof(FrameUniforms));
    }
}

VkPipeline InstancedRenderer::createPipeline(VkContext& ctx, ResourceList& rl, VkRenderPass pass,
                                             VkSampleCountFlagBits samples, bool hdrOut) {
    PipelineDesc pd;
    pd.vs = SPV(instanced_vert);
    pd.fs = SPV(instanced_frag);
    pd.renderPass = pass;
    pd.layout = layout_;
    pd.samples = samples;
    pd.bindings = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    pd.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                     {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                     {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                     {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                     {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                     {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48}};
    pd.cull = VK_CULL_MODE_BACK_BIT;
    pd.depthTest = true;
    pd.depthWrite = true;
    pd.hdrOut = hdrOut;
    return ctx.createGraphicsPipeline(rl, pd);
}

void InstancedRenderer::update(uint32_t slot, double time, float aspect) {
    const float t = static_cast<float>(time);
    FrameUniforms u{};
    const float a = t * 0.06f;
    Vec3 eye{std::cos(a) * 72.0f, 20.0f + 7.0f * std::sin(t * 0.11f), std::sin(a) * 72.0f};
    Vec3 target{0.0f, 0.0f, 0.0f};
    Mat4 vp = perspective(55.0f * 3.14159265f / 180.0f, aspect, 0.5f, 400.0f) * lookAt(eye, target, {0, 1, 0});
    std::memcpy(u.viewProj, vp.m, sizeof vp.m);
    u.camPos[0] = eye.x;
    u.camPos[1] = eye.y;
    u.camPos[2] = eye.z;
    u.params[0] = t;
    Vec3 sun = normalize({0.4f, 0.8f, 0.3f});
    u.sunDir[0] = sun.x;
    u.sunDir[1] = sun.y;
    u.sunDir[2] = sun.z;
    u.sunColor[0] = 1.2f;
    u.sunColor[1] = 1.1f;
    u.sunColor[2] = 1.0f;
    const float colors[kLights][3] = {{1.0f, 0.4f, 0.1f}, {0.2f, 0.5f, 1.0f}, {1.0f, 0.2f, 0.6f}, {0.3f, 1.0f, 0.4f},
                                      {1.0f, 0.9f, 0.4f}, {0.5f, 0.3f, 1.0f}, {0.2f, 1.0f, 0.9f}, {1.0f, 0.5f, 0.3f}};
    for (int l = 0; l < kLights; ++l) {
        float r = 15.0f + 11.0f * l;
        float ang = t * (0.25f + 0.04f * l) + l * 0.785f;
        u.lightPos[l][0] = std::cos(ang) * r;
        u.lightPos[l][1] = 6.0f + 3.0f * std::sin(t * 0.7f + l);
        u.lightPos[l][2] = std::sin(ang) * r;
        u.lightPos[l][3] = 45.0f;  // influence radius
        for (int c = 0; c < 3; ++c) u.lightColor[l][c] = colors[l][c] * 220.0f;
    }
    std::memcpy(ubo_[slot].mapped, &u, sizeof u);
}

void InstancedRenderer::draw(VkCommandBuffer cmd, uint32_t slot, VkPipeline pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[slot], 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &instances_.buffer, &zero);

    vkCmdBindVertexBuffers(cmd, 0, 1, &sphere_.vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, sphere_.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, sphere_.indexCount, sphereInstances_, 0, 0, 0);

    vkCmdBindVertexBuffers(cmd, 0, 1, &torus_.vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, torus_.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, torus_.indexCount, torusInstances_, 0, 0, sphereInstances_);
}

namespace {

class InstancedScene : public Scene {
public:
    const char* name() const override { return "instanced"; }

    void init(VkContext& ctx) override {
        renderer_.init(ctx, persistent, kInstancedSceneCount);
        samples_ = ctx.msaaSamples;
    }

    void createSized(VkContext& ctx) override {
        const VkExtent2D e = ctx.swapExtent;
        const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
        RenderPassDesc rp;
        rp.color = ctx.swapFormat;
        rp.samples = samples_;
        rp.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        rp.final = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        rp.depth = ctx.depthFormat;
        rp.resolve = msaa;
        pass_ = ctx.createRenderPass(sized, rp);
        Image depth = ctx.createImage(sized, ctx.depthFormat, e,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                      samples_, VK_IMAGE_ASPECT_DEPTH_BIT);
        Image color;
        if (msaa)
            color = ctx.createImage(sized, ctx.swapFormat, e,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples_,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        framebuffers_.clear();
        for (VkImageView v : ctx.swapViews) {
            if (msaa)
                framebuffers_.push_back(ctx.createFramebuffer(sized, pass_, {color.view, depth.view, v}, e));
            else
                framebuffers_.push_back(ctx.createFramebuffer(sized, pass_, {v, depth.view}, e));
        }
        pipeline_ = renderer_.createPipeline(ctx, sized, pass_, samples_, false);
    }

    void record(VkContext& ctx, const FrameInfo& fi) override {
        const VkExtent2D e = ctx.swapExtent;
        renderer_.update(fi.slot, fi.time, float(e.width) / float(e.height));
        VkClearValue color{}, depth{};
        color.color = {{0.004f, 0.006f, 0.012f, 1.0f}};
        depth.depthStencil = {1.0f, 0};
        VkContext::beginRenderPass(fi.cmd, pass_, framebuffers_[fi.imageIndex], e, {color, depth});
        renderer_.draw(fi.cmd, fi.slot, pipeline_);
        vkCmdEndRenderPass(fi.cmd);
    }

private:
    InstancedRenderer renderer_;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
};

}  // namespace

std::unique_ptr<Scene> makeInstancedScene() { return std::make_unique<InstancedScene>(); }
