/*
 * vk_compat.h -- the few plain Vulkan value types that the API-independent
 * half of renderer/hw (command recording in hw.c, hw_glue.c) uses, so that
 * code compiles unchanged in the WebGPU build. No Vulkan functions here.
 */
#ifndef AC3_VK_COMPAT_H
#define AC3_VK_COMPAT_H
#include <stdint.h>

typedef struct { int32_t x, y; } VkOffset2D;
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct { float x, y, width, height, minDepth, maxDepth; } VkViewport;

#endif
