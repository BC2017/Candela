#include "candela/platform/Input.h"

#include "candela/platform/Window.h"

#include <GLFW/glfw3.h>

namespace candela {

void InputActions::bindKey(const std::string& action, int glfwKey) {
    m_bindings[action].keys.push_back(glfwKey);
}

void InputActions::bindMouseButton(const std::string& action, int glfwButton) {
    m_bindings[action].mouseButtons.push_back(glfwButton);
}

bool InputActions::isDown(const Window& window,
                          const std::string& action) const {
    auto it = m_bindings.find(action);
    if (it == m_bindings.end()) {
        return false;
    }
    for (int key : it->second.keys) {
        if (window.isKeyDown(key)) {
            return true;
        }
    }
    for (int button : it->second.mouseButtons) {
        if (window.isMouseButtonDown(button)) {
            return true;
        }
    }
    return false;
}

float InputActions::axis(const Window& window, const std::string& positive,
                         const std::string& negative) const {
    return (isDown(window, positive) ? 1.0f : 0.0f) -
           (isDown(window, negative) ? 1.0f : 0.0f);
}

InputActions InputActions::flyCameraDefaults() {
    InputActions input;
    input.bindKey("move_forward", GLFW_KEY_W);
    input.bindKey("move_back", GLFW_KEY_S);
    input.bindKey("move_right", GLFW_KEY_D);
    input.bindKey("move_left", GLFW_KEY_A);
    input.bindKey("move_up", GLFW_KEY_E);
    input.bindKey("move_down", GLFW_KEY_Q);
    input.bindKey("sprint", GLFW_KEY_LEFT_SHIFT);
    input.bindMouseButton("look", GLFW_MOUSE_BUTTON_RIGHT);
    return input;
}

} // namespace candela
