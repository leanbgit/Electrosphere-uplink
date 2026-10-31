#include "gpu_sw.h"
#include "ac3_oracle.h"
#include <string.h>
#include <stdlib.h>

gpu_sw_state_t gpu_sw;

#ifdef AC3_ENHANCED
const gpu_sw_hooks_t *gpu_sw_hooks;
#define HOOK(fn, ...) do { if (gpu_sw_hooks && gpu_sw_hooks->fn) gpu_sw_hooks->fn(__VA_ARGS__); } while (0)
#else
#define HOOK(fn, ...) do { } while (0)
#endif

static gpu_display_t disp = { 1, 0, 0, 320, 240, 0 };
static u32 prims_drawn;

void gpu_display_state(gpu_display_t *out) { if (out) *out = disp; }
u32 gpu_sw_prim_count(void) { return prims_drawn; }

static inline s32 sx11(s32 v) { return (s32)((u32)v << 21) >> 21; }

#define VRAM(x, y) ac3_vram[((u32)(y) & 511u) * GPU_VRAM_W + ((u32)(x) & 1023u)]
#define MEMV(m, x, y) (m)[((u32)(y) & 511u) * GPU_VRAM_W + ((u32)(x) & 1023u)]

/* A second video memory bank (co-op: the guest's plane's textures). The game
 * never addresses it: a polygon reads its texture page from it when bit 11
 * of its texture page is set (the bit Sony's development units used for
 * their larger video memory) and its colour table when bit 15 of its colour
 * table is set (unused). NULL unless something put textures there. */
u16 *ac3_vram_bank;
void gpu_sw_bank_load(s32 x, s32 y, s32 w, s32 h, const u8 *src) {
    if (!ac3_vram_bank) ac3_vram_bank = (u16 *)calloc(GPU_VRAM_W * GPU_VRAM_H, 2);
    if (!ac3_vram_bank || !src) return;
    for (s32 r = 0; r < h; r++)
        for (s32 c = 0; c < w; c++, src += 2)
            MEMV(ac3_vram_bank, x + c, y + r) = (u16)(src[0] | src[1] << 8);
}

static const s8 dither_matrix[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 },
};

static u8 lut_dither[4][4][512];
static u8 lut_plain[512];

static void build_luts(void) {
    static int ready;
    if (ready) return;
    for (int v = 0; v < 512; v++) {
        int p = v >> 3;
        lut_plain[v] = (u8)(p > 31 ? 31 : p);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int d = (v + dither_matrix[y][x]) >> 3;
                lut_dither[y][x][v] = (u8)(d < 0 ? 0 : (d > 31 ? 31 : d));
            }
    }
    ready = 1;
}

void gpu_sw_reset(void) {
    build_luts();
    memset(&gpu_sw, 0, sizeof gpu_sw);
    gpu_sw_load_cancel();
    disp.enabled = 0;
    disp.x = disp.y = 0;
    disp.w = 256; disp.h = 240;
    disp.rgb24 = 0;
}

void gpu_sw_gp0_state(u32 w) {
    switch (w >> 24) {
    case 0xE1:
        gpu_sw.texpage = (u16)(w & 0x3FFFu);
        break;
    case 0xE2:
        gpu_sw.texwin = w & 0xFFFFFu;
        break;
    case 0xE3:
        gpu_sw.area_x0 = (s32)(w & 0x3FFu);
        gpu_sw.area_y0 = (s32)((w >> 10) & 0x3FFu);
        break;
    case 0xE4:
        gpu_sw.area_x1 = (s32)(w & 0x3FFu);
        gpu_sw.area_y1 = (s32)((w >> 10) & 0x3FFu);
        break;
    case 0xE5:
        gpu_sw.ofs_x = sx11((s32)(w & 0x7FFu));
        gpu_sw.ofs_y = sx11((s32)((w >> 11) & 0x7FFu));
        break;
    case 0xE6:
        gpu_sw.mask_or  = (w & 1u) ? 0x8000u : 0u;
        gpu_sw.mask_and = (w & 2u) ? 0x8000u : 0u;
        break;
    default:
        break;
    }
}

void gpu_sw_texpage_attr(u16 attr) {
    if (ac3_vram_bank) attr &= (u16)~0x800u;   /* the bank bit stays with its polygon */
    gpu_sw.texpage = (u16)((gpu_sw.texpage & 0x3600u) | (attr & 0x09FFu));
}

void gpu_sw_texwin(u16 *mask, u16 *off) {
    u32 w = gpu_sw.texwin;
    u32 mx = (w & 0x1Fu) << 3,         my = ((w >> 5) & 0x1Fu) << 3;
    u32 ox = ((w >> 10) & 0x1Fu) << 3, oy = ((w >> 15) & 0x1Fu) << 3;
    if (mask) *mask = (u16)((my << 8) | mx);
    if (off)  *off  = (u16)((oy << 8) | ox);
}

typedef struct {
    int textured, raw;
    int dither;
    int blend;
    u32 page_x, page_y, depth;
    u32 clut_x, clut_y;
    const u16 *tex, *pal;     /* ac3_vram, or the second bank */
    u32 and_u, or_u, and_v, or_v;
} shade_t;

static void shade_setup(shade_t *s, int textured, int raw, int semitrans,
                        u16 tpage, u16 clut, u16 tw_mask, u16 tw_off) {
    build_luts();
    memset(s, 0, sizeof *s);
    s->textured = textured;
    s->raw = raw;
    s->blend = semitrans ? (int)((tpage >> 5) & 3u) : -1;
    if (!textured) return;
    s->page_x = (tpage & 0xFu) * 64u;
    s->page_y = ((tpage >> 4) & 1u) * 256u;
    s->depth  = (tpage >> 7) & 3u;
    s->clut_x = (clut & 0x3Fu) * 16u;
    s->clut_y = (clut >> 6) & 0x1FFu;
    s->tex = (tpage & 0x800u) && ac3_vram_bank ? ac3_vram_bank : ac3_vram;
    s->pal = (clut & 0x8000u) && ac3_vram_bank ? ac3_vram_bank : ac3_vram;
    u32 mu = tw_mask & 0xFFu, mv = tw_mask >> 8;
    u32 ou = tw_off & 0xFFu,  ov = tw_off >> 8;
    s->and_u = ~mu & 0xFFu; s->or_u = ou & mu;
    s->and_v = ~mv & 0xFFu; s->or_v = ov & mv;
}

static inline u16 texel(const shade_t *s, u32 u, u32 v) {
    u = ((u & 0xFFu) & s->and_u) | s->or_u;
    v = ((v & 0xFFu) & s->and_v) | s->or_v;
    u32 y = s->page_y + v;
    switch (s->depth) {
    case 0: {
        u32 w = MEMV(s->tex, s->page_x + (u >> 2), y);
        u32 i = (w >> ((u & 3u) * 4u)) & 0xFu;
        return MEMV(s->pal, s->clut_x + i, s->clut_y);
    }
    case 1: {
        u32 w = MEMV(s->tex, s->page_x + (u >> 1), y);
        u32 i = (w >> ((u & 1u) * 8u)) & 0xFFu;
        return MEMV(s->pal, s->clut_x + i, s->clut_y);
    }
    default:
        return MEMV(s->tex, s->page_x + u, y);
    }
}

static inline u16 blend15(u16 b, u16 f, int mode) {
    s32 br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    s32 fr = f & 31, fg = (f >> 5) & 31, fb = (f >> 10) & 31;
    s32 r, g, bl;
    switch (mode) {
    case 0:  r = (br + fr) >> 1; g = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
    case 1:  r = br + fr;        g = bg + fg;        bl = bb + fb;        break;
    case 2:  r = br - fr;        g = bg - fg;        bl = bb - fb;        break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); bl = bb + (fb >> 2); break;
    }
    r  = r  < 0 ? 0 : (r  > 31 ? 31 : r);
    g  = g  < 0 ? 0 : (g  > 31 ? 31 : g);
    bl = bl < 0 ? 0 : (bl > 31 ? 31 : bl);
    return (u16)(r | (g << 5) | (bl << 10));
}

static inline void plot(s32 x, s32 y, u16 c, int blend) {
    u16 *d = &VRAM(x, y);
    u16 bg = *d;
    if (bg & gpu_sw.mask_and) return;
    if (blend >= 0) c = (u16)((c & 0x8000u) | blend15(bg, c, blend));
    *d = (u16)(c | gpu_sw.mask_or);
}

static inline void shade(const shade_t *s, s32 x, s32 y,
                         u32 r, u32 g, u32 b, u32 u, u32 v) {
    const u8 *lut = s->dither ? lut_dither[y & 3][x & 3] : lut_plain;
    if (s->textured) {
        u16 t = texel(s, u, v);
        if (!t) return;
        if (!s->raw)
            t = (u16)((t & 0x8000u)
                    | lut[((t & 0x1Fu) * r) >> 4]
                    | (lut[(((t >> 5) & 0x1Fu) * g) >> 4] << 5)
                    | (lut[(((t >> 10) & 0x1Fu) * b) >> 4] << 10));
        plot(x, y, t, (s->blend >= 0 && (t & 0x8000u)) ? s->blend : -1);
    } else {
        u16 c = (u16)(lut[r] | (lut[g] << 5) | (lut[b] << 10));
        plot(x, y, c, s->blend);
    }
}

#define FBS 12
#define PAD 12

typedef struct { s32 x, y, u, v, r, g, b; } tvert_t;
typedef struct { u32 u, v, r, g, b; } interp_t;
typedef struct { interp_t dx, dy; } grad_t;

static inline void interp_add(interp_t *i, const interp_t *d, u32 n) {
    i->u += d->u * n; i->v += d->v * n;
    i->r += d->r * n; i->g += d->g * n; i->b += d->b * n;
}

static inline u32 grad_term(s64 num, s32 denom) {
    return (u32)(s32)(num * (1 << FBS) / denom) << PAD;
}

static int setup_gradients(grad_t *g, const tvert_t *A, const tvert_t *B,
                           const tvert_t *C, int gouraud, int textured) {
    s32 denom = (B->x - A->x) * (C->y - B->y) - (C->x - B->x) * (B->y - A->y);
    if (!denom) return 0;
    memset(g, 0, sizeof *g);
#define ACROSS(f) ((s64)(B->f - A->f) * (C->y - B->y) - (s64)(C->f - B->f) * (B->y - A->y))
#define DOWN(f)   ((s64)(B->x - A->x) * (C->f - B->f) - (s64)(C->x - B->x) * (B->f - A->f))
    if (gouraud) {
        g->dx.r = grad_term(ACROSS(r), denom); g->dy.r = grad_term(DOWN(r), denom);
        g->dx.g = grad_term(ACROSS(g), denom); g->dy.g = grad_term(DOWN(g), denom);
        g->dx.b = grad_term(ACROSS(b), denom); g->dy.b = grad_term(DOWN(b), denom);
    }
    if (textured) {
        g->dx.u = grad_term(ACROSS(u), denom); g->dy.u = grad_term(DOWN(u), denom);
        g->dx.v = grad_term(ACROSS(v), denom); g->dy.v = grad_term(DOWN(v), denom);
    }
#undef ACROSS
#undef DOWN
    return 1;
}

static inline s64 xfp(s32 x) {
    return (s64)(((u64)(u32)x << 32) + ((1ull << 32) - (1u << 11)));
}

static inline s64 xfp_step(s32 dx, s32 dy) {
    s64 d = (s64)dx * 4294967296ll;
    if (d < 0) d -= dy - 1;
    if (d > 0) d += dy - 1;
    return d / dy;
}

static inline s32 xfp_int(u64 v) { return (s32)((s64)v >> 32); }

static void draw_span(const shade_t *s, s32 y, s32 x_start, s32 x_bound,
                      interp_t ig, const grad_t *g) {
    s32 x_ig = x_start;
    s32 w = x_bound - x_start;
    s32 x = sx11(x_start);
    if (x < gpu_sw.area_x0) {
        s32 d = gpu_sw.area_x0 - x;
        x_ig += d; x += d; w -= d;
    }
    if (x + w > gpu_sw.area_x1 + 1) w = gpu_sw.area_x1 + 1 - x;
    if (w <= 0) return;

    interp_add(&ig, &g->dx, (u32)x_ig);
    interp_add(&ig, &g->dy, (u32)y);
    do {
        shade(s, x, y, ig.r >> 24, ig.g >> 24, ig.b >> 24, ig.u >> 24, ig.v >> 24);
        x++;
        interp_add(&ig, &g->dx, 1);
    } while (--w > 0);
}

static void draw_triangle(const shade_t *s, int gouraud, tvert_t *v) {
    unsigned core;
    {
        unsigned cv;
        if (v[1].x <= v[0].x) cv = (v[2].x <= v[1].x) ? 4u : 2u;
        else if (v[2].x < v[0].x) cv = 4u;
        else cv = 1u;

        tvert_t t;
        if (v[2].y < v[1].y) {
            t = v[2]; v[2] = v[1]; v[1] = t;
            cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
        }
        if (v[1].y < v[0].y) {
            t = v[1]; v[1] = v[0]; v[0] = t;
            cv = ((cv >> 1) & 1u) | ((cv << 1) & 2u) | (cv & 4u);
        }
        if (v[2].y < v[1].y) {
            t = v[2]; v[2] = v[1]; v[1] = t;
            cv = ((cv >> 1) & 2u) | ((cv << 1) & 4u) | (cv & 1u);
        }
        core = cv >> 1;
    }

    if (v[0].y == v[2].y) return;
    if (v[2].y - v[0].y >= 512) return;
    if (abs(v[2].x - v[0].x) >= 1024 || abs(v[2].x - v[1].x) >= 1024 ||
        abs(v[1].x - v[0].x) >= 1024) return;

    grad_t g;
    if (!setup_gradients(&g, &v[0], &v[1], &v[2], gouraud, s->textured)) return;
    prims_drawn++;

    interp_t ig;
    ig.u = ((u32)(v[core].u << FBS) + (1u << (FBS - 1))) << PAD;
    ig.v = ((u32)(v[core].v << FBS) + (1u << (FBS - 1))) << PAD;
    ig.r = ((u32)(v[core].r << FBS) + (1u << (FBS - 1))) << PAD;
    ig.g = ((u32)(v[core].g << FBS) + (1u << (FBS - 1))) << PAD;
    ig.b = ((u32)(v[core].b << FBS) + (1u << (FBS - 1))) << PAD;
    interp_add(&ig, &g.dx, (u32)-v[core].x);
    interp_add(&ig, &g.dy, (u32)-v[core].y);

    s64 base_coord = xfp(v[0].x);
    s64 base_step  = xfp_step(v[2].x - v[0].x, v[2].y - v[0].y);
    s64 upper_step, lower_step;
    int right_facing;
    if (v[1].y == v[0].y) {
        upper_step = 0;
        right_facing = v[1].x > v[0].x;
    } else {
        upper_step = xfp_step(v[1].x - v[0].x, v[1].y - v[0].y);
        right_facing = upper_step > base_step;
    }
    lower_step = (v[2].y == v[1].y)
               ? 0 : xfp_step(v[2].x - v[1].x, v[2].y - v[1].y);

    struct {
        u64 x[2], step[2];
        s32 y, y_bound;
        int upward;
    } part[2];
    unsigned vo = core ? 1u : 0u;
    unsigned vp = core == 2 ? 3u : 0u;

    part[vo].y       = v[0 ^ vo].y;
    part[vo].y_bound = v[1 ^ vo].y;
    part[vo].x[right_facing]     = (u64)xfp(v[0 ^ vo].x);
    part[vo].step[right_facing]  = (u64)upper_step;
    part[vo].x[!right_facing]    = (u64)(base_coord + (s64)(v[vo].y - v[0].y) * base_step);
    part[vo].step[!right_facing] = (u64)base_step;
    part[vo].upward = vo != 0;

    part[vo ^ 1].y       = v[1 ^ vp].y;
    part[vo ^ 1].y_bound = v[2 ^ vp].y;
    part[vo ^ 1].x[right_facing]     = (u64)xfp(v[1 ^ vp].x);
    part[vo ^ 1].step[right_facing]  = (u64)lower_step;
    part[vo ^ 1].x[!right_facing]    =
        (u64)(base_coord + (s64)(v[1 ^ vp].y - v[0].y) * base_step);
    part[vo ^ 1].step[!right_facing] = (u64)base_step;
    part[vo ^ 1].upward = vp != 0;

    for (int i = 0; i < 2; i++) {
        s32 yi = part[i].y, yb = part[i].y_bound;
        u64 lc = part[i].x[0], ls = part[i].step[0];
        u64 rc = part[i].x[1], rs = part[i].step[1];
        if (part[i].upward) {
            while (yi > yb) {
                yi--; lc -= ls; rc -= rs;
                s32 y = sx11(yi);
                if (y < gpu_sw.area_y0) break;
                if (y > gpu_sw.area_y1) continue;
                draw_span(s, yi, xfp_int(lc), xfp_int(rc), ig, &g);
            }
        } else {
            while (yi < yb) {
                s32 y = sx11(yi);
                if (y > gpu_sw.area_y1) break;
                if (y >= gpu_sw.area_y0)
                    draw_span(s, yi, xfp_int(lc), xfp_int(rc), ig, &g);
                yi++; lc += ls; rc += rs;
            }
        }
    }
}

void gpu_sw_poly(const gpu_poly_t *p) {
    if (!ac3_vram) return;
    int textured = p->textured != 0;
    int raw = textured && p->raw;
    shade_t s;
    shade_setup(&s, textured, raw, p->semitrans, p->tpage, p->clut,
                p->twin_mask, p->twin_off);
    int dtd = (gpu_sw.texpage >> 9) & 1;
    s.dither = dtd && (p->gouraud || (textured && !raw));

    tvert_t v[4];
    int n = p->nverts == 4 ? 4 : 3;
    for (int i = 0; i < n; i++) {
        v[i].x = sx11(p->x[i]) + gpu_sw.ofs_x;
        v[i].y = sx11(p->y[i]) + gpu_sw.ofs_y;
        v[i].u = p->u[i]; v[i].v = p->v[i];
        v[i].r = p->r[i]; v[i].g = p->g[i]; v[i].b = p->b[i];
    }
    tvert_t t[3] = { v[0], v[1], v[2] };
    draw_triangle(&s, p->gouraud, t);
    if (n == 4) {
        tvert_t q[3] = { v[1], v[2], v[3] };
        draw_triangle(&s, p->gouraud, q);
    }
    HOOK(poly, p);
}

void gpu_sw_rect(const gpu_rect_t *r) {
    if (!ac3_vram) return;
    HOOK(rect, r);
    int textured = r->textured != 0;
    shade_t s;
    shade_setup(&s, textured, textured && r->raw, r->semitrans, r->tpage,
                r->clut, r->twin_mask, r->twin_off);
    s.dither = 0;

    s32 x = sx11(sx11(r->x) + gpu_sw.ofs_x);
    s32 y = sx11(sx11(r->y) + gpu_sw.ofs_y);
    s32 x_bound = x + (s32)(r->w & 0x3FFu);
    s32 y_bound = y + (s32)(r->h & 0x1FFu);
    u8 u = r->u, v = r->v;
    int u_inc = 1, v_inc = 1;
    if (textured) {
        if (gpu_sw.texpage & 0x1000u) { u_inc = -1; u |= 1; }
        if (gpu_sw.texpage & 0x2000u) v_inc = -1;
    }
    if (x < gpu_sw.area_x0) {
        if (textured) u = (u8)(u + (gpu_sw.area_x0 - x) * u_inc);
        x = gpu_sw.area_x0;
    }
    if (y < gpu_sw.area_y0) {
        if (textured) v = (u8)(v + (gpu_sw.area_y0 - y) * v_inc);
        y = gpu_sw.area_y0;
    }
    if (x_bound > gpu_sw.area_x1 + 1) x_bound = gpu_sw.area_x1 + 1;
    if (y_bound > gpu_sw.area_y1 + 1) y_bound = gpu_sw.area_y1 + 1;
    if (x >= x_bound || y >= y_bound) return;
    prims_drawn++;

    for (; y < y_bound; y++) {
        u8 ur = u;
        for (s32 px = x; px < x_bound; px++) {
            shade(&s, px, y, r->r, r->g, r->b, ur, v);
            ur = (u8)(ur + u_inc);
        }
        v = (u8)(v + v_inc);
    }
}

static inline s64 line_div(s32 delta, s32 k) {
    s64 d = (s64)delta * 4294967296ll;
    if (d < 0) d -= k - 1;
    if (d > 0) d += k - 1;
    return d / k;
}

static void draw_segment(const shade_t *s, int gouraud, int dither,
                         s32 x0, s32 y0, u32 r0, u32 g0, u32 b0,
                         s32 x1, s32 y1, u32 r1, u32 g1, u32 b1) {
    s32 dx = abs(x1 - x0), dy = abs(y1 - y0);
    if (dx >= 1024 || dy >= 512) return;
    if (x0 > x1) {
        s32 t; u32 c;
        t = x0; x0 = x1; x1 = t;  t = y0; y0 = y1; y1 = t;
        c = r0; r0 = r1; r1 = c;  c = g0; g0 = g1; g1 = c;  c = b0; b0 = b1; b1 = c;
    }
    s32 k = dx > dy ? dx : dy;
    s64 sx = 0, sy = 0;
    s32 sr = 0, sg = 0, sb = 0;
    if (k) {
        sx = line_div(x1 - x0, k);
        sy = line_div(y1 - y0, k);
        if (gouraud) {
            sr = (s32)((u32)(s32)(r1 - r0) << 12) / k;
            sg = (s32)((u32)(s32)(g1 - g0) << 12) / k;
            sb = (s32)((u32)(s32)(b1 - b0) << 12) / k;
        }
    }
    u64 cx = ((u64)(s64)x0 << 32) | (1ull << 31);
    u64 cy = ((u64)(s64)y0 << 32) | (1ull << 31);
    cx -= 1024;
    if (sy < 0) cy -= 1024;
    u32 cr = (r0 << 12) | (1u << 11);
    u32 cg = (g0 << 12) | (1u << 11);
    u32 cb = (b0 << 12) | (1u << 11);
    prims_drawn++;

    for (s32 i = 0; i <= k; i++) {
        s32 x = (s32)((cx >> 32) & 2047u);
        s32 y = (s32)((cy >> 32) & 2047u);
        if (x >= gpu_sw.area_x0 && x <= gpu_sw.area_x1 &&
            y >= gpu_sw.area_y0 && y <= gpu_sw.area_y1) {
            u32 r = gouraud ? ((cr >> 12) & 0xFFu) : r0;
            u32 g = gouraud ? ((cg >> 12) & 0xFFu) : g0;
            u32 b = gouraud ? ((cb >> 12) & 0xFFu) : b0;
            const u8 *lut = dither ? lut_dither[y & 3][x & 3] : lut_plain;
            plot(x, y, (u16)(lut[r] | (lut[g] << 5) | (lut[b] << 10)), s->blend);
        }
        cx += (u64)sx; cy += (u64)sy;
        cr += (u32)sr; cg += (u32)sg; cb += (u32)sb;
    }
}

void gpu_sw_line(const gpu_line_t *l) {
    if (!ac3_vram || l->nverts < 2) return;
    HOOK(line, l);
    shade_t s;
    shade_setup(&s, 0, 0, l->semitrans, l->tpage, 0, 0, 0);
    int dither = l->gouraud && ((gpu_sw.texpage >> 9) & 1);
    for (int i = 0; i + 1 < l->nverts; i++) {
        s32 x0 = sx11(l->x[i])     + gpu_sw.ofs_x, y0 = sx11(l->y[i])     + gpu_sw.ofs_y;
        s32 x1 = sx11(l->x[i + 1]) + gpu_sw.ofs_x, y1 = sx11(l->y[i + 1]) + gpu_sw.ofs_y;
        draw_segment(&s, l->gouraud, dither,
                     x0, y0, l->r[i], l->g[i], l->b[i],
                     x1, y1, l->r[i + 1], l->g[i + 1], l->b[i + 1]);
    }
}

void gpu_sw_fill(const PSX_RECT *r, u16 pixel) {
    if (!ac3_vram || !r) return;
    for (s32 y = 0; y < r->h; y++)
        for (s32 x = 0; x < r->w; x++)
            VRAM(r->x + x, r->y + y) = pixel;
    HOOK(fill, r, pixel);
}

void gpu_sw_copy(const PSX_RECT *src, s32 dx, s32 dy) {
    if (!ac3_vram || !src) return;
    HOOK(copy, src, dx, dy);
    for (s32 y = 0; y < src->h; y++) {
        for (s32 x = 0; x < src->w; x += 128) {
            s32 n = src->w - x < 128 ? src->w - x : 128;
            u16 buf[128];
            for (s32 i = 0; i < n; i++)
                buf[i] = VRAM(src->x + x + i, src->y + y);
            for (s32 i = 0; i < n; i++) {
                u16 *d = &VRAM(dx + x + i, dy + y);
                if (!(*d & gpu_sw.mask_and)) *d = (u16)(buf[i] | gpu_sw.mask_or);
            }
        }
    }
}

static struct { s32 x, y, w, h, cx, cy; u32 left; } up;

static void upload_done(void) {
#ifdef AC3_ENHANCED
    PSX_RECT r = { (s16)up.x, (s16)up.y, (s16)up.w, (s16)up.h };
    HOOK(upload, &r);
#endif
}

void gpu_sw_load_begin(const PSX_RECT *r) {
    up.x = r->x; up.y = r->y; up.w = r->w; up.h = r->h;
    up.cx = up.cy = 0;
    up.left = (r->w > 0 && r->h > 0) ? (u32)r->w * (u32)r->h : 0u;
}

static inline void load_pixel(u16 p) {
    if (!up.left) return;
    u16 *d = &VRAM(up.x + up.cx, up.y + up.cy);
    if (!(*d & gpu_sw.mask_and)) *d = (u16)(p | gpu_sw.mask_or);
    if (++up.cx == up.w) { up.cx = 0; up.cy++; }
    if (--up.left == 0) upload_done();
}

void gpu_sw_load_word(u32 w) {
    if (!ac3_vram) return;
    load_pixel((u16)(w & 0xFFFFu));
    load_pixel((u16)(w >> 16));
}

u32 gpu_sw_load_words_left(void) { return (up.left + 1u) / 2u; }
void gpu_sw_load_cancel(void) {
    if (up.left && up.left < (u32)up.w * (u32)up.h) upload_done();
    memset(&up, 0, sizeof up);
}

void gpu_sw_store(const PSX_RECT *r, u16 *dst, u32 n) {
    if (!ac3_vram || !r || !dst || r->w <= 0) return;
    for (u32 i = 0; i < n; i++)
        dst[i] = VRAM(r->x + (s32)(i % (u32)r->w), r->y + (s32)(i / (u32)r->w));
}

void gpu_sw_display_enable(int on) { disp.enabled = on != 0; }

void gpu_sw_display_start(s32 x, s32 y) {
    disp.x = x & 0x3FF;
    disp.y = y & 0x1FF;
}

void gpu_sw_display_mode(s32 w, s32 h, int rgb24) {
    disp.w = w;
    disp.h = h;
    disp.rgb24 = rgb24 != 0;
}

void ac3_gpu_sw_machine_save(FILE *f) {
    ac3_ms_tag(f, "GPUS");
    ac3_ms_u32(f, gpu_sw.texpage);
    ac3_ms_u32(f, gpu_sw.texwin);
    ac3_ms_u32(f, (u32)gpu_sw.area_x0);
    ac3_ms_u32(f, (u32)gpu_sw.area_y0);
    ac3_ms_u32(f, (u32)gpu_sw.area_x1);
    ac3_ms_u32(f, (u32)gpu_sw.area_y1);
    ac3_ms_u32(f, (u32)gpu_sw.ofs_x);
    ac3_ms_u32(f, (u32)gpu_sw.ofs_y);
    ac3_ms_u32(f, gpu_sw.mask_or);
    ac3_ms_u32(f, gpu_sw.mask_and);
    ac3_ms_u32(f, (u32)disp.enabled);
    ac3_ms_u32(f, (u32)disp.x);
    ac3_ms_u32(f, (u32)disp.y);
    ac3_ms_u32(f, (u32)disp.w);
    ac3_ms_u32(f, (u32)disp.h);
    ac3_ms_u32(f, (u32)disp.rgb24);
    ac3_ms_u32(f, up.left);
    ac3_ms_tag(f, "VRAM");
    if (ac3_vram) ac3_ms_bytes(f, ac3_vram, GPU_VRAM_W * GPU_VRAM_H * 2u);
    else ac3_ms_bytes(f, NULL, 0);
}
