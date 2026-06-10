#include "candela/assets/AssetRegistry.h"

#include "candela/assets/BlendImporter.h"
#include "candela/core/Events.h"
#include "candela/core/Jobs.h"
#include "candela/core/Log.h"
#include "candela/rhi/Context.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>

namespace candela {

std::string guidToString(AssetGuid guid) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(guid));
    return buffer;
}

AssetGuid guidFromString(const std::string& text) {
    return static_cast<AssetGuid>(std::strtoull(text.c_str(), nullptr, 16));
}

namespace {

AssetGuid randomGuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    AssetGuid guid = kInvalidGuid;
    while (guid == kInvalidGuid) {
        guid = rng();
    }
    return guid;
}

// Reads the GUID from `<source>.meta`, creating the file if missing.
AssetGuid ensureMeta(const std::filesystem::path& sourcePath) {
    const std::filesystem::path metaPath =
        sourcePath.string() + ".meta";
    if (std::filesystem::exists(metaPath)) {
        std::ifstream file(metaPath);
        nlohmann::json meta = nlohmann::json::parse(file, nullptr,
                                                    /*allow_exceptions=*/false);
        if (!meta.is_discarded() && meta.contains("guid")) {
            const AssetGuid guid =
                guidFromString(meta["guid"].get<std::string>());
            if (guid != kInvalidGuid) {
                return guid;
            }
        }
        CD_WARN("Corrupt meta file, regenerating: {}", metaPath.string());
    }

    const AssetGuid guid = randomGuid();
    nlohmann::json meta;
    meta["guid"] = guidToString(guid);
    meta["type"] = "model";
    std::ofstream file(metaPath);
    file << meta.dump(2) << '\n';
    return guid;
}

} // namespace

AssetRegistry::AssetRegistry(Context& context, Bindless& bindless,
                             EventBus& events)
    : m_context(context), m_bindless(bindless), m_events(events) {}

AssetRegistry::~AssetRegistry() {
    // Block on any in-flight imports, then free GPU data. The GPU may still
    // be rendering with these buffers (destruction order vs. the renderer is
    // the caller's business) — wait for idle before destroying anything.
    for (auto& [guid, entry] : m_entries) {
        while (true) {
            {
                std::scoped_lock lock(m_mutex);
                if (entry.state != State::Loading) {
                    break;
                }
            }
            std::this_thread::yield();
        }
    }
    m_context.waitIdle();
    for (auto& [guid, entry] : m_entries) {
        if (entry.model) {
            entry.model->destroy(m_context);
        }
        if (entry.pendingModel) {
            entry.pendingModel->destroy(m_context);
        }
    }
}

void AssetRegistry::scan(const std::filesystem::path& contentDir) {
    std::error_code ec;
    uint32_t found = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(contentDir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        // Hidden directories hold derived data (.candela-import .glb caches);
        // scanning them would duplicate their source assets.
        if (it->is_directory() &&
            it->path().filename().string().starts_with('.')) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        const auto extension = it->path().extension();
        if (extension != ".gltf" && extension != ".glb" &&
            extension != ".blend") {
            continue;
        }
        const AssetGuid guid = ensureMeta(it->path());

        std::scoped_lock lock(m_mutex);
        Entry& entry = m_entries[guid];
        entry.path = it->path();
        // Normalized key — callers may pass any separator style.
        m_pathToGuid[std::filesystem::weakly_canonical(it->path())
                         .generic_string()] = guid;
        ++found;
    }
    CD_INFO("AssetRegistry: {} importable assets under {}", found,
            contentDir.string());
}

AssetGuid AssetRegistry::guidForPath(const std::filesystem::path& path) const {
    std::error_code ec;
    const std::string key =
        std::filesystem::weakly_canonical(path, ec).generic_string();
    std::scoped_lock lock(m_mutex);
    auto it = m_pathToGuid.find(key);
    return it != m_pathToGuid.end() ? it->second : kInvalidGuid;
}

std::filesystem::path AssetRegistry::pathForGuid(AssetGuid guid) const {
    std::scoped_lock lock(m_mutex);
    auto it = m_entries.find(guid);
    return it != m_entries.end() ? it->second.path : std::filesystem::path{};
}

std::vector<std::pair<AssetGuid, std::filesystem::path>>
AssetRegistry::allAssets() const {
    std::scoped_lock lock(m_mutex);
    std::vector<std::pair<AssetGuid, std::filesystem::path>> result;
    result.reserve(m_entries.size());
    for (const auto& [guid, entry] : m_entries) {
        result.emplace_back(guid, entry.path);
    }
    return result;
}

void AssetRegistry::startLoad(AssetGuid guid, Entry& entry) {
    // Caller holds m_mutex.
    entry.state = State::Loading;
    std::error_code ec;
    entry.sourceTime = std::filesystem::last_write_time(entry.path, ec);

    const std::filesystem::path path = entry.path;
    JobSystem::submit([this, guid, path] {
        ModelAsset imported =
            path.extension() == ".blend"
                ? importBlendModel(m_context, m_bindless, path)
                : importGltfModel(m_context, m_bindless, path);
        std::scoped_lock lock(m_mutex);
        Entry& target = m_entries[guid];
        target.pendingModel = std::make_unique<ModelAsset>(std::move(imported));
        target.state = State::Loaded;
        target.reloadEventPending = true;
    });
}

void AssetRegistry::requestModel(AssetGuid guid) {
    std::scoped_lock lock(m_mutex);
    auto it = m_entries.find(guid);
    if (it == m_entries.end()) {
        CD_WARN("requestModel: unknown asset {}", guidToString(guid));
        return;
    }
    if (it->second.state == State::Unloaded) {
        startLoad(guid, it->second);
    }
}

const ModelAsset* AssetRegistry::tryGetModel(AssetGuid guid) {
    std::scoped_lock lock(m_mutex);
    auto it = m_entries.find(guid);
    if (it == m_entries.end()) {
        return nullptr;
    }
    return it->second.model.get();
}

const ModelAsset* AssetRegistry::getModelBlocking(AssetGuid guid) {
    requestModel(guid);
    while (true) {
        update();
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_entries.find(guid);
            if (it == m_entries.end()) {
                return nullptr;
            }
            if (it->second.model) {
                return it->second.model.get();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void AssetRegistry::update() {
    // Adopt finished imports on the main thread. Replacing a live model's GPU
    // buffers requires the GPU to be done with them.
    std::vector<AssetGuid> reloaded;
    {
        std::scoped_lock lock(m_mutex);
        for (auto& [guid, entry] : m_entries) {
            if (!entry.pendingModel) {
                continue;
            }
            if (entry.model) {
                m_context.waitIdle();
                entry.model->destroy(m_context);
            }
            entry.model = std::move(entry.pendingModel);
            if (entry.reloadEventPending) {
                entry.reloadEventPending = false;
                reloaded.push_back(guid);
            }
        }
    }
    for (AssetGuid guid : reloaded) {
        m_events.publish(AssetReloadedEvent{guid});
    }

    // Source watch, throttled to 1 Hz.
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastWatchPoll < std::chrono::seconds(1)) {
        return;
    }
    m_lastWatchPoll = now;

    std::scoped_lock lock(m_mutex);
    for (auto& [guid, entry] : m_entries) {
        if (entry.state != State::Loaded || !entry.model) {
            continue;
        }
        std::error_code ec;
        const auto sourceTime =
            std::filesystem::last_write_time(entry.path, ec);
        if (ec || sourceTime == entry.sourceTime) {
            continue;
        }
        CD_INFO("Source changed, re-importing: {}", entry.path.string());
        startLoad(guid, entry);
    }
}

} // namespace candela
