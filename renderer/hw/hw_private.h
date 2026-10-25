#ifndef AC3_HW_PRIVATE_H
#define AC3_HW_PRIVATE_H

#include "hw.h"
#include "hw_world.h"

#define VRAM_W 1024
#define VRAM_H 512

typedef struct {
    float x, y, z, w;
    float u, v;
    u8 r, g, b, a;
    u32 info[4];
} hw_vtx_t;

#define F_TEXTURED 1u
#define F_RAW      2u
#define F_MASK_OR  4u
#define F_COPY     8u
#define F_FLAT     16u
#define F_CHECK    32u
#define F_ZTEST    64u

enum { P_OPAQUE, P_BLEND0, P_BLEND1, P_BLEND2, P_BLEND3,
       P_OPAQUE_CHECK, P_COPY, P_COPY_CHECK, P_COUNT };
enum { OP_DRAW, OP_FILL, OP_UPLOAD, OP_SYNC, OP_ZCLEAR, OP_FX_BEGIN, OP_FX_END };

typedef struct {
    u8 op, pipe, pass;
    VkRect2D sc;
    u32 first, count;
    s16 x, y, w, h;
    float color[4];
    u32 staging;
} cmd_t;

typedef struct { s32 x0, y0, x1, y1; } rect_t;

#define MAX_DIRTY 16

#ifdef AC3_HW_WEBGPU
typedef struct {
    int ready;
    int S, persp;
    u32 W, Hs;
    u32 draw_order;
    int ztest;

    /* GPU objects (renderer/webgpu/hw_webgpu.c) */
    WGPUDevice dev;
    WGPUQueue queue;
    WGPUTexture target, read, zimg, zsnap, order, order_snap, upimg, vram16;
    WGPUTextureView target_view, read_view, z_view, zsnap_view, order_view,
                    order_snap_view, up_view, vram16_view;
    WGPUSampler nearest, linear;
    WGPUBindGroupLayout tex_layout, smp_layout, push_layout;
    WGPUBindGroup set_read, smp_nearest, disp_tex, disp_smp, push_group;
    WGPUPipelineLayout layout;
    WGPURenderPipeline pipe[P_COUNT];
    WGPURenderPipeline fill_pipe[2];        /* [0] color+z+order, [1] z+order only */
    WGPURenderPipeline blit_target, blit_read;
    WGPUBindGroupLayout blit_tex_layout;
    WGPUBindGroup blit_group;
    WGPUPipelineLayout blit_layout;
    WGPURenderPipeline disp_pipe;
    WGPUPipelineLayout disp_layout;
    WGPUBindGroupLayout disp_tex_layout, disp_push_layout;
    WGPUBuffer push_buf, fill_buf, disp_buf, vbuf, sbuf;
    u64 vcap, scap;

    hw_vtx_t *verts; u32 nverts, cap_verts;
    u8 *staging;     u32 nstaging, cap_staging;
    cmd_t *cmds;     u32 ncmds, cap_cmds;
    rect_t dirty[MAX_DIRTY]; int ndirty;

    int images_cleared;
    u32 prims, precise, last_prims, last_precise;
} hw_state_t;
#else
typedef struct {
    int ready;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfamily;
    int S, persp;
    u32 W, Hs;

    VkImage target, read, zimg, upimg;
    VkDeviceMemory target_mem, read_mem, z_mem, up_mem;
    VkImageView target_view, read_view, z_view, up_view;
    VkImage zsnap; VkDeviceMemory zsnap_mem; VkImageView zsnap_view;
    VkImage order, order_snap;
    VkDeviceMemory order_mem, order_snap_mem;
    VkImageView order_view, order_snap_view;
    u32 draw_order;
    int ztest;
    VkSampler nearest, linear;
    VkRenderPass pass;
    VkFramebuffer fb;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set_read, set_disp;
    VkPipelineLayout layout;
    VkPipeline pipe[P_COUNT];
    VkPipelineLayout disp_layout;
    VkPipeline disp_pipe;

    VkBuffer vbuf[HW_FRAMES];  VkDeviceMemory vmem[HW_FRAMES];
    void *vmap[HW_FRAMES];     VkDeviceSize vcap[HW_FRAMES];
    VkBuffer sbuf[HW_FRAMES];  VkDeviceMemory smem[HW_FRAMES];
    void *smap[HW_FRAMES];     VkDeviceSize scap[HW_FRAMES];

    hw_vtx_t *verts; u32 nverts, cap_verts;
    u8 *staging;     u32 nstaging, cap_staging;
    cmd_t *cmds;     u32 ncmds, cap_cmds;
    rect_t dirty[MAX_DIRTY]; int ndirty;

    int images_cleared;
    u32 prims, precise, last_prims, last_precise;
} hw_state_t;
#endif

extern hw_state_t hw_state;

#ifndef AC3_HW_WEBGPU
uint32_t hw_find_mem(uint32_t bits, VkMemoryPropertyFlags want);
int  hw_make_image(VkFormat fmt, u32 w, u32 h, VkImageUsageFlags usage,
                   VkImage *img, VkDeviceMemory *mem, VkImageView *view);
int  hw_make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer *buf, VkDeviceMemory *mem, void **map);
void hw_free_host_buffer(VkBuffer *buf, VkDeviceMemory *mem, void **map);
VkShaderModule hw_shader(const uint32_t *code, size_t bytes);
void hw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                VkAccessFlags sa, VkAccessFlags da,
                VkPipelineStageFlags ss, VkPipelineStageFlags ds);
#endif
cmd_t *hw_new_cmd(u8 op);
void hw_mark_dirty(s32 x0, s32 y0, s32 x1, s32 y1);


int  hw_fx_init(void);
void hw_fx_shutdown(void);
#ifdef AC3_HW_WEBGPU
void hw_fx_prepare_webgpu(WGPUCommandEncoder enc);
void hw_fx_record_webgpu(WGPUCommandEncoder enc, const cmd_t *c);
#else
void hw_fx_prepare(VkCommandBuffer cb, int slot);
void hw_fx_record(VkCommandBuffer cb, int slot, const cmd_t *c);
#endif
void hw_fx_frame_done(void);

#endif
