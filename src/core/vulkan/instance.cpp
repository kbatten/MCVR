#include "core/vulkan/instance.hpp"

#include "core/render/modules/world/dlss/dlss_wrapper.hpp"
#include "core/render/modules/world/xess_upscaler/xess_wrapper.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_set>
#include <vector>

const char *DEBUG_LAYER = "VK_LAYER_KHRONOS_validation";

std::ostream &instanceCout() {
    return std::cout << "[Instance] ";
}

std::ostream &instanceCerr() {
    return std::cerr << "[Instance] ";
}

// Debug callback
VkBool32 debugCallback(VkDebugReportFlagsEXT flags,
                       VkDebugReportObjectTypeEXT objType,
                       uint64_t srcObject,
                       size_t location,
                       int32_t msgCode,
                       const char *pLayerPrefix,
                       const char *pMsg,
                       void *pUserData) {
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        instanceCerr() << "ERROR: [" << pLayerPrefix << "] Code " << msgCode << " : " << pMsg << std::endl;
    } else if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) {
        instanceCerr() << "WARNING: [" << pLayerPrefix << "] Code " << msgCode << " : " << pMsg << std::endl;
    }

    return VK_FALSE;
}

// Route Khronos validation output to a dedicated, explicitly-flushed file. radiance_surface.log is
// truncated later by Window (surface creation) and appended to by PhysicalDevice, so validation
// messages emitted during instance creation would be clobbered there -- keep them separate.
static std::ofstream &validationLog() {
    static std::ofstream f("radiance_validation.log", std::ios::trunc);
    return f;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                         const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                         void * /*pUserData*/) {
    const char *level = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = "ERROR";
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = "WARNING";
    }
    validationLog() << "[" << level << "] " << (data && data->pMessageIdName ? data->pMessageIdName : "")
                    << ": " << (data && data->pMessage ? data->pMessage : "") << "\n";
    validationLog().flush();
    return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugUtilsCallback;
    return info;
}

static std::vector<std::string> enumerateInstanceLayerNames() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) { return {}; }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) { return {}; }
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto &layer : layers) { names.emplace_back(layer.layerName); }
    return names;
}

vk::Instance::Instance() {
    GLFW_Init();

    if (volkInitialize() != VK_SUCCESS) {
        printf("volkInitialize failed!\n");
        exit(EXIT_SUCCESS);
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VulkanClear";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ClearScreenEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::set<std::string> extStorage;
    std::vector<std::string> dlssRequiredExtensions;
    bool dlssRequirementQuerySuccess = false;
#ifdef MCVR_ENABLE_XESS
    std::vector<std::string> xessRequiredExtensions;
    bool xessRequirementQuerySuccess = false;
#endif

    // Get instance extensions required by GLFW to draw to window
    unsigned int glfwExtensionCount;
    const char **glfwExtensions;
    glfwExtensions = GLFW_GetRequiredInstanceExtensions(&glfwExtensionCount);
#ifdef DEBUG
    instanceCout() << "glfw extensions:" << std::endl;
#endif
    for (int i = 0; i < glfwExtensionCount; i++) {
#ifdef DEBUG
        instanceCout() << "\t" << glfwExtensions[i] << std::endl;
#endif
        extStorage.insert(glfwExtensions[i]);
    }

    // DLSS extensions
    std::vector<VkExtensionProperties> dlssExtensions;
    NVSDK_NGX_Result dlssExtensionQueryResult = NgxContext::getDlssRRRequiredInstanceExtensions(dlssExtensions);
    if (NVSDK_NGX_SUCCEED(dlssExtensionQueryResult)) {
        dlssRequirementQuerySuccess = true;
#ifdef DEBUG
        instanceCout() << "dlss extensions:" << std::endl;
#endif
        for (const auto &dlssExtension : dlssExtensions) {
#ifdef DEBUG
            instanceCout() << "\t" << dlssExtension.extensionName << std::endl;
#endif
            extStorage.insert(dlssExtension.extensionName);
            dlssRequiredExtensions.emplace_back(dlssExtension.extensionName);
        }
    } else {
        instanceCerr() << "failed to query dlss instance extensions; skipping." << std::endl;
    }

#ifdef MCVR_ENABLE_XESS
    std::vector<const char *> xessExtensions;
    uint32_t xessMinApiVersion = 0;
    if (mcvr::XeSSWrapper::getRequiredInstanceExtensions(xessExtensions, &xessMinApiVersion)) {
        xessRequirementQuerySuccess = true;
#    ifdef DEBUG
        instanceCout() << "xess extensions:" << std::endl;
#    endif
        for (const char *extension : xessExtensions) {
#    ifdef DEBUG
            instanceCout() << "\t" << extension << std::endl;
#    endif
            extStorage.insert(extension);
            xessRequiredExtensions.emplace_back(extension);
        }

        if (xessMinApiVersion > appInfo.apiVersion) { appInfo.apiVersion = xessMinApiVersion; }
    } else {
        instanceCerr() << "xess instance extensions unavailable; skipping." << std::endl;
    }
#endif

    // dynamic vertex input state ext
    // repeated for dlss, but make sure
    extStorage.insert(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    // Enable Khronos validation whenever the layer is installed on the system (Vulkan SDK / layer
    // redistributable). On player machines without it this is a no-op; on a dev box it self-activates
    // even in Release/RelWithDebInfo -- where the DEBUG compile define is off -- and routes messages
    // to radiance_validation.log via the messenger below. Opt out with RADIANCE_NO_VALIDATION=1.
    const std::vector<std::string> availableLayers = enumerateInstanceLayerNames();
    const bool layerFound =
        std::find(availableLayers.begin(), availableLayers.end(), DEBUG_LAYER) != availableLayers.end();
    const bool optedOut = std::getenv("RADIANCE_NO_VALIDATION") != nullptr;
    const bool validationEnabled = layerFound && !optedOut;
    // GPU-assisted validation instruments shaders to catch GPU-side out-of-bounds descriptor access /
    // bad reads that CPU-side validation cannot see (e.g. sampling a freed bindless texture slot ->
    // page fault -> TDR). Opt-in via RADIANCE_GPU_AV=1: it is heavy and reserves a descriptor set, so
    // it stays off by default even when validation is on. Requires the two atomics device features
    // enabled in device.cpp (also gated on RADIANCE_GPU_AV) so fragment/vertex shaders can be
    // instrumented.
    const bool gpuAvEnabled = validationEnabled && std::getenv("RADIANCE_GPU_AV") != nullptr;

    // Always write a status block so radiance_validation.log is created on every run: an empty/missing
    // file is otherwise ambiguous (validation layer absent vs. a stale core.dll that predates this
    // code). The build marker pins which DLL actually ran; the layer list shows what the loader saw.
    {
        auto &log = validationLog();
        log << "[Instance] radiance validation status (core.dll built " << __DATE__ << " " << __TIME__
            << ")\n";
        log << "  target layer " << DEBUG_LAYER << ": found=" << (layerFound ? "yes" : "no")
            << " optedOut=" << (optedOut ? "yes" : "no") << " -> validation "
            << (validationEnabled ? "ENABLED" : "disabled") << "\n";
        log << "  GPU-assisted validation: " << (gpuAvEnabled ? "ENABLED (RADIANCE_GPU_AV)" : "off")
            << "\n";
        log << "  " << availableLayers.size() << " instance layer(s) enumerated:\n";
        for (const auto &name : availableLayers) { log << "    " << name << "\n"; }
        log.flush();
    }

    if (validationEnabled) { extStorage.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

    // Check for extensions
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

    if (extensionCount == 0) {
        instanceCerr() << "no extensions supported!" << std::endl;
        exit(EXIT_FAILURE);
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());
    std::unordered_set<std::string> availableExtensionSet;
    availableExtensionSet.reserve(availableExtensions.size());
    for (const auto &availableExtension : availableExtensions) {
        availableExtensionSet.insert(availableExtension.extensionName);
    }

#ifdef DEBUG
    instanceCout() << "supported extensions:" << std::endl;
    for (const auto &extension : availableExtensions) {
        instanceCout() << "\t" << extension.extensionName << std::endl;
    }
#endif

    auto areRequiredExtensionsSupported = [&](const std::vector<std::string> &requiredExtensions) {
        for (const auto &requiredExtension : requiredExtensions) {
            if (availableExtensionSet.find(requiredExtension) == availableExtensionSet.end()) { return false; }
        }
        return true;
    };

    dlssInstanceExtensionsCompatible_ =
        dlssRequirementQuerySuccess && areRequiredExtensionsSupported(dlssRequiredExtensions);
    if (!dlssInstanceExtensionsCompatible_) {
        instanceCerr() << "dlss instance extension requirements are not fully satisfied." << std::endl;
    }

#ifdef MCVR_ENABLE_XESS
    xessInstanceExtensionsCompatible_ =
        xessRequirementQuerySuccess && areRequiredExtensionsSupported(xessRequiredExtensions);
    if (!xessInstanceExtensionsCompatible_) {
        instanceCerr() << "xess instance extension requirements are not fully satisfied." << std::endl;
    }
#endif

    std::vector<const char *> extensions;
    for (const auto &extension : extStorage) {
        if (availableExtensionSet.find(extension) == availableExtensionSet.end()) {
            instanceCerr() << "extension not supported, skipping: " << extension << std::endl;
            continue;
        }
        extensions.push_back(extension.c_str());
    }

#ifdef DEBUG
    instanceCout() << "selected extensions:" << std::endl;
    for (const auto &extension : extensions) { instanceCout() << "\t" << extension << std::endl; }
#endif

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Chain the messenger create-info into pNext so validation messages emitted *during*
    // vkCreateInstance/vkDestroyInstance are captured too, not just runtime ones. Must outlive the
    // vkCreateInstance call below (it does -- same scope).
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = makeDebugMessengerCreateInfo();
    // GPU-AV is requested via VkValidationFeaturesEXT chained into pNext (read by the validation
    // layer, which provides VK_EXT_validation_features -- so it is NOT added to the extension list).
    // These locals also outlive vkCreateInstance below (same scope).
    VkValidationFeatureEnableEXT gpuAvFeatures[] = {
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
    };
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    if (validationEnabled) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &DEBUG_LAYER;
        createInfo.pNext = &messengerInfo;
        if (gpuAvEnabled) {
            validationFeatures.enabledValidationFeatureCount =
                static_cast<uint32_t>(sizeof(gpuAvFeatures) / sizeof(gpuAvFeatures[0]));
            validationFeatures.pEnabledValidationFeatures = gpuAvFeatures;
            validationFeatures.pNext = &messengerInfo;  // chain: createInfo -> features -> messenger
            createInfo.pNext = &validationFeatures;
        }
    }

    // Initialize Vulkan instance
    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        instanceCerr() << "failed to create instance!" << std::endl;
        exit(EXIT_FAILURE);
    } else {
#ifdef DEBUG
        instanceCout() << "created vulkan instance" << std::endl;
#endif
    }

    volkLoadInstance(instance_);

    // Persistent messenger for runtime validation messages (the pNext one above only covers instance
    // create/destroy). vkCreateDebugUtilsMessengerEXT is loaded by volkLoadInstance now that
    // VK_EXT_debug_utils is enabled.
    if (validationEnabled && vkCreateDebugUtilsMessengerEXT != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT persistentInfo = makeDebugMessengerCreateInfo();
        VkResult messengerResult =
            vkCreateDebugUtilsMessengerEXT(instance_, &persistentInfo, nullptr, &debugMessenger_);
        if (messengerResult != VK_SUCCESS) {
            debugMessenger_ = VK_NULL_HANDLE;
            instanceCerr() << "failed to create debug utils messenger: " << messengerResult << std::endl;
        } else {
            validationLog() << "[Instance] Khronos validation active -> radiance_validation.log\n";
            validationLog().flush();
        }
    }
}

vk::Instance::~Instance() {
    if (debugMessenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);

#ifdef DEBUG
    instanceCout() << "instance deconstructed" << std::endl;
#endif
}

VkInstance &vk::Instance::vkInstance() {
    return instance_;
}

bool vk::Instance::isDlssInstanceExtensionsCompatible() const {
    return dlssInstanceExtensionsCompatible_;
}

bool vk::Instance::isXessInstanceExtensionsCompatible() const {
    return xessInstanceExtensionsCompatible_;
}
