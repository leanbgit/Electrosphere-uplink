/*
 * headless_platform.c -- display and audio stand-ins for the headless native
 * build (platform/headless/build.sh), used for co-op reverse engineering and
 * determinism checks. Nothing is shown or played: the software GPU still
 * draws into VRAM, screenshots are written as PPM, and audio is mixed at the
 * fixed per-field rate (the same rule replays use), then discarded.
 * Nothing here touches game logic.
 */
#include "gpu_backend.h"
#include "spu_backend.h"
#include "spu_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef AC3_ENHANCED
#include "hw/hw.h"
#include "hw/hw_glue.h"
#include "hw_packet.h"
#endif

extern u16 *ac3_vram;

#ifdef AC3_ENHANCED
/* HD=1 builds: the enhanced renderer's game side runs exactly as in the
 * browser (web_platform.c hd_frame), minus the GPU: each field is packed and
 * dropped. This is what makes widescreen and the effects switches change what
 * the game computes, so the bench can measure it. */
static int hd;
int ac3_render_restart_pending(int *english) { (void)english; return 0; }
int ac3_render_relaunch(int argc, char **argv, int english) { (void)argc; (void)argv; (void)english; return 0; }
static void hd_init(void) {
    ac3_hd.forest = 0;                       /* as in the web version */
    hw_config_t cfg = { ac3_hd.scale, ac3_hd.perspective };
    hw_init_cpu(&cfg);
    hw_glue_attach();
    hd = 1;
    printf("display: headless with the enhanced renderer's game side (widescreen %d, fx %d, atmosphere %d)\n",
           ac3_hd.widescreen, ac3_hd.fx, ac3_hd.atmosphere);
}
static void hd_frame(void) {
    gpu_display_t d;
    gpu_display_state(&d);
    u8 *p = NULL;
    static u32 seq;
    hw_packet_build(&p, ++seq, d.rgb24 ? PKT_VRAM : PKT_HD, &d, NULL);
    hw_glue_frame_end(&d);
}
#endif

/* ------------------------------------------------------------- display */

static void hl_init(void) {
#ifdef AC3_ENHANCED
    hd_init();
    if (hd) return;
#endif
    printf("display: headless (software GPU, no window)\n");
}
static void hl_shutdown(void) { }
static void hl_frame_begin(void) { }
static void hl_frame_end(void) {
#ifdef AC3_ENHANCED
    if (hd) hd_frame();
#endif
}
static void hl_set_draw_env(const PSX_DRAWENV *e) { (void)e; }
static void hl_set_disp_env(const PSX_DISPENV *e) { (void)e; }
static void hl_set_draw_area(s16 a, s16 b, s16 c, s16 d) { (void)a; (void)b; (void)c; (void)d; }
static void hl_set_draw_offset(s16 x, s16 y) { (void)x; (void)y; }
static void hl_set_draw_mode(u16 t, u16 m, u16 o) { (void)t; (void)m; (void)o; }
static void hl_set_mask(int s, int c) { (void)s; (void)c; }
static void hl_poly(const gpu_poly_t *p) { (void)p; }
static void hl_rect(const gpu_rect_t *r) { (void)r; }
static void hl_line(const gpu_line_t *l) { (void)l; }
static void hl_load_image(const PSX_RECT *r, const void *d) { (void)r; (void)d; }
static void hl_store_image(const PSX_RECT *r, void *d) { (void)r; (void)d; }
static void hl_move_image(const PSX_RECT *r, s16 x, s16 y) { (void)r; (void)x; (void)y; }
static void hl_fill_rect(const PSX_RECT *r, u8 a, u8 b, u8 c) { (void)r; (void)a; (void)b; (void)c; }

const gpu_backend_t gpu_backend_headless = {
    "headless",
    hl_init, hl_shutdown,
    hl_frame_begin, hl_frame_end,
    hl_set_draw_env, hl_set_disp_env,
    hl_set_draw_area, hl_set_draw_offset, hl_set_draw_mode, hl_set_mask,
    hl_poly, hl_rect, hl_line,
    hl_load_image, hl_store_image, hl_move_image, hl_fill_rect,
};

int ac3_render_quit_requested(void) { return 0; }

/* The visible part of VRAM as a PPM (15-bit or 24-bit display modes),
 * the same conversion as the page and renderer/shaders/psx.frag. */
#include "coop.h"
void ac3_render_screenshot(const char *path) {
    gpu_display_t d;
    gpu_display_state(&d);
    u16 *vram = ac3_coop_display_vram(ac3_vram);   /* co-op guest: its own picture */
    if (!vram || !d.enabled || d.w <= 0 || d.h <= 0) return;
    if (d.w > 1024) d.w = 1024;
    if (d.h > 512) d.h = 512;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "screenshot: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", d.w, d.h);
    u8 *row = (u8 *)malloc((size_t)d.w * 3);
    for (int y = 0; y < d.h; y++) {
        const u16 *v = vram + ((d.y + y) & 511) * GPU_VRAM_W;
        u8 *o = row;
        for (int x = 0; x < d.w; x++) {
            if (d.rgb24) {
                int b = x * 3, wx = d.x + (b >> 1);
                u16 w0 = v[wx & 1023], w1 = v[(wx + 1) & 1023];
                if ((b & 1) == 0) { o[0] = w0 & 255; o[1] = w0 >> 8; o[2] = w1 & 255; }
                else              { o[0] = w0 >> 8;  o[1] = w1 & 255; o[2] = w1 >> 8; }
            } else {
                u16 c = v[(d.x + x) & 1023];
                u32 r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
                o[0] = (u8)((r << 3) | (r >> 2));
                o[1] = (u8)((g << 3) | (g >> 2));
                o[2] = (u8)((bl << 3) | (bl >> 2));
            }
            o += 3;
        }
        fwrite(row, 3, (size_t)d.w, f);
    }
    free(row);
    fclose(f);
}

void ac3_render_screenshot_when_drawing(const char *path, uint32_t min_prims) {
    (void)min_prims;
    ac3_render_screenshot(path);
}

/* --------------------------------------------------------------- audio */
/* Fixed amount per field: the mix never depends on a wall clock, which is
 * what lockstep play will need too (see docs/coop/S1_determinism.md). */

static void na_init(void) { printf("audio: headless (mixed at the fixed field rate, discarded)\n"); }
static void na_shutdown(void) { }
static void na_key_on(int v, const spu_voice_t *voice) { (void)v; (void)voice; }
static void na_key_off(int v) { (void)v; }
static void na_upload(u32 a, const u8 *d, u32 s) { (void)a; (void)d; (void)s; }
static void na_master(s16 l, s16 r) { (void)l; (void)r; }
static void na_cdvol(s16 l, s16 r) { (void)l; (void)r; }
static int  na_frames_wanted(void) { return ac3_spu_field_frames(); }
static void na_submit(const s16 *frames, int nframes) { (void)frames; (void)nframes; }

const spu_backend_t spu_backend_headless = {
    "headless",
    na_init, na_shutdown,
    na_key_on, na_key_off, na_upload, na_master, na_cdvol,
    na_frames_wanted, na_submit,
};

void ac3_audio_queue_stats(u32 *under, u32 *over) {
    if (under) *under = 0;
    if (over) *over = 0;
}
