#include "Commands.h"
#include "EditorApp.h"

#include <candela/assets/AssetRegistry.h>
#include <candela/core/Events.h>
#include <candela/core/Jobs.h>
#include <candela/core/Log.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Renderer.h>
#include <candela/scene/SceneSerializer.h>
#include <candela/scene/World.h>

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>

namespace {

using namespace candela;
using namespace candela::editor;

// Exercises the command stack end-to-end: create / move / edit / delete /
// instantiate, undo everything, verify, redo everything, verify.
bool runCommandSelftest(World& world, AssetRegistry& assets,
                        AssetGuid modelGuid) {
    assignEditorIds(world);
    CommandStack commands;
    const size_t count0 = world.registry.view<Name>().size();

    EntitySnapshot lightSnapshot;
    lightSnapshot.editorId = reserveEditorId();
    lightSnapshot.name = "selftest_light";
    lightSnapshot.pointLight = PointLightComponent{};
    commands.perform(world,
                     std::make_unique<CreateEntityCommand>(lightSnapshot));
    const uint64_t lightId = lightSnapshot.editorId;

    LocalTransform before =
        world.registry.get<LocalTransform>(findByEditorId(world, lightId));
    LocalTransform after = before;
    after.translation = {1.0f, 2.0f, 3.0f};
    commands.perform(world,
                     std::make_unique<TransformCommand>(lightId, before, after));

    PointLightComponent lightBefore{};
    PointLightComponent lightAfter{};
    lightAfter.intensity = 9.0f;
    commands.perform(
        world, std::make_unique<ComponentEditCommand<PointLightComponent>>(
                   lightId, lightBefore, lightAfter));

    commands.perform(world,
                     std::make_unique<DeleteEntityCommand>(snapshotEntity(
                         world, findByEditorId(world, lightId))));

    // Models must group under exactly one new root entity.
    auto countRoots = [&world] {
        size_t roots = 0;
        for (const entt::entity entity : world.registry.view<Name>()) {
            const auto* parent = world.registry.try_get<Parent>(entity);
            if (parent == nullptr || !world.registry.valid(parent->value)) {
                ++roots;
            }
        }
        return roots;
    };
    const size_t rootsBefore = countRoots();
    commands.perform(
        world, std::make_unique<InstantiateModelCommand>(modelGuid, assets));
    const size_t rootsAfter = countRoots();
    const size_t countAfterAll = world.registry.view<Name>().size();
    bool pass = rootsAfter == rootsBefore + 1;
    CD_INFO("Selftest grouping: instantiate added {} root(s) (expected 1) "
            "=> {}",
            rootsAfter - rootsBefore, pass ? "PASS" : "FAIL");
    if (!pass) {
        return false;
    }

    // Undo all five.
    while (commands.canUndo()) {
        commands.undo(world);
    }
    const size_t afterUndo = world.registry.view<Name>().size();
    pass = afterUndo == count0 && findByEditorId(world, lightId) == entt::null;
    CD_INFO("Selftest undo: entities {} -> {} (expected {}), light gone: {} "
            "=> {}",
            count0, afterUndo, count0,
            findByEditorId(world, lightId) == entt::null,
            pass ? "PASS" : "FAIL");
    if (!pass) {
        return false;
    }

    // Redo all five — back to the exact post-command state.
    while (commands.canRedo()) {
        commands.redo(world);
    }
    const size_t afterRedo = world.registry.view<Name>().size();
    pass = afterRedo == countAfterAll;
    CD_INFO("Selftest redo: entities {} (expected {}) => {}", afterRedo,
            countAfterAll, pass ? "PASS" : "FAIL");

    // Clean up the redone instantiation so the scene file stays untouched.
    while (commands.canUndo()) {
        commands.undo(world);
    }
    return pass;
}

} // namespace

int main(int argc, char** argv) {
    candela::Log::init();
    CD_INFO("Candela Studio — Phase 4");

    bool selftest = false;
    uint64_t maxFrames = 0;
    std::filesystem::path screenshotPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
        }
    }

    JobSystem::init();
    int exitCode = 0;
    {
        WindowDesc desc;
        desc.width = 1920;
        desc.height = 1080;
        desc.title = "Candela Studio";
        Window window{desc};
        Renderer renderer{window};
        EventBus events;
        AssetRegistry assets{renderer.context(), renderer.bindless(), events};

        const std::filesystem::path assetDir{CANDELA_ASSET_DIR};
        assets.scan(assetDir);
        const std::filesystem::path scenePath =
            assetDir / "scenes" / "sponza.candela";

        World world;
        if (std::filesystem::exists(scenePath)) {
            SceneSerializer::load(world, assets, scenePath);
        } else {
            CD_WARN("No scene at {} — starting empty (run the sandbox once "
                    "or save from the editor)",
                    scenePath.string());
        }

        EditorApp editor{window, renderer, assets, assetDir, scenePath};

        if (selftest) {
            const AssetGuid sponza = assets.guidForPath(
                assetDir / "Sponza" / "glTF" / "Sponza.gltf");
            if (sponza != kInvalidGuid) {
                assets.getModelBlocking(sponza); // selftest needs templates
            }
            if (!runCommandSelftest(world, assets, sponza)) {
                exitCode = 1;
            }
            if (maxFrames == 0) {
                maxFrames = 240; // run on for the pick selftest below
            }
        }

        uint64_t frameCount = 0;
        bool pickIssued = false;
        bool pickChecked = false;
        auto lastFrameTime = std::chrono::steady_clock::now();

        while (!window.shouldClose()) {
            window.pollEvents();
            const auto now = std::chrono::steady_clock::now();
            const float dt =
                std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            assets.update();
            RenderOptions options = editor.frame(world, dt);

            // Pick selftest: once Sponza is in, click the viewport center
            // and expect to hit something.
            if (selftest && !pickIssued && frameCount > 60) {
                options.pickPixel = glm::ivec2(
                    static_cast<int>(options.viewportExtent.width / 2),
                    static_cast<int>(options.viewportExtent.height / 2));
                pickIssued = true;
            }

            if (!screenshotPath.empty() && maxFrames != 0 &&
                frameCount == maxFrames - 10) {
                renderer.requestScreenshot(screenshotPath);
            }

            world.updateTransforms();
            renderer.drawFrame(editor.camera(), world, assets, options);
            FrameMark;

            if (selftest && pickIssued && !pickChecked) {
                if (auto picked = renderer.takePickResult()) {
                    pickChecked = true;
                    const bool hit = *picked != 0;
                    CD_INFO("Selftest pick at viewport center: id {} => {}",
                            *picked, hit ? "PASS" : "FAIL");
                    if (!hit) {
                        exitCode = 1;
                    }
                }
            }

            ++frameCount;
            if (maxFrames != 0 && frameCount >= maxFrames) {
                CD_INFO("Reached frame limit ({}), exiting", maxFrames);
                break;
            }
        }
        if (selftest && !pickChecked) {
            CD_ERROR("Selftest pick: no result received => FAIL");
            exitCode = 1;
        }
        CD_INFO("Editor shutting down after {} frames", frameCount);
    }
    JobSystem::shutdown();
    return exitCode;
}
