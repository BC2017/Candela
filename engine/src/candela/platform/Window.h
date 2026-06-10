#pragma once

#include "candela/rhi/VulkanCommon.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace candela {

struct WindowDesc {
    uint32_t width = 1600;
    uint32_t height = 900;
    std::string title = "Candela";
};

class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();
    void waitEvents();

    VkSurfaceKHR createSurface(VkInstance instance) const;
    glm::uvec2 framebufferSize() const;

    // Returns true once after the framebuffer was resized, then clears the flag.
    bool consumeResizeFlag();

    bool isKeyDown(int glfwKey) const;
    bool isMouseButtonDown(int glfwButton) const;

    // Cursor movement since the last pollEvents(), then resets to zero.
    glm::vec2 consumeMouseDelta();

    // Hides and locks the cursor (for mouse-look) when true.
    void setCursorCaptured(bool captured);

    void setTitle(const std::string& title);
    GLFWwindow* handle() const { return m_window; }

private:
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* m_window = nullptr;
    bool m_resized = false;
    bool m_cursorCaptured = false;
    glm::vec2 m_lastCursorPos{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
};

} // namespace candela
