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
    if (p_glfwGetWin32Window == nullptr) {
        std::cerr << "[Window] glfwGetWin32Window is not resolved!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
    HWND hwnd = reinterpret_cast<HWND>(GLFW_GetWin32Window(window_));
    std::cerr << "[Window] glfwWindow=" << window_ << " hwnd=" << hwnd << std::endl;
    if (hwnd == nullptr) {
        std::cerr << "[Window] glfwGetWin32Window returned a null HWND!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
    if (vkCreateWin32SurfaceKHR == nullptr) {
        std::cerr << "[Window] vkCreateWin32SurfaceKHR was not loaded by volk!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    createInfo.hwnd = hwnd;
    std::cerr << "[Window] creating Win32 surface (hinstance=" << createInfo.hinstance << ")..." << std::endl;
    result = vkCreateWin32SurfaceKHR(instance_->vkInstance(), &createInfo, nullptr, &surface_);
    std::cerr << "[Window] vkCreateWin32SurfaceKHR result=" << result << " surface=" << surface_ << std::endl;
#else
    result = GLFW_CreateWindowSurface(instance_->vkInstance(), window_, nullptr, &surface_);
#endif
    if (result != VK_SUCCESS) {
        std::cerr << "Cannot create vulkan window surface!" << std::endl;
        GLFW_Terminate();
        exit(EXIT_FAILURE);
    }
    std::cerr << "[Window] surface created OK; proceeding to physical-device/swapchain" << std::endl;
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
