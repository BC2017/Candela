#include "EditorApp.h"

#include <candela/assets/AssetRegistry.h>
#include <candela/core/Log.h>
#include <candela/platform/Window.h>
#include <candela/scene/SceneSerializer.h>
#include <candela/scene/World.h>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (first-run default layout)
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>

namespace candela::editor {

namespace {

// Finite-far GL-style projection for ImGuizmo (the renderer's infinite
// reverse-Z projection confuses its depth math).
glm::mat4 gizmoProjection(const Camera& camera, float aspect) {
    return glm::perspectiveRH_ZO(camera.fovYRadians, aspect, camera.nearPlane,
                                 1000.0f);
}

} // namespace

EditorApp::EditorApp(Window& window, Renderer& renderer, AssetRegistry& assets,
                     std::filesystem::path assetDir,
                     std::filesystem::path scenePath)
    : m_window(window), m_renderer(renderer), m_assets(assets),
      m_assetDir(std::move(assetDir)), m_scenePath(std::move(scenePath)) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // The engine manages cursor capture for mouse-look; stop the backend
    // from fighting it.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

    Context& context = renderer.context();
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr,
                                    &m_imguiPool));

    const VkFormat swapchainFormat = renderer.swapchainFormat();
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = context.instance();
    initInfo.PhysicalDevice = context.physicalDevice();
    initInfo.Device = context.device();
    initInfo.QueueFamily = context.graphicsQueueFamily();
    initInfo.Queue = context.graphicsQueue();
    initInfo.DescriptorPool = m_imguiPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = renderer.swapchainImageCount();
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {};
    initInfo.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats =
        &swapchainFormat;
    ImGui_ImplVulkan_Init(&initInfo);

    m_input = InputActions::flyCameraDefaults();
    m_camera.position = {-7.0f, 1.8f, -0.5f};
    m_camera.yawRadians = glm::radians(-90.0f);
}

EditorApp::~EditorApp() {
    m_renderer.context().waitIdle();
    if (m_viewportTexture != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_viewportTexture);
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(m_renderer.context().device(), m_imguiPool,
                            nullptr);
}

void EditorApp::updateViewportTexture() {
    if (m_renderer.viewportGeneration() == m_viewportTextureGeneration ||
        m_renderer.viewportImageView() == VK_NULL_HANDLE) {
        return;
    }
    m_renderer.context().waitIdle();
    if (m_viewportTexture != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_viewportTexture);
    }
    m_viewportTexture = ImGui_ImplVulkan_AddTexture(
        m_renderer.bindless().clampSampler(), m_renderer.viewportImageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_viewportTextureGeneration = m_renderer.viewportGeneration();
}

entt::entity EditorApp::selectedEntity(World& world) {
    return findByEditorId(world, m_selected);
}

RenderOptions EditorApp::frame(World& world, float dt) {
    assignEditorIds(world);
    updateViewportTexture();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // First run (no imgui.ini): lay out the default workspace.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
        node != nullptr && node->IsLeafNode() && node->Windows.Size == 0) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId,
                                      ImGui::GetMainViewport()->Size);
        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Left, 0.16f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right,
                                                    0.24f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Down, 0.22f, nullptr, &center);
        const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(
            right, ImGuiDir_Down, 0.45f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Scene Settings", rightBottom);
        ImGui::DockBuilderDockWindow("Content", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    drawMenuBar(world);
    drawViewport(world);
    drawHierarchy(world);
    drawInspector(world);
    drawContentBrowser(world);
    drawSceneSettings(world);
    handleShortcuts(world);

    // Camera: fly with RMB over the viewport; orbit with Alt+LMB.
    const bool flying = m_viewportHovered &&
                        m_input.isDown(m_window, "look");
    if (flying) {
        m_camera.update(m_window, m_input, dt);
    } else {
        m_window.setCursorCaptured(false);
        const bool orbiting = m_viewportHovered && ImGui::GetIO().KeyAlt &&
                              ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        if (orbiting) {
            const glm::vec2 delta = m_window.consumeMouseDelta();
            m_camera.yawRadians -= delta.x * m_camera.lookSensitivity;
            m_camera.pitchRadians = std::clamp(
                m_camera.pitchRadians - delta.y * m_camera.lookSensitivity,
                glm::radians(-89.0f), glm::radians(89.0f));
            // Reposition so the pivot stays fixed in view.
            const glm::vec3 forward = glm::normalize(glm::vec3(
                glm::inverse(m_camera.view())[2]) * -1.0f);
            const glm::vec3 pivot =
                m_camera.position + forward * m_orbitDistance;
            const glm::mat4 view = m_camera.view();
            const glm::vec3 newForward =
                glm::normalize(glm::vec3(glm::inverse(view)[2]) * -1.0f);
            m_camera.position = pivot - newForward * m_orbitDistance;
        } else {
            m_window.consumeMouseDelta();
        }
    }

    ImGui::Render();

    RenderOptions options;
    options.viewportExtent = m_viewportExtent;
    options.debugView = m_debugView;
    options.pickPixel = m_pendingPick;
    m_pendingPick.reset();
    options.recordUI = [](VkCommandBuffer cmd) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    };
    return options;
}

void EditorApp::drawMenuBar(World& world) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_playing)) {
            saveScene(world);
        }
        if (ImGui::MenuItem("Reload Scene", nullptr, false, !m_playing)) {
            SceneSerializer::load(world, m_assets, m_scenePath);
            m_commands.clear();
            m_selected = 0;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undoText =
            std::string("Undo ") + m_commands.undoLabel();
        const std::string redoText =
            std::string("Redo ") + m_commands.redoLabel();
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false,
                            m_commands.canUndo() && !m_playing)) {
            m_commands.undo(world);
        }
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false,
                            m_commands.canRedo() && !m_playing)) {
            m_commands.redo(world);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        static const char* kViews[] = {"Final", "Albedo",   "Normals",
                                       "Metal/Rough", "AO", "Cascades",
                                       "RT Reflections"};
        for (int i = 0; i < 7; ++i) {
            if (ImGui::MenuItem(kViews[i], nullptr,
                                static_cast<int>(m_debugView) == i)) {
                m_debugView = static_cast<DebugView>(i);
            }
        }
        ImGui::Separator();
        ImGui::MenuItem("Light Gizmos", nullptr, &m_showLightGizmos);
        ImGui::EndMenu();
    }
    if (m_playing ? ImGui::MenuItem("[ Stop ]") : ImGui::MenuItem("[ Play ]")) {
        m_playing ? stopPlay(world) : startPlay(world);
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s%s", m_scenePath.filename().string().c_str(),
                        m_playing ? "  (playing)" : "");
    ImGui::EndMainMenuBar();
}

void EditorApp::drawViewport(World& world) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    m_viewportHovered = ImGui::IsWindowHovered();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    m_viewportExtent = {
        static_cast<uint32_t>((std::max)(avail.x, 64.0f)),
        static_cast<uint32_t>((std::max)(avail.y, 64.0f))};

    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    m_viewportImagePos = {imagePos.x, imagePos.y};

    // Skip the image on frames where the renderer will recreate it (panel
    // resize) — drawing the stale descriptor after the view is destroyed is
    // a validation error. The texture re-registers next frame.
    const VkExtent2D imageExtent = m_renderer.viewportImageExtent();
    const bool textureValid =
        m_viewportTexture != VK_NULL_HANDLE &&
        m_viewportTextureGeneration == m_renderer.viewportGeneration() &&
        m_viewportExtent.width == imageExtent.width &&
        m_viewportExtent.height == imageExtent.height;
    if (textureValid) {
        ImGui::Image(reinterpret_cast<ImTextureID>(m_viewportTexture), avail);
    } else {
        ImGui::Dummy(avail);
    }

    // Content-browser models can be dropped straight into the scene.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("CANDELA_ASSET")) {
            AssetGuid guid;
            std::memcpy(&guid, payload->Data, sizeof(guid));
            m_commands.perform(world, std::make_unique<InstantiateModelCommand>(
                                          guid, m_assets));
        }
        ImGui::EndDragDropTarget();
    }

    // Gizmo for the selected entity.
    bool gizmoActive = false;
    const entt::entity selected = selectedEntity(world);
    if (selected != entt::null && !m_playing &&
        world.registry.all_of<LocalTransform, WorldTransform>(selected)) {
        const float aspect = static_cast<float>(m_viewportExtent.width) /
                             static_cast<float>(m_viewportExtent.height);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imagePos.x, imagePos.y, avail.x, avail.y);

        const glm::mat4 view = m_camera.view();
        const glm::mat4 projection = gizmoProjection(m_camera, aspect);
        glm::mat4 worldMatrix =
            world.registry.get<WorldTransform>(selected).value;

        static const ImGuizmo::OPERATION kOps[] = {
            ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                             kOps[m_gizmoOperation], ImGuizmo::LOCAL,
                             glm::value_ptr(worldMatrix));
        gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

        if (ImGuizmo::IsUsing()) {
            if (!m_gizmoWasUsing) {
                m_gizmoBefore = world.registry.get<LocalTransform>(selected);
                m_gizmoWasUsing = true;
            }
            // World matrix → local TRS (relative to the parent if any).
            glm::mat4 localMatrix = worldMatrix;
            if (const auto* parent = world.registry.try_get<Parent>(selected);
                parent != nullptr && world.registry.valid(parent->value)) {
                const glm::mat4 parentWorld =
                    world.registry.get<WorldTransform>(parent->value).value;
                localMatrix = glm::inverse(parentWorld) * worldMatrix;
            }
            float translation[3];
            float rotation[3];
            float scale[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(localMatrix),
                                                  translation, rotation, scale);
            auto& local = world.registry.get<LocalTransform>(selected);
            local.translation = {translation[0], translation[1],
                                 translation[2]};
            local.rotation = glm::quat(glm::radians(
                glm::vec3(rotation[0], rotation[1], rotation[2])));
            local.scale = {scale[0], scale[1], scale[2]};
        } else if (m_gizmoWasUsing) {
            m_gizmoWasUsing = false;
            m_commands.perform(
                world, std::make_unique<TransformCommand>(
                           m_selected, m_gizmoBefore,
                           world.registry.get<LocalTransform>(selected)));
        }
    }

    drawLightGizmos(world, {imagePos.x, imagePos.y}, {avail.x, avail.y});

    // Click picking (when the gizmo isn't involved).
    if (m_viewportHovered && !gizmoActive && !ImGui::GetIO().KeyAlt &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        m_pendingPick = glm::ivec2(
            static_cast<int>(mouse.x - imagePos.x),
            static_cast<int>(mouse.y - imagePos.y));
    }
    if (auto picked = m_renderer.takePickResult()) {
        if (*picked == 0) {
            m_selected = 0;
        } else {
            const auto entity = static_cast<entt::entity>(*picked - 1);
            if (world.registry.valid(entity)) {
                m_selected = editorIdOf(world, entity);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorApp::drawLightGizmos(World& world, const glm::vec2& imagePos,
                                const glm::vec2& imageSize) {
    if (!m_showLightGizmos || m_playing || imageSize.x < 1.0f ||
        imageSize.y < 1.0f) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(
        ImVec2(imagePos.x, imagePos.y),
        ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y), true);

    const float aspect = imageSize.x / imageSize.y;
    const glm::mat4 viewProjection = m_camera.viewProjection(aspect);

    // World → viewport pixels under the negative-viewport convention
    // (matches ndcToUV in the shaders). False when behind the camera.
    auto project = [&](const glm::vec3& world_, ImVec2& out) {
        const glm::vec4 clip = viewProjection * glm::vec4(world_, 1.0f);
        if (clip.w < 1e-4f) {
            return false;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = ImVec2(imagePos.x + (ndc.x * 0.5f + 0.5f) * imageSize.x,
                     imagePos.y + (0.5f - ndc.y * 0.5f) * imageSize.y);
        return true;
    };
    auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 color,
                    float thickness) {
        ImVec2 screenA;
        ImVec2 screenB;
        if (project(a, screenA) && project(b, screenB)) {
            drawList->AddLine(screenA, screenB, color, thickness);
        }
    };

    // Point lights: a wireframe sphere showing position and falloff radius.
    // (Point lights are omnidirectional — direction arrows belong to the sun.)
    constexpr int kSegments = 40;
    constexpr float kTau = 6.28318530f;
    static const glm::vec3 kCircleBases[3][2] = {
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    };
    for (auto [entity, transform, light] :
         world.registry.view<WorldTransform, PointLightComponent>().each()) {
        const glm::vec3 center = glm::vec3(transform.value[3]);
        const bool isSelected = editorIdOf(world, entity) == m_selected;
        const float alpha = isSelected ? 0.95f : 0.4f;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(light.color.r, light.color.g, light.color.b, alpha));
        const float thickness = isSelected ? 2.0f : 1.2f;

        for (const auto& basis : kCircleBases) {
            glm::vec3 previous = center + basis[0] * light.radius;
            for (int i = 1; i <= kSegments; ++i) {
                const float angle = kTau * static_cast<float>(i) / kSegments;
                const glm::vec3 point =
                    center + (basis[0] * std::cos(angle) +
                              basis[1] * std::sin(angle)) *
                                 light.radius;
                line(previous, point, color, thickness);
                previous = point;
            }
        }

        ImVec2 screenCenter;
        if (project(center, screenCenter)) {
            const ImU32 solid = ImGui::ColorConvertFloat4ToU32(
                ImVec4(light.color.r, light.color.g, light.color.b, 1.0f));
            drawList->AddCircleFilled(screenCenter, isSelected ? 5.0f : 3.5f,
                                      solid);
            drawList->AddCircle(screenCenter, isSelected ? 7.0f : 5.0f,
                                IM_COL32(255, 255, 255, 160));
        }
    }

    // Sun: a direction arrow anchored above the world origin, pointing the
    // way the light travels.
    {
        const SceneSettings& settings = world.settings;
        const glm::vec3 direction = -glm::normalize(settings.toSun); // travel
        const glm::vec3 tip{0.0f, 1.5f, 0.0f};
        const glm::vec3 tail = tip - direction * 3.0f;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(settings.sunColor.r, settings.sunColor.g,
                   settings.sunColor.b, 0.9f));

        line(tail, tip, color, 2.5f);
        // Arrowhead: four fins offset perpendicular to the shaft.
        const glm::vec3 up = std::abs(direction.y) > 0.95f
                                 ? glm::vec3(1.0f, 0.0f, 0.0f)
                                 : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 side = glm::normalize(glm::cross(direction, up));
        const glm::vec3 side2 = glm::normalize(glm::cross(direction, side));
        const glm::vec3 back = tip - direction * 0.5f;
        for (const glm::vec3& fin :
             {side * 0.18f, side * -0.18f, side2 * 0.18f, side2 * -0.18f}) {
            line(tip, back + fin, color, 2.0f);
        }
        // A small sun disc at the tail.
        ImVec2 screenTail;
        if (project(tail, screenTail)) {
            drawList->AddCircle(screenTail, 8.0f, color, 0, 2.0f);
            drawList->AddCircleFilled(screenTail, 3.0f, color);
        }
    }

    drawList->PopClipRect();
}

void EditorApp::drawEntityNode(World& world, entt::entity entity) {
    auto& registry = world.registry;
    const uint64_t id = editorIdOf(world, entity);

    std::vector<entt::entity> children;
    for (auto [child, parent] : registry.view<Parent>().each()) {
        if (parent.value == entity) {
            children.push_back(child);
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (id == m_selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string& name = registry.get<Name>(entity).value;
    const bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<uintptr_t>(id)), flags, "%s",
        name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_selected = id;
    }

    if (!m_playing && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("CANDELA_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    if (!m_playing && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("CANDELA_ENTITY")) {
            uint64_t draggedId;
            std::memcpy(&draggedId, payload->Data, sizeof(draggedId));
            if (draggedId != id) {
                const entt::entity dragged = findByEditorId(world, draggedId);
                uint64_t oldParent = 0;
                if (const auto* parent = registry.try_get<Parent>(dragged);
                    parent != nullptr && registry.valid(parent->value)) {
                    oldParent = editorIdOf(world, parent->value);
                }
                m_commands.perform(world, std::make_unique<ReparentCommand>(
                                              draggedId, oldParent, id));
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (open) {
        for (const entt::entity child : children) {
            drawEntityNode(world, child);
        }
        ImGui::TreePop();
    }
}

void EditorApp::drawHierarchy(World& world) {
    ImGui::Begin("Hierarchy");

    if (!m_playing) {
        if (ImGui::Button("+ Empty")) {
            EntitySnapshot snapshot;
            snapshot.editorId = reserveEditorId();
            snapshot.name = "Entity";
            m_commands.perform(world,
                               std::make_unique<CreateEntityCommand>(snapshot));
            m_selected = snapshot.editorId;
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Light")) {
            EntitySnapshot snapshot;
            snapshot.editorId = reserveEditorId();
            snapshot.name = "Point Light";
            snapshot.transform.translation = m_camera.position;
            snapshot.pointLight = PointLightComponent{};
            m_commands.perform(world,
                               std::make_unique<CreateEntityCommand>(snapshot));
            m_selected = snapshot.editorId;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selected == 0);
        if (ImGui::Button("Delete")) {
            deleteSelected(world);
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    // Roots: entities without a valid parent.
    std::vector<entt::entity> roots;
    for (const entt::entity entity : world.registry.view<Name>()) {
        const auto* parent = world.registry.try_get<Parent>(entity);
        if (parent == nullptr || !world.registry.valid(parent->value)) {
            roots.push_back(entity);
        }
    }
    std::reverse(roots.begin(), roots.end()); // creation order
    for (const entt::entity root : roots) {
        drawEntityNode(world, root);
    }

    ImGui::End();
}

void EditorApp::drawInspector(World& world) {
    ImGui::Begin("Inspector");
    const entt::entity entity = selectedEntity(world);
    if (entity == entt::null) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(m_playing);
    auto& registry = world.registry;

    // Name.
    auto& name = registry.get<Name>(entity);
    std::strncpy(m_nameBuffer, name.value.c_str(), sizeof(m_nameBuffer) - 1);
    if (ImGui::InputText("Name", m_nameBuffer, sizeof(m_nameBuffer))) {
        name.value = m_nameBuffer;
    }
    if (ImGui::IsItemActivated()) {
        m_nameBefore = name.value;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_nameBefore != name.value) {
        m_commands.perform(world, std::make_unique<RenameCommand>(
                                      m_selected, m_nameBefore, name.value));
    }

    // Transform.
    if (ImGui::CollapsingHeader("Transform",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& local = registry.get<LocalTransform>(entity);
        bool activated = false;
        bool released = false;
        auto track = [&] {
            activated |= ImGui::IsItemActivated();
            released |= ImGui::IsItemDeactivatedAfterEdit();
        };
        ImGui::DragFloat3("Translation",
                          glm::value_ptr(local.translation), 0.05f);
        track();
        glm::vec3 euler = glm::degrees(glm::eulerAngles(local.rotation));
        if (ImGui::DragFloat3("Rotation", glm::value_ptr(euler), 0.5f)) {
            local.rotation = glm::quat(glm::radians(euler));
        }
        track();
        ImGui::DragFloat3("Scale", glm::value_ptr(local.scale), 0.02f);
        track();
        if (activated) {
            m_transformBefore = local;
        }
        if (released) {
            m_commands.perform(world, std::make_unique<TransformCommand>(
                                          m_selected, m_transformBefore,
                                          local));
        }
    }

    // Point light.
    if (auto* light = registry.try_get<PointLightComponent>(entity)) {
        if (ImGui::CollapsingHeader("Point Light",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            bool activated = false;
            bool released = false;
            auto track = [&] {
                activated |= ImGui::IsItemActivated();
                released |= ImGui::IsItemDeactivatedAfterEdit();
            };
            ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            track();
            ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.0f,
                             100.0f);
            track();
            ImGui::DragFloat("Radius", &light->radius, 0.1f, 0.1f, 100.0f);
            track();
            if (activated) {
                m_lightBefore = *light;
            }
            if (released) {
                m_commands.perform(
                    world,
                    std::make_unique<ComponentEditCommand<PointLightComponent>>(
                        m_selected, m_lightBefore, *light));
            }
            if (ImGui::Button("Remove Light")) {
                m_commands.perform(
                    world,
                    std::make_unique<RemoveComponentCommand<PointLightComponent>>(
                        m_selected, *light));
            }
        }
    }

    // Camera.
    if (auto* cameraComponent = registry.try_get<CameraComponent>(entity)) {
        if (ImGui::CollapsingHeader("Camera")) {
            bool activated = false;
            bool released = false;
            ImGui::DragFloat("FOV", &cameraComponent->fovYDegrees, 0.2f, 10.0f,
                             140.0f);
            activated |= ImGui::IsItemActivated();
            released |= ImGui::IsItemDeactivatedAfterEdit();
            if (activated) {
                m_cameraBefore = *cameraComponent;
            }
            if (released) {
                m_commands.perform(
                    world,
                    std::make_unique<ComponentEditCommand<CameraComponent>>(
                        m_selected, m_cameraBefore, *cameraComponent));
            }
        }
    }

    // Mesh renderer (read-only — material editing is a later phase).
    if (const auto* mesh = registry.try_get<MeshRenderer>(entity)) {
        if (ImGui::CollapsingHeader("Mesh Renderer")) {
            ImGui::Text("Model: %s",
                        m_assets.pathForGuid(mesh->model)
                            .filename()
                            .string()
                            .c_str());
            ImGui::Text("Mesh index: %u", mesh->meshIndex);
        }
    }

    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("add_component");
    }
    if (ImGui::BeginPopup("add_component")) {
        if (!registry.all_of<PointLightComponent>(entity) &&
            ImGui::MenuItem("Point Light")) {
            m_commands.perform(
                world,
                std::make_unique<AddComponentCommand<PointLightComponent>>(
                    m_selected, PointLightComponent{}));
        }
        if (!registry.all_of<CameraComponent>(entity) &&
            ImGui::MenuItem("Camera")) {
            m_commands.perform(
                world, std::make_unique<AddComponentCommand<CameraComponent>>(
                           m_selected, CameraComponent{}));
        }
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
    ImGui::End();
}

void EditorApp::drawContentBrowser(World& world) {
    ImGui::Begin("Content");
    for (const auto& [guid, path] : m_assets.allAssets()) {
        ImGui::PushID(static_cast<int>(guid & 0x7FFFFFFF));
        ImGui::Selectable(path.filename().string().c_str());
        if (!m_playing && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("CANDELA_ASSET", &guid, sizeof(guid));
            ImGui::TextUnformatted(path.filename().string().c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_playing);
        if (ImGui::SmallButton("Add to Scene")) {
            m_commands.perform(world, std::make_unique<InstantiateModelCommand>(
                                          guid, m_assets));
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::End();
}

void EditorApp::drawSceneSettings(World& world) {
    ImGui::Begin("Scene Settings");
    ImGui::BeginDisabled(m_playing);
    SceneSettings& settings = world.settings;
    bool activated = false;
    bool released = false;
    auto track = [&] {
        activated |= ImGui::IsItemActivated();
        released |= ImGui::IsItemDeactivatedAfterEdit();
    };
    ImGui::DragFloat3("To Sun", glm::value_ptr(settings.toSun), 0.01f);
    track();
    ImGui::DragFloat("Sun Intensity", &settings.sunIntensity, 0.05f, 0.0f,
                     100.0f);
    track();
    ImGui::ColorEdit3("Sun Color", glm::value_ptr(settings.sunColor));
    track();
    ImGui::DragFloat("IBL Intensity", &settings.iblIntensity, 0.02f, 0.0f,
                     10.0f);
    track();
    ImGui::DragFloat("Exposure", &settings.exposure, 0.02f, 0.0f, 10.0f);
    track();
    ImGui::DragFloat("Bloom", &settings.bloomStrength, 0.005f, 0.0f, 1.0f);
    track();
    if (activated) {
        m_settingsBefore = settings;
    }
    if (released) {
        m_commands.perform(world, std::make_unique<SettingsCommand>(
                                      m_settingsBefore, settings));
    }

    // Checkboxes commit instantly — snapshot before the toggle.
    {
        const SceneSettings before = settings;
        if (ImGui::Checkbox("TAA", &settings.taa)) {
            m_commands.perform(world, std::make_unique<SettingsCommand>(
                                          before, settings));
        }
    }

    ImGui::SeparatorText("Ray Tracing");
    ImGui::BeginDisabled(!m_renderer.context().rayTracingSupported());
    // Checkboxes commit instantly — snapshot before each toggle.
    auto rtCheckbox = [&](const char* label, bool* value) {
        const SceneSettings before = settings;
        if (ImGui::Checkbox(label, value)) {
            m_commands.perform(world, std::make_unique<SettingsCommand>(
                                          before, settings));
        }
    };
    rtCheckbox("RT Shadows", &settings.rtShadows);
    rtCheckbox("RT Ambient Occlusion", &settings.rtAmbientOcclusion);
    rtCheckbox("RT Reflections", &settings.rtReflections);
    if (!m_renderer.context().rayTracingSupported()) {
        ImGui::TextDisabled("(no ray tracing support on this GPU)");
    }
    ImGui::EndDisabled();

    ImGui::EndDisabled();
    ImGui::End();
}

void EditorApp::handleShortcuts(World& world) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_playing) {
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        m_commands.undo(world);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        m_commands.redo(world);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        saveScene(world);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_selected != 0) {
        deleteSelected(world);
    }
    // Gizmo modes when interacting with the viewport (not flying).
    if (m_viewportHovered && !m_input.isDown(m_window, "look")) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOperation = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOperation = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOperation = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) focusSelected(world);
    }
}

void EditorApp::deleteSelected(World& world) {
    const entt::entity entity = selectedEntity(world);
    if (entity == entt::null) {
        return;
    }
    m_commands.perform(world, std::make_unique<DeleteEntityCommand>(
                                  snapshotEntity(world, entity)));
    m_selected = 0;
}

void EditorApp::focusSelected(World& world) {
    const entt::entity entity = selectedEntity(world);
    if (entity == entt::null ||
        !world.registry.all_of<WorldTransform>(entity)) {
        return;
    }
    const glm::vec3 target =
        glm::vec3(world.registry.get<WorldTransform>(entity).value[3]);
    const glm::vec3 forward =
        glm::normalize(glm::vec3(glm::inverse(m_camera.view())[2]) * -1.0f);
    m_orbitDistance = 4.0f;
    m_camera.position = target - forward * m_orbitDistance;
}

void EditorApp::startPlay(World& world) {
    m_playSnapshot = SceneSerializer::saveToString(world);
    m_playing = true;
    CD_INFO("Play mode: registry snapshot taken ({} bytes)",
            m_playSnapshot.size());
}

void EditorApp::stopPlay(World& world) {
    SceneSerializer::loadFromString(world, m_assets, m_playSnapshot);
    m_playSnapshot.clear();
    m_playing = false;
    // Entity handles and EditorIds were rebuilt — stale references would
    // dangle, so selection and undo history reset.
    m_selected = 0;
    m_commands.clear();
    assignEditorIds(world);
    CD_INFO("Play mode stopped: registry restored");
}

void EditorApp::saveScene(World& world) {
    SceneSerializer::save(world, m_scenePath);
}

} // namespace candela::editor
