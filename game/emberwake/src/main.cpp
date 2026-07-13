// Emberwake — a procedural action dungeon-crawler built on Candela.
//
// You are a wisp of flame descending a dark dungeon. Shades hunt you; your
// flame is your life — take hits and your light gutters. Fight down three
// seeded floors, break The Smother, and relight the Great Brazier.
//
// WASD moves (camera-relative), left mouse fires ember bolts at the cursor,
// hold right mouse to orbit the camera, R restarts, Escape quits.
//
// --seed N / --size WxH / --frames N / --screenshot <path> / --autoplay
// (a bot that fights its way down — the headless end-to-end gameplay test).
#include "Dungeon.h"

#include <candela/assets/AssetRegistry.h>
#include <candela/assets/ModelAsset.h>
#include <candela/core/Events.h>
#include <candela/core/Jobs.h>
#include <candela/core/Log.h>
#include <candela/platform/Input.h>
#include <candela/platform/Window.h>
#include <candela/renderer/Camera.h>
#include <candela/renderer/Renderer.h>
#include <candela/rhi/Context.h>
#include <candela/scene/World.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace candela;

// --- Tuning ---
constexpr int kDungeonWidth = 44;
constexpr int kDungeonHeight = 34;
constexpr int kDungeonRooms = 9;
constexpr int kFloorCount = 3;

constexpr float kWispRadius = 0.28f;
constexpr float kWispHeight = 0.55f;
constexpr float kAccel = 26.0f;
constexpr float kDamping = 7.5f;
constexpr float kMaxSpeed = 4.2f;
constexpr float kWallHeight = 1.5f;

constexpr float kMaxFlame = 100.0f;
constexpr float kIFrames = 0.8f;
constexpr float kBoltSpeed = 9.0f;
constexpr float kBoltRadius = 0.11f;
constexpr float kBoltDamage = 34.0f;
constexpr float kBoltLife = 2.2f;
constexpr float kShootCooldown = 0.28f;
constexpr float kDarkBoltSpeed = 6.0f;
constexpr float kDarkBoltDamage = 12.0f;

constexpr float kAggroRange = 9.0f;
constexpr float kCameraDistance = 7.5f;

// Lights sit on child entities above their emitting mesh — a light at the
// centre of its own geometry is fully self-shadowed under RT shadows.
constexpr float kWispLightLift = 0.42f;

enum class EnemyKind { Shade, Hollow, Smother };

struct Enemy {
    EnemyKind kind = EnemyKind::Shade;
    float hp = 100.0f;
    float maxHp = 100.0f;
    float speed = 2.7f;
    float radius = 0.34f;
    float touchDamage = 18.0f;
    bool aggro = false;
    float shootTimer = 0.0f;
    float repathTimer = 0.0f;
    float bobPhase = 0.0f;
    glm::vec2 velocity{0.0f};
    std::deque<glm::vec2> waypoints;
    // Boss add-summon thresholds (fraction of max hp), consumed in order.
    std::vector<float> summonAt;
};

enum class PickupKind { Ember, Shard };

struct Pickup {
    PickupKind kind = PickupKind::Ember;
    float bobPhase = 0.0f;
};

struct Bolt {
    bool hostile = false; // true: hurts the player; false: hurts enemies
    glm::vec2 velocity{0.0f};
    float life = kBoltLife;
};

// Fading one-shot light (enemy death flash). Fades over `life` seconds,
// then the entity is destroyed.
struct Flash {
    float life = 0.4f;
    float age = 0.0f;
    float peakIntensity = 6.0f;
};

struct Guids {
    AssetGuid wisp, shade, hollow, smother, bolt, pickups, brazier, rift,
        wall, floor;
};

struct MeshIds {
    uint32_t emberBolt = 0, darkBolt = 0, ember = 0, shard = 0,
             brazierUnlit = 0, brazierLit = 0;
};

uint32_t meshIndexByName(const ModelAsset& model, std::string_view name) {
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        if (model.meshes[i].name == name) {
            return static_cast<uint32_t>(i);
        }
    }
    CD_ASSERT(false, "Mesh '{}' missing from model", name);
    return 0;
}

glm::vec3 lookDirection(float yaw, float pitch) {
    const float cosPitch = std::cos(pitch);
    return {-std::sin(yaw) * cosPitch, std::sin(pitch),
            -std::cos(yaw) * cosPitch};
}

// Cursor ray intersected with the wisp's hover plane. The engine renders
// with a negative viewport (GL-style NDC, +Y up-screen); depth 1 is the near
// plane under reverse-Z.
glm::vec2 aimPointOnPlane(const Camera& camera, float aspect, glm::vec2 mouse,
                          glm::vec2 windowSize, float planeY) {
    const glm::vec2 ndc{2.0f * mouse.x / windowSize.x - 1.0f,
                        1.0f - 2.0f * mouse.y / windowSize.y};
    const glm::mat4 invViewProj =
        glm::inverse(camera.projection(aspect) * camera.view());
    const glm::vec4 nearPoint = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);
    const glm::vec3 onNear = glm::vec3(nearPoint) / nearPoint.w;
    glm::vec3 dir = onNear - camera.position;
    if (std::abs(dir.y) < 1e-5f) {
        dir.y = -1e-5f;
    }
    const float t = (planeY - camera.position.y) / dir.y;
    const glm::vec3 hit = camera.position + dir * std::max(t, 0.0f);
    return {hit.x, hit.z};
}

SceneSettings dungeonSettings() {
    SceneSettings settings;
    settings.toSun = glm::normalize(glm::vec3(0.4f, 0.75f, 0.3f));
    settings.sunIntensity = 0.05f; // near-black depths; the wisp carries light
    settings.sunColor = {0.5f, 0.6f, 1.0f};
    settings.iblIntensity = 0.035f;
    settings.exposure = 1.35f;
    settings.bloomStrength = 0.11f;
    return settings;
}

std::string formatTime(float seconds) {
    const int whole = static_cast<int>(seconds);
    return std::format("{:02}:{:02}", whole / 60, whole % 60);
}

// Minimal ImGui bring-up against the engine's dynamic-rendering backbuffer
// pass (mirrors the editor's, minus docking and theming).
class GameUi {
public:
    GameUi(Window& window, Renderer& renderer) : m_renderer(renderer) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // a HUD has no layout to persist
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        const std::filesystem::path fontPath =
            std::filesystem::path(CANDELA_IMGUI_FONT_DIR) /
            "Roboto-Medium.ttf";
        if (std::filesystem::exists(fontPath)) {
            io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 18.0f);
        }
        ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

        Context& context = renderer.context();
        VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 16;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr,
                                        &m_pool));

        const VkFormat swapchainFormat = renderer.swapchainFormat();
        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = context.instance();
        initInfo.PhysicalDevice = context.physicalDevice();
        initInfo.Device = context.device();
        initInfo.QueueFamily = context.graphicsQueueFamily();
        initInfo.Queue = context.graphicsQueue();
        initInfo.DescriptorPool = m_pool;
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
    }

    ~GameUi() {
        m_renderer.context().waitIdle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(m_renderer.context().device(), m_pool,
                                nullptr);
    }

    GameUi(const GameUi&) = delete;
    GameUi& operator=(const GameUi&) = delete;

    void beginFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

private:
    Renderer& m_renderer;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

// Fading centre-screen announcements ("You descend...", boss callouts).
struct Messages {
    struct Entry {
        std::string text;
        float age = 0.0f;
    };
    std::vector<Entry> entries;

    void post(std::string text) { entries.push_back({std::move(text), 0.0f}); }

    void update(float dt) {
        for (Entry& entry : entries) {
            entry.age += dt;
        }
        std::erase_if(entries,
                      [](const Entry& entry) { return entry.age > 3.0f; });
    }

    void draw(glm::vec2 windowSize) const {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        float y = windowSize.y * 0.22f;
        for (const Entry& entry : entries) {
            const float alpha =
                std::clamp(1.0f - (entry.age - 2.0f) / 1.0f, 0.0f, 1.0f);
            const ImVec2 size =
                ImGui::CalcTextSize(entry.text.c_str());
            drawList->AddText(
                ImVec2((windowSize.x - size.x) * 0.5f, y),
                ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.6f, alpha)),
                entry.text.c_str());
            y += size.y + 6.0f;
        }
    }
};

enum class GameState { Title, Playing, Dead, Victory };

// Everything that describes one run in progress.
struct Run {
    int floor = 1;
    float flame = kMaxFlame;
    float iframes = 0.0f;
    float shootCooldown = 0.0f;
    int shards = 0;
    int kills = 0;
    float time = 0.0f;
    glm::vec2 wispPos{0.0f};
    glm::vec2 wispVel{0.0f};
    bool bossDead = false;
    bool brazierLit = false;
};

struct FloorEntities {
    entt::entity wisp = entt::null;
    entt::entity wispLight = entt::null;
    entt::entity exitMarker = entt::null; // rift, or the brazier on floor 3
    entt::entity boss = entt::null;
};

// Spawns one enemy hovering at a cell.
entt::entity spawnEnemy(World& world, const Guids& guids, EnemyKind kind,
                        glm::vec2 pos, float bobPhase) {
    const entt::entity entity = world.createEntity(
        kind == EnemyKind::Shade    ? "shade"
        : kind == EnemyKind::Hollow ? "hollow"
                                    : "smother");
    auto& transform = world.registry.get<LocalTransform>(entity);
    transform.translation = {pos.x, 0.15f, pos.y};
    Enemy enemy;
    enemy.kind = kind;
    enemy.bobPhase = bobPhase;
    switch (kind) {
    case EnemyKind::Shade:
        enemy.maxHp = 100.0f;
        enemy.speed = 2.7f;
        enemy.radius = 0.34f;
        enemy.touchDamage = 18.0f;
        world.registry.emplace<MeshRenderer>(entity, guids.shade, 0u);
        break;
    case EnemyKind::Hollow:
        enemy.maxHp = 68.0f;
        enemy.speed = 2.2f;
        enemy.radius = 0.30f;
        enemy.touchDamage = 10.0f;
        world.registry.emplace<MeshRenderer>(entity, guids.hollow, 0u);
        break;
    case EnemyKind::Smother:
        enemy.maxHp = 700.0f;
        enemy.speed = 1.7f;
        enemy.radius = 0.85f;
        enemy.touchDamage = 30.0f;
        enemy.summonAt = {2.0f / 3.0f, 1.0f / 3.0f};
        world.registry.emplace<MeshRenderer>(entity, guids.smother, 0u);
        break;
    }
    enemy.hp = enemy.maxHp;
    world.registry.emplace<Enemy>(entity, enemy);

    // A faint core glow so threats read in the dark (and telegraph their
    // kind by colour). Lifted above the mesh like every other light.
    const entt::entity glow = world.createEntity("enemy_glow");
    const bool boss = kind == EnemyKind::Smother;
    world.registry.get<LocalTransform>(glow).translation = {
        0.0f, boss ? 1.4f : 0.75f, 0.0f};
    world.setParent(glow, entity);
    auto& light = world.registry.emplace<PointLightComponent>(glow);
    light.color = kind == EnemyKind::Shade    ? glm::vec3(0.55f, 0.3f, 1.0f)
                  : kind == EnemyKind::Hollow ? glm::vec3(0.2f, 0.8f, 0.95f)
                                              : glm::vec3(1.0f, 0.2f, 0.12f);
    light.intensity = boss ? 2.2f : 0.9f;
    light.radius = boss ? 5.0f : 2.8f;
    return entity;
}

// Builds all entities for a floor into a cleared registry.
FloorEntities buildFloor(World& world, const emberwake::Dungeon& dungeon,
                         const Guids& guids, const MeshIds& meshes, int floor,
                         uint64_t seed, glm::vec2 wispPos) {
    FloorEntities out;
    world.registry.clear();
    world.settings = dungeonSettings();

    // Floor slab.
    {
        const entt::entity slab = world.createEntity("floor");
        auto& t = world.registry.get<LocalTransform>(slab);
        t.translation = {static_cast<float>(dungeon.width()) * 0.5f, 0.0f,
                         static_cast<float>(dungeon.height()) * 0.5f};
        t.scale = {static_cast<float>(dungeon.width()), 1.0f,
                   static_cast<float>(dungeon.height())};
        world.registry.emplace<MeshRenderer>(slab, guids.floor, 0u);
    }

    // Walls: only solid cells that border open space.
    uint32_t wallCount = 0;
    for (int z = 0; z < dungeon.height(); ++z) {
        for (int x = 0; x < dungeon.width(); ++x) {
            if (!dungeon.solid(x, z)) {
                continue;
            }
            bool exposed = false;
            for (int dz = -1; dz <= 1 && !exposed; ++dz) {
                for (int dx = -1; dx <= 1 && !exposed; ++dx) {
                    exposed = !dungeon.solid(x + dx, z + dz);
                }
            }
            if (!exposed) {
                continue;
            }
            const entt::entity wall =
                world.createEntity(std::format("wall_{}_{}", x, z));
            auto& t = world.registry.get<LocalTransform>(wall);
            t.translation = {static_cast<float>(x) + 0.5f, kWallHeight * 0.5f,
                             static_cast<float>(z) + 0.5f};
            t.scale = {1.0f, kWallHeight, 1.0f};
            world.registry.emplace<MeshRenderer>(wall, guids.wall, 0u);
            ++wallCount;
        }
    }

    // Exit: rift on floors 1..N-1, the Great Brazier on the last floor.
    const glm::vec2 exitPos =
        emberwake::Dungeon::cellCenter(dungeon.stairs());
    if (floor < kFloorCount) {
        const entt::entity rift = world.createEntity("rift");
        world.registry.get<LocalTransform>(rift).translation = {
            exitPos.x, 0.0f, exitPos.y};
        world.registry.emplace<MeshRenderer>(rift, guids.rift, 0u);
        const entt::entity riftLight = world.createEntity("rift_light");
        world.registry.get<LocalTransform>(riftLight).translation = {
            0.0f, 0.6f, 0.0f};
        world.setParent(riftLight, rift);
        auto& light = world.registry.emplace<PointLightComponent>(riftLight);
        light.color = {0.55f, 0.25f, 1.0f};
        light.intensity = 1.4f;
        light.radius = 4.5f;
        out.exitMarker = rift;
    } else {
        const entt::entity brazier = world.createEntity("brazier");
        world.registry.get<LocalTransform>(brazier).translation = {
            exitPos.x, 0.0f, exitPos.y};
        world.registry.emplace<MeshRenderer>(brazier, guids.brazier,
                                             meshes.brazierUnlit);
        out.exitMarker = brazier;

        // The Smother guards it, parked between brazier and the approach.
        const glm::vec2 bossPos = exitPos + glm::vec2(0.0f, -2.0f);
        out.boss = spawnEnemy(world, guids, EnemyKind::Smother,
                              dungeon.resolveCollision(bossPos, 0.85f), 0.0f);
    }

    // Enemies and pickups, deterministically scattered per floor.
    emberwake::Rng rng(seed * 977u + static_cast<uint64_t>(floor) * 131u);
    const int shadeCount = 3 + floor * 2;      // 5 / 7 / 9
    const int hollowCount = floor - 1;         // 0 / 1 / 2
    const int emberCount = 3;
    const int shardCount = 6;
    const auto enemyCells = dungeon.scatter(
        rng, shadeCount + hollowCount, 8);
    for (size_t i = 0; i < enemyCells.size(); ++i) {
        const EnemyKind kind = static_cast<int>(i) < shadeCount
                                   ? EnemyKind::Shade
                                   : EnemyKind::Hollow;
        spawnEnemy(world, guids, kind,
                   emberwake::Dungeon::cellCenter(enemyCells[i]),
                   rng.unit() * 6.28f);
    }
    const auto pickupCells =
        dungeon.scatter(rng, emberCount + shardCount, 4);
    for (size_t i = 0; i < pickupCells.size(); ++i) {
        const bool isEmber = static_cast<int>(i) < emberCount;
        const entt::entity pickup =
            world.createEntity(isEmber ? "ember" : "shard");
        const glm::vec2 pos =
            emberwake::Dungeon::cellCenter(pickupCells[i]);
        world.registry.get<LocalTransform>(pickup).translation = {
            pos.x, 0.35f, pos.y};
        world.registry.emplace<MeshRenderer>(
            pickup, guids.pickups, isEmber ? meshes.ember : meshes.shard);
        Pickup component;
        component.kind = isEmber ? PickupKind::Ember : PickupKind::Shard;
        component.bobPhase = rng.unit() * 6.28f;
        world.registry.emplace<Pickup>(pickup, component);
    }

    // The wisp and its lifted light.
    out.wisp = world.createEntity("wisp");
    world.registry.get<LocalTransform>(out.wisp).translation = {
        wispPos.x, kWispHeight, wispPos.y};
    world.registry.emplace<MeshRenderer>(out.wisp, guids.wisp, 0u);
    out.wispLight = world.createEntity("wisp_light");
    world.registry.get<LocalTransform>(out.wispLight).translation = {
        0.0f, kWispLightLift, 0.0f};
    world.setParent(out.wispLight, out.wisp);
    auto& light = world.registry.emplace<PointLightComponent>(out.wispLight);
    light.color = {1.0f, 0.72f, 0.4f};
    light.intensity = 2.6f;
    light.radius = 8.0f;

    CD_INFO("Floor {}: {} walls, {} enemies, {} pickups (seed {})", floor,
            wallCount, enemyCells.size(), pickupCells.size(), seed);
    return out;
}

entt::entity spawnBolt(World& world, const Guids& guids, const MeshIds& meshes,
                       bool hostile, glm::vec2 from, glm::vec2 direction) {
    const entt::entity entity =
        world.createEntity(hostile ? "dark_bolt" : "ember_bolt");
    world.registry.get<LocalTransform>(entity).translation = {
        from.x, kWispHeight, from.y};
    world.registry.emplace<MeshRenderer>(
        entity, guids.bolt, hostile ? meshes.darkBolt : meshes.emberBolt);
    Bolt bolt;
    bolt.hostile = hostile;
    bolt.velocity =
        glm::normalize(direction) * (hostile ? kDarkBoltSpeed : kBoltSpeed);
    world.registry.emplace<Bolt>(entity, bolt);
    // Player bolts carry a small lifted light — the projectile IS a light
    // source sweeping the dungeon. Hostile bolts stay emissive-only (a
    // swarm of shadow-casting lights would drown the light budget).
    if (!hostile) {
        const entt::entity boltLight = world.createEntity("bolt_light");
        world.registry.get<LocalTransform>(boltLight).translation = {
            0.0f, 0.3f, 0.0f};
        world.setParent(boltLight, entity);
        auto& light = world.registry.emplace<PointLightComponent>(boltLight);
        light.color = {1.0f, 0.6f, 0.25f};
        light.intensity = 1.8f;
        light.radius = 4.0f;
    }
    return entity;
}

void spawnFlash(World& world, glm::vec2 pos, glm::vec3 color,
                float intensity) {
    const entt::entity entity = world.createEntity("flash");
    world.registry.get<LocalTransform>(entity).translation = {pos.x, 0.8f,
                                                              pos.y};
    auto& light = world.registry.emplace<PointLightComponent>(entity);
    light.color = color;
    light.intensity = intensity;
    light.radius = 6.0f;
    Flash flash;
    flash.peakIntensity = intensity;
    world.registry.emplace<Flash>(entity, flash);
}

// Recursively destroys an entity and any children parented to it.
void destroyWithChildren(World& world, entt::entity entity) {
    std::vector<entt::entity> children;
    for (const auto [child, parent] : world.registry.view<Parent>().each()) {
        if (parent.value == entity) {
            children.push_back(child);
        }
    }
    for (const entt::entity child : children) {
        destroyWithChildren(world, child);
    }
    world.registry.destroy(entity);
}

} // namespace

int main(int argc, char** argv) {
    Log::init();
    CD_INFO("Emberwake — descend, burn, relight");

    uint64_t maxFrames = 0;
    std::filesystem::path screenshotPath;
    bool botMode = false;
    bool combatShot = false; // aim --screenshot at the first firefight
    uint64_t seed = 7;
    uint32_t windowWidth = 1600;
    uint32_t windowHeight = 900;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--autoplay") == 0) {
            botMode = true;
        } else if (std::strcmp(argv[i], "--shot-combat") == 0) {
            combatShot = true;
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[i + 1], nullptr, 10);
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
        desc.title = "Emberwake";
        desc.width = windowWidth;
        desc.height = windowHeight;
        Window window{desc};
        Renderer renderer{window};
        EventBus events;
        AssetRegistry assets{renderer.context(), renderer.bindless(), events};
        GameUi ui{window, renderer};

        const std::filesystem::path contentDir{EMBERWAKE_CONTENT_DIR};
        assets.scan(contentDir);
        Guids guids;
        guids.wisp = assets.guidForPath(contentDir / "wisp.glb");
        guids.shade = assets.guidForPath(contentDir / "shade.glb");
        guids.hollow = assets.guidForPath(contentDir / "hollow.glb");
        guids.smother = assets.guidForPath(contentDir / "smother.glb");
        guids.bolt = assets.guidForPath(contentDir / "bolt.glb");
        guids.pickups = assets.guidForPath(contentDir / "pickups.glb");
        guids.brazier = assets.guidForPath(contentDir / "brazier.glb");
        guids.rift = assets.guidForPath(contentDir / "rift.glb");
        guids.wall = assets.guidForPath(contentDir / "dwall.glb");
        guids.floor = assets.guidForPath(contentDir / "dfloor.glb");
        for (const AssetGuid guid :
             {guids.wisp, guids.shade, guids.hollow, guids.smother,
              guids.bolt, guids.pickups, guids.brazier, guids.rift,
              guids.wall, guids.floor}) {
            CD_ASSERT(guid != kInvalidGuid, "Emberwake content missing under {}",
                      contentDir.string());
            assets.getModelBlocking(guid);
        }
        MeshIds meshes;
        meshes.emberBolt =
            meshIndexByName(*assets.getModelBlocking(guids.bolt), "EmberBolt");
        meshes.darkBolt =
            meshIndexByName(*assets.getModelBlocking(guids.bolt), "DarkBolt");
        meshes.ember =
            meshIndexByName(*assets.getModelBlocking(guids.pickups), "Ember");
        meshes.shard =
            meshIndexByName(*assets.getModelBlocking(guids.pickups), "Shard");
        meshes.brazierUnlit = meshIndexByName(
            *assets.getModelBlocking(guids.brazier), "BrazierUnlit");
        meshes.brazierLit = meshIndexByName(
            *assets.getModelBlocking(guids.brazier), "BrazierLit");

        const InputActions input = InputActions::flyCameraDefaults();
        Camera camera;
        float camYaw = glm::radians(180.0f);
        float camPitch = glm::radians(-52.0f);
        glm::vec3 cameraTarget{0.0f};

        World world;
        emberwake::Dungeon dungeon;
        GameState state = GameState::Title;
        Run run;
        FloorEntities floor;
        Messages messages;
        emberwake::Rng gameRng(seed ^ 0x9E3779B9u);

        auto enterFloor = [&](int floorNumber) {
            run.floor = floorNumber;
            dungeon = emberwake::Dungeon::generate(
                seed + static_cast<uint64_t>(floorNumber) * 1013u,
                kDungeonWidth, kDungeonHeight, kDungeonRooms);
            run.wispPos =
                emberwake::Dungeon::cellCenter(dungeon.spawn());
            run.wispVel = {0.0f, 0.0f};
            run.bossDead = false;
            floor = buildFloor(world, dungeon, guids, meshes, floorNumber,
                               seed, run.wispPos);
            cameraTarget = {run.wispPos.x, kWispHeight, run.wispPos.y};
            run.iframes = 1.2f; // spawn grace
        };
        auto startRun = [&] {
            run = Run{};
            enterFloor(1);
            state = GameState::Playing;
            messages.post("Floor 1 — find the rift");
        };

        // Bot steering state.
        float botRepath = 0.0f;
        float botStrafeFlip = 0.0f;
        float botStrafeSign = 1.0f;
        std::deque<glm::vec2> botWaypoints;
        entt::entity botTarget = entt::null;
        entt::entity botIgnored = entt::null;
        float botTargetHp = 0.0f;
        float botStuckTime = 0.0f;
        float botIgnoredUntil = 0.0f;

        uint64_t frameCount = 0;
        uint64_t victoryAtFrame = 0;
        uint64_t firstHitFrame = 0;
        bool screenshotTaken = false;
        bool prevRestartDown = false;
        auto lastFrame = std::chrono::steady_clock::now();

        // The title screen floats over the first floor as a backdrop.
        run = Run{};
        enterFloor(1);

        while (!window.shouldClose()) {
            window.pollEvents();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(
                std::chrono::duration<float>(now - lastFrame).count(), 0.05f);
            lastFrame = now;

            assets.update();
            ui.beginFrame();
            const glm::uvec2 fb = window.framebufferSize();
            const glm::vec2 windowSize{static_cast<float>(fb.x),
                                       static_cast<float>(fb.y)};
            const float aspect = windowSize.x / std::max(windowSize.y, 1.0f);

            // --- Camera orbit (hold RMB) ---
            const bool looking =
                state == GameState::Playing && input.isDown(window, "look");
            window.setCursorCaptured(looking);
            const glm::vec2 mouseDelta = window.consumeMouseDelta();
            if (looking) {
                camYaw -= mouseDelta.x * 0.0035f;
                camPitch = std::clamp(camPitch - mouseDelta.y * 0.0035f,
                                      glm::radians(-72.0f),
                                      glm::radians(-25.0f));
            }

            // --- State transitions ---
            const bool restartDown =
                window.isKeyDown(GLFW_KEY_R) ||
                window.isMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
            const bool restartPressed = restartDown && !prevRestartDown;
            prevRestartDown = restartDown;
            if (state == GameState::Title &&
                (restartPressed || botMode)) {
                startRun();
            } else if ((state == GameState::Dead ||
                        state == GameState::Victory) &&
                       (restartPressed || (botMode && state == GameState::Dead))) {
                startRun();
            }

            if (state == GameState::Playing) {
                run.time += dt;
                run.iframes = std::max(0.0f, run.iframes - dt);
                run.shootCooldown = std::max(0.0f, run.shootCooldown - dt);

                // --- Player intent: keyboard or bot ---
                glm::vec2 move{0.0f};
                bool wantShoot = false;
                glm::vec2 aim{0.0f};

                const glm::vec3 camForward3 = lookDirection(camYaw, 0.0f);
                const glm::vec2 forward = glm::normalize(
                    glm::vec2(camForward3.x, camForward3.z));
                const glm::vec2 right{-forward.y, forward.x};

                if (!botMode) {
                    move = forward *
                               input.axis(window, "move_forward", "move_back") +
                           right * input.axis(window, "move_right", "move_left");
                    double mouseX = 0.0;
                    double mouseY = 0.0;
                    glfwGetCursorPos(window.handle(), &mouseX, &mouseY);
                    aim = aimPointOnPlane(
                        camera, aspect,
                        {static_cast<float>(mouseX),
                         static_cast<float>(mouseY)},
                        windowSize, kWispHeight);
                    wantShoot = !looking && window.isMouseButtonDown(
                                                GLFW_MOUSE_BUTTON_LEFT);
                } else {
                    // --- Bot: fight what it sees, heal when hurt, descend ---
                    botRepath = std::max(0.0f, botRepath - dt);
                    botStrafeFlip -= dt;
                    if (botStrafeFlip <= 0.0f) {
                        botStrafeFlip = 2.4f;
                        botStrafeSign = -botStrafeSign;
                    }

                    // Nearest enemy a bolt can actually reach (width-aware
                    // clearance, not zero-width LOS — a doorway corner that
                    // eats every shot would stall the fight forever).
                    entt::entity target = entt::null;
                    float targetDist = 11.0f;
                    glm::vec2 targetPos{0.0f};
                    glm::vec2 targetVel{0.0f};
                    for (const auto [entity, enemy] :
                         world.registry.view<Enemy>().each()) {
                        if (entity == botIgnored &&
                            run.time < botIgnoredUntil) {
                            continue;
                        }
                        const auto& t =
                            world.registry.get<LocalTransform>(entity);
                        const glm::vec2 pos{t.translation.x, t.translation.z};
                        const float dist = glm::distance(pos, run.wispPos);
                        if (dist < targetDist &&
                            dungeon.corridorClear(run.wispPos, pos,
                                                  kBoltRadius + 0.02f)) {
                            target = entity;
                            targetDist = dist;
                            targetPos = pos;
                            targetVel = enemy.velocity;
                        }
                    }

                    // Safety net: a target whose health never moves is
                    // unhittable from here — ignore it and keep moving.
                    if (target != entt::null) {
                        const float hp =
                            world.registry.get<Enemy>(target).hp;
                        if (target == botTarget && hp == botTargetHp) {
                            botStuckTime += dt;
                            if (botStuckTime > 4.0f) {
                                botIgnored = target;
                                botIgnoredUntil = run.time + 6.0f;
                                botStuckTime = 0.0f;
                                target = entt::null;
                            }
                        } else {
                            botTarget = target;
                            botTargetHp = hp;
                            botStuckTime = 0.0f;
                        }
                    }

                    if (target != entt::null) {
                        aim = targetPos + targetVel * 0.15f;
                        wantShoot = true;
                        if (targetDist < 3.5f) {
                            move = glm::normalize(run.wispPos - targetPos);
                        } else if (targetDist > 7.0f) {
                            move = glm::normalize(targetPos - run.wispPos);
                        } else {
                            // Strafe, but only into open space — sideways
                            // into a corridor wall just jitters in place.
                            const glm::vec2 toTarget =
                                glm::normalize(targetPos - run.wispPos);
                            const glm::vec2 side{-toTarget.y, toTarget.x};
                            if (!dungeon.lineClear(
                                    run.wispPos,
                                    run.wispPos + side * botStrafeSign *
                                                      1.2f)) {
                                botStrafeSign = -botStrafeSign;
                            }
                            move = side * botStrafeSign;
                            if (!dungeon.lineClear(run.wispPos,
                                                   run.wispPos +
                                                       move * 1.2f)) {
                                move = -toTarget; // boxed in: back off
                            }
                        }
                        botWaypoints.clear();
                    } else {
                        // Navigate: ember when hurt, otherwise the exit.
                        if (botWaypoints.empty() || botRepath <= 0.0f) {
                            botRepath = 1.5f;
                            botWaypoints.clear();
                            glm::ivec2 goal = dungeon.stairs();
                            if (run.flame < 45.0f) {
                                float best = 1e9f;
                                for (const auto [entity, pickup] :
                                     world.registry.view<Pickup>().each()) {
                                    if (pickup.kind != PickupKind::Ember) {
                                        continue;
                                    }
                                    const auto& t =
                                        world.registry.get<LocalTransform>(
                                            entity);
                                    const glm::vec2 pos{t.translation.x,
                                                        t.translation.z};
                                    const float dist =
                                        glm::distance(pos, run.wispPos);
                                    if (dist < best) {
                                        best = dist;
                                        goal = emberwake::Dungeon::cellOf(pos);
                                    }
                                }
                            } else if (run.floor == kFloorCount &&
                                       !run.bossDead &&
                                       floor.boss != entt::null &&
                                       world.registry.valid(floor.boss)) {
                                const auto& t =
                                    world.registry.get<LocalTransform>(
                                        floor.boss);
                                goal = emberwake::Dungeon::cellOf(
                                    {t.translation.x, t.translation.z});
                            }
                            for (const glm::ivec2 step : dungeon.pathBetween(
                                     emberwake::Dungeon::cellOf(run.wispPos),
                                     goal)) {
                                botWaypoints.push_back(
                                    emberwake::Dungeon::cellCenter(step));
                            }
                        }
                        if (!botWaypoints.empty()) {
                            const glm::vec2 toNext =
                                botWaypoints.front() - run.wispPos;
                            if (glm::dot(toNext, toNext) < 0.2f * 0.2f) {
                                botWaypoints.pop_front();
                            } else {
                                move = glm::normalize(toNext);
                            }
                        }
                    }
                }

                if (glm::dot(move, move) > 1.0f) {
                    move = glm::normalize(move);
                }

                // --- Player physics ---
                run.wispVel += move * kAccel * dt;
                run.wispVel *= std::max(0.0f, 1.0f - kDamping * dt);
                const float speed = glm::length(run.wispVel);
                if (speed > kMaxSpeed) {
                    run.wispVel *= kMaxSpeed / speed;
                }
                run.wispPos += run.wispVel * dt;
                run.wispPos =
                    dungeon.resolveCollision(run.wispPos, kWispRadius);

                // --- Shooting ---
                if (wantShoot && run.shootCooldown <= 0.0f) {
                    const glm::vec2 dir = aim - run.wispPos;
                    if (glm::dot(dir, dir) > 1e-4f) {
                        spawnBolt(world, guids, meshes, false, run.wispPos,
                                  dir);
                        run.shootCooldown = kShootCooldown;
                    }
                }

                // --- Bolts ---
                {
                    std::vector<entt::entity> dead;
                    for (const auto [entity, bolt] :
                         world.registry.view<Bolt>().each()) {
                        auto& t = world.registry.get<LocalTransform>(entity);
                        glm::vec2 pos{t.translation.x, t.translation.z};
                        bolt.life -= dt;
                        bool destroyed = bolt.life <= 0.0f;
                        // Substep so fast bolts can't tunnel wall corners.
                        const int substeps = 3;
                        for (int step = 0; step < substeps && !destroyed;
                             ++step) {
                            pos += bolt.velocity *
                                   (dt / static_cast<float>(substeps));
                            if (dungeon.resolveCollision(pos, kBoltRadius) !=
                                pos) {
                                destroyed = true;
                                break;
                            }
                            if (bolt.hostile) {
                                if (run.iframes <= 0.0f &&
                                    glm::distance(pos, run.wispPos) <
                                        kWispRadius + kBoltRadius) {
                                    run.flame -= kDarkBoltDamage;
                                    run.iframes = kIFrames * 0.5f;
                                    destroyed = true;
                                }
                            } else {
                                for (const auto [enemyEntity, enemy] :
                                     world.registry.view<Enemy>().each()) {
                                    const auto& et =
                                        world.registry.get<LocalTransform>(
                                            enemyEntity);
                                    const glm::vec2 enemyPos{
                                        et.translation.x, et.translation.z};
                                    if (glm::distance(pos, enemyPos) <
                                        enemy.radius + kBoltRadius) {
                                        enemy.hp -= kBoltDamage;
                                        enemy.aggro = true;
                                        if (firstHitFrame == 0) {
                                            firstHitFrame = frameCount;
                                        }
                                        enemy.velocity +=
                                            glm::normalize(bolt.velocity) *
                                            1.2f;
                                        destroyed = true;
                                        break;
                                    }
                                }
                            }
                        }
                        t.translation = {pos.x, kWispHeight, pos.y};
                        if (destroyed) {
                            dead.push_back(entity);
                        }
                    }
                    for (const entt::entity entity : dead) {
                        destroyWithChildren(world, entity);
                    }
                }

                // --- Enemies ---
                {
                    std::vector<entt::entity> killed;
                    std::vector<glm::vec2> summonPositions;
                    // Collected first: separation reads all positions.
                    std::vector<entt::entity> enemyEntities;
                    for (const entt::entity entity :
                         world.registry.view<Enemy>()) {
                        enemyEntities.push_back(entity);
                    }
                    for (const entt::entity entity : enemyEntities) {
                        auto& enemy = world.registry.get<Enemy>(entity);
                        auto& t = world.registry.get<LocalTransform>(entity);
                        glm::vec2 pos{t.translation.x, t.translation.z};
                        const float distToPlayer =
                            glm::distance(pos, run.wispPos);
                        const bool los =
                            dungeon.lineClear(pos, run.wispPos);
                        if (!enemy.aggro && distToPlayer < kAggroRange &&
                            los) {
                            enemy.aggro = true;
                            if (enemy.kind == EnemyKind::Smother) {
                                messages.post("The Smother wakes");
                            }
                        }

                        glm::vec2 desired{0.0f};
                        if (enemy.aggro) {
                            const bool keepRange =
                                enemy.kind == EnemyKind::Hollow;
                            if (keepRange) {
                                if (distToPlayer < 4.0f) {
                                    desired =
                                        glm::normalize(pos - run.wispPos);
                                } else if (distToPlayer > 8.0f || !los) {
                                    desired = {0.0f, 0.0f}; // path below
                                }
                                enemy.shootTimer -= dt;
                                if (los && distToPlayer < 9.0f &&
                                    enemy.shootTimer <= 0.0f &&
                                    dungeon.corridorClear(
                                        pos, run.wispPos,
                                        kBoltRadius + 0.02f)) {
                                    enemy.shootTimer = 1.8f;
                                    spawnBolt(world, guids, meshes, true, pos,
                                              run.wispPos - pos);
                                }
                            }
                            if (enemy.kind == EnemyKind::Smother) {
                                enemy.shootTimer -= dt;
                                if (enemy.shootTimer <= 0.0f) {
                                    enemy.shootTimer = 4.0f;
                                    for (int i = 0; i < 10; ++i) {
                                        const float angle =
                                            static_cast<float>(i) * 0.628f;
                                        spawnBolt(world, guids, meshes, true,
                                                  pos,
                                                  {std::cos(angle),
                                                   std::sin(angle)});
                                    }
                                }
                                // Summon adds at hp thresholds.
                                if (!enemy.summonAt.empty() &&
                                    enemy.hp <= enemy.maxHp *
                                                    enemy.summonAt.front()) {
                                    enemy.summonAt.erase(
                                        enemy.summonAt.begin());
                                    summonPositions.push_back(pos);
                                    messages.post(
                                        "The Smother calls its shades");
                                }
                            }
                            const bool chases =
                                enemy.kind != EnemyKind::Hollow;
                            if (chases || (!los && distToPlayer > 8.0f)) {
                                if (los) {
                                    desired =
                                        glm::normalize(run.wispPos - pos);
                                    enemy.waypoints.clear();
                                } else {
                                    enemy.repathTimer -= dt;
                                    if (enemy.waypoints.empty() ||
                                        enemy.repathTimer <= 0.0f) {
                                        enemy.repathTimer = 0.6f;
                                        enemy.waypoints.clear();
                                        for (const glm::ivec2 step :
                                             dungeon.pathBetween(
                                                 emberwake::Dungeon::cellOf(
                                                     pos),
                                                 emberwake::Dungeon::cellOf(
                                                     run.wispPos))) {
                                            enemy.waypoints.push_back(
                                                emberwake::Dungeon::
                                                    cellCenter(step));
                                        }
                                    }
                                    if (!enemy.waypoints.empty()) {
                                        const glm::vec2 toNext =
                                            enemy.waypoints.front() - pos;
                                        if (glm::dot(toNext, toNext) <
                                            0.25f * 0.25f) {
                                            enemy.waypoints.pop_front();
                                        } else {
                                            desired =
                                                glm::normalize(toNext);
                                        }
                                    }
                                }
                            }
                        }

                        enemy.velocity += desired * 14.0f * dt;
                        enemy.velocity *=
                            std::max(0.0f, 1.0f - 6.0f * dt);
                        const float enemySpeed = glm::length(enemy.velocity);
                        if (enemySpeed > enemy.speed) {
                            enemy.velocity *= enemy.speed / enemySpeed;
                        }
                        pos += enemy.velocity * dt;

                        // Separation from other enemies.
                        for (const entt::entity other : enemyEntities) {
                            if (other == entity ||
                                !world.registry.valid(other)) {
                                continue;
                            }
                            const auto& ot =
                                world.registry.get<LocalTransform>(other);
                            const glm::vec2 otherPos{ot.translation.x,
                                                     ot.translation.z};
                            const float minDist =
                                enemy.radius +
                                world.registry.get<Enemy>(other).radius;
                            const glm::vec2 apart = pos - otherPos;
                            const float dist = glm::length(apart);
                            if (dist > 1e-4f && dist < minDist) {
                                pos += apart * ((minDist - dist) / dist) *
                                       0.5f;
                            }
                        }
                        pos = dungeon.resolveCollision(pos, enemy.radius);

                        // Touch damage.
                        if (run.iframes <= 0.0f &&
                            glm::distance(pos, run.wispPos) <
                                enemy.radius + kWispRadius + 0.05f) {
                            run.flame -= enemy.touchDamage;
                            run.iframes = kIFrames;
                            const glm::vec2 knock =
                                run.wispPos - pos;
                            if (glm::dot(knock, knock) > 1e-6f) {
                                run.wispVel +=
                                    glm::normalize(knock) * 5.0f;
                            }
                        }

                        const float bob =
                            0.15f + 0.06f * std::sin(run.time * 2.1f +
                                                     enemy.bobPhase);
                        t.translation = {pos.x, bob, pos.y};
                        t.rotation = glm::angleAxis(
                            run.time * 0.6f + enemy.bobPhase,
                            glm::vec3(0.0f, 1.0f, 0.0f));

                        if (enemy.hp <= 0.0f) {
                            killed.push_back(entity);
                        }
                    }
                    for (const entt::entity entity : killed) {
                        const auto& t =
                            world.registry.get<LocalTransform>(entity);
                        const glm::vec2 pos{t.translation.x, t.translation.z};
                        const Enemy& enemy = world.registry.get<Enemy>(entity);
                        const bool wasBoss =
                            enemy.kind == EnemyKind::Smother;
                        spawnFlash(world, pos,
                                   wasBoss ? glm::vec3(1.0f, 0.25f, 0.15f)
                                           : glm::vec3(0.7f, 0.4f, 1.0f),
                                   wasBoss ? 14.0f : 5.0f);
                        ++run.kills;
                        if (wasBoss) {
                            run.bossDead = true;
                            floor.boss = entt::null;
                            messages.post(
                                "The Smother shatters — light the brazier");
                        }
                        destroyWithChildren(world, entity);
                    }
                    for (const glm::vec2 pos : summonPositions) {
                        for (int i = 0; i < 2; ++i) {
                            const float angle =
                                gameRng.unit() * 6.28f;
                            const glm::vec2 offset{std::cos(angle) * 1.6f,
                                                   std::sin(angle) * 1.6f};
                            spawnEnemy(world, guids, EnemyKind::Shade,
                                       dungeon.resolveCollision(pos + offset,
                                                                0.34f),
                                       gameRng.unit() * 6.28f);
                        }
                    }
                }

                // --- Pickups ---
                {
                    std::vector<entt::entity> taken;
                    for (const auto [entity, pickup] :
                         world.registry.view<Pickup>().each()) {
                        auto& t = world.registry.get<LocalTransform>(entity);
                        t.translation.y =
                            0.35f + 0.08f * std::sin(run.time * 2.4f +
                                                     pickup.bobPhase);
                        t.rotation = glm::angleAxis(
                            run.time * 1.3f + pickup.bobPhase,
                            glm::vec3(0.0f, 1.0f, 0.0f));
                        const glm::vec2 pos{t.translation.x, t.translation.z};
                        if (glm::distance(pos, run.wispPos) < 0.55f) {
                            if (pickup.kind == PickupKind::Ember) {
                                run.flame = std::min(kMaxFlame,
                                                     run.flame + 30.0f);
                                messages.post("The ember feeds your flame");
                            } else {
                                run.shards += 1;
                            }
                            taken.push_back(entity);
                        }
                    }
                    for (const entt::entity entity : taken) {
                        destroyWithChildren(world, entity);
                    }
                }

                // --- Exit: rift descent / brazier victory ---
                {
                    const auto& t =
                        world.registry.get<LocalTransform>(floor.exitMarker);
                    const glm::vec2 exitPos{t.translation.x, t.translation.z};
                    const float dist =
                        glm::distance(exitPos, run.wispPos);
                    if (run.floor < kFloorCount && dist < 0.7f) {
                        enterFloor(run.floor + 1);
                        messages.post(std::format(
                            "Floor {} — {}", run.floor,
                            run.floor == kFloorCount
                                ? "The Smother stirs below"
                                : "find the rift"));
                        botWaypoints.clear();
                    } else if (run.floor == kFloorCount && run.bossDead &&
                               !run.brazierLit && dist < 1.3f) {
                        run.brazierLit = true;
                        world.registry.get<MeshRenderer>(floor.exitMarker)
                            .meshIndex = meshes.brazierLit;
                        const entt::entity brazierLight =
                            world.createEntity("brazier_light");
                        world.registry.get<LocalTransform>(brazierLight)
                            .translation = {0.0f, 1.7f, 0.0f};
                        world.setParent(brazierLight, floor.exitMarker);
                        auto& light =
                            world.registry.emplace<PointLightComponent>(
                                brazierLight);
                        light.color = {1.0f, 0.55f, 0.2f};
                        light.intensity = 10.0f;
                        light.radius = 18.0f;
                        state = GameState::Victory;
                        victoryAtFrame = frameCount;
                        CD_INFO("VICTORY: brazier lit in {} — {} kills, {} "
                                "shards",
                                formatTime(run.time), run.kills, run.shards);
                    }
                }

                // --- Flashes fade and die ---
                {
                    std::vector<entt::entity> finished;
                    for (const auto [entity, flash] :
                         world.registry.view<Flash>().each()) {
                        flash.age += dt;
                        if (flash.age >= flash.life) {
                            finished.push_back(entity);
                            continue;
                        }
                        world.registry.get<PointLightComponent>(entity)
                            .intensity = flash.peakIntensity *
                                         (1.0f - flash.age / flash.life);
                    }
                    for (const entt::entity entity : finished) {
                        destroyWithChildren(world, entity);
                    }
                }

                // --- Wisp visuals: hover bob, gutter with damage ---
                {
                    auto& t = world.registry.get<LocalTransform>(floor.wisp);
                    t.translation = {run.wispPos.x,
                                     kWispHeight + 0.06f * std::sin(
                                                       run.time * 2.3f),
                                     run.wispPos.y};
                    const float frac =
                        std::max(run.flame, 0.0f) / kMaxFlame;
                    auto& light = world.registry.get<PointLightComponent>(
                        floor.wispLight);
                    const float flicker =
                        0.92f + 0.06f * std::sin(run.time * 9.0f) +
                        0.03f * std::sin(run.time * 23.0f);
                    light.intensity =
                        2.6f * (0.3f + 0.7f * frac) * flicker;
                    light.radius = 8.0f * (0.45f + 0.55f * frac);
                }

                // --- Death ---
                if (run.flame <= 0.0f) {
                    state = GameState::Dead;
                    CD_INFO("DEATH on floor {} at {} — {} kills, {} shards",
                            run.floor, formatTime(run.time), run.kills,
                            run.shards);
                }
            }

            messages.update(dt);

            // --- Follow camera ---
            const glm::vec3 targetGoal{run.wispPos.x, kWispHeight,
                                       run.wispPos.y};
            const float follow = 1.0f - std::exp(-10.0f * dt);
            cameraTarget = glm::mix(cameraTarget, targetGoal, follow);
            camera.yawRadians = camYaw;
            camera.pitchRadians = camPitch;
            camera.position =
                cameraTarget - lookDirection(camYaw, camPitch) *
                                   kCameraDistance;

            // --- HUD ---
            {
                const ImGuiWindowFlags hudFlags =
                    ImGuiWindowFlags_NoDecoration |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing |
                    ImGuiWindowFlags_NoInputs;
                if (state == GameState::Playing ||
                    state == GameState::Dead ||
                    state == GameState::Victory) {
                    ImGui::SetNextWindowPos({16.0f, 16.0f});
                    ImGui::SetNextWindowBgAlpha(0.45f);
                    ImGui::Begin("flame", nullptr, hudFlags);
                    ImGui::TextColored({1.0f, 0.8f, 0.5f, 1.0f}, "Flame");
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                          {1.0f, 0.55f, 0.15f, 1.0f});
                    ImGui::ProgressBar(
                        std::max(run.flame, 0.0f) / kMaxFlame,
                        ImVec2(220.0f, 14.0f), "");
                    ImGui::PopStyleColor();
                    ImGui::End();

                    ImGui::SetNextWindowPos({windowSize.x - 16.0f, 16.0f},
                                            ImGuiCond_Always, {1.0f, 0.0f});
                    ImGui::SetNextWindowBgAlpha(0.45f);
                    ImGui::Begin("info", nullptr, hudFlags);
                    ImGui::Text("Floor %d/%d", run.floor, kFloorCount);
                    ImGui::Text("Shards %d", run.shards);
                    ImGui::Text("%s", formatTime(run.time).c_str());
                    ImGui::End();

                    // Boss bar while The Smother lives and is angry.
                    if (floor.boss != entt::null &&
                        world.registry.valid(floor.boss)) {
                        const Enemy& boss =
                            world.registry.get<Enemy>(floor.boss);
                        if (boss.aggro) {
                            ImGui::SetNextWindowPos(
                                {windowSize.x * 0.5f, windowSize.y - 42.0f},
                                ImGuiCond_Always, {0.5f, 0.0f});
                            ImGui::SetNextWindowBgAlpha(0.45f);
                            ImGui::Begin("boss", nullptr, hudFlags);
                            ImGui::TextColored({1.0f, 0.35f, 0.3f, 1.0f},
                                               "The Smother");
                            ImGui::PushStyleColor(
                                ImGuiCol_PlotHistogram,
                                {0.85f, 0.2f, 0.15f, 1.0f});
                            ImGui::ProgressBar(boss.hp / boss.maxHp,
                                               ImVec2(320.0f, 12.0f), "");
                            ImGui::PopStyleColor();
                            ImGui::End();
                        }
                    }
                }

                auto centerPanel = [&](const char* title,
                                       std::initializer_list<std::string>
                                           lines) {
                    ImGui::SetNextWindowPos(
                        {windowSize.x * 0.5f, windowSize.y * 0.42f},
                        ImGuiCond_Always, {0.5f, 0.5f});
                    ImGui::SetNextWindowBgAlpha(0.7f);
                    ImGui::Begin(title, nullptr,
                                 hudFlags & ~ImGuiWindowFlags_NoInputs);
                    ImGui::TextColored({1.0f, 0.75f, 0.35f, 1.0f}, "%s",
                                       title);
                    ImGui::Separator();
                    for (const std::string& line : lines) {
                        ImGui::TextUnformatted(line.c_str());
                    }
                    ImGui::End();
                };
                if (state == GameState::Title) {
                    centerPanel("EMBERWAKE",
                                {"You are a wisp of flame in a dark dungeon.",
                                 "Relight the Great Brazier three floors "
                                 "down.",
                                 "",
                                 "WASD  move      Left mouse  fire",
                                 "Hold right mouse  orbit camera",
                                 "",
                                 "Click to begin"});
                } else if (state == GameState::Dead) {
                    centerPanel(
                        "YOUR FLAME GUTTERS OUT",
                        {std::format("Floor {}   {}   {} kills   {} shards",
                                     run.floor, formatTime(run.time),
                                     run.kills, run.shards),
                         "", "Press R to rise again"});
                } else if (state == GameState::Victory) {
                    centerPanel(
                        "THE GREAT BRAZIER ROARS",
                        {std::format("Cleared in {}   {} kills   {} shards",
                                     formatTime(run.time), run.kills,
                                     run.shards),
                         "", "Press R for a new descent"});
                }
                messages.draw(windowSize);
            }

            ImGui::Render();
            world.updateTransforms();

            // Screenshot: near the frame limit, or — in bot runs — a dozen
            // frames after victory (auto-sized ImGui panels and temporal
            // accumulation need a moment to settle).
            if (!screenshotPath.empty() && !screenshotTaken) {
                const bool frameTrigger =
                    maxFrames != 0 && frameCount == maxFrames - 10;
                const bool victoryTrigger =
                    botMode && state == GameState::Victory &&
                    frameCount == victoryAtFrame + 12;
                const bool combatTrigger =
                    combatShot && firstHitFrame != 0 &&
                    frameCount == firstHitFrame + 8;
                if (frameTrigger || victoryTrigger || combatTrigger) {
                    renderer.requestScreenshot(screenshotPath);
                    screenshotTaken = true;
                }
            }

            RenderOptions options;
            options.recordUI = [](VkCommandBuffer cmd) {
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            };
            renderer.drawFrame(camera, world, assets, options);
            FrameMark;

            ++frameCount;
            // Heartbeat for headless runs — makes stalls diagnosable.
            if (frameCount % 600 == 0) {
                CD_INFO("heartbeat: frame {} floor {} t={} kills {} flame "
                        "{:.0f} state {}",
                        frameCount, run.floor, formatTime(run.time),
                        run.kills, run.flame,
                        static_cast<int>(state));
            }
            if (maxFrames != 0 && frameCount >= maxFrames) {
                CD_INFO("Reached frame limit ({}), exiting", maxFrames);
                break;
            }
            // Bot runs end shortly after the win — the job is done.
            if (botMode && state == GameState::Victory &&
                frameCount > victoryAtFrame + 40) {
                CD_INFO("Bot run complete, exiting");
                break;
            }
        }
        CD_INFO("Emberwake: exited on floor {} ({}) after {} frames — {} "
                "kills, {} shards",
                run.floor,
                state == GameState::Victory ? "victory"
                : state == GameState::Dead ? "dead"
                                           : "in progress",
                frameCount, run.kills, run.shards);
    }
    JobSystem::shutdown();
    return 0;
}
