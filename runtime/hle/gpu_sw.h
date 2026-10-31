#ifndef AC3_GPU_SW_H
#define AC3_GPU_SW_H

#include "gpu_backend.h"

typedef struct {
    u16 texpage;
    u32 texwin;
    s32 area_x0, area_y0;
    s32 area_x1, area_y1;
    s32 ofs_x, ofs_y;
    u16 mask_or;
    u16 mask_and;
} gpu_sw_state_t;

extern gpu_sw_state_t gpu_sw;

void gpu_sw_reset(void);

void gpu_sw_gp0_state(u32 word);

void gpu_sw_texpage_attr(u16 attr);
extern u16 *ac3_vram_bank;   /* the second video memory bank (co-op: the guest's plane), or NULL */
void gpu_sw_bank_load(s32 x, s32 y, s32 w, s32 h, const u8 *src);

void gpu_sw_texwin(u16 *mask, u16 *off);

void gpu_sw_poly(const gpu_poly_t *p);
void gpu_sw_rect(const gpu_rect_t *r);
void gpu_sw_line(const gpu_line_t *l);

void gpu_sw_fill(const PSX_RECT *r, u16 pixel);
void gpu_sw_copy(const PSX_RECT *src, s32 dx, s32 dy);
void gpu_sw_load_begin(const PSX_RECT *r);
void gpu_sw_load_word(u32 word);
u32  gpu_sw_load_words_left(void);
void gpu_sw_load_cancel(void);
void gpu_sw_store(const PSX_RECT *r, u16 *dst, u32 n);

void gpu_sw_display_enable(int on);
void gpu_sw_display_start(s32 x, s32 y);
void gpu_sw_display_mode(s32 w, s32 h, int rgb24);

u32 gpu_sw_prim_count(void);

#ifdef AC3_ENHANCED
typedef struct {
    void (*poly)(const gpu_poly_t *p);
    void (*rect)(const gpu_rect_t *r);
    void (*line)(const gpu_line_t *l);
    void (*fill)(const PSX_RECT *r, u16 pixel);
    void (*copy)(const PSX_RECT *src, s32 dx, s32 dy);
    void (*upload)(const PSX_RECT *r);
    void (*list)(int begin);
    void (*node)(u32 addr, int len);
} gpu_sw_hooks_t;

extern const gpu_sw_hooks_t *gpu_sw_hooks;
#endif

#endif
