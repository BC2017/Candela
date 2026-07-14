// Lightkeeper — a small game built on Candela.
//
// You are a wisp of flame drifting through a night-shrouded shrine. Relight
// every extinguished candle; when the last one burns, dawn breaks over the
// maze. WASD moves (camera-relative), hold the right mouse button to orbit
// the camera, Escape quits. Progress and the run timer live in the window
// title; everything else is told through light.
#include "Level.h"

#include <candela/assets/AssetRegistry.h>
#include <candela/assets/ModelAsset.h>
#include <candela/core/Events.h>
#include <candela/core/Jobs.h>
#include <candela/core/Log.h>
#include <candela/platform/Input.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>
#include <candela/scene/World.h>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

namespace {

using namespace candela;

// --- Tuning ---
constexpr float kWispRadius = 0.28f;   // collision circle
constexpr float kWispHeight = 0.55f;   // hover height above the floor
constexpr float kAccel = 22.0f;        // m/s^2 while a key is held
constexpr float kDamping = 7.5f;       // exponential velocity decay
constexpr float kMaxSpeed = 3.6f;      // m/s
constexpr float kIgniteRadius = 0.55f; // wisp-to-candle ignition distance
constexpr float kWallHeight = 1.2f;
constexpr float kCameraDistance = 4.6f;
constexpr float kDawnSeconds = 9.0f;

// Game-local ECS component; never serialized.
struct Candle {
    bool lit = false;
    float litAt = 0.0f;  // game time of ignition (light fades in from here)
    float phase = 0.0f;  // decorrelates the flicker between candles
    entt::entity light = entt::null; // child entity carrying the point light
};

// Point lights sit on child entities floating above the emitting mesh: a
// light at the centre of its own geometry is fully self-shadowed under
// ray-traced shadows (every shadow ray hits the emitter first).
constexpr float kWispLightLift = 0.42f;
constexpr float kCandleLightLift = 0.62f;

uint32_t meshIndexByName(const ModelAsset& model, std::string_view name) {
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        if (model.meshes[i].name == name) {
            return static_cast<uint32_t>(i);
        }
    }
    CD_ASSERT(false, "Mesh '{}' missing from model", name);
    return 0;
}

// Direction the engine camera looks along for given yaw/pitch (matches
// Camera::forward: yaw = atan2(-x, -z), pitch = asin(y)).
glm::vec3 lookDirection(float yaw, float pitch) {
    const float cosPitch = std::cos(pitch);
    return {-std::sin(yaw) * cosPitch, std::sin(pitch),
            -std::cos(yaw) * cosPitch};
}

SceneSettings nightSettings() {
    SceneSettings night;
    night.toSun = glm::normalize(glm::vec3(0.85f, 0.18f, 0.35f)); // low moon
    night.sunIntensity = 0.10f;
    night.sunColor = {0.45f, 0.55f, 0.95f};
    night.iblIntensity = 0.05f;
    night.exposure = 1.3f;
    night.bloomStrength = 0.12f;
    return night;
}

// Blends the scene from night to daybreak; t in [0, 1].
void applyDawn(SceneSettings& settings, float t) {
    const SceneSettings night = nightSettings();
    const float s = t * t * (3.0f - 2.0f * t); // smoothstep
    settings.toSun = glm::normalize(
        glm::mix(night.toSun, glm::vec3(0.35f, 0.9f, 0.2f), s));
    settings.sunIntensity = glm::mix(night.sunIntensity, 3.8f, s);
    // The sun colour passes through sunrise orange on its way to morning
    // white — a straight blue-to-white mix has no golden hour in it.
    const glm::vec3 sunriseOrange{1.0f, 0.5f, 0.22f};
    const glm::vec3 morningWhite{1.0f, 0.9f, 0.72f};
    settings.sunColor =
        s < 0.5f ? glm::mix(night.sunColor, sunriseOrange, s * 2.0f)
                 : glm::mix(sunriseOrange, morningWhite, (s - 0.5f) * 2.0f);
    settings.iblIntensity = glm::mix(night.iblIntensity, 0.45f, s);
    settings.exposure = glm::mix(night.exposure, 1.0f, s);
    settings.bloomStrength = glm::mix(night.bloomStrength, 0.06f, s);
}

std::string formatTime(float seconds) {
    const int whole = static_cast<int>(seconds);
    return std::format("{:02}:{:02}", whole / 60, whole % 60);
}

} // namespace

int main(int argc, char** argv) {
    Log::init();
    CD_INFO("Lightkeeper — a Candela game");

    uint64_t maxFrames = 0; // headless smoke runs: --frames N
    std::filesystem::path screenshotPath;
    bool autoplay = false;      // attract mode: the wisp plays itself
    uint32_t windowWidth = 1600;
    uint32_t windowHeight = 900;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--autoplay") == 0) {
            autoplay = true;
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            unsigned width = 0;
            unsigned height = 0;
            if (std::sscanf(argv[i + 1], "%ux%u", &width, &height) == 2 &&
                width > 0 && height > 0) {
                windowWidth = width;
                windowHeight = height;
            }
        }
    }

    JobSystem::init();
    {
        WindowDesc desc;
        desc.title = "Lightkeeper";
        desc.width = windowWidth;
        desc.height = windowHeight;
        Window window{desc};
        Renderer renderer{window};
        EventBus events;
        AssetRegistry assets{renderer.context(), renderer.bindless(), events};

        const std::filesystem::path contentDir{LIGHTKEEPER_CONTENT_DIR};
        assets.scan(contentDir);
        const AssetGuid wispGuid = assets.guidForPath(contentDir / "wisp.glb");
        const AssetGuid candleGuid =
            assets.guidForPath(contentDir / "candle.glb");
        const AssetGuid wallGuid = assets.guidForPath(contentDir / "wall.glb");
        const AssetGuid floorGuid =
            assets.guidForPath(contentDir / "floor.glb");
        CD_ASSERT(wispGuid != kInvalidGuid && candleGuid != kInvalidGuid &&
                      wallGuid != kInvalidGuid && floorGuid != kInvalidGuid,
                  "Lightkeeper content missing under {}",
                  contentDir.string());

        const ModelAsset* candleModel = assets.getModelBlocking(candleGuid);
        const uint32_t unlitMesh = meshIndexByName(*candleModel, "CandleUnlit");
        const uint32_t litMesh = meshIndexByName(*candleModel, "CandleLit");
        assets.getModelBlocking(wispGuid);
        assets.getModelBlocking(wallGuid);
        assets.getModelBlocking(floorGuid);

        const lightkeeper::Level level =
            lightkeeper::Level::parse(lightkeeper::defaultMaze());
        CD_ASSERT(level.rectangular() && level.allCandlesReachable(),
                  "Broken maze — run lightkeeper-leveltest");

        World world;
        world.settings = nightSettings();

        // --- Floor: one slab stretched under the whole maze ---
        {
            const entt::entity floor = world.createEntity("floor");
            auto& t = world.registry.get<LocalTransform>(floor);
            t.translation = {static_cast<float>(level.width()) * 0.5f, 0.0f,
                             static_cast<float>(level.height()) * 0.5f};
            t.scale = {static_cast<float>(level.width()), 1.0f,
                       static_cast<float>(level.height())};
            world.registry.emplace<MeshRenderer>(floor, floorGuid, 0u);
        }

        // --- Walls: one cube per solid cell that borders open space ---
        uint32_t wallCount = 0;
        for (int z = 0; z < level.height(); ++z) {
            for (int x = 0; x < level.width(); ++x) {
                if (!level.solid(x, z)) {
                    continue;
                }
                bool exposed = false;
                for (int dz = -1; dz <= 1 && !exposed; ++dz) {
                    for (int dx = -1; dx <= 1 && !exposed; ++dx) {
                        exposed = !level.solid(x + dx, z + dz);
                    }
                }
                if (!exposed) {
                    continue; // fully buried — never visible
                }
                const entt::entity wall = world.createEntity(
                    std::format("wall_{}_{}", x, z));
                auto& t = world.registry.get<LocalTransform>(wall);
                t.translation = {static_cast<float>(x) + 0.5f,
                                 kWallHeight * 0.5f,
                                 static_cast<float>(z) + 0.5f};
                t.scale = {1.0f, kWallHeight, 1.0f};
                world.registry.emplace<MeshRenderer>(wall, wallGuid, 0u);
                ++wallCount;
            }
        }

        // --- Candles ---
        std::vector<entt::entity> candleEntities;
        for (size_t i = 0; i < level.candles().size(); ++i) {
            const glm::ivec2 cell = level.candles()[i];
            const glm::vec2 center = lightkeeper::Level::cellCenter(cell);
            const entt::entity candle = world.createEntity(
                std::format("candle_{}_{}", cell.x, cell.y));
            auto& t = world.registry.get<LocalTransform>(candle);
            t.translation = {center.x, 0.0f, center.y};
            world.registry.emplace<MeshRenderer>(candle, candleGuid,
                                                 unlitMesh);
            const entt::entity flameLight = world.createEntity(
                std::format("candle_light_{}_{}", cell.x, cell.y));
            world.registry.get<LocalTransform>(flameLight).translation = {
                0.0f, kCandleLightLift, 0.0f};
            world.setParent(flameLight, candle);
            Candle state;
            state.phase = static_cast<float>(i) * 2.39996f; // golden angle
            state.light = flameLight;
            world.registry.emplace<Candle>(candle, state);
            candleEntities.push_back(candle);
        }
        const int candleTotal = static_cast<int>(candleEntities.size());

        // --- The wisp (player) ---
        glm::vec2 wispPos = lightkeeper::Level::cellCenter(level.playerStart());
        glm::vec2 wispVel{0.0f};
        const entt::entity wisp = world.createEntity("wisp");
        world.registry.emplace<MeshRenderer>(wisp, wispGuid, 0u);
        const entt::entity wispLight = world.createEntity("wisp_light");
        world.registry.get<LocalTransform>(wispLight).translation = {
            0.0f, kWispLightLift, 0.0f};
        world.setParent(wispLight, wisp);
        {
            auto& light =
                world.registry.emplace<PointLightComponent>(wispLight);
            light.color = {1.0f, 0.72f, 0.4f};
            light.intensity = 2.4f;
            light.radius = 7.0f;
        }

        const InputActions input = InputActions::flyCameraDefaults();
        Camera camera;
        float camYaw = glm::radians(180.0f); // look down +Z into the maze
        float camPitch = glm::radians(-38.0f);
        glm::vec3 cameraTarget{wispPos.x, kWispHeight, wispPos.y};

        int litCount = 0;
        float gameTime = 0.0f;
        float winTime = -1.0f; // >= 0 once every candle burns
        float dawn = 0.0f;     // 0 = night, 1 = full daybreak
        std::deque<glm::vec2> waypoints; // autoplay steering targets

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
            gameTime += dt;

            assets.update();

            // --- Camera orbit (hold RMB) ---
            const bool looking = input.isDown(window, "look");
            window.setCursorCaptured(looking);
            const glm::vec2 mouseDelta = window.consumeMouseDelta();
            if (looking) {
                camYaw -= mouseDelta.x * 0.0035f;
                camPitch = std::clamp(camPitch - mouseDelta.y * 0.0035f,
                                      glm::radians(-70.0f),
                                      glm::radians(-15.0f));
            }

            // --- Wisp movement: camera-relative WASD on the floor plane ---
            const glm::vec3 camForward3 = lookDirection(camYaw, 0.0f);
            const glm::vec2 forward =
                glm::normalize(glm::vec2(camForward3.x, camForward3.z));
            const glm::vec2 right{-forward.y, forward.x};
            glm::vec2 move =
                forward * input.axis(window, "move_forward", "move_back") +
                right * input.axis(window, "move_right", "move_left");
            if (glm::dot(move, move) > 1.0f) {
                move = glm::normalize(move);
            }

            // Attract mode: BFS to the nearest unlit candle and steer along
            // the cell centres. Doubles as the headless gameplay test — a
            // full autoplay run proves ignition, the win state, and dawn.
            if (autoplay) {
                if (waypoints.empty() && litCount < candleTotal) {
                    const glm::ivec2 cell{
                        static_cast<int>(std::floor(wispPos.x)),
                        static_cast<int>(std::floor(wispPos.y))};
                    std::vector<glm::ivec2> best;
                    for (const entt::entity entity : candleEntities) {
                        if (world.registry.get<Candle>(entity).lit) {
                            continue;
                        }
                        const auto& t =
                            world.registry.get<LocalTransform>(entity);
                        const glm::ivec2 goal{
                            static_cast<int>(std::floor(t.translation.x)),
                            static_cast<int>(std::floor(t.translation.z))};
                        std::vector<glm::ivec2> path =
                            level.pathBetween(cell, goal);
                        if (!path.empty() &&
                            (best.empty() || path.size() < best.size())) {
                            best = std::move(path);
                        }
                    }
                    for (const glm::ivec2 step : best) {
                        waypoints.push_back(
                            lightkeeper::Level::cellCenter(step));
                    }
                }
                move = {0.0f, 0.0f};
                if (!waypoints.empty()) {
                    const glm::vec2 toNext = waypoints.front() - wispPos;
                    if (glm::dot(toNext, toNext) < 0.2f * 0.2f) {
                        waypoints.pop_front();
                    } else {
                        move = glm::normalize(toNext);
                    }
                }
            }
            wispVel += move * kAccel * dt;
            wispVel *= std::max(0.0f, 1.0f - kDamping * dt);
            const float speed = glm::length(wispVel);
            if (speed > kMaxSpeed) {
                wispVel *= kMaxSpeed / speed;
            }
            wispPos += wispVel * dt;
            wispPos = level.resolveCollision(wispPos, kWispRadius);

            // Hover bob + a live flame flicker on the wisp's light.
            {
                auto& t = world.registry.get<LocalTransform>(wisp);
                t.translation = {wispPos.x,
                                 kWispHeight +
                                     0.06f * std::sin(gameTime * 2.3f),
                                 wispPos.y};
                auto& light =
                    world.registry.get<PointLightComponent>(wispLight);
                light.intensity =
                    2.4f * (0.92f + 0.06f * std::sin(gameTime * 9.0f) +
                            0.03f * std::sin(gameTime * 23.0f));
            }

            // --- Candle ignition + lit-candle flicker ---
            for (const entt::entity entity : candleEntities) {
                auto& candle = world.registry.get<Candle>(entity);
                const auto& t = world.registry.get<LocalTransform>(entity);
                const glm::vec2 candleXZ{t.translation.x, t.translation.z};
                if (!candle.lit) {
                    const glm::vec2 toCandle = candleXZ - wispPos;
                    if (glm::dot(toCandle, toCandle) <
                        kIgniteRadius * kIgniteRadius) {
                        candle.lit = true;
                        candle.litAt = gameTime;
                        world.registry.get<MeshRenderer>(entity).meshIndex =
                            litMesh;
                        auto& light =
                            world.registry.emplace<PointLightComponent>(
                                candle.light);
                        light.color = {1.0f, 0.6f, 0.25f};
                        light.intensity = 0.0f;
                        light.radius = 5.0f;
                        ++litCount;
                        CD_INFO("Candle lit ({}/{})", litCount, candleTotal);
                        if (litCount == candleTotal) {
                            winTime = gameTime;
                            CD_INFO("All candles lit in {} — dawn breaks",
                                    formatTime(winTime));
                        }
                    }
                    continue;
                }
                auto& light =
                    world.registry.get<PointLightComponent>(candle.light);
                const float fadeIn =
                    std::min((gameTime - candle.litAt) / 0.8f, 1.0f);
                light.intensity =
                    1.7f * fadeIn *
                    (0.85f +
                     0.11f * std::sin(gameTime * 11.0f + candle.phase) +
                     0.04f * std::sin(gameTime * 27.0f + candle.phase * 3.1f));
            }

            // --- Dawn ramp after the last candle ---
            if (winTime >= 0.0f && dawn < 1.0f) {
                dawn = std::min(dawn + dt / kDawnSeconds, 1.0f);
                applyDawn(world.settings, dawn);
            }

            // --- Follow camera ---
            const glm::vec3 targetGoal{wispPos.x, kWispHeight, wispPos.y};
            const float follow = 1.0f - std::exp(-10.0f * dt);
            cameraTarget = glm::mix(cameraTarget, targetGoal, follow);
            camera.yawRadians = camYaw;
            camera.pitchRadians = camPitch;
            camera.position = cameraTarget -
                              lookDirection(camYaw, camPitch) *
                                  kCameraDistance;

            world.updateTransforms();

            // Screenshot: near the frame limit, or — on an autoplay win —
            // at mid-dawn, the moment worth framing.
            if (!screenshotPath.empty() && !screenshotTaken) {
                const bool frameTrigger =
                    maxFrames != 0 && frameCount == maxFrames - 10;
                const bool dawnTrigger = autoplay && dawn >= 0.5f;
                if (frameTrigger || dawnTrigger) {
                    renderer.requestScreenshot(screenshotPath);
                    screenshotTaken = true;
                }
            }

            renderer.drawFrame(camera, world, assets);
            FrameMark;

            if (now - lastTitle >= std::chrono::milliseconds(250)) {
                lastTitle = now;
                if (winTime < 0.0f) {
                    window.setTitle(std::format(
                        "Lightkeeper — {}/{} candles — {} — WASD move, hold "
                        "RMB to look, Esc quits",
                        litCount, candleTotal, formatTime(gameTime)));
                } else {
                    window.setTitle(std::format(
                        "Lightkeeper — dawn breaks! All {} candles lit in {} "
                        "— Esc quits",
                        candleTotal, formatTime(winTime)));
                }
            }

            ++frameCount;
            if (maxFrames != 0 && frameCount >= maxFrames) {
                CD_INFO("Reached frame limit ({}), exiting", maxFrames);
                break;
            }
        }
        CD_INFO("Lightkeeper: {} walls, {} candles ({} lit), {} frames",
                wallCount, candleTotal, litCount, frameCount);
    }
    JobSystem::shutdown();
    return 0;
}
