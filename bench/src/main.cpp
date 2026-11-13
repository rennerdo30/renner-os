// renner-bench: cross-platform Vulkan GPU benchmark (core Vulkan 1.2 + VK_KHR_swapchain).
#include "scenes/scenes.h"
#include "vk_context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#ifndef RENNER_BENCH_VERSION
#define RENNER_BENCH_VERSION "dev"
#endif

namespace {

enum ExitCode { kExitOk = 0, kExitUsage = 1, kExitVulkan = 2, kExitAborted = 3, kExitValidation = 4 };

const char* const kAllScenes[] = {"raymarch", "instanced", "particles", "postfx"};

struct Options {
    std::vector<std::string> scenes;
    double seconds = 10.0;
    double warmup = 2.0;
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    PresentPref present = PresentPref::Immediate;
    std::string presentName = "immediate";
    int gpu = -1;
    std::string jsonPath;
    bool listGpus = false;
    bool validate = false;
    bool fxaa = true;
    std::string captureDir;
};

struct SceneResult {
    std::string name;
    size_t frames = 0;
    double avgFps = 0, avgMs = 0, medianMs = 0, p99Ms = 0, p999Ms = 0, low1Fps = 0;
    double gpuMedianMs = -1;  // < 0: not available
    size_t gpuSamples = 0;
};

void usage() {
    std::printf(
        "renner-bench %s - Vulkan GPU benchmark\n\n"
        "Usage: renner-bench [options]\n"
        "  --scene NAME|all     raymarch, instanced, particles, postfx, a comma list, or all (default all)\n"
        "  --seconds N          measured seconds per scene (default 10)\n"
        "  --warmup S           warmup seconds per scene, excluded from stats (default 2)\n"
        "  --width W --height H window size (default 1280x720)\n"
        "  --fullscreen         borderless fullscreen at desktop resolution\n"
        "  --present MODE       immediate|mailbox|fifo (default immediate; falls back to mailbox, then fifo)\n"
        "  --gpu INDEX          use this GPU (see --list-gpus)\n"
        "  --json PATH          write results as JSON\n"
        "  --list-gpus          list Vulkan devices and exit\n"
        "  --validate           enable VK_LAYER_KHRONOS_validation if installed\n"
        "  --no-fxaa            skip FXAA in the postfx scene\n"
        "  --capture DIR        save each scene's last frame (t = warmup + seconds) as DIR/<scene>.bmp\n"
        "  --help               this text\n\n"
        "Exit codes: 0 ok, 1 usage, 2 Vulkan/runtime error, 3 aborted (Esc), 4 validation errors\n",
        RENNER_BENCH_VERSION);
}

bool parseNumber(const char* s, double& out) {
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool parseArgs(int argc, char** argv, Options& o, bool& helpOnly) {
    helpOnly = false;
    std::string sceneArg = "all";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char*& v) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                return false;
            }
            v = argv[++i];
            return true;
        };
        const char* v = nullptr;
        double d = 0;
        if (a == "--help" || a == "-h") {
            helpOnly = true;
            return true;
        } else if (a == "--scene") {
            if (!value(v)) return false;
            sceneArg = v;
        } else if (a == "--seconds") {
            if (!value(v) || !parseNumber(v, d) || d <= 0) { std::fprintf(stderr, "error: --seconds needs a positive number\n"); return false; }
            o.seconds = d;
        } else if (a == "--warmup") {
            if (!value(v) || !parseNumber(v, d) || d < 0) { std::fprintf(stderr, "error: --warmup needs a number >= 0\n"); return false; }
            o.warmup = d;
        } else if (a == "--width" || a == "--height") {
            if (!value(v) || !parseNumber(v, d) || d < 1 || d > 16384 || d != std::floor(d)) {
                std::fprintf(stderr, "error: %s needs an integer between 1 and 16384\n", a.c_str());
                return false;
            }
            (a == "--width" ? o.width : o.height) = static_cast<int>(d);
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--present") {
            if (!value(v)) return false;
            o.presentName = v;
            if (o.presentName == "immediate") o.present = PresentPref::Immediate;
            else if (o.presentName == "mailbox") o.present = PresentPref::Mailbox;
            else if (o.presentName == "fifo") o.present = PresentPref::Fifo;
            else { std::fprintf(stderr, "error: --present must be immediate, mailbox or fifo\n"); return false; }
        } else if (a == "--gpu") {
            if (!value(v) || !parseNumber(v, d) || d < 0 || d != std::floor(d)) { std::fprintf(stderr, "error: --gpu needs an index >= 0\n"); return false; }
            o.gpu = static_cast<int>(d);
        } else if (a == "--json") {
            if (!value(v)) return false;
            o.jsonPath = v;
        } else if (a == "--list-gpus") {
            o.listGpus = true;
        } else if (a == "--validate") {
            o.validate = true;
        } else if (a == "--capture") {
            if (!value(v)) return false;
            o.captureDir = v;
        } else if (a == "--no-fxaa") {
            o.fxaa = false;
        } else {
            std::fprintf(stderr, "error: unknown option %s (try --help)\n", a.c_str());
            return false;
        }
    }
    if (sceneArg == "all") {
        o.scenes.assign(std::begin(kAllScenes), std::end(kAllScenes));
    } else {
        std::stringstream ss(sceneArg);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (std::find(std::begin(kAllScenes), std::end(kAllScenes), item) == std::end(kAllScenes)) {
                std::fprintf(stderr, "error: unknown scene '%s' (raymarch, instanced, particles, postfx, all)\n", item.c_str());
                return false;
            }
            o.scenes.push_back(item);
        }
        if (o.scenes.empty()) { std::fprintf(stderr, "error: --scene is empty\n"); return false; }
    }
    return true;
}

std::unique_ptr<Scene> makeScene(const std::string& name, const Options& o) {
    if (name == "raymarch") return makeRaymarchScene();
    if (name == "instanced") return makeInstancedScene();
    if (name == "particles") return makeParticlesScene();
    return makePostfxScene(o.fxaa);
}

std::string osDescription() {
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof vi;
        if (fn && fn(&vi) == 0) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "Windows %lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            return buf;
        }
    }
    return "Windows";
#else
    std::string name = SDL_GetPlatform();
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12);
            name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
            break;
        }
    }
    struct utsname u{};
    if (uname(&u) == 0) name += std::string(" (kernel ") + u.release + ", " + u.machine + ")";
    return name;
#endif
}

double percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    size_t rank = static_cast<size_t>(std::ceil(q * sorted.size()));
    rank = std::clamp<size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

SceneResult computeStats(const std::string& name, std::vector<double> frameMs, std::vector<double> gpuMs) {
    SceneResult r;
    r.name = name;
    r.frames = frameMs.size();
    if (!frameMs.empty()) {
        double total = 0;
        for (double v : frameMs) total += v;
        std::sort(frameMs.begin(), frameMs.end());
        r.avgMs = total / frameMs.size();
        r.avgFps = total > 0 ? 1000.0 * frameMs.size() / total : 0;
        r.medianMs = percentile(frameMs, 0.5);
        r.p99Ms = percentile(frameMs, 0.99);
        r.p999Ms = percentile(frameMs, 0.999);
        // "1% low": average FPS over the slowest 1% of frames (at least one frame).
        size_t n = std::max<size_t>(1, static_cast<size_t>(std::ceil(frameMs.size() * 0.01)));
        double worst = 0;
        for (size_t i = frameMs.size() - n; i < frameMs.size(); ++i) worst += frameMs[i];
        r.low1Fps = worst > 0 ? 1000.0 * n / worst : 0;
    }
    r.gpuSamples = gpuMs.size();
    if (!gpuMs.empty()) {
        std::sort(gpuMs.begin(), gpuMs.end());
        r.gpuMedianMs = percentile(gpuMs, 0.5);
    }
    return r;
}

// Runs one scene for warmup + seconds. Returns false when the user aborted.
bool runScene(VkContext& ctx, SDL_Window* window, const Options& o, const std::string& sceneName, size_t index,
              size_t total, SceneResult& result) {
    using clock = std::chrono::steady_clock;
    std::unique_ptr<Scene> scene = makeScene(sceneName, o);
    scene->init(ctx);
    scene->createSized(ctx);
    ctx.takeGpuTimes();

    std::vector<double> frameMs;
    frameMs.reserve(65536);
    const double duration = o.warmup + o.seconds;
    const auto start = clock::now();
    auto last = start;
    double lastTime = 0.0, titleTime = -1.0;
    size_t titleFrames = 0;
    bool needRecreate = false, aborted = false;

    auto seconds = [](clock::duration d) { return std::chrono::duration<double>(d).count(); };

    while (!aborted) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) aborted = true;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) aborted = true;
            if (ev.type == SDL_WINDOWEVENT && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                                               ev.window.event == SDL_WINDOWEVENT_RESIZED))
                needRecreate = true;
        }
        if (aborted) break;
        const auto now = clock::now();
        const double t = seconds(now - start);
        if (t >= duration) break;

        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) || (needRecreate && !ctx.surfaceHasArea())) {
            SDL_WaitEventTimeout(nullptr, 50);
            last = clock::now();
            continue;
        }
        if (needRecreate) {
            ctx.waitIdle();
            scene->destroySized();
            ctx.recreateSwapchain();
            scene->createSized(ctx);
            needRecreate = false;
            last = clock::now();
            continue;
        }

        const bool measured = t >= o.warmup;
        const float dt = static_cast<float>(t - lastTime);
        lastTime = t;
        needRecreate = ctx.drawFrame(t, dt, measured, [&](const FrameInfo& fi) { scene->record(ctx, fi); });
        const auto end = clock::now();
        const double ms = 1000.0 * seconds(end - last);
        last = end;
        if (measured) frameMs.push_back(ms);

        ++titleFrames;
        const double tt = seconds(end - start);
        if (titleTime < 0) titleTime = tt;
        if (tt - titleTime >= 0.25) {
            double fps = titleFrames / (tt - titleTime);
            int pct = static_cast<int>(std::min(100.0, 100.0 * tt / duration));
            char title[160];
            std::snprintf(title, sizeof title, "renner-bench | %s (%zu/%zu) | %.0f FPS | %s %d%%", sceneName.c_str(),
                          index + 1, total, fps, tt < o.warmup ? "warmup" : "measuring", pct);
            SDL_SetWindowTitle(window, title);
            titleTime = tt;
            titleFrames = 0;
        }
    }

    // One extra, unmeasured frame at exactly t = warmup + seconds, saved as an image.
    if (!aborted && !o.captureDir.empty()) {
        if (!ctx.canCapture) {
            std::fprintf(stderr, "warning: this swapchain cannot be captured (no TRANSFER_SRC or unusual format)\n");
        } else if (!needRecreate && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            const std::string path = o.captureDir + "/" + sceneName + ".bmp";
            ctx.drawFrame(duration, static_cast<float>(duration - lastTime), false,
                          [&](const FrameInfo& fi) { scene->record(ctx, fi); }, &path);
        }
    }
    ctx.finishFrames();
    scene->destroy();
    result = computeStats(sceneName, std::move(frameMs), ctx.takeGpuTimes());
    return !aborted;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string num(double v, int decimals = 3) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

long computeScore(const std::vector<SceneResult>& results) {
    if (results.empty()) return 0;
    double logSum = 0;
    for (auto& r : results) {
        if (r.avgFps <= 0) return 0;
        logSum += std::log(r.avgFps);
    }
    return std::lround(10.0 * std::exp(logSum / results.size()));
}

bool writeJson(const std::string& path, const VkContext& ctx, const Options& o, const std::string& os,
               const std::vector<SceneResult>& results, long score, bool aborted) {
    std::ostringstream j;
    char ids[32];
    j << "{\n";
    j << "  \"tool\": \"renner-bench\",\n  \"version\": \"" << RENNER_BENCH_VERSION << "\",\n";
    j << "  \"gpu\": {\n";
    j << "    \"index\": " << ctx.gpuIndex << ",\n";
    j << "    \"name\": \"" << jsonEscape(ctx.props.deviceName) << "\",\n";
    j << "    \"type\": \"" << deviceTypeName(ctx.props.deviceType) << "\",\n";
    std::snprintf(ids, sizeof ids, "0x%04x", ctx.props.vendorID);
    j << "    \"vendor_id\": \"" << ids << "\",\n";
    std::snprintf(ids, sizeof ids, "0x%04x", ctx.props.deviceID);
    j << "    \"device_id\": \"" << ids << "\",\n";
    j << "    \"driver_name\": \"" << jsonEscape(ctx.driverName) << "\",\n";
    j << "    \"driver_info\": \"" << jsonEscape(ctx.driverInfo) << "\",\n";
    j << "    \"driver_version\": \"" << formatDriverVersion(ctx.props.vendorID, ctx.props.driverVersion) << "\",\n";
    j << "    \"driver_version_raw\": " << ctx.props.driverVersion << ",\n";
    j << "    \"api_version\": \"" << formatApiVersion(ctx.props.apiVersion) << "\"\n";
    j << "  },\n";
    j << "  \"os\": \"" << jsonEscape(os) << "\",\n";
    j << "  \"resolution\": { \"width\": " << ctx.swapExtent.width << ", \"height\": " << ctx.swapExtent.height << " },\n";
    j << "  \"fullscreen\": " << (o.fullscreen ? "true" : "false") << ",\n";
    j << "  \"present_mode\": \"" << presentModeName(ctx.presentMode) << "\",\n";
    j << "  \"present_mode_requested\": \"" << o.presentName << "\",\n";
    j << "  \"msaa_samples\": " << static_cast<int>(ctx.msaaSamples) << ",\n";
    j << "  \"swapchain_format\": " << static_cast<int>(ctx.swapFormat) << ",\n";
    j << "  \"seconds_per_scene\": " << num(o.seconds, 2) << ",\n";
    j << "  \"warmup_seconds\": " << num(o.warmup, 2) << ",\n";
    j << "  \"gpu_timestamps\": " << (ctx.timestamps ? "true" : "false") << ",\n";
    j << "  \"validation\": " << (ctx.validationEnabled ? "true" : "false") << ",\n";
    j << "  \"sync_validation\": " << (ctx.syncValidationEnabled ? "true" : "false") << ",\n";
    j << "  \"validation_errors\": " << ctx.validationErrors << ",\n";
    j << "  \"scenes\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const SceneResult& r = results[i];
        j << "    {\n";
        j << "      \"name\": \"" << r.name << "\",\n";
        j << "      \"frames\": " << r.frames << ",\n";
        j << "      \"avg_fps\": " << num(r.avgFps, 2) << ",\n";
        j << "      \"avg_ms\": " << num(r.avgMs) << ",\n";
        j << "      \"median_ms\": " << num(r.medianMs) << ",\n";
        j << "      \"p99_ms\": " << num(r.p99Ms) << ",\n";
        j << "      \"p999_ms\": " << num(r.p999Ms) << ",\n";
        j << "      \"low_1pct_fps\": " << num(r.low1Fps, 2) << ",\n";
        j << "      \"gpu_median_ms\": " << (r.gpuMedianMs >= 0 ? num(r.gpuMedianMs) : std::string("null")) << ",\n";
        j << "      \"gpu_samples\": " << r.gpuSamples << "\n";
        j << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    j << "  ],\n";
    j << "  \"score\": " << score << ",\n";
    j << "  \"aborted\": " << (aborted ? "true" : "false") << "\n";
    j << "}\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << j.str();
    return static_cast<bool>(f);
}

void printHeader(const VkContext& ctx, const Options& o, const std::string& os) {
    std::printf("renner-bench %s\n", RENNER_BENCH_VERSION);
    std::printf("GPU        : [%d] %s (%s)\n", ctx.gpuIndex, ctx.props.deviceName, deviceTypeName(ctx.props.deviceType));
    std::printf("Driver     : %s %s (version %s), Vulkan %s\n", ctx.driverName.c_str(), ctx.driverInfo.c_str(),
                formatDriverVersion(ctx.props.vendorID, ctx.props.driverVersion).c_str(),
                formatApiVersion(ctx.props.apiVersion).c_str());
    std::printf("OS         : %s\n", os.c_str());
    std::printf("Resolution : %ux%u%s, present %s (requested %s), MSAA %dx, GPU timestamps %s\n", ctx.swapExtent.width,
                ctx.swapExtent.height, o.fullscreen ? " fullscreen" : "", presentModeName(ctx.presentMode),
                o.presentName.c_str(), static_cast<int>(ctx.msaaSamples), ctx.timestamps ? "yes" : "no");
    std::printf("Run        : %.1f s warmup + %.1f s per scene%s\n\n", o.warmup, o.seconds,
                ctx.validationEnabled ? (ctx.syncValidationEnabled ? ", validation layer on (with synchronization validation)"
                                                                   : ", validation layer on")
                                      : "");
    std::fflush(stdout);
}

void printTable(const std::vector<SceneResult>& results, long score) {
    std::printf("%-10s %8s %10s %9s %9s %9s %9s %9s %9s\n", "scene", "frames", "avg FPS", "avg ms", "median",
                "p99 ms", "p99.9 ms", "1% low", "GPU ms");
    std::printf("%s\n", std::string(91, '-').c_str());
    for (auto& r : results) {
        std::string gpu = r.gpuMedianMs >= 0 ? num(r.gpuMedianMs) : "n/a";
        std::printf("%-10s %8zu %10.1f %9.3f %9.3f %9.3f %9.3f %9.1f %9s\n", r.name.c_str(), r.frames, r.avgFps, r.avgMs,
                    r.medianMs, r.p99Ms, r.p999Ms, r.low1Fps, gpu.c_str());
    }
    std::printf("%s\n", std::string(91, '-').c_str());
    std::printf("Score: %ld  (10 x geometric mean of average FPS over %zu scene%s)\n", score, results.size(),
                results.size() == 1 ? "" : "s");
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    bool helpOnly = false;
    if (!parseArgs(argc, argv, o, helpOnly)) return kExitUsage;
    if (helpOnly) {
        usage();
        return kExitOk;
    }
    if (o.listGpus) {
        try {
            return VkContext::listGpus();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return kExitVulkan;
        }
    }

    if (!o.captureDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(o.captureDir, ec);
        if (ec) {
            std::fprintf(stderr, "error: cannot create %s: %s\n", o.captureDir.c_str(), ec.message().c_str());
            return kExitUsage;
        }
    }

    SDL_SetMainReady();
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");  // render exactly WxH pixels
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return kExitVulkan;
    }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (o.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window* window = SDL_CreateWindow("renner-bench", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, o.width,
                                          o.height, flags);
    if (!window) {
        std::fprintf(stderr, "error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return kExitVulkan;
    }

    int rc = kExitOk;
    VkContext ctx;
    const std::string os = osDescription();
    std::vector<SceneResult> results;
    bool aborted = false;
    try {
        ContextOptions co;
        co.validate = o.validate;
        co.gpuIndex = o.gpu;
        co.present = o.present;
        ctx.init(window, co);
        printHeader(ctx, o, os);

        for (size_t i = 0; i < o.scenes.size(); ++i) {
            std::printf("running %-10s ...", o.scenes[i].c_str());
            std::fflush(stdout);
            SceneResult r;
            bool ok = runScene(ctx, window, o, o.scenes[i], i, o.scenes.size(), r);
            if (!ok) {
                std::printf(" aborted\n");
                aborted = true;
                break;
            }
            std::printf(" %8.1f FPS\n", r.avgFps);
            results.push_back(r);
        }

        long score = computeScore(results);
        std::printf("\n");
        printTable(results, score);
        if (aborted) std::printf("Aborted by the user; the score covers only finished scenes.\n");
        ctx.shutdown();  // first, so validation messages from teardown are counted in the JSON
        if (!o.jsonPath.empty()) {
            if (writeJson(o.jsonPath, ctx, o, os, results, score, aborted))
                std::printf("JSON written to %s\n", o.jsonPath.c_str());
            else {
                std::fprintf(stderr, "error: could not write %s\n", o.jsonPath.c_str());
                rc = kExitVulkan;
            }
        }
        if (ctx.validationEnabled) {
            std::printf("Validation: %u error(s), %u warning(s)\n", ctx.validationErrors, ctx.validationWarnings);
            if (ctx.validationErrors > 0 && rc == kExitOk) rc = kExitValidation;
        }
        if (aborted && rc == kExitOk) rc = kExitAborted;
    } catch (const VulkanError& e) {
        std::fprintf(stderr, "\nVulkan error: %s\n", e.what());
        rc = kExitVulkan;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nerror: %s\n", e.what());
        rc = kExitVulkan;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}
