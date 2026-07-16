#pragma once

#include "candela/assets/AssetRegistry.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <vector>

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

// A skinned mesh instance. Lives on the node entity that carried a skinIndex,
// alongside a Skeleton (and usually an Animator). The renderer pre-skins it
// into a per-frame buffer instead of drawing the bind pose.
struct SkinnedMeshRenderer {
    AssetGuid model = kInvalidGuid;
    uint32_t meshIndex = 0;
    int skinIndex = -1; // index into ModelAsset::skins
};

// The joint entities driving a SkinnedMeshRenderer, in palette order. joints[j]
// is the entity whose WorldTransform gives joint j's world matrix; inverseBind
// and jointNodeIndex are parallel arrays. jointNodeIndex maps each joint to its
// model node index so the animator can resolve channel targets.
struct Skeleton {
    std::vector<entt::entity> joints;
    std::vector<glm::mat4> inverseBind;
    std::vector<int> jointNodeIndex;
};

// Drives one clip on the skeleton this entity owns.
struct Animator {
    AssetGuid model = kInvalidGuid;
    int clip = 0;    // index into ModelAsset::animations
    float time = 0.0f;
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
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
