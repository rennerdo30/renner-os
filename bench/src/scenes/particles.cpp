// Scene 3: 1M particles simulated in a compute shader (curl noise plus three moving attractors),
// drawn as additive camera-facing quads into an RGBA16F target, then tonemapped to the swapchain.
// Compute- and bandwidth-heavy.
#include "scenes/scenes.h"

#include "composite_frag.h"
#include "fullscreen_vert.h"
#include "particles_comp.h"
#include "particles_frag.h"
#include "particles_vert.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kParticleCount = 1u << 20;  // 1,048,576
constexpr uint32_t kGroupSize = 256;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct SimPush {
    float time;
    float dt;
    uint32_t count;
    uint32_t mode;  // 0 = initialize, 1 = step
    float attractor[3][4];
};

struct DrawPush {
    float viewProj[16];
    float right[4];  // xyz camera right, w particle size
    float up[4];
};

struct CompositePush {
    float params[4];  // exposure
};

class ParticlesScene : public Scene {
public:
    const char* name() const override { return "particles"; }

    void init(VkContext& ctx) override {
        particles_ = ctx.createBuffer(persistent, VkDeviceSize(kParticleCount) * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        bufferLayout_ = ctx.createSetLayout(persistent, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
        textureLayout_ = ctx.createSetLayout(persistent, {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                           VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
        simLayout_ = ctx.createPipelineLayout(persistent, {bufferLayout_}, sizeof(SimPush), VK_SHADER_STAGE_COMPUTE_BIT);
        drawLayout_ = ctx.createPipelineLayout(persistent, {bufferLayout_}, sizeof(DrawPush), VK_SHADER_STAGE_VERTEX_BIT);
        compositeLayout_ = ctx.createPipelineLayout(persistent, {textureLayout_}, sizeof(CompositePush),
                                                    VK_SHADER_STAGE_FRAGMENT_BIT);
        VkDescriptorPool pool = ctx.createDescriptorPool(persistent, 1, {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}});
        bufferSet_ = ctx.allocateSet(pool, bufferLayout_);
        ctx.writeBuffer(bufferSet_, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, particles_.buffer, VK_WHOLE_SIZE);
        simPipeline_ = ctx.createComputePipeline(persistent, SPV(particles_comp), simLayout_);
        sampler_ = ctx.createSampler(persistent);
        initialized_ = false;
    }

    void createSized(VkContext& ctx) override {
        const VkExtent2D e = ctx.swapExtent;
        hdr_ = ctx.createImage(sized, kHdrFormat, e, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        RenderPassDesc hp;
        hp.color = kHdrFormat;
        hp.load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        hp.final = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        hdrPass_ = ctx.createRenderPass(sized, hp);
        hdrFramebuffer_ = ctx.createFramebuffer(sized, hdrPass_, {hdr_.view}, e);

        RenderPassDesc sp;
        sp.color = ctx.swapFormat;
        sp.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        sp.final = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapPass_ = ctx.createRenderPass(sized, sp);
        swapFramebuffers_.clear();
        for (VkImageView v : ctx.swapViews) swapFramebuffers_.push_back(ctx.createFramebuffer(sized, swapPass_, {v}, e));

        VkDescriptorPool pool = ctx.createDescriptorPool(sized, 1, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}});
        textureSet_ = ctx.allocateSet(pool, textureLayout_);
        ctx.writeImage(textureSet_, 0, hdr_.view, sampler_);

        PipelineDesc pd;
        pd.vs = SPV(particles_vert);
        pd.fs = SPV(particles_frag);
        pd.renderPass = hdrPass_;
        pd.layout = drawLayout_;
        pd.blend = Blend::Additive;
        drawPipeline_ = ctx.createGraphicsPipeline(sized, pd);

        PipelineDesc cd;
        cd.vs = SPV(fullscreen_vert);
        cd.fs = SPV(composite_frag);
        cd.renderPass = swapPass_;
        cd.layout = compositeLayout_;
        compositePipeline_ = ctx.createGraphicsPipeline(sized, cd);
    }

    void record(VkContext& ctx, const FrameInfo& fi) override {
        const VkExtent2D e = ctx.swapExtent;
        const float t = static_cast<float>(fi.time);
        VkCommandBuffer cmd = fi.cmd;

        // Simulation. The previous frame's draw reads (and simulation writes) must finish first.
        VkMemoryBarrier pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &pre, 0, nullptr, 0, nullptr);
        SimPush sim{};
        sim.time = t;
        sim.dt = std::clamp(fi.dt, 0.0f, 1.0f / 30.0f);
        sim.count = kParticleCount;
        sim.mode = initialized_ ? 1u : 0u;
        for (int i = 0; i < 3; ++i) {
            float ph = i * 2.094f;
            sim.attractor[i][0] = 3.2f * std::sin(t * (0.31f + 0.07f * i) + ph);
            sim.attractor[i][1] = 1.8f * std::sin(t * (0.23f + 0.05f * i) + ph * 1.7f);
            sim.attractor[i][2] = 3.2f * std::cos(t * (0.27f + 0.06f * i) + ph);
            sim.attractor[i][3] = 2.2f + 0.8f * std::sin(t * 0.5f + i);
        }
        initialized_ = true;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simLayout_, 0, 1, &bufferSet_, 0, nullptr);
        vkCmdPushConstants(cmd, simLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof sim, &sim);
        vkCmdDispatch(cmd, (kParticleCount + kGroupSize - 1) / kGroupSize, 1, 1);

        VkMemoryBarrier post{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &post,
                             0, nullptr, 0, nullptr);

        // Additive particle quads into the HDR target.
        const float a = t * 0.12f;
        Vec3 eye{std::cos(a) * 9.5f, 2.5f + 1.5f * std::sin(t * 0.17f), std::sin(a) * 9.5f};
        Vec3 fwd = normalize(Vec3{0, 0, 0} - eye);
        Vec3 right = normalize(cross(fwd, {0, 1, 0}));
        Vec3 up = cross(right, fwd);
        Mat4 vp = perspective(60.0f * 3.14159265f / 180.0f, float(e.width) / float(e.height), 0.1f, 100.0f) *
                  lookAt(eye, {0, 0, 0}, {0, 1, 0});
        DrawPush dp{};
        std::memcpy(dp.viewProj, vp.m, sizeof vp.m);
        dp.right[0] = right.x;
        dp.right[1] = right.y;
        dp.right[2] = right.z;
        dp.right[3] = 0.022f;
        dp.up[0] = up.x;
        dp.up[1] = up.y;
        dp.up[2] = up.z;
        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkContext::beginRenderPass(cmd, hdrPass_, hdrFramebuffer_, e, {clear});
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout_, 0, 1, &bufferSet_, 0, nullptr);
        vkCmdPushConstants(cmd, drawLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof dp, &dp);
        vkCmdDraw(cmd, kParticleCount * 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        // Tonemap to the swapchain.
        CompositePush cp{{2.2f, 0.0f, 0.0f, 0.0f}};
        VkContext::beginRenderPass(cmd, swapPass_, swapFramebuffers_[fi.imageIndex], e, {});
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1, &textureSet_, 0, nullptr);
        vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof cp, &cp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

private:
    Buffer particles_;
    bool initialized_ = false;
    VkDescriptorSetLayout bufferLayout_ = VK_NULL_HANDLE, textureLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout simLayout_ = VK_NULL_HANDLE, drawLayout_ = VK_NULL_HANDLE, compositeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet bufferSet_ = VK_NULL_HANDLE, textureSet_ = VK_NULL_HANDLE;
    VkPipeline simPipeline_ = VK_NULL_HANDLE, drawPipeline_ = VK_NULL_HANDLE, compositePipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    Image hdr_;
    VkRenderPass hdrPass_ = VK_NULL_HANDLE, swapPass_ = VK_NULL_HANDLE;
    VkFramebuffer hdrFramebuffer_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapFramebuffers_;
};

}  // namespace

std::unique_ptr<Scene> makeParticlesScene() { return std::make_unique<ParticlesScene>(); }
