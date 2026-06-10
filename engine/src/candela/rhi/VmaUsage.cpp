// Single translation unit hosting the VulkanMemoryAllocator implementation.
// Function pointers come from volk, so VMA must not declare static prototypes.
// This file also gets -w under GCC (see engine/CMakeLists.txt) — VMA's
// implementation is too warning-heavy to suppress piecemeal.

#include "candela/core/Compiler.h"

#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

CD_PUSH_DISABLE_WARNINGS
#include <vk_mem_alloc.h>
CD_POP_WARNINGS
