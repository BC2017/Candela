#include "candela/scene/SceneSerializer.h"

#include "candela/assets/AssetRegistry.h"
#include "candela/core/Log.h"
#include "candela/scene/World.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>

namespace candela::SceneSerializer {

namespace {

nlohmann::json vec3ToJson(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

glm::vec3 vec3FromJson(const nlohmann::json& j) {
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

nlohmann::json quatToJson(const glm::quat& q) {
    return nlohmann::json::array({q.w, q.x, q.y, q.z});
}

glm::quat quatFromJson(const nlohmann::json& j) {
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
            j[3].get<float>()};
}

// Physics enums serialize as stable strings so scene files stay readable and
// robust to future enum reordering.
const char* rigidBodyMotionName(RigidBody::MotionType motion) {
    switch (motion) {
    case RigidBody::MotionType::Static:
        return "static";
    case RigidBody::MotionType::Kinematic:
        return "kinematic";
    case RigidBody::MotionType::Dynamic:
    default:
        return "dynamic";
    }
}

RigidBody::MotionType rigidBodyMotionFromName(const std::string& name) {
    if (name == "static") {
        return RigidBody::MotionType::Static;
    }
    if (name == "kinematic") {
        return RigidBody::MotionType::Kinematic;
    }
    return RigidBody::MotionType::Dynamic;
}

const char* rigidBodyShapeName(RigidBody::ShapeType shape) {
    switch (shape) {
    case RigidBody::ShapeType::Sphere:
        return "sphere";
    case RigidBody::ShapeType::Capsule:
        return "capsule";
    case RigidBody::ShapeType::Box:
    default:
        return "box";
    }
}

RigidBody::ShapeType rigidBodyShapeFromName(const std::string& name) {
    if (name == "sphere") {
        return RigidBody::ShapeType::Sphere;
    }
    if (name == "capsule") {
        return RigidBody::ShapeType::Capsule;
    }
    return RigidBody::ShapeType::Box;
}

} // namespace

namespace {

nlohmann::json worldToJson(const World& world) {
    const auto& registry = world.registry;
    nlohmann::json scene;
    scene["version"] = 1;

    nlohmann::json settings;
    settings["toSun"] = vec3ToJson(world.settings.toSun);
    settings["sunIntensity"] = world.settings.sunIntensity;
    settings["sunColor"] = vec3ToJson(world.settings.sunColor);
    settings["iblIntensity"] = world.settings.iblIntensity;
    settings["exposure"] = world.settings.exposure;
    settings["bloomStrength"] = world.settings.bloomStrength;
    settings["rtShadows"] = world.settings.rtShadows;
    settings["rtAmbientOcclusion"] = world.settings.rtAmbientOcclusion;
    settings["rtReflections"] = world.settings.rtReflections;
    settings["taa"] = world.settings.taa;
    scene["settings"] = settings;

    // Stable order: iterate the Name storage (every entity has one) in
    // creation order so save → load → save round-trips identically.
    std::vector<entt::entity> entities;
    for (const entt::entity entity : registry.view<Name>()) {
        entities.push_back(entity);
    }
    // entt views iterate newest-first; reverse to creation order.
    std::reverse(entities.begin(), entities.end());

    std::unordered_map<entt::entity, int> entityToIndex;
    for (size_t i = 0; i < entities.size(); ++i) {
        entityToIndex[entities[i]] = static_cast<int>(i);
    }

    nlohmann::json entityArray = nlohmann::json::array();
    for (const entt::entity entity : entities) {
        nlohmann::json e;
        e["name"] = registry.get<Name>(entity).value;

        const auto& local = registry.get<LocalTransform>(entity);
        e["transform"] = {{"t", vec3ToJson(local.translation)},
                          {"r", quatToJson(local.rotation)},
                          {"s", vec3ToJson(local.scale)}};

        if (const auto* parent = registry.try_get<Parent>(entity);
            parent != nullptr && registry.valid(parent->value)) {
            e["parent"] = entityToIndex.at(parent->value);
        }
        if (const auto* mesh = registry.try_get<MeshRenderer>(entity)) {
            e["meshRenderer"] = {{"model", guidToString(mesh->model)},
                                 {"mesh", mesh->meshIndex}};
        }
        if (const auto* light = registry.try_get<PointLightComponent>(entity)) {
            e["pointLight"] = {{"color", vec3ToJson(light->color)},
                               {"intensity", light->intensity},
                               {"radius", light->radius}};
        }
        if (const auto* camera = registry.try_get<CameraComponent>(entity)) {
            e["camera"] = {{"fovYDegrees", camera->fovYDegrees},
                           {"nearPlane", camera->nearPlane}};
        }
        if (const auto* rb = registry.try_get<RigidBody>(entity)) {
            e["rigidBody"] = {{"motion", rigidBodyMotionName(rb->motionType)},
                              {"shape", rigidBodyShapeName(rb->shape)},
                              {"halfExtents", vec3ToJson(rb->halfExtents)},
                              {"mass", rb->mass},
                              {"friction", rb->friction},
                              {"restitution", rb->restitution}};
        }
        if (const auto* cc = registry.try_get<CharacterController>(entity)) {
            e["characterController"] = {{"radius", cc->radius},
                                        {"halfHeight", cc->halfHeight},
                                        {"mass", cc->mass},
                                        {"friction", cc->friction}};
        }
        entityArray.push_back(e);
    }
    scene["entities"] = entityArray;
    return scene;
}

void worldFromJson(World& world, AssetRegistry& assets,
                   const nlohmann::json& scene) {
    world.registry.clear();
    world.settings = {};

    if (scene.contains("settings")) {
        const auto& settings = scene["settings"];
        world.settings.toSun = vec3FromJson(settings["toSun"]);
        world.settings.sunIntensity = settings["sunIntensity"].get<float>();
        world.settings.sunColor = vec3FromJson(settings["sunColor"]);
        world.settings.iblIntensity = settings["iblIntensity"].get<float>();
        world.settings.exposure = settings["exposure"].get<float>();
        world.settings.bloomStrength = settings["bloomStrength"].get<float>();
        // Added in Phase 5 — default on for older scene files.
        world.settings.rtShadows = settings.value("rtShadows", true);
        world.settings.rtAmbientOcclusion =
            settings.value("rtAmbientOcclusion", true);
        world.settings.rtReflections = settings.value("rtReflections", true);
        world.settings.taa = settings.value("taa", true);
    }

    std::vector<entt::entity> entities;
    for (const auto& e : scene["entities"]) {
        const entt::entity entity =
            world.createEntity(e["name"].get<std::string>());
        auto& local = world.registry.get<LocalTransform>(entity);
        local.translation = vec3FromJson(e["transform"]["t"]);
        local.rotation = quatFromJson(e["transform"]["r"]);
        local.scale = vec3FromJson(e["transform"]["s"]);

        if (e.contains("meshRenderer")) {
            const AssetGuid guid =
                guidFromString(e["meshRenderer"]["model"].get<std::string>());
            world.registry.emplace<MeshRenderer>(
                entity, guid, e["meshRenderer"]["mesh"].get<uint32_t>());
            assets.requestModel(guid); // async — geometry streams in
        }
        if (e.contains("pointLight")) {
            auto& light =
                world.registry.emplace<PointLightComponent>(entity);
            light.color = vec3FromJson(e["pointLight"]["color"]);
            light.intensity = e["pointLight"]["intensity"].get<float>();
            light.radius = e["pointLight"]["radius"].get<float>();
        }
        if (e.contains("camera")) {
            auto& camera = world.registry.emplace<CameraComponent>(entity);
            camera.fovYDegrees = e["camera"]["fovYDegrees"].get<float>();
            camera.nearPlane = e["camera"]["nearPlane"].get<float>();
        }
        if (e.contains("rigidBody")) {
            const auto& j = e["rigidBody"];
            auto& rb = world.registry.emplace<RigidBody>(entity);
            rb.motionType =
                rigidBodyMotionFromName(j["motion"].get<std::string>());
            rb.shape = rigidBodyShapeFromName(j["shape"].get<std::string>());
            rb.halfExtents = vec3FromJson(j["halfExtents"]);
            rb.mass = j["mass"].get<float>();
            rb.friction = j["friction"].get<float>();
            rb.restitution = j["restitution"].get<float>();
        }
        if (e.contains("characterController")) {
            const auto& j = e["characterController"];
            auto& cc = world.registry.emplace<CharacterController>(entity);
            cc.radius = j["radius"].get<float>();
            cc.halfHeight = j["halfHeight"].get<float>();
            cc.mass = j["mass"].get<float>();
            cc.friction = j["friction"].get<float>();
        }
        entities.push_back(entity);
    }

    // Parent links resolve by index after all entities exist.
    size_t index = 0;
    for (const auto& e : scene["entities"]) {
        if (e.contains("parent")) {
            world.setParent(entities[index],
                            entities[e["parent"].get<size_t>()]);
        }
        ++index;
    }
}

} // namespace

bool save(const World& world, const std::filesystem::path& path) {
    std::ofstream file(path);
    if (!file) {
        CD_ERROR("Failed to open scene for writing: {}", path.string());
        return false;
    }
    file << worldToJson(world).dump(2) << '\n';
    const auto* names = world.registry.storage<Name>();
    CD_INFO("Saved scene: {} ({} entities)", path.string(),
            names != nullptr ? names->size() : 0);
    return true;
}

bool load(World& world, AssetRegistry& assets,
          const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        CD_ERROR("Failed to open scene: {}", path.string());
        return false;
    }
    nlohmann::json scene =
        nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (scene.is_discarded()) {
        CD_ERROR("Scene is not valid JSON: {}", path.string());
        return false;
    }
    worldFromJson(world, assets, scene);
    CD_INFO("Loaded scene: {} ({} entities)", path.string(),
            world.registry.storage<Name>().size());
    return true;
}

std::string saveToString(const World& world) {
    return worldToJson(world).dump(2);
}

bool loadFromString(World& world, AssetRegistry& assets,
                    const std::string& text) {
    nlohmann::json scene =
        nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (scene.is_discarded()) {
        return false;
    }
    worldFromJson(world, assets, scene);
    return true;
}

} // namespace candela::SceneSerializer
