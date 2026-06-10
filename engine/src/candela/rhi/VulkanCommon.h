#pragma once

#include <volk.h>

// The enum-stringifier ships with the SDK's utility headers, not with plain
// Vulkan-Headers (headers-only/CI builds). Provide just the two helpers we
// use; diagnostics degrade to numeric values.
#if __has_include(<vulkan/vk_enum_string_helper.h>)
#include <vulkan/vk_enum_string_helper.h>
#else
inline const char* string_VkResult(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    default: return "VkResult(unrecognized — headers-only build)";
    }
}

inline const char* string_VkFormat(VkFormat) {
    return "VkFormat(unavailable — headers-only build)";
}
#endif

#include "candela/core/Log.h"

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult vk_check_result_ = (expr);                                    \
        CD_ASSERT(vk_check_result_ == VK_SUCCESS, "{} returned {}", #expr,     \
                  string_VkResult(vk_check_result_));                          \
    } while (0)
