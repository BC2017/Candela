#pragma once

#include <glm/vec2.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace candela {

class Window;

// Maps named actions to physical inputs so gameplay/camera code stops
// hardcoding key constants. Axis pairs combine two actions into [-1, 1].
//
//   InputActions input;
//   input.bindKey("move_forward", GLFW_KEY_W);
//   if (input.isDown(window, "move_forward")) ...
class InputActions {
public:
    void bindKey(const std::string& action, int glfwKey);
    void bindMouseButton(const std::string& action, int glfwButton);

    bool isDown(const Window& window, const std::string& action) const;

    // (positive - negative) per component, e.g. axis("move_right","move_left").
    float axis(const Window& window, const std::string& positive,
               const std::string& negative) const;

    // WASD-style defaults used by the fly camera.
    static InputActions flyCameraDefaults();

private:
    struct Binding {
        std::vector<int> keys;
        std::vector<int> mouseButtons;
    };
    std::unordered_map<std::string, Binding> m_bindings;
};

} // namespace candela
