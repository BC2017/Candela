#pragma once

#include "candela/assets/ModelAsset.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace candela {

class Context;
class Bindless;
class EventBus;

// 64-bit random GUID. Stored as hex in .meta files and scene files.
using AssetGuid = uint64_t;
constexpr AssetGuid kInvalidGuid = 0;

std::string guidToString(AssetGuid guid);
AssetGuid guidFromString(const std::string& text);

struct AssetReloadedEvent {
    AssetGuid guid = kInvalidGuid;
};

// Scans a content directory for importable sources (.gltf/.glb), stamps each
// with a sidecar `<file>.meta` carrying its GUID, and serves loaded assets by
// GUID. Loads run on the job system; consumers poll tryGetModel() and draw
// whatever has arrived (progressive streaming by construction). Source files
// are watched for changes and re-imported in place.
class AssetRegistry {
public:
    AssetRegistry(Context& context, Bindless& bindless, EventBus& events);
    ~AssetRegistry();

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // Walks the content directory, creating .meta files where missing.
    void scan(const std::filesystem::path& contentDir);

    AssetGuid guidForPath(const std::filesystem::path& path) const;
    std::filesystem::path pathForGuid(AssetGuid guid) const;

    // All known assets (for the editor's content browser).
    std::vector<std::pair<AssetGuid, std::filesystem::path>> allAssets() const;

    // Kicks an async import if not already loaded/loading.
    void requestModel(AssetGuid guid);

    // Non-blocking: nullptr until the import finishes.
    const ModelAsset* tryGetModel(AssetGuid guid);

    // Blocking: requests, then waits for completion.
    const ModelAsset* getModelBlocking(AssetGuid guid);

    // Main-thread per-frame tick: publishes finished loads' events and polls
    // source timestamps for hot reload (throttled internally).
    void update();

private:
    enum class State { Unloaded, Loading, Loaded };

    struct Entry {
        std::filesystem::path path;
        State state = State::Unloaded;
        std::unique_ptr<ModelAsset> model;
        std::unique_ptr<ModelAsset> pendingModel; // finished import, awaiting adoption
        std::filesystem::file_time_type sourceTime{};
        bool reloadEventPending = false;
    };

    void startLoad(AssetGuid guid, Entry& entry);

    Context& m_context;
    Bindless& m_bindless;
    EventBus& m_events;

    mutable std::mutex m_mutex;
    std::unordered_map<AssetGuid, Entry> m_entries;
    std::unordered_map<std::string, AssetGuid> m_pathToGuid;
    std::chrono::steady_clock::time_point m_lastWatchPoll{};
};

} // namespace candela
