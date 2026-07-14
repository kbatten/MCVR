#pragma once

#include "core/all_extern.hpp"

namespace vk {
class Instance : public SharedObject<Instance> {
  public:
    Instance();
    ~Instance();

    VkInstance &vkInstance();
    bool isDlssInstanceExtensionsCompatible() const;
    bool isXessInstanceExtensionsCompatible() const;

  private:
    VkInstance instance_;
    bool dlssInstanceExtensionsCompatible_ = false;
    bool xessInstanceExtensionsCompatible_ = false;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
};
} // namespace vk
