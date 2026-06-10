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
