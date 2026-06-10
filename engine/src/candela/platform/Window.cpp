#include "candela/platform/Window.h"

#include <GLFW/glfw3.h>

namespace candela {

namespace {
uint32_t s_windowCount = 0;
}

Window::Window(const WindowDesc& desc) {
    if (s_windowCount++ == 0) {
        glfwSetErrorCallback([](int code, const char* message) {
            CD_ERROR("[glfw] {} (code {})", message, code);
        });
        CD_ASSERT(glfwInit() == GLFW_TRUE, "glfwInit failed");
    }
    CD_ASSERT(glfwVulkanSupported() == GLFW_TRUE,
              "GLFW could not find the Vulkan loader");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(static_cast<int>(desc.width),
                                static_cast<int>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    CD_ASSERT(m_window != nullptr, "Failed to create window");

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, &Window::framebufferSizeCallback);
    glfwSetKeyCallback(m_window, [](GLFWwindow* window, int key, int /*scancode*/,
                                    int action, int /*mods*/) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    });
}

Window::~Window() {
    glfwDestroyWindow(m_window);
    if (--s_windowCount == 0) {
        glfwTerminate();
    }
}

void Window::framebufferSizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    static_cast<Window*>(glfwGetWindowUserPointer(window))->m_resized = true;
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

void Window::pollEvents() {
    glfwPollEvents();

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(m_window, &x, &y);
    const glm::vec2 cursor{static_cast<float>(x), static_cast<float>(y)};
    m_mouseDelta = cursor - m_lastCursorPos;
    m_lastCursorPos = cursor;
}

bool Window::isKeyDown(int glfwKey) const {
    return glfwGetKey(m_window, glfwKey) == GLFW_PRESS;
}

bool Window::isMouseButtonDown(int glfwButton) const {
    return glfwGetMouseButton(m_window, glfwButton) == GLFW_PRESS;
}

glm::vec2 Window::consumeMouseDelta() {
    const glm::vec2 delta = m_mouseDelta;
    m_mouseDelta = {0.0f, 0.0f};
    return delta;
}

void Window::setCursorCaptured(bool captured) {
    if (captured == m_cursorCaptured) {
        return;
    }
    m_cursorCaptured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION,
                         captured ? GLFW_TRUE : GLFW_FALSE);
    }
    // Swallow the cursor jump produced by the mode change.
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(m_window, &x, &y);
    m_lastCursorPos = {static_cast<float>(x), static_cast<float>(y)};
    m_mouseDelta = {0.0f, 0.0f};
}

void Window::waitEvents() {
    glfwWaitEvents();
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VK_CHECK(glfwCreateWindowSurface(instance, m_window, nullptr, &surface));
    return surface;
}

glm::uvec2 Window::framebufferSize() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

bool Window::consumeResizeFlag() {
    const bool resized = m_resized;
    m_resized = false;
    return resized;
}

void Window::setTitle(const std::string& title) {
    glfwSetWindowTitle(m_window, title.c_str());
}

} // namespace candela
