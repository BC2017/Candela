#include <candela/core/Log.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Renderer.h>

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>

int main(int argc, char** argv) {
    candela::Log::init();
    CD_INFO("Candela sandbox — Phase 0");

    // --frames N: exit after N frames (used by automated verification).
    uint64_t maxFrames = 0;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        }
    }

    candela::Window window{candela::WindowDesc{}};
    candela::Renderer renderer{window};

    uint64_t frameCount = 0;
    uint32_t framesThisSecond = 0;
    auto lastTitleUpdate = std::chrono::steady_clock::now();

    while (!window.shouldClose()) {
        window.pollEvents();
        renderer.drawFrame();
        FrameMark;

        ++frameCount;
        ++framesThisSecond;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTitleUpdate >= std::chrono::seconds(1)) {
            const float ms = 1000.0f / static_cast<float>(framesThisSecond);
            window.setTitle(std::format("Candela — Phase 0 | {:.2f} ms ({} fps)",
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
