#include "candela/physics/PhysicsSystem.h"

#include "candela/core/Log.h"
#include "candela/scene/Components.h"
#include "candela/scene/World.h"

// Jolt's headers are warning-heavy for our /W4 -Wall -Wextra -Werror flags, so
// the whole TU has warnings disabled in engine/CMakeLists.txt (mirroring the
// VmaUsage.cpp / StbUsage.cpp precedent). Jolt.h MUST be included before any
// other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

namespace candela {

namespace {

// --- Collision layers: two object layers and two broad-phase layers. ---------
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM = 2;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING{0};
constexpr JPH::BroadPhaseLayer MOVING{1};
constexpr unsigned NUM = 2;
} // namespace BroadPhaseLayers

// Maps each object layer to a broad-phase layer.
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        objectToBroadPhase_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        objectToBroadPhase_[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM;
    }

    JPH::BroadPhaseLayer
    GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return objectToBroadPhase_[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char*
    GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        if (inLayer == BroadPhaseLayers::NON_MOVING) {
            return "NON_MOVING";
        }
        if (inLayer == BroadPhaseLayers::MOVING) {
            return "MOVING";
        }
        return "INVALID";
    }
#endif

private:
    JPH::BroadPhaseLayer objectToBroadPhase_[Layers::NUM];
};

// Which object layers collide with which broad-phase layers.
class ObjectVsBroadPhaseLayerFilterImpl final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1,
                       JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
        case Layers::NON_MOVING:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

// Which object layers collide with which object layers.
class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inObject1,
                       JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
        case Layers::NON_MOVING:
            return inObject2 == Layers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

// --- glm <-> Jolt conversions. DOUBLE_PRECISION is off so RVec3 == Vec3. -----
inline JPH::Vec3 toJoltVec3(const glm::vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}
inline JPH::RVec3 toJoltPos(const glm::vec3& v) {
    return JPH::RVec3(v.x, v.y, v.z);
}
inline JPH::Quat toJoltQuat(const glm::quat& q) {
    return JPH::Quat(q.x, q.y, q.z, q.w); // (x, y, z, w)
}
inline glm::vec3 toGlm(JPH::RVec3Arg v) {
    return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
            static_cast<float>(v.GetZ())};
}
inline glm::quat toGlm(JPH::QuatArg q) {
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); // (w, x, y, z)
}

JPH::ShapeRefC makeShape(const RigidBody& rb) {
    switch (rb.shape) {
    case RigidBody::ShapeType::Sphere:
        return JPH::SphereShapeSettings(rb.halfExtents.x).Create().Get();
    case RigidBody::ShapeType::Capsule:
        // Capsule: halfExtents.y = half cylinder height, halfExtents.x = radius.
        return JPH::CapsuleShapeSettings(rb.halfExtents.y, rb.halfExtents.x)
            .Create()
            .Get();
    case RigidBody::ShapeType::Box:
    default:
        return JPH::BoxShapeSettings(toJoltVec3(rb.halfExtents)).Create().Get();
    }
}

// --- Global Jolt bring-up, refcounted so multiple systems are safe. ----------
int g_joltRefs = 0;

void traceImpl(const char* fmt, ...) {
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);
    CD_INFO("[Jolt] {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertFailedImpl(const char* expression, const char* message,
                      const char* file, JPH::uint line) {
    CD_ERROR("[Jolt] assert failed: {}:{}: ({}) {}", file, line, expression,
             message != nullptr ? message : "");
    return true; // breakpoint
}
#endif

void joltAcquire() {
    if (g_joltRefs++ != 0) {
        return;
    }
    JPH::RegisterDefaultAllocator();
    JPH::Trace = &traceImpl;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &assertFailedImpl;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void joltRelease() {
    if (--g_joltRefs != 0) {
        return;
    }
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

} // namespace

struct PhysicsSystem::Impl {
    JPH::TempAllocatorImpl tempAllocator{16 * 1024 * 1024}; // 16 MB scratch
    JPH::JobSystemThreadPool jobSystem;                     // Init'd in ctor
    BPLayerInterfaceImpl bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBp;
    ObjectLayerPairFilterImpl objVsObj;
    JPH::PhysicsSystem physics;
    float accumulator = 0.0f;
    std::vector<JPH::BodyID> ownedBodies;
    std::unordered_map<entt::entity, JPH::Ref<JPH::Character>> characters;
};

PhysicsSystem::PhysicsSystem() {
    joltAcquire();
    impl_ = std::make_unique<Impl>();

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int numThreads = static_cast<int>(std::min(hw - 1u, 8u));
    impl_->jobSystem.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                          numThreads);

    impl_->physics.Init(/*maxBodies*/ 2048, /*numBodyMutexes*/ 0,
                        /*maxBodyPairs*/ 2048, /*maxContactConstraints*/ 2048,
                        impl_->bpLayers, impl_->objVsBp, impl_->objVsObj);
    // Gravity left at Jolt's default (0, -9.81, 0).
}

PhysicsSystem::~PhysicsSystem() {
    // Tear down bodies/characters while the physics system is still alive.
    if (impl_) {
        for (auto& [entity, character] : impl_->characters) {
            character->RemoveFromPhysicsSystem();
        }
        impl_->characters.clear();

        JPH::BodyInterface& bi = impl_->physics.GetBodyInterface();
        for (const JPH::BodyID id : impl_->ownedBodies) {
            if (bi.IsAdded(id)) {
                bi.RemoveBody(id);
            }
            bi.DestroyBody(id);
        }
        impl_->ownedBodies.clear();
    }
    impl_.reset(); // destroy PhysicsSystem/jobs/allocator before global teardown
    joltRelease();
}

void PhysicsSystem::update(World& world, float dt) {
    entt::registry& reg = world.registry;
    JPH::BodyInterface& bi = impl_->physics.GetBodyInterface();

    // (1a) Lazily create rigid bodies for new RigidBody components.
    for (const entt::entity entity : reg.view<RigidBody, LocalTransform>()) {
        RigidBody& rb = reg.get<RigidBody>(entity);
        if (rb.bodyId != kInvalidBodyId) {
            continue;
        }
        const LocalTransform& lt = reg.get<LocalTransform>(entity);

        JPH::EMotionType motion = JPH::EMotionType::Dynamic;
        if (rb.motionType == RigidBody::MotionType::Static) {
            motion = JPH::EMotionType::Static;
        } else if (rb.motionType == RigidBody::MotionType::Kinematic) {
            motion = JPH::EMotionType::Kinematic;
        }
        const JPH::ObjectLayer layer = (motion == JPH::EMotionType::Static)
                                           ? Layers::NON_MOVING
                                           : Layers::MOVING;

        JPH::BodyCreationSettings settings(makeShape(rb),
                                           toJoltPos(lt.translation),
                                           toJoltQuat(lt.rotation), motion,
                                           layer);
        settings.mFriction = rb.friction;
        settings.mRestitution = rb.restitution;
        if (motion == JPH::EMotionType::Dynamic) {
            settings.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rb.mass;
        }

        const JPH::BodyID id = bi.CreateAndAddBody(
            settings, motion == JPH::EMotionType::Static
                          ? JPH::EActivation::DontActivate
                          : JPH::EActivation::Activate);
        rb.bodyId = id.GetIndexAndSequenceNumber();
        impl_->ownedBodies.push_back(id);
    }

    // (1b) Lazily create capsule character controllers.
    for (const entt::entity entity :
         reg.view<CharacterController, LocalTransform>()) {
        CharacterController& cc = reg.get<CharacterController>(entity);
        if (cc.bodyId != kInvalidBodyId) {
            continue;
        }
        const LocalTransform& lt = reg.get<LocalTransform>(entity);

        JPH::CharacterSettings cs;
        cs.mShape =
            JPH::CapsuleShapeSettings(cc.halfHeight, cc.radius).Create().Get();
        cs.mLayer = Layers::MOVING;
        cs.mMass = cc.mass;
        cs.mFriction = cc.friction;
        cs.mUp = JPH::Vec3::sAxisY();
        cs.mSupportingVolume =
            JPH::Plane(JPH::Vec3::sAxisY(), -(cc.halfHeight + cc.radius));

        JPH::Ref<JPH::Character> character = new JPH::Character(
            &cs, toJoltPos(lt.translation), toJoltQuat(lt.rotation),
            /*userData*/ 0, &impl_->physics);
        character->AddToPhysicsSystem(JPH::EActivation::Activate);
        cc.bodyId = character->GetBodyID().GetIndexAndSequenceNumber();
        impl_->characters.emplace(entity, std::move(character));
    }

    // (2) Fixed-timestep stepping with a clamped accumulator.
    impl_->accumulator += dt;
    const float maxAccum = kFixedDelta * 8.0f; // cap catch-up at 8 steps
    if (impl_->accumulator > maxAccum) {
        impl_->accumulator = maxAccum;
    }
    while (impl_->accumulator >= kFixedDelta) {
        // Drive each character from its desired horizontal velocity; gravity
        // owns the vertical component (ground contact arrests it in-step).
        for (auto& [entity, character] : impl_->characters) {
            if (!reg.valid(entity) || !reg.all_of<CharacterController>(entity)) {
                continue;
            }
            const glm::vec3 dv =
                reg.get<CharacterController>(entity).desiredVelocity;
            const JPH::Vec3 cur = character->GetLinearVelocity();
            character->SetLinearVelocity(JPH::Vec3(dv.x, cur.GetY(), dv.z));
        }
        impl_->physics.Update(kFixedDelta, /*collisionSteps*/ 1,
                              &impl_->tempAllocator, &impl_->jobSystem);
        for (auto& [entity, character] : impl_->characters) {
            character->PostSimulation(0.05f);
        }
        impl_->accumulator -= kFixedDelta;
    }

    // (3) Write DYNAMIC-body and character poses back into LocalTransform.
    for (const entt::entity entity : reg.view<RigidBody, LocalTransform>()) {
        RigidBody& rb = reg.get<RigidBody>(entity);
        if (rb.bodyId == kInvalidBodyId ||
            rb.motionType != RigidBody::MotionType::Dynamic) {
            continue;
        }
        const JPH::BodyID id(rb.bodyId);
        LocalTransform& lt = reg.get<LocalTransform>(entity);
        lt.translation = toGlm(bi.GetPosition(id));
        lt.rotation = toGlm(bi.GetRotation(id));
    }
    for (auto& [entity, character] : impl_->characters) {
        if (!reg.valid(entity) || !reg.all_of<LocalTransform>(entity)) {
            continue;
        }
        LocalTransform& lt = reg.get<LocalTransform>(entity);
        lt.translation = toGlm(character->GetPosition());
        lt.rotation = toGlm(character->GetRotation());
        if (reg.all_of<CharacterController>(entity)) {
            reg.get<CharacterController>(entity).onGround =
                character->GetGroundState() ==
                JPH::CharacterBase::EGroundState::OnGround;
        }
    }
}

// --- GPU-free self-test: --physicstest --------------------------------------
namespace {

int g_failures = 0;

#define PHYS_CHECK(cond)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

} // namespace

int runPhysicsSelfTest() {
    g_failures = 0;

    World world; // pure ECS — no assets, no GPU
    PhysicsSystem physics;

    // Static ground: a wide, thin box whose top surface sits at y = 0
    // (centre y = -0.5, half-height 0.5).
    const entt::entity ground = world.createEntity("ground");
    world.registry.get<LocalTransform>(ground).translation = {0.0f, -0.5f, 0.0f};
    RigidBody& g = world.registry.emplace<RigidBody>(ground);
    g.motionType = RigidBody::MotionType::Static;
    g.shape = RigidBody::ShapeType::Box;
    g.halfExtents = {50.0f, 0.5f, 50.0f};

    // Dynamic unit box (half-extent 0.5, mass 1) dropped from y = 5.
    const entt::entity box = world.createEntity("box");
    world.registry.get<LocalTransform>(box).translation = {0.0f, 5.0f, 0.0f};
    RigidBody& b = world.registry.emplace<RigidBody>(box);
    b.motionType = RigidBody::MotionType::Dynamic;
    b.shape = RigidBody::ShapeType::Box;
    b.halfExtents = {0.5f, 0.5f, 0.5f};

    // Step ~2.5 s at the fixed 60 Hz delta (extra margin over "~2 s"). Measure
    // per-step motion over the final 30 steps to prove the box is at rest.
    float prevY = 5.0f;
    float maxSettleDelta = 0.0f;
    for (int i = 0; i < 150; ++i) {
        physics.update(world, PhysicsSystem::kFixedDelta);
        const float y = world.registry.get<LocalTransform>(box).translation.y;
        if (i >= 120) {
            maxSettleDelta = std::max(maxSettleDelta, std::abs(y - prevY));
        }
        prevY = y;
    }

    const float restY = world.registry.get<LocalTransform>(box).translation.y;

    // The box centre rests on the plane (ground top 0.0 + box half-height 0.5).
    PHYS_CHECK(std::abs(restY - 0.5f) < 0.05f);
    // It did not tunnel through the ground.
    PHYS_CHECK(restY > 0.45f);
    // Over the final 30 steps it moved < 1 mm/step, i.e. linear velocity ~ 0.
    PHYS_CHECK(maxSettleDelta < 1e-3f);

    if (g_failures == 0) {
        std::printf(
            "physicstest: box at rest y=%.4f (max step delta=%.2e) — PASS\n",
            static_cast<double>(restY),
            static_cast<double>(maxSettleDelta));
        return 0;
    }
    std::printf("physicstest: %d FAILURES\n", g_failures);
    return 1;
}

} // namespace candela
