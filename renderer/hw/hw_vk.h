/*
 * hw_vk.h -- shared by the Vulkan files of the enhanced renderer (native
 * builds only; the web build's GPU side is renderer/webgpu/hw_webgpu.c):
 *   hw_vk_setup.c   device objects: images, buffers, the render pass,
 *                   descriptors and pipelines
 *   hw_vk_frame.c   each frame: the command list (hw.c) recorded into a
 *                   Vulkan command buffer; the display pass; readback
 */
#ifndef AC3_HW_VK_H
#define AC3_HW_VK_H
#include "hw_private.h"
#ifndef AC3_HW_WEBGPU
#include <stdio.h>

/* a Vulkan call that must succeed: otherwise logs and returns 0 */
#define VKOK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "ac3: hw: %s failed (%d) at %s:%d\n", #expr, (int)_r, \
            __FILE__, __LINE__); return 0; } } while (0)

/* the primitives' push constants (hw_prim shaders) */
typedef struct { float inv_size[2]; int scale; int pass; } prim_push_t;

static inline int same_rect(const VkRect2D *a, const VkRect2D *b) {
    return a->offset.x == b->offset.x && a->offset.y == b->offset.y &&
           a->extent.width == b->extent.width && a->extent.height == b->extent.height;
}

#endif
#endif
