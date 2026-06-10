#include "candela/assets/BlendImporter.h"

#include "candela/core/Log.h"

#include <tracy/Tracy.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace candela {

namespace {

#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
constexpr const char* kBlenderExeName = "blender.exe";
#else
constexpr char kPathListSeparator = ':';
constexpr const char* kBlenderExeName = "blender";
#endif

std::filesystem::path searchPathEnv() {
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) {
        return {};
    }
    std::istringstream stream(pathEnv);
    std::string entry;
    std::error_code ec;
    while (std::getline(stream, entry, kPathListSeparator)) {
        if (entry.empty()) {
            continue;
        }
        const std::filesystem::path candidate =
            std::filesystem::path(entry) / kBlenderExeName;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path searchKnownInstallDirs() {
    std::error_code ec;
#if defined(_WIN32)
    // Versioned folders ("Blender 4.2", ...) under the default install root;
    // lexicographically-last picks the newest among same-width versions.
    const std::filesystem::path root = "C:/Program Files/Blender Foundation";
    std::filesystem::path best;
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        const std::filesystem::path candidate = it->path() / kBlenderExeName;
        if (std::filesystem::exists(candidate, ec) &&
            (best.empty() || best < candidate)) {
            best = candidate;
        }
    }
    return best;
#elif defined(__APPLE__)
    const std::filesystem::path candidate =
        "/Applications/Blender.app/Contents/MacOS/Blender";
    return std::filesystem::exists(candidate, ec) ? candidate
                                                  : std::filesystem::path{};
#else
    for (const char* candidate :
         {"/usr/bin/blender", "/usr/local/bin/blender", "/snap/bin/blender"}) {
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
#endif
}

std::string quoted(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

} // namespace

std::filesystem::path findBlenderExecutable() {
    static const std::filesystem::path cached = [] {
        if (const char* env = std::getenv("CANDELA_BLENDER")) {
            std::error_code ec;
            const std::filesystem::path explicitPath = env;
            if (std::filesystem::exists(explicitPath, ec)) {
                return explicitPath;
            }
            CD_WARN("CANDELA_BLENDER is set but does not exist: {}",
                    explicitPath.string());
        }
        if (auto found = searchPathEnv(); !found.empty()) {
            return found;
        }
        return searchKnownInstallDirs();
    }();
    return cached;
}

std::filesystem::path convertBlendToGlb(
    const std::filesystem::path& blendPath) {
    ZoneScoped;

    std::error_code ec;
    const std::filesystem::path cacheDir =
        blendPath.parent_path() / ".candela-import";
    const std::filesystem::path glbPath =
        cacheDir / (blendPath.stem().string() + ".glb");

    const auto sourceTime = std::filesystem::last_write_time(blendPath, ec);
    if (!ec && std::filesystem::exists(glbPath, ec) &&
        std::filesystem::last_write_time(glbPath, ec) >= sourceTime) {
        return glbPath;
    }

    const std::filesystem::path blender = findBlenderExecutable();
    if (blender.empty()) {
        CD_ERROR("Cannot import {}: Blender not found — install Blender or "
                 "set CANDELA_BLENDER to the executable",
                 blendPath.string());
        return {};
    }

    std::filesystem::create_directories(cacheDir, ec);

    // Exporter options stay minimal so the script works across Blender
    // releases (2.83+): GLB output (textures embedded), modifiers applied,
    // +Y up to match the glTF convention the rest of the pipeline assumes.
    const std::filesystem::path scriptPath =
        cacheDir / (blendPath.stem().string() + ".export.py");
    {
        std::ofstream script(scriptPath);
        if (!script) {
            CD_ERROR("Failed to write export script: {}", scriptPath.string());
            return {};
        }
        script << "import bpy\n"
               << "bpy.ops.export_scene.gltf(\n"
               << "    filepath=\"" << glbPath.generic_string() << "\",\n"
               << "    export_format='GLB',\n"
               << "    export_apply=True,\n"
               << "    export_yup=True,\n"
               << ")\n";
    }

    std::string command = quoted(blender) +
                          " --background --factory-startup"
                          " --python-exit-code 1 " +
                          quoted(blendPath) + " --python " +
                          quoted(scriptPath);
#if defined(_WIN32)
    // system() hands the line to `cmd /c`, which strips the outermost quote
    // pair; wrap the whole command so the per-path quotes survive.
    command = "\"" + command + "\"";
#endif

    CD_INFO("Converting {} with Blender ({})", blendPath.filename().string(),
            blender.string());
    const auto start = std::chrono::steady_clock::now();
    int exitCode = std::system(command.c_str());
#if !defined(_WIN32)
    // POSIX system() returns a waitpid status; extract the exit code.
    if (exitCode > 0) {
        exitCode = (exitCode >> 8) & 0xFF;
    }
#endif
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    std::filesystem::remove(scriptPath, ec);

    if (exitCode != 0 || !std::filesystem::exists(glbPath, ec)) {
        CD_ERROR("Blender glTF export failed for {} (exit code {})",
                 blendPath.string(), exitCode);
        // Drop any stale/partial cache so the next attempt reconverts.
        std::filesystem::remove(glbPath, ec);
        return {};
    }

    CD_INFO("Converted {} -> {} in {} ms", blendPath.filename().string(),
            glbPath.filename().string(), elapsedMs);
    return glbPath;
}

ModelAsset importBlendModel(Context& context, Bindless& bindless,
                            const std::filesystem::path& path) {
    ZoneScoped;
    const std::filesystem::path glbPath = convertBlendToGlb(path);
    if (glbPath.empty()) {
        // Errors already logged; an empty model instantiates as an empty
        // root entity instead of crashing the load.
        return {};
    }
    return importGltfModel(context, bindless, glbPath);
}

} // namespace candela
