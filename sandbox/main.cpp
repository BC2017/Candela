#include <candela/assets/AssetRegistry.h>
#include <candela/assets/ModelAsset.h>
#include <candela/core/Events.h>
#include <candela/core/Jobs.h>
#include <candela/core/Log.h>
#include <candela/platform/Input.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>
#include <candela/scene/SceneSerializer.h>
#include <candela/scene/World.h>

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace {

// Builds the demo scene in code: Sponza + four colonnade lights + settings.
void buildDefaultScene(candela::World& world, candela::AssetRegistry& assets,
                       candela::AssetGuid sponzaGuid) {
    world.instantiateModel(assets, sponzaGuid);

    struct LightSpec {
        glm::vec3 position;
        glm::vec3 color;
    };
    const LightSpec lightSpecs[] = {
        {{-9.5f, 1.6f, 1.2f}, {1.0f, 0.55f, 0.25f}},
        {{-9.5f, 1.6f, -1.8f}, {1.0f, 0.55f, 0.25f}},
        {{9.0f, 1.6f, 1.2f}, {0.3f, 0.55f, 1.0f}},
        {{9.0f, 1.6f, -1.8f}, {0.3f, 0.55f, 1.0f}},
    };
    int lightIndex = 0;
    for (const LightSpec& spec : lightSpecs) {
        const entt::entity entity =
            world.createEntity(std::format("light_{}", lightIndex++));
        world.registry.get<candela::LocalTransform>(entity).translation =
            spec.position;
        auto& light =
            world.registry.emplace<candela::PointLightComponent>(entity);
        light.color = spec.color;
        light.intensity = 2.5f;
        light.radius = 8.0f;
    }

    world.settings = {}; // defaults documented in Components.h
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

} // namespace

int main(int argc, char** argv) {
    candela::Log::init();
    CD_INFO("Candela sandbox — Phase 3");

    uint64_t maxFrames = 0;
    bool roundtripCheck = false;
    std::filesystem::path screenshotPath;
    std::filesystem::path modelPath; // view a single model instead of a scene
    bool noRT = false;               // isolate raster path (debugging aid)
    std::filesystem::path flythroughDir; // capture a camera-path PNG sequence
    bool benchmark = false; // dense stress scene + frame-time report
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--roundtrip-check") == 0) {
            roundtripCheck = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            modelPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--no-rt") == 0) {
            noRT = true;
        } else if (std::strcmp(argv[i], "--flythrough") == 0 && i + 1 < argc) {
            flythroughDir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
        }
    }

    candela::JobSystem::init();

    {
        candela::Window window{candela::WindowDesc{}};
        candela::Renderer renderer{window};
        candela::EventBus events;
        candela::AssetRegistry assets{renderer.context(), renderer.bindless(),
                                      events};

        events.subscribe<candela::AssetReloadedEvent>(
            [&assets](const candela::AssetReloadedEvent& event) {
                CD_INFO("Asset ready: {}",
                        assets.pathForGuid(event.guid).filename().string());
            });

        const std::filesystem::path assetDir{CANDELA_ASSET_DIR};
        assets.scan(assetDir);

        candela::World world;
        candela::Camera camera;
        camera.position = {-7.0f, 1.8f, -0.5f};
        camera.yawRadians = glm::radians(-90.0f);

        const std::filesystem::path scenePath =
            assetDir / "scenes" / "sponza.candela";
        if (benchmark) {
            // Stress scene: Sponza plus a two-deck grid of DamagedHelmets
            // (~19M visible triangles, 130 TLAS instances) and 48 colored
            // point lights — every light is a shadow-ray loop in the RT path.
            const candela::AssetGuid sponza = assets.guidForPath(
                assetDir / "Sponza" / "glTF" / "Sponza.gltf");
            const candela::AssetGuid helmet = assets.guidForPath(
                assetDir / "DamagedHelmet" / "glTF" / "DamagedHelmet.gltf");
            if (sponza == candela::kInvalidGuid ||
                helmet == candela::kInvalidGuid) {
                CD_ERROR("Benchmark needs Sponza and DamagedHelmet — run "
                         "scripts/get-assets.ps1");
                return 1;
            }
            world.instantiateModel(assets, sponza);
            uint32_t helmets = 0;
            for (int layer = 0; layer < 4; ++layer) {
                for (int x = 0; x < 40; ++x) {
                    for (int z = 0; z < 6; ++z) {
                        const auto entities =
                            world.instantiateModel(assets, helmet);
                        auto& t = world.registry.get<candela::LocalTransform>(
                            entities.front());
                        t.translation = {
                            -8.6f + 0.44f * static_cast<float>(x),
                            1.0f + 2.0f * static_cast<float>(layer),
                            -1.35f + 0.55f * static_cast<float>(z)};
                        t.scale = glm::vec3(0.40f);
                        t.rotation = glm::angleAxis(
                            glm::radians(static_cast<float>(
                                (x * 7 + z * 13 + layer * 29) % 360)),
                            glm::vec3(0.0f, 1.0f, 0.0f));
                        ++helmets;
                    }
                }
            }
            for (int i = 0; i < 64; ++i) {
                const entt::entity light =
                    world.createEntity("bench_light_" + std::to_string(i));
                auto& t = world.registry.get<candela::LocalTransform>(light);
                t.translation = {-8.5f + 0.27f * static_cast<float>(i),
                                 1.0f + 2.2f * static_cast<float>(i % 4),
                                 (i % 2 == 0) ? -1.2f : 1.2f};
                candela::PointLightComponent point;
                point.radius = 6.0f;
                point.intensity = 3.0f;
                point.color = {
                    0.5f + 0.5f * std::sin(0.41f * static_cast<float>(i)),
                    0.5f + 0.5f * std::sin(0.67f * static_cast<float>(i) + 2.1f),
                    0.5f + 0.5f * std::sin(0.93f * static_cast<float>(i) + 4.2f)};
                world.registry.emplace<candela::PointLightComponent>(light,
                                                                     point);
            }
            world.updateTransforms();
            const std::filesystem::path benchPath =
                assetDir / "scenes" / "benchmark.candela";
            std::filesystem::create_directories(benchPath.parent_path());
            candela::SceneSerializer::save(world, benchPath);
            CD_INFO("Benchmark scene: {} helmets, 64 lights (saved {})",
                    helmets, benchPath.string());
        } else if (!modelPath.empty()) {
            // Model-viewer mode: one model, default sun + IBL, auto-framed.
            candela::AssetGuid guid =
                assets.guidForPath(std::filesystem::absolute(modelPath));
            if (guid == candela::kInvalidGuid) {
                // Not under the scanned content dir — register it in place
                // (any .gltf/.glb/.blend anywhere on disk).
                guid = assets.registerFile(
                    std::filesystem::absolute(modelPath));
            }
            if (guid == candela::kInvalidGuid) {
                CD_ERROR("Not an importable model file: {}",
                         modelPath.string());
                return 1;
            }
            world.instantiateModel(assets, guid);
            world.settings = {};
            if (noRT) {
                world.settings.rtShadows = false;
                world.settings.rtAmbientOcclusion = false;
                world.settings.rtReflections = false;
            }

            const candela::ModelAsset* model = assets.getModelBlocking(guid);
            const glm::vec3 center =
                (model->boundsMin + model->boundsMax) * 0.5f;
            const float radius = (std::max)(
                glm::length(model->boundsMax - model->boundsMin) * 0.5f,
                0.01f);
            camera.position =
                center + glm::normalize(glm::vec3(1.0f, 0.45f, 1.0f)) *
                             radius * 1.9f;
            const glm::vec3 toCenter =
                glm::normalize(center - camera.position);
            camera.pitchRadians = std::asin(toCenter.y);
            camera.yawRadians = std::atan2(-toCenter.x, -toCenter.z);
            CD_INFO("Model viewer: {} (radius {:.2f} m)",
                    modelPath.filename().string(), radius);
        } else if (std::filesystem::exists(scenePath)) {
            candela::SceneSerializer::load(world, assets, scenePath);
        } else {
            const candela::AssetGuid sponzaGuid = assets.guidForPath(
                assetDir / "Sponza" / "glTF" / "Sponza.gltf");
            if (sponzaGuid != candela::kInvalidGuid) {
                buildDefaultScene(world, assets, sponzaGuid);
            } else {
                CD_WARN("Sponza not found — run scripts/get-assets.ps1. "
                        "Starting with an empty scene.");
            }
            std::filesystem::create_directories(scenePath.parent_path());
            candela::SceneSerializer::save(world, scenePath);
        }

        if (roundtripCheck) {
            // Exit-criteria proof: saving the just-loaded/just-built world
            // must reproduce the scene file byte for byte.
            const std::filesystem::path checkPath =
                std::filesystem::temp_directory_path() /
                "candela-roundtrip.candela";
            candela::World reloaded;
            candela::SceneSerializer::load(reloaded, assets, scenePath);
            candela::SceneSerializer::save(reloaded, checkPath);
            const bool identical =
                readFileText(scenePath) == readFileText(checkPath);
            CD_INFO("Scene round-trip check: {}",
                    identical ? "PASS" : "FAIL");
            if (!identical) {
                return 1;
            }
        }

        const candela::InputActions input =
            candela::InputActions::flyCameraDefaults();

        // Flythrough: Catmull-Rom path down the Sponza atrium at a fixed
        // 60 Hz timestep, one PNG per frame after a warmup for asset
        // streaming and temporal convergence.
        constexpr uint64_t kFlythroughWarmup = 900;
        constexpr uint64_t kFlythroughFrames = 720; // 12 s at 60 fps
        const glm::vec3 flyPoints[] = {
            {-9.5f, 1.6f, -0.3f}, {-8.5f, 1.6f, -0.2f}, {-4.0f, 2.6f, 0.8f},
            {1.5f, 1.5f, -0.8f},  {6.5f, 2.1f, 0.3f},   {8.5f, 2.4f, 0.0f},
        };
        auto catmullRom = [&](float t) {
            const int segments = static_cast<int>(std::size(flyPoints)) - 3;
            const float scaled = t * static_cast<float>(segments);
            const int seg = (std::min)(static_cast<int>(scaled), segments - 1);
            const float u = scaled - static_cast<float>(seg);
            const glm::vec3 p0 = flyPoints[seg];
            const glm::vec3 p1 = flyPoints[seg + 1];
            const glm::vec3 p2 = flyPoints[seg + 2];
            const glm::vec3 p3 = flyPoints[seg + 3];
            return 0.5f * ((2.0f * p1) + (-p0 + p2) * u +
                           (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u * u +
                           (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u * u * u);
        };
        if (!flythroughDir.empty()) {
            std::filesystem::create_directories(flythroughDir);
            maxFrames = kFlythroughWarmup + kFlythroughFrames;
        }
        // The benchmark flies the same path (without writing PNGs),
        // collecting frame times after the warmup.
        if (benchmark && maxFrames == 0) {
            maxFrames = kFlythroughWarmup + kFlythroughFrames;
        }
        std::vector<float> benchTimesMs;
        benchTimesMs.reserve(kFlythroughFrames);

        uint64_t frameCount = 0;
        uint32_t framesThisSecond = 0;
        auto lastTitleUpdate = std::chrono::steady_clock::now();
        auto lastFrameTime = lastTitleUpdate;

        while (!window.shouldClose()) {
            window.pollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt =
                std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            assets.update();
            if (flythroughDir.empty() && !benchmark) {
                camera.update(window, input, dt);
            } else if (frameCount >= kFlythroughWarmup) {
                // Drive the camera along the spline, looking down the
                // smoothed tangent.
                const uint64_t frame = frameCount - kFlythroughWarmup;
                const float t = static_cast<float>(frame) /
                                static_cast<float>(kFlythroughFrames - 1);
                const glm::vec3 position = catmullRom(t);
                const glm::vec3 ahead =
                    catmullRom((std::min)(t + 0.04f, 1.0f) ) ;
                const glm::vec3 dir = glm::normalize(
                    ahead - position + glm::vec3(1e-5f, 0.0f, 0.0f));
                camera.position = position;
                camera.pitchRadians = std::asin(glm::clamp(dir.y, -1.0f, 1.0f));
                camera.yawRadians = std::atan2(-dir.x, -dir.z);
                if (!flythroughDir.empty()) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "frame_%05llu.png",
                                  static_cast<unsigned long long>(frame));
                    renderer.requestScreenshot(flythroughDir / name);
                }
                if (benchmark) {
                    benchTimesMs.push_back(dt * 1000.0f);
                }
            } else {
                // Warmup parked at the path start so history converges there.
                camera.position = flyPoints[1];
                camera.yawRadians = glm::radians(-90.0f);
                camera.pitchRadians = 0.0f;
            }
            world.updateTransforms();

            // Capture late so async assets and temporal accumulation settle.
            if (!screenshotPath.empty() && maxFrames != 0 &&
                frameCount == maxFrames - 10) {
                renderer.requestScreenshot(screenshotPath);
            }

            renderer.drawFrame(camera, world, assets);
            FrameMark;

            ++frameCount;
            ++framesThisSecond;
            if (now - lastTitleUpdate >= std::chrono::seconds(1)) {
                const float ms =
                    1000.0f / static_cast<float>(framesThisSecond);
                window.setTitle(std::format(
                    "Candela — Phase 3 | {:.2f} ms ({} fps) | {} entities",
                    ms, framesThisSecond,
                    world.registry.storage<candela::Name>().size()));
                framesThisSecond = 0;
                lastTitleUpdate = now;
            }

            if (maxFrames != 0 && frameCount >= maxFrames) {
                CD_INFO("Reached frame limit ({}), exiting", maxFrames);
                break;
            }
        }

        const candela::RenderStats& stats = renderer.stats();
        CD_INFO("Final frame: {} draws ({} culled), {:.2f}M tris, "
                "GPU {:.2f} ms",
                stats.drawCalls, stats.culledDraws,
                static_cast<double>(stats.triangles) / 1e6,
                static_cast<double>(stats.gpuTotalMs));
        if (benchmark && benchTimesMs.size() > 10) {
            std::sort(benchTimesMs.begin(), benchTimesMs.end());
            const auto at = [&](double q) {
                return benchTimesMs[static_cast<size_t>(
                    q * static_cast<double>(benchTimesMs.size() - 1))];
            };
            float sum = 0.0f;
            for (const float ms : benchTimesMs) {
                sum += ms;
            }
            const float average =
                sum / static_cast<float>(benchTimesMs.size());
            CD_INFO("Benchmark ({} frames): avg {:.2f} ms ({:.0f} fps) | "
                    "p50 {:.2f} | p95 {:.2f} | max {:.2f} ms",
                    benchTimesMs.size(), average, 1000.0f / average, at(0.5),
                    at(0.95), at(1.0));
            CD_INFO("Benchmark scene: {} draws + {} culled, {:.2f}M visible "
                    "tris, {} TLAS instances, {} lights",
                    stats.drawCalls, stats.culledDraws,
                    static_cast<double>(stats.triangles) / 1e6,
                    stats.rtInstances, stats.pointLights);
            for (const candela::GpuPassTiming& pass : stats.gpuPasses) {
                CD_INFO("  GPU {:>16}: {:.3f} ms", pass.name,
                        pass.milliseconds);
            }
        }
        CD_INFO("Shutting down after {} frames", frameCount);
    }

    candela::JobSystem::shutdown();
    return 0;
}
