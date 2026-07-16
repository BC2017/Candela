// Tumble — a physics playground built on Candela.
//
// A capsule character you drive with WASD shoves, stacks, and topples a heap of
// dynamic boxes in a walled arena — a showcase of the engine's Jolt physics
// pillar. This first slice is the headless physics core: `candela-tumble
// --selftest` builds the sandbox, charges the character into a box stack, and
// asserts the character stays grounded while the stack scatters. The rendered
// play mode lands in the next increment.
#include <candela/core/Log.h>
#include <candela/physics/PhysicsSystem.h>
#include <candela/scene/Components.h>
#include <candela/scene/World.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace candela;

// Arena + stack tuning, shared by the self-test and (later) the rendered game.
constexpr float kArenaHalf = 8.0f; // half-width of the square floor
constexpr float kWallHeight = 2.0f;
constexpr float kBoxHalf = 0.4f;  // dynamic box half-extent
constexpr int kStackBase = 3;     // boxes per side of the bottom layer
constexpr int kStackLayers = 4;   // layers (narrowing as it rises)

struct Sandbox {
    entt::entity player = entt::null;
    std::vector<entt::entity> boxes;
    glm::vec3 stackCenter{0.0f};
};

entt::entity addStaticBox(World& world, const char* name, glm::vec3 pos,
                          glm::vec3 halfExtents) {
    const entt::entity e = world.createEntity(name);
    world.registry.get<LocalTransform>(e).translation = pos;
    RigidBody& rb = world.registry.emplace<RigidBody>(e);
    rb.motionType = RigidBody::MotionType::Static;
    rb.shape = RigidBody::ShapeType::Box;
    rb.halfExtents = halfExtents;
    return e;
}

// Builds the arena (floor + 4 walls), a stack of dynamic boxes, and the capsule
// character. Pure ECS — no GPU. Returns handles for gameplay and the test.
Sandbox buildSandbox(World& world) {
    Sandbox sb;

    // Floor: top surface at y = 0.
    addStaticBox(world, "floor", {0.0f, -0.5f, 0.0f},
                 {kArenaHalf, 0.5f, kArenaHalf});
    // Four rim walls.
    const float wy = kWallHeight * 0.5f;
    addStaticBox(world, "wall_n", {0.0f, wy, -kArenaHalf},
                 {kArenaHalf, wy, 0.25f});
    addStaticBox(world, "wall_s", {0.0f, wy, kArenaHalf},
                 {kArenaHalf, wy, 0.25f});
    addStaticBox(world, "wall_w", {-kArenaHalf, wy, 0.0f},
                 {0.25f, wy, kArenaHalf});
    addStaticBox(world, "wall_e", {kArenaHalf, wy, 0.0f},
                 {0.25f, wy, kArenaHalf});

    // A narrowing stack of dynamic boxes near +Z.
    sb.stackCenter = {0.0f, 0.0f, 3.0f};
    const float step = kBoxHalf * 2.02f; // a hair of clearance
    for (int layer = 0; layer < kStackLayers; ++layer) {
        const int n = kStackBase - layer;
        if (n <= 0) {
            break;
        }
        const float y = kBoxHalf + static_cast<float>(layer) * step;
        for (int ix = 0; ix < n; ++ix) {
            for (int iz = 0; iz < n; ++iz) {
                const float ox =
                    (static_cast<float>(ix) - static_cast<float>(n - 1) * 0.5f) *
                    step;
                const float oz =
                    (static_cast<float>(iz) - static_cast<float>(n - 1) * 0.5f) *
                    step;
                const entt::entity box = world.createEntity("box");
                world.registry.get<LocalTransform>(box).translation = {
                    sb.stackCenter.x + ox, y, sb.stackCenter.z + oz};
                RigidBody& rb = world.registry.emplace<RigidBody>(box);
                rb.motionType = RigidBody::MotionType::Dynamic;
                rb.shape = RigidBody::ShapeType::Box;
                rb.halfExtents = glm::vec3(kBoxHalf);
                rb.mass = 2.0f;
                rb.friction = 0.6f;
                sb.boxes.push_back(box);
            }
        }
    }

    // The player: a capsule character controller at -Z, facing the stack (+Z).
    sb.player = world.createEntity("player");
    world.registry.get<LocalTransform>(sb.player).translation = {0.0f, 1.0f,
                                                                  -3.0f};
    CharacterController& cc =
        world.registry.emplace<CharacterController>(sb.player);
    cc.radius = 0.35f;
    cc.halfHeight = 0.6f;

    return sb;
}

int runSelfTest() {
    World world;
    PhysicsSystem physics;
    Sandbox sb = buildSandbox(world);

    // Let the stack settle onto the floor before recording its footprint.
    for (int i = 0; i < 30; ++i) {
        physics.update(world, PhysicsSystem::kFixedDelta);
    }
    std::vector<glm::vec3> startPos;
    startPos.reserve(sb.boxes.size());
    for (const entt::entity box : sb.boxes) {
        startPos.push_back(world.registry.get<LocalTransform>(box).translation);
    }
    const float playerStartZ =
        world.registry.get<LocalTransform>(sb.player).translation.z;

    // Charge the character straight into the stack for ~3 s at 60 Hz.
    world.registry.get<CharacterController>(sb.player).desiredVelocity = {
        0.0f, 0.0f, 4.0f};
    int groundedFrames = 0;
    const int steps = 180;
    for (int i = 0; i < steps; ++i) {
        physics.update(world, PhysicsSystem::kFixedDelta);
        if (world.registry.get<CharacterController>(sb.player).onGround) {
            ++groundedFrames;
        }
    }

    const glm::vec3 playerEnd =
        world.registry.get<LocalTransform>(sb.player).translation;

    float maxBoxDisplacement = 0.0f;
    bool allFinite = true;
    for (size_t i = 0; i < sb.boxes.size(); ++i) {
        const glm::vec3 p =
            world.registry.get<LocalTransform>(sb.boxes[i]).translation;
        allFinite = allFinite && std::isfinite(p.x) && std::isfinite(p.y) &&
                    std::isfinite(p.z);
        maxBoxDisplacement =
            std::max(maxBoxDisplacement, glm::length(p - startPos[i]));
    }

    int failures = 0;
    const auto check = [&](bool cond, const char* what) {
        if (!cond) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    check(playerEnd.z > playerStartZ + 2.0f, "character advanced into the stack");
    check(std::abs(playerEnd.y - 1.0f) < 1.0f, "character stayed near floor");
    check(groundedFrames > steps / 2, "character grounded for most of the run");
    check(maxBoxDisplacement > kBoxHalf, "stack was knocked / scattered");
    check(allFinite, "all box positions finite (no blow-up)");

    if (failures == 0) {
        std::printf("tumble selftest: player z %.2f->%.2f, grounded %d/%d, "
                    "max box displacement %.2f m — PASS\n",
                    static_cast<double>(playerStartZ),
                    static_cast<double>(playerEnd.z), groundedFrames, steps,
                    static_cast<double>(maxBoxDisplacement));
        return 0;
    }
    std::printf("tumble selftest: %d FAILURES\n", failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    Log::init();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            return runSelfTest();
        }
    }
    CD_INFO("Tumble — a physics playground. Headless proof: --selftest");
    CD_INFO("(Rendered play mode lands in the next increment.)");
    return 0;
}
