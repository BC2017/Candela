# Candela Physics Pillar — Implementation Design

Vertical slice: integrate **Jolt Physics** as an engine subsystem with a fixed-timestep
`PhysicsSystem`, two new ECS components, a capsule character controller, scene
serialization, and a GPU-free headless proof (`--physicstest`). Correctness and clean,
mergeable integration over breadth.

All paths below are relative to `C:/Claude Code Workspace/Candela`. Every staged file is
written under `C:/Users/corek/.claude/jobs/56c6b107/tmp/staging/physics/` mirroring the repo
layout, and listed in `MANIFEST.json`.

---

## 1. Files at a glance

### Create
| Staged path | Purpose |
|---|---|
| `engine/src/candela/physics/PhysicsSystem.h` | Public API — PIMPL, **no Jolt include** |
| `engine/src/candela/physics/PhysicsSystem.cpp` | The only TU that includes Jolt; layers, filters, body/character mgmt, fixed step, self-test |

### Modify (Read-first; full modified file staged; `action:"modify"` in MANIFEST)
| Repo path | Change (additive, localized) |
|---|---|
| `cmake/Dependencies.cmake` | Jolt `FetchContent_Declare` + option block + add to `MakeAvailable` |
| `engine/CMakeLists.txt` | add `physics/PhysicsSystem.cpp`, link `Jolt` PRIVATE, per-source warning relax |
| `engine/src/candela/scene/Components.h` | append `RigidBody` + `CharacterController` structs |
| `engine/src/candela/scene/SceneSerializer.cpp` | (de)serialize the two components |
| `sandbox/main.cpp` | `--physicstest` early dispatch; wire `PhysicsSystem::update` into the frame loop |

No changes to `World.{h,cpp}`, the Vertex layout, `gbuffer.slang`, the RenderGraph, or the
games' bespoke grid collision. Physics drives `LocalTransform`; `World::updateTransforms()` is
untouched and simply runs after the physics step.

---

## 2. CMake — Jolt via FetchContent (defines in ONE place)

Jolt's buildable `CMakeLists.txt` lives in its **`Build/`** subdirectory, so the declare needs
`SOURCE_SUBDIR Build`. Pin a released tag for reproducibility.

Append to `cmake/Dependencies.cmake` **before** the existing `FetchContent_MakeAvailable(...)`
call, then add `jolt` to that call's argument list:

```cmake
# Jolt Physics. Its CMakeLists is under Build/, hence SOURCE_SUBDIR. All
# ABI-affecting JPH_* defines are carried by the exported `Jolt` target's
# INTERFACE_COMPILE_DEFINITIONS, so every TU that includes Jolt headers gets a
# byte-identical layout automatically — this is the single source of truth.
set(TARGET_UNIT_TESTS       OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD      OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES          OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER           OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS     OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS      OFF CACHE BOOL "" FORCE)  # respect our toolchain flags
# Force the SSE2 x86-64 baseline: both MSVC and GCC compile SSE2 intrinsics with
# no special -march flag, so consumers need no arch flags to match Jolt's ABI.
# (Any of these ON would emit a JPH_USE_* interface define whose intrinsics GCC
# refuses to compile without a matching -msseX / -mavx flag on OUR targets.)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_AVX    OFF CACHE BOOL "" FORCE)
set(USE_AVX2   OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT  OFF CACHE BOOL "" FORCE)
set(USE_TZCNT  OFF CACHE BOOL "" FORCE)
set(USE_F16C   OFF CACHE BOOL "" FORCE)
set(USE_FMADD  OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC      OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION                  OFF CACHE BOOL "" FORCE)  # RVec3 == Vec3 (float)

FetchContent_Declare(jolt
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.3.0
  GIT_SHALLOW ON
  SOURCE_SUBDIR Build)
```

Then extend the existing aggregate call:

```cmake
FetchContent_MakeAvailable(volk vkbootstrap vma glfw glm spdlog tracy fastgltf stb
                           entt nlohmann_json imgui imguizmo jolt)
```

**Why this is the "one place":** Jolt's CMake attaches every layout-affecting define
(`JPH_OBJECT_LAYER_BITS=16` default, precision, profiling, any enabled SIMD) to the `Jolt`
target as *interface* usage requirements. Because `candela` links `Jolt`, `PhysicsSystem.cpp`
inherits exactly those defines — we never hand-author a `JPH_*` define anywhere, eliminating
the classic Jolt "header/lib compiled with different flags → silent struct-layout corruption"
bug. Turning the SSE4/AVX options OFF is the deliberate portability choice that lets the SAME
`candela` compile flags satisfy Jolt on both MSVC and GCC.

Watch-item (documented, not blocking): if a future dep sets `CMAKE_MSVC_RUNTIME_LIBRARY`,
confirm Jolt matches; today all deps use the default dynamic runtime, so they agree.

### `engine/CMakeLists.txt`
- Add `src/candela/physics/PhysicsSystem.cpp` to the `candela` source list.
- Add `Jolt` to `target_link_libraries(candela ...)` — **PRIVATE**, appended after the
  existing `PUBLIC` block:
  ```cmake
  target_link_libraries(candela PRIVATE Jolt)
  ```
  `candela` is a STATIC lib, so a PRIVATE link dep is still propagated to the final executables
  as a link-only requirement (sandbox/editor/games link Jolt transitively with **no** Jolt
  include dirs or defines leaking into them — reinforcing the one-place rule). The header is
  PIMPL, so nothing outside `PhysicsSystem.cpp` needs Jolt headers.
- Warnings: `PhysicsSystem.cpp` is our code under `/W4 /WX` (MSVC) and `-Wall -Wextra -Werror`
  (GCC), but it includes heavy third-party headers. Primary approach — wrap **only** the Jolt
  `#include`s in the `.cpp` with Jolt's own `JPH_SUPPRESS_WARNINGS_STD_BEGIN/END` is not
  sufficient for our flags, so instead bracket the includes with compiler pragmas
  (`__pragma(warning(push,0))` / `#pragma GCC diagnostic push` + `-Wall` off) to keep
  warnings-as-errors on our logic. Fallback (mirrors the existing `ModelAsset.cpp` /
  `VmaUsage.cpp` precedent) if a stray header warning still escapes the bracket:
  ```cmake
  if(MSVC)
    set_source_files_properties(src/candela/physics/PhysicsSystem.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4324")   # structure padding from alignment
  else()
    set_source_files_properties(src/candela/physics/PhysicsSystem.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
  endif()
  ```
  The staged file will use the pragma-bracket around includes and add the `set_source_files_properties`
  guard as belt-and-braces.

---

## 3. ECS components — `scene/Components.h` (append, additive)

No Jolt include. Runtime handles are plain integers so Components.h stays Jolt-free and the ABI
defines remain confined to `PhysicsSystem.cpp`.

```cpp
// --- Physics (Jolt-backed; see physics/PhysicsSystem.h). Runtime handles are
// opaque integers so this header never includes Jolt. ------------------------

// Sentinel matching JPH::BodyID::cInvalidBodyID without pulling in Jolt.
inline constexpr uint32_t kInvalidBodyId = 0xffffffffu;

struct RigidBody {
    enum class MotionType : uint8_t { Static, Kinematic, Dynamic };
    enum class ShapeType  : uint8_t { Box, Sphere, Capsule };

    MotionType motionType = MotionType::Dynamic;
    ShapeType  shape      = ShapeType::Box;

    // Box   -> halfExtents (x,y,z).
    // Sphere-> radius = halfExtents.x.
    // Capsule-> radius = halfExtents.x, half cylinder height = halfExtents.y.
    glm::vec3 halfExtents{0.5f};

    float mass        = 1.0f;   // ignored for Static/Kinematic
    float friction    = 0.5f;
    float restitution = 0.0f;

    // Runtime — assigned by PhysicsSystem on first Update; not authored.
    uint32_t bodyId = kInvalidBodyId;
};

struct CharacterController {
    float radius     = 0.3f;   // capsule radius
    float halfHeight = 0.6f;   // half height of the cylindrical section
    float mass       = 75.0f;
    float friction   = 0.5f;

    // Runtime — the backing body's id; the JPH::Character lives in PhysicsSystem.
    uint32_t bodyId = kInvalidBodyId;
};
```

---

## 4. `physics/PhysicsSystem.h` — public API (PIMPL, Jolt-free)

```cpp
#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace candela {

class World;

// Fixed-timestep rigid-body + character physics backed by Jolt. One instance
// owns a JPH::PhysicsSystem, temp allocator, and job pool. All Jolt types and
// their ABI-defining macros are confined to PhysicsSystem.cpp (PIMPL).
class PhysicsSystem {
public:
    static constexpr float kFixedDelta = 1.0f / 60.0f;  // 60 Hz simulation

    PhysicsSystem();
    ~PhysicsSystem();
    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    // One frame of physics:
    //  1. lazily create a Jolt body for every RigidBody / CharacterController
    //     whose bodyId is still kInvalidBodyId, seeded from its LocalTransform;
    //  2. accumulate dt and run zero+ fixed steps (accumulator clamped to avoid
    //     the spiral of death);
    //  3. write DYNAMIC-body and character transforms back into LocalTransform.
    // MUST be called BEFORE World::updateTransforms() so WorldTransform picks up
    // the new local pose the same frame.
    void update(World& world, float dt);

    // Read-back helper (gameplay / tests). Returns {0,0,0} if the entity has no
    // live body. Takes World to resolve the component's runtime bodyId.
    glm::vec3 linearVelocity(World& world, entt::entity entity) const; // entt fwd via World.h

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GPU-free self-test behind sandbox `--physicstest`. Drops a dynamic box onto a
// static ground plane, steps ~2 s of fixed timesteps, asserts it comes to rest
// ON the plane with ~zero residual motion. Returns 0 on pass, 1 on failure.
int runPhysicsSelfTest();

} // namespace candela
```

> `entt::entity` in `linearVelocity` requires `<entt/entt.hpp>`; the header includes
> `candela/scene/World.h` (which transitively provides entt) rather than glm-only, OR the
> method is dropped and the self-test infers rest from positional stability (see §7). Decision:
> **drop `linearVelocity` from the public header** and keep the self-test dependency-light —
> rest is proven by position deltas, so the header needs only `<glm/glm.hpp>` + `<memory>` and
> a `class World;` forward decl. Cleaner and avoids leaking entt into a glm-only header.

Final header therefore omits `linearVelocity`; `update(World&, float)` + `runPhysicsSelfTest()`
are the entire surface.

---

## 5. `physics/PhysicsSystem.cpp` — internals

### Includes (Jolt.h first, bracketed by warning pragmas)
```cpp
#include "candela/physics/PhysicsSystem.h"
#include "candela/scene/World.h"       // World, registry, LocalTransform, RigidBody...
#include "candela/core/Log.h"

// -- third-party, warnings suppressed around the block --
#include <Jolt/Jolt.h>                 // MUST precede all other Jolt headers
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Character/Character.h>

#include <cstdarg>
#include <thread>
#include <unordered_map>
```
`JPH::PhysicsSystem` name-clashes with `candela::PhysicsSystem`; inside the `.cpp` always
qualify Jolt as `JPH::` and our class members via `impl_`, so no ambiguity. Use
`using namespace JPH;` sparingly or not at all to keep the clash obvious.

### Layers (2, as specified)
```cpp
namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer NUM        = 2;
}
namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING{0};
    static constexpr JPH::BroadPhaseLayer MOVING{1};
    static constexpr JPH::uint NUM = 2;
}
```

### Three filter/interface impls (standard Jolt HelloWorld shape)
- `class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface`
  - `GetNumBroadPhaseLayers() -> 2`
  - `GetBroadPhaseLayer(ObjectLayer) -> mapping[layer]` where
    `mapping = { NON_MOVING, MOVING }`
  - (debug) `GetBroadPhaseLayerName`
- `class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter`
  - `ShouldCollide(ObjectLayer, BroadPhaseLayer)`: NON_MOVING collides only with MOVING;
    MOVING collides with all.
- `class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter`
  - `ShouldCollide(ObjectLayer, ObjectLayer)`: NON_MOVING×NON_MOVING = false; else true.

### glm <-> Jolt conversion helpers (file-static, `inline`)
```cpp
inline JPH::RVec3 toJolt(const glm::vec3& v){ return JPH::RVec3(v.x, v.y, v.z); }
inline glm::vec3  toGlm (JPH::RVec3Arg v){ return { (float)v.GetX(),(float)v.GetY(),(float)v.GetZ() }; }
inline JPH::Quat  toJolt(const glm::quat& q){ return JPH::Quat(q.x, q.y, q.z, q.w); }   // x,y,z,w
inline glm::quat  toGlm (JPH::QuatArg q){ return glm::quat(q.GetW(),q.GetX(),q.GetY(),q.GetZ()); } // w,x,y,z
```

### `struct PhysicsSystem::Impl`
```cpp
struct PhysicsSystem::Impl {
    JPH::TempAllocatorImpl        tempAllocator{ 16 * 1024 * 1024 };  // 16 MB scratch
    JPH::JobSystemThreadPool      jobSystem;    // constructed in ctor body (needs thread count)
    BPLayerInterfaceImpl          bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBp;
    ObjectLayerPairFilterImpl     objVsObj;
    JPH::PhysicsSystem            physics;
    float                         accumulator = 0.0f;
    std::unordered_map<entt::entity, JPH::Ref<JPH::Character>> characters;
};
```

### Global Jolt bring-up (refcounted so multiple PhysicsSystems are safe)
File-static `int g_joltRefs = 0;`. In the ctor, if `g_joltRefs++ == 0`:
```cpp
JPH::RegisterDefaultAllocator();
JPH::Trace = &traceImpl;                    // forwards to CD_INFO
JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &assertFailedImpl;)
JPH::Factory::sInstance = new JPH::Factory();
JPH::RegisterTypes();
```
In the dtor, remove every body/character first, then if `--g_joltRefs == 0`:
```cpp
JPH::UnregisterTypes();
delete JPH::Factory::sInstance;
JPH::Factory::sInstance = nullptr;
```
`traceImpl(const char* fmt, ...)` uses `vsnprintf` into a stack buffer → `CD_INFO`.

### Constructor body
```cpp
PhysicsSystem::PhysicsSystem() : impl_(std::make_unique<Impl>()) {
    // (global bring-up as above)
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    impl_->jobSystem.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                          static_cast<int>(std::min(hw - 1u, 8u)));  // leave a core for the main thread
    impl_->physics.Init(/*maxBodies*/       2048,
                        /*numBodyMutexes*/  0,
                        /*maxBodyPairs*/    2048,
                        /*maxContactConstraints*/ 2048,
                        impl_->bpLayers, impl_->objVsBp, impl_->objVsObj);
    // Gravity left at Jolt's default (0,-9.81,0).
}
```
> `JobSystemThreadPool` has a default ctor + `Init`, so it can live by value in `Impl` and be
> initialized in the body. (Alternatively construct in-place with the parameterized ctor via a
> `std::optional`/`unique_ptr`; value + `Init` is simplest.)

### Shape construction
```cpp
static JPH::ShapeRefC makeShape(const RigidBody& rb) {
    switch (rb.shape) {
    case RigidBody::ShapeType::Box:
        return JPH::BoxShapeSettings(toJolt(rb.halfExtents)).Create().Get();
    case RigidBody::ShapeType::Sphere:
        return JPH::SphereShapeSettings(rb.halfExtents.x).Create().Get();
    case RigidBody::ShapeType::Capsule:
        return JPH::CapsuleShapeSettings(rb.halfExtents.y, rb.halfExtents.x).Create().Get();
    }
    return {};
}
```
(`BoxShapeSettings` half-extent must be ≥ the convex radius; defaults are fine for ≥0.05 m
boxes. `.Create()` returns a `Result`; `.Get()` yields the `ShapeRefC`.)

### `update(World& world, float dt)`
```
auto& reg = world.registry;
JPH::BodyInterface& bi = impl_->physics.GetBodyInterface();

// (1a) lazily create rigid bodies
for (entt::entity e : reg.view<RigidBody, LocalTransform>()) {
    RigidBody& rb = reg.get<RigidBody>(e);
    if (rb.bodyId != kInvalidBodyId) continue;
    const LocalTransform& lt = reg.get<LocalTransform>(e);
    const JPH::EMotionType mt =
        rb.motionType == RigidBody::MotionType::Static    ? JPH::EMotionType::Static
      : rb.motionType == RigidBody::MotionType::Kinematic ? JPH::EMotionType::Kinematic
                                                          : JPH::EMotionType::Dynamic;
    const JPH::ObjectLayer layer =
        (mt == JPH::EMotionType::Static) ? Layers::NON_MOVING : Layers::MOVING;
    JPH::BodyCreationSettings s(makeShape(rb), toJolt(lt.translation),
                               toJolt(lt.rotation), mt, layer);
    s.mFriction = rb.friction;
    s.mRestitution = rb.restitution;
    if (mt == JPH::EMotionType::Dynamic) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = rb.mass;
    }
    const JPH::BodyID id = bi.CreateAndAddBody(
        s, mt == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                          : JPH::EActivation::Activate);
    rb.bodyId = id.GetIndexAndSequenceNumber();
}

// (1b) lazily create character controllers
for (entt::entity e : reg.view<CharacterController, LocalTransform>()) {
    CharacterController& cc = reg.get<CharacterController>(e);
    if (cc.bodyId != kInvalidBodyId) continue;
    const LocalTransform& lt = reg.get<LocalTransform>(e);
    JPH::CharacterSettings cs;
    cs.mShape = JPH::CapsuleShapeSettings(cc.halfHeight, cc.radius).Create().Get();
    cs.mLayer = Layers::MOVING;
    cs.mMass  = cc.mass;
    cs.mFriction = cc.friction;
    cs.mUp = JPH::Vec3::sAxisY();
    // Supporting plane just under the capsule bottom so ground is detected.
    cs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -(cc.halfHeight + cc.radius));
    JPH::Ref<JPH::Character> ch = new JPH::Character(
        &cs, toJolt(lt.translation), toJolt(lt.rotation), /*userData*/ 0, &impl_->physics);
    ch->AddToPhysicsSystem(JPH::EActivation::Activate);
    cc.bodyId = ch->GetBodyID().GetIndexAndSequenceNumber();
    impl_->characters.emplace(e, std::move(ch));
}

// (2) fixed-timestep stepping with clamped accumulator
impl_->accumulator += dt;
const float maxAccum = kFixedDelta * 8.0f;   // cap catch-up at 8 steps
if (impl_->accumulator > maxAccum) impl_->accumulator = maxAccum;
while (impl_->accumulator >= kFixedDelta) {
    impl_->physics.Update(kFixedDelta, /*collisionSteps*/ 1,
                          &impl_->tempAllocator, &impl_->jobSystem);
    for (auto& [e, ch] : impl_->characters) ch->PostSimulation(0.05f);
    impl_->accumulator -= kFixedDelta;
}

// (3) write DYNAMIC + character poses back to LocalTransform
for (entt::entity e : reg.view<RigidBody, LocalTransform>()) {
    RigidBody& rb = reg.get<RigidBody>(e);
    if (rb.bodyId == kInvalidBodyId ||
        rb.motionType != RigidBody::MotionType::Dynamic) continue;
    const JPH::BodyID id(rb.bodyId);
    LocalTransform& lt = reg.get<LocalTransform>(e);
    lt.translation = toGlm(bi.GetPosition(id));
    lt.rotation    = toGlm(bi.GetRotation(id));
}
for (auto& [e, ch] : impl_->characters) {
    if (!reg.valid(e) || !reg.all_of<LocalTransform>(e)) continue;
    LocalTransform& lt = reg.get<LocalTransform>(e);
    lt.translation = toGlm(ch->GetPosition());
    lt.rotation    = toGlm(ch->GetRotation());
}
```

**Ordering / hierarchy scope.** Physics treats `LocalTransform` as the world pose of physics
entities — the vertical slice assumes physics-driven bodies are top-level (no `Parent`), so
`Local == World`. `PhysicsSystem::update()` is called immediately before
`World::updateTransforms()` each frame in `main.cpp`; the latter then recomputes every
`WorldTransform` from the freshly-written locals. Parented dynamic bodies (converting between a
parent's world space and the body's) are called out as a follow-up, not part of this slice.

**Handle round-trip.** `JPH::BodyID` packs an index + sequence number into a `uint32`; we store
`GetIndexAndSequenceNumber()` and rebuild with `JPH::BodyID(stored)`. `kInvalidBodyId`
(`0xffffffff`) equals `JPH::BodyID::cInvalidBodyID`.

Bodies are not destroyed per-frame; a `~PhysicsSystem()` sweep removes/destroys all bodies and
clears the character map before global teardown. (Per-entity destroy-on-component-removal is a
follow-up; the slice creates and simulates, which is what the test and sandbox exercise.)

---

## 6. Serialization — `scene/SceneSerializer.cpp` (additive)

In `worldToJson`, after the `pointLight` block, emit when present (enums as stable strings):
```cpp
if (const auto* rb = registry.try_get<RigidBody>(entity)) {
    e["rigidBody"] = {
        {"motion", rigidBodyMotionName(rb->motionType)},   // "static"/"kinematic"/"dynamic"
        {"shape",  rigidBodyShapeName(rb->shape)},          // "box"/"sphere"/"capsule"
        {"halfExtents", vec3ToJson(rb->halfExtents)},
        {"mass", rb->mass}, {"friction", rb->friction}, {"restitution", rb->restitution}};
}
if (const auto* cc = registry.try_get<CharacterController>(entity)) {
    e["characterController"] = {
        {"radius", cc->radius}, {"halfHeight", cc->halfHeight},
        {"mass", cc->mass}, {"friction", cc->friction}};
}
```
Runtime `bodyId` is **never** serialized (it stays `kInvalidBodyId` on load → recreated lazily).

In `worldFromJson`, after the `pointLight` parse:
```cpp
if (e.contains("rigidBody")) {
    const auto& j = e["rigidBody"];
    auto& rb = world.registry.emplace<RigidBody>(entity);
    rb.motionType  = rigidBodyMotionFromName(j["motion"].get<std::string>());
    rb.shape       = rigidBodyShapeFromName(j["shape"].get<std::string>());
    rb.halfExtents = vec3FromJson(j["halfExtents"]);
    rb.mass = j["mass"].get<float>();
    rb.friction = j["friction"].get<float>();
    rb.restitution = j["restitution"].get<float>();
}
if (e.contains("characterController")) {
    const auto& j = e["characterController"];
    auto& cc = world.registry.emplace<CharacterController>(entity);
    cc.radius = j["radius"].get<float>();
    cc.halfHeight = j["halfHeight"].get<float>();
    cc.mass = j["mass"].get<float>();
    cc.friction = j["friction"].get<float>();
}
```
Add four small file-static enum<->string helpers in the anonymous namespace. This preserves the
byte-identical round-trip guarantee (keys only appear when the component exists; save/load are
symmetric). Low compile risk — pure `nlohmann::json` + glm, both already used here. If anything
about the enum helpers risked the round-trip test, the fallback is to note serialization as a
follow-up; the plan keeps it in-scope because it is mechanically identical to the existing
`pointLight` handling.

---

## 7. Headless test — `--physicstest` (GPU-free, like leveltest)

**Where.** Implemented as `int candela::runPhysicsSelfTest()` in `PhysicsSystem.cpp` (keeps the
assertions next to the physics code and the `main.cpp` edit tiny). Dispatched from
`sandbox/main.cpp` at the very top of `main`, **before** `Window`/`Renderer`/`AssetRegistry`
and before `JobSystem::init()` — so it never touches Vulkan and needs no `JobSystem::shutdown()`
bookkeeping (Jolt uses its own thread pool):

```cpp
for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--physicstest") == 0) {
        candela::Log::init();
        return candela::runPhysicsSelfTest();   // 0 = pass, 1 = fail
    }
}
```
(Placed just after `Log::init()`/before the existing arg loop, or folded in as the first branch
— either way it short-circuits before any GPU object is constructed.)

**What it does** (mirrors the leveltest `CHECK`/exit-code style):
```
World world;                       // pure ECS, no assets, no GPU
PhysicsSystem physics;

// static ground: top surface at y = 0  (center y = -0.5, half-height 0.5)
auto ground = world.createEntity("ground");
world.registry.get<LocalTransform>(ground).translation = {0, -0.5f, 0};
auto& g = world.registry.emplace<RigidBody>(ground);
g.motionType = RigidBody::MotionType::Static;
g.shape = RigidBody::ShapeType::Box;
g.halfExtents = {50.0f, 0.5f, 50.0f};

// dynamic unit box dropped from y = 5
auto box = world.createEntity("box");
world.registry.get<LocalTransform>(box).translation = {0, 5.0f, 0};
auto& b = world.registry.emplace<RigidBody>(box);       // Dynamic Box, half 0.5, mass 1
b.halfExtents = {0.5f, 0.5f, 0.5f};

// step ~2.5 s at the fixed 60 Hz delta (extra margin over "~2 s")
float prevY = 5.0f, maxSettleDelta = 0.0f;
for (int i = 0; i < 150; ++i) {
    physics.update(world, PhysicsSystem::kFixedDelta);
    const float y = world.registry.get<LocalTransform>(box).translation.y;
    if (i >= 120) maxSettleDelta = std::max(maxSettleDelta, std::abs(y - prevY));
    prevY = y;
}
const float restY = world.registry.get<LocalTransform>(box).translation.y;
```

**Assertions** (`CHECK` macro, `g_failures`, exit 0/1 exactly like leveltest):
- `CHECK(std::abs(restY - 0.5f) < 0.05f)` — box center rests on the plane
  (ground top 0.0 + box half-height 0.5).
- `CHECK(restY > 0.45f)` — it did **not** tunnel through the ground.
- `CHECK(maxSettleDelta < 1e-3f)` — over the final 30 steps the box moved < 1 mm/step, i.e.
  linear velocity ≈ 0 (rest proven from positional stability, so no velocity accessor is needed
  and the header stays glm-only).

On pass: `printf("physicstest: box at rest y=%.4f (Δ=%.2e) — PASS\n", restY, maxSettleDelta); return 0;`
On fail: print failure count, `return 1`.

This is the pillar's headless proof: it exercises the real create-lazily → fixed-step →
sync-back path end to end with zero GPU dependency, runnable on any build machine (`candela-sandbox --physicstest`).

---

## 8. Sandbox frame-loop wiring — `sandbox/main.cpp` (additive)

Two minimal edits beyond the `--physicstest` early return:
1. Construct one `candela::PhysicsSystem physics;` alongside `World world;` in the runtime block
   (include `<candela/physics/PhysicsSystem.h>`).
2. In the frame loop, insert **before** the existing `world.updateTransforms();`:
   ```cpp
   physics.update(world, dt);   // writes LocalTransform for dynamic bodies
   world.updateTransforms();    // (unchanged) recomputes WorldTransform
   ```
   `dt` already exists in the loop. With no physics components authored in the default Sponza
   scene this is a no-op there (empty views), so existing behavior/benchmarks are unchanged; it
   activates as soon as a scene carries `RigidBody`/`CharacterController` entities.

No other `main.cpp` logic changes; the edit is localized and merge-friendly.

---

## 9. Correctness / portability checklist

- **ABI single-source:** zero hand-authored `JPH_*` defines; all inherited from the `Jolt`
  target. SSE4/AVX disabled → identical baseline on MSVC and GCC with our existing flags.
- **Warnings-as-errors:** Jolt built with its own flags (untouched by `candela_apply_warnings`);
  `PhysicsSystem.cpp` brackets Jolt includes in warning-off pragmas + a `set_source_files_properties`
  guard, keeping `/WX` on our logic.
- **No hang:** `--physicstest` returns before `JobSystem::init()`; Jolt owns its thread pool and
  tears it down in `~PhysicsSystem`.
- **Vertex layout / shaders / RenderGraph / games' grid collision:** untouched.
- **Shared-file edits** (`Dependencies.cmake`, `engine/CMakeLists.txt`, `Components.h`,
  `SceneSerializer.cpp`, `main.cpp`): all strictly append/insert, no reorganization → clean
  3-way merge with the other pillars.
- **Determinism of scene round-trip:** preserved (component keys emitted only when present;
  runtime `bodyId` never serialized).

## 10. Follow-ups (explicitly out of slice)
- Destroy Jolt bodies when a component/entity is removed (currently swept at shutdown).
- Parented dynamic bodies (world/local space conversion).
- Character input/locomotion driving (`SetLinearVelocity`) and a dedicated `--charactertest`.
- Kinematic bodies driven from `LocalTransform` (push transforms into Jolt for kinematics).
- A standalone `candela-physicstest` CMake target (in addition to the CLI flag) if the team
  wants it in the pure-logic test set next to `lightkeeper-leveltest`.
```
