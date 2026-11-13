// Scene 1: full-screen SDF raymarcher (menger fractal, repeated twisted pillars, blended glowing
// orbs, terrain) with soft shadows, ambient occlusion, one reflection bounce and fog. ALU-heavy.
#include "scenes/scenes.h"

#include "fullscreen_vert.h"
#include "raymarch_frag.h"

#include <vector>

namespace {

struct RaymarchPush {
    float res[4];     // width, height, 1/width, 1/height
    float params[4];  // time
};

class RaymarchScene : public Scene {
public:
    const char* name() const override { return "raymarch"; }

    void init(VkContext& ctx) override {
        layout_ = ctx.createPipelineLayout(persistent, {}, sizeof(RaymarchPush), VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    void createSized(VkContext& ctx) override {
        RenderPassDesc rp;
        rp.color = ctx.swapFormat;
        rp.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
        rp.final = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        pass_ = ctx.createRenderPass(sized, rp);
        framebuffers_.clear();
        for (VkImageView v : ctx.swapViews)
            framebuffers_.push_back(ctx.createFramebuffer(sized, pass_, {v}, ctx.swapExtent));
        PipelineDesc pd;
        pd.vs = SPV(fullscreen_vert);
        pd.fs = SPV(raymarch_frag);
        pd.renderPass = pass_;
        pd.layout = layout_;
        pipeline_ = ctx.createGraphicsPipeline(sized, pd);
    }

    void record(VkContext& ctx, const FrameInfo& fi) override {
        const VkExtent2D e = ctx.swapExtent;
        RaymarchPush pc{{float(e.width), float(e.height), 1.0f / e.width, 1.0f / e.height},
                        {float(fi.time), 0.0f, 0.0f, 0.0f}};
        VkContext::beginRenderPass(fi.cmd, pass_, framebuffers_[fi.imageIndex], e, {});
        vkCmdBindPipeline(fi.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(fi.cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(fi.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(fi.cmd);
    }

private:
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
};

}  // namespace

std::unique_ptr<Scene> makeRaymarchScene() { return std::make_unique<RaymarchScene>(); }
