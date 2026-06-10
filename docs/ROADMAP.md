# Candela Roadmap

Seven phases. Each phase ends with something visible on screen — no phase is "infrastructure only" except Phase 0, which is deliberately tiny. Phases 4 and 5 can be swapped or interleaved; the editor only depends on the raster renderer.

---

## Phase 0 — Foundation
**Goal: a window clearing to a color, with the whole toolchain proven.**

- CMake superbuild + vcpkg manifest (`vcpkg.json`), C++20, MSVC. Warnings-as-errors, clang-format config.
- Vulkan 1.3 instance/device via vk-bootstrap, loaded through volk. Validation layers + debug-utils object naming on from day one.
- Swapchain with frames-in-flight (2), per-frame command pools, a deletion queue for deferred resource destruction.
- VMA integration; thin RAII wrappers for buffers/images (`GpuBuffer`, `GpuImage`).
- GLFW window + input events behind a `platform/` interface (the Linux seam).
- spdlog logging, assert macros, Tracy hooked up (CPU zones + Vulkan GPU zones).
- Slang build step: compile `shaders/*.slang` → SPIR-V at build time; runtime file-watch hot reload.

**Exit criteria:** window clears to a color, validation-clean, Tracy shows the frame, shader hot reload swaps the clear via a fullscreen triangle.

## Phase 1 — Core renderer architecture
**Goal: a textured glTF mesh on screen, through a render graph, with bindless resources.**

- **Render graph:** passes declare image/buffer reads & writes; graph computes barriers (sync2) and transient resource lifetimes; uses dynamic rendering (no VkRenderPass). This is the spine everything else hangs on — every later feature is "add a pass."
- **Bindless descriptor model:** one global descriptor set of sampled images (descriptor indexing, update-after-bind); materials reference textures by index; vertex/material data pulled via buffer device address. Pipeline layout settled here so RT shaders can share it later.
- Pipeline + shader-module cache keyed by shader hash; hot reload rebuilds pipelines.
- fastgltf scene import: meshes, transforms, materials, textures (KTX2/BC-compressed via KTX-Software on import).
- Depth buffer, reverse-Z, fly camera.

**Exit criteria:** Sponza loads and renders unlit/textured at >144 fps, resize works, no validation errors, render-graph passes visible as Tracy GPU zones.

## Phase 2 — PBR and raster lighting
**Goal: Sponza looks *good*. This is the fallback path that ships under every RT effect.**

- Deferred G-buffer pass: albedo, normal (octahedral-encoded), metallic/roughness/AO, emissive, velocity (for TAA/denoising later).
- Cook-Torrance GGX direct lighting; directional + point + spot lights as a GPU light buffer.
- **IBL:** HDR equirect → cubemap, irradiance convolution, prefiltered specular mip chain, BRDF LUT — all offline-ish compute passes cached to disk.
- Cascaded shadow maps for the sun (4 cascades, PCF) — this remains the no-RT shadow fallback.
- HDR throughout; tonemap pass (ACES or AgX), physically based exposure, bloom (downsample/upsample chain).

**Exit criteria:** glTF PBR sample models match the Khronos reference renderer side-by-side; Sponza with sun + IBL + shadows looks credible.

## Phase 3 — Engine systems
**Goal: it stops being a renderer demo and becomes an engine.**

- EnTT ECS: `Transform` (with hierarchy/dirty propagation), `MeshRenderer`, `Light`, `Camera`, `Name` components; renderer consumes the registry instead of a hardcoded scene.
- Asset system: content folder with GUID-stamped `.meta` files; import pipeline (glTF → internal mesh/material/texture assets); asset registry with async load; hot reload.
- Scene serialization to JSON (`.candela` scenes): save/load round-trip.
- Job system (small thread-pool with work stealing, or enkiTS) — asset loads and command recording go wide.
- Event bus + input action mapping.

**Exit criteria:** build a scene in code, save it, restart, load it back identically; assets re-import when source files change.

## Phase 4 — Editor (Candela Studio)
**Goal: a usable editor you stop wanting to quit.**

- ImGui docking shell; scene renders to an offscreen target shown in a **Viewport** panel (handles resize, aspect, multiple viewports later).
- **Hierarchy** panel (create/delete/reparent via drag-drop), **Inspector** (per-component UI via a small reflection/registration mechanism), **Content browser** (thumbnails, drag into scene).
- ImGuizmo translate/rotate/scale gizmos; editor camera (orbit + fly).
- Mouse picking via an entity-ID buffer rendered alongside the G-buffer.
- Undo/redo as a command stack — retrofitting this is miserable, so it lands with the first edit operation, not after.
- Play/stop: snapshot the registry on play, restore on stop.
- Renderer debug views (albedo/normals/roughness/cascades) in a dropdown.

**Exit criteria:** assemble and light a scene entirely with the mouse, save it, and undo every step of it.

## Phase 5 — Ray tracing
**Goal: the hybrid lighting that justifies the engine's name.**

Ordered by payoff-per-effort; each effect is a render-graph pass with a raster fallback and an editor toggle.

1. **Acceleration structures:** BLAS per mesh (built on load, compacted), TLAS rebuilt/refit per frame from the ECS. Instance data shared with the bindless material model from Phase 1.
2. **RT shadows** (ray queries inside the deferred lighting pass — no RT pipeline needed yet): per-light visibility rays, soft shadows via light-radius cone sampling. Fallback: CSM/PCF.
3. **RT ambient occlusion:** short rays from the G-buffer, temporally accumulated. Fallback: GTAO (add it here if RT-off parity matters).
4. **RT reflections:** full RT pipeline (raygen/hit/miss, shader binding table), GGX importance-sampled rays from G-buffer roughness/normals, hit-point shading reuses the deferred light loop. Fallback: screen-space reflections or IBL-only.
5. **Denoising:** temporal accumulation + à-trous spatial filter (SVGF-style), or integrate AMD FidelityFX Denoiser. Velocity buffer from Phase 2 pays off here.
6. **(Stretch)** one-bounce diffuse GI via RT, or DDGI probe grid.

**Exit criteria:** Sponza/Bistro with RT shadows + AO + reflections at interactive rates on the dev GPU; toggling each effect at runtime cleanly swaps in its fallback.

## Phase 6 — Image quality & performance
**Goal: it looks and runs like a serious engine.**

- TAA (velocity-buffer-based) — also stabilizes the RT denoisers; FSR 2/3 upscaling as a stretch goal.
- Frustum culling, then GPU-driven drawing (compute culling + draw-indirect) as the scene count demands it.
- Pipeline cache to disk, async pipeline compilation, GPU timestamp HUD.
- A polished demo scene (Bistro or similar) and a captured flythrough — the engine's portfolio shot.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Vulkan boilerplate burnout before pixels appear | Phase 0 is intentionally minimal; vk-bootstrap/VMA/dynamic rendering cut the classic 1000-line triangle to a fraction |
| Render graph over-engineering | Build the simplest version that computes barriers; resist generic-framework ambitions until ≥3 real passes exist |
| Bindless/RT descriptor mismatch discovered late | Bindless layout is settled in Phase 1 explicitly so RT shaders can share it |
| Editor undo retrofitted too late | Undo lands with the first editor edit operation (Phase 4, by design) |
| Denoising rabbit hole | Start with plain temporal accumulation; SVGF/FidelityFX only once accumulation visibly falls short |
| Scope creep (animation, physics, audio…) | Out of scope until Phase 6 ships; tracked in a someday-list, not the roadmap |

## Deliberately out of scope (for now)

Skeletal animation, physics, audio, networking, scripting (Lua is the likely first addition), macOS/MoltenVK (no RT support), and any console targets.
