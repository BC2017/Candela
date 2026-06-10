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

**Phase 4 complete — Candela Studio.** `candela-studio.exe`: Dear ImGui (docking) editor with the scene rendered offscreen into a **Viewport** panel (resizes live), **Hierarchy** (create/delete/drag-reparent), **Inspector** (undoable per-component editing), **Content browser** (drag models into the scene), **Scene Settings**, ImGuizmo translate/rotate/scale gizmos (W/E/R), click-to-select via an entity-ID G-buffer target with GPU readback, renderer debug views (albedo/normals/metal-rough/AO/cascades), play/stop via registry snapshot, and **undo/redo for every operation** (command stack with stable editor IDs that survive delete→undo). `--selftest` exercises the command stack and picking end-to-end. The sandbox remains the editor-free runtime.

**Phase 3** — Candela became an engine, not a renderer demo:
EnTT ECS (transform hierarchy, mesh renderers, lights, per-scene settings in registry context) with the renderer consuming the registry each frame; an asset registry that stamps sources with GUID `.meta` files, imports glTF models **asynchronously on a job system** (geometry streams in while the app runs — Sponza pops in ~1.5 s after the window appears), and re-imports automatically when source files change; `.candela` JSON scene serialization with a byte-identical save→load→save round-trip (`--roundtrip-check`); a typed event bus (asset-reloaded events); and input-action mapping replacing hardcoded keys. All GPU submission paths are mutex-serialized so imports run safely on worker threads.

**Phase 2** — deferred PBR pipeline, fully render-graph driven:
4 shadow cascades (texel-snapped, PCF) → G-buffer (albedo / octahedral normals / metallic-roughness-AO) → Cook-Torrance GGX lighting with sun + point lights and split-sum IBL (GPU-precomputed irradiance, GGX-prefiltered specular, BRDF LUT from a Poly Haven HDRI) → dual-Kawase bloom → ACES tonemap. Full glTF material loading: normal mapping with tangents, metallic-roughness, occlusion, sRGB-aware texture cache. Sponza renders validation-clean at ~550 fps (debug, 1600×900, RTX 5080); all shaders hot-reload as a group.

Run `scripts\get-assets.ps1` once to download the Sponza test scene and HDRI.

Deferred to later phases: emissive + velocity G-buffer targets (TAA/denoising), sky rendering from the environment cube, alpha-tested shadows, IBL disk caching, KTX2/BC texture compression (Phase 3 asset pipeline). Phase 1: render graph, bindless, glTF. Phase 0: Vulkan bring-up, Slang hot reload.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the phased build plan. Next: Phase 5 (ray tracing — BLAS/TLAS, RT shadows, AO, reflections, denoising).
