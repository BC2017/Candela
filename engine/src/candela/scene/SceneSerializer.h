#pragma once

#include <filesystem>
#include <string>

namespace candela {

class World;
class AssetRegistry;

// .candela scene files: JSON with scene settings and an entity array.
// Save and load are deterministic — loading a scene and saving it again
// produces byte-identical output (the Phase 3 round-trip guarantee).
namespace SceneSerializer {

bool save(const World& world, const std::filesystem::path& path);

// Clears the world and rebuilds it from the file. Models referenced by
// MeshRenderers are requested asynchronously — geometry streams in as
// imports complete.
bool load(World& world, AssetRegistry& assets,
          const std::filesystem::path& path);

// In-memory variants (editor play-mode snapshots).
std::string saveToString(const World& world);
bool loadFromString(World& world, AssetRegistry& assets,
                    const std::string& text);

} // namespace SceneSerializer

} // namespace candela
