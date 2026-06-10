#include "candela/scene/World.h"

#include "candela/assets/AssetRegistry.h"
#include "candela/core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <tracy/Tracy.hpp>

#include <unordered_map>

namespace candela {

glm::mat4 LocalTransform::matrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), translation);
    m *= glm::mat4_cast(rotation);
    m = glm::scale(m, scale);
    return m;
}

entt::entity World::createEntity(const std::string& name) {
    const entt::entity entity = registry.create();
    registry.emplace<Name>(entity, name);
    registry.emplace<LocalTransform>(entity);
    registry.emplace<WorldTransform>(entity);
    return entity;
}

void World::setParent(entt::entity child, entt::entity parent) {
    registry.emplace_or_replace<Parent>(child, parent);
}

void World::updateTransforms() {
    ZoneScoped;

    std::unordered_map<entt::entity, glm::mat4> computed;
    computed.reserve(registry.storage<LocalTransform>().size());

    // Memoized recursion: parents resolve before children regardless of
    // iteration order.
    auto resolve = [&](auto&& self, entt::entity entity) -> const glm::mat4& {
        if (auto it = computed.find(entity); it != computed.end()) {
            return it->second;
        }
        const auto& local = registry.get<LocalTransform>(entity);
        glm::mat4 world = local.matrix();
        if (const auto* parent = registry.try_get<Parent>(entity);
            parent != nullptr && registry.valid(parent->value) &&
            registry.all_of<LocalTransform>(parent->value)) {
            world = self(self, parent->value) * world;
        }
        return computed.emplace(entity, world).first->second;
    };

    for (const entt::entity entity : registry.view<LocalTransform>()) {
        registry.get_or_emplace<WorldTransform>(entity).value =
            resolve(resolve, entity);
    }
}

std::vector<entt::entity> World::instantiateModel(AssetRegistry& assets,
                                                  AssetGuid guid) {
    const ModelAsset* model = assets.getModelBlocking(guid);
    if (model == nullptr) {
        CD_WARN("instantiateModel: asset {} not found", guidToString(guid));
        return {};
    }

    // Everything groups under one root entity named after the model so the
    // instance selects, moves, and deletes as a single object — multi-root
    // glTFs (e.g. FlightHelmet) would otherwise scatter into siblings.
    std::string rootName = assets.pathForGuid(guid).stem().string();
    if (rootName.empty()) {
        rootName = "Model";
    }
    const entt::entity root = createEntity(rootName);

    std::vector<entt::entity> entities;
    entities.reserve(model->nodes.size() + 1);
    entities.push_back(root);

    std::vector<entt::entity> nodeEntities(model->nodes.size());
    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const NodeTemplate& node = model->nodes[i];
        const entt::entity entity =
            createEntity(node.name.empty() ? "node" : node.name);
        auto& transform = registry.get<LocalTransform>(entity);
        transform.translation = node.translation;
        transform.rotation = node.rotation;
        transform.scale = node.scale;
        if (node.meshIndex >= 0) {
            registry.emplace<MeshRenderer>(
                entity, guid, static_cast<uint32_t>(node.meshIndex));
        }
        nodeEntities[i] = entity;
        entities.push_back(entity);
    }
    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const int parent = model->nodes[i].parent;
        setParent(nodeEntities[i],
                  parent >= 0 ? nodeEntities[static_cast<size_t>(parent)]
                              : root);
    }
    return entities;
}

} // namespace candela
