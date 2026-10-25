#ifndef AC3_GPU_BACKEND_H
#define AC3_GPU_BACKEND_H

#include "hle.h"

#define GPU_VRAM_W 1024
#define GPU_VRAM_H 512

typedef struct {
    int nverts;
    int textured, gouraud, semitrans, raw;
    s16 x[4], y[4];
    u8  r[4], g[4], b[4];
    u8  u[4], v[4];
    u16 clut, tpage;
    u16 twin_mask, twin_off;
} gpu_poly_t;

typedef struct {
    int textured, semitrans, raw;
    s16 x, y;
    u16 w, h;
    u8  r, g, b;
    u8  u, v;
    u16 clut, tpage;
    u16 twin_mask, twin_off;
} gpu_rect_t;

typedef struct {
    int nverts, gouraud, semitrans;
    s16 x[256], y[256];
    u8  r[256], g[256], b[256];
    u16 tpage;
} gpu_line_t;

typedef struct {
    const char *name;
    void (*init)(void);
    void (*shutdown)(void);

    void (*frame_begin)(void);
    void (*frame_end)(void);

    void (*set_draw_env)(const PSX_DRAWENV *env);
    void (*set_disp_env)(const PSX_DISPENV *env);
    void (*set_draw_area)(s16 x0, s16 y0, s16 x1, s16 y1);
    void (*set_draw_offset)(s16 x, s16 y);
    void (*set_draw_mode)(u16 tpage, u16 twin_mask, u16 twin_off);
    void (*set_mask)(int set_bit, int check_bit);

    void (*poly)(const gpu_poly_t *p);
    void (*rect)(const gpu_rect_t *r);
    void (*line)(const gpu_line_t *l);

    void (*load_image)(const PSX_RECT *dst, const void *data);
    void (*store_image)(const PSX_RECT *src, void *data);
    void (*move_image)(const PSX_RECT *src, s16 dx, s16 dy);
    void (*fill_rect)(const PSX_RECT *r, u8 red, u8 green, u8 blue);
} gpu_backend_t;

extern const gpu_backend_t *gpu;
extern const gpu_backend_t gpu_backend_stats;
extern const gpu_backend_t gpu_backend_vulkan;

typedef struct {
    int enabled;
    int x, y;
    int w, h;
    int rgb24;
} gpu_display_t;

void gpu_display_state(gpu_display_t *out);

int ac3_render_quit_requested(void);

void gpu_select_backend(const gpu_backend_t *b);

extern u16 *ac3_vram;
void ac3_vram_init(void);

u32 ac3_vram_last_write(int x, int y);

int ac3_vram_last_write_info(int x, int y, u32 *frame, const char **what,
                             int *rx, int *ry, int *rw, int *rh,
                             u8 *r, u8 *g, u8 *b);
static inline u16 *vram_at(int x, int y) { return ac3_vram + (y * GPU_VRAM_W + x); }

#endif
