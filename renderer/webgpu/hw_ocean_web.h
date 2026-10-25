/* hw_ocean_web.h -- ocean constants and CPU helpers shared by both workers
 * (same values as renderer/hw/hw_ocean.h, which is Vulkan-bound). */
#ifndef AC3_HW_OCEAN_WEB_H
#define AC3_HW_OCEAN_WEB_H
#include "hw_private.h"

#define HW_OCEAN_N         256
#define HW_OCEAN_CASCADES  3
#define HW_OCEAN_MIPS      9
#define HW_OCEAN_QUADS     16
#define HW_OCEAN_MAX_NODES 2048
#define HW_OCEAN_PARAMS    9
#define HW_OCEAN_REPEAT_S  200.0
#define HW_OCEAN_CHOPPY    1.2f
#define HW_OCEAN_FOAM_LIFE 2.0

typedef struct { float x, z, cell, range; } hw_ocean_node_t;

/* the initial spectrum (h0: N*N*CASCADES rgba32f, or NULL) and the shader parameters */
void hw_ocean_spectrum(float *h0, float params[HW_OCEAN_PARAMS][4]);
u32  hw_ocean_select(const hw_scene_t *s, float horizon, hw_ocean_node_t *out, u32 max);

#endif
