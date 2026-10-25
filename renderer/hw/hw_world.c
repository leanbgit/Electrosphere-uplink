#include "hw_world.h"
#include "gpu_sw.h"
#include "pgxp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_GP 0x800BCB74u

hw_map_t hw_map;
int hw_world_verbose;

static int ram_ok(u32 a, u32 len) {
    return (a >> 24) == 0x80u && (a & 0x1FFFFFu) + len <= AC3_RAM_SIZE;
}

int hw_world_camera(hw_scene_t *s) {
    s->valid = 0;
    u32 world = MEM_R32(GAME_GP + 736);
    if (!ram_ok(world, 720)) return 0;
    u32 view = MEM_R32(world + 40);
    if (!ram_ok(view, 64)) return 0;
    for (int i = 0; i < 9; i++) s->rot[i] = (float)MEM_R16S(world + 76 + 2u * (u32)i) / 4096.0f;
    for (int r = 0; r < 3; r++) {
        float l = s->rot[3 * r] * s->rot[3 * r] + s->rot[3 * r + 1] * s->rot[3 * r + 1] +
                  s->rot[3 * r + 2] * s->rot[3 * r + 2];
        if (l < 0.8f || l > 1.2f) return 0;
    }
    for (int i = 0; i < 3; i++) s->cam[i] = (float)(s32)MEM_R32(view + 4 + 4u * (u32)i) / 4096.0f;
    s32 H = MEM_R16S(view + 36);
    if (H < 64 || H > 4096) return 0;
    s->H = (float)H;
    s->ws = ac3_ws_factor;

    float L[3], len = 0;
    for (int i = 0; i < 3; i++) {
        L[i] = (float)MEM_R16S(world + 108 + 2u * (u32)i) / 4096.0f;
        len += L[i] * L[i];
    }
    if (len < 0.25f) { L[0] = 0.5f; L[1] = -0.6f; L[2] = 0.6f; len = 0.97f; }
    len = sqrtf(len);
    for (int i = 0; i < 3; i++) s->sun[i] = L[i] / len;
    float c[3] = { (float)MEM_R16S(world + 140), (float)MEM_R16S(world + 146),
                   (float)MEM_R16S(world + 152) };
    float m = c[0] > c[1] ? c[0] : c[1];
    if (c[2] > m) m = c[2];
    for (int i = 0; i < 3; i++) s->sun_col[i] = m > 64.0f ? c[i] / m : 1.0f;
    s->sea_y = hw_map.valid && hw_map.has_sea ? hw_map.sea_y : -1024.0f;
    s->valid = 1;
    return 1;
}

static float sky_acc[HW_SKY_BINS][4];

static void view_dir_world(const hw_scene_t *s, float sx, float sy, float w[3]) {
    float d[3] = { sx / (s->H * s->ws), sy / s->H, 1.0f };
    for (int i = 0; i < 3; i++)
        w[i] = s->rot[i] * d[0] + s->rot[3 + i] * d[1] + s->rot[6 + i] * d[2];
}

void hw_world_sky_begin(hw_scene_t *s) {
    memset(sky_acc, 0, sizeof sky_acc);
    s->sky_samples = 0;
}

void hw_world_sky_add(hw_scene_t *s, float sx, float sy, u8 r, u8 g, u8 b) {
    if (!s->valid) return;
    float w[3];
    view_dir_world(s, sx, sy, w);
    float e = atan2f(-w[1], sqrtf(w[0] * w[0] + w[2] * w[2]));
    float f = (e / 3.14159265f + 0.5f) * (float)(HW_SKY_BINS - 1);
    if (f < 0) f = 0;
    if (f > HW_SKY_BINS - 1) f = (float)(HW_SKY_BINS - 1);
    int i = (int)f;
    float t = f - (float)i;
    float col[3] = { r / 255.0f, g / 255.0f, b / 255.0f };
    for (int k = 0; k < 2; k++) {
        int j = i + k;
        if (j >= HW_SKY_BINS) break;
        float wt = k ? t : 1.0f - t;
        for (int c = 0; c < 3; c++) sky_acc[j][c] += col[c] * wt;
        sky_acc[j][3] += wt;
    }
    s->sky_samples++;
}

void hw_world_sky_finish(hw_scene_t *s) {
    int have[HW_SKY_BINS], any = 0;
    for (int i = 0; i < HW_SKY_BINS; i++) {
        have[i] = sky_acc[i][3] > 0.05f;
        if (have[i]) {
            any = 1;
            for (int c = 0; c < 3; c++) s->sky[i][c] = sky_acc[i][c] / sky_acc[i][3];
        }
    }
    if (!any) {
        for (int i = 0; i < HW_SKY_BINS; i++)
            for (int c = 0; c < 3; c++) s->sky[i][c] = s->fog_col[c];
        return;
    }
    for (int i = 0; i < HW_SKY_BINS; i++) {
        if (have[i]) continue;
        int lo = i - 1, hi = i + 1;
        while (lo >= 0 && !have[lo]) lo--;
        while (hi < HW_SKY_BINS && !have[hi]) hi++;
        for (int c = 0; c < 3; c++) {
            if (lo < 0) s->sky[i][c] = s->sky[hi][c];
            else if (hi >= HW_SKY_BINS) s->sky[i][c] = s->sky[lo][c];
            else {
                float t = (float)(i - lo) / (float)(hi - lo);
                s->sky[i][c] = s->sky[lo][c] * (1 - t) + s->sky[hi][c] * t;
            }
        }
    }
}

void hw_world_sky_at(const hw_scene_t *s, float e, float out[3]) {
    float f = (e / 3.14159265f + 0.5f) * (float)(HW_SKY_BINS - 1);
    if (f < 0) f = 0;
    if (f > HW_SKY_BINS - 1.001f) f = HW_SKY_BINS - 1.001f;
    int i = (int)f;
    float t = f - (float)i;
    for (int c = 0; c < 3; c++) out[c] = s->sky[i][c] * (1 - t) + s->sky[i + 1][c] * t;
}

static u16 texel(u16 tpage, u16 clut, u32 u, u32 v) {
    u32 px = (tpage & 15u) * 64u, py = ((tpage >> 4) & 1u) * 256u;
    u32 depth = (tpage >> 7) & 3u;
    u32 cx = (clut & 63u) * 16u, cy = (clut >> 6) & 511u;
    u &= 255u; v &= 255u;
    if (depth == 0) {
        u16 w = *vram_at((int)((px + (u >> 2)) & 1023u), (int)((py + v) & 511u));
        return *vram_at((int)((cx + ((w >> ((u & 3u) * 4u)) & 15u)) & 1023u), (int)cy);
    }
    if (depth == 1) {
        u16 w = *vram_at((int)((px + (u >> 1)) & 1023u), (int)((py + v) & 511u));
        return *vram_at((int)((cx + ((w >> ((u & 1u) * 8u)) & 255u)) & 1023u), (int)cy);
    }
    return *vram_at((int)((px + u) & 1023u), (int)((py + v) & 511u));
}

typedef struct { float x, y, z; } v3_t;

typedef struct {
    u8  idx[4];
    float u[4], v[4];
    u16 clut, tpage;
    float r[4], g[4], b[4];
    u16 tex;
} tpoly_t;

#define MAX_TEX 1024
typedef struct { u32 key; double w, r, g, b, l, l2; u8 cls; u32 n, nsea; } tex_stat_t;
static tex_stat_t texs[MAX_TEX];
static int ntex;
static u16 sample_tex[HW_ATLAS_W * HW_ATLAS_H];
static double tile_weight;

static u16 tex_index(u16 tpage, u16 clut) {
    u32 key = ((u32)tpage << 16) | clut;
    for (int i = 1; i < ntex; i++) if (texs[i].key == key) return (u16)i;
    if (ntex >= MAX_TEX) return 0;
    memset(&texs[ntex], 0, sizeof texs[ntex]);
    texs[ntex].key = key;
    return (u16)ntex++;
}

#define MAX_TV 512

static int tile_verts(u32 vb, v3_t *out) {
    if (!ram_ok(vb, 8)) return 0;
    s32 n = (s32)MEM_R32(vb);
    if (n <= 0 || n > MAX_TV) return 0;
    if (!ram_ok(vb, 4u + 20u * (u32)((n + 2) / 3))) return 0;
    for (int i = 0; i < n; i++) {
        u32 g = vb + 4u + 20u * (u32)(i / 3);
        int k = i % 3;
        u32 xy = MEM_R32(g + 4u * (u32)k);
        u32 zw = k == 0 ? MEM_R32(g + 12) & 0xFFFFu : k == 1 ? MEM_R32(g + 12) >> 16
                                                             : MEM_R32(g + 16) & 0xFFFFu;
        out[i].x = (float)(s16)(xy & 0xFFFFu);
        out[i].y = (float)(s16)(xy >> 16);
        out[i].z = (float)(s16)zw;
    }
    return n;
}

static void read_poly(u32 e, int gouraud, tpoly_t *p) {
    u32 w4 = MEM_R32(e + 16), w5 = MEM_R32(e + 20), w6 = MEM_R32(e + 24);
    u32 w7 = MEM_R32(e + 28);
    for (int k = 0; k < 4; k++) p->idx[k] = (u8)(w4 >> (8 * k));
    p->u[0] = (float)(w5 & 255u);         p->v[0] = (float)((w5 >> 8) & 255u);
    p->u[1] = (float)(w6 & 255u);         p->v[1] = (float)((w6 >> 8) & 255u);
    p->u[2] = (float)(w7 & 255u);         p->v[2] = (float)((w7 >> 8) & 255u);
    p->u[3] = (float)((w7 >> 16) & 255u); p->v[3] = (float)(w7 >> 24);
    p->clut = (u16)(w5 >> 16);
    p->tpage = (u16)(w6 >> 16);
    for (int k = 0; k < 4; k++) {
        u32 c = MEM_R32(e + 32 + (gouraud ? 4u * (u32)k : 0u));
        p->r[k] = (float)(c & 255u);
        p->g[k] = (float)((c >> 8) & 255u);
        p->b[k] = (float)((c >> 16) & 255u);
    }
}

typedef struct {
    float h[HW_TILE_RES * HW_TILE_RES];
    float c[HW_TILE_RES * HW_TILE_RES][3];
    int   has[HW_TILE_RES * HW_TILE_RES];
    u16   tex[HW_TILE_RES * HW_TILE_RES];
} tile_buf_t;

static float edge2(float ax, float az, float bx, float bz, float px, float pz) {
    return (bx - ax) * (pz - az) - (bz - az) * (px - ax);
}

static void raster_tri(tile_buf_t *tb, const v3_t *v, const tpoly_t *p, int a, int b, int c) {
    const v3_t *A = &v[p->idx[a]], *B = &v[p->idx[b]], *C = &v[p->idx[c]];
    float area = edge2(A->x, A->z, B->x, B->z, C->x, C->z);
    if (fabsf(area) < 64.0f) return;
    float step = HW_TILE_SIZE / HW_TILE_RES;
    float minx = fminf(A->x, fminf(B->x, C->x)), maxx = fmaxf(A->x, fmaxf(B->x, C->x));
    float minz = fminf(A->z, fminf(B->z, C->z)), maxz = fmaxf(A->z, fmaxf(B->z, C->z));
    int i0 = (int)floorf((minx + 1024.0f) / step - 0.5f), i1 = (int)ceilf((maxx + 1024.0f) / step - 0.5f);
    int j0 = (int)floorf((minz + 1024.0f) / step - 0.5f), j1 = (int)ceilf((maxz + 1024.0f) / step - 0.5f);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > HW_TILE_RES - 1) i1 = HW_TILE_RES - 1;
    if (j1 > HW_TILE_RES - 1) j1 = HW_TILE_RES - 1;
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            float px = -1024.0f + (i + 0.5f) * step, pz = -1024.0f + (j + 0.5f) * step;
            float w0 = edge2(B->x, B->z, C->x, C->z, px, pz) / area;
            float w1 = edge2(C->x, C->z, A->x, A->z, px, pz) / area;
            float w2 = 1.0f - w0 - w1;
            const float eps = -0.02f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            float y = w0 * A->y + w1 * B->y + w2 * C->y;
            int s = j * HW_TILE_RES + i;
            if (tb->has[s] && y >= tb->h[s]) continue;
            float acc[3] = { 0, 0, 0 };
            int n = 0;
            for (int q = 0; q < 4; q++) {
                float ox = ((q & 1) ? 0.25f : -0.25f) * step, oz = ((q & 2) ? 0.25f : -0.25f) * step;
                float b0 = edge2(B->x, B->z, C->x, C->z, px + ox, pz + oz) / area;
                float b1 = edge2(C->x, C->z, A->x, A->z, px + ox, pz + oz) / area;
                float b2 = 1.0f - b0 - b1;
                if (b0 < -0.1f || b1 < -0.1f || b2 < -0.1f) { b0 = w0; b1 = w1; b2 = w2; }
                float u = b0 * p->u[a] + b1 * p->u[b] + b2 * p->u[c];
                float vv = b0 * p->v[a] + b1 * p->v[b] + b2 * p->v[c];
                u16 t = texel(p->tpage, p->clut, (u32)(int)u, (u32)(int)vv);
                if (!t) continue;
                float mr = b0 * p->r[a] + b1 * p->r[b] + b2 * p->r[c];
                float mg = b0 * p->g[a] + b1 * p->g[b] + b2 * p->g[c];
                float mb = b0 * p->b[a] + b1 * p->b[b] + b2 * p->b[c];
                acc[0] += fminf((float)(t & 31u) * 8.0f * mr / 128.0f, 255.0f);
                acc[1] += fminf((float)((t >> 5) & 31u) * 8.0f * mg / 128.0f, 255.0f);
                acc[2] += fminf((float)((t >> 10) & 31u) * 8.0f * mb / 128.0f, 255.0f);
                n++;
            }
            if (!n) continue;
            tb->h[s] = y;
            tb->has[s] = 1;
            tb->tex[s] = p->tex;
            for (int k = 0; k < 3; k++) tb->c[s][k] = acc[k] / (float)n;
        }
}

static u32 poly_average(const tpoly_t *p) {
    float umin = p->u[0], umax = p->u[0], vmin = p->v[0], vmax = p->v[0];
    for (int k = 1; k < 4; k++) {
        umin = fminf(umin, p->u[k]); umax = fmaxf(umax, p->u[k]);
        vmin = fminf(vmin, p->v[k]); vmax = fmaxf(vmax, p->v[k]);
    }
    float mr = (p->r[0] + p->r[1] + p->r[2] + p->r[3]) * 0.25f;
    float mg = (p->g[0] + p->g[1] + p->g[2] + p->g[3]) * 0.25f;
    float mb = (p->b[0] + p->b[1] + p->b[2] + p->b[3]) * 0.25f;
    float acc[3] = { 0, 0, 0 };
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            u32 u = (u32)(int)(umin + (umax - umin) * (i + 0.5f) / 4.0f);
            u32 vv = (u32)(int)(vmin + (vmax - vmin) * (j + 0.5f) / 4.0f);
            u16 t = texel(p->tpage, p->clut, u, vv);
            if (!t) continue;
            acc[0] += fminf((float)(t & 31u) * 8.0f * mr / 128.0f, 255.0f);
            acc[1] += fminf((float)((t >> 5) & 31u) * 8.0f * mg / 128.0f, 255.0f);
            acc[2] += fminf((float)((t >> 10) & 31u) * 8.0f * mb / 128.0f, 255.0f);
            n++;
        }
    if (!n) return 0;
    return (u32)(acc[0] / n) | ((u32)(acc[1] / n) << 8) | ((u32)(acc[2] / n) << 16) | 0xFF000000u;
}

static void mesh_push(const v3_t *v, const tpoly_t *p, int a, int b, int c, u32 avg) {
    if (hw_map.mesh_n + 3 > hw_map.mesh_cap) {
        u32 cap = hw_map.mesh_cap ? hw_map.mesh_cap * 2 : 65536;
        hw_tile_vtx_t *m = realloc(hw_map.mesh, (size_t)cap * sizeof *m);
        if (!m) return;
        hw_map.mesh = m;
        hw_map.mesh_cap = cap;
    }
    const int ks[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        int k = ks[i];
        hw_tile_vtx_t *o = &hw_map.mesh[hw_map.mesh_n++];
        const v3_t *q = &v[p->idx[k]];
        o->x = q->x; o->y = q->y; o->z = q->z;
        o->u = p->u[k]; o->v = p->v[k];
        o->rgba = (u32)p->r[k] | ((u32)p->g[k] << 8) | ((u32)p->b[k] << 16) | 0xFF000000u;
        o->tex = ((u32)p->tpage << 16) | p->clut;
        o->avg = avg;
    }
}

static void raster_list(tile_buf_t *tb, const v3_t *v, int nv, u32 list, int gouraud) {
    if (!list || !ram_ok(list, 4)) return;
    s32 n = (s32)MEM_R32(list);
    u32 stride = gouraud ? 48u : 36u;
    if (n <= 0 || n > 512 || !ram_ok(list, 4u + stride * (u32)n)) return;
    for (s32 i = 0; i < n; i++) {
        tpoly_t p;
        read_poly(list + 4u + stride * (u32)i, gouraud, &p);
        p.tex = tex_index(p.tpage, p.clut);
        int bad = 0;
        for (int k = 0; k < 4; k++) if (p.idx[k] >= nv) bad = 1;
        if (bad) continue;
        raster_tri(tb, v, &p, 0, 1, 2);
        if (p.idx[3] != p.idx[2] && p.idx[3] != p.idx[1]) raster_tri(tb, v, &p, 1, 2, 3);
        u32 avg = poly_average(&p);
        if (!avg) continue;
        mesh_push(v, &p, 0, 1, 2, avg);
        if (p.idx[3] != p.idx[2] && p.idx[3] != p.idx[1]) mesh_push(v, &p, 1, 2, 3, avg);
    }
}

static void build_tile(int t, u32 tp) {
    static tile_buf_t tb;
    static v3_t v[MAX_TV];
    memset(&tb, 0, sizeof tb);
    int ax = (t % HW_ATLAS_COLS) * HW_TILE_RES, ay = (t / HW_ATLAS_COLS) * HW_TILE_RES;
    float base = -1024.0f;
    int nv = 0;
    hw_map.mesh_first[t] = hw_map.mesh_n;
    if (ram_ok(tp, 20)) {
        u32 vb = MEM_R32(tp + 4);
        nv = tile_verts(vb, v);
        if (ram_ok(vb - 4, 4)) base = (float)MEM_R16S(vb - 4);
        if (nv) {
            raster_list(&tb, v, nv, MEM_R32(tp + 12), 0);
            raster_list(&tb, v, nv, MEM_R32(tp + 16), 1);
        }
    }
    hw_map.mesh_count[t] = hw_map.mesh_n - hw_map.mesh_first[t];
    int any = 0;
    for (int s = 0; s < HW_TILE_RES * HW_TILE_RES; s++) any |= tb.has[s];
    for (int pass = 0; any && pass < HW_TILE_RES; pass++) {
        int missing = 0;
        static int nh[HW_TILE_RES * HW_TILE_RES];
        memcpy(nh, tb.has, sizeof nh);
        for (int j = 0; j < HW_TILE_RES; j++)
            for (int i = 0; i < HW_TILE_RES; i++) {
                int s = j * HW_TILE_RES + i;
                if (tb.has[s]) continue;
                float h = 0, c[3] = { 0, 0, 0 };
                int n = 0;
                for (int dj = -1; dj <= 1; dj++)
                    for (int di = -1; di <= 1; di++) {
                        int x = i + di, y = j + dj;
                        if (x < 0 || y < 0 || x >= HW_TILE_RES || y >= HW_TILE_RES) continue;
                        int q = y * HW_TILE_RES + x;
                        if (!tb.has[q]) continue;
                        h += tb.h[q];
                        for (int k = 0; k < 3; k++) c[k] += tb.c[q][k];
                        n++;
                    }
                if (!n) { missing = 1; continue; }
                for (int dj = -1; dj <= 1 && !tb.tex[s]; dj++)
                    for (int di = -1; di <= 1; di++) {
                        int x = i + di, y = j + dj;
                        if (x < 0 || y < 0 || x >= HW_TILE_RES || y >= HW_TILE_RES) continue;
                        if (tb.has[y * HW_TILE_RES + x]) { tb.tex[s] = tb.tex[y * HW_TILE_RES + x]; break; }
                    }
                tb.h[s] = h / (float)n;
                for (int k = 0; k < 3; k++) tb.c[s][k] = c[k] / (float)n;
                nh[s] = 1;
            }
        memcpy(tb.has, nh, sizeof nh);
        if (!missing) break;
    }
    float top = 1e9f;
    for (int j = 0; j < HW_TILE_RES; j++)
        for (int i = 0; i < HW_TILE_RES; i++) {
            int s = j * HW_TILE_RES + i;
            float h = tb.has[s] ? tb.h[s] : base;
            u32 r = tb.has[s] ? (u32)tb.c[s][0] : 96u;
            u32 g = tb.has[s] ? (u32)tb.c[s][1] : 96u;
            u32 b = tb.has[s] ? (u32)tb.c[s][2] : 96u;
            size_t o = (size_t)(ay + j) * HW_ATLAS_W + (size_t)(ax + i);
            hw_map.height[o] = h;
            hw_map.color[o] = r | (g << 8) | (b << 16);
            sample_tex[o] = tb.has[s] ? tb.tex[s] : 0;
            if (tb.has[s] && tb.tex[s] && tile_weight > 0) {
                tex_stat_t *ts = &texs[tb.tex[s]];
                double l = 0.3 * r + 0.59 * g + 0.11 * b;
                ts->w += tile_weight;
                ts->r += tile_weight * r; ts->g += tile_weight * g; ts->b += tile_weight * b;
                ts->l += tile_weight * l; ts->l2 += tile_weight * l * l;
            }
            if (h < top) top = h;
        }
    hw_map.tile_top[t] = top;
}

static u32 map_signature(u32 grid, u32 table) {
    u32 h = grid * 2654435761u ^ table;
    for (u32 i = 0; i < 65536u; i += 61u) h = h * 31u + MEM_R16U(grid + 2u * i);
    u32 n = MEM_R32(table);
    h = h * 31u + n;
    for (u32 i = 1; i <= n && i < 512u; i += 5u) h = h * 31u + MEM_R32(table + 4u * i);
    return h ? h : 1u;
}

static int luma(u32 c) {
    return (int)((c & 255u) * 3u + ((c >> 8) & 255u) * 6u + ((c >> 16) & 255u)) / 10;
}

enum { TEX_OTHER, TEX_WATER, TEX_FOREST, TEX_BUILT };

typedef struct { float lo, hi, rgb[3]; int tex, holes; } tile_stat_t;

static void tile_stat(int tile, tile_stat_t *s) {
    static u32 votes[MAX_TEX];
    int ax = (tile % HW_ATLAS_COLS) * HW_TILE_RES, ay = (tile / HW_ATLAS_COLS) * HW_TILE_RES;
    memset(votes, 0, sizeof votes);
    s->lo = 1e9f; s->hi = -1e9f;
    s->rgb[0] = s->rgb[1] = s->rgb[2] = 0.0f;
    for (int j = 0; j < HW_TILE_RES; j++)
        for (int i = 0; i < HW_TILE_RES; i++) {
            size_t o = (size_t)(ay + j) * HW_ATLAS_W + (size_t)(ax + i);
            float h = hw_map.height[o];
            s->lo = fminf(s->lo, h); s->hi = fmaxf(s->hi, h);
            for (int k = 0; k < 3; k++) s->rgb[k] += (float)((hw_map.color[o] >> (8 * k)) & 255u);
            votes[sample_tex[o]]++;
        }
    for (int k = 0; k < 3; k++) s->rgb[k] /= (float)(HW_TILE_RES * HW_TILE_RES);
    s->tex = 0;
    for (int i = 1; i < ntex; i++) if (votes[i] > votes[s->tex]) s->tex = i;
    s->holes = votes[0] != 0;
}

static void classify(const u32 *count) {
    tile_stat_t s;
    double land[3] = { 0, 0, 0 }, land_n = 0;
    for (int tile = 1; tile < 512; tile++) {
        if (!count[tile]) continue;
        tile_stat(tile, &s);
        if (s.hi - s.lo < 1.0f) continue;
        for (int k = 0; k < 3; k++) land[k] += (double)s.rgb[k] * count[tile];
        land_n += count[tile];
    }
    double land_br = land_n > 0 ? (land[2] / land_n + 1.0) / (land[0] / land_n + 1.0) : 1.0;
    int sea_tex = 0;
    u32 best_count = 0, wc = 0;
    float sea_y = -1024.0f;
    for (int tile = 1; tile < 512; tile++) {
        if (count[tile] < HW_MAP_N) continue;
        tile_stat(tile, &s);
        float lo = s.lo, *acc = s.rgb;
        int tex = s.tex;
        u32 color = (u32)acc[0] | ((u32)acc[1] << 8) | ((u32)acc[2] << 16);
        float mean = (acc[0] + acc[1] + acc[2]) / 3.0f;
        int blue = acc[1] + acc[2] - 2.0f * acc[0] >= 0.25f * (mean + 4.0f) && acc[2] >= acc[0] - 2.0f;
        int bluer = (acc[2] + 1.0) / (acc[0] + 1.0) >= 1.4 * land_br;
        if (s.holes || !tex || s.hi - lo >= 1.0f || !(blue || bluer) || luma(color) >= 170) continue;
        const tex_stat_t *t = &texs[tex];
        double l = t->w > 0 ? t->l / t->w : 0;
        double var = t->w > 0 ? t->l2 / t->w - l * l : 1e9;
        if (var > 64.0) continue;
        int lower = sea_tex && lo > sea_y + 1.0f, level = !sea_tex || fabsf(lo - sea_y) <= 1.0f;
        if (!lower && !(level && count[tile] > best_count)) continue;
        best_count = count[tile]; sea_tex = tex; sea_y = lo; wc = color;
    }
    hw_map.has_sea = sea_tex != 0;
    hw_map.sea_y = sea_y;
    hw_map.water_rgb = wc;
    hw_map.sea_tex = hw_map.has_sea ? texs[sea_tex].key : 0;
    for (int i = 0; i < ntex; i++) texs[i].n = texs[i].nsea = 0;
    for (size_t o = 0; o < (size_t)HW_ATLAS_W * HW_ATLAS_H; o++) {
        tex_stat_t *t = &texs[sample_tex[o]];
        t->n++;
        if (hw_map.has_sea && fabsf(hw_map.height[o] - hw_map.sea_y) < 1.5f) t->nsea++;
    }

    int nf = 0, nb = 0;
    for (int i = 1; i < ntex; i++) {
        tex_stat_t *t = &texs[i];
        t->cls = TEX_OTHER;
        if (t->w <= 0) continue;
        double r = t->r / t->w, g = t->g / t->w, b = t->b / t->w, l = t->l / t->w;
        double var = t->l2 / t->w - l * l;
        double sd = var > 0 ? sqrt(var) : 0;
        const tex_stat_t *st = &texs[sea_tex];
        double sr = st->w > 0 ? st->r / st->w : 0, sg = st->w > 0 ? st->g / st->w : 0;
        double sb = st->w > 0 ? st->b / st->w : 0;
        double at_sea = t->n ? (double)t->nsea / t->n : 0.0;
        int same_pal = (t->key & 0xFFFFu) == (st->key & 0xFFFFu) &&
                       ((t->key >> 16) & 0x180u) == ((st->key >> 16) & 0x180u);
        int looks = fabs(r - sr) <= 5.0 && fabs(g - sg) <= 5.0 && fabs(b - sb) <= 5.0 && sd < 4.0;
        if (hw_map.has_sea && (i == sea_tex || ((same_pal || looks) && at_sea > 0.9)))
            t->cls = TEX_WATER;
        else if (g >= r + 1.5 && (g - b) >= 0.3 * l && l < 62.0 && sd < 16.0) { t->cls = TEX_FOREST; nf++; }
        else if (l >= 45.0 || sd >= 16.0) { t->cls = TEX_BUILT; nb++; }
    }
    hw_map.nsea_keys = 0;
    for (int i = 1; i < ntex && hw_map.nsea_keys < HW_SEA_KEYS; i++)
        if (texs[i].cls == TEX_WATER) hw_map.sea_keys[hw_map.nsea_keys++] = texs[i].key;
    for (u32 i = 0; hw_map.has_sea && i + 2 < hw_map.mesh_n; i += 3) {
        hw_tile_vtx_t *m = &hw_map.mesh[i];
        int sea = 1;
        for (int k = 0; k < 3; k++)
            if (!hw_world_sea_texture((u16)(m[k].tex >> 16), (u16)m[k].tex) ||
                fabsf(m[k].y - hw_map.sea_y) >= 1.5f) sea = 0;
        if (sea)
            for (int k = 0; k < 3; k++) m[k].avg &= 0x00FFFFFFu;
    }
    hw_map.forest_samples = hw_map.water_samples = 0;
    memset(hw_map.tile_water, 1, sizeof hw_map.tile_water);
    for (size_t o = 0; o < (size_t)HW_ATLAS_W * HW_ATLAS_H; o++) {
        u32 c = hw_map.color[o] & 0xFFFFFFu;
        u32 f = 0;
        switch (texs[sample_tex[o]].cls) {
        case TEX_WATER:
            if (fabsf(hw_map.height[o] - hw_map.sea_y) < 1.5f) { f = HW_CELL_WATER; hw_map.water_samples++; }
            break;
        case TEX_FOREST: f = HW_CELL_FOREST; hw_map.forest_samples++; break;
        case TEX_BUILT:  f = HW_CELL_BUILT; break;
        default: break;
        }
        hw_map.color[o] = c | (f << 24);
        if (!(f & HW_CELL_WATER)) {
            int tx = (int)(o % HW_ATLAS_W) / HW_TILE_RES, ty = (int)(o / HW_ATLAS_W) / HW_TILE_RES;
            hw_map.tile_water[ty * HW_ATLAS_COLS + tx] = 0;
        }
    }
    if (hw_world_verbose) {
        static int order[MAX_TEX];
        for (int i = 0; i < ntex; i++) order[i] = i;
        for (int i = 1; i < ntex; i++)
            for (int j = i; j > 1 && texs[order[j]].w > texs[order[j - 1]].w; j--) {
                int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            }
        for (int k = 1; k < ntex && k < 60; k++) {
            tex_stat_t *t = &texs[order[k]];
            if (t->w <= 0) continue;
            double l = t->l / t->w, var = t->l2 / t->w - l * l;
            printf("  texture %04X/%04X weight %8.0f  rgb %3.0f %3.0f %3.0f  sd %5.1f  %s\n",
                   t->key >> 16, t->key & 0xFFFFu, t->w, t->r / t->w, t->g / t->w, t->b / t->w,
                   var > 0 ? sqrt(var) : 0.0,
                   t->cls == TEX_WATER ? "water" : t->cls == TEX_FOREST ? "forest" :
                   t->cls == TEX_BUILT ? "built" : "");
        }
    }
    printf("world: %d terrain textures: %d forest, %d built; sea texture %04X/%04X",
           ntex - 1, nf, nb, sea_tex ? texs[sea_tex].key >> 16 : 0,
           sea_tex ? texs[sea_tex].key & 0xFFFFu : 0);
    for (int k = 1; k < hw_map.nsea_keys; k++)
        printf(" + %04X/%04X", hw_map.sea_keys[k] >> 16, hw_map.sea_keys[k] & 0xFFFFu);
    printf("\n");
}

int hw_world_map_update(void) {
    static int rebuild_after = -1;
    u32 world = MEM_R32(GAME_GP + 736);
    if (!ram_ok(world, 720)) return 0;
    u32 grid = MEM_R32(world + 584), table = MEM_R32(world + 588);
    if (!ram_ok(grid, 2u * HW_MAP_N * HW_MAP_N) || !ram_ok(table, 8)) { hw_map.valid = 0; return 0; }
    u32 n = MEM_R32(table);
    if (n < 1 || n > 511 || !ram_ok(table, 4u * (n + 1))) { hw_map.valid = 0; return 0; }
    u32 sig = map_signature(grid, table);
    if (hw_map.valid && sig == hw_map.signature) {
        if (rebuild_after < 0) return 0;
        if (--rebuild_after > 0) return 0;
    } else {
        rebuild_after = 60;
    }
    if (rebuild_after == 0) rebuild_after = -1;

    static u32 count[512];
    memset(count, 0, sizeof count);
    for (int i = 0; i < HW_MAP_N * HW_MAP_N; i++) {
        u16 t = (u16)(MEM_R16U(grid + 2u * (u32)i) & 0x1FFu);
        hw_map.grid[i] = t <= n ? t : 0;
        count[hw_map.grid[i]]++;
    }
    ntex = 1;
    hw_map.mesh_n = 0;
    memset(&texs[0], 0, sizeof texs[0]);
    for (int t = 0; t < 512; t++) {
        u32 tp = (t >= 1 && (u32)t <= n) ? MEM_R32(table + 4u * (u32)t) : 0;
        tile_weight = (double)count[t];
        build_tile(t, tp);
    }
    hw_map.top_y = 1e9f;
    for (int i = 0; i < HW_MAP_N * HW_MAP_N; i++) {
        float h = hw_map.tile_top[hw_map.grid[i]];
        if (h < hw_map.top_y) hw_map.top_y = h;
    }
    classify(count);
    hw_map.signature = sig;
    hw_map.valid = 1;
    hw_map.version++;
    printf("world: terrain map %u tile types (%u triangles), sea %s at %.0f, highest %.0f; "
           "%d forest and %d water samples\n", n, hw_map.mesh_n / 3, hw_map.has_sea ? "yes" : "no",
           hw_map.sea_y, hw_map.top_y, hw_map.forest_samples, hw_map.water_samples);
    return 1;
}

int hw_world_sea_texture(u16 tpage, u16 clut) {
    for (int i = 0; i < hw_map.nsea_keys; i++) {
        u32 k = hw_map.sea_keys[i];
        if ((u16)k == clut && (((k >> 16) ^ tpage) & 0x19Fu) == 0) return 1;
    }
    return 0;
}

static inline int wrap_cell(int c) {
    if (c < 0) return c & 7;
    if (c >= HW_MAP_N) return (c & 7) | 0xF8;
    return c;
}

float hw_world_ground(float x, float z, u32 *flags) {
    if (!hw_map.valid) { if (flags) *flags = 0; return -1024.0f; }
    float fx = x / HW_TILE_SIZE + 128.0f, fz = z / HW_TILE_SIZE + 128.0f;
    int tx = (int)floorf(fx), tz = (int)floorf(fz);
    int t = hw_map.grid[wrap_cell(tz) * HW_MAP_N + wrap_cell(tx)];
    float lx = (fx - (float)tx) * HW_TILE_RES - 0.5f, lz = (fz - (float)tz) * HW_TILE_RES - 0.5f;
    if (lx < 0) lx = 0;
    if (lz < 0) lz = 0;
    if (lx > HW_TILE_RES - 1) lx = HW_TILE_RES - 1;
    if (lz > HW_TILE_RES - 1) lz = HW_TILE_RES - 1;
    int i = (int)lx, j = (int)lz;
    int i1 = i + 1 < HW_TILE_RES ? i + 1 : i, j1 = j + 1 < HW_TILE_RES ? j + 1 : j;
    float a = lx - (float)i, b = lz - (float)j;
    int ax = (t % HW_ATLAS_COLS) * HW_TILE_RES, ay = (t / HW_ATLAS_COLS) * HW_TILE_RES;
    const float *H = hw_map.height;
    float h00 = H[(size_t)(ay + j) * HW_ATLAS_W + (size_t)(ax + i)];
    float h10 = H[(size_t)(ay + j) * HW_ATLAS_W + (size_t)(ax + i1)];
    float h01 = H[(size_t)(ay + j1) * HW_ATLAS_W + (size_t)(ax + i)];
    float h11 = H[(size_t)(ay + j1) * HW_ATLAS_W + (size_t)(ax + i1)];
    if (flags) {
        int ni = a < 0.5f ? i : i1, nj = b < 0.5f ? j : j1;
        *flags = hw_map.color[(size_t)(ay + nj) * HW_ATLAS_W + (size_t)(ax + ni)] >> 24;
    }
    return (h00 * (1 - a) + h10 * a) * (1 - b) + (h01 * (1 - a) + h11 * a) * b;
}

#define EARTH_INV_2R (1.0f / (2.0f * 6.371e6f * 5.0f))

float hw_world_raycast(const hw_scene_t *s, float sx, float sy, float t0, float t1) {
    if (!s->valid || !hw_map.valid) return 0.0f;
    float d[3];
    view_dir_world(s, sx, sy, d);
    float prev = t0;
    for (float t = t0; t < t1; ) {
        float p[3] = { s->cam[0] + d[0] * t, s->cam[1] + d[1] * t, s->cam[2] + d[2] * t };
        float dx = p[0] - s->cam[0], dz = p[2] - s->cam[2];
        float g = hw_world_ground(p[0], p[2], NULL) + (dx * dx + dz * dz) * EARTH_INV_2R;
        if (p[1] >= g) {
            float lo = prev, hi = t;
            for (int k = 0; k < 10; k++) {
                float m = 0.5f * (lo + hi);
                float q[3] = { s->cam[0] + d[0] * m, s->cam[1] + d[1] * m, s->cam[2] + d[2] * m };
                float ex = q[0] - s->cam[0], ez = q[2] - s->cam[2];
                float gm = hw_world_ground(q[0], q[2], NULL) + (ex * ex + ez * ez) * EARTH_INV_2R;
                if (q[1] >= gm) hi = m; else lo = m;
            }
            return 0.5f * (lo + hi);
        }
        prev = t;
        float step = t * 0.01f;
        t += step > 16.0f ? step : 16.0f;
    }
    return 0.0f;
}

extern int ac3_write_png_rgb(const char *path, unsigned w, unsigned h, const unsigned char *rgb);

void hw_world_map_png(const char *path) {
    if (!hw_map.valid) return;
    const int N = 1024;
    unsigned char *rgb = malloc((size_t)N * N * 3);
    if (!rgb) return;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float wx = ((float)x / 4.0f - 128.0f) * HW_TILE_SIZE + HW_TILE_SIZE / 8;
            float wz = ((float)y / 4.0f - 128.0f) * HW_TILE_SIZE + HW_TILE_SIZE / 8;
            float fx = wx / HW_TILE_SIZE + 128.0f, fz = wz / HW_TILE_SIZE + 128.0f;
            int tx = (int)floorf(fx), tz = (int)floorf(fz);
            int t = hw_map.grid[wrap_cell(tz) * HW_MAP_N + wrap_cell(tx)];
            int i = (int)((fx - tx) * HW_TILE_RES), j = (int)((fz - tz) * HW_TILE_RES);
            int ax = (t % HW_ATLAS_COLS) * HW_TILE_RES, ay = (t / HW_ATLAS_COLS) * HW_TILE_RES;
            u32 c = hw_map.color[(size_t)(ay + j) * HW_ATLAS_W + (size_t)(ax + i)];
            unsigned char *o = rgb + ((size_t)y * N + x) * 3;
            o[0] = (unsigned char)(c & 255u);
            o[1] = (unsigned char)((c >> 8) & 255u);
            o[2] = (unsigned char)((c >> 16) & 255u);
            u32 f = c >> 24;
            if (((x ^ y) & 1) && f) {
                if (f & HW_CELL_WATER)  { o[0] = 0;   o[1] = 60;  o[2] = 200; }
                if (f & HW_CELL_FOREST) { o[0] = 0;   o[1] = 200; o[2] = 0;   }
                if (f & HW_CELL_BUILT)  { o[0] = 200; o[1] = 0;   o[2] = 200; }
            }
        }
    ac3_write_png_rgb(path, (unsigned)N, (unsigned)N, rgb);
    free(rgb);
}
