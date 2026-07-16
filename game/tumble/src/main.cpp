// Tumble — a physics playground built on Candela.
//
// Drive a capsule character with WASD around a walled arena to shove, stack, and
// topple a heap of dynamic boxes — a showcase of the engine's Jolt physics
// pillar. Hold the right mouse button to orbit the follow camera, Escape quits.
//
// Headless proof: `candela-tumble --selftest` runs the whole sandbox as a
// pure simulation (no GPU) and asserts the character stays grounded while it
// scatters the stack. `--autoplay` drives itself into the stack (an attract
// mode and a rendered end-to-end check with --screenshot).
#include <candela/assets/AssetRegistry.h>
#include <candela/assets/ModelAsset.h>
#include <candela/core/Events.h>
#include <candela/core/Jobs.h>
#include <candela/core/Log.h>
#include <candela/physics/PhysicsSystem.h>
#include <candela/platform/Input.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>
#include <candela/scene/Components.h>
#include <candela/scene/World.h>

#include <tracy/Tracy.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

namespace {

using namespace candela;

// Arena + stack tuning, shared by the self-test and the rendered game.
constexpr float kArenaHalf = 8.0f; // half-width of the square floor
constexpr float kWallHeight = 2.0f;
constexpr float kBoxHalf = 0.4f; // dynamic box half-extent
constexpr int kStackBase = 3;    // boxes per side of the bottom layer
constexpr int kStackLayers = 4;  // layers (narrowing as it rises)

// Player + camera tuning.
constexpr float kPlayerRadius = 0.35f;
constexpr float kPlayerHalfHeight = 0.6f;
constexpr float kMoveSpeed = 4.5f; // m/s the character is driven at
constexpr float kCameraDistance = 6.5f;
constexpr float kCameraHeight = 1.4f; // aim point above the player's feet

struct Sandbox {
    entt::entity player = entt::null;
    std::vector<entt::entity> boxes;
    glm::vec3 stackCenter{0.0f};
};

// Direction the engine camera looks along for a given yaw/pitch (matches
// Camera::forward: yaw = atan2(-x, -z), pitch = asin(y)).
glm::vec3 lookDirection(float yaw, float pitch) {
    const float cosPitch = std::cos(pitch);
    return {-std::sin(yaw) * cosPitch, std::sin(pitch),
            -std::cos(yaw) * cosPitch};
}

// Adds a static box collider; also draws it (unit cube scaled to the collider)
// when a mesh is supplied. cube.glb is a centred unit cube, so scale = 2*half.
entt::entity addStaticBox(World& world, const char* name, glm::vec3 pos,
                          glm::vec3 halfExtents, AssetGuid cubeGuid) {
    const entt::entity e = world.createEntity(name);
    LocalTransform& lt = world.registry.get<LocalTransform>(e);
    lt.translation = pos;
    if (cubeGuid != kInvalidGuid) {
        lt.scale = halfExtents * 2.0f;
        world.registry.emplace<MeshRenderer>(e, cubeGuid, 0u);
    }
    RigidBody& rb = world.registry.emplace<RigidBody>(e);
    rb.motionType = RigidBody::MotionType::Static;
    rb.shape = RigidBody::ShapeType::Box;
    rb.halfExtents = halfExtents;
    return e;
}

// Builds the arena (floor + 4 walls), a stack of dynamic boxes, and the capsule
// character. Pure ECS. With a valid cubeGuid every body also gets a cube mesh
// scaled to its collider; pass kInvalidGuid for the GPU-free self-test.
Sandbox buildSandbox(World& world, AssetGuid cubeGuid) {
    Sandbox sb;

    addStaticBox(world, "floor", {0.0f, -0.5f, 0.0f},
                 {kArenaHalf, 0.5f, kArenaHalf}, cubeGuid);
    const float wy = kWallHeight * 0.5f;
    addStaticBox(world, "wall_n", {0.0f, wy, -kArenaHalf},
                 {kArenaHalf, wy, 0.25f}, cubeGuid);
    addStaticBox(world, "wall_s", {0.0f, wy, kArenaHalf},
                 {kArenaHalf, wy, 0.25f}, cubeGuid);
    addStaticBox(world, "wall_w", {-kArenaHalf, wy, 0.0f},
                 {0.25f, wy, kArenaHalf}, cubeGuid);
    addStaticBox(world, "wall_e", {kArenaHalf, wy, 0.0f},
                 {0.25f, wy, kArenaHalf}, cubeGuid);

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
                LocalTransform& lt = world.registry.get<LocalTransform>(box);
                lt.translation = {sb.stackCenter.x + ox, y, sb.stackCenter.z + oz};
                if (cubeGuid != kInvalidGuid) {
                    lt.scale = glm::vec3(kBoxHalf * 2.0f);
                    world.registry.emplace<MeshRenderer>(box, cubeGuid, 0u);
                }
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

    sb.player = world.createEntity("player");
    LocalTransform& plt = world.registry.get<LocalTransform>(sb.player);
    plt.translation = {0.0f, 1.0f, -3.0f};
    if (cubeGuid != kInvalidGuid) {
        // Approximate the capsule with a tall box, so the player reads clearly.
        plt.scale = {kPlayerRadius * 2.0f,
                     (kPlayerHalfHeight + kPlayerRadius) * 2.0f,
                     kPlayerRadius * 2.0f};
        world.registry.emplace<MeshRenderer>(sb.player, cubeGuid, 0u);
    }
    CharacterController& cc =
        world.registry.emplace<CharacterController>(sb.player);
    cc.radius = kPlayerRadius;
    cc.halfHeight = kPlayerHalfHeight;

    return sb;
}

int runSelfTest() {
    World world;
    PhysicsSystem physics;
    Sandbox sb = buildSandbox(world, kInvalidGuid);

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

SceneSettings daylightSettings() {
    SceneSettings s;
    s.toSun = glm::normalize(glm::vec3(0.35f, 1.0f, 0.28f));
    s.sunIntensity = 4.2f;
    s.sunColor = {1.0f, 0.96f, 0.9f};
    s.iblIntensity = 0.55f;
    s.exposure = 1.0f;
    s.bloomStrength = 0.04f;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    Log::init();

    uint64_t maxFrames = 0;
    std::filesystem::path screenshotPath;
    bool autoplay = false;
    uint32_t windowWidth = 1600;
    uint32_t windowHeight = 900;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            return runSelfTest();
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--autoplay") == 0) {
            autoplay = true;
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            unsigned w = 0;
            unsigned h = 0;
            if (std::sscanf(argv[i + 1], "%ux%u", &w, &h) == 2 && w > 0 &&
                h > 0) {
                windowWidth = w;
                windowHeight = h;
            }
        }
    }

    JobSystem::init();
    {
        WindowDesc desc;
        desc.title = "Tumble";
        desc.width = windowWidth;
        desc.height = windowHeight;
        Window window{desc};
        Renderer renderer{window};
        EventBus events;
        AssetRegistry assets{renderer.context(), renderer.bindless(), events};

        const std::filesystem::path contentDir{TUMBLE_CONTENT_DIR};
        assets.scan(contentDir);
        const AssetGuid cubeGuid = assets.guidForPath(contentDir / "cube.glb");
        CD_ASSERT(cubeGuid != kInvalidGuid, "Tumble content missing: {}",
                  (contentDir / "cube.glb").string());
        assets.getModelBlocking(cubeGuid);

        World world;
        world.settings = daylightSettings();
        Sandbox sb = buildSandbox(world, cubeGuid);
        PhysicsSystem physics;

        const InputActions input = InputActions::flyCameraDefaults();
        Camera camera;
        float camYaw = glm::radians(180.0f); // look down +Z toward the stack
        float camPitch = glm::radians(-28.0f);
        glm::vec3 cameraTarget{0.0f, kCameraHeight, -3.0f};

        uint64_t frameCount = 0;
        bool screenshotTaken = false;
        auto lastFrame = std::chrono::steady_clock::now();
        auto lastTitle = lastFrame;

        while (!window.shouldClose()) {
            window.pollEvents();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(
                std::chrono::duration<float>(now - lastFrame).count(), 0.05f);
            lastFrame = now;

            assets.update();

            // Camera orbit (hold RMB).
            const bool looking = input.isDown(window, "look");
            window.setCursorCaptured(looking);
            const glm::vec2 mouseDelta = window.consumeMouseDelta();
            if (looking) {
                camYaw -= mouseDelta.x * 0.0035f;
                camPitch = std::clamp(camPitch - mouseDelta.y * 0.0035f,
                                      glm::radians(-70.0f), glm::radians(-8.0f));
            }

            const glm::vec3 playerPos =
                world.registry.get<LocalTransform>(sb.player).translation;

            // Camera-relative WASD on the floor plane.
            const glm::vec3 camF = lookDirection(camYaw, 0.0f);
            const glm::vec2 forward =
                glm::normalize(glm::vec2(camF.x, camF.z));
            const glm::vec2 rightv{-forward.y, forward.x};
            glm::vec2 move =
                forward * input.axis(window, "move_forward", "move_back") +
                rightv * input.axis(window, "move_right", "move_left");

            if (autoplay) {
                // Charge the stack; once through it, sweep the far side so the
                // scatter keeps developing for a screenshot.
                const glm::vec2 toStack{sb.stackCenter.x - playerPos.x,
                                        sb.stackCenter.z - playerPos.z};
                move = toStack;
            }
            if (glm::dot(move, move) > 1.0f) {
                move = glm::normalize(move);
            }
            world.registry.get<CharacterController>(sb.player).desiredVelocity =
                glm::vec3(move.x, 0.0f, move.y) * kMoveSpeed;

            physics.update(world, dt);

            // Follow camera trails the player.
            const glm::vec3 targetGoal{playerPos.x, kCameraHeight, playerPos.z};
            const float follow = 1.0f - std::exp(-8.0f * dt);
            cameraTarget = glm::mix(cameraTarget, targetGoal, follow);
            camera.yawRadians = camYaw;
            camera.pitchRadians = camPitch;
            camera.position =
                cameraTarget - lookDirection(camYaw, camPitch) * kCameraDistance;

            world.updateTransforms();

            if (!screenshotPath.empty() && !screenshotTaken && maxFrames != 0 &&
                frameCount == maxFrames - 10) {
                renderer.requestScreenshot(screenshotPath);
                screenshotTaken = true;
            }

            renderer.drawFrame(camera, world, assets);
            FrameMark;

            if (now - lastTitle >= std::chrono::milliseconds(250)) {
                lastTitle = now;
                window.setTitle(std::format(
                    "Tumble — {} boxes — WASD move, hold RMB to look, Esc quits",
                    sb.boxes.size()));
            }

            ++frameCount;
            if (maxFrames != 0 && frameCount >= maxFrames) {
                CD_INFO("Reached frame limit ({}), exiting", maxFrames);
                break;
            }
        }
        CD_INFO("Tumble: {} boxes, {} frames", sb.boxes.size(), frameCount);
    }
    JobSystem::shutdown();
    return 0;
}
