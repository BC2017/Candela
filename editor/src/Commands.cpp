#include "Commands.h"

#include <candela/assets/AssetRegistry.h>
#include <candela/core/Log.h>

namespace candela::editor {

namespace {
uint64_t s_nextEditorId = 1;
}

uint64_t reserveEditorId() {
    return s_nextEditorId++;
}

uint64_t assignEditorIds(World& world) {
    uint64_t assigned = 0;
    for (const entt::entity entity : world.registry.view<Name>()) {
        if (!world.registry.all_of<EditorId>(entity)) {
            world.registry.emplace<EditorId>(entity, s_nextEditorId++);
            ++assigned;
        }
    }
    return assigned;
}

entt::entity findByEditorId(World& world, uint64_t id) {
    if (id == 0) {
        return entt::null;
    }
    for (auto [entity, editorId] : world.registry.view<EditorId>().each()) {
        if (editorId.value == id) {
            return entity;
        }
    }
    return entt::null;
}

uint64_t editorIdOf(World& world, entt::entity entity) {
    if (entity == entt::null || !world.registry.valid(entity)) {
        return 0;
    }
    if (const auto* id = world.registry.try_get<EditorId>(entity)) {
        return id->value;
    }
    return world.registry.emplace<EditorId>(entity, s_nextEditorId++).value;
}

EntitySnapshot snapshotEntity(World& world, entt::entity entity) {
    auto& registry = world.registry;
    EntitySnapshot snapshot;
    snapshot.editorId = editorIdOf(world, entity);
    snapshot.name = registry.get<Name>(entity).value;
    snapshot.transform = registry.get<LocalTransform>(entity);
    if (const auto* parent = registry.try_get<Parent>(entity);
        parent != nullptr && registry.valid(parent->value)) {
        snapshot.parentEditorId = editorIdOf(world, parent->value);
    }
    if (const auto* mesh = registry.try_get<MeshRenderer>(entity)) {
        snapshot.meshRenderer = *mesh;
    }
    if (const auto* light = registry.try_get<PointLightComponent>(entity)) {
        snapshot.pointLight = *light;
    }
    if (const auto* camera = registry.try_get<CameraComponent>(entity)) {
        snapshot.camera = *camera;
    }
    // Children: entities whose Parent is this one.
    for (auto [child, parent] : registry.view<Parent>().each()) {
        if (parent.value == entity) {
            snapshot.children.push_back(snapshotEntity(world, child));
        }
    }
    return snapshot;
}

entt::entity restoreEntity(World& world, const EntitySnapshot& snapshot,
                           uint64_t parentOverride) {
    const entt::entity entity = world.createEntity(snapshot.name);
    world.registry.emplace<EditorId>(entity, snapshot.editorId);
    s_nextEditorId = (std::max)(s_nextEditorId, snapshot.editorId + 1);
    world.registry.get<LocalTransform>(entity) = snapshot.transform;

    const uint64_t parentId =
        parentOverride != 0 ? parentOverride : snapshot.parentEditorId;
    if (parentId != 0) {
        if (auto parent = findByEditorId(world, parentId);
            parent != entt::null) {
            world.setParent(entity, parent);
        }
    }
    if (snapshot.meshRenderer) {
        world.registry.emplace<MeshRenderer>(entity, *snapshot.meshRenderer);
    }
    if (snapshot.pointLight) {
        world.registry.emplace<PointLightComponent>(entity,
                                                    *snapshot.pointLight);
    }
    if (snapshot.camera) {
        world.registry.emplace<CameraComponent>(entity, *snapshot.camera);
    }
    for (const EntitySnapshot& child : snapshot.children) {
        restoreEntity(world, child, snapshot.editorId);
    }
    return entity;
}

namespace {

void destroyRecursive(World& world, entt::entity entity) {
    // Collect children first — destroying invalidates iteration.
    std::vector<entt::entity> children;
    for (auto [child, parent] : world.registry.view<Parent>().each()) {
        if (parent.value == entity) {
            children.push_back(child);
        }
    }
    for (const entt::entity child : children) {
        destroyRecursive(world, child);
    }
    world.registry.destroy(entity);
}

} // namespace

void CommandStack::perform(World& world, std::unique_ptr<EditCommand> command) {
    command->apply(world);
    m_commands.resize(m_next); // drop the redo tail
    m_commands.push_back(std::move(command));
    ++m_next;
}

void CommandStack::undo(World& world) {
    if (canUndo()) {
        m_commands[--m_next]->revert(world);
    }
}

void CommandStack::redo(World& world) {
    if (canRedo()) {
        m_commands[m_next++]->apply(world);
    }
}

void CommandStack::clear() {
    m_commands.clear();
    m_next = 0;
}

const char* CommandStack::undoLabel() const {
    return canUndo() ? m_commands[m_next - 1]->label() : "";
}

const char* CommandStack::redoLabel() const {
    return canRedo() ? m_commands[m_next]->label() : "";
}

void TransformCommand::apply(World& world) {
    if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
        world.registry.get<LocalTransform>(entity) = m_after;
    }
}

void TransformCommand::revert(World& world) {
    if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
        world.registry.get<LocalTransform>(entity) = m_before;
    }
}

void RenameCommand::apply(World& world) {
    if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
        world.registry.get<Name>(entity).value = m_after;
    }
}

void RenameCommand::revert(World& world) {
    if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
        world.registry.get<Name>(entity).value = m_before;
    }
}

void CreateEntityCommand::apply(World& world) {
    restoreEntity(world, m_snapshot);
}

void CreateEntityCommand::revert(World& world) {
    if (auto entity = findByEditorId(world, m_snapshot.editorId);
        entity != entt::null) {
        destroyRecursive(world, entity);
    }
}

void DeleteEntityCommand::apply(World& world) {
    if (auto entity = findByEditorId(world, m_snapshot.editorId);
        entity != entt::null) {
        destroyRecursive(world, entity);
    }
}

void DeleteEntityCommand::revert(World& world) {
    restoreEntity(world, m_snapshot);
}

void ReparentCommand::setParentById(World& world, uint64_t childId,
                                    uint64_t parentId) {
    const entt::entity child = findByEditorId(world, childId);
    if (child == entt::null) {
        return;
    }
    if (parentId == 0) {
        world.registry.remove<Parent>(child);
    } else if (auto parent = findByEditorId(world, parentId);
               parent != entt::null) {
        world.setParent(child, parent);
    }
}

void ReparentCommand::apply(World& world) {
    setParentById(world, m_child, m_newParent);
}

void ReparentCommand::revert(World& world) {
    setParentById(world, m_child, m_oldParent);
}

void InstantiateModelCommand::apply(World& world) {
    if (!m_applied) {
        // First run: instantiate and capture snapshots of the new roots.
        const auto created = world.instantiateModel(*m_assets, m_guid);
        assignEditorIds(world);
        for (const entt::entity entity : created) {
            const auto* parent = world.registry.try_get<Parent>(entity);
            const bool isRoot =
                parent == nullptr || !world.registry.valid(parent->value);
            if (isRoot) {
                m_created.push_back(snapshotEntity(world, entity));
            }
        }
        m_applied = true;
        return;
    }
    for (const EntitySnapshot& snapshot : m_created) {
        restoreEntity(world, snapshot);
    }
}

void InstantiateModelCommand::revert(World& world) {
    for (const EntitySnapshot& snapshot : m_created) {
        if (auto entity = findByEditorId(world, snapshot.editorId);
            entity != entt::null) {
            destroyRecursive(world, entity);
        }
    }
}

} // namespace candela::editor
