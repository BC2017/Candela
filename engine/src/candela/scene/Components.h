#pragma once

#include "candela/assets/AssetRegistry.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <entt/entt.hpp>

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
};

} // namespace candela
