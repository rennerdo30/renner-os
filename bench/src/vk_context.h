// Vulkan context: instance, device, swapchain, frame loop and small resource helpers.
// Uses only core Vulkan 1.2 plus VK_KHR_swapchain (and VK_EXT_debug_utils with --validate).
#pragma once

#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

constexpr uint32_t kFramesInFlight = 2;

const char* vkResultName(VkResult r);

// Thrown for every failed Vulkan call; carries the call text and the VkResult.
class VulkanError : public std::runtime_error {
public:
    VulkanError(const std::string& call, VkResult r, const char* file, int line);
    VkResult result;
};

#define VK_CHECK(expr)                                                     \
    do {                                                                   \
        VkResult vkr_ = (expr);                                            \
        if (vkr_ != VK_SUCCESS) throw VulkanError(#expr, vkr_, __FILE__, __LINE__); \
    } while (0)

// Destruction callbacks, run in reverse creation order by flush().
class ResourceList {
public:
    void add(std::function<void()> fn) { fns_.push_back(std::move(fn)); }
    void flush() {
        for (auto it = fns_.rbegin(); it != fns_.rend(); ++it) (*it)();
        fns_.clear();
    }

private:
    std::vector<std::function<void()>> fns_;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // persistently mapped when host visible
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

struct ShaderCode {
    const uint32_t* code;
    size_t bytes;
};
#define SPV(arr) ShaderCode{arr, sizeof(arr)}

// One subpass, one color attachment, optional depth and optional MSAA resolve.
struct RenderPassDesc {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkImageLayout initial = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout final = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // of the resolve attachment when resolving
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool resolve = false;  // attachments: 0 MSAA color, 1 depth, 2 resolve target
};

enum class Blend { None, Additive };

struct PipelineDesc {
    ShaderCode vs{};
    ShaderCode fs{};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    bool depthTest = false;
    bool depthWrite = false;
    Blend blend = Blend::None;
    bool hdrOut = false;  // specialization constant 1
};

enum class PresentPref { Immediate, Mailbox, Fifo };

struct ContextOptions {
    bool validate = false;
    int gpuIndex = -1;
    PresentPref present = PresentPref::Immediate;
};

struct FrameInfo {
    VkCommandBuffer cmd;
    uint32_t slot;        // frame-in-flight index, 0..kFramesInFlight-1
    uint32_t imageIndex;  // swapchain image
    double time;          // seconds since scene start (drives all animation)
    float dt;             // seconds since the previous frame
};

class VkContext {
public:
    void init(SDL_Window* window, const ContextOptions& opt);
    void shutdown();
    static int listGpus();

    // Swapchain
    bool surfaceHasArea();  // false while minimized
    void recreateSwapchain();
    void waitIdle();

    // Renders one frame. Returns true when the swapchain must be recreated.
    // With capturePath set, the frame is also copied back and written as a 24-bit BMP.
    bool drawFrame(double time, float dt, bool measured, const std::function<void(const FrameInfo&)>& record,
                   const std::string* capturePath = nullptr);
    // Waits for the GPU and collects the remaining timestamps.
    void finishFrames();
    std::vector<double> takeGpuTimes();

    // Resource helpers. Each registers its destruction in the given list.
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    Buffer createBuffer(ResourceList& rl, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    Buffer createDeviceBuffer(ResourceList& rl, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    Image createImage(ResourceList& rl, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                      VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
    VkRenderPass createRenderPass(ResourceList& rl, const RenderPassDesc& d);
    VkFramebuffer createFramebuffer(ResourceList& rl, VkRenderPass pass, std::initializer_list<VkImageView> views,
                                    VkExtent2D extent);
    VkSampler createSampler(ResourceList& rl);
    VkDescriptorSetLayout createSetLayout(ResourceList& rl, std::initializer_list<VkDescriptorSetLayoutBinding> b);
    VkPipelineLayout createPipelineLayout(ResourceList& rl, std::initializer_list<VkDescriptorSetLayout> sets,
                                          uint32_t pushBytes, VkShaderStageFlags pushStages);
    VkDescriptorPool createDescriptorPool(ResourceList& rl, uint32_t maxSets,
                                          std::initializer_list<VkDescriptorPoolSize> sizes);
    VkDescriptorSet allocateSet(VkDescriptorPool pool, VkDescriptorSetLayout layout);
    void writeImage(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler);
    void writeBuffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize size);
    VkPipeline createGraphicsPipeline(ResourceList& rl, const PipelineDesc& d);
    VkPipeline createComputePipeline(ResourceList& rl, ShaderCode cs, VkPipelineLayout layout);
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

    static void beginRenderPass(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, VkExtent2D extent,
                                std::initializer_list<VkClearValue> clears);
    static void setViewport(VkCommandBuffer cmd, VkExtent2D extent);

    // Public state, read by scenes and main.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool manualGamma = false;  // swapchain is UNORM: shaders encode sRGB themselves
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;

    int gpuIndex = 0;
    VkPhysicalDeviceProperties props{};
    std::string driverName, driverInfo;
    bool timestamps = false;
    bool canCapture = false;  // swapchain supports TRANSFER_SRC and a known 8-bit RGBA/BGRA format
    bool validationEnabled = false;
    bool syncValidationEnabled = false;  // VK_EXT_validation_features: synchronization validation
    uint32_t validationErrors = 0;
    uint32_t validationWarnings = 0;

private:
    void createInstance(SDL_Window* window, bool validate);
    void pickDevice(int requested);
    void createDevice();
    void createSwapchain();
    void destroySwapchainViews();
    void collectTimestamps(uint32_t slot);

    SDL_Window* window_ = nullptr;
    PresentPref presentPref_ = PresentPref::Immediate;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[kFramesInFlight]{};
    VkFence fences_[kFramesInFlight]{};
    VkSemaphore imageAvailable_[kFramesInFlight]{};
    std::vector<VkSemaphore> renderDone_;  // one per swapchain image
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    double timestampPeriodNs_ = 1.0;
    uint64_t timestampMask_ = ~0ull;
    bool slotPending_[kFramesInFlight]{};
    bool slotMeasured_[kFramesInFlight]{};
    std::vector<double> gpuTimesMs_;
    uint64_t frameCounter_ = 0;
};

std::string formatDriverVersion(uint32_t vendorId, uint32_t version);
std::string formatApiVersion(uint32_t version);
const char* deviceTypeName(VkPhysicalDeviceType t);
const char* presentModeName(VkPresentModeKHR m);
