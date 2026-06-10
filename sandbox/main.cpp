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
        if (!modelPath.empty()) {
            // Model-viewer mode: one model, default sun + IBL, auto-framed.
            const candela::AssetGuid guid =
                assets.guidForPath(std::filesystem::absolute(modelPath));
            if (guid == candela::kInvalidGuid) {
                CD_ERROR("Model not in the asset registry: {}",
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
            camera.update(window, input, dt);
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

        CD_INFO("Shutting down after {} frames", frameCount);
    }

    candela::JobSystem::shutdown();
    return 0;
}
