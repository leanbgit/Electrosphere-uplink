/* hw_ocean_webgpu.h -- the FFT ocean's GPU side in the render worker. */
#ifndef AC3_HW_OCEAN_WEBGPU_H
#define AC3_HW_OCEAN_WEBGPU_H
#include "hw_ocean_web.h"

int  hw_ocean_webgpu_init(void);
void hw_ocean_webgpu_shutdown(void);
int  hw_ocean_webgpu_ready(void);
void hw_ocean_webgpu_simulate(WGPUCommandEncoder enc, float t, float dt);
WGPUTextureView hw_ocean_webgpu_disp(void);
WGPUTextureView hw_ocean_webgpu_deriv(void);
WGPUSampler     hw_ocean_webgpu_sampler(void);
WGPUBuffer      hw_ocean_webgpu_indices(u32 *count);
WGPUBuffer      hw_ocean_webgpu_nodes(void);

#endif
