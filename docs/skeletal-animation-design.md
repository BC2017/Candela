# Candela — Skeletal Animation (Compute Pre-Skinning) — Implementation Design

Approach is **decided and fixed**: a compute pass reads bind-pose `Vertex[]` +
per-vertex joint/weight data + a joint-matrix palette and writes fully-skinned
vertices into a per-instance buffer with the **identical 48-byte `Vertex`
layout**. Every downstream consumer (G-buffer VS, shadow VS, RT `InstanceData`)
is simply pointed at the skinned buffer. No vertex-shader skinning (it would
desync the raster geometry from the ray-traced BLAS). The BLAS of skinned
meshes is **refit** (mode = UPDATE) from the skinned buffer each frame so RT
shadows / AO / reflections track the animation.

This is a **vertical slice**: one animated skinned model rendering correctly in
both raster and RT paths, plus a GPU-free `--animinfo` CLI dump and a pure-logic
sampling test. Deferred (explicitly NOT implemented): per-vertex deformation
motion vectors — skinned meshes get camera velocity only in v1 (the existing
`GBufferPush.model`-as-prev-transform behavior is unchanged).

All new work respects the hard constraints: `Vertex` stays 48 bytes;
`GBufferPush` stays exactly 128 bytes and full; the render graph tracks image
barriers only, so all skinning/BLAS work happens on the **raw** command buffer
before `graph.execute()`, exactly where `fillRayTracingInstances()` / `buildTLAS()`
already live; FetchContent-only deps (no new deps needed at all — glTF skins are
parsed with the fastgltf we already fetch); MSVC + GCC clean; edits to shared
files are additive and localized for a clean later 3-way merge.

---

## 0. Data-flow summary (per frame)

```
World::updateAnimations(dt)          // NEW: sample clip → write joint LocalTransform
World::updateTransforms()            // existing: joints' WorldTransform now animated
Renderer::drawFrame
  fillRayTracingInstances(...)       // existing (CPU, pre-record)
  fillSkinningData(frame, world, ...) // NEW (CPU, pre-record): palette matrices +
                                      //   skinned-instance table into mapped ring buffers
  recordCommands(raw cmd):
    [raw] skinning compute dispatch   // NEW: reads bindpose Vertex[] + SkinVertex[] +
                                      //   palette → writes skinned Vertex[] (per instance)
    [raw] buffer barrier COMPUTE_WRITE → { VERTEX_SHADER read, AS_BUILD read }
    [raw] refitSkinnedBlas(...)       // NEW: per skinned instance, mode=UPDATE from skinned buf
    [raw] AS-build barrier            // (refit write → AS read, folded into existing TLAS path)
    [raw] buildTLAS(...)              // existing — now references refit BLAS addresses
    graph.execute(...)               // existing passes; skinned draws point Vertex* at skinned buf
```

The skinned-vertex ring and palette ring are **per-frame-in-flight** and
host-visible mapped, matching the existing `tlasInstances` / `instanceData`
pattern in `createRayTracingResources()`.

---

## 1. Asset layer — `engine/src/candela/assets/ModelAsset.h` (MODIFY)

Additive declarations only; existing structs unchanged except two new fields on
`GpuPrimitive` and one on `NodeTemplate`.

```cpp
// New CPU/GPU vertex-side skin attributes. 24 bytes, tightly packed.
struct SkinVertex {
    uint16_t joints[4];   // indices into the Skin.jointNodes palette
    float    weights[4];
};
static_assert(sizeof(SkinVertex) == 24, "SkinVertex layout must match skinning.slang");

// A glTF skin: which nodes are joints, their inverse bind matrices, optional
// skeleton root. jointNodes/inverseBind are parallel arrays (palette order).
struct Skin {
    std::vector<int>       jointNodes;   // indices into ModelAsset::nodes
    std::vector<glm::mat4> inverseBind;  // one per joint, same order
    int                    skeletonRoot = -1; // node index, -1 if unspecified
};

// One animated property track (TRS of a single node) sampled over time.
struct AnimationChannel {
    int   targetNode = -1;                 // index into ModelAsset::nodes
    enum class Path : uint8_t { Translation, Rotation, Scale } path;
    enum class Interp : uint8_t { Step, Linear, CubicSpline } interp = Interp::Linear;
    std::vector<float>     times;          // keyframe input (seconds), sorted
    std::vector<glm::vec4> values;         // vec3 in xyz (T/S) or quat xyzw (R)
};

struct AnimationClip {
    std::string                   name;
    float                         duration = 0.0f; // max keyframe time
    std::vector<AnimationChannel> channels;
};
```

`GpuPrimitive` gains (append after existing fields, before the bounds block is
fine — keep it localized near the geometry buffers):

```cpp
    // Skinning: present iff the owning mesh's node had a skinIndex. The BLAS
    // for skinned meshes is built ALLOW_UPDATE and refit per frame.
    bool      skinned = false;
    GpuBuffer skinVertexBuffer;  // SkinVertex[vertexCount], BDA-addressable
```

`GpuMesh` gains one flag (a mesh is skinned iff any instantiating node has a
skin; simpler: derived at instantiate-time, but we mark the primitive at import
so the BLAS build path can choose ALLOW_UPDATE):

```cpp
    bool skinned = false; // any primitive skinned → BLAS built ALLOW_UPDATE
```

`NodeTemplate` gains:

```cpp
    int skinIndex = -1; // index into ModelAsset::skins, -1 = not skinned
```

`ModelAsset` gains:

```cpp
    std::vector<Skin>          skins;
    std::vector<AnimationClip> animations;
```

`ModelAsset::destroy()` (in .cpp) must also `destroyBuffer(context,
primitive.skinVertexBuffer)` when present. `skins`/`animations`/`nodes` are
`.clear()`-ed.

### Import changes — `engine/src/candela/assets/ModelAsset.cpp` (MODIFY)

fastgltf API confirmed from the fetched headers
(`build/debug/_deps/fastgltf-src/include/fastgltf/types.hpp`):
- `fastgltf::Asset::skins` → `std::vector<fastgltf::Skin>`;
  `Skin{ Optional<size_t> inverseBindMatrices; Optional<size_t> skeleton;
  MaybeSmallVector<size_t> joints; string name; }`.
- `fastgltf::Asset::animations` → `std::vector<fastgltf::Animation>`;
  `Animation{ MaybeSmallVector<AnimationChannel> channels;
  MaybeSmallVector<AnimationSampler> samplers; string name; }`.
- `AnimationChannel{ size_t samplerIndex; Optional<size_t> nodeIndex;
  AnimationPath path; }`; `AnimationPath{ Translation=1, Rotation=2, Scale=3,
  Weights=4 }`.
- `AnimationSampler{ size_t inputAccessor; size_t outputAccessor;
  AnimationInterpolation interpolation; }`;
  `AnimationInterpolation{ Linear=0, Step=1, CubicSpline=2 }`.
- `fastgltf::Node::skinIndex` → `Optional<size_t>`.
- glm element traits already exist for `glm::u16vec4` and `glm::u8vec4`
  (`glm_element_traits.hpp` lines 38/56), so JOINTS_0 is read with
  `iterateAccessorWithIndex<glm::u16vec4>` (fastgltf widens u8→u16); WEIGHTS_0
  with `iterateAccessorWithIndex<glm::vec4>` (handles float + normalized
  integer sources).

**Per-primitive skin attributes** (inside the existing primitive loop, right
after the TANGENT block, before the index read):

```cpp
bool primSkinned = false;
std::vector<SkinVertex> skinVerts;
if (const auto* jointsAttr = primitive.findAttribute("JOINTS_0");
    jointsAttr != primitive.attributes.end()) {
    if (const auto* weightsAttr = primitive.findAttribute("WEIGHTS_0");
        weightsAttr != primitive.attributes.end()) {
        primSkinned = true;
        skinVerts.assign(positionAccessor.count, SkinVertex{});
        fastgltf::iterateAccessorWithIndex<glm::u16vec4>(
            asset, asset.accessors[jointsAttr->accessorIndex],
            [&](glm::u16vec4 j, size_t i) {
                skinVerts[i].joints[0]=j.x; skinVerts[i].joints[1]=j.y;
                skinVerts[i].joints[2]=j.z; skinVerts[i].joints[3]=j.w;
            });
        fastgltf::iterateAccessorWithIndex<glm::vec4>(
            asset, asset.accessors[weightsAttr->accessorIndex],
            [&](glm::vec4 w, size_t i) {
                // glTF weights need not sum to 1; renormalize defensively.
                float s = w.x+w.y+w.z+w.w; if (s > 0.0f) w /= s;
                skinVerts[i].weights[0]=w.x; skinVerts[i].weights[1]=w.y;
                skinVerts[i].weights[2]=w.z; skinVerts[i].weights[3]=w.w;
            });
    }
}
```

When `primSkinned`, upload the skin buffer and set `gpu.skinned`:

```cpp
if (primSkinned) {
    gpu.skinned = true;
    gpu.skinVertexBuffer = createBufferWithData(
        context, std::as_bytes(std::span(skinVerts)),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    mesh.skinned = true;
}
```

**BLAS build flag** (in the "Bottom-level acceleration structures" section):
when `mesh.skinned`, OR `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`
into `buildInfo.flags`, and **skip compaction** for that mesh (a compacted AS
cannot be updated — compaction and ALLOW_UPDATE are mutually exclusive in
practice; keep the build-sized structure so per-frame refit is legal). Concretely:

```cpp
const bool allowUpdate = mesh.skinned;
buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
    | (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                   : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
```
and guard the whole compaction block with `if (!allowUpdate) { ...compact... }`,
still fetching `blasAddress` at the end for both branches. This keeps the change
localized to two spots in the existing loop.

> Note: the import-time BLAS is built from the **bind-pose** vertex buffer.
> That's correct: refit only re-fits triangle positions from the skinned buffer;
> the initial build just establishes topology/size. The refit path (renderer)
> supplies the skinned vertex address as the geometry source.

**Skins** (new section, after meshes / before or after node hierarchy — placing
it after the node loop lets us keep node indices):

```cpp
model.skins.resize(asset.skins.size());
for (size_t s = 0; s < asset.skins.size(); ++s) {
    const auto& gs = asset.skins[s];
    Skin& skin = model.skins[s];
    skin.skeletonRoot = gs.skeleton ? int(*gs.skeleton) : -1;
    skin.jointNodes.assign(gs.joints.begin(), gs.joints.end());
    skin.inverseBind.assign(skin.jointNodes.size(), glm::mat4(1.0f));
    if (gs.inverseBindMatrices) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
            asset, asset.accessors[*gs.inverseBindMatrices],
            [&](const fastgltf::math::fmat4x4& m, size_t i) {
                std::memcpy(&skin.inverseBind[i], m.data(), sizeof(float)*16);
            });
    }
}
```
(fastgltf `math::fmat4x4` is column-major, matching glm; `.data()` yields 16
contiguous floats — verify with `math.hpp`; a manual col/row copy like the
existing node-matrix decompose is the fallback if `.data()` is absent.)

**Node skinIndex** (in the existing node loop, alongside `meshIndex`):

```cpp
if (gltfNode.skinIndex) node.skinIndex = int(*gltfNode.skinIndex);
```

**Animations** (new section):

```cpp
model.animations.resize(asset.animations.size());
for (size_t a = 0; a < asset.animations.size(); ++a) {
    const auto& ga = asset.animations[a];
    AnimationClip& clip = model.animations[a];
    clip.name = ga.name;
    for (const auto& ch : ga.channels) {
        if (!ch.nodeIndex) continue;
        if (ch.path == fastgltf::AnimationPath::Weights) continue; // morphs deferred
        const auto& samp = ga.samplers[ch.samplerIndex];
        AnimationChannel out;
        out.targetNode = int(*ch.nodeIndex);
        out.path = ch.path == fastgltf::AnimationPath::Translation
            ? AnimationChannel::Path::Translation
            : ch.path == fastgltf::AnimationPath::Scale
            ? AnimationChannel::Path::Scale : AnimationChannel::Path::Rotation;
        out.interp = samp.interpolation == fastgltf::AnimationInterpolation::Step
            ? AnimationChannel::Interp::Step
            : samp.interpolation == fastgltf::AnimationInterpolation::CubicSpline
            ? AnimationChannel::Interp::CubicSpline
            : AnimationChannel::Interp::Linear;
        fastgltf::iterateAccessor<float>(
            asset, asset.accessors[samp.inputAccessor],
            [&](float t){ out.times.push_back(t); });
        if (out.path == AnimationChannel::Path::Rotation) {
            fastgltf::iterateAccessor<glm::vec4>(
                asset, asset.accessors[samp.outputAccessor],
                [&](glm::vec4 q){ out.values.push_back(q); });
        } else {
            fastgltf::iterateAccessor<glm::vec3>(
                asset, asset.accessors[samp.outputAccessor],
                [&](glm::vec3 v){ out.values.push_back(glm::vec4(v,0.0f)); });
        }
        if (!out.times.empty()) clip.duration =
            std::max(clip.duration, out.times.back());
        clip.channels.push_back(std::move(out));
    }
}
```

The pure sampling math is factored into a **header-only** helper so the
headless test links it without pulling in Vulkan (see §5):
`engine/src/candela/assets/AnimationSampling.h` (NEW) — `sampleChannel(clip,
channelIndex, t)` returning a TRS delta, and `sampleClip(clip, t, outLocalTRS
per node)`. `ModelAsset.cpp` and `World.cpp` both include it.

---

## 2. ECS — `engine/src/candela/scene/Components.h` (MODIFY, append only)

```cpp
struct SkinnedMeshRenderer {
    AssetGuid model = kInvalidGuid;
    uint32_t  meshIndex = 0;
    int       skinIndex = -1; // into ModelAsset::skins
};

// Joint entities + their inverse-bind matrices, in palette order. joints[j]
// is the entt entity whose WorldTransform gives joint j's world matrix.
struct Skeleton {
    std::vector<entt::entity> joints;
    std::vector<glm::mat4>    inverseBind;
};

// Drives one clip on the skeleton this entity owns.
struct Animator {
    AssetGuid model = kInvalidGuid;
    int       clip  = 0;     // index into ModelAsset::animations
    float     time  = 0.0f;  // seconds
    float     speed = 1.0f;
    bool      loop  = true;
    bool      playing = true;
};
```

`Skeleton` and `SkinnedMeshRenderer` live on the same entity (the node that had
`skinIndex`). `Animator` also lives there for the v1 slice (one animator per
skinned entity). Not serialized in v1 beyond a minimal round-trip stub (§6).

---

## 3. World — `engine/src/candela/scene/World.h/.cpp` (MODIFY)

### `instantiateModel` (MODIFY, additive block)

After the existing node-entity creation loop and BEFORE returning, wire skins:
for each node `i` with `model->nodes[i].skinIndex >= 0`, attach a
`SkinnedMeshRenderer` **in place of** the plain `MeshRenderer` that the mesh
branch would add (guard the existing `MeshRenderer` emplace with
`node.skinIndex < 0`), then build the `Skeleton` from the skin's `jointNodes`
mapped through `nodeEntities`, and attach an `Animator` if the model has clips:

```cpp
// (inside the per-node loop, replacing the unconditional MeshRenderer add)
if (node.meshIndex >= 0) {
    if (node.skinIndex >= 0) {
        registry.emplace<SkinnedMeshRenderer>(entity, guid,
            uint32_t(node.meshIndex), node.skinIndex);
    } else {
        registry.emplace<MeshRenderer>(entity, guid, uint32_t(node.meshIndex));
    }
}
```

```cpp
// (after the parent-link loop, once nodeEntities is fully populated)
for (size_t i = 0; i < model->nodes.size(); ++i) {
    const int si = model->nodes[i].skinIndex;
    if (si < 0) continue;
    const Skin& skin = model->skins[size_t(si)];
    Skeleton skel;
    skel.joints.reserve(skin.jointNodes.size());
    for (int jn : skin.jointNodes)
        skel.joints.push_back(nodeEntities[size_t(jn)]);
    skel.inverseBind = skin.inverseBind;
    registry.emplace<Skeleton>(nodeEntities[i], std::move(skel));
    if (!model->animations.empty())
        registry.emplace<Animator>(nodeEntities[i], guid, 0, 0.0f, 1.0f, true, true);
}
```

`instantiateModel` already takes `AssetRegistry&` and holds the `ModelAsset*`,
so `skins`/`animations` are in scope.

### `updateAnimations(float dt)` (NEW method)

Declared in `World.h`:
```cpp
    // Advances every Animator, samples its clip, and writes the sampled TRS
    // into each targeted joint entity's LocalTransform. MUST run before
    // updateTransforms() so the animated pose flows through the hierarchy.
    void updateAnimations(AssetRegistry& assets, float dt);
```

Definition (`World.cpp`), using the header-only `AnimationSampling.h`:
```cpp
void World::updateAnimations(AssetRegistry& assets, float dt) {
    for (auto [e, anim] : registry.view<Animator>().each()) {
        if (!anim.playing) continue;
        const ModelAsset* model = assets.tryGetModel(anim.model);
        if (!model || anim.clip >= int(model->animations.size())) continue;
        const AnimationClip& clip = model->animations[size_t(anim.clip)];
        anim.time += dt * anim.speed;
        if (anim.loop && clip.duration > 0.0f)
            anim.time = std::fmod(anim.time, clip.duration);
        // The Animator lives on the skinned entity; its Skeleton names the
        // joint entities. Channels target model node indices, so we need the
        // node→entity map. In v1 we reuse the Skeleton's joint entities and a
        // per-Animator node-entity lookup captured at instantiate time OR we
        // sample by matching channel.targetNode to the skin's jointNodes.
        // (see note below)
        applySampledPose(registry, *model, clip, anim.time);
    }
}
```

**Node→entity resolution.** Channels target *model node indices*; joints are a
subset of nodes, but animation channels can target non-joint nodes too (e.g. a
root motion node). For a clean v1 that covers the common rigged-character case,
we resolve by joint palette: `applySampledPose` samples each channel, and for
channels whose `targetNode` appears in the skinned entity's `Skeleton` (via a
`nodeIndex→jointSlot` map we also store), writes the joint entity's
`LocalTransform`. To keep this robust and simple, **extend `Skeleton`** with a
parallel `std::vector<int> jointNodeIndex;` (the model node index of each joint)
so `updateAnimations` can map `channel.targetNode → joint entity`. This keeps
everything inside the ECS with no side tables.

> Design decision: store `jointNodeIndex` on `Skeleton` (append the field) so
> the sampler maps channel target nodes to joint entities without a separate
> map. Channels targeting nodes not in the skin are ignored in v1 (covers
> standard single-skeleton characters; documented as a limitation).

`applySampledPose(registry, model, clip, t)` (free function in `World.cpp`,
using `AnimationSampling.h`): for each channel, `sampleChannel` → a T, R, or S
value; find the joint entity via the owning skeleton's `jointNodeIndex`; write
the corresponding `LocalTransform` field. Iterating skinned entities once and
their channels is O(channels), tiny.

The **palette** itself (`inverse(meshWorld) * world(joint) * inverseBind`) is
NOT computed here — it needs `WorldTransform`, produced by the subsequent
`updateTransforms()`. The renderer computes it in `fillSkinningData` (§4.2),
after transforms are current. This matches the task's data-flow.

### Call-site ordering

Anywhere `world.updateTransforms()` is called each frame (sandbox loop, editor,
games), insert `world.updateAnimations(assets, dt)` immediately before it. For
the sandbox that's in `main.cpp` (§6). `updateTransforms` is unchanged.

---

## 4. Renderer — `engine/src/candela/renderer/Renderer.h/.cpp` (MODIFY)

### 4.1 Resources (Renderer.h, FrameData + constants)

Add to `FrameData` (per frame in flight):
```cpp
    // Skinning: one large ring the skinned vertices for ALL skinned instances
    // this frame are packed into (sub-allocated by offset), plus the joint
    // palette ring. Both host-visible-mapped and BDA-addressable, like the
    // TLAS instance buffers.
    GpuBuffer skinnedVertices;      // Vertex[], device address per sub-range
    GpuBuffer jointPalette;         // mat4[], mapped
    void*     jointPaletteMapped = nullptr;
```
`skinnedVertices` is device-local (written by compute, read by VS/AS build — no
host mapping needed); `jointPalette` is host-visible mapped (written CPU-side in
`fillSkinningData`). Sizes: `kMaxSkinnedVertices` (e.g. 1<<20 vertices ×48B =
48 MB) and `kMaxJointMatrices` (e.g. 4096 × 64B). New constants next to
`kMaxRTInstances`:
```cpp
    static constexpr uint32_t kMaxSkinnedVertices = 1u << 20;
    static constexpr uint32_t kMaxJointMatrices   = 4096;
```
Created in a new `createSkinningResources()` called from the ctor right after
`createRayTracingResources()` (guarded by nothing — skinning works without RT;
the compute pass and raster path don't need ray tracing, only the BLAS refit
does and that's already RT-gated).

`skinnedVertices` usage flags:
`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
| (rtSupported ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0)`.

New pipeline handle: `VkPipeline m_skinningPipeline = VK_NULL_HANDLE;` built in
`createPipelines()` via `buildComputePipeline("skinning.slang", "csMain")`
(always required — add to the always-built group, not RT-gated). Destroy it in
the dtor's pipeline list and `createPipelines` swap arrays (extend the fixed-size
arrays from 9 to 10, and `required`/loop bounds accordingly, keeping the RT-gated
slots last).

New per-frame CPU-side record table (Renderer.h, near `m_frameDraws`):
```cpp
    struct SkinnedInstance {
        const GpuPrimitive* primitive;      // bind-pose source + skin buffer
        VkDeviceAddress     skinnedVertices; // sub-range in frame.skinnedVertices
        uint32_t            paletteOffset;   // first joint matrix index
        uint32_t            jointCount;
        glm::mat4           transform;
        uint32_t            entityId;
        bool                visible;
    };
    std::vector<SkinnedInstance> m_skinnedInstances;
    uint32_t m_skinnedVertexCursor = 0; // running sub-alloc offset (vertices)
    uint32_t m_paletteCursor = 0;       // running palette offset (matrices)
```

### 4.2 `fillSkinningData(FrameData&, World&, AssetRegistry&)` (NEW, CPU, pre-record)

Called in `drawFrame` right after `fillRayTracingInstances`, i.e. after the
frame's fence has cleared (safe to write mapped buffers) and after
`world.updateTransforms()` has run this frame. Walks
`registry.view<WorldTransform, SkinnedMeshRenderer, Skeleton>()`:

For each skinned entity:
1. Resolve `model = assets.tryGetModel(smr.model)`; skip if not ready / mesh
   out of range.
2. `meshWorld = worldTransform.value`; `invMeshWorld = inverse(meshWorld)`.
3. Palette: for each joint `j`, `palette[paletteBase+j] = invMeshWorld *
   registry.get<WorldTransform>(skel.joints[j]).value * skel.inverseBind[j]`.
   Write into `frame.jointPaletteMapped` at `m_paletteCursor`. Guard against
   `kMaxJointMatrices`.
4. For each primitive of the mesh with `primitive.skinned`, sub-allocate
   `primitive.vertexCount` vertices from `frame.skinnedVertices` at
   `m_skinnedVertexCursor` (compute the device address =
   `frame.skinnedVertices.deviceAddress + cursor*sizeof(Vertex)`), push a
   `SkinnedInstance{ primitive, skinnedAddr, paletteBase, jointCount,
   meshWorld, entityId, visible }`, advance cursor. Guard against
   `kMaxSkinnedVertices`.
5. Frustum-cull with the same `aabbVisible` used for `m_frameDraws` (skinned
   meshes use bind-pose bounds inflated conservatively — v1 uses primitive
   bounds directly; acceptable for the slice, note as a known slight
   under-conservatism).

Reset `m_skinnedVertexCursor`, `m_paletteCursor`, `m_skinnedInstances.clear()`
at the top. Skinned entities are **excluded** from the normal `m_frameDraws`
loop and the `fillRayTracingInstances` loop, which iterate `MeshRenderer`
(skinned entities carry `SkinnedMeshRenderer` instead — no code change needed
there, they simply won't match the `MeshRenderer` view). Skinned RT instances
are appended to the TLAS in a dedicated pass (§4.4).

### 4.3 Skinning compute dispatch (raw cmd, in `recordCommands`, before graph)

Placed at the very top of `recordCommands`, BEFORE the existing TLAS-build
block (skinned vertices must exist before BLAS refit and before the graph's VS
reads). One dispatch per skinned instance (counts are small — one character in
the slice):

```cpp
struct SkinningPush {                   // fresh 128B budget, BDA pointers
    VkDeviceAddress bindPoseVertices;   // Vertex*  (source)
    VkDeviceAddress skinVertices;       // SkinVertex*
    VkDeviceAddress palette;            // float4x4* (frame.jointPalette + offset)
    VkDeviceAddress skinnedVertices;    // Vertex*  (dest sub-range)
    uint32_t        vertexCount;
    uint32_t        pad[3];
};
```
For each `SkinnedInstance`: bind `m_skinningPipeline`, push the four addresses
(`palette = frame.jointPalette.deviceAddress + paletteOffset*sizeof(mat4)`),
`vkCmdDispatch((vertexCount+63)/64, 1, 1)`. Wrap in a `TracyVkZone` +
timestamp pair named `"skinning"` matching the existing tlas-build timestamp
convention.

After all dispatches, one buffer barrier on the raw cmd:
`srcStage=COMPUTE_SHADER, srcAccess=SHADER_WRITE` →
`dstStage=VERTEX_SHADER | ACCELERATION_STRUCTURE_BUILD_KHR`,
`dstAccess=SHADER_READ | ACCELERATION_STRUCTURE_READ_KHR |
VK_ACCESS_2_SHADER_STORAGE_READ_BIT`. Use a global `VkMemoryBarrier2`
(mirrors `buildTLAS`'s barrier style). This covers both the graph's G-buffer/
shadow VS reads (graph doesn't insert buffer barriers) and the refit reads.

### 4.4 BLAS refit + TLAS (raw cmd)

New `refitSkinnedBlas(VkCommandBuffer cmd, FrameData&)`: for each unique skinned
`GpuMesh` referenced this frame, issue `vkCmdBuildAccelerationStructuresKHR`
with `mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`,
`srcAccelerationStructure = dstAccelerationStructure = mesh.blas`, flags
including `ALLOW_UPDATE`, geometry `triangles.vertexData.deviceAddress = <this
instance's skinned sub-range address>`, reusing per-frame scratch (a new
`frame.blasRefitScratch` buffer, sized to the max refit scratch — query with
`vkGetAccelerationStructureBuildSizesKHR` at create time; UPDATE scratch is
`updateScratchSize`). Then a barrier AS_BUILD_WRITE → AS_BUILD_READ before
`buildTLAS`.

> Complication: the BLAS is a single AS per mesh but skinned geometry is
> instance-specific (each instance has its own palette). For the v1 slice we
> support **one instance per skinned mesh** (the common case: a unique
> character). Refit updates that mesh's BLAS from that instance's skinned
> sub-range. Multiple instances of the same skinned mesh sharing one BLAS is a
> documented v1 limitation (would need per-instance BLAS copies). The
> `SkinnedInstance` → mesh mapping enforces this by refitting per instance in
> order; last write wins, and the slice scene has one.

TLAS: extend `fillRayTracingInstances` (or add a sibling
`appendSkinnedRTInstances`) to also emit `VkAccelerationStructureInstanceKHR`
for each `SkinnedInstance`, with `accelerationStructureReference =
mesh.blasAddress` and `instanceCustomIndex` pointing at InstanceData entries
whose `vertices` field = the **skinned** sub-range address (so RT hit-shading
reads skinned positions/uvs). This is the "point InstanceDataGPU.vertices at the
skinned buffer" requirement. Keep it a small additive loop after the existing
`MeshRenderer` loop, sharing the `instanceCount`/`dataCount` cursors.

### 4.5 Raster draw (graph G-buffer & shadow passes)

In `recordCommands`, the G-buffer and shadow pass `execute` lambdas currently
iterate `m_frameDraws`. Append a second loop over `m_skinnedInstances` that
issues the identical draw but with `push.vertices = inst.skinnedVertices` (the
skinned sub-range address) — for the G-buffer, all other `GBufferPush` fields
come from `inst.primitive` exactly as for `FrameDraw`; `GBufferPush` is
untouched (still 128B, we only change what `vertices` points at). Same for
`ShadowPush.vertices`. Capture `m_skinnedInstances` by copying the needed data
into the lambda (they already capture `this`; the vectors are members, so
`this` access is fine and matches the existing `m_frameDraws` capture).

No new G-buffer/shadow pipeline — skinned meshes reuse `m_gbufferPipeline` /
`m_shadowPipeline` unchanged (the whole point of pre-skinning).

### 4.6 `drawFrame` ordering (MODIFY)

```
vkWaitForFences(...)
m_rtInstanceCount = fillRayTracingInstances(...)   // existing
fillSkinningData(frame, world, assets)             // NEW
... existing timestamp/pick/screenshot readback ...
recordCommands(...)   // now: skinning dispatch → barrier → refit → TLAS → graph
```
`updateAnimations` + `updateTransforms` happen in the app loop before
`drawFrame` (the renderer consumes current `WorldTransform`s). Confirmed by the
existing contract: the sandbox already calls `world.updateTransforms()` before
`renderer.drawFrame(...)`.

---

## 5. Shader — `shaders/skinning.slang` (NEW)

Compute pass, mirrors `ao.slang`'s BDA-push style. Uses `common.slang`'s
`Vertex` (48B scalar layout). New matching `SkinVertex` struct (24B). Fresh
128-byte push (independent of `GBufferPush`).

```hlsl
#include "common.slang"

struct SkinVertex {
    uint16_t j0, j1, j2, j3;   // slang: use uint packed; see note
    float w0, w1, w2, w3;
};

struct SkinningPush {
    Vertex*     bindPose;      // 8
    SkinVertex* skin;          // 16
    float4x4*   palette;       // 24
    Vertex*     dst;           // 32
    uint        vertexCount;   // 36
    uint pad0, pad1, pad2;     // 48
};
[[vk::push_constant]] ConstantBuffer<SkinningPush> gPush;

[shader("compute")][numthreads(64,1,1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= gPush.vertexCount) return;
    Vertex v = gPush.bindPose[i];
    SkinVertex s = gPush.skin[i];
    float4x4 m = s.w0 * gPush.palette[s.j0] + s.w1 * gPush.palette[s.j1]
               + s.w2 * gPush.palette[s.j2] + s.w3 * gPush.palette[s.j3];
    float3 p  = mul(m, float4(v.position(), 1.0)).xyz;
    float3 n  = normalize(mul((float3x3)m, v.normal()));
    float3 tg = normalize(mul((float3x3)m, v.tangent().xyz));
    Vertex o = v;
    o.px=p.x; o.py=p.y; o.pz=p.z;
    o.nx=n.x; o.ny=n.y; o.nz=n.z;
    o.tx=tg.x; o.ty=tg.y; o.tz=tg.z; // tw (handedness) preserved from v
    gPush.dst[i] = o;
}
```
Notes:
- The 16-bit joint indices: read `SkinVertex` as scalar `uint16_t` fields. If
  slang's 16-bit storage via BDA is awkward, the fallback is to pack joints as
  two `uint`s (`j01`, `j23`) in `SkinVertex` on both C++ and shader sides and
  unpack with `>>16`/`&0xffff` — still 24 bytes total (2×uint + 4×float would be
  24B). **Decision: use the two-uint packing** to avoid slang 8/16-bit BDA
  capability requirements; `SkinVertex` in C++ becomes
  `{uint32_t joints01, joints23; float weights[4];}` — still 24 bytes,
  `static_assert(sizeof==24)` holds, and the import code packs
  `joints01 = j.x | (j.y<<16)`. (Adjust §1 accordingly; the 24B invariant is the
  contract, the internal packing is an implementation choice.)
- The palette already folds `inverse(meshWorld)` in, so the compute output is in
  **mesh-local** space; the G-buffer VS then applies `gPush.model` (=meshWorld)
  exactly as for static meshes — no double transform, and RT instance transforms
  (also meshWorld) stay consistent. This is the key correctness invariant.
- Pipelines are compiled by the existing `ShaderCache`/hot-reload path; no CMake
  change needed for shaders (they're globbed from `CANDELA_SHADER_DIR`).

`shaders/common.slang` (MODIFY, additive): optionally add the shared
`SkinVertex` struct there instead of duplicating in skinning.slang — but to keep
the shared-file edit minimal and since only skinning.slang needs it, define it
locally in `skinning.slang`. **Decision: no change to common.slang.**

---

## 6. Sandbox — `sandbox/main.cpp` (MODIFY) + test target

### `--animinfo <path>` (GPU-FREE, returns before JobSystem::init)

Add flag parsing alongside the others. Handle it at the very top of `main`,
**before** `candela::JobSystem::init()`, so no GPU/JobSystem is touched and no
shutdown is needed:

```cpp
if (!animInfoPath.empty()) {
    // GPU-free: parse the glTF for skins/animations and print a summary.
    // importGltfModel needs a Context; instead call a lightweight parse.
}
```
Problem: `importGltfModel` requires `Context&`/`Bindless&` (GPU). For a truly
GPU-free dump we add a small free function
`printAnimationInfo(const std::filesystem::path&)` in a NEW header
`engine/src/candela/assets/AnimationInfo.h` (+ `.cpp`) that runs only the
fastgltf parse (no GPU) and prints clip names/durations/channel counts and skin
joint counts. It reuses the same fastgltf calls as the importer's skin/animation
sections, factored so both share the parsing intent. Called from `main` and
`return 0;` before `JobSystem::init()`. (Add the `.cpp` to
`engine/CMakeLists.txt` sources list.)

Output format (example):
```
animinfo: Fox.gltf — 2 skins, 3 animations
  skin[0]: 24 joints, skeletonRoot=1
  clip[0] "Survey"  dur 2.083s  18 channels (T:6 R:12 S:0)
  clip[1] "Walk"    dur 0.708s  ...
```

### Skinned model in the sandbox loop

- Add `world.updateAnimations(assets, dt);` immediately before the existing
  `world.updateTransforms();` in the main loop.
- Model-viewer mode (`--model`) already instantiates any model; a skinned glTF
  (e.g. `Fox.gltf`, `CesiumMan.gltf`) now animates automatically because
  `instantiateModel` attaches the `Animator`. No new flag needed to *view* one;
  `--animinfo` is the GPU-free inspector.

### Pure-logic sampling test — `sandbox/AnimationSamplingTest.cpp` (NEW)

Mirrors `lightkeeper-leveltest`: no engine, no GPU, links only `glm` and the
header-only `AnimationSampling.h`. New CMake target in `sandbox/CMakeLists.txt`:

```cmake
add_executable(anim-sampling-test AnimationSamplingTest.cpp)
target_link_libraries(anim-sampling-test PRIVATE glm::glm)
target_include_directories(anim-sampling-test PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src)
candela_apply_warnings(anim-sampling-test)
```
The test builds an in-memory `AnimationClip` by hand (no glTF parse — keeps it
dependency-free) and asserts:
- **Exact keyframe hit**: `sampleChannel` at `t == times[k]` returns
  `values[k]` (translation/scale vec3, rotation quat) within 1e-6.
- **Linear interpolation midpoint**: translation channel with keys
  (0→(0,0,0), 1→(2,0,0)) at t=0.5 gives (1,0,0).
- **Rotation slerp**: keys (0→identity, 1→90° about Y) at t=0.5 gives 45°
  about Y (compare quats up to sign; assert `abs(dot) ≈ cos(22.5°)`).
- **Step interpolation** holds the left key across the interval.
- **Clamp before first / after last** keyframe returns the endpoint values.
- **Loop wrap** (`fmod`) — sampling at `t = duration + ε` under loop equals
  sampling at `ε`.
Exit 0 on all-pass, non-zero otherwise; prints a one-line summary like the
level test.

`engine/src/candela/assets/AnimationSampling.h` (NEW, header-only) contains:
```cpp
namespace candela {
struct AnimationClip; struct AnimationChannel; // fwd via include of ModelAsset.h
inline glm::vec3 sampleVec3(const AnimationChannel&, float t);
inline glm::quat sampleQuat(const AnimationChannel&, float t);
}
```
Binary-search the sorted `times`, then Step/Linear/Slerp per `interp`.
CubicSpline: v1 falls back to Linear on the endpoint tangents' value component
(documented; correct cubic is a follow-up). This header is included by
`ModelAsset.cpp`, `World.cpp`, and the test — pure math, no Vulkan, compiles on
MSVC + GCC.

---

## 7. Scene serialization — `engine/src/candela/scene/SceneSerializer.cpp` (MODIFY, minimal)

To keep the existing `--roundtrip-check` passing and not lose skinned entities:
serialize `SkinnedMeshRenderer` and `Animator` analogously to `MeshRenderer`
(append two `if (const auto* ...)` blocks in `worldToJson` and two
`if (e.contains(...))` blocks in `worldFromJson`). `Skeleton` is **not**
serialized — it's rebuilt by `instantiateModel`; on load, a `SkinnedMeshRenderer`
entity without a rebuilt skeleton simply won't skin until re-instantiated. For
v1 the slice's scene is built in code (model-viewer / `instantiateModel`), so
full skeleton persistence is a documented follow-up; the additive JSON keeps
round-trip stable (unknown-to-old-code fields are ignored, and we emit them in
stable order). Keep edits localized and additive for clean merging.

> Decision: serialize the light-weight driver components (SkinnedMeshRenderer,
> Animator) for round-trip stability; rebuild Skeleton from the asset at
> instantiate. Do not block the slice on full skeleton persistence.

---

## 8. CMake / dependencies

- **No new FetchContent deps.** glTF skins/animations are parsed with the
  already-fetched fastgltf; math is glm. `cmake/Dependencies.cmake` and root
  `CMakeLists.txt` are untouched.
- `engine/CMakeLists.txt` (MODIFY): add `src/candela/assets/AnimationInfo.cpp`
  to the `candela` library sources. (`AnimationSampling.h` and
  `AnimationInfo.h` are headers — no source-list entry beyond the one `.cpp`.)
  The `ModelAsset.cpp` `/wd4100` + GCC `-Wno-unused-parameter;-Wno-redundant-move`
  suppression already covers the added fastgltf iteration code.
- `sandbox/CMakeLists.txt` (MODIFY): add the `anim-sampling-test` target (§6).
- `shaders/skinning.slang` needs no CMake entry (runtime-globbed).

---

## 9. Files summary

**NEW**
- `shaders/skinning.slang` — compute pre-skinning pass.
- `engine/src/candela/assets/AnimationSampling.h` — header-only TRS sampling
  (used by importer, World, and the test).
- `engine/src/candela/assets/AnimationInfo.h` / `.cpp` — GPU-free glTF
  animation/skin summary for `--animinfo`.
- `sandbox/AnimationSamplingTest.cpp` — pure-logic sampling test (`anim-sampling-test`).

**MODIFY**
- `engine/src/candela/assets/ModelAsset.h` — SkinVertex, Skin, AnimationChannel,
  AnimationClip; GpuPrimitive.skinned + skinVertexBuffer; GpuMesh.skinned;
  NodeTemplate.skinIndex; ModelAsset.skins/animations.
- `engine/src/candela/assets/ModelAsset.cpp` — parse JOINTS_0/WEIGHTS_0, skins,
  animations, skinIndex; ALLOW_UPDATE BLAS (skip compaction) for skinned meshes;
  destroy skinVertexBuffer.
- `engine/src/candela/scene/Components.h` — SkinnedMeshRenderer, Skeleton
  (with jointNodeIndex), Animator.
- `engine/src/candela/scene/World.h` — updateAnimations declaration.
- `engine/src/candela/scene/World.cpp` — instantiateModel skin/skeleton/animator
  wiring; updateAnimations + applySampledPose.
- `engine/src/candela/renderer/Renderer.h` — FrameData skinning buffers,
  m_skinningPipeline, SkinnedInstance + cursors, new method decls.
- `engine/src/candela/renderer/Renderer.cpp` — createSkinningResources,
  skinning pipeline in createPipelines/dtor, fillSkinningData, skinning dispatch
  + barrier + BLAS refit in recordCommands, skinned draws in G-buffer/shadow
  loops, skinned RT instances, drawFrame ordering.
- `engine/src/candela/scene/SceneSerializer.cpp` — serialize SkinnedMeshRenderer
  + Animator (additive).
- `engine/CMakeLists.txt` — add AnimationInfo.cpp.
- `sandbox/main.cpp` — `--animinfo` (pre-init return), updateAnimations call.
- `sandbox/CMakeLists.txt` — anim-sampling-test target.

## 10. Correctness invariants (must hold)

1. `sizeof(Vertex)==48`, `sizeof(SkinVertex)==24`, `sizeof(GBufferPush)==128`,
   `offsetof(GBufferPush,baseColorFactor)==112` — all unchanged/added asserts.
2. Palette folds `inverse(meshWorld)` so skinned output is mesh-local; VS/RT
   apply `meshWorld` once → geometry matches between raster and RT.
3. Skinning dispatch + its buffer barrier run on the RAW cmd before
   `graph.execute()` (graph tracks image barriers only).
4. Skinned BLAS built `ALLOW_UPDATE`, never compacted; refit mode=UPDATE with
   src==dst; refit + its AS barrier precede `buildTLAS`.
5. `updateAnimations` writes joint `LocalTransform` strictly before
   `updateTransforms`.
6. `--animinfo` returns before `JobSystem::init()` (no shutdown-hang risk).
7. Skinned entities carry `SkinnedMeshRenderer` (not `MeshRenderer`), so the
   existing static draw/RT loops skip them and the skinned loops own them.
