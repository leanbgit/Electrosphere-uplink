#ifndef AC3_CAPTURE_H
#define AC3_CAPTURE_H

#include "gpu_backend.h"

void ac3_capture_request(void);
void ac3_capture_on_video(void);
void ac3_capture_frame(ctx_t *ctx);

void ac3_capture_log(const char *line);

void ac3_gpu_current_offset(s16 *x, s16 *y);

void ac3_capture_poly(const gpu_poly_t *p);
void ac3_capture_rect(const gpu_rect_t *r);
void ac3_capture_line(const gpu_line_t *l);
void ac3_capture_load_image(const PSX_RECT *r, u32 src);
void ac3_capture_vram_copy(const PSX_RECT *src, s16 dx, s16 dy, int skipped);
void ac3_capture_fill_rect(const PSX_RECT *r, u8 red, u8 green, u8 blue,
                           const char *who);

void ac3_capture_mdec_in(u32 buf, u32 cmd, u32 words, int bpp24);
void ac3_capture_mdec_out(u32 dst, u32 words, u32 mb_done, u32 mb_want,
                          u32 bytes_in);

typedef struct {
    u32 lba, mode;
    int streaming, credit, rate;
    int filter_active;
    u8  filter_file, filter_chan;
    u32 fifo_lba;
    int fifo_valid;
} ac3_cd_snap_t;
void ac3_cd_snapshot(ac3_cd_snap_t *s);

typedef struct {
    u32 coef_buf, coef_pos, coef_end;
    int out_24bit;
    u32 frames;
    u32 blocks, blocks_empty, coefs;
    s32 dc_min, dc_max;
    u32 out_bytes;
} ac3_mdec_snap_t;
void ac3_mdec_snapshot(ac3_mdec_snap_t *s);
void ac3_gpu_env_snapshot(PSX_DISPENV *disp, PSX_DRAWENV *draw);

const u8 *ac3_mdec_last_stream(u32 *len);
const u8 *ac3_mdec_last_pixels(u32 *len, int *bpp24, u32 *strip);

int ac3_write_png_rgb(const char *path, unsigned w, unsigned h,
                      const unsigned char *rgb);

#endif
