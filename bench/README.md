# renner-bench

A cross-platform (Windows, Linux) Vulkan GPU benchmark for comparing native Windows, native Linux
and Linux VMs with Venus (virtio-gpu Vulkan forwarding).

It uses only **core Vulkan 1.2 plus `VK_KHR_swapchain`**: no vendor extensions, no optional device
features. Venus exposes an ordinary Vulkan 1.3/1.4 driver, so the same binary runs everywhere.
With `--validate`, it also uses `VK_EXT_debug_utils` and `VK_EXT_validation_features` from the validation layer.
All content is procedural: there are no asset files. The shaders are compiled to SPIR-V at build
time and embedded in the binary.

## What it measures

Four scenes. Each runs for a warmup period (excluded from the stats), then for a measured period.
All animation is driven by the time since the scene started, not by the frame count, so the same
moment of the scene looks the same on every machine (see `--capture`).

| Scene | Content | Stresses |
|---|---|---|
| `raymarch` | Full-screen SDF raymarcher: rotating Menger sponge (4 iterations), twisted pillars repeated over the plane with smoothly blended caps, metaball orbs fused with a ring, rolling terrain. Soft shadows (64 steps), 5-tap AO, one reflection bounce with shading, fog, ACES. | ALU |
| `instanced` | 240,000 instances (half icospheres with 320 triangles, half tori with 576 triangles; about 107M triangles per frame), generated once on the CPU. The per-instance spin and orbit are animated in the vertex shader. GGX/Cook-Torrance lighting from a sun and 8 moving point lights, emissive objects, depth buffer, 4x MSAA (1x if the device lacks it). | vertex, raster |
| `particles` | 1,048,576 particles in a storage buffer, simulated each frame by a compute shader (two octaves of curl noise with analytic-derivative value noise, plus three moving attractors, and respawn). Drawn as additive camera-facing quads (6.3M vertices) into an RGBA16F target, then tonemapped. | compute, bandwidth |
| `postfx` | The instanced scene with 80,000 objects, from a different viewpoint, rendered to RGBA16F. Then a bloom chain: soft threshold with a Karis average, 6 downsample passes (13-tap) and 5 upsample passes (3x3 tent, additive), ACES tonemapping to RGBA8, and FXAA to the swapchain. | bandwidth, fill rate |

Per scene, the benchmark reports:

- **frames**: frames rendered in the measured period;
- **avg FPS**: frames divided by the measured time;
- **avg, median, p99, p99.9 frame time (ms)**: the CPU-side time between consecutive presented
  frames (nearest-rank percentiles);
- **1% low FPS**: the average FPS over the slowest 1% of frames (at least one frame);
- **GPU ms**: the median GPU time per frame, from `vkCmdWriteTimestamp` at the top and bottom of
  each frame's command buffer. It is shown as n/a if the queue has `timestampValidBits == 0`.

**Score** = round(10 × geometric mean of the scenes' average FPS). Scores are comparable only for
the same scene set, resolution and present mode. With `--scene` set to one scene, the score is
10 × that scene's FPS.

Frame time includes presentation. Windowed runs on a desktop with a compositor (DWM, KWin, Mutter)
are affected by the compositor and by anything else using the GPU. GPU ms is the more stable number
for comparing raw GPU throughput; FPS is what a user experiences. For comparable results, close
other GPU load and use the same present mode on every machine.

### Implementation notes

- Two frames in flight, with a fence and an acquire semaphore per frame and a render-finished
  semaphore per swapchain image. Image layouts are handled by render-pass transitions, with
  explicit external subpass dependencies; buffer hazards between compute and vertex use pipeline
  barriers. A run with `--validate` (including synchronization validation) reports no errors.
- Window resize, `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` recreate the swapchain and
  every size-dependent resource. The animation clock keeps running while the window is minimized.
- The particle simulation integrates state, so it advances by the frame's `dt` (clamped to 1/30 s).
  The work per frame is fixed, but the exact particle positions depend slightly on the frame-time
  sequence. Every other scene is a pure function of time.
- If the surface offers only UNORM formats, the shaders encode sRGB themselves (specialization constant).
- Every `VkResult` is checked. A failure stops the run with the call, the result name and the
  source line, for example `vkQueueSubmit(...) failed with VK_ERROR_DEVICE_LOST (-4) at vk_context.cpp:<line>`.
  A fence that does not signal within 10 s is reported as a hung GPU.

## Command line

```
renner-bench [--scene NAME|all] [--seconds N] [--warmup S] [--width W --height H] [--fullscreen]
             [--present immediate|mailbox|fifo] [--gpu INDEX] [--json PATH] [--list-gpus]
             [--validate] [--no-fxaa] [--capture DIR]
```

| Option | Default | Meaning |
|---|---|---|
| `--scene` | `all` | `raymarch`, `instanced`, `particles`, `postfx`, a comma list, or `all` |
| `--seconds` | 10 | measured seconds per scene |
| `--warmup` | 2 | warmup seconds per scene (not measured) |
| `--width`, `--height` | 1280x720 | window size in pixels |
| `--fullscreen` | off | borderless fullscreen at the desktop resolution (no mode change; the JSON reports the real resolution) |
| `--present` | `immediate` | the requested present mode. Fallback order: immediate, then mailbox, then fifo (mailbox tries immediate second). The mode actually used is printed and stored in the JSON. |
| `--gpu` | auto | device index from `--list-gpus` (auto picks discrete, then integrated, then virtual, then CPU) |
| `--json` | none | write the results as JSON |
| `--list-gpus` | | list the devices and exit |
| `--validate` | off | enable `VK_LAYER_KHRONOS_validation` (with synchronization validation) if it is installed |
| `--no-fxaa` | off | the postfx scene skips FXAA (it uses a pass-through copy instead) |
| `--capture DIR` | off | after each scene, render one extra unmeasured frame at exactly t = warmup + seconds and save it as `DIR/<scene>.bmp`, to compare the output between machines and drivers |

Keys: **Esc** (or closing the window) aborts. The window title shows the scene, the current FPS,
and the warmup or measurement progress.

Exit codes: `0` ok, `1` usage error, `2` Vulkan or runtime error (with a message naming the call
and the `VkResult`), `3` aborted by the user, `4` the validation layer reported errors.

## JSON format

Example (one scene shown):

```json
{
  "tool": "renner-bench",
  "version": "1.0.0",
  "gpu": {
    "index": 0,
    "name": "AMD Radeon RX 9070 XT",
    "type": "discrete",
    "vendor_id": "0x1002",
    "device_id": "0x7550",
    "driver_name": "AMD proprietary driver",
    "driver_info": "26.10.1 (LLPC)",
    "driver_version": "2.0.416",
    "driver_version_raw": 8389024,
    "api_version": "1.4.359"
  },
  "os": "Windows 10.0.26300",
  "resolution": { "width": 1280, "height": 720 },
  "fullscreen": false,
  "present_mode": "immediate",
  "present_mode_requested": "immediate",
  "msaa_samples": 4,
  "swapchain_format": 50,
  "seconds_per_scene": 10.00,
  "warmup_seconds": 2.00,
  "gpu_timestamps": true,
  "validation": false,
  "sync_validation": false,
  "validation_errors": 0,
  "scenes": [
    {
      "name": "raymarch",
      "frames": 4190,
      "avg_fps": 419.00,
      "avg_ms": 2.387,
      "median_ms": 2.160,
      "p99_ms": 3.370,
      "p999_ms": 4.190,
      "low_1pct_fps": 270.50,
      "gpu_median_ms": 1.841,
      "gpu_samples": 4190
    }
  ],
  "score": 4190,
  "aborted": false
}
```

- `driver_name` and `driver_info` come from `VkPhysicalDeviceDriverProperties`.
- `driver_version` is decoded with the vendor's scheme (NVIDIA, Intel on Windows, otherwise the
  Vulkan major.minor.patch packing).
- `os` is the Windows version and build, or the `PRETTY_NAME` from `/etc/os-release` plus the kernel.
- `gpu_median_ms` is `null` when there are no timestamps.
- `swapchain_format` is the numeric `VkFormat` (50 = `VK_FORMAT_B8G8R8A8_SRGB`).

## Building

The build needs CMake 3.20 or newer, a C++17 compiler, SDL2, the Vulkan headers and loader, and
`glslc` (shaderc) or `glslangValidator`. The shaders in `shaders/` are compiled at build time and
embedded in the binary as generated headers (`<build>/shaders_gen/*.h`).

### Windows (MSYS2 UCRT64)

```sh
pacman -S mingw-w64-ucrt-x86_64-{cmake,ninja,gcc,SDL2,vulkan-headers,vulkan-loader,shaderc}
cmake -S bench -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

The result is `build/renner-bench.exe` plus `build/SDL2.dll`, which is copied there automatically.
libstdc++, libgcc and winpthread are linked statically. At run time the binary uses the system's
`vulkan-1.dll`, which the GPU driver installs.

Optional validation layer: run `pacman -S mingw-w64-ucrt-x86_64-vulkan-validation-layers`. Its
manifest lands in `<msys64>/ucrt64/bin`, so point the loader to it:

```sh
VK_ADD_LAYER_PATH='C:\msys64\ucrt64\bin' ./renner-bench.exe --validate
```

### Arch Linux

```sh
sudo pacman -S cmake ninja gcc sdl2 vulkan-headers vulkan-icd-loader shaderc
cmake -S bench -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/renner-bench
```

You also need a Vulkan driver, such as `vulkan-radeon`, `vulkan-intel` or `nvidia-utils`. In a VM
with virtio-gpu, use the Venus driver from `vulkan-virtio`. The optional validation layer is
`vulkan-validation-layers`. SDL2 is found through its CMake package (SDL2 or `sdl2-compat`), with
pkg-config as a fallback.

## Tips for comparable runs

- Use the same resolution, present mode and scene set on every machine. `immediate` measures
  throughput; `fifo` is capped by the refresh rate.
- On hosts with overlay layers (OBS, Steam, Overwolf, Epic), set
  `VK_LOADER_LAYERS_DISABLE='~implicit~'` so that no third-party layer is in the frame path.
- In VMs, compare `--list-gpus` first: the auto-pick prefers a discrete or virtual GPU over a
  software device such as lavapipe or llvmpipe, but `--gpu` makes the choice explicit.
- Use `--capture DIR` on two machines and compare the images, to check that a driver renders correctly.

## Files

- `src/main.cpp`: command line, scene loop, statistics, table and JSON output.
- `src/vk_context.{h,cpp}`: instance, device, swapchain, the frame loop with timestamps, and
  resource helpers (buffers, images, render passes, pipelines).
- `src/scenes/scenes.h`: the scene interface, math helpers and the shared instanced-mesh renderer.
- `src/scenes/{raymarch,instanced,particles,postfx}.cpp`: the four scenes.
- `shaders/*.{vert,frag,comp}`, `shaders/common.glsl`: GLSL sources.
- `cmake/embed_spirv.cmake`: turns SPIR-V into a `uint32_t` array header.
