#include "vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint64_t kTimeoutNs = 10ull * 1000 * 1000 * 1000;  // a frame taking 10 s means a hung GPU
const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

const char* baseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* ctx = static_cast<VkContext*>(user);
    if (types == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT && !(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        std::fprintf(stderr, "[vulkan loader] %s\n", data->pMessage);  // e.g. layers disabled by environment
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (ctx) ctx->validationErrors++;
        std::fprintf(stderr, "[validation error] %s\n", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (ctx) ctx->validationWarnings++;
        std::fprintf(stderr, "[validation warning] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) != VK_SUCCESS) return false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool hasInstanceExtension(const char* layer, const char* name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateInstanceExtensionProperties(layer, &n, exts.data()) != VK_SUCCESS) return false;
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool deviceHasSwapchain(VkPhysicalDevice pd) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data()) != VK_SUCCESS) return false;
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
    return false;
}

// Queue family with graphics + compute (+ present when a surface is given), or -1.
int findQueueFamily(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, fams.data());
    for (uint32_t i = 0; i < n; ++i) {
        const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((fams[i].queueFlags & need) != need) continue;
        if (surface) {
            VkBool32 present = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present));
            if (!present) continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

int typeRank(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: return 3;
        default: return 4;  // CPU
    }
}

void queryDriver(VkPhysicalDevice pd, std::string& name, std::string& info) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(pd, &p);
    name.clear();
    info.clear();
    if (p.apiVersion < VK_API_VERSION_1_2) return;
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    name = drv.driverName;
    info = drv.driverInfo;
}

}  // namespace

const char* vkResultName(VkResult r) {
    switch (r) {
#define C(x) \
    case x: return #x;
        C(VK_SUCCESS) C(VK_NOT_READY) C(VK_TIMEOUT) C(VK_EVENT_SET) C(VK_EVENT_RESET) C(VK_INCOMPLETE)
        C(VK_ERROR_OUT_OF_HOST_MEMORY) C(VK_ERROR_OUT_OF_DEVICE_MEMORY) C(VK_ERROR_INITIALIZATION_FAILED)
        C(VK_ERROR_DEVICE_LOST) C(VK_ERROR_MEMORY_MAP_FAILED) C(VK_ERROR_LAYER_NOT_PRESENT)
        C(VK_ERROR_EXTENSION_NOT_PRESENT) C(VK_ERROR_FEATURE_NOT_PRESENT) C(VK_ERROR_INCOMPATIBLE_DRIVER)
        C(VK_ERROR_TOO_MANY_OBJECTS) C(VK_ERROR_FORMAT_NOT_SUPPORTED) C(VK_ERROR_FRAGMENTED_POOL)
        C(VK_ERROR_UNKNOWN) C(VK_ERROR_OUT_OF_POOL_MEMORY) C(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        C(VK_ERROR_FRAGMENTATION) C(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS) C(VK_ERROR_SURFACE_LOST_KHR)
        C(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) C(VK_SUBOPTIMAL_KHR) C(VK_ERROR_OUT_OF_DATE_KHR)
        C(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR) C(VK_ERROR_VALIDATION_FAILED_EXT)
#undef C
        default: return "VK_RESULT_UNKNOWN";
    }
}

VulkanError::VulkanError(const std::string& call, VkResult r, const char* file, int line)
    : std::runtime_error(call + " failed with " + vkResultName(r) + " (" + std::to_string(static_cast<int>(r)) +
                         ") at " + baseName(file) + ":" + std::to_string(line)),
      result(r) {}

std::string formatApiVersion(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

std::string formatDriverVersion(uint32_t vendor, uint32_t v) {
    char buf[64];
    if (vendor == 0x10DE) {  // NVIDIA: 10.8.8.6
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);
    }
#ifdef _WIN32
    else if (vendor == 0x8086) {  // Intel on Windows: 18.14
        std::snprintf(buf, sizeof buf, "%u.%u", v >> 14, v & 0x3fff);
    }
#endif
    else {
        return formatApiVersion(v);
    }
    return buf;
}

const char* deviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        default: return "other";
    }
}

// ---------------------------------------------------------------------------------------------
// Instance and device

int VkContext::listGpus() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "renner-bench";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &inst));
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> devs(n);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, devs.data()));
    std::printf("%u Vulkan device(s):\n", n);
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        std::string dn, di;
        queryDriver(devs[i], dn, di);
        bool ok = p.apiVersion >= VK_API_VERSION_1_2 && deviceHasSwapchain(devs[i]) && findQueueFamily(devs[i], VK_NULL_HANDLE) >= 0;
        std::printf("  [%u] %s (%s), Vulkan %s, driver %s %s %s%s\n", i, p.deviceName, deviceTypeName(p.deviceType),
                    formatApiVersion(p.apiVersion).c_str(), dn.c_str(), di.c_str(),
                    formatDriverVersion(p.vendorID, p.driverVersion).c_str(), ok ? "" : "  [unsupported: needs Vulkan 1.2 + swapchain]");
    }
    vkDestroyInstance(inst, nullptr);
    return 0;
}

void VkContext::createInstance(SDL_Window* window, bool validate) {
    uint32_t apiVersion = VK_API_VERSION_1_0;
    VK_CHECK(vkEnumerateInstanceVersion(&apiVersion));
    if (apiVersion < VK_API_VERSION_1_2)
        throw std::runtime_error("the Vulkan loader supports only " + formatApiVersion(apiVersion) + ", 1.2 is required");

    unsigned int extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr))
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    std::vector<const char*> exts(extCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, exts.data()))
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());

    std::vector<const char*> layers;
    bool debugUtils = false, syncValidation = false;
    if (validate) {
        if (hasLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            validationEnabled = true;
            debugUtils = hasInstanceExtension(nullptr, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                         hasInstanceExtension(kValidationLayer, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            syncValidation = hasInstanceExtension(kValidationLayer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (debugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (syncValidation) exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "warning: --validate given but %s is not installed; running without it\n",
                         kValidationLayer);
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "renner-bench";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app.pEngineName = "renner-bench";
    app.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;
    dbg.pUserData = this;

    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enables;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    const void* chain = nullptr;
    if (debugUtils) {
        dbg.pNext = chain;
        chain = &dbg;
    }
    if (syncValidation) {
        features.pNext = chain;
        chain = &features;
    }
    ci.pNext = chain;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
    syncValidationEnabled = syncValidation;

    if (debugUtils) {
        dbg.pNext = nullptr;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) VK_CHECK(create(instance, &dbg, nullptr, &messenger_));
    }
}

void VkContext::pickDevice(int requested) {
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    if (n == 0) throw std::runtime_error("no Vulkan device found");
    std::vector<VkPhysicalDevice> devs(n);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devs.data()));

    auto suitable = [&](VkPhysicalDevice pd, std::string& why) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_2) { why = "supports only Vulkan " + formatApiVersion(p.apiVersion); return false; }
        if (!deviceHasSwapchain(pd)) { why = "has no VK_KHR_swapchain"; return false; }
        if (findQueueFamily(pd, surface) < 0) { why = "has no graphics+compute queue that can present"; return false; }
        return true;
    };

    std::string why;
    if (requested >= 0) {
        if (static_cast<uint32_t>(requested) >= n)
            throw std::runtime_error("--gpu " + std::to_string(requested) + " is out of range (" + std::to_string(n) + " devices)");
        if (!suitable(devs[requested], why))
            throw std::runtime_error("GPU " + std::to_string(requested) + " is not usable: it " + why);
        gpuIndex = requested;
    } else {
        int best = -1, bestRank = 99;
        for (uint32_t i = 0; i < n; ++i) {
            if (!suitable(devs[i], why)) continue;
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devs[i], &p);
            int r = typeRank(p.deviceType);
            if (r < bestRank) { best = static_cast<int>(i); bestRank = r; }
        }
        if (best < 0) throw std::runtime_error("no Vulkan 1.2 device with swapchain and presentation support found");
        gpuIndex = best;
    }
    physical = devs[gpuIndex];
    queueFamily = static_cast<uint32_t>(findQueueFamily(physical, surface));
    vkGetPhysicalDeviceProperties(physical, &props);
    queryDriver(physical, driverName, driverInfo);
}

void VkContext::createDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};  // no optional features needed
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(physical, &ci, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // Timestamps
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, fams.data());
    uint32_t bits = fams[queueFamily].timestampValidBits;
    timestamps = bits > 0 && props.limits.timestampPeriod > 0.0f;
    if (timestamps) {
        timestampMask_ = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        timestampPeriodNs_ = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 2 * kFramesInFlight;
        VK_CHECK(vkCreateQueryPool(device, &qp, nullptr, &queryPool_));
    }

    // Depth format and MSAA level
    const VkFormat depthCandidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32,
                                        VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (VkFormat f : depthCandidates) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat = f;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) throw std::runtime_error("no supported depth format");
    VkSampleCountFlags both = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    msaaSamples = (both & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

    // Per-frame command buffers and synchronization
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &cmdPool_));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmd_));
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &fences_[i]));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &imageAvailable_[i]));
    }
}

void VkContext::init(SDL_Window* window, const ContextOptions& opt) {
    window_ = window;
    presentPref_ = opt.present;
    createInstance(window, opt.validate);
    if (!SDL_Vulkan_CreateSurface(window, instance, &surface))
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    pickDevice(opt.gpuIndex);
    createDevice();
    createSwapchain();
}

void VkContext::waitIdle() {
    if (device) VK_CHECK(vkDeviceWaitIdle(device));
}

void VkContext::shutdown() {
    if (device) {
        vkDeviceWaitIdle(device);
        destroySwapchainViews();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (fences_[i]) vkDestroyFence(device, fences_[i], nullptr);
            if (imageAvailable_[i]) vkDestroySemaphore(device, imageAvailable_[i], nullptr);
        }
        if (queryPool_) vkDestroyQueryPool(device, queryPool_, nullptr);
        if (cmdPool_) vkDestroyCommandPool(device, cmdPool_, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------------------------
// Swapchain

bool VkContext::surfaceHasArea() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    if (caps.currentExtent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        return w > 0 && h > 0;
    }
    return caps.currentExtent.width > 0 && caps.currentExtent.height > 0;
}

void VkContext::destroySwapchainViews() {
    for (VkImageView v : swapViews) vkDestroyImageView(device, v, nullptr);
    swapViews.clear();
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device, s, nullptr);
    renderDone_.clear();
}

void VkContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));

    uint32_t nf = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(nf);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, formats.data()));
    if (formats.empty()) throw std::runtime_error("surface reports no formats");
    VkSurfaceFormatKHR chosen = formats[0];
    bool found = false;
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (auto& f : formats) {
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    swapFormat = chosen.format;
    manualGamma = !(swapFormat == VK_FORMAT_B8G8R8A8_SRGB || swapFormat == VK_FORMAT_R8G8B8A8_SRGB);

    uint32_t nm = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &nm, nullptr));
    std::vector<VkPresentModeKHR> modes(nm);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &nm, modes.data()));
    std::vector<VkPresentModeKHR> order;
    if (presentPref_ == PresentPref::Immediate) order = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    if (presentPref_ == PresentPref::Mailbox) order = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    order.push_back(VK_PRESENT_MODE_FIFO_KHR);
    presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported
    for (VkPresentModeKHR m : order) {
        if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
            presentMode = m;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    swapExtent = extent;

    uint32_t count = std::max(3u, caps.minImageCount + 1);  // triple buffering keeps immediate/mailbox from stalling
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & a) { alpha = a; break; }
        }
    }

    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    canCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
                 (swapFormat == VK_FORMAT_B8G8R8A8_SRGB || swapFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                  swapFormat == VK_FORMAT_R8G8B8A8_SRGB || swapFormat == VK_FORMAT_R8G8B8A8_UNORM);
    if (canCapture) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));
    destroySwapchainViews();
    if (old) vkDestroySwapchainKHR(device, old, nullptr);

    uint32_t n = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr));
    swapImages.resize(n);
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data()));
    for (VkImage img : swapImages) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &view));
        swapViews.push_back(view);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &s));
        renderDone_.push_back(s);
    }
}

void VkContext::recreateSwapchain() {
    waitIdle();
    createSwapchain();
}

// ---------------------------------------------------------------------------------------------
// Frame loop

void VkContext::collectTimestamps(uint32_t slot) {
    if (!slotPending_[slot]) return;
    slotPending_[slot] = false;
    uint64_t data[4] = {};  // value, availability, value, availability
    VkResult r = vkGetQueryPoolResults(device, queryPool_, slot * 2, 2, sizeof data, data, 2 * sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r == VK_NOT_READY) return;
    VK_CHECK(r);
    if (!slotMeasured_[slot] || !data[1] || !data[3]) return;
    uint64_t ticks = ((data[2] & timestampMask_) - (data[0] & timestampMask_)) & timestampMask_;
    gpuTimesMs_.push_back(static_cast<double>(ticks) * timestampPeriodNs_ / 1.0e6);
}

namespace {

void recordCapture(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkBuffer dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = dst;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
                         nullptr);
}

bool writeBmp(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra) {
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    const uint32_t dataSize = rowBytes * h;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; ++i) header[off + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    header[26] = 1;
    header[28] = 24;
    put32(34, dataSize);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(header, 1, sizeof header, f) == sizeof header;
    std::vector<uint8_t> row(rowBytes, 0);
    for (uint32_t y = 0; y < h && ok; ++y) {
        const uint8_t* src = px + static_cast<size_t>(h - 1 - y) * w * 4;  // BMP rows are bottom-up
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = src + x * 4;
            row[x * 3 + 0] = bgra ? p[0] : p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgra ? p[2] : p[0];
        }
        ok = std::fwrite(row.data(), 1, rowBytes, f) == rowBytes;
    }
    return std::fclose(f) == 0 && ok;
}

}  // namespace

bool VkContext::drawFrame(double time, float dt, bool measured, const std::function<void(const FrameInfo&)>& record,
                          const std::string* capturePath) {
    const uint32_t slot = static_cast<uint32_t>(frameCounter_ % kFramesInFlight);
    VkResult r = vkWaitForFences(device, 1, &fences_[slot], VK_TRUE, kTimeoutNs);
    if (r == VK_TIMEOUT) throw VulkanError("vkWaitForFences (GPU did not finish a frame within 10 s)", r, __FILE__, __LINE__);
    VK_CHECK(r);
    collectTimestamps(slot);

    uint32_t imageIndex = 0;
    r = vkAcquireNextImageKHR(device, swapchain, kTimeoutNs, imageAvailable_[slot], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return true;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throw VulkanError("vkAcquireNextImageKHR", r, __FILE__, __LINE__);
    bool recreate = (r == VK_SUBOPTIMAL_KHR);

    ResourceList captureRes;
    Buffer captureBuf;
    const bool capture = capturePath && canCapture;
    if (capture)
        captureBuf = createBuffer(captureRes, VkDeviceSize(swapExtent.width) * swapExtent.height * 4,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    VK_CHECK(vkResetFences(device, 1, &fences_[slot]));
    VkCommandBuffer cmd = cmd_[slot];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (timestamps) {
        vkCmdResetQueryPool(cmd, queryPool_, slot * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, slot * 2);
    }
    record(FrameInfo{cmd, slot, imageIndex, time, dt});
    if (capture) recordCapture(cmd, swapImages[imageIndex], swapExtent, captureBuf.buffer);
    if (timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, slot * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[slot];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[imageIndex];
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fences_[slot]));
    slotPending_[slot] = timestamps;
    slotMeasured_[slot] = measured;
    ++frameCounter_;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(queue, &pi);
    if (capture) {
        VK_CHECK(vkWaitForFences(device, 1, &fences_[slot], VK_TRUE, kTimeoutNs));
        const bool bgra = swapFormat == VK_FORMAT_B8G8R8A8_SRGB || swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
        if (!writeBmp(*capturePath, static_cast<const uint8_t*>(captureBuf.mapped), swapExtent.width, swapExtent.height, bgra))
            std::fprintf(stderr, "warning: could not write %s\n", capturePath->c_str());
        captureRes.flush();
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return true;
    if (r != VK_SUCCESS) throw VulkanError("vkQueuePresentKHR", r, __FILE__, __LINE__);
    return recreate;
}

void VkContext::finishFrames() {
    waitIdle();
    for (uint32_t i = 0; i < kFramesInFlight; ++i) collectTimestamps(i);
}

std::vector<double> VkContext::takeGpuTimes() {
    std::vector<double> out;
    out.swap(gpuTimesMs_);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Resource helpers

uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    // Device-local is a preference, not a requirement (some virtual GPUs expose only host memory).
    if (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return findMemoryType(typeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    throw std::runtime_error("no suitable Vulkan memory type");
}

Buffer VkContext::createBuffer(ResourceList& rl, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    if (hostVisible) VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    VkDevice dev = device;
    rl.add([dev, b] {
        vkDestroyBuffer(dev, b.buffer, nullptr);
        vkFreeMemory(dev, b.memory, nullptr);  // implicitly unmaps
    });
    return b;
}

Buffer VkContext::createDeviceBuffer(ResourceList& rl, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b = createBuffer(rl, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    ResourceList staging;
    Buffer s = createBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(s.mapped, data, static_cast<size_t>(size));
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, s.buffer, b.buffer, 1, &region);
    });
    staging.flush();
    return b;
}

Image VkContext::createImage(ResourceList& rl, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                             VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    Image img;
    img.format = format;
    img.extent = extent;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device, &ci, nullptr, &img.image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(device, img.image, img.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &vci, nullptr, &img.view));
    VkDevice dev = device;
    rl.add([dev, img] {
        vkDestroyImageView(dev, img.view, nullptr);
        vkDestroyImage(dev, img.image, nullptr);
        vkFreeMemory(dev, img.memory, nullptr);
    });
    return img;
}

VkRenderPass VkContext::createRenderPass(ResourceList& rl, const RenderPassDesc& d) {
    const bool msaaResolve = d.resolve && d.samples != VK_SAMPLE_COUNT_1_BIT;
    const bool hasDepth = d.depth != VK_FORMAT_UNDEFINED;
    std::vector<VkAttachmentDescription> att;

    VkAttachmentDescription c{};
    c.format = d.color;
    c.samples = d.samples;
    c.loadOp = d.load;
    c.storeOp = msaaResolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    c.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    c.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    c.initialLayout = msaaResolve ? VK_IMAGE_LAYOUT_UNDEFINED : d.initial;
    c.finalLayout = msaaResolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : d.final;
    att.push_back(c);
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkAttachmentReference depthRef{};
    if (hasDepth) {
        VkAttachmentDescription z{};
        z.format = d.depth;
        z.samples = d.samples;
        z.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        z.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        z.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        z.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        z.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        z.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef = {static_cast<uint32_t>(att.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        att.push_back(z);
    }
    VkAttachmentReference resolveRef{};
    if (msaaResolve) {
        VkAttachmentDescription r{};
        r.format = d.color;
        r.samples = VK_SAMPLE_COUNT_1_BIT;
        r.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        r.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        r.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        r.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        r.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        r.finalLayout = d.final;
        resolveRef = {static_cast<uint32_t>(att.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        att.push_back(r);
    }

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pResolveAttachments = msaaResolve ? &resolveRef : nullptr;
    sub.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    // Incoming: earlier attachment writes (WAW), earlier sampling of the same image (WAR) and the
    // swapchain acquire wait (COLOR_ATTACHMENT_OUTPUT). Outgoing: results are sampled by later passes.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<uint32_t>(att.size());
    ci.pAttachments = att.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(device, &ci, nullptr, &pass));
    VkDevice dev = device;
    rl.add([dev, pass] { vkDestroyRenderPass(dev, pass, nullptr); });
    return pass;
}

VkFramebuffer VkContext::createFramebuffer(ResourceList& rl, VkRenderPass pass, std::initializer_list<VkImageView> views,
                                           VkExtent2D extent) {
    std::vector<VkImageView> v(views);
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = pass;
    ci.attachmentCount = static_cast<uint32_t>(v.size());
    ci.pAttachments = v.data();
    ci.width = extent.width;
    ci.height = extent.height;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFramebuffer(device, &ci, nullptr, &fb));
    VkDevice dev = device;
    rl.add([dev, fb] { vkDestroyFramebuffer(dev, fb, nullptr); });
    return fb;
}

VkSampler VkContext::createSampler(ResourceList& rl) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &ci, nullptr, &s));
    VkDevice dev = device;
    rl.add([dev, s] { vkDestroySampler(dev, s, nullptr); });
    return s;
}

VkDescriptorSetLayout VkContext::createSetLayout(ResourceList& rl, std::initializer_list<VkDescriptorSetLayoutBinding> b) {
    std::vector<VkDescriptorSetLayoutBinding> v(b);
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(v.size());
    ci.pBindings = v.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &l));
    VkDevice dev = device;
    rl.add([dev, l] { vkDestroyDescriptorSetLayout(dev, l, nullptr); });
    return l;
}

VkPipelineLayout VkContext::createPipelineLayout(ResourceList& rl, std::initializer_list<VkDescriptorSetLayout> sets,
                                                 uint32_t pushBytes, VkShaderStageFlags pushStages) {
    std::vector<VkDescriptorSetLayout> v(sets);
    VkPushConstantRange range{pushStages, 0, pushBytes};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = static_cast<uint32_t>(v.size());
    ci.pSetLayouts = v.data();
    ci.pushConstantRangeCount = pushBytes ? 1 : 0;
    ci.pPushConstantRanges = pushBytes ? &range : nullptr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &ci, nullptr, &l));
    VkDevice dev = device;
    rl.add([dev, l] { vkDestroyPipelineLayout(dev, l, nullptr); });
    return l;
}

VkDescriptorPool VkContext::createDescriptorPool(ResourceList& rl, uint32_t maxSets,
                                                 std::initializer_list<VkDescriptorPoolSize> sizes) {
    std::vector<VkDescriptorPoolSize> v(sizes);
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = maxSets;
    ci.poolSizeCount = static_cast<uint32_t>(v.size());
    ci.pPoolSizes = v.data();
    VkDescriptorPool p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &ci, nullptr, &p));
    VkDevice dev = device;
    rl.add([dev, p] { vkDestroyDescriptorPool(dev, p, nullptr); });
    return p;
}

VkDescriptorSet VkContext::allocateSet(VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet s = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &ai, &s));
    return s;
}

void VkContext::writeImage(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void VkContext::writeBuffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer buffer,
                            VkDeviceSize size) {
    VkDescriptorBufferInfo bi{buffer, 0, size};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

static VkShaderModule makeModule(VkDevice device, const ShaderCode& c) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = c.bytes;
    ci.pCode = c.code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
}

VkPipeline VkContext::createGraphicsPipeline(ResourceList& rl, const PipelineDesc& d) {
    VkShaderModule vs = makeModule(device, d.vs);
    VkShaderModule fs = makeModule(device, d.fs);

    // Specialization constants shared by all shaders: 0 = manual sRGB encode, 1 = write raw HDR.
    const uint32_t specData[2] = {manualGamma ? 1u : 0u, d.hdrOut ? 1u : 0u};
    const VkSpecializationMapEntry entries[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo spec{2, entries, sizeof specData, specData};

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", &spec};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", &spec};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(d.bindings.size());
    vi.pVertexBindingDescriptions = d.bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(d.attributes.size());
    vi.pVertexAttributeDescriptions = d.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (d.blend == Blend::Additive) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d.layout;
    ci.renderPass = d.renderPass;
    ci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    VK_CHECK(r);
    VkDevice dev = device;
    rl.add([dev, p] { vkDestroyPipeline(dev, p, nullptr); });
    return p;
}

VkPipeline VkContext::createComputePipeline(ResourceList& rl, ShaderCode cs, VkPipelineLayout layout) {
    VkShaderModule m = makeModule(device, cs);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(device, m, nullptr);
    VK_CHECK(r);
    VkDevice dev = device;
    rl.add([dev, p] { vkDestroyPipeline(dev, p, nullptr); });
    return p;
}

void VkContext::immediateSubmit(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    fn(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(device, 1, &fence, VK_TRUE, kTimeoutNs);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, cmdPool_, 1, &cmd);
    if (r == VK_TIMEOUT) throw VulkanError("vkWaitForFences (upload did not finish within 10 s)", r, __FILE__, __LINE__);
    VK_CHECK(r);
}

void VkContext::beginRenderPass(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, VkExtent2D extent,
                                std::initializer_list<VkClearValue> clears) {
    std::vector<VkClearValue> v(clears);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, extent};
    bi.clearValueCount = static_cast<uint32_t>(v.size());
    bi.pClearValues = v.empty() ? nullptr : v.data();
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(cmd, extent);
}

void VkContext::setViewport(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}
