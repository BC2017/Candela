#include "candela/renderer/Cascades.h"

#include "candela/renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace candela {

CascadeSet computeCascades(const Camera& camera, float aspect, glm::vec3 toSun,
                           float maxShadowDistance, uint32_t shadowMapSize) {
    CascadeSet cascades;

    // Practical split scheme: blend of logarithmic and uniform.
    const float nearPlane = camera.nearPlane;
    const float lambda = 0.75f;
    float splits[CascadeSet::kCount];
    for (uint32_t i = 0; i < CascadeSet::kCount; ++i) {
        const float p = static_cast<float>(i + 1) / CascadeSet::kCount;
        const float logSplit =
            nearPlane * std::pow(maxShadowDistance / nearPlane, p);
        const float uniformSplit =
            nearPlane + (maxShadowDistance - nearPlane) * p;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
    cascades.splitDepths = {splits[0], splits[1], splits[2], splits[3]};

    const glm::mat4 invView = glm::inverse(camera.view());
    const float tanHalfFovY = std::tan(camera.fovYRadians * 0.5f);
    const float tanHalfFovX = tanHalfFovY * aspect;
    const glm::vec3 sunDir = glm::normalize(toSun);

    float sliceNear = nearPlane;
    for (uint32_t cascade = 0; cascade < CascadeSet::kCount; ++cascade) {
        const float sliceFar = splits[cascade];

        // Frustum slice corners in world space.
        glm::vec3 corners[8];
        uint32_t cornerIndex = 0;
        for (float depth : {sliceNear, sliceFar}) {
            const float halfHeight = tanHalfFovY * depth;
            const float halfWidth = tanHalfFovX * depth;
            for (float ySign : {-1.0f, 1.0f}) {
                for (float xSign : {-1.0f, 1.0f}) {
                    const glm::vec4 viewCorner{xSign * halfWidth,
                                               ySign * halfHeight, -depth,
                                               1.0f};
                    corners[cornerIndex++] = glm::vec3(invView * viewCorner);
                }
            }
        }

        // Bounding sphere keeps the ortho size stable while the camera
        // rotates (prevents per-frame shimmer from extent changes).
        glm::vec3 center{0.0f};
        for (const glm::vec3& corner : corners) {
            center += corner;
        }
        center /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& corner : corners) {
            radius = (std::max)(radius, glm::length(corner - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // Snap the center to shadow-texel increments in light space.
        const glm::vec3 up = std::abs(sunDir.y) > 0.95f
                                 ? glm::vec3(0.0f, 0.0f, 1.0f)
                                 : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightView =
            glm::lookAt(center + sunDir * radius, center, up);
        const float texelSize =
            (2.0f * radius) / static_cast<float>(shadowMapSize);
        glm::vec4 lightSpaceCenter = lightView * glm::vec4(center, 1.0f);
        lightSpaceCenter.x = std::floor(lightSpaceCenter.x / texelSize) * texelSize;
        lightSpaceCenter.y = std::floor(lightSpaceCenter.y / texelSize) * texelSize;
        const glm::vec3 snappedCenter =
            glm::vec3(glm::inverse(lightView) * lightSpaceCenter);

        const glm::mat4 snappedView =
            glm::lookAt(snappedCenter + sunDir * radius, snappedCenter, up);
        // Pull the near plane back so casters between the sun and the slice
        // (e.g. Sponza's roof) still shadow it.
        const float casterPullback = 4.0f * radius;
        const glm::mat4 lightProjection = glm::orthoRH_ZO(
            -radius, radius, -radius, radius, -casterPullback, radius * 2.0f);

        cascades.viewProjection[cascade] = lightProjection * snappedView;
        sliceNear = sliceFar;
    }

    return cascades;
}

} // namespace candela
