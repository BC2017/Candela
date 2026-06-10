#pragma once

#include "Commands.h"

#include <candela/platform/Input.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>

#include <filesystem>
#include <optional>
#include <string>

namespace candela {
class Window;
class AssetRegistry;
class World;
} // namespace candela

namespace candela::editor {

// Candela Studio: ImGui docking shell with viewport, hierarchy, inspector,
// content browser, scene settings; gizmos, picking, undo/redo, play mode.
class EditorApp {
public:
    EditorApp(Window& window, Renderer& renderer, AssetRegistry& assets,
              std::filesystem::path assetDir,
              std::filesystem::path scenePath);
    ~EditorApp();

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // Builds the frame's UI and camera motion; the result feeds drawFrame.
    RenderOptions frame(World& world, float dt);

    Camera& camera() { return m_camera; }
    CommandStack& commands() { return m_commands; }
    bool playing() const { return m_playing; }

    void saveScene(World& world);

private:
    void drawMenuBar(World& world);
    void drawViewport(World& world);
    void drawHierarchy(World& world);
    void drawEntityNode(World& world, entt::entity entity);
    void drawInspector(World& world);
    void drawContentBrowser(World& world);
    void drawSceneSettings(World& world);
    void handleShortcuts(World& world);
    void updateViewportTexture();
    void startPlay(World& world);
    void stopPlay(World& world);
    void deleteSelected(World& world);
    void focusSelected(World& world);
    entt::entity selectedEntity(World& world);

    Window& m_window;
    Renderer& m_renderer;
    AssetRegistry& m_assets;
    std::filesystem::path m_assetDir;
    std::filesystem::path m_scenePath;

    Camera m_camera;
    InputActions m_input;
    CommandStack m_commands;

    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;
    VkDescriptorSet m_viewportTexture = VK_NULL_HANDLE;
    uint64_t m_viewportTextureGeneration = UINT64_MAX;

    uint64_t m_selected = 0; // EditorId, 0 = none
    VkExtent2D m_viewportExtent{1280, 720};
    glm::vec2 m_viewportImagePos{0.0f};
    bool m_viewportHovered = false;
    std::optional<glm::ivec2> m_pendingPick;

    int m_gizmoOperation = 0; // 0 translate, 1 rotate, 2 scale
    bool m_gizmoWasUsing = false;
    LocalTransform m_gizmoBefore;

    // Inspector edit-in-progress captures (pushed as commands on release).
    LocalTransform m_transformBefore;
    PointLightComponent m_lightBefore;
    CameraComponent m_cameraBefore;
    SceneSettings m_settingsBefore;
    std::string m_nameBefore;
    char m_nameBuffer[128] = {};

    bool m_playing = false;
    std::string m_playSnapshot;

    DebugView m_debugView = DebugView::Final;
    float m_orbitDistance = 5.0f;
};

} // namespace candela::editor
