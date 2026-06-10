#pragma once

#include <glm/glm.hpp>

namespace candela {

class Window;

// Fly camera: WASD + QE to move (Shift to sprint), hold right mouse to look.
// Produces a reverse-Z infinite perspective projection (depth 1 at the near
// plane, 0 at infinity) with the Vulkan Y flip handled by a negative viewport.
class Camera {
public:
    void update(Window& window, float dt);

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
    glm::mat4 viewProjection(float aspect) const;

    glm::vec3 position{0.0f, 1.5f, 0.0f};
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float fovYRadians = glm::radians(70.0f);
    float nearPlane = 0.05f;
    float moveSpeed = 3.0f;
    float lookSensitivity = 0.0022f;

private:
    glm::vec3 forward() const;
};

} // namespace candela
