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
    // Reject reparents that would create a cycle: walk up from the prospective
    // parent and abort if we reach the child. Guarding here protects every
    // caller (editor drag-drop, scene deserialization, model instantiation) —
    // a parent cycle would otherwise make updateTransforms recurse until the
    // stack overflows. The existing hierarchy is always acyclic (this guard is
    // the only way a Parent link is installed), so the walk terminates.
    if (child == parent) {
        CD_WARN("setParent: an entity cannot be its own parent");
        return;
    }
    for (const Parent* link = registry.try_get<Parent>(parent);
         link != nullptr && registry.valid(link->value);
         link = registry.try_get<Parent>(link->value)) {
        if (link->value == child) {
            CD_WARN("setParent: rejected reparent that would create a cycle");
            return;
        }
    }
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
        const glm::mat4 localMatrix = registry.get<LocalTransform>(entity).matrix();
        // Insert the local transform as a placeholder BEFORE recursing so that
        // even if a parent cycle somehow exists it terminates against this
        // entry instead of overflowing the stack. unordered_map node refs are
        // stable across the insertions the recursion performs, so `slot` holds.
        auto slot = computed.emplace(entity, localMatrix).first;
        if (const auto* parent = registry.try_get<Parent>(entity);
            parent != nullptr && registry.valid(parent->value) &&
            registry.all_of<LocalTransform>(parent->value)) {
            slot->second = self(self, parent->value) * localMatrix;
        }
        return slot->second;
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
