#include "candela/renderer/Camera.h"

#include "candela/platform/Window.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace candela {

void Camera::update(Window& window, float dt) {
    const bool looking = window.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
    window.setCursorCaptured(looking);

    const glm::vec2 mouseDelta = window.consumeMouseDelta();
    if (looking) {
        yawRadians -= mouseDelta.x * lookSensitivity;
        pitchRadians -= mouseDelta.y * lookSensitivity;
        pitchRadians = std::clamp(pitchRadians, glm::radians(-89.0f),
                                  glm::radians(89.0f));
    }

    const glm::vec3 fwd = forward();
    const glm::vec3 right =
        glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));

    glm::vec3 move{0.0f};
    if (window.isKeyDown(GLFW_KEY_W)) move += fwd;
    if (window.isKeyDown(GLFW_KEY_S)) move -= fwd;
    if (window.isKeyDown(GLFW_KEY_D)) move += right;
    if (window.isKeyDown(GLFW_KEY_A)) move -= right;
    if (window.isKeyDown(GLFW_KEY_E)) move.y += 1.0f;
    if (window.isKeyDown(GLFW_KEY_Q)) move.y -= 1.0f;

    if (glm::dot(move, move) > 0.0f) {
        const float speed = window.isKeyDown(GLFW_KEY_LEFT_SHIFT)
                                ? moveSpeed * 4.0f
                                : moveSpeed;
        position += glm::normalize(move) * speed * dt;
    }
}

glm::vec3 Camera::forward() const {
    return {std::cos(pitchRadians) * std::sin(yawRadians) * -1.0f,
            std::sin(pitchRadians),
            std::cos(pitchRadians) * std::cos(yawRadians) * -1.0f};
}

glm::mat4 Camera::view() const {
    return glm::lookAt(position, position + forward(),
                       glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::projection(float aspect) const {
    // Reverse-Z infinite perspective, right-handed, depth range [0,1].
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][3] = -1.0f;
    proj[3][2] = nearPlane;
    return proj;
}

glm::mat4 Camera::viewProjection(float aspect) const {
    return projection(aspect) * view();
}

} // namespace candela
