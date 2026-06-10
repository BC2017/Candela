#pragma once

#include "candela/assets/ModelAsset.h"

#include <filesystem>

namespace candela {

class Context;
class Bindless;

// Locates a Blender executable: the CANDELA_BLENDER environment variable
// first, then PATH, then well-known install locations. Empty when nothing is
// found; the result is computed once and cached for the process lifetime.
std::filesystem::path findBlenderExecutable();

// Converts a .blend file to glTF by running Blender headless with its bundled
// glTF exporter, writing `<dir>/.candela-import/<stem>.glb` next to the
// source. Conversion is skipped while the cached .glb is newer than the
// .blend. Returns the .glb path, or empty on failure (missing Blender,
// export error).
std::filesystem::path convertBlendToGlb(const std::filesystem::path& blendPath);

// Imports a Blender .blend file (meshes, materials, textures, hierarchy) by
// converting it to glTF and routing through importGltfModel. Requires Blender
// to be installed; returns an empty model on conversion failure. Safe to call
// from a job thread.
ModelAsset importBlendModel(Context& context, Bindless& bindless,
                            const std::filesystem::path& path);

} // namespace candela
