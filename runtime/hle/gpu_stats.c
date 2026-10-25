#include "gpu_backend.h"
#include <stdio.h>

static struct {
    unsigned frames;
    unsigned poly3, poly4, poly_tex, poly_gouraud, poly_semi;
    unsigned rects, lines;
    unsigned load_image, store_image, move_image, fill_rect;
    unsigned draw_env, disp_env;
} S;

static void st_init(void) { }
static void st_shutdown(void) { }
static void st_frame_begin(void) { S.frames++; }
static void st_frame_end(void) { }

static void st_poly(const gpu_poly_t *p) {
    if (p->nverts == 3) S.poly3++; else S.poly4++;
    if (p->textured) S.poly_tex++;
    if (p->gouraud) S.poly_gouraud++;
    if (p->semitrans) S.poly_semi++;
}
static void st_rect(const gpu_rect_t *r) { (void)r; S.rects++; }
static void st_line(const gpu_line_t *l) { (void)l; S.lines++; }

static void st_set_draw_env(const PSX_DRAWENV *e) { (void)e; S.draw_env++; }
static void st_set_disp_env(const PSX_DISPENV *e) { (void)e; S.disp_env++; }
static void st_set_draw_area(s16 a, s16 b, s16 c, s16 d) {
    (void)a; (void)b; (void)c; (void)d;
}
static void st_set_draw_offset(s16 x, s16 y) { (void)x; (void)y; }
static void st_set_draw_mode(u16 t, u16 m, u16 o) { (void)t; (void)m; (void)o; }
static void st_set_mask(int s, int c) { (void)s; (void)c; }

static void st_load_image(const PSX_RECT *r, const void *d) {
    (void)r; (void)d; S.load_image++;
}
static void st_store_image(const PSX_RECT *r, void *d) {
    (void)r; (void)d; S.store_image++;
}
static void st_move_image(const PSX_RECT *r, s16 x, s16 y) {
    (void)r; (void)x; (void)y; S.move_image++;
}
static void st_fill_rect(const PSX_RECT *r, u8 a, u8 b, u8 c) {
    (void)r; (void)a; (void)b; (void)c; S.fill_rect++;
}

void ac3_gpu_stats_dump(void) {
    printf("GPU stats\n");
    printf("  frames            %u\n", S.frames);
    printf("  triangles         %u\n", S.poly3);
    printf("  quads             %u\n", S.poly4);
    printf("    textured        %u\n", S.poly_tex);
    printf("    gouraud         %u\n", S.poly_gouraud);
    printf("    semi-transparent%u\n", S.poly_semi);
    printf("  rectangles        %u\n", S.rects);
    printf("  lines             %u\n", S.lines);
    printf("  LoadImage         %u\n", S.load_image);
    printf("  StoreImage        %u\n", S.store_image);
    printf("  MoveImage         %u\n", S.move_image);
    printf("  fills             %u\n", S.fill_rect);
    printf("  PutDrawEnv        %u\n", S.draw_env);
    printf("  PutDispEnv        %u\n", S.disp_env);
}

const gpu_backend_t gpu_backend_stats = {
    "stats",
    st_init, st_shutdown,
    st_frame_begin, st_frame_end,
    st_set_draw_env, st_set_disp_env,
    st_set_draw_area, st_set_draw_offset, st_set_draw_mode, st_set_mask,
    st_poly, st_rect, st_line,
    st_load_image, st_store_image, st_move_image, st_fill_rect,
};
