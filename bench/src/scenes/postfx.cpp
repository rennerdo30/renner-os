// Scene 4: the instanced scene at a lower count rendered to RGBA16F, then a bloom chain (threshold,
// 6 downsample and 5 upsample passes), ACES tonemapping to an LDR target and FXAA to the swapchain.
// Bandwidth- and fill-rate-heavy.
#include "scenes/scenes.h"

#include "bloom_down_frag.h"
#include "bloom_up_frag.h"
#include "fullscreen_vert.h"
#include "fxaa_frag.h"
#include "post_tonemap_frag.h"

#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t kPostfxInstances = 80000;
constexpr int kBloomLevels = 6;  // 1/2 .. 1/64 of the screen
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct PostPush {
    float params[4];
};

class PostfxScene : public Scene {
public:
    explicit PostfxScene(bool fxaa) : fxaa_(fxaa) {}
    const char* name() const override { return "postfx"; }

    void init(VkContext& ctx) override {
        renderer_.init(ctx, persistent, kPostfxInstances);
        sampler_ = ctx.createSampler(persistent);
        oneTex_ = ctx.createSetLayout(persistent, {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
        twoTex_ = ctx.createSetLayout(persistent, {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
        layout1_ = ctx.createPipelineLayout(persistent, {oneTex_}, sizeof(PostPush), VK_SHADER_STAGE_FRAGMENT_BIT);
        layout2_ = ctx.createPipelineLayout(persistent, {twoTex_}, sizeof(PostPush), VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    void createSized(VkContext& ctx) override {
        const VkExtent2D e = ctx.swapExtent;
        const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        // Scene pass: HDR color + depth.
        hdr_ = ctx.createImage(sized, kHdrFormat, e, rt, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth = ctx.createImage(sized, ctx.depthFormat, e,
                                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        RenderPassDesc sp;
        sp.color = kHdrFormat;
        sp.depth = ctx.depthFormat;
        scenePass_ = ctx.createRenderPass(sized, sp);
        sceneFb_ = ctx.createFramebuffer(sized, scenePass_, {hdr_.view, depth.view}, e);
        scenePipeline_ = renderer_.createPipeline(ctx, sized, scenePass_, VK_SAMPLE_COUNT_1_BIT, true);

        // Bloom levels.
        RenderPassDesc dp;
        dp.color = kHdrFormat;
        dp.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        downPass_ = ctx.createRenderPass(sized, dp);
        RenderPassDesc up;
        up.color = kHdrFormat;
        up.load = VK_ATTACHMENT_LOAD_OP_LOAD;
        up.initial = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        upPass_ = ctx.createRenderPass(sized, up);
        for (int i = 0; i < kBloomLevels; ++i) {
            VkExtent2D le{std::max(1u, e.width >> (i + 1)), std::max(1u, e.height >> (i + 1))};
            bloom_[i] = ctx.createImage(sized, kHdrFormat, le, rt, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            downFb_[i] = ctx.createFramebuffer(sized, downPass_, {bloom_[i].view}, le);
            upFb_[i] = ctx.createFramebuffer(sized, upPass_, {bloom_[i].view}, le);
        }

        // Tonemap into LDR, then FXAA into the swapchain.
        ldr_ = ctx.createImage(sized, kLdrFormat, e, rt, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        RenderPassDesc tp;
        tp.color = kLdrFormat;
        tp.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        tonemapPass_ = ctx.createRenderPass(sized, tp);
        tonemapFb_ = ctx.createFramebuffer(sized, tonemapPass_, {ldr_.view}, e);
        RenderPassDesc fp;
        fp.color = ctx.swapFormat;
        fp.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fp.final = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapPass_ = ctx.createRenderPass(sized, fp);
        swapFbs_.clear();
        for (VkImageView v : ctx.swapViews) swapFbs_.push_back(ctx.createFramebuffer(sized, swapPass_, {v}, e));

        // Descriptor sets.
        VkDescriptorPool pool = ctx.createDescriptorPool(sized, 2 * kBloomLevels + 2,
                                                         {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kBloomLevels + 3}});
        for (int i = 0; i < kBloomLevels; ++i) {
            downSet_[i] = ctx.allocateSet(pool, oneTex_);
            ctx.writeImage(downSet_[i], 0, i == 0 ? hdr_.view : bloom_[i - 1].view, sampler_);
            if (i < kBloomLevels - 1) {
                upSet_[i] = ctx.allocateSet(pool, oneTex_);
                ctx.writeImage(upSet_[i], 0, bloom_[i + 1].view, sampler_);
            }
        }
        tonemapSet_ = ctx.allocateSet(pool, twoTex_);
        ctx.writeImage(tonemapSet_, 0, hdr_.view, sampler_);
        ctx.writeImage(tonemapSet_, 1, bloom_[0].view, sampler_);
        fxaaSet_ = ctx.allocateSet(pool, oneTex_);
        ctx.writeImage(fxaaSet_, 0, ldr_.view, sampler_);

        // Pipelines.
        PipelineDesc pd;
        pd.vs = SPV(fullscreen_vert);
        pd.fs = SPV(bloom_down_frag);
        pd.renderPass = downPass_;
        pd.layout = layout1_;
        downPipeline_ = ctx.createGraphicsPipeline(sized, pd);
        pd.fs = SPV(bloom_up_frag);
        pd.renderPass = upPass_;
        pd.blend = Blend::Additive;
        upPipeline_ = ctx.createGraphicsPipeline(sized, pd);
        pd.fs = SPV(post_tonemap_frag);
        pd.renderPass = tonemapPass_;
        pd.layout = layout2_;
        pd.blend = Blend::None;
        tonemapPipeline_ = ctx.createGraphicsPipeline(sized, pd);
        pd.fs = SPV(fxaa_frag);
        pd.renderPass = swapPass_;
        pd.layout = layout1_;
        fxaaPipeline_ = ctx.createGraphicsPipeline(sized, pd);
    }

    void fullscreen(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set, const PostPush& pc) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    void record(VkContext& ctx, const FrameInfo& fi) override {
        const VkExtent2D e = ctx.swapExtent;
        VkCommandBuffer cmd = fi.cmd;

        renderer_.update(fi.slot, fi.time + 40.0, float(e.width) / float(e.height));  // other viewpoint than "instanced"
        VkClearValue color{}, depth{};
        color.color = {{0.004f, 0.006f, 0.012f, 1.0f}};
        depth.depthStencil = {1.0f, 0};
        VkContext::beginRenderPass(cmd, scenePass_, sceneFb_, e, {color, depth});
        renderer_.draw(cmd, fi.slot, scenePipeline_);
        vkCmdEndRenderPass(cmd);

        // Downsample chain; the first pass applies the soft threshold.
        for (int i = 0; i < kBloomLevels; ++i) {
            VkExtent2D src = i == 0 ? e : bloom_[i - 1].extent;
            PostPush pc{{1.0f / src.width, 1.0f / src.height, 1.0f, i == 0 ? 1.0f : 0.0f}};
            VkContext::beginRenderPass(cmd, downPass_, downFb_[i], bloom_[i].extent, {});
            fullscreen(cmd, downPipeline_, layout1_, downSet_[i], pc);
            vkCmdEndRenderPass(cmd);
        }
        // Upsample chain: add the blurred smaller level onto the next larger one.
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            VkExtent2D src = bloom_[i + 1].extent;
            PostPush pc{{1.0f / src.width, 1.0f / src.height, 1.0f, 0.0f}};
            VkContext::beginRenderPass(cmd, upPass_, upFb_[i], bloom_[i].extent, {});
            fullscreen(cmd, upPipeline_, layout1_, upSet_[i], pc);
            vkCmdEndRenderPass(cmd);
        }

        PostPush tp{{1.0f, 0.35f, 0.0f, 0.0f}};  // exposure, bloom strength
        VkContext::beginRenderPass(cmd, tonemapPass_, tonemapFb_, e, {});
        fullscreen(cmd, tonemapPipeline_, layout2_, tonemapSet_, tp);
        vkCmdEndRenderPass(cmd);

        PostPush fp{{1.0f / e.width, 1.0f / e.height, fxaa_ ? 1.0f : 0.0f, 0.0f}};
        VkContext::beginRenderPass(cmd, swapPass_, swapFbs_[fi.imageIndex], e, {});
        fullscreen(cmd, fxaaPipeline_, layout1_, fxaaSet_, fp);
        vkCmdEndRenderPass(cmd);
    }

private:
    bool fxaa_;
    InstancedRenderer renderer_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout oneTex_ = VK_NULL_HANDLE, twoTex_ = VK_NULL_HANDLE;
    VkPipelineLayout layout1_ = VK_NULL_HANDLE, layout2_ = VK_NULL_HANDLE;

    Image hdr_, ldr_, bloom_[kBloomLevels];
    VkRenderPass scenePass_ = VK_NULL_HANDLE, downPass_ = VK_NULL_HANDLE, upPass_ = VK_NULL_HANDLE;
    VkRenderPass tonemapPass_ = VK_NULL_HANDLE, swapPass_ = VK_NULL_HANDLE;
    VkFramebuffer sceneFb_ = VK_NULL_HANDLE, tonemapFb_ = VK_NULL_HANDLE;
    VkFramebuffer downFb_[kBloomLevels]{}, upFb_[kBloomLevels]{};
    std::vector<VkFramebuffer> swapFbs_;
    VkDescriptorSet downSet_[kBloomLevels]{}, upSet_[kBloomLevels]{}, tonemapSet_ = VK_NULL_HANDLE, fxaaSet_ = VK_NULL_HANDLE;
    VkPipeline scenePipeline_ = VK_NULL_HANDLE, downPipeline_ = VK_NULL_HANDLE, upPipeline_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE, fxaaPipeline_ = VK_NULL_HANDLE;
};

}  // namespace

std::unique_ptr<Scene> makePostfxScene(bool fxaa) { return std::make_unique<PostfxScene>(fxaa); }
