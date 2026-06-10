// Single translation unit hosting the VulkanMemoryAllocator implementation.
// Function pointers come from volk, so VMA must not declare static prototypes.

#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#pragma warning(push, 0)
#include <vk_mem_alloc.h>
#pragma warning(pop)
