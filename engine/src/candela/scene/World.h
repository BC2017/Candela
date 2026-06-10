#pragma once

#include "candela/scene/Components.h"

#include <entt/entt.hpp>

#include <vector>

namespace candela {

class AssetRegistry;

// The ECS world: an entt registry plus the transform hierarchy system and
// model instantiation.
class World {
public:
    entt::registry registry;
    SceneSettings settings;

    entt::entity createEntity(const std::string& name);
    void setParent(entt::entity child, entt::entity parent);

    // Recomputes every WorldTransform from the LocalTransform/Parent
    // hierarchy. O(n) memoized recursion; dirty-flag propagation is a
    // profiling-driven upgrade once scenes get large.
    void updateTransforms();

    // Spawns the model's node hierarchy as entities (root entities have no
    // parent). Requires the asset to be loaded; returns created entities.
    std::vector<entt::entity> instantiateModel(AssetRegistry& assets,
                                               AssetGuid guid);
};

} // namespace candela
