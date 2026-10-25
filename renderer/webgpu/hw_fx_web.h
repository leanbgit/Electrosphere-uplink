/*
 * hw_fx_web.h -- world effects (renderer/hw/hw_fx.c) in the browser build,
 * split across the two workers:
 *   game worker   renderer/webgpu/hw_fx_game.c    scenes, uniforms, far-terrain
 *                                                  tile lists, map data (CPU)
 *   render worker renderer/webgpu/hw_fx_webgpu.c  textures, pipelines, passes (GPU)
 * Each field's effects data travels inside the packet (hw_packet.h) as:
 *   fx_pkt_t
 *   fx_ubo_t       [nscenes]
 *   tile_draw_t    [ndraws[0] + ndraws[1]]
 *   float[2]       [ninst[0] + ninst[1]]       far-terrain tile origins
 *   hw_ocean_node_t[nnodes[0] + nnodes[1]]     visible water patches
 *   map (only when it changed): u16 grid[256*256], float tile_top[512],
 *        float height[ATLAS], u32 color[ATLAS], hw_tile_vtx_t mesh[mesh_n]
 */
#ifndef AC3_HW_FX_WEB_H
#define AC3_HW_FX_WEB_H
#include "hw_private.h"
#include "hw_ocean_web.h"

#define FX_OCEAN_PARAMS 9      /* HW_OCEAN_PARAMS in renderer/hw/hw_ocean.h */

/* same layout as hw_fx.c's uniform block (and the WGSL `Scene` struct): 1104 bytes */
typedef struct {
    float vtw[12], wtv[12];
    float cam[4], proj[4], ofs[4], area[4], sun[4], sun_col[4], fog[4];
    float map[4], cloud[4], wind[4], water[4], misc[4];
    float pal[4], pal_src[4];
    float hdr[4];
    float ocean[FX_OCEAN_PARAMS][4];
    float sky[32][4];
    float prev_wtv[12];
    float prev_cam[4], prev_proj[4], prev_area[4], cloud_tc[4];
} fx_ubo_t;
_Static_assert(sizeof(fx_ubo_t) == 1104, "fx_ubo_t must match the WGSL Scene struct");

#define FX_UBO_STRIDE  2048    /* per-scene slot in the uniform buffer (dynamic offset) */
#define FX_MAX_SCENES  8
#define FX_TILE_CELLS  44
#define FX_MAX_INST    ((2 * FX_TILE_CELLS + 1) * (2 * FX_TILE_CELLS + 1))
#define FX_MESH_CAP    (1u << 17)
#define FX_MAX_DRAWS   512
#define FX_BLOOM_MIPS  6

typedef struct { u32 first_vtx, nvtx, first_inst, ninst; } tile_draw_t;

typedef struct {
    u32 nscenes;
    u32 flags;                 /* hw_fx_flags this field (FX_* bits) */
    u32 map_valid, forest;     /* hw_map.valid, hw_map.forest_samples > 0 */
    u8  kind[FX_MAX_SCENES];   /* OP_FX_BEGIN / OP_FX_END */
    s32 area[FX_MAX_SCENES][4];
    u32 ndraws[2];
    s32 tiles_scene[2];
    u32 ninst[2];
    u32 map_bytes;             /* 0, or the size of the map part that follows */
    u32 mesh_n;
    u32 ocean_sim;             /* run the wave simulation this field */
    float ocean_t, ocean_dt;   /* time within the 200 s wave loop; seconds since the last run */
    u32 nnodes[2];
    s32 ocean_scene[2];
} fx_pkt_t;

#define FX_MAP_BYTES(mesh_n) ((size_t)HW_MAP_N * HW_MAP_N * 2 + 512 * 4 + \
                              (size_t)HW_ATLAS_W * HW_ATLAS_H * 8 + (size_t)(mesh_n) * sizeof(hw_tile_vtx_t))

/* game side */
size_t hw_fx_pack_size(void);          /* also does this field's CPU work */
void   hw_fx_pack(u8 *dst);            /* writes hw_fx_pack_size() bytes */
void   hw_fx_resend_map(void);         /* the render worker missed packets: send the map again */
/* render side */
int    hw_fx_unpack(const u8 *src, size_t len);

#endif
