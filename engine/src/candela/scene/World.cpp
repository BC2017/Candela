#include "candela/scene/World.h"

#include "candela/assets/AssetRegistry.h"
#include "candela/core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <tracy/Tracy.hpp>

#include <cmath>
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

void World::updateAnimations(AssetRegistry& assets, float dt) {
    ZoneScoped;

    for (auto [entity, anim] : registry.view<Animator>().each()) {
        if (!anim.playing) {
            continue;
        }
        const ModelAsset* model = assets.tryGetModel(anim.model);
        if (model == nullptr || anim.clip < 0 ||
            anim.clip >= static_cast<int>(model->animations.size())) {
            continue;
        }
        const Skeleton* skeleton = registry.try_get<Skeleton>(entity);
        if (skeleton == nullptr) {
            continue;
        }
        const AnimationClip& clip =
            model->animations[static_cast<size_t>(anim.clip)];

        anim.time += dt * anim.speed;
        if (clip.duration > 0.0f) {
            if (anim.loop) {
                anim.time = std::fmod(anim.time, clip.duration);
                if (anim.time < 0.0f) {
                    anim.time += clip.duration;
                }
            } else {
                anim.time = glm::clamp(anim.time, 0.0f, clip.duration);
            }
        }

        for (const AnimationChannel& channel : clip.channels) {
            // Resolve the channel's target node to a joint entity. v1 animates
            // only joints that belong to this entity's skeleton.
            entt::entity joint = entt::null;
            for (size_t j = 0; j < skeleton->jointNodeIndex.size(); ++j) {
                if (skeleton->jointNodeIndex[j] == channel.targetNode) {
                    joint = skeleton->joints[j];
                    break;
                }
            }
            if (joint == entt::null || !registry.valid(joint) ||
                !registry.all_of<LocalTransform>(joint)) {
                continue;
            }
            auto& local = registry.get<LocalTransform>(joint);
            switch (channel.path) {
            case AnimationChannel::Path::Translation:
                local.translation = sampleVec3(channel, anim.time);
                break;
            case AnimationChannel::Path::Scale:
                local.scale = sampleVec3(channel, anim.time);
                break;
            case AnimationChannel::Path::Rotation:
                local.rotation = sampleQuat(channel, anim.time);
                break;
            }
        }
    }
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
            // Skinned meshes carry a SkinnedMeshRenderer (the renderer's
            // skinned path owns them); everything else a plain MeshRenderer.
            if (node.skinIndex >= 0) {
                registry.emplace<SkinnedMeshRenderer>(
                    entity, guid, static_cast<uint32_t>(node.meshIndex),
                    node.skinIndex);
            } else {
                registry.emplace<MeshRenderer>(
                    entity, guid, static_cast<uint32_t>(node.meshIndex));
            }
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

    // Wire skinning: build each skinned entity's Skeleton from the skin's
    // joint node list (mapped through nodeEntities) and attach an Animator
    // playing the first clip if the model has any.
    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const int skinIndex = model->nodes[i].skinIndex;
        if (skinIndex < 0 ||
            skinIndex >= static_cast<int>(model->skins.size())) {
            continue;
        }
        const Skin& skin = model->skins[static_cast<size_t>(skinIndex)];
        Skeleton skeleton;
        skeleton.joints.reserve(skin.jointNodes.size());
        skeleton.jointNodeIndex = skin.jointNodes;
        skeleton.inverseBind = skin.inverseBind;
        for (int jointNode : skin.jointNodes) {
            if (jointNode >= 0 &&
                jointNode < static_cast<int>(nodeEntities.size())) {
                skeleton.joints.push_back(
                    nodeEntities[static_cast<size_t>(jointNode)]);
            } else {
                skeleton.joints.push_back(entt::null);
            }
        }
        registry.emplace<Skeleton>(nodeEntities[i], std::move(skeleton));
        if (!model->animations.empty()) {
            registry.emplace<Animator>(nodeEntities[i], guid, 0, 0.0f, 1.0f,
                                       true, true);
        }
    }
    return entities;
}

} // namespace candela
