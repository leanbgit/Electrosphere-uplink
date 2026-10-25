#ifndef AC3_HW_OCEAN_H
#define AC3_HW_OCEAN_H

#include "hw_private.h"

#define HW_OCEAN_N         256
#define HW_OCEAN_CASCADES  3
#define HW_OCEAN_MIPS      9
#define HW_OCEAN_QUADS     16
#define HW_OCEAN_MAX_NODES 2048
#define HW_OCEAN_PARAMS    9

typedef struct { float x, z, cell, range; } hw_ocean_node_t;

int  hw_ocean_init(void);
void hw_ocean_shutdown(void);
int  hw_ocean_ready(void);

void hw_ocean_simulate(VkCommandBuffer cb, double time);

VkImageView hw_ocean_disp_view(void);
VkImageView hw_ocean_deriv_view(void);
VkSampler   hw_ocean_sampler(void);

void hw_ocean_params(float out[HW_OCEAN_PARAMS][4]);

u32  hw_ocean_select(const hw_scene_t *s, float horizon, hw_ocean_node_t *out, u32 max);

VkBuffer hw_ocean_indices(u32 *count);

#endif
