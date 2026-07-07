#include "core/vulkan/window.hpp"

#include "core/vulkan/instance.hpp"

#include <iostream>

bool vk::Window::framebufferResized = false;

vk::Window::Window(std::shared_ptr<Instance> instance, uint32_t width, uint32_t height)
    : instance_(instance), width_(width), height_(height) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // TODO: enable this
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_ = glfwCreateWindow(width_, height_, "Vulkan Window", nullptr, nullptr);
    if (!window_) {
        std::cerr << "Cannot create glfw window!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }

    VkResult result = GLFW_CreateWindowSurface(instance_->vkInstance(), window_, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        std::cerr << "Cannot create vulkan window surface!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
}

vk::Window::Window(std::shared_ptr<Instance> instance, GLFWwindow *window_) : instance_(instance), window_(window_) {
    GLFW_GetWindowSize(window_, reinterpret_cast<int *>(&width_), reinterpret_cast<int *>(&height_));
    // Radiance runs Minecraft on its own GL backend (so MC's GlDevice initialises with a real GL
    // context and the mod's GL-command capture mixins fire). That means this window keeps a GL
    // context and is NOT GLFW_NO_API, so glfwCreateWindowSurface would reject it ("requires the
    // window to have the client API set to GLFW_NO_API"). Create the Vulkan surface from the native
    // window handle instead -- VK_KHR_win32_surface is already enabled (glfwGetRequiredInstanceExtensions).
    VkResult result;
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = GetModuleHandle(nullptr);
    createInfo.hwnd = reinterpret_cast<HWND>(GLFW_GetWin32Window(window_));
    result = vkCreateWin32SurfaceKHR(instance_->vkInstance(), &createInfo, nullptr, &surface_);
#else
    result = GLFW_CreateWindowSurface(instance_->vkInstance(), window_, nullptr, &surface_);
#endif
    if (result != VK_SUCCESS) {
        std::cerr << "Cannot create vulkan window surface!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
}

vk::Window::~Window() {
    vkDestroySurfaceKHR(instance_->vkInstance(), surface_, nullptr);

#ifdef DEBUG
    std::cout << "[Window] window deconstructed" << std::endl;
#endif
}

uint32_t vk::Window::width() {
    return width_;
}

uint32_t vk::Window::height() {
    return height_;
}

GLFWwindow *vk::Window::window() {
    return window_;
}

VkSurfaceKHR &vk::Window::vkSurface() {
    return surface_;
}
