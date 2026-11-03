/*
 * gpu_internal.h -- shared by psyq_gpu.c and gpu_packets.c (not used
 * elsewhere).
 *
 *   gpu_packets.c  the GPU's command words (GP0 packets): primitives, the
 *                  drawing environment, fills, copies and uploads, decoded and
 *                  handed to the software GPU (gpu_sw.c); walking an ordering
 *                  table; which VRAM block was last written by what (debugging)
 *   psyq_gpu.c     the game's library (libgpu): ordering tables, drawing and
 *                  display environments, image transfers
 */
#ifndef AC3_GPU_INTERNAL_H
#define AC3_GPU_INTERNAL_H
#include "gpu_backend.h"
#include "gpu_sw.h"

static inline void unpack_xy(u32 w, s16 *x, s16 *y) {
    *x = (s16)(w & 0xFFFF);
    *y = (s16)(w >> 16);
}

static inline void unpack_rgb(u32 w, u8 *r, u8 *g, u8 *b) {
    *r = (u8)(w & 0xFF);
    *g = (u8)((w >> 8) & 0xFF);
    *b = (u8)((w >> 16) & 0xFF);
}

/* positions and sizes in command words (sizes of 0 mean the full 1024 / 512) */
#define GP0_POS_X(v)   ((s16)((v) & 0x3FFu))
#define GP0_POS_Y(v)   ((s16)((v) & 0x1FFu))
#define GP0_COPY_W(v)  ((s16)(((((v) & 0x3FFu) - 1u) & 0x3FFu) + 1u))
#define GP0_COPY_H(v)  ((s16)(((((v) & 0x1FFu) - 1u) & 0x1FFu) + 1u))

/* what last wrote a VRAM block (ac3_vram_last_write) */
enum { VRAM_W_NONE = 0, VRAM_W_UPLOAD, VRAM_W_FILL, VRAM_W_COPY };

#define vram_mark_written gpu_vram_mark_written
#define draw_rect         gpu_draw_rect
#define gp0_env           gpu_gp0_env
#define gp0_fill          gpu_gp0_fill
#define gp0_copy          gpu_gp0_copy
#define gp0_load_begin    gpu_gp0_load_begin
#define gp0_in_polyline   gpu_gp0_in_polyline
#define decode_packet     gpu_decode_packet
#define walk_list         gpu_walk_list
void vram_mark_written(const PSX_RECT *r, u8 kind, u8 red, u8 green, u8 blue);
void draw_rect(gpu_rect_t *r);
void gp0_env(u32 w);                                   /* E1-E6: the drawing environment */
void gp0_fill(u32 c, u32 xy, u32 wh, const char *who);
void gp0_copy(u32 s, u32 d, u32 z);                    /* VRAM to VRAM */
void gp0_load_begin(u32 xy, u32 wh, PSX_RECT *out);    /* CPU to VRAM: the words follow */
extern int gp0_in_polyline;                            /* inside a polyline's points */
void decode_packet(u32 addr, int len);                 /* one packet of an ordering table */
void walk_list(u32 head);                              /* a whole ordering table */

#endif
