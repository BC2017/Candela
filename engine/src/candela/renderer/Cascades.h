#pragma once

#include <glm/glm.hpp>

#include <array>

namespace candela {

class Camera;

struct CascadeSet {
    static constexpr uint32_t kCount = 4;
    std::array<glm::mat4, kCount> viewProjection;
    // View-space distance at which each cascade ends (for cascade selection).
    glm::vec4 splitDepths{0.0f};
};

// Fits kCount orthographic cascades along the camera frustum out to
// maxShadowDistance, with texel snapping to stop shadow shimmer.
// `toSun` points from the scene toward the sun.
CascadeSet computeCascades(const Camera& camera, float aspect, glm::vec3 toSun,
                           float maxShadowDistance, uint32_t shadowMapSize);

} // namespace candela
