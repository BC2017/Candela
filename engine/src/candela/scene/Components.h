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

// --- Physics (Jolt-backed; see physics/PhysicsSystem.h). Runtime handles are
// opaque integers so this header never includes Jolt. -----------------------

// Sentinel matching JPH::BodyID::cInvalidBodyID without pulling in Jolt.
inline constexpr uint32_t kInvalidBodyId = 0xffffffffu;

// A rigid body simulated by the PhysicsSystem. Static/kinematic bodies are
// world geometry; dynamic bodies have their pose written back to
// LocalTransform each frame.
struct RigidBody {
    enum class MotionType : uint8_t { Static, Kinematic, Dynamic };
    enum class ShapeType : uint8_t { Box, Sphere, Capsule };

    MotionType motionType = MotionType::Dynamic;
    ShapeType shape = ShapeType::Box;

    // Box    -> halfExtents (x, y, z).
    // Sphere -> radius = halfExtents.x.
    // Capsule-> radius = halfExtents.x, half cylinder height = halfExtents.y.
    glm::vec3 halfExtents{0.5f};

    float mass = 1.0f; // ignored for Static/Kinematic
    float friction = 0.5f;
    float restitution = 0.0f;

    // Runtime — assigned by PhysicsSystem on first update; never authored or
    // serialized.
    uint32_t bodyId = kInvalidBodyId;
};

// A capsule character controller (the player). The backing JPH::Character
// lives in the PhysicsSystem; its pose is synced back to LocalTransform.
struct CharacterController {
    float radius = 0.3f;     // capsule radius
    float halfHeight = 0.6f; // half height of the cylindrical section
    float mass = 75.0f;
    float friction = 0.5f;

    // Runtime — the backing body's id; never authored or serialized.
    uint32_t bodyId = kInvalidBodyId;
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
