# Candela

A high-fidelity 3D game engine in C++ with a Vulkan renderer.

> *candela* — the SI unit of luminous intensity.

## Pillars

- **Physically based rendering.** Cook-Torrance GGX metallic-roughness materials, image-based lighting, full HDR pipeline.
- **Hybrid ray-traced lighting.** Rasterized deferred G-buffer with hardware ray-traced shadows, reflections, and ambient occlusion/GI layered on top. Every RT effect has a raster fallback so the engine runs on non-RT GPUs.
- **A real editor.** Dear ImGui (docking) editor with a 3D viewport, scene hierarchy, inspector, content browser, transform gizmos, and play-in-editor.
- **Modern Vulkan.** Vulkan 1.3 core: dynamic rendering, synchronization2, bindless descriptors (descriptor indexing), buffer device address. No legacy render-pass objects.

## Decisions of record

| Decision | Choice | Rationale |
|---|---|---|
| Platforms | Windows first, Linux-ready | Develop/test on Windows; platform code isolated behind abstractions so a Linux port stays cheap |
| Light transport | Hybrid raster + RT effects | Best perf/quality tradeoff; runs without RT hardware; the deferred pipeline is reusable everywhere |
| Editor UI | Dear ImGui, docking branch | Industry default for in-engine tooling; renders inside the engine's own frame |
| Gameplay | Native C++ against the ECS | No scripting VM until the engine API stabilizes; a Lua layer is a candidate later phase |
| Language / build | C++20, CMake + FetchContent | Self-contained: clone + build, no global package-manager bootstrap. Revisit vcpkg if dependency build times grow |
| Shaders | Slang → SPIR-V, hot-reloadable | One language for raster + RT shaders, generics/modules, good Vulkan support |

## Repository layout

```
Candela/
├── engine/           # candela core — static library
│   └── src/
│       ├── core/     # logging, assertions, time, jobs, events, math (GLM)
│       ├── platform/ # window, input, filesystem (the Linux-readiness seam)
│       ├── rhi/      # Vulkan: device, swapchain, allocator, descriptors, render graph
│       ├── renderer/ # passes: gbuffer, lighting, shadows, RT effects, post
│       ├── scene/    # ECS (EnTT), transforms, cameras, serialization
│       └── assets/   # import, registry, GUIDs, hot reload
├── editor/           # candela-studio — the editor executable
├── sandbox/          # minimal runtime app for engine-only testing
├── shaders/          # Slang source, compiled to SPIR-V at build + hot reload
├── assets/           # test content (glTF scenes, HDRIs)
└── docs/             # ROADMAP.md and design notes
```

## Third-party libraries

| Library | Purpose |
|---|---|
| volk + vk-bootstrap | Vulkan loading and instance/device boilerplate |
| VulkanMemoryAllocator | GPU memory |
| GLFW | Windowing + input |
| GLM | Math |
| EnTT | ECS |
| Dear ImGui (docking) + ImGuizmo | Editor UI + gizmos |
| Slang | Shader compilation to SPIR-V |
| fastgltf | glTF 2.0 import |
| stb, KTX-Software | Image loading, GPU texture compression |
| spdlog | Logging |
| Tracy | CPU/GPU profiling |
| nlohmann/json | Scene + asset metadata serialization |

## Building

Requires: Visual Studio 2022+ (MSVC), CMake 3.28+, Ninja, and the [Vulkan SDK](https://vulkan.lunarg.com/) (provides `slangc` for shader compilation, used at build time and for hot reload).

```powershell
scripts\build.ps1                 # debug build
scripts\build.ps1 -Preset release # optimized build
build\debug\sandbox\candela-sandbox.exe
```

Edit `shaders/fullscreen.slang` while the sandbox runs to see shader hot reload. Press Escape to quit.

## Status

**Phase 0 complete** — Vulkan 1.3 device bring-up (volk + vk-bootstrap + VMA), swapchain with frames-in-flight, dynamic rendering + synchronization2, fullscreen triangle via Slang shaders with hot reload (resilient to broken edits), validation-clean, Tracy client integrated.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the phased build plan. Next: Phase 1 (render graph + bindless + glTF).
