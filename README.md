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

**Phase 5 complete — temporal foundations.** RG16F velocity G-buffer target (jitter-free reprojection), a shared temporal-accumulation compute pass (velocity reprojection + 3×3 neighborhood clamp + exponential blend over persistent ping-pong history), **frame-animated RT sampling that converges**: RT AO drops to 4 rays/frame and accumulates to dozens of effective samples, reflections converge their GGX noise away, and **TAA** (Halton-jittered projection) cleans every edge before bloom/tonemap. Shadow cascade passes are skipped entirely while RT shadows are active. ~0.8 ms/frame with everything on (debug, 1600×900, RTX 5080). Also new: `--screenshot <path>` writes the exact backbuffer to PNG for headless verification and flythrough capture. Still open from Phase 5: BLAS compaction, alpha-tested shadow rays.

**Phase 5 core — hybrid ray tracing.** BLAS per mesh built at import (geometries map 1:1 to primitives), double-buffered TLAS rebuilt per frame from the ECS, and three ray-query effects with per-scene toggles and intact raster fallbacks: **RT shadows** (sun *and* point lights — small casters like banners finally shadow correctly; falls back to CSM), **RT ambient occlusion** (8 cosine rays, IGN-jittered; falls back to texture AO), and **RT reflections** (compute pass, GGX importance-sampled ray queries with manual hit shading through bindless + buffer-device-address instance data, environment fallback on miss and for rough surfaces). ~2.3 ms/frame with everything on (debug, 1600×900, RTX 5080). Devices without RT select a raster-only device automatically. Remaining Phase 5 items: temporal accumulation/denoising (current sampling uses stable per-pixel jitter), BLAS compaction, alpha-tested shadow rays.

**Phase 4 — Candela Studio.** `candela-studio.exe`: Dear ImGui (docking) editor with the scene rendered offscreen into a **Viewport** panel (resizes live), **Hierarchy** (create/delete/drag-reparent), **Inspector** (undoable per-component editing), **Content browser** (drag models into the scene), **Scene Settings**, ImGuizmo translate/rotate/scale gizmos (W/E/R), click-to-select via an entity-ID G-buffer target with GPU readback, renderer debug views (albedo/normals/metal-rough/AO/cascades), play/stop via registry snapshot, and **undo/redo for every operation** (command stack with stable editor IDs that survive delete→undo). `--selftest` exercises the command stack and picking end-to-end. The sandbox remains the editor-free runtime.

**Phase 3** — Candela became an engine, not a renderer demo:
EnTT ECS (transform hierarchy, mesh renderers, lights, per-scene settings in registry context) with the renderer consuming the registry each frame; an asset registry that stamps sources with GUID `.meta` files, imports glTF models **asynchronously on a job system** (geometry streams in while the app runs — Sponza pops in ~1.5 s after the window appears), and re-imports automatically when source files change; `.candela` JSON scene serialization with a byte-identical save→load→save round-trip (`--roundtrip-check`); a typed event bus (asset-reloaded events); and input-action mapping replacing hardcoded keys. All GPU submission paths are mutex-serialized so imports run safely on worker threads.

**Phase 2** — deferred PBR pipeline, fully render-graph driven:
4 shadow cascades (texel-snapped, PCF) → G-buffer (albedo / octahedral normals / metallic-roughness-AO) → Cook-Torrance GGX lighting with sun + point lights and split-sum IBL (GPU-precomputed irradiance, GGX-prefiltered specular, BRDF LUT from a Poly Haven HDRI) → dual-Kawase bloom → ACES tonemap. Full glTF material loading: normal mapping with tangents, metallic-roughness, occlusion, sRGB-aware texture cache. Sponza renders validation-clean at ~550 fps (debug, 1600×900, RTX 5080); all shaders hot-reload as a group.

Run `scripts\get-assets.ps1` once to download the test content: Sponza, FlightHelmet, DamagedHelmet, MetalRoughSpheres (Khronos glTF samples), and a Poly Haven HDRI. All models appear in the editor's content browser; the sandbox can view one directly with auto-framing: `candela-sandbox --model assets\FlightHelmet\glTF\FlightHelmet.gltf` (add `--no-rt` to isolate the raster path, `--screenshot out.png --frames 1500` for headless captures). MetalRoughSpheres matches the Khronos reference render; DamagedHelmet is correct except its emissive visor glow (emissive support is a known deferral). The editor starts with an empty untitled scene; load saved scenes via File > Open Scene or `candela-studio --scene <path>`.

Deferred to later phases: emissive + velocity G-buffer targets (TAA/denoising), sky rendering from the environment cube, alpha-tested shadows, IBL disk caching, KTX2/BC texture compression (Phase 3 asset pipeline). Phase 1: render graph, bindless, glTF. Phase 0: Vulkan bring-up, Slang hot reload.

**Phase 6 complete — image quality & performance.** Demo flythrough ([docs/media/flythrough.mp4](docs/media/flythrough.mp4)): a 60 fps Catmull-Rom camera path through Sponza with every feature on, captured by the engine itself (`candela-sandbox --flythrough <dir>` writes one exact-backbuffer PNG per frame; per-frame-in-flight readback buffers make every-frame capture possible). Also landed: **BLAS compaction** (15.5 MB → 6.1 MB on the test content), **alpha-tested shadow rays** (alpha-masked geometry is non-opaque in the BLAS; visibility rays sample albedo alpha at candidate hits, so foliage casts leaf-shaped shadows). Bistro itself awaits a usable glTF port — the only public one uses AVIF textures.

**Phase 6 (core) — image quality & performance.** HDRI sky rendered from the environment cubemap along the view ray; CPU frustum culling against per-primitive AABBs (G-buffer only — shadow casters behind the camera still shadow the view; 25/103 draws culled in the default Sponza view); emissive materials (texture × factor into a sixth G-buffer target, added before bloom — DamagedHelmet's visor finally glows); persistent SPIR-V disk cache + VkPipelineCache (editor cold start 4.2 s → **1.1 s warm**); GitHub Actions build CI that compiles without an installed Vulkan SDK (FetchContent headers fallback). Remaining for Phase 6: Bistro demo scene + captured flythrough, alpha-tested shadow rays, BLAS compaction.

**Post-roadmap: editor identity + stress benchmark.** Candela Studio now has its own look — charcoal theme with a candlelight-amber accent, Roboto type, a floating gizmo-mode toolbar in the viewport, and an amber Play button. `candela-sandbox --benchmark` generates and flies a deliberately brutal scene: **960 DamagedHelmets (~139M scene triangles), 961 TLAS instances rebuilt every frame, and 64 ray-traced-shadow point lights** through Sponza, then reports frame-time statistics — currently avg 17.3 ms / 58 fps (p50 16.1, p95 20.9; debug build, 1600×900, RTX 5080). The generated `benchmark.candela` opens in the editor. Headroom for the someday-list's light clustering and GPU-driven drawing to chew on.

**Post-roadmap: Blender `.blend` import.** `.blend` files in the content directory are now first-class model assets: the registry converts them by running Blender headless with its bundled glTF exporter (cached as `.glb` under a `.candela-import/` folder next to the source, reconverted automatically when the `.blend` changes) and routes the result through the existing glTF pipeline — materials, textures, and the object hierarchy all come across. They appear in the content browser like any model, and **File > Import Blender Scene** opens one as a fresh scene. Requires a Blender installation, auto-detected from `PATH` and standard install locations (override with the `CANDELA_BLENDER` environment variable).

See [docs/ROADMAP.md](docs/ROADMAP.md) for the phased build plan.
