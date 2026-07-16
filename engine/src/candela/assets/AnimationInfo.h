#pragma once

#include <filesystem>

namespace candela {

// GPU-free glTF inspector: parses the file for skins and animation clips (no
// Context, no Bindless, no job system) and prints a summary to stdout. Backs
// the sandbox `--animinfo <path>` flag, which returns before JobSystem::init().
void printAnimationInfo(const std::filesystem::path& path);

} // namespace candela
