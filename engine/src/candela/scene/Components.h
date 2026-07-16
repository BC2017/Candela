#pragma once

#include "candela/assets/AssetRegistry.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <string>

namespace candela {

struct Name {
    std::string value;
};

struct LocalTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
};

struct Parent {
    entt::entity value = entt::null;
};

// Computed by World::updateTransforms each frame.
struct WorldTransform {
    glm::mat4 value{1.0f};
};

struct MeshRenderer {
    AssetGuid model = kInvalidGuid;
    uint32_t meshIndex = 0;
};

struct PointLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float radius = 10.0f;
};

// Authored camera (unused until play-in-editor; the sandbox fly camera is
// engine-side). Serialized so scenes round-trip completely.
struct CameraComponent {
    float fovYDegrees = 70.0f;
    float nearPlane = 0.05f;
};

// Marks the entity whose WorldTransform drives the 3D audio listener (usually
// the camera/player). If several exist, the AudioSystem uses the first active.
struct AudioListener {
    bool active = true;
};

// A sound emitter driven by the AudioSystem. `clip` is a filesystem path to a
// .wav/.ogg/.mp3 (AssetGuid routing is a follow-up — the registry currently
// models only ModelAsset). `instance`/`started` are runtime-only and are NOT
// serialized, so scene save → load → save round-trips byte-stable.
struct AudioSource {
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatial = true;  // false = flat 2D
    bool autoplay = true; // start once on the first AudioSystem tick
    float minDistance = 1.0f;
    float maxDistance = 100.0f;

    // Runtime state (not serialized).
    uint32_t instance = 0; // AudioEngine::InstanceId; 0 = not started
    bool started = false;
};

// Per-scene lighting/environment settings, stored in registry context.
struct SceneSettings {
    glm::vec3 toSun = glm::normalize(glm::vec3(0.25f, 1.0f, 0.12f));
    float sunIntensity = 6.0f;
    glm::vec3 sunColor{1.0f, 0.96f, 0.88f};
    float iblIntensity = 0.8f;
    float exposure = 1.0f;
    float bloomStrength = 0.05f;
    // Ray-traced effects (ignored on devices without ray tracing — the
    // raster fallbacks render instead).
    bool rtShadows = true;
    bool rtAmbientOcclusion = true;
    bool rtReflections = true;
    // Temporal anti-aliasing (also drives sub-pixel jitter; the RT temporal
    // accumulation works regardless).
    bool taa = true;
};

} // namespace candela
