/*
 * hw_selftest.c -- native tools only: --selftest-hw. The enhanced renderer
 * draws test scenes (flat, textured, see-through, sprites, the mask bit,
 * VRAM copies) at 1x and 4x and is compared with the software GPU, which is
 * the reference; plus the precise vertices of a known projection, and the
 * order of deferred see-through primitives.
 */
#ifndef AC3_HW_WEBGPU
#ifdef _WIN32
#include <direct.h>
#endif
#include "hw_glue_internal.h"

extern void ac3_vram_init(void);

static int st_fail;

static void st_upload(int x, int y, int w, int h, const u16 *px) {
    PSX_RECT r = { (s16)x, (s16)y, (s16)w, (s16)h };
    gpu_sw_load_begin(&r);
    for (int i = 0; i < w * h; i += 2) {
        u32 lo = px[i], hi = (i + 1 < w * h) ? px[i + 1] : 0;
        gpu_sw_load_word(lo | (hi << 16));
    }
}

static void st_env(int x0, int y0, int x1, int y1, int ox, int oy, u16 texpage) {
    gpu_sw_gp0_state(0xE1000000u | texpage);
    gpu_sw_gp0_state(0xE3000000u | (u32)x0 | ((u32)y0 << 10));
    gpu_sw_gp0_state(0xE4000000u | (u32)x1 | ((u32)y1 << 10));
    gpu_sw_gp0_state(0xE5000000u | ((u32)ox & 0x7FF) | (((u32)oy & 0x7FF) << 11));
}

static u32 st_rng = 1234567u;
static u32 st_rand(void) { st_rng = st_rng * 1103515245u + 12345u; return st_rng >> 8; }

static u16 page4, page8, page15, page15n, clut4, clut8;

static void st_textures(void) {
    static u16 t4[64 * 256], t8[128 * 256], t15[256 * 256], n15[256 * 256];
    static u16 c4[16], c8[256];
    for (int v = 0; v < 256; v++)
        for (int w = 0; w < 64; w++) {
            u16 word = 0;
            for (int k = 0; k < 4; k++) {
                int u = w * 4 + k;
                word |= (u16)((((u >> 4) + (v >> 5)) & 15) << (k * 4));
            }
            t4[v * 64 + w] = word;
        }
    for (int i = 0; i < 16; i++)
        c4[i] = (u16)((i * 2) | ((31 - i) << 5) | ((8 + i) << 10) | ((i & 1) << 15));
    c4[0] = 0x0421;
    for (int v = 0; v < 256; v++)
        for (int w = 0; w < 128; w++) {
            int u0 = w * 2, u1 = w * 2 + 1;
            t8[v * 128 + w] = (u16)(((u0 + v) & 255) | (((u1 + v) & 255) << 8));
        }
    for (int i = 0; i < 256; i++)
        c8[i] = (u16)((i >> 3) | (((255 - i) >> 3) << 5) | (16 << 10) | ((i & 32) << 10));
    c8[0] = 0x0842;
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++) {
            t15[v * 256 + u] = (u16)((u >> 3) | ((v >> 3) << 5) | (((u + v) >> 4) << 10) |
                                     (((u >> 5) & 1) << 15));
            n15[v * 256 + u] = (u16)(st_rand() | 1u);
        }
    st_upload(640, 0, 64, 256, t4);
    st_upload(704, 0, 128, 256, t8);
    st_upload(768, 256, 256, 256, t15);
    st_upload(512, 256, 256, 256, n15);
    st_upload(0, 480, 16, 1, c4);
    st_upload(0, 481, 256, 1, c8);
    clut4 = (u16)(480 << 6);
    clut8 = (u16)(481 << 6);
    page4 = (u16)(10 | (0 << 7));
    page8 = (u16)(11 | (1 << 7));
    page15 = (u16)(12 | (1 << 4) | (2 << 7));
    page15n = (u16)(8 | (1 << 4) | (2 << 7));
}

static void st_clear(void) {
    PSX_RECT bg = { 0, 0, 320, 240 };
    gpu_sw_fill(&bg, 0x2108);
    st_env(0, 0, 319, 239, 0, 0, 0);
    gpu_sw_gp0_state(0xE6000000u);
}

static void st_poly(int nverts, int gouraud, int kind, int raw, int semi, int blend,
                    s16 cx, s16 cy, int rx, int ry) {
    gpu_poly_t p;
    memset(&p, 0, sizeof p);
    p.nverts = nverts;
    p.gouraud = gouraud;
    p.textured = kind != 0;
    p.raw = raw;
    p.semitrans = semi;
    u16 b = (u16)(blend << 5);
    p.tpage = (u16)((kind == 1 ? page4 : kind == 2 ? page8 : page15) | b);
    if (!p.textured) p.tpage = b;
    p.clut = kind == 1 ? clut4 : clut8;
    for (int i = 0; i < nverts; i++) {
        p.x[i] = (s16)(cx + (s16)(st_rand() % (2 * rx + 1)) - rx);
        p.y[i] = (s16)(cy + (s16)(st_rand() % (2 * ry + 1)) - ry);
        p.u[i] = (u8)st_rand(); p.v[i] = (u8)st_rand();
        p.r[i] = (u8)st_rand(); p.g[i] = (u8)st_rand(); p.b[i] = (u8)st_rand();
        if (!gouraud && i) { p.r[i] = p.r[0]; p.g[i] = p.g[0]; p.b[i] = p.b[0]; }
    }
    if (p.textured) gpu_sw_texpage_attr(p.tpage);
    else gpu_sw.texpage = (u16)((gpu_sw.texpage & ~0x60u) | b);
    gpu_sw_texwin(&p.twin_mask, &p.twin_off);
    gpu_sw_poly(&p);
}

static void scene_flat(void) {
    for (int n = 0; n < 120; n++)
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), 0, 0, 0, 0,
                (s16)(20 + st_rand() % 280), (s16)(20 + st_rand() % 200), 50, 40);
}

static void scene_textured(void) {
    for (int n = 0; n < 120; n++)
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), 1 + (int)(st_rand() % 3),
                (st_rand() & 3) == 0, 0, 0,
                (s16)(20 + st_rand() % 280), (s16)(20 + st_rand() % 200), 50, 40);
}

static void scene_blend(void) {
    for (int i = 0; i < 16; i++) {
        PSX_RECT r = { (s16)(i * 20), 0, 20, 240 };
        gpu_sw_fill(&r, (u16)((i * 2) | ((31 - i * 2) << 5) | ((i & 1) ? 0x7C00 : 0)));
    }
    for (int n = 0; n < 80; n++) {
        int kind = (int)(st_rand() % 4);
        st_poly((st_rand() & 1) ? 4 : 3, (int)(st_rand() & 1), kind, 0, 1,
                (int)(st_rand() & 3), (s16)(20 + st_rand() % 280),
                (s16)(20 + st_rand() % 200), 45, 35);
    }
}

static void scene_sprites(void) {
    for (int n = 0; n < 60; n++) {
        gpu_rect_t r;
        memset(&r, 0, sizeof r);
        r.textured = 1;
        r.raw = (int)(st_rand() & 1);
        r.x = (s16)(st_rand() % 290); r.y = (s16)(st_rand() % 210);
        r.w = (u16)(1 + st_rand() % 60); r.h = (u16)(1 + st_rand() % 60);
        r.u = (u8)st_rand(); r.v = (u8)st_rand();
        r.r = (u8)(64 + st_rand() % 128); r.g = (u8)(64 + st_rand() % 128);
        r.b = (u8)(64 + st_rand() % 128);
        gpu_sw.texpage = (u16)((page15n & 0x1FF) |
                               ((n % 5 == 0) ? 0x1000 : 0) | ((n % 7 == 0) ? 0x2000 : 0));
        r.tpage = gpu_sw.texpage;
        gpu_sw_texwin(&r.twin_mask, &r.twin_off);
        gpu_sw_rect(&r);
    }
}

static void scene_mask(void) {
    gpu_sw_gp0_state(0xE6000001u);
    gpu_poly_t m;
    memset(&m, 0, sizeof m);
    m.nverts = 4;
    m.x[0] = 100; m.y[0] = 60; m.x[1] = 220; m.y[1] = 60;
    m.x[2] = 100; m.y[2] = 180; m.x[3] = 220; m.y[3] = 180;
    for (int i = 0; i < 4; i++) { m.r[i] = 255; m.g[i] = 64; m.b[i] = 0; }
    gpu_sw_poly(&m);
    gpu_sw_gp0_state(0xE6000002u);
    for (int i = 0; i < 4; i++) { m.x[i] = (s16)(m.x[i] - 50); m.y[i] = (s16)(m.y[i] - 40); m.g[i] = 255; }
    gpu_sw_poly(&m);
    for (int k = 0; k < 8; k++)
        st_poly(4, 1, (k & 1) ? 3 : 0, 0, 1, k >> 1,
                (s16)(90 + k * 20), (s16)(110 + (k & 3) * 12), 40, 30);
    gpu_sw_gp0_state(0xE6000000u);
}

static void scene_copy(void) {
    scene_flat();
    PSX_RECT src = { 40, 40, 64, 32 };
    gpu_sw_copy(&src, 200, 150);
    PSX_RECT src2 = { 0, 0, 16, 1 };
    gpu_sw_gp0_state(0xE6000001u);
    gpu_sw_copy(&src2, 32, 480);
    gpu_sw_gp0_state(0xE6000000u);
}

typedef struct {
    const char *name;
    void (*draw)(void);
    double min_ok;
    double min_exact;
} st_case_t;

static int st_run(const st_case_t *c, int S) {
    st_clear();
    c->draw();
    headless_flush();
    int W = 320, Hh = 240;
    u8 *px = malloc((size_t)W * S * Hh * S * 4);
    if (!px || !hw_readback(HV.q, HV.pool, 0, 0, W, Hh, px)) { free(px); return 0; }
    long in_total = 0, in_exact = 0, in_ok = 0, in_mask = 0, all_ok = 0;
    for (int y = 0; y < Hh; y++)
        for (int x = 0; x < W; x++) {
            size_t hy = (size_t)y * S + (size_t)(S / 2), hx = (size_t)x * S + (size_t)(S / 2);
            const u8 *h = px + (hy * (size_t)(W * S) + hx) * 4;
            u16 s = *vram_at(x, y);
            int dr = abs((h[0] >> 3) - (s & 31));
            int dg = abs((h[1] >> 3) - ((s >> 5) & 31));
            int db = abs((h[2] >> 3) - ((s >> 10) & 31));
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d <= 2) all_ok++;
            int interior = 1;
            for (int j = -1; j <= 1 && interior; j++)
                for (int i = -1; i <= 1; i++) {
                    int nx = x + i, ny = y + j;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= Hh) continue;
                    u16 n = *vram_at(nx, ny);
                    if (((n ^ s) & 0x8000u) ||
                        abs((int)(n & 31) - (int)(s & 31)) > 3 ||
                        abs((int)((n >> 5) & 31) - (int)((s >> 5) & 31)) > 3 ||
                        abs((int)((n >> 10) & 31) - (int)((s >> 10) & 31)) > 3) {
                        interior = 0;
                        break;
                    }
                }
            if (!interior) continue;
            in_total++;
            if (d == 0) in_exact++;
            if (d <= 2) in_ok++;
            if (((s >> 15) & 1) != (h[3] > 127)) in_mask++;
        }
    double it = in_total ? (double)in_total : 1.0;
    double pe = 100.0 * in_exact / it, pok = 100.0 * in_ok / it;
    double pm = 100.0 * in_mask / it;
    double pall = 100.0 * all_ok / ((double)W * Hh);
    int pass = pok >= c->min_ok && (S > 1 || pe >= c->min_exact) &&
               pm <= 100.0 - c->min_ok && pall >= 85.0;
    printf("  %dx %-22s interior %6.2f%% identical %6.2f%% within 2/31, mask %4.2f%% off;"
           " all pixels %6.2f%%  %s\n",
           S, c->name, pe, pok, pm, pall, pass ? "ok" : "MISMATCH");
    char name[96];
    snprintf(name, sizeof name, "selftest_out/hw_%s_%dx.png", c->name, S);
    for (char *q = name; *q; q++) if (*q == ' ') *q = '_';
    write_rgba_png(name, px, (unsigned)(W * S), (unsigned)(Hh * S));
    snprintf(name, sizeof name, "selftest_out/hw_%s_native.png", c->name);
    for (char *q = name; *q; q++) if (*q == ' ') *q = '_';
    write_sw_png(name, 0, 0, W, Hh);
    free(px);
    return pass;
}

static void st_gte(void) {
    ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ac3_gte_ctc2(&ctx, 0, 0x00001000); ac3_gte_ctc2(&ctx, 1, 0);
    ac3_gte_ctc2(&ctx, 2, 0x00001000); ac3_gte_ctc2(&ctx, 3, 0);
    ac3_gte_ctc2(&ctx, 4, 0x1000);
    ac3_gte_ctc2(&ctx, 5, 0); ac3_gte_ctc2(&ctx, 6, 0); ac3_gte_ctc2(&ctx, 7, 0);
    ac3_gte_ctc2(&ctx, 24, 160u << 16);
    ac3_gte_ctc2(&ctx, 25, 120u << 16);
    ac3_gte_ctc2(&ctx, 26, 256);
    ac3_gte_mtc2(&ctx, 0, (37u << 16) | 100u);
    ac3_gte_mtc2(&ctx, 1, 700);
    for (int ws = 0; ws < 2; ws++) {
        ac3_ws_factor = ws ? 0.75f : 1.0f;
        ac3_pgxp_enabled = 1;
        ac3_gte_op(&ctx, 0x00080001);
        u32 sxy = ac3_gte_mfc2(&ctx, 14);
        s16 ix = (s16)(sxy & 0xFFFF), iy = (s16)(sxy >> 16);
        double want_x = 160.0 + 100.0 * 256.0 / 700.0 * ac3_ws_factor;
        double want_y = 120.0 + 37.0 * 256.0 / 700.0;
        ac3_pgxp_vtx_t v;
        memset(&v, 0, sizeof v);
        int found = ac3_pgxp_lookup(sxy, &v);
        int ok = found && fabs(v.sx - want_x) < 0.01 && fabs(v.sy - want_y) < 0.01 &&
                 fabs(v.vz - 700.0) < 0.01 && abs(ix - (int)want_x) <= 1 &&
                 abs(iy - (int)want_y) <= 1;
        printf("  GTE %s: integer (%d,%d), precise (%.3f,%.3f) z %.1f, expected (%.3f,%.3f)  %s\n",
               ws ? "16:9" : "4:3 ", ix, iy, v.sx, v.sy, v.vz, want_x, want_y,
               ok ? "ok" : "MISMATCH");
        if (!ok) st_fail++;
    }
    ac3_ws_factor = 1.0f;
    ac3_pgxp_next_frame();
}

static void st_effect_order(void) {
    ac3_hd_settings_t saved = ac3_hd;
    int map_valid = hw_map.valid;
    memset(&ac3_hd, 0, sizeof ac3_hd);
    ac3_hd.fx = 1;
    hw_map.valid = 1;
    hw_scene_t scene = { 0 };
    scene.valid = 1; scene.H = 256; scene.ws = 1;
    scene.rot[0] = scene.rot[4] = scene.rot[8] = 1;
    scene.sun[1] = -1;
    for (int i = 0; i < HW_SKY_BINS; i++)
        for (int k = 0; k < 3; k++) scene.sky[i][k] = 0.2f;
    int S = hw_scale(), W = 320 * S, Hh = 240 * S;
    u8 *px = malloc((size_t)W * Hh * 4);
    int ok = px != NULL;
    for (int list = 0; list < 2 && ok; list++) {
        st_clear();
        glue_list_begin(&scene);
        glue_phase(PH_TERRAIN);
        gpu_poly_t p = { 0 };
        p.nverts = 4;
        p.x[0] = p.x[2] = 40; p.x[1] = p.x[3] = 200;
        p.y[0] = p.y[1] = 40; p.y[2] = p.y[3] = 90;
        for (int k = 0; k < 4; k++) p.r[k] = 120;
        p.semitrans = 1; p.tpage = 1 << 5;
        glue_poly(&gpu_sw, &p, NULL);
        gpu_rect_t r = { 0 };
        r.x = 40; r.y = 100; r.w = 160; r.h = 30;
        r.r = 120; r.semitrans = 1; r.tpage = 1 << 5;
        glue_rect(&gpu_sw, &r);
        gpu_line_t l = { 0 };
        l.nverts = 2; l.x[0] = 40; l.x[1] = 200;
        l.y[0] = l.y[1] = 140; l.r[0] = l.r[1] = 120;
        l.semitrans = 1; l.tpage = 1 << 5;
        glue_line(&gpu_sw, &l);
        if (!list) {
            p.semitrans = 0;
            p.x[0] = p.x[2] = 100; p.x[1] = p.x[3] = 180;
            p.y[0] = p.y[1] = 20; p.y[2] = p.y[3] = 180;
            for (int k = 0; k < 4; k++) { p.r[k] = 0; p.b[k] = 120; }
            glue_poly(&gpu_sw, &p, NULL);
        }
        r.x = 100; r.y = 155; r.w = 80; r.h = 10;
        glue_rect(&gpu_sw, &r);
        glue_phase(PH_HUD);
        glue_list_end();
        headless_flush();
        if (!hw_readback(HV.q, HV.pool, 0, 0, 320, 240, px)) { ok = 0; break; }
        const int ys[] = { 60, 115, 140 };
        for (int i = 0; i < 3; i++) {
            const u8 *visible = px + ((size_t)(ys[i] * S) * W + 60 * S) * 4;
            const u8 *covered = px + ((size_t)(ys[i] * S) * W + 120 * S) * 4;
            if (visible[0] < 100 || (list ? covered[0] < 100 : covered[0] > 8)) ok = 0;
        }
        const u8 *front = px + ((size_t)(160 * S) * W + 120 * S) * 4;
        if (front[0] < 100) ok = 0;
    }
    printf("  %dx deferred polygon/sprite/line occlusion and list reset  %s\n", S, ok ? "ok" : "MISMATCH");
    if (!ok) st_fail++;
    free(px);
    ac3_hd = saved;
    hw_map.valid = map_valid;
    hw_draw_order(0);
}

int ac3_selftest_hw(void) {
    static const st_case_t cases[] = {
        { "flat and gouraud",   scene_flat,     99.0, 80.0 },
        { "textured 4 8 15 bit", scene_textured, 97.0,  0.0 },
        { "semi-transparency",  scene_blend,    97.0,  0.0 },
        { "sprites",            scene_sprites,  99.5,  0.0 },
        { "mask bit",           scene_mask,     99.5, 98.0 },
        { "vram copies",        scene_copy,     99.0, 80.0 },
    };
    printf("enhanced renderer: precise vertices\n");
    st_gte();
    printf("enhanced renderer against the software GPU\n");
    _mkdir("selftest_out");
    int scales[2] = { 1, 4 };
    ac3_vram_init();
    ac3_hd.pgxp = 0;
    for (int si = 0; si < 2; si++) {
        memset(ac3_vram, 0, (size_t)GPU_VRAM_W * GPU_VRAM_H * 2);
        gpu_sw_reset();
        st_rng = 1234567u;
        if (!headless_init(scales[si])) { printf("  no Vulkan device\n"); return 1; }
        int S = hw_scale();
        gpu_sw_hooks = &glue_hooks;
        st_textures();
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            st_rng = 777u + i;
            if (!st_run(&cases[i], S)) st_fail++;
        }
        st_effect_order();
        gpu_sw_hooks = NULL;
        headless_shutdown();
    }
    ac3_hd.pgxp = 1;
    printf("%s (%d failures)\n", st_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", st_fail);
    return st_fail ? 1 : 0;
}

#endif /* !AC3_HW_WEBGPU */
