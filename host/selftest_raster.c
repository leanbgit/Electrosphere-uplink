#include "gpu_backend.h"
#include "gpu_sw.h"
#include <stdio.h>
#include <string.h>

extern void ac3_mem_init(void);
extern void psyq_DrawPrim(ctx_t *ctx);
extern void psyq_PutDrawEnv(ctx_t *ctx);
extern void psyq_PutDispEnv(ctx_t *ctx);
extern void psyq_SetDispMask(ctx_t *ctx);
extern void psyq_ClearImage(ctx_t *ctx);
extern void psyq_LoadImage(ctx_t *ctx);
extern void psyq_StoreImage(ctx_t *ctx);
extern void psyq_ResetGraph(ctx_t *ctx);

#define PKT   0x80120000u
#define ARG   0x80130000u
#define DATA  0x80131000u

static int failures;

static int check(const char *what, long got, long want) {
    int ok = (got == want);
    printf("  %-40s 0x%04lX %s\n", what, (unsigned long)got & 0xFFFFFFFFul,
           ok ? "ok" : "MISMATCH");
    if (!ok) { printf("      expected 0x%04lX\n", (unsigned long)want); failures++; }
    return ok;
}

static u16 px(int x, int y) { return ac3_vram[y * GPU_VRAM_W + x]; }
static void set_px(int x, int y, u16 v) { ac3_vram[y * GPU_VRAM_W + x] = v; }
static u16 c555(int r, int g, int b) { return (u16)(r | (g << 5) | (b << 10)); }
static u32 xy(int x, int y) { return ((u32)(y & 0xFFFF) << 16) | (u32)(x & 0xFFFF); }
static u32 rgb(int r, int g, int b) {
    return ((u32)(b & 0xFF) << 16) | ((u32)(g & 0xFF) << 8) | (u32)(r & 0xFF);
}

static void gp0(const u32 *w, int n) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    MEM_W32(PKT, ((u32)n << 24) | 0x00FFFFFFu);
    for (int i = 0; i < n; i++) MEM_W32(PKT + 4u + 4u * (u32)i, w[i]);
    c.r[4] = PKT;
    psyq_DrawPrim(&c);
}
#define GP0(...) do { const u32 w_[] = { __VA_ARGS__ }; \
                      gp0(w_, (int)(sizeof w_ / sizeof w_[0])); } while (0)

static void reset_state(void) {
    GP0(0xE1000000u, 0xE2000000u, 0xE3000000u,
        0xE4000000u | (511u << 10) | 1023u, 0xE5000000u, 0xE6000000u);
}

static void clear_vram(u16 v) {
    for (int i = 0; i < GPU_VRAM_W * GPU_VRAM_H; i++) ac3_vram[i] = v;
}

static void t_coverage(void) {
    printf("  --- which pixels an edge includes ---\n");
    clear_vram(0);
    reset_state();
    GP0(0x28000000u | rgb(0xF8, 0, 0), xy(10, 10), xy(14, 10), xy(10, 14), xy(14, 14));
    int inside = 1, outside = 1;
    for (int y = 8; y < 17; y++)
        for (int x = 8; x < 17; x++) {
            int in = x >= 10 && x < 14 && y >= 10 && y < 14;
            if (in) inside &= px(x, y) == 0x001F;
            else    outside &= px(x, y) == 0;
        }
    check("quad (10,10)-(14,14): 4x4 pixels", inside, 1);
    check("  nothing on its right/bottom edge", outside, 1);

    GP0(0x20000000u | rgb(0, 0xF8, 0), xy(20, 20), xy(24, 20), xy(20, 24));
    int tri = 1;
    for (int y = 19; y < 26; y++)
        for (int x = 19; x < 26; x++) {
            int in = y >= 20 && y < 24 && x >= 20 && x < 24 - (y - 20);
            tri &= px(x, y) == (in ? 0x03E0 : 0);
        }
    check("triangle: rows 4,3,2,1 pixels wide", tri, 1);
}

static void t_culling(void) {
    printf("  --- primitives the GPU refuses ---\n");
    clear_vram(0);
    reset_state();
    GP0(0x20000000u | rgb(0xF8, 0xF8, 0xF8), xy(0, 0), xy(1024, 0), xy(0, 1));
    check("triangle 1024 wide: dropped", px(0, 0), 0);
    GP0(0x20000000u | rgb(0xF8, 0xF8, 0xF8), xy(0, 5), xy(1023, 5), xy(0, 6));
    check("triangle 1023 wide: drawn", px(500, 5), 0x7FFF);
    GP0(0x20000000u | rgb(0xF8, 0xF8, 0xF8), xy(2, 0), xy(3, 0), xy(2, 512));
    check("triangle 512 tall: dropped", px(2, 100), 0);
    GP0(0x20000000u | rgb(0xF8, 0xF8, 0xF8), xy(4, 0), xy(5, 0), xy(4, 511));
    check("triangle 511 tall: drawn", px(4, 0), 0x7FFF);
    GP0(0x28000000u | rgb(0xF8, 0, 0), xy(1000, 100), xy(1004, 100),
        xy(1000, 104), xy(-30, 104));
    check("quad: small half drawn", px(1000, 100), 0x001F);
    check("  over-wide half dropped", px(900, 103), 0);
    GP0(0x60000000u | rgb(0xF8, 0, 0), xy(2048 + 30, 40), xy(2, 2));
    check("x = 2078 is x = 30", px(30, 40), 0x001F);
}

static void t_offset_area(void) {
    printf("  --- drawing offset and drawing area ---\n");
    clear_vram(0);
    reset_state();
    GP0(0xE5000000u | (5u << 11) | 5u);
    GP0(0x60000000u | rgb(0, 0, 0xF8), xy(10, 10), xy(4, 4));
    check("TILE (10,10)+offset (5,5) lands at 15", px(15, 15), 0x7C00);
    check("  ...to 18", px(18, 18), 0x7C00);
    check("  ...not 19", px(19, 18), 0);
    GP0(0xE5000000u, 0xE3000000u | (40u << 10) | 40u,
        0xE4000000u | (41u << 10) | 41u);
    GP0(0x60000000u | rgb(0xF8, 0, 0), xy(38, 38), xy(8, 8));
    check("clipped to the area: (40,40) in", px(40, 40), 0x001F);
    check("  (41,41) in -- the corner is inclusive", px(41, 41), 0x001F);
    check("  (42,41) out", px(42, 41), 0);
    check("  (39,40) out", px(39, 40), 0);
}

static void t_colour(void) {
    printf("  --- colours are five bits, blends are too ---\n");
    clear_vram(0);
    reset_state();
    GP0(0x60000000u | rgb(0x7F, 0x80, 0xFF), xy(0, 0), xy(1, 1));
    check("rgb(7F,80,FF) truncates to 15,16,31", px(0, 0), 0x7E0F);

    const u16 want[4] = { c555(16, 15, 6), c555(31, 31, 13),
                          c555(30, 0, 7),  c555(31, 7, 10) };
    const char *name[4] = { "B/2+F/2", "B+F", "B-F", "B+F/4" };
    for (int m = 0; m < 4; m++) {
        set_px(10 + m, 0, c555(31, 0, 10));
        GP0(0xE1000000u | ((u32)m << 5));
        GP0(0x62000000u | rgb(8, 248, 24), xy(10 + m, 0), xy(1, 1));
        char label[48];
        snprintf(label, sizeof label, "semi-transparent TILE, %s", name[m]);
        check(label, px(10 + m, 0), want[m]);
    }
}

static const u32 CLUT_4 = (480u << 6) | 0u;

static void t_texture(void) {
    printf("  --- texels, palettes, per-texel blending ---\n");
    clear_vram(0);
    reset_state();
    set_px(0, 480, 0x0000);
    set_px(1, 480, 0x001F);
    set_px(2, 480, 0x83E0);
    set_px(3, 480, 0x8000);
    set_px(512, 0, 0x3210);
    for (int x = 20; x < 24; x++) set_px(x, 30, c555(1, 1, 1));

    GP0(0xE1000000u | (1u << 5) | 8u);
    GP0(0x67000000u, xy(20, 30), CLUT_4 << 16, xy(4, 1));
    check("index 0 (0x0000) is not drawn", px(20, 30), c555(1, 1, 1));
    check("opaque texel replaces", px(21, 30), 0x001F);
    check("STP texel adds, keeps bit 15", px(22, 30), 0x8000 | c555(1, 31, 1));
    check("black STP texel is drawn (adds 0)", px(23, 30), 0x8000 | c555(1, 1, 1));

    set_px(640, 0, c555(16, 16, 16));
    GP0(0xE1000000u | (2u << 7) | 10u);
    GP0(0x64000000u | rgb(128, 128, 128), xy(30, 30), 0, xy(1, 1));
    check("modulation by 128 is unity", px(30, 30), c555(16, 16, 16));
    GP0(0x64000000u | rgb(255, 64, 0), xy(31, 30), 0, xy(1, 1));
    check("modulation: (16*c)>>4, then >>3", px(31, 30), c555(31, 8, 0));

    for (int u = 0; u < 16; u++) set_px(640 + u, 1, (u16)(0x100 + u));
    GP0(0xE2000000u | 1u);
    GP0(0x65000000u, xy(40, 31), (1u << 8), xy(16, 1));
    check("window: u=3 reads texel 3", px(43, 31), 0x103);
    check("window: u=11 reads texel 3", px(51, 31), 0x103);
    GP0(0xE2000000u | (1u << 10) | 1u);
    GP0(0x65000000u, xy(40, 32), (1u << 8), xy(16, 1));
    check("window origin 8: u=3 reads texel 11", px(43, 32), 0x10B);
}

static void t_dither(void) {
    printf("  --- the 4x4 ordered dither ---\n");
    clear_vram(0);
    reset_state();
    GP0(0xE1000200u);
    GP0(0x38000000u | rgb(14, 14, 14), xy(32, 32), rgb(14, 14, 14), xy(36, 32),
        rgb(14, 14, 14), xy(32, 36), rgb(14, 14, 14), xy(36, 36));
    static const int d[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 },
                                 { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
    int all = 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int v = (14 + d[y][x]) >> 3;
            if (px(32 + x, 32 + y) != c555(v, v, v)) {
                printf("      (%d,%d) got 0x%04X want 0x%04X\n", x, y,
                       px(32 + x, 32 + y), c555(v, v, v));
                all = 0;
            }
        }
    check("gouraud: (14 + matrix) >> 3", all, 1);
    GP0(0x28000000u | rgb(14, 14, 14), xy(40, 32), xy(44, 32), xy(40, 36), xy(44, 36));
    check("flat untextured: never dithered", px(40, 33), c555(1, 1, 1));
    GP0(0x60000000u | rgb(15, 15, 15), xy(48, 33), xy(4, 1));
    check("rectangle: never dithered", px(48, 33), c555(1, 1, 1));
    GP0(0xE1000000u);
    GP0(0x38000000u | rgb(14, 14, 14), xy(52, 32), rgb(14, 14, 14), xy(56, 32),
        rgb(14, 14, 14), xy(52, 36), rgb(14, 14, 14), xy(56, 36));
    check("dither off: gouraud truncates", px(52, 33), c555(1, 1, 1));
}

static void t_mask(void) {
    printf("  --- the mask bit ---\n");
    clear_vram(0);
    reset_state();
    GP0(0xE6000001u);
    GP0(0x60000000u | rgb(0xF8, 0, 0), xy(0, 50), xy(2, 1));
    check("E6 bit 0: writes carry bit 15", px(0, 50), 0x801F);
    set_px(10, 50, 0x8000 | 0x1234);
    set_px(11, 50, 0x1234);
    GP0(0xE6000002u);
    GP0(0x60000000u | rgb(0, 0xF8, 0), xy(10, 50), xy(2, 1));
    check("E6 bit 1: pixel with bit 15 kept", px(10, 50), 0x9234);
    check("  pixel without it written", px(11, 50), 0x03E0);
    GP0(0xE6000001u, 0xA0000000u, xy(20, 50), xy(2, 1), 0x00020001u);
    check("upload under E6 bit 0", px(20, 50), 0x8001);
    GP0(0xE6000000u);
}

static void t_lines(void) {
    printf("  --- lines ---\n");
    clear_vram(0);
    reset_state();
    GP0(0xE5000000u | (60u << 11) | 40u);
    GP0(0x40000000u | rgb(0xF8, 0xF8, 0xF8), xy(0, 0), xy(3, 0));
    check("horizontal: first end point", px(40, 60), 0x7FFF);
    check("  last end point included", px(43, 60), 0x7FFF);
    check("  nothing past it", px(44, 60), 0);
    GP0(0x40000000u | rgb(0xF8, 0, 0), xy(0, 2), xy(3, 5));
    int diag = px(40, 62) == 0x1F && px(41, 63) == 0x1F &&
               px(42, 64) == 0x1F && px(43, 65) == 0x1F;
    check("diagonal: one pixel per step", diag, 1);
    GP0(0x40000000u | rgb(0xF8, 0, 0), xy(0, 10), xy(1024, 10));
    check("1024 wide: dropped", px(40, 70), 0);
    GP0(0x48000000u | rgb(0, 0, 0xF8), xy(0, 20), xy(2, 20), xy(2, 22), 0x55555555u);
    check("polyline: second segment drawn", px(42, 82), 0x7C00);
}

static void put_disp(int x, int y, int w, int h, int rgb24) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 5; i++) MEM_W32(ARG + 4u * (u32)i, 0);
    MEM_W16(ARG + 0, (u16)x); MEM_W16(ARG + 2, (u16)y);
    MEM_W16(ARG + 4, (u16)w); MEM_W16(ARG + 6, (u16)h);
    MEM_W8(ARG + 17, (u8)rgb24);
    c.r[4] = ARG;
    psyq_PutDispEnv(&c);
}

static void t_display(void) {
    printf("  --- what the video output scans out ---\n");
    gpu_display_t d;
    ctx_t c;
    memset(&c, 0, sizeof c);
    put_disp(0, 256, 320, 240, 0);
    gpu_display_state(&d);
    check("DISPENV (0,256) 320x240: start y", d.y, 256);
    check("  width", d.w, 320);
    check("  lines", d.h, 240);
    put_disp(0, 0, 320, 480, 0);
    gpu_display_state(&d);
    check("disp.h 480: interlaced 480 lines", d.h, 480);
    put_disp(0, 0, 256, 240, 0);
    gpu_display_state(&d);
    check("disp.w 256: 256 mode", d.w, 256);
    put_disp(0, 0, 640, 240, 0);
    gpu_display_state(&d);
    check("disp.w 640: 640 mode", d.w, 640);
    put_disp(0, 256, 320, 240, 1);
    gpu_display_state(&d);
    check("isrgb24", d.rgb24, 1);
    c.r[4] = 0; psyq_SetDispMask(&c);
    gpu_display_state(&d);
    check("SetDispMask(0): blank", d.enabled, 0);
    c.r[4] = 1; psyq_SetDispMask(&c);
    gpu_display_state(&d);
    check("SetDispMask(1): on", d.enabled, 1);
}

static void put_env(int cx, int cy, int cw, int ch, int ox, int oy, int bg,
                    int r, int g, int b) {
    ctx_t c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 23; i++) MEM_W32(ARG + 4u * (u32)i, 0);
    MEM_W16(ARG + 0, (u16)cx); MEM_W16(ARG + 2, (u16)cy);
    MEM_W16(ARG + 4, (u16)cw); MEM_W16(ARG + 6, (u16)ch);
    MEM_W16(ARG + 8, (u16)ox); MEM_W16(ARG + 10, (u16)oy);
    MEM_W8(ARG + 24, (u8)bg);
    MEM_W8(ARG + 25, (u8)r); MEM_W8(ARG + 26, (u8)g); MEM_W8(ARG + 27, (u8)b);
    c.r[4] = ARG;
    psyq_PutDrawEnv(&c);
}

static void t_env(void) {
    printf("  --- PutDrawEnv and ClearImage ---\n");
    clear_vram(0);
    reset_state();
    GP0(0xE6000003u);
    put_env(0, 256, 320, 240, 0, 256, 1, 0xF8, 0, 0);
    check("isbg, 64-aligned: filled", px(0, 256), 0x001F);
    check("  to its last pixel", px(319, 495), 0x001F);
    check("  and no further", px(320, 256), 0);
    check("the environment turned the mask off", gpu_sw.mask_or | gpu_sw.mask_and, 0);
    check("  set the area", gpu_sw.area_y1, 495);
    check("  and the offset", gpu_sw.ofs_y, 256);
    put_env(8, 300, 100, 20, 8, 300, 1, 0, 0xF8, 0);
    check("isbg, unaligned: drawn at the clip origin", px(8, 300), 0x03E0);
    check("  to its last pixel", px(107, 319), 0x03E0);
    check("  and no further", px(108, 300), 0x001F);

    GP0(0xE3000000u | (10u << 10) | 10u, 0xE4000000u | (11u << 10) | 11u);
    ctx_t c;
    memset(&c, 0, sizeof c);
    MEM_W16(ARG + 0, 500); MEM_W16(ARG + 2, 40);
    MEM_W16(ARG + 4, 10);  MEM_W16(ARG + 6, 2);
    c.r[4] = ARG; c.r[5] = 0; c.r[6] = 0; c.r[7] = 0xF8;
    psyq_ClearImage(&c);
    check("ClearImage unaligned: cleared outside the area", px(509, 41), 0x7C00);
    check("  not past its width", px(510, 41), 0);
    check("  drawing area restored", gpu_sw.area_x1, 11);
    MEM_W16(ARG + 0, 576); MEM_W16(ARG + 2, 40);
    MEM_W16(ARG + 4, 64);  MEM_W16(ARG + 6, 1);
    c.r[4] = ARG; c.r[5] = 0xF8; c.r[6] = 0xF8; c.r[7] = 0xF8;
    psyq_ClearImage(&c);
    check("ClearImage aligned: a fill", px(639, 40), 0x7FFF);
}

static void t_readback(void) {
    printf("  --- the game reads back what it drew ---\n");
    clear_vram(0);
    reset_state();
    GP0(0x60000000u | rgb(0x40, 0x80, 0xC0), xy(100, 100), xy(2, 1));
    ctx_t c;
    memset(&c, 0, sizeof c);
    MEM_W16(ARG + 0, 100); MEM_W16(ARG + 2, 100);
    MEM_W16(ARG + 4, 2);   MEM_W16(ARG + 6, 1);
    MEM_W32(DATA, 0);
    c.r[4] = ARG; c.r[5] = DATA;
    psyq_StoreImage(&c);
    check("StoreImage returns the drawn pixel", MEM_R16U(DATA), c555(8, 16, 24));
}

int ac3_selftest_raster(void) {
    ac3_mem_init();
    ac3_vram_init();
    {
        ctx_t c;
        memset(&c, 0, sizeof c);
        psyq_ResetGraph(&c);
    }
    printf("software GPU self-test\n");
    t_coverage();
    t_culling();
    t_offset_area();
    t_colour();
    t_texture();
    t_dither();
    t_mask();
    t_lines();
    t_display();
    t_env();
    t_readback();
    printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return failures ? 1 : 0;
}
