#include <candela/core/Log.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>
#include <candela/scene/GltfScene.h>

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>

int main(int argc, char** argv) {
    candela::Log::init();
    CD_INFO("Candela sandbox — Phase 1");

    // --frames N: exit after N frames (used by automated verification).
    uint64_t maxFrames = 0;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        }
    }

    candela::Window window{candela::WindowDesc{}};
    candela::Renderer renderer{window};

    const std::filesystem::path scenePath =
        std::filesystem::path(CANDELA_ASSET_DIR) / "Sponza" / "glTF" /
        "Sponza.gltf";
    if (std::filesystem::exists(scenePath)) {
        renderer.setScene(candela::loadGltfScene(renderer.context(),
                                                 renderer.bindless(),
                                                 scenePath));
    } else {
        CD_WARN("Scene not found: {} — run scripts/get-assets.ps1 to download "
                "Sponza. Rendering an empty world.",
                scenePath.string());
    }

    candela::Camera camera;
    camera.position = {-7.0f, 1.8f, -0.5f};
    camera.yawRadians = glm::radians(-90.0f);

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

        camera.update(window, dt);
        renderer.drawFrame(camera);
        FrameMark;

        ++frameCount;
        ++framesThisSecond;
        if (now - lastTitleUpdate >= std::chrono::seconds(1)) {
            const float ms = 1000.0f / static_cast<float>(framesThisSecond);
            window.setTitle(std::format(
                "Candela — Phase 1 | {:.2f} ms ({} fps) | RMB look, WASD move",
                ms, framesThisSecond));
            framesThisSecond = 0;
            lastTitleUpdate = now;
        }

        if (maxFrames != 0 && frameCount >= maxFrames) {
            CD_INFO("Reached frame limit ({}), exiting", maxFrames);
            break;
        }
    }

    CD_INFO("Shutting down after {} frames", frameCount);
    return 0;
}
