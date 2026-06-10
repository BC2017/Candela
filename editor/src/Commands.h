#pragma once

#include <candela/scene/World.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace candela::editor {

// Stable editor-side identity. Entity handles change when delete is undone,
// so commands reference entities by EditorId and resolve at execution time.
struct EditorId {
    uint64_t value = 0;
};

uint64_t assignEditorIds(World& world); // returns ids assigned (idempotent)
entt::entity findByEditorId(World& world, uint64_t id);
uint64_t editorIdOf(World& world, entt::entity entity);
uint64_t reserveEditorId(); // for snapshots describing not-yet-created entities

// Recursive component snapshot used by delete/recreate.
struct EntitySnapshot {
    uint64_t editorId = 0;
    std::string name;
    LocalTransform transform;
    uint64_t parentEditorId = 0; // 0 = root
    std::optional<MeshRenderer> meshRenderer;
    std::optional<PointLightComponent> pointLight;
    std::optional<CameraComponent> camera;
    std::vector<EntitySnapshot> children;
};

EntitySnapshot snapshotEntity(World& world, entt::entity entity);
entt::entity restoreEntity(World& world, const EntitySnapshot& snapshot,
                           uint64_t parentOverride = 0);

class EditCommand {
public:
    virtual ~EditCommand() = default;
    virtual void apply(World& world) = 0;
    virtual void revert(World& world) = 0;
    virtual const char* label() const = 0;
};

class CommandStack {
public:
    // Applies the command and pushes it (clears the redo tail).
    void perform(World& world, std::unique_ptr<EditCommand> command);

    bool canUndo() const { return m_next > 0; }
    bool canRedo() const { return m_next < m_commands.size(); }
    void undo(World& world);
    void redo(World& world);
    void clear();
    const char* undoLabel() const;
    const char* redoLabel() const;
    size_t depth() const { return m_next; }

private:
    std::vector<std::unique_ptr<EditCommand>> m_commands;
    size_t m_next = 0; // index of the next redo slot
};

// --- Concrete commands ---

class TransformCommand : public EditCommand {
public:
    TransformCommand(uint64_t id, LocalTransform before, LocalTransform after)
        : m_id(id), m_before(before), m_after(after) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Transform"; }

private:
    uint64_t m_id;
    LocalTransform m_before;
    LocalTransform m_after;
};

class RenameCommand : public EditCommand {
public:
    RenameCommand(uint64_t id, std::string before, std::string after)
        : m_id(id), m_before(std::move(before)), m_after(std::move(after)) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Rename"; }

private:
    uint64_t m_id;
    std::string m_before;
    std::string m_after;
};

// Edit/add/remove for value components (PointLightComponent, CameraComponent).
template <typename T>
class ComponentEditCommand : public EditCommand {
public:
    ComponentEditCommand(uint64_t id, T before, T after)
        : m_id(id), m_before(before), m_after(after) {}
    void apply(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.emplace_or_replace<T>(entity, m_after);
        }
    }
    void revert(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.emplace_or_replace<T>(entity, m_before);
        }
    }
    const char* label() const override { return "Edit Component"; }

private:
    uint64_t m_id;
    T m_before;
    T m_after;
};

template <typename T>
class AddComponentCommand : public EditCommand {
public:
    AddComponentCommand(uint64_t id, T value) : m_id(id), m_value(value) {}
    void apply(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.emplace_or_replace<T>(entity, m_value);
        }
    }
    void revert(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.remove<T>(entity);
        }
    }
    const char* label() const override { return "Add Component"; }

private:
    uint64_t m_id;
    T m_value;
};

template <typename T>
class RemoveComponentCommand : public EditCommand {
public:
    RemoveComponentCommand(uint64_t id, T removedValue)
        : m_id(id), m_value(removedValue) {}
    void apply(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.remove<T>(entity);
        }
    }
    void revert(World& world) override {
        if (auto entity = findByEditorId(world, m_id); entity != entt::null) {
            world.registry.emplace_or_replace<T>(entity, m_value);
        }
    }
    const char* label() const override { return "Remove Component"; }

private:
    uint64_t m_id;
    T m_value;
};

class CreateEntityCommand : public EditCommand {
public:
    explicit CreateEntityCommand(EntitySnapshot snapshot)
        : m_snapshot(std::move(snapshot)) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Create Entity"; }
    uint64_t editorId() const { return m_snapshot.editorId; }

private:
    EntitySnapshot m_snapshot;
};

class DeleteEntityCommand : public EditCommand {
public:
    explicit DeleteEntityCommand(EntitySnapshot snapshot)
        : m_snapshot(std::move(snapshot)) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Delete Entity"; }

private:
    EntitySnapshot m_snapshot;
};

class ReparentCommand : public EditCommand {
public:
    ReparentCommand(uint64_t child, uint64_t oldParent, uint64_t newParent)
        : m_child(child), m_oldParent(oldParent), m_newParent(newParent) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Reparent"; }

private:
    static void setParentById(World& world, uint64_t child, uint64_t parent);
    uint64_t m_child;
    uint64_t m_oldParent;
    uint64_t m_newParent;
};

class InstantiateModelCommand : public EditCommand {
public:
    explicit InstantiateModelCommand(AssetGuid guid, AssetRegistry& assets)
        : m_guid(guid), m_assets(&assets) {}
    void apply(World& world) override;
    void revert(World& world) override;
    const char* label() const override { return "Instantiate Model"; }

private:
    AssetGuid m_guid;
    AssetRegistry* m_assets;
    std::vector<EntitySnapshot> m_created; // captured on first apply
    bool m_applied = false;
};

class SettingsCommand : public EditCommand {
public:
    SettingsCommand(SceneSettings before, SceneSettings after)
        : m_before(before), m_after(after) {}
    void apply(World& world) override { world.settings = m_after; }
    void revert(World& world) override { world.settings = m_before; }
    const char* label() const override { return "Scene Settings"; }

private:
    SceneSettings m_before;
    SceneSettings m_after;
};

} // namespace candela::editor
