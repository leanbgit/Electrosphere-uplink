#ifndef AC3_HW_WORLD_H
#define AC3_HW_WORLD_H

#include "ac3_runtime.h"

#define HW_SKY_BINS 32

typedef struct {
    u32   valid;
    u32   frame;
    float rot[9];
    float cam[3];
    float H, ws;
    float ofs[2];
    s32   area[4];
    float sun[3];
    float sun_col[3];
    float fog_col[3];
    float fog_z[2];        /* game's fog: fade start and end (view depth), 0 = off */
    float game_fog_col[3]; /* and the colour it fades to */
    float sea_y;
    float time;
    float sky[HW_SKY_BINS][3];
    u32   sky_samples;
    u32   pad[3];
} hw_scene_t;

int  hw_world_camera(hw_scene_t *s);

void hw_world_sky_begin(hw_scene_t *s);
void hw_world_sky_add(hw_scene_t *s, float sx, float sy, u8 r, u8 g, u8 b);
void hw_world_sky_finish(hw_scene_t *s);
void hw_world_sky_at(const hw_scene_t *s, float e, float out[3]);

#define HW_TILE_RES    32
#define HW_ATLAS_COLS  16
#define HW_ATLAS_ROWS  32
#define HW_ATLAS_W     (HW_TILE_RES * HW_ATLAS_COLS)
#define HW_ATLAS_H     (HW_TILE_RES * HW_ATLAS_ROWS)
#define HW_MAP_N       256
#define HW_TILE_SIZE   2048.0f

enum { HW_CELL_WATER = 1, HW_CELL_FOREST = 2, HW_CELL_BUILT = 4 };

typedef struct {
    float x, y, z;
    float u, v;
    u32   rgba;
    u32   tex;
    u32   avg;
} hw_tile_vtx_t;

typedef struct {
    int   valid;
    u32   version;
    u32   signature;
    u16   grid[HW_MAP_N * HW_MAP_N];
    float tile_top[512];
    float height[HW_ATLAS_W * HW_ATLAS_H];
    u32   color[HW_ATLAS_W * HW_ATLAS_H];
    float sea_y;
    int   has_sea;
    float top_y;
    u32   water_rgb;
    int   forest_samples, water_samples;
    hw_tile_vtx_t *mesh;
    u32   mesh_n, mesh_cap;
    u32   mesh_first[512], mesh_count[512];
    u8    tile_water[512];
    u32   sea_tex;
#define HW_SEA_KEYS 8
    u32   sea_keys[HW_SEA_KEYS];
    int   nsea_keys;
} hw_map_t;

extern hw_map_t hw_map;
extern int hw_world_verbose;

int   hw_world_map_update(void);
float hw_world_ground(float x, float z, u32 *flags);
float hw_world_raycast(const hw_scene_t *s, float sx, float sy, float t0, float t1);

int   hw_world_sea_texture(u16 tpage, u16 clut);

void  hw_world_map_png(const char *path);

#endif
