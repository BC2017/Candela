#pragma once

#include <glm/glm.hpp>

#include <memory>

namespace candela {

class World;

// Fixed-timestep rigid-body + character physics backed by Jolt. One instance
// owns a JPH::PhysicsSystem, a temp allocator, and a job pool. Every Jolt type
// and its ABI-defining macros are confined to PhysicsSystem.cpp (PIMPL), so
// this header — and everything that includes it — stays Jolt-free.
class PhysicsSystem {
public:
    static constexpr float kFixedDelta = 1.0f / 60.0f; // 60 Hz simulation

    PhysicsSystem();
    ~PhysicsSystem();
    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    // One frame of physics:
    //  1. lazily create a Jolt body for every RigidBody / CharacterController
    //     whose bodyId is still kInvalidBodyId, seeded from its LocalTransform;
    //  2. accumulate dt and run zero-or-more fixed steps (the accumulator is
    //     clamped to avoid the spiral of death);
    //  3. write DYNAMIC-body and character poses back into LocalTransform.
    // MUST be called BEFORE World::updateTransforms() so WorldTransform picks
    // up the freshly written local pose the same frame. Physics entities are
    // assumed top-level for this slice (LocalTransform == world pose).
    void update(World& world, float dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GPU-free self-test behind the sandbox's --physicstest flag. Drops a dynamic
// box onto a static ground plane, steps ~2.5 s of fixed timesteps, and asserts
// the box comes to rest ON the plane with ~zero residual motion. Returns 0 on
// pass and 1 on failure, mirroring the lightkeeper leveltest exit convention.
int runPhysicsSelfTest();

} // namespace candela
