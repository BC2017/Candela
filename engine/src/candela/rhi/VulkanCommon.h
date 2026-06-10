#pragma once

#include <volk.h>
#include <vulkan/vk_enum_string_helper.h>

#include "candela/core/Log.h"

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult vk_check_result_ = (expr);                                    \
        CD_ASSERT(vk_check_result_ == VK_SUCCESS, "{} returned {}", #expr,     \
                  string_VkResult(vk_check_result_));                          \
    } while (0)
